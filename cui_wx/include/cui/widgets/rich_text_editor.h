#pragma once
/// @file widgets/rich_text_editor.h
/// @brief 富文本编辑器：对齐 Quill.js 的文档模型（Delta）、格式集与 snow 外观。
///
/// 对应外部实现：Quill 1.3（`quilljs.com`）的 `quill.core` + `quill.format` +
/// `themes/snow`。三层分工：
///   - `RichDocument` / `RichLine` / `RichRun`  = Quill 的 Scroll + Block +
///     Inline blot，同时也是 Delta 的载体（`deltaJson()`）。
///   - `RichTextEditorController`               = Quill 的 `QuillController`：
///     文档 + 选区 + 历史（undo/redo）+ 格式化命令，全部与 Widget 无关，可单测。
///   - `RichTextEditor` / `RichTextEditorState` = 渲染与输入：工具栏 + 编辑区。
///
/// 本仓的多行文本基础设施只有「单行、不换行」的 `RenderParagraph`，所以编辑区
/// 是**按行建模**的（与 Quill 一致：一个 block = 一行），软换行（soft wrap）没有：
/// 一行写不完就往右长出，编辑区自带横向滚动。以下是有意的偏离，勿当 bug 查：
///   1. **无软换行**：`RenderParagraph` 单行，故长段落不折行，靠横向滚动看全。
///   2. **`script` 只有字号差**：`TextStyle` 没有 `baselineOffset`，上/下标只能
///      缩小字号，无法真正抬升/下沉。
///   3. **`direction: rtl` 未接**：本仓没有 `Directionality` 上下文。
///   4. **剪贴板未接**：Ctrl+C/V 走不了（Ctrl+A 全选、Ctrl+X 当作删除选中）。
///   5. **`justify` 等同 `left`**：不折行时无处可两端对齐。
///   6. **图片/视频 blot 未接**：Quill 的 `image` 是 embed blot，本模型 run 只有文本。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/color.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/ticker.h"
#include "cui/platform/event.h"  // KeyCode, KeyModifier
#include "cui/rendering/render_box.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/framework/focus.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/popup_menu.h"
#include "cui/widgets/scroll.h"
#include "cui/widgets/text.h"
#include "cui/widgets/text_input.h"

namespace cui {

// ══ 一、格式枚举 ════════════════════════════════════════════════════════════

/// 块格式。对应 Quill 的 `header` / `blockquote` / `code-block` blot 属性。
enum class RichBlock { Normal, Header1, Header2, Header3, Blockquote, CodeBlock };
/// 列表 blot（Quill 还有 `checked`，本仓未接待办清单语义）。
enum class RichListKind { None, Bullet, Ordered };
/// `align` blot 属性。
enum class RichAlign { Left, Center, Right, Justify };
/// `size` 白名单：Quill 只认 small / (缺省 normal) / large / huge。
enum class RichSize { Small, Large, Huge };
/// `script` 白名单。
enum class RichScript { Normal, Super, Sub };
/// `font` 白名单（Quill 的 sans-serif/serif/monospace 三档）。
enum class RichFontKind { Default, SansSerif, Serif, Monospace };

/// 可切换的行内属性（工具栏按钮 ↔ `QuillController::format` 的参数）。
enum class RichAttr { Bold, Italic, Underline, Strike, InlineCode, Link, Color, Background, Font, Size, Script };

// ══ 二、UTF-8 与 JSON 小工具 ════════════════════════════════════════════════

/// 码点边界回退：返回 ≤ p 的最近边界（行尾本身即边界）。
/// 判定看 `s[p]`：续字节 ⇒ p 落在字符中间，要继续往前退；首字节/行尾 ⇒ p 就是边界。
inline std::size_t richPrevBoundary(const std::string& s, std::size_t p) {
    const std::size_t n = s.size();
    p = std::min(p, n);
    while (p > 0 && p < n && (static_cast<unsigned char>(s[p]) & 0xC0u) == 0x80u)
        --p;
    return p;
}
/// 码点边界前进。
inline std::size_t richNextBoundary(const std::string& s, std::size_t p) {
    p = std::min(p, s.size());
    std::size_t n = s.size();
    while (p < n && (static_cast<unsigned char>(s[p]) & 0xC0u) == 0x80u)
        ++p;
    return p;
}

/// JSON 字符串转义（UTF-8 原字节透传，只处理引号/反斜杠/控制符）。
inline std::string richJsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (u < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", u);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// ══ 三、行内格式 / run / line / document ═══════════════════════════════════

/// 格式写入的「只动这些属性」掩码。对应 Quill `formatText(i, l, {bold:true})`
/// 只改一条属性的语义——本仓一次调用可能带上多个字段，故用掩码而不是 map。
struct RichFormatMask {
    bool bold = false, italic = false, underline = false, strike = false;
    bool code = false, link = false, color = false, background = false;
    bool font = false, size = false, script = false;

    bool operator==(const RichFormatMask&) const = default;
    bool any() const { return *this != RichFormatMask{}; }
};

/// 行内属性集合。对应 Quill Inline blot 的 `attributes`。
///
/// `color` 用 `a < 0` 哨兵表示「未指定」（同 `TextStyle`），其余字段用
/// 空串 / `nullopt` / `false` 表示未开——这样 `operator==` 可以直接默认比较，
/// Delta 序列化时也只输出真正设了的属性。
struct RichInline {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool code = false;
    std::string link;
    Color color = Color::fromRGBA(0, 0, 0, -1);
    Color background = Color::fromRGBA(0, 0, 0, 0);
    std::string font;
    std::optional<RichSize> size;
    RichScript script = RichScript::Normal;

    bool operator==(const RichInline&) const = default;
    bool operator!=(const RichInline&) const = default;

    /// 是否等于「无任何格式」（Quill 的 `removeFormat` 落点）。
    bool isClean() const { return *this == RichInline{}; }

    /// 把 `mask` 点亮的属性写成 `set` 里的值（其余保持本值）。
    void writeIf(const RichInline& set, const RichFormatMask& mask) {
        if (mask.bold) bold = set.bold;
        if (mask.italic) italic = set.italic;
        if (mask.underline) underline = set.underline;
        if (mask.strike) strike = set.strike;
        if (mask.code) code = set.code;
        if (mask.link) link = set.link;
        if (mask.color) color = set.color;
        if (mask.background) background = set.background;
        if (mask.font) font = set.font;
        if (mask.size) size = set.size;
        if (mask.script) script = set.script;
    }

    /// Delta 里的 `attributes` 对象（空 = 无属性）。
    std::string attributesJson() const {
        std::string out;
        auto put = [&](const std::string& kv) {
            out += out.empty() ? "" : ",";
            out += kv;
        };
        if (bold) put("\"bold\":true");
        if (italic) put("\"italic\":true");
        if (underline) put("\"underline\":true");
        if (strike) put("\"strike\":true");
        if (code) put("\"code\":true");
        if (!link.empty()) put("\"link\":\"" + richJsonEscape(link) + "\"");
        if (color.a >= 0) put("\"color\":\"" + colorToHex(color) + "\"");
        if (background.a > 0) put("\"background\":\"" + colorToHex(background) + "\"");
        if (!font.empty()) put("\"font\":\"" + richJsonEscape(font) + "\"");
        if (size) {
            const char* n = *size == RichSize::Small   ? "small"
                           : *size == RichSize::Large ? "large" : "huge";
            put(std::string("\"size\":\"") + n + "\"");
        }
        if (script != RichScript::Normal)
            put(std::string("\"script\":\"") +
                (script == RichScript::Super ? "super" : "sub") + "\"");
        return out;
    }
};

/// 一段同格式文本。对应 Quill 的 Inline blot。
struct RichRun {
    std::string text;
    RichInline fmt;

    bool operator==(const RichRun&) const = default;
};

/// 一个块 = 一行。对应 Quill 的 Block blot（本仓不做软换行）。
struct RichLine {
    std::vector<RichRun> runs;
    RichBlock block = RichBlock::Normal;
    RichListKind list = RichListKind::None;
    int indent = 0;
    RichAlign align = RichAlign::Left;

    bool operator==(const RichLine&) const = default;

    std::string plainText() const {
        std::string out;
        for (const auto& r : runs)
            out += r.text;
        return out;
    }
    std::size_t byteLength() const {
        std::size_t n = 0;
        for (const auto& r : runs)
            n += r.text.size();
        return n;
    }
    /// 块属性 → Delta 里换行 insert 的 attributes。
    std::string blockAttributesJson() const {
        std::string out;
        auto put = [&](const std::string& kv) {
            out += out.empty() ? "" : ",";
            out += kv;
        };
        switch (block) {
        case RichBlock::Header1: put("\"header\":1"); break;
        case RichBlock::Header2: put("\"header\":2"); break;
        case RichBlock::Header3: put("\"header\":3"); break;
        case RichBlock::Blockquote: put("\"blockquote\":true"); break;
        case RichBlock::CodeBlock: put("\"code-block\":true"); break;
        case RichBlock::Normal: break;
        }
        if (list == RichListKind::Bullet) put("\"list\":\"bullet\"");
        if (list == RichListKind::Ordered) put("\"list\":\"ordered\"");
        if (indent > 0) put("\"indent\":" + std::to_string(indent));
        if (align == RichAlign::Center) put("\"align\":\"center\"");
        if (align == RichAlign::Right) put("\"align\":\"right\"");
        if (align == RichAlign::Justify) put("\"align\":\"justify\"");
        return out;
    }

    /// 把行内所有 run 的格式替换成 `fmt`（Quill 进 code-block 时清行内格式）。
    void setAllRunsFormat(const RichInline& fmt) {
        for (auto& r : runs)
            r.fmt = fmt;
    }

    /// 合并相邻同格式 run；丢弃空 run。对应 Quill blot 的 `optimize`。
    void optimize() {
        std::vector<RichRun> out;
        for (auto& r : runs) {
            if (r.text.empty())
                continue;
            if (!out.empty() && out.back().fmt == r.fmt)
                out.back().text += r.text;
            else
                out.push_back(r);
        }
        runs = std::move(out);
    }

    /// 在字节偏移 `at` 处切开 run；返回后半段的迭代器（`at` 落在边界时即新 run）。
    std::vector<RichRun>::iterator splitRunAt(std::size_t at) {
        std::size_t pos = 0;
        for (auto it = runs.begin(); it != runs.end(); ++it) {
            std::size_t end = pos + it->text.size();
            if (at <= end) {
                if (at == pos)
                    return it;
                if (at == end)
                    return std::next(it);
                RichRun tail{it->text.substr(at - pos), it->fmt};
                it->text.erase(at - pos);
                return runs.insert(std::next(it), std::move(tail));
            }
            pos = end;
        }
        return runs.end();
    }

    /// 对 [begin,end) 施加 `set` 里 `mask` 点亮的属性。
    ///
    /// 一次遍历重建 run 列表（不用 `splitRunAt` 拿迭代器再改：`vector::insert`
    /// 会让上一个迭代器失效）。`begin`/`end` 必须是码点边界，由调用方 clamp 保证。
    void formatRange(std::size_t begin, std::size_t end, const RichInline& set,
                     const RichFormatMask& mask) {
        std::size_t n = byteLength();
        begin = std::min(begin, n);
        end = std::min(end, n);
        if (end <= begin)
            return;
        std::vector<RichRun> out;
        std::size_t pos = 0;
        for (const auto& r : runs) {
            const std::size_t s = pos, e = pos + r.text.size();
            if (e <= begin || s >= end) {
                out.push_back(r);
            } else {
                if (s < begin)
                    out.push_back(RichRun{r.text.substr(0, begin - s), r.fmt});
                RichInline f = r.fmt;
                f.writeIf(set, mask);
                out.push_back(RichRun{r.text.substr(std::max(s, begin) - s,
                                                    std::min(e, end) - std::max(s, begin)),
                                      f});
                if (e > end)
                    out.push_back(RichRun{r.text.substr(end - s), r.fmt});
            }
            pos = e;
        }
        runs = std::move(out);
        optimize();
    }
};

/// 文档内位置：行号 + 行内字节偏移。对应 Quill 的 `index`（这里拆成二维，
/// 因为本仓不做「整篇一个字符串 + 偏移」的 Delta 索引）。
struct RichPos {
    std::size_t line = 0;
    std::size_t col = 0;

    bool operator==(const RichPos&) const = default;
    bool operator!=(const RichPos&) const = default;
    bool operator<(const RichPos& o) const {
        return line < o.line || (line == o.line && col < o.col);
    }
};

/// 选区（start <= end）。对应 Quill 的 `SelectionInfo {index, length}`。
struct RichRange {
    RichPos start, end;

    bool operator==(const RichRange&) const = default;
    bool collapsed() const { return start == end; }
};

/// 富文本文档。对应 Quill 的 `Delta` + Scroll blot。
///
/// 不变量：`lines` 至少 1 行（Quill 的文档恒以一个空行收尾，这里用「最后一行
/// 可空」表达），run 不空、相邻同格式 run 已合并。
class RichDocument {
public:
    std::vector<RichLine> lines;

    RichDocument() { lines.push_back(RichLine{}); }

    /// 纯文本 → 每行一个普通块（`\r\n`/`\r` 也当换行）。
    static RichDocument fromPlainText(const std::string& text) {
        RichDocument doc;
        doc.lines.clear();
        std::size_t start = 0;
        for (;;) {
            std::size_t nl = text.find('\n', start);
            std::string piece = nl == std::string::npos ? text.substr(start)
                                                        : text.substr(start, nl - start);
            if (!piece.empty() && piece.back() == '\r')
                piece.pop_back();
            RichLine ln;
            if (!piece.empty())
                ln.runs.push_back(RichRun{piece, RichInline{}});
            doc.lines.push_back(std::move(ln));
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
        if (doc.lines.empty())
            doc.lines.push_back(RichLine{});
        return doc;
    }

    bool operator==(const RichDocument&) const = default;
    bool operator!=(const RichDocument&) const = default;

    std::size_t lineCount() const { return lines.size(); }
    const RichLine& line(std::size_t i) const { return lines[std::min(i, lines.size() - 1)]; }

    /// 夹位置（行号 + 行内偏移，并对齐码点边界）。
    RichPos clamp(RichPos p) const {
        if (lines.empty())
            return RichPos{};
        p.line = std::min(p.line, lines.size() - 1);
        p.col = richPrevBoundary(line(p.line).plainText(), std::min(p.col, line(p.line).byteLength()));
        return p;
    }
    RichPos lineStart(std::size_t l) const { return RichPos{std::min(l, lines.size() - 1), 0}; }
    RichPos lineEnd(std::size_t l) const {
        std::size_t i = std::min(l, lines.size() - 1);
        return RichPos{i, lines[i].byteLength()};
    }
    RichPos docStart() const { return RichPos{0, 0}; }
    RichPos docEnd() const { return lineEnd(lines.size() - 1); }
    RichRange ordered(RichPos a, RichPos b) const {
        a = clamp(a);
        b = clamp(b);
        return b < a ? RichRange{b, a} : RichRange{a, b};
    }

    /// 整篇纯文本（行间 `\n`），对应 Quill `getText()`。
    std::string plainText() const {
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i)
                out += '\n';
            out += lines[i].plainText();
        }
        return out + "\n";  // Quill 的 getText() 以换行收尾
    }

    /// 行内 [begin,end) 的纯文本。
    std::string textInLine(std::size_t l, std::size_t begin, std::size_t end) const {
        std::string all = line(l).plainText();
        begin = std::min(begin, all.size());
        end = std::min(end, all.size());
        return end > begin ? all.substr(begin, end - begin) : std::string();
    }

    /// 选区纯文本（跨行时带 `\n`），对应 Quill `getText(index, length)`。
    std::string textInRange(const RichRange& r) const {
        RichRange q = ordered(r.start, r.end);
        if (q.collapsed())
            return {};
        std::string out;
        for (std::size_t l = q.start.line; l <= q.end.line; ++l) {
            if (l > q.start.line)
                out += '\n';
            std::size_t b = l == q.start.line ? q.start.col : 0;
            std::size_t e = l == q.end.line ? q.end.col : line(l).byteLength();
            out += textInLine(l, b, e);
        }
        return out;
    }

    /// 逐行词法无关；这里只给 Delta JSON（Quill `getContents()` 的 JSON 形态）。
    std::string deltaJson() const {
        std::string out = "[";
        bool first = true;
        auto emit = [&](const std::string& obj) {
            out += first ? "" : ",";
            first = false;
            out += obj;
        };
        for (const auto& ln : lines) {
            for (const auto& r : ln.runs) {
                if (r.text.empty())
                    continue;
                std::string attrs = r.fmt.attributesJson();
                emit("{\"insert\":\"" + richJsonEscape(r.text) + "\"" +
                     (attrs.empty() ? "" : ",\"attributes\":{" + attrs + "}") + "}");
            }
            std::string attrs = ln.blockAttributesJson();
            emit("{\"insert\":\"\\n\"" + (attrs.empty() ? "" : ",\"attributes\":{" + attrs + "}") +
                 "}");
        }
        out += "]";
        return out;
    }

    /// 全文 optimize（清空 run、合并同格式邻居）。
    void optimize() {
        for (auto& ln : lines)
            ln.optimize();
        if (lines.empty())
            lines.push_back(RichLine{});
    }

    // ── 编辑操作（全部返回「是否真的改了」）────────────────────────────────

    /// 在 `at` 插入文本，`\n` 拆行（行属性继承当前行——调用方负责 Enter 的特殊语义）。
    /// 返回后 `outPos` 是插入结束位置。
    bool insertText(RichPos at, const std::string& text, RichPos* outPos) {
        RichPos p = clamp(at);
        if (text.empty()) {
            if (outPos)
                *outPos = p;
            return false;
        }
        std::size_t li = p.line;
        std::size_t col = p.col;
        std::size_t start = 0;
        for (;;) {
            std::size_t nl = text.find('\n', start);
            std::string piece = nl == std::string::npos ? text.substr(start)
                                                        : text.substr(start, nl - start);
            if (!piece.empty()) {
                RichLine& ln = lines[li];
                auto it = ln.splitRunAt(col);
                RichInline fmt = it == ln.runs.end()
                                     ? (ln.runs.empty() ? RichInline{} : ln.runs.back().fmt)
                                     : it->fmt;
                ln.runs.insert(it, RichRun{piece, fmt});
                col += piece.size();
                ln.optimize();
            }
            if (nl == std::string::npos)
                break;
            start = nl + 1;
            // 换行：把当前行从 col 切开，新行继承块属性（Quill Enter 的行属性口径）。
            if (splitAt(li, col)) {
                ++li;
                col = 0;
            }
        }
        if (outPos)
            *outPos = RichPos{li, col};
        return true;
    }

    /// 在 (line, col) 处拆行：前半留在 `li`，后半进入新行。返回是否拆成。
    bool splitAt(std::size_t li, std::size_t col) {
        if (li >= lines.size())
            return false;
        RichLine& ln = lines[li];
        auto tailIt = ln.splitRunAt(col);
        RichLine nl;
        nl.runs.assign(tailIt, ln.runs.end());
        ln.runs.erase(tailIt, ln.runs.end());
        nl.block = ln.block;
        nl.list = ln.list;
        nl.indent = ln.indent;
        nl.align = ln.align;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(li) + 1, std::move(nl));
        lines[li].optimize();
        lines[li + 1].optimize();
        return true;
    }

    /// 删除 [a,b) 区间（跨行时把 b 所在行并进 a 所在行）。返回删除后的落点。
    bool deleteRange(RichPos a, RichPos b, RichPos* outPos) {
        RichRange r = ordered(a, b);
        if (r.collapsed()) {
            if (outPos)
                *outPos = r.start;
            return false;
        }
        if (r.start.line == r.end.line) {
            RichLine& ln = lines[r.start.line];
            cutWithinLine(ln, r.start.col, r.end.col);
            if (outPos)
                *outPos = RichPos{r.start.line, r.start.col};
            return true;
        }
        // 首行去掉 [start, ∞) 之后的尾巴、尾行去掉 [0, end)，再把中间行删除并接起来。
        RichLine& first = lines[r.start.line];
        RichLine& last = lines[r.end.line];
        trimFrom(first, r.start.col);
        RichLine head = last;
        std::size_t keep = r.end.col;
        auto it = head.splitRunAt(keep);
        head.runs.erase(head.runs.begin(), it);
        head.block = first.block;
        head.list = first.list;
        head.indent = first.indent;
        head.align = first.align;
        first.runs.insert(first.runs.end(), head.runs.begin(), head.runs.end());
        first.optimize();
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(r.start.line) + 1,
                    lines.begin() + static_cast<std::ptrdiff_t>(r.end.line) + 1);
        if (outPos)
            *outPos = RichPos{r.start.line, r.start.col};
        if (lines.empty())
            lines.push_back(RichLine{});
        return true;
    }

    /// 删除整行（保留至少一行）。`li` 之后不存在时删 `li-1`。
    bool removeLine(std::size_t li) {
        if (lines.size() <= 1) {
            lines[0] = RichLine{};
            return true;
        }
        if (li >= lines.size())
            li = lines.size() - 1;
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(li));
        return true;
    }

    /// 对 [a,b) 的行内区间施加格式（`mask` 点亮哪些写哪些）。
    bool applyInline(RichPos a, RichPos b, const RichInline& set,
                     const RichFormatMask& mask) {
        RichRange r = ordered(a, b);
        if (r.collapsed())
            return false;
        for (std::size_t l = r.start.line; l <= r.end.line && l < lines.size(); ++l) {
            std::size_t begin = l == r.start.line ? r.start.col : 0;
            std::size_t end = l == r.end.line ? r.end.col : lines[l].byteLength();
            lines[l].formatRange(begin, end, set, mask);
        }
        return true;
    }

    /// 对覆盖到的行改块属性（Quill `formatLine`）。
    bool applyBlock(RichPos a, RichPos b,
                    const std::function<void(RichLine&)>& fn) {
        RichRange r = ordered(a, b);
        for (std::size_t l = r.start.line; l <= r.end.line && l < lines.size(); ++l) {
            fn(lines[l]);
        }
        return true;
    }

    /// 清掉 [a,b) 的所有行内格式（Quill `removeFormat`）。
    bool cleanInline(RichPos a, RichPos b) {
        return applyInline(a, b, RichInline{}, cleanMask());
    }

    /// 覆盖所有属性的 mask（用于 clean）。
    static RichFormatMask cleanMask() {
        RichFormatMask m;
        m.bold = m.italic = m.underline = m.strike = m.code = true;
        m.link = m.color = m.background = m.font = m.size = m.script = true;
        return m;
    }

    /// 只点亮某条属性的 mask。
    static RichFormatMask maskFor(RichAttr attr) {
        RichFormatMask m;
        switch (attr) {
        case RichAttr::Bold: m.bold = true; break;
        case RichAttr::Italic: m.italic = true; break;
        case RichAttr::Underline: m.underline = true; break;
        case RichAttr::Strike: m.strike = true; break;
        case RichAttr::InlineCode: m.code = true; break;
        case RichAttr::Link: m.link = true; break;
        case RichAttr::Color: m.color = true; break;
        case RichAttr::Background: m.background = true; break;
        case RichAttr::Font: m.font = true; break;
        case RichAttr::Size: m.size = true; break;
        case RichAttr::Script: m.script = true; break;
        }
        return m;
    }

private:
    /// 删掉行内 [begin,end)：一次遍历重建，避免 splitRunAt 之间的迭代器失效。
    static void cutWithinLine(RichLine& ln, std::size_t begin, std::size_t end) {
        std::vector<RichRun> out;
        std::size_t pos = 0;
        for (const auto& r : ln.runs) {
            const std::size_t s = pos, e = pos + r.text.size();
            if (e <= begin || s >= end) {
                out.push_back(r);
            } else {
                if (s < begin)
                    out.push_back(RichRun{r.text.substr(0, begin - s), r.fmt});
                if (e > end)
                    out.push_back(RichRun{r.text.substr(end - s), r.fmt});
            }
            pos = e;
        }
        ln.runs = std::move(out);
        ln.optimize();
    }
    void trimFrom(RichLine& ln, std::size_t col) {
        auto it = ln.splitRunAt(col);
        ln.runs.erase(it, ln.runs.end());
        ln.optimize();
    }
};

// ══ 四、控制器（对应 Quill 的 `QuillController`）════════════════════════════

/// bool 型行内属性的读取（Bold/Italic/Underline/Strike/InlineCode）。
inline bool richFlagOf(const RichInline& f, RichAttr a) {
    switch (a) {
    case RichAttr::Bold: return f.bold;
    case RichAttr::Italic: return f.italic;
    case RichAttr::Underline: return f.underline;
    case RichAttr::Strike: return f.strike;
    case RichAttr::InlineCode: return f.code;
    default: return false;
    }
}
inline void richSetFlag(RichInline& f, RichAttr a, bool v) {
    switch (a) {
    case RichAttr::Bold: f.bold = v; break;
    case RichAttr::Italic: f.italic = v; break;
    case RichAttr::Underline: f.underline = v; break;
    case RichAttr::Strike: f.strike = v; break;
    case RichAttr::InlineCode: f.code = v; break;
    default: break;
    }
}
/// 把 `b` 点亮的位并到 `a` 上（pending 掩码累加）。
inline void richMergeMask(RichFormatMask& a, const RichFormatMask& b) {
    a.bold = a.bold || b.bold;
    a.italic = a.italic || b.italic;
    a.underline = a.underline || b.underline;
    a.strike = a.strike || b.strike;
    a.code = a.code || b.code;
    a.link = a.link || b.link;
    a.color = a.color || b.color;
    a.background = a.background || b.background;
    a.font = a.font || b.font;
    a.size = a.size || b.size;
    a.script = a.script || b.script;
}
/// 全属性 mask：插入文本时把「光标格式」整份写进新区间。
inline RichFormatMask richAllInlineMask() {
    RichFormatMask m;
    m.bold = m.italic = m.underline = m.strike = m.code = true;
    m.link = m.color = m.background = m.font = m.size = m.script = true;
    return m;
}

/// 富文本编辑控制器：文档 + 选区 + 历史 + 格式化命令。
///
/// 与 Widget 无关，可以脱离渲染单测（`tests/test_rich_text_editor.cpp` 的第 1–4 节）。
/// 垂直方向移动光标需要像素几何，所以 `moveVertical` 收一个「(行号, x) → 位置」
/// 的回调，由 `RichTextEditorState` 用它上一次 build 算出的行几何提供。
class RichTextEditorController {
public:
    using Listener = std::function<void()>;
    static constexpr std::size_t kHistoryLimit = 200;
    static constexpr int kMaxIndent = 5;

    explicit RichTextEditorController(RichDocument document = RichDocument{})
        : doc_(std::move(document)) {
        normalizeDoc();
        caret_ = base_ = doc_.docEnd();
    }

    // ── 文档 ────────────────────────────────────────────────────────────────
    const RichDocument& document() const { return doc_; }
    std::string plainText() const { return doc_.plainText(); }
    /// Quill `getContents()` 的 Delta JSON（测试与状态栏都用它比对）。
    std::string deltaJson() const { return doc_.deltaJson(); }

    void setDocument(RichDocument d) {
        doc_ = std::move(d);
        normalizeDoc();
        base_ = caret_ = doc_.docStart();
        pending_ = RichInline{};
        pendingMask_ = RichFormatMask{};
        undo_.clear();
        redo_.clear();
        notify();
    }
    void setPlainText(const std::string& text) {
        setDocument(RichDocument::fromPlainText(text));
    }
    /// Quill `deleteText(0, length)`。
    void clear() { setDocument(RichDocument{}); }

    // ── 选区 ────────────────────────────────────────────────────────────────
    const RichPos& caret() const { return caret_; }
    const RichPos& base() const { return base_; }
    RichRange selection() const { return doc_.ordered(base_, caret_); }
    bool hasSelection() const { return !(base_ == caret_); }
    void setCaret(RichPos p) { setSelection(p, p); }
    void setSelection(RichPos b, RichPos e) {
        RichPos nb = doc_.clamp(b), ne = doc_.clamp(e);
        if (nb == base_ && ne == caret_)
            return;
        base_ = nb;
        caret_ = ne;
        pending_ = RichInline{};
        pendingMask_ = RichFormatMask{};
        notify();
    }
    void selectAll() { setSelection(doc_.docStart(), doc_.docEnd()); }

    // ── 格式读取（工具栏 active 态的口径）─────────────────────────────────
    /// 选区格式：跨多个 run 时逐属性取「一致才保留」（Quill `getFormat()`）。
    /// 折叠光标 ⇒ 左侧字符的格式 + 未落地的 `pending`。
    RichInline formatAtSelection() const {
        RichRange r = selection();
        if (r.collapsed())
            return mergePending(formatLeftOf(r.start));
        std::vector<RichInline> list = formatsInLineRange(r.start.line, r.start.col,
                                                          r.end.line, r.end.col);
        if (list.empty())
            return mergePending(formatLeftOf(r.start));
        RichInline acc = list.front();
        for (std::size_t i = 1; i < list.size(); ++i)
            acc = commonOf(acc, list[i]);
        return acc;
    }
    RichBlock blockAt() const { return doc_.line(caret_.line).block; }
    RichListKind listAt() const { return doc_.line(caret_.line).list; }
    RichAlign alignAt() const { return doc_.line(caret_.line).align; }
    int indentAt() const { return doc_.line(caret_.line).indent; }
    const RichInline& pending() const { return pending_; }

    // ── 编辑命令 ────────────────────────────────────────────────────────────
    /// 插入文本（先替换选区）。`\n` 走 `insertLineBreak` 语义由调用方分流。
    bool insertText(const std::string& text) {
        if (text.empty())
            return false;
        std::string plain;
        plain.reserve(text.size());
        for (char c : text)
            if (c != '\n')
                plain += c;
        if (plain.empty())
            return insertLineBreak();
        pushHistory();
        RichPos at = caret_;
        if (hasSelection()) {
            RichRange r = selection();
            doc_.deleteRange(r.start, r.end, &at);
        } else {
            at = doc_.clamp(at);
        }
        const RichInline fmt = formatForInsert(at);
        RichPos end = at;
        doc_.insertText(at, plain, &end);
        doc_.applyInline(at, end, fmt, richAllInlineMask());
        caret_ = base_ = end;
        pending_ = RichInline{};
        pendingMask_ = RichFormatMask{};
        notify();
        return true;
    }

    /// 回车：拆行 + Quill 的块属性口径。
    ///   - 空行上的列表/引用/代码块 ⇒ 退出该格式（Quill「再回车一次离开」）；
    ///   - 标题 ⇒ 新行回到正文（Quill 的 header 不延续）；
    ///   - 列表非空 ⇒ 延续，ordered 不需要编号（渲染侧现算）。
    bool insertLineBreak() {
        pushHistory();
        RichPos at = caret_;
        if (hasSelection()) {
            RichRange r = selection();
            doc_.deleteRange(r.start, r.end, &at);
        }
        at = doc_.clamp(at);
        RichLine& cur = doc_.lines[at.line];
        const bool curEmpty = cur.byteLength() == 0;
        if (curEmpty && (cur.list != RichListKind::None || cur.block == RichBlock::Blockquote ||
                         cur.block == RichBlock::CodeBlock)) {
            cur.list = RichListKind::None;
            cur.block = RichBlock::Normal;
            cur.indent = 0;
            caret_ = base_ = at;
            notify();
            return true;
        }
        // splitAt 会 realloc `lines` ⇒ 引用全部失效，需要的值先按值抄下来。
        const RichBlock curBlock = cur.block;
        const RichAlign curAlign = cur.align;
        if (!doc_.splitAt(at.line, at.col)) {
            notify();
            return false;
        }
        RichLine& nl = doc_.lines[at.line + 1];
        nl.block = curBlock == RichBlock::Header1 || curBlock == RichBlock::Header2 ||
                                   curBlock == RichBlock::Header3
                               ? RichBlock::Normal
                               : curBlock;
        nl.align = curAlign;
        caret_ = base_ = RichPos{at.line + 1, 0};
        pending_ = RichInline{};
        pendingMask_ = RichFormatMask{};
        notify();
        return true;
    }

    /// 退格：有选区删选区；行首先「降级块格式」再合并上一行（Quill 的口径）。
    bool deleteBackward() {
        if (hasSelection())
            return deleteSelection();
        RichPos p = doc_.clamp(caret_);
        if (p.col > 0) {
            pushHistory();
            // `p.col - 1`：先迈回一个字节再夹边界，才真的退过前一个码点。
            doc_.deleteRange(doc_.clamp(RichPos{p.line, richPrevBoundary(textAt(p), p.col - 1)}), p,
                             &caret_);
            base_ = caret_;
            notify();
            return true;
        }
        if (p.line == 0)
            return false;
        pushHistory();
        RichLine& ln = doc_.lines[p.line];
        RichLine& prev = doc_.lines[p.line - 1];
        const bool sameShape = ln.block == prev.block && ln.list == prev.list &&
                               ln.indent == prev.indent;
        if (!sameShape) {  // 第一次退格只去掉块格式，不并线
            ln.block = prev.block;
            ln.list = prev.list;
            ln.indent = prev.indent;
            caret_ = base_ = p;
            notify();
            return true;
        }
        doc_.deleteRange(RichPos{p.line - 1, prev.byteLength()}, p, &caret_);
        base_ = caret_;
        notify();
        return true;
    }

    bool deleteForward() {
        if (hasSelection())
            return deleteSelection();
        RichPos p = doc_.clamp(caret_);
        if (p.col < doc_.line(p.line).byteLength()) {
            pushHistory();
            doc_.deleteRange(p, RichPos{p.line, richNextBoundary(textAt(p), p.col + 1)},
                             &caret_);
            base_ = caret_;
            notify();
            return true;
        }
        if (p.line + 1 >= doc_.lines.size())
            return false;
        pushHistory();
        doc_.deleteRange(p, RichPos{p.line + 1, 0}, &caret_);
        base_ = caret_;
        notify();
        return true;
    }

    bool deleteSelection() {
        if (!hasSelection())
            return false;
        pushHistory();
        RichRange r = selection();
        RichPos out = r.start;
        doc_.deleteRange(r.start, r.end, &out);
        base_ = caret_ = out;
        notify();
        return true;
    }

    void moveLeft(bool select) {
        RichPos p = doc_.clamp(caret_);
        std::size_t target = p.col > 0 ? richPrevBoundary(textAt(p), p.col - 1) : 0;
        RichPos np = target < p.col ? RichPos{p.line, target} : RichPos{p.line, 0};
        if (target == 0 && p.col == 0 && p.line > 0) {  // 行首再向左 ⇒ 上一行行尾（Quill 一致）
            std::size_t l = p.line - 1;
            np = RichPos{l, doc_.line(l).byteLength()};
        }
        moveTo(np, select);
    }
    void moveRight(bool select) {
        RichPos p = doc_.clamp(caret_);
        std::size_t n = doc_.line(p.line).byteLength();
        // 末行行尾再向右不出文档（Quill 同口径）；否则进下一行行首。
        RichPos np = p.col < n
                         ? RichPos{p.line, richNextBoundary(textAt(p), p.col + 1)}
                         : (p.line + 1 < doc_.lines.size() ? RichPos{p.line + 1, 0} : p);
        moveTo(np, select);
    }
    void moveHome(bool select, bool toDocStart) {
        moveTo(toDocStart ? doc_.docStart() : doc_.lineStart(caret_.line), select);
    }
    void moveEnd(bool select, bool toDocEnd) {
        moveTo(toDocEnd ? doc_.docEnd() : doc_.lineEnd(caret_.line), select);
    }
    /// 上下移动：`hit(line, x)` 由渲染侧把像素 x 映射成该行内的位置。
    void moveVertical(int deltaLines, double x,
                      const std::function<RichPos(std::size_t, double)>& hit) {
        if (deltaLines == 0)
            return;
        std::size_t target = caret_.line;
        if (deltaLines < 0) {
            if (target == 0)
                return;
            target -= std::min<std::size_t>(static_cast<std::size_t>(-deltaLines), target);
        } else {
            target = std::min(doc_.lines.size() - 1,
                              target + static_cast<std::size_t>(deltaLines));
        }
        if (target == caret_.line)
            return;
        RichPos np = hit ? hit(target, x) : doc_.lineStart(target);
        moveTo(np, false);
    }
    void moveTo(RichPos p, bool select) {
        p = doc_.clamp(p);
        if (select)
            setSelection(base_, p);
        else
            setCaret(p);
    }

    // ── 格式命令（Quill format / formatText / formatLine / removeFormat）──
    /// 折叠光标 ⇒ 落进 pending（下一次输入带上）；有选区 ⇒ 直接改文档。
    void applyFormat(RichAttr attr, const RichInline& value) {
        RichFormatMask mask = RichDocument::maskFor(attr);
        RichRange r = selection();
        if (r.collapsed()) {
            pending_.writeIf(value, mask);
            richMergeMask(pendingMask_, mask);
            notify();
            return;
        }
        pushHistory();
        doc_.applyInline(r.start, r.end, value, mask);
        clampSelection();
        notify();
    }
    void toggleFlag(RichAttr attr) {
        RichInline cur = formatAtSelection();
        RichInline next = cur;
        richSetFlag(next, attr, !richFlagOf(cur, attr));
        applyFormat(attr, next);
    }
    void setLink(const std::string& url) {
        RichInline v;
        v.link = url;
        applyFormat(RichAttr::Link, v);
    }
    void setColor(const std::optional<Color>& c) {
        RichInline v;
        if (c)
            v.color = *c;
        applyFormat(RichAttr::Color, v);
    }
    void setBackground(const std::optional<Color>& c) {
        RichInline v;
        if (c)
            v.background = *c;
        applyFormat(RichAttr::Background, v);
    }
    void setFont(const std::string& family) {
        RichInline v;
        v.font = family;
        applyFormat(RichAttr::Font, v);
    }
    void setSize(const std::optional<RichSize>& s) {
        RichInline v;
        v.size = s;
        applyFormat(RichAttr::Size, v);
    }
    void setScript(RichScript s) {
        RichInline v;
        v.script = s;
        applyFormat(RichAttr::Script, v);
    }

    /// 块命令：作用在选区覆盖的行上（折叠 ⇒ 光标所在行）。
    void setHeader(int level) {
        applyBlockLines([level](RichLine& ln) {
            ln.block = level <= 0      ? RichBlock::Normal
                       : level == 1    ? RichBlock::Header1
                       : level == 2    ? RichBlock::Header2
                                       : RichBlock::Header3;
            if (level > 0)
                ln.list = RichListKind::None;
        });
    }
    void toggleBlockquote() {
        applyBlockLines([](RichLine& ln) {
            ln.block = ln.block == RichBlock::Blockquote ? RichBlock::Normal
                                                          : RichBlock::Blockquote;
            if (ln.block == RichBlock::Blockquote)
                ln.list = RichListKind::None;
        });
    }
    /// 代码块：整段进/整段出。进去时清行内格式（Quill 的 code blot 是纯净的）。
    void toggleCodeBlock() {
        RichRange r = selection();
        bool allCode = true;
        for (std::size_t l = r.start.line; l <= r.end.line; ++l)
            if (doc_.line(l).block != RichBlock::CodeBlock)
                allCode = false;
        if (allCode) {
            applyBlockLines([](RichLine& ln) {
                ln.block = RichBlock::Normal;
                ln.setAllRunsFormat(RichInline{});
            });
            return;
        }
        pushHistory();
        doc_.applyBlock(r.start, r.end, [](RichLine& ln) {
            ln.block = RichBlock::CodeBlock;
            ln.list = RichListKind::None;
            ln.align = RichAlign::Left;
            ln.setAllRunsFormat(RichInline{});
        });
        clampSelection();
        notify();
    }
    void toggleList(RichListKind kind) {
        applyBlockLines([kind](RichLine& ln) {
            ln.list = ln.list == kind ? RichListKind::None : kind;
            if (ln.list != RichListKind::None)
                ln.block = RichBlock::Normal;
        });
    }
    void changeIndent(int delta) {
        applyBlockLines([delta](RichLine& ln) {
            ln.indent = std::clamp(ln.indent + delta, 0, kMaxIndent);
        });
    }
    void setAlign(RichAlign a) {
        applyBlockLines([a](RichLine& ln) { ln.align = a; });
    }
    /// Quill `removeFormat`：清行内格式，块格式保留。
    void clean() {
        RichRange r = selection();
        if (r.collapsed()) {
            pending_ = RichInline{};
            pendingMask_ = richAllInlineMask();
            notify();
            return;
        }
        pushHistory();
        doc_.cleanInline(r.start, r.end);
        clampSelection();
        notify();
    }

    // ── 历史（Quill 的 `history` 模块）─────────────────────────────────────
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::size_t undoDepth() const { return undo_.size(); }
    std::size_t redoDepth() const { return redo_.size(); }
    void undo() {
        if (undo_.empty())
            return;
        redo_.push_back(Snapshot{doc_, base_, caret_});
        restore(undo_.back());
        undo_.pop_back();
    }
    void redo() {
        if (redo_.empty())
            return;
        undo_.push_back(Snapshot{doc_, base_, caret_});
        restore(redo_.back());
        redo_.pop_back();
    }

    // ── 监听（句柄式摘除，铁律同 TextEditingController）──────────────────
    int addListener(Listener fn) {
        int id = nextListenerId_++;
        listeners_.emplace_back(id, std::move(fn));
        return id;
    }
    void removeListener(int id) {
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if (it->first == id) {
                listeners_.erase(it);
                return;
            }
        }
    }
    std::size_t listenerCount() const { return listeners_.size(); }

private:
    struct Snapshot {
        RichDocument doc;
        RichPos base, caret;
    };

    void normalizeDoc() {
        doc_.optimize();
        if (doc_.lines.empty())
            doc_.lines.push_back(RichLine{});
    }
    void notify() {
        auto copy = listeners_;
        for (auto& kv : copy)
            if (kv.second)
                kv.second();
    }
    void pushHistory() {
        undo_.push_back(Snapshot{doc_, base_, caret_});
        if (undo_.size() > kHistoryLimit)
            undo_.erase(undo_.begin());
        redo_.clear();
    }
    void restore(const Snapshot& s) {
        doc_ = s.doc;
        base_ = doc_.clamp(s.base);
        caret_ = doc_.clamp(s.caret);
        pending_ = RichInline{};
        pendingMask_ = RichFormatMask{};
        notify();
    }
    void clampSelection() {
        base_ = doc_.clamp(base_);
        caret_ = doc_.clamp(caret_);
    }
    std::string textAt(RichPos p) const { return doc_.line(std::min(p.line, doc_.lines.size() - 1)).plainText(); }

    /// 光标「左侧字符」所在 run 的格式；行首则回看上一行行尾。
    RichInline formatLeftOf(RichPos p) const {
        if (p.col == 0 && p.line == 0)
            return RichInline{};
        std::size_t l = p.line, col = p.col;
        if (col == 0) {
            l = p.line - 1;
            col = doc_.line(l).byteLength();
        }
        const RichLine& ln = doc_.line(l);
        const std::size_t target = col - 1;
        std::size_t pos = 0;
        for (const auto& r : ln.runs) {
            if (target < pos + r.text.size())
                return r.fmt;
            pos += r.text.size();
        }
        return ln.runs.empty() ? RichInline{} : ln.runs.back().fmt;
    }
    RichInline mergePending(const RichInline& base) const {
        RichInline out = base;
        out.writeIf(pending_, pendingMask_);
        return out;
    }
    /// 插入文本时要落的格式：左侧格式 + pending；代码块内强制纯 code。
    RichInline formatForInsert(RichPos p) const {
        RichInline out = mergePending(formatLeftOf(p));
        if (doc_.line(p.line).block == RichBlock::CodeBlock)
            return RichInline{};
        return out;
    }
    /// 选区覆盖到的 run 的格式清单（逐行拼接）。
    std::vector<RichInline> formatsInLineRange(std::size_t l0, std::size_t c0,
                                               std::size_t l1, std::size_t c1) const {
        std::vector<RichInline> out;
        for (std::size_t l = l0; l <= l1 && l < doc_.lines.size(); ++l) {
            std::size_t begin = l == l0 ? c0 : 0;
            std::size_t end = l == l1 ? c1 : doc_.line(l).byteLength();
            std::size_t pos = 0;
            for (const auto& r : doc_.line(l).runs) {
                const std::size_t s = pos, e = pos + r.text.size();
                if (e > begin && s < end)
                    out.push_back(r.fmt);
                pos = e;
            }
        }
        return out;
    }
    /// 逐属性「一致才保留」（Quill getFormat 在混合格式上的表现）。
    static RichInline commonOf(const RichInline& a, const RichInline& b) {
        RichInline o;
        o.bold = a.bold && b.bold;
        o.italic = a.italic && b.italic;
        o.underline = a.underline && b.underline;
        o.strike = a.strike && b.strike;
        o.code = a.code && b.code;
        o.link = a.link == b.link ? a.link : std::string();
        o.color = a.color == b.color ? a.color : Color::fromRGBA(0, 0, 0, -1);
        o.background = a.background == b.background ? a.background : Color::fromRGBA(0, 0, 0, 0);
        o.font = a.font == b.font ? a.font : std::string();
        o.size = (a.size && b.size && *a.size == *b.size) ? a.size : std::nullopt;
        o.script = a.script == b.script ? a.script : RichScript::Normal;
        return o;
    }
    /// 块命令的公共骨架：作用到选区覆盖的行（末行仅在 col>0 时参与，
    /// 对应 Quill 按 index/length 算行时不含尾部换行的口径）。
    void applyBlockLines(const std::function<void(RichLine&)>& fn) {
        RichRange r = selection();
        pushHistory();
        for (std::size_t l = r.start.line; l <= r.end.line && l < doc_.lines.size(); ++l) {
            if (l == r.end.line && r.end.col == 0 && l > r.start.line)
                continue;
            fn(doc_.lines[l]);
        }
        clampSelection();
        notify();
    }

    RichDocument doc_;
    RichPos base_, caret_;
    RichInline pending_;
    RichFormatMask pendingMask_;
    std::vector<Snapshot> undo_, redo_;
    std::vector<std::pair<int, Listener>> listeners_;
    int nextListenerId_ = 1;
};

/// 编辑器外观（对应 Quill snow 主题的那套固定配色）。
///
/// `snow()` 是字面复刻 snow 的白底灰框；`fromApp()` 用当前 `Theme` 派生，
/// 暗色下才不会黑底黑字。两者字段一一对应，宿主可整份覆盖。
struct RichEditorTheme {
    double bodySize = 15.0;      ///< Quill `.ql-editor` 基准字号
    double lineHeightFactor = 1.45;
    double indentUnit = 24.0;    ///< Quill `.ql-indent-N` 每级 3em ≈ 这里的 24px

    Color toolbarBackground, toolbarBorder, toolbarDivider;
    Color buttonForeground, buttonActiveForeground, buttonActiveBackground;
    Color popoverBackground, popoverBorder;

    Color editorBackground, editorBorder, editorText, placeholder;
    Color selection, caret, selectionInactive;  ///< 失焦时选区变灰（浏览器同款）
    Color blockquoteBar, blockquoteText, codeBlockBackground, codeBlockForeground;
    Color inlineCodeBackground, inlineCodeForeground, headerText, linkColor;

    static RichEditorTheme snow() {
        RichEditorTheme t;
        t.toolbarBackground = colorFromHex("#fbfbfb");
        t.toolbarBorder = colorFromHex("#e6e6e6");
        t.toolbarDivider = colorFromHex("#e6e6e6");
        t.buttonForeground = colorFromHex("#444444");
        t.buttonActiveForeground = colorFromHex("#06c");
        t.buttonActiveBackground = colorFromHex("#f3f6fb");
        t.popoverBackground = colorFromHex("#ffffff");
        t.popoverBorder = colorFromHex("#cccccc");
        t.editorBackground = colorFromHex("#ffffff");
        t.editorBorder = colorFromHex("#cccccc");
        t.editorText = colorFromHex("#000000");
        t.placeholder = colorFromHex("#aaaaaa");
        t.selection = colorFromHex("#b3d4fc");
        t.caret = colorFromHex("#222222");
        t.selectionInactive = colorFromHex("#bbbbbb");
        t.blockquoteBar = colorFromHex("#444444");
        t.blockquoteText = colorFromHex("#666666");
        t.codeBlockBackground = colorFromHex("#f0f0f0");
        t.codeBlockForeground = colorFromHex("#222222");
        t.inlineCodeBackground = colorFromHex("#eeeeee");
        t.inlineCodeForeground = colorFromHex("#222222");
        t.headerText = colorFromHex("#000000");
        t.linkColor = colorFromHex("#06c");
        return t;
    }
    static RichEditorTheme fromApp() {
        const Theme& th = theme();
        RichEditorTheme t;
        t.toolbarBackground = th.surfaceVariant;
        t.toolbarBorder = th.border;
        t.toolbarDivider = th.divider;
        t.buttonForeground = th.textSecondary;
        t.buttonActiveForeground = th.textOnPrimary;
        t.buttonActiveBackground = th.primary;
        t.popoverBackground = th.surface;
        t.popoverBorder = th.border;
        t.editorBackground = th.surface;
        t.editorBorder = th.border;
        t.editorText = th.textPrimary;
        t.placeholder = th.textDisabled;
        t.selection = th.primaryLight;
        t.caret = th.textPrimary;
        t.selectionInactive = th.textDisabled;
        t.blockquoteBar = th.primary;
        t.blockquoteText = th.textSecondary;
        t.codeBlockBackground = th.surfaceVariant;
        t.codeBlockForeground = th.textPrimary;
        t.inlineCodeBackground = th.surfaceVariant;
        t.inlineCodeForeground = th.primary;
        t.headerText = th.textPrimary;
        t.linkColor = th.primary;
        return t;
    }
};

// ══ 五、控件：snow 工具栏 + 真编辑区 ═══════════════════════════════════════

/// 平台等宽字体（口径同 CodeEditor）。
inline std::string richMonoFamily() {
#if defined(_WIN32)
    return "Consolas";
#elif defined(__APPLE__)
    return "Menlo";
#else
    return "monospace";
#endif
}
/// Quill 的 font 白名单名 → 本机字体族（空串 = 跟随默认 sans）。
inline std::string richResolveFont(const std::string& quillName) {
    if (quillName == "monospace")
        return richMonoFamily();
    if (quillName == "serif") {
#if defined(_WIN32)
        return "Times New Roman";
#elif defined(__APPLE__)
        return "Times";
#else
        return "serif";
#endif
    }
    return std::string();
}
/// Quill 的 size 白名单 → 像素（small 12 / large 18 / huge 24；normal = 正文号）。
inline double richSizePx(RichSize s, double bodySize) {
    switch (s) {
    case RichSize::Small: return 12.0;
    case RichSize::Large: return 18.0;
    case RichSize::Huge: return 24.0;
    }
    return bodySize;
}
/// 与 `RenderParagraph::measureNatural` 同口径的量宽（务必只在 run 的整个文本上用，
/// 码点级调用要保证 style 一致）。
inline double richWidthFor(const TextStyle& st, const std::string& text) {
    const char* fam = st.fontFamily.empty() ? nullptr : st.fontFamily.c_str();
    const double fs = st.fontSize > 0 ? st.fontSize : 14.0;
    const double fw = st.fontWeight > 0 ? st.fontWeight : 400.0;
    return Painter::measureTextWidth(fam, text, fs, fw, st.italic);
}
inline double richHeightFor(const TextStyle& st) {
    const char* fam = st.fontFamily.empty() ? nullptr : st.fontFamily.c_str();
    const double fs = st.fontSize > 0 ? st.fontSize : 14.0;
    const double fw = st.fontWeight > 0 ? st.fontWeight : 400.0;
    return Painter::textHeight(fam, fs, fw, st.italic);
}

/// Quill snow 取色板的默认色（8 列 × 5 行）。
inline const std::vector<const char*>& richPaletteColors() {
    static const std::vector<const char*> kColors = {
        "#000000", "#e60000", "#ff9900", "#ffff00", "#00ff00", "#00ffff", "#ffffff", "#cccccc",
        "#eeeeee", "#dddddd", "#bbbbbb", "#999999", "#777777", "#666666", "#555555", "#444444",
        "#333333", "#222222", "#e6b8af", "#f7cac9", "#fde9d9", "#fff2cc", "#d9ead3", "#d0e0e3",
        "#c9daf8", "#b45f06", "#8e7cc3", "#a2c4c9", "#6aa84f", "#674ea7", "#3c78d8", "#6fa8dc",
        "#3d85c6", "#134f5c", "#0b5394", "#073763", "#20124d", "#4d124f", "#990000", "#000099",
    };
    return kColors;
}

/// 富文本编辑器（对齐 Quill.js）。
///
/// 键盘路由走 `Focus` + `FocusNode::onKeyEvent`（同 `EditableText`），所以宿主桥
/// 不需要为它加新接线；IME 的 commit 直接插入，preedit 作为临时 run 画在光标处。
class RichTextEditor : public StatefulWidget {
public:
    using DocumentCallback = std::function<void(const RichDocument&)>;
    using SelectionCallback = std::function<void(const RichRange&)>;

    /// 兼容旧调用点 `RichTextEditor("示例文本")`。
    explicit RichTextEditor(std::string sample = "The quick brown fox jumps over the lazy dog.",
                            KeyPtr key = nullptr)
        : RichTextEditor(RichDocument::fromPlainText(sample), "Start writing…", 460.0,
                         std::nullopt, false, std::nullopt, nullptr, nullptr, std::move(key)) {}

    explicit RichTextEditor(RichDocument document, std::string placeholder = "Start writing…",
                            double width = 460.0, std::optional<double> maxHeight = std::nullopt,
                            bool autofocus = false,
                            std::optional<RichEditorTheme> editorTheme = std::nullopt,
                            DocumentCallback onChanged = {},
                            SelectionCallback onSelectionChanged = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), document_(std::move(document)),
          placeholder_(std::move(placeholder)), width_(width), maxHeight_(maxHeight),
          autofocus_(autofocus), editorTheme_(editorTheme), onChanged_(std::move(onChanged)),
          onSelectionChanged_(std::move(onSelectionChanged)) {
        controller_ = cui::make_shared<RichTextEditorController>(document_);
        focusNode_ = cui::make_shared<FocusNode>();
    }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "RichTextEditor"; }

    /// 编辑状态的唯一持有者（跨重建存活；测试与宿主都从这里读写）。
    const SharedPtr<RichTextEditorController>& controller() const { return controller_; }
    const SharedPtr<FocusNode>& focusNode() const { return focusNode_; }
    const std::string& placeholder() const { return placeholder_; }
    double width() const { return width_; }
    const std::optional<double>& maxHeight() const { return maxHeight_; }
    bool autofocus() const { return autofocus_; }
    const std::optional<RichEditorTheme>& editorTheme() const { return editorTheme_; }
    const DocumentCallback& onChanged() const { return onChanged_; }
    const SelectionCallback& onSelectionChanged() const { return onSelectionChanged_; }

    /// widget 侧的文档快照：State 在 `didUpdateWidget` 里比对它，发现宿主换过
    /// 内容就采纳（宿主优先于编辑器内的编辑历史）。
    const RichDocument& document() const { return document_; }
    void setDocument(RichDocument d) {
        document_ = std::move(d);
        if (controller_)
            controller_->setDocument(document_);
    }

private:
    RichDocument document_;
    std::string placeholder_;
    double width_ = 460.0;
    std::optional<double> maxHeight_;
    bool autofocus_ = false;
    std::optional<RichEditorTheme> editorTheme_;
    DocumentCallback onChanged_;
    SelectionCallback onSelectionChanged_;
    SharedPtr<RichTextEditorController> controller_;
    SharedPtr<FocusNode> focusNode_;
};

/// 编辑器状态：选区/光标的像素侧信息 + 渲染。文档与历史都在控制器里。
class RichTextEditorState : public State {
public:
    void initState() override;
    void didUpdateWidget(cui::Widget* oldWidget) override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

    /// 键盘入口（`Focus` 每帧把它接到节点上，测试也可直接调）。
    KeyEventResult onKey(const KeyEvent& ev);

    const SharedPtr<RichTextEditorController>& controller() const { return ctl_; }
    /// 光标闪烁半周期（Quill `.ql-cursor` 的 1s 一个来回）。
    static constexpr double kBlinkPeriodMs = 530.0;
    /// 上一次 build 算出的行几何（测试钉光标位置用）。
    struct LineGeom {
        std::size_t line = 0;
        double y = 0.0, h = 0.0;
        double textX = 0.0, textW = 0.0;
        double textTop = 0.0, textH = 0.0;
    };
    const std::vector<LineGeom>& lineGeoms() const { return geom_; }
    /// 光标是否该上屏：Quill 未聚焦时不画光标（选区只是变灰），所以这里必须
    /// 带上 `focused_`，不然刚挂上（从未获焦）的编辑器也会顶着一根光标。
    bool caretVisible() const { return focused_ && caretOn_; }

private:
    RichTextEditor* w() { return static_cast<RichTextEditor*>(widget()); }

    // ── 样式 / 度量 ─────────────────────────────────────────────────────────
    TextStyle styleForRun(const RichRun& r, const RichLine& ln) const;
    TextStyle baseStyleFor(const RichLine& ln) const;
    /// 行内 [0,col) 的像素宽（与 RenderParagraph 的绘制同口径）。
    double prefixWidth(const RichLine& ln, std::size_t col) const;
    /// 像素 x（相对该行文本左端）→ 行内字节偏移。
    std::size_t colForX(const RichLine& ln, double dx) const;
    RichPos posAtLocal(double lx, double ly) const;
    RichPos posAtLine(std::size_t lineIdx, double xInContent) const;
    double caretXInContent() const;

    // ── 交互 ────────────────────────────────────────────────────────────────
    void placeCaret(const PointerEvent& e, bool extend);
    void restartBlink();
    void onFocusChanged(bool focused);
    void scrollCaretIntoView();
    void notifyHost();

    // ── 浮层（取色板 / 链接输入）────────────────────────────────────────────
    void closePopover();
    void openPalette(const Offset& global, bool background);
    void openLinkEditor(const Offset& global);
    SharedPtr<Widget> buildPalette(BuildContext& entryContext);
    SharedPtr<Widget> buildLinkEditor(BuildContext& entryContext);
    /// 浮层锚点：全局坐标 → 浮层坐标 + 贴边内收（口径同 ContextMenu）。
    Offset popoverOrigin(BuildContext& entryContext, double panelW, double panelH) const;

    SharedPtr<RichTextEditorController> ctl_;
    SharedPtr<FocusNode> node_;
    int ctlListener_ = -1;
    bool focused_ = false;
    bool caretOn_ = true;
    std::unique_ptr<Ticker> blink_;
    double lastBlinkMs_ = 0.0, blinkAcc_ = 0.0;

    ScrollController hScroll_, vScroll_;
    std::vector<LineGeom> geom_;
    RichEditorTheme ed_ = RichEditorTheme::snow();
    double visibleW_ = 0.0, visibleH_ = 0.0, contentH_ = 0.0;
    std::size_t caretGlyphWLine_ = 0;  ///< 仅供调试断言（build 记下的光标行号）

    std::string preedit_;
    RichDocument widgetMirror_;  ///< 上次与 widget 同步过的文档（判断宿主是否换过内容）

    SharedPtr<OverlayEntry> popover_;
    Offset popoverGlobal_{};
    bool popoverBackground_ = false;
    SharedPtr<TextEditingController> urlCtl_;
    /// 帧末回调（滚动光标）的存活哨兵：`dispose` 置 false，回调里先查它。
    std::shared_ptr<bool> alive_;
};

inline std::unique_ptr<State> RichTextEditor::createState() {
    return std::make_unique<RichTextEditorState>();
}

// ── 生命周期 ────────────────────────────────────────────────────────────────

inline void RichTextEditorState::initState() {
    auto* self = w();
    alive_ = std::make_shared<bool>(true);
    ctl_ = self->controller();
    node_ = self->focusNode();
    if (node_)
        node_->setEditable(true);  // 同 EditableText：防止祖先滚动容器抢焦点
    ed_ = self->editorTheme() ? *self->editorTheme() : RichEditorTheme::fromApp();
    State::initState();
    blink_ = std::make_unique<Ticker>([this](Duration elapsed) {
        const double nowMs = elapsed.count();
        double dt = nowMs - lastBlinkMs_;
        lastBlinkMs_ = nowMs;
        if (dt < 0.0)
            dt = 0.0;
        blinkAcc_ += dt;
        if (blinkAcc_ < kBlinkPeriodMs)
            return;
        blinkAcc_ = 0.0;
        caretOn_ = !caretOn_;
        setState();
    });
    widgetMirror_ = ctl_->document();
    if (!(self->document() == widgetMirror_)) {
        ctl_->setDocument(self->document());
        widgetMirror_ = ctl_->document();
    }
    ctlListener_ = ctl_->addListener([this] {
        notifyHost();
        setState();
    });
}

inline void RichTextEditorState::didUpdateWidget(cui::Widget* oldWidget) {
    auto* self = w();
    ed_ = self->editorTheme() ? *self->editorTheme() : RichEditorTheme::fromApp();
    if (self->controller() != ctl_) {
        if (ctl_ && ctlListener_ >= 0)
            ctl_->removeListener(ctlListener_);
        ctl_ = self->controller();
        ctlListener_ = ctl_->addListener([this] {
            notifyHost();
            setState();
        });
        widgetMirror_ = ctl_->document();
    }
    if (self->focusNode() != node_)
        node_ = self->focusNode();
    // 宿主直接换了 widget 的文档 ⇒ 采纳（编辑器的内容以宿主为准）。
    if (ctl_ && !(self->document() == widgetMirror_)) {
        ctl_->setDocument(self->document());
        widgetMirror_ = ctl_->document();
    }
    State::didUpdateWidget(oldWidget);
}

inline void RichTextEditorState::dispose() {
    closePopover();
    if (ctl_ && ctlListener_ >= 0) {
        ctl_->removeListener(ctlListener_);
        ctlListener_ = -1;
    }
    if (blink_)
        blink_->stop();
    State::dispose();
}

inline void RichTextEditorState::notifyHost() {
    widgetMirror_ = ctl_->document();
    auto* self = w();
    if (self->onChanged())
        self->onChanged()(ctl_->document());
    if (self->onSelectionChanged())
        self->onSelectionChanged()(ctl_->selection());
}

inline void RichTextEditorState::onFocusChanged(bool focused) {
    focused_ = focused;
    if (focused) {
        caretOn_ = true;
        blinkAcc_ = 0.0;
        lastBlinkMs_ = 0.0;
        if (blink_ && !blink_->isActive())
            blink_->start();
    } else {
        caretOn_ = false;
        if (blink_)
            blink_->stop();
        closePopover();
        preedit_.clear();
    }
    setState();
}

inline void RichTextEditorState::restartBlink() {
    caretOn_ = true;
    blinkAcc_ = 0.0;
    if (focused_ && blink_ && !blink_->isActive())
        blink_->start();
}

// ── 样式与度量（务必与 RenderParagraph::measureNatural 同口径）─────────────

inline TextStyle RichTextEditorState::baseStyleFor(const RichLine& ln) const {
    TextStyle s;
    s.size(ed_.bodySize);
    switch (ln.block) {
    case RichBlock::Header1: s.size(ed_.bodySize * 2.0).bold(); break;
    case RichBlock::Header2: s.size(ed_.bodySize * 1.5).bold(); break;
    case RichBlock::Header3: s.size(ed_.bodySize * 1.17).bold(); break;
    case RichBlock::Blockquote: s.setItalic(true); break;
    case RichBlock::CodeBlock:
        s.family(richMonoFamily().c_str()).size(std::round(ed_.bodySize * 0.92));
        break;
    case RichBlock::Normal: break;
    }
    s.fgColor(ln.block == RichBlock::Blockquote    ? ed_.blockquoteText
              : ln.block == RichBlock::CodeBlock    ? ed_.codeBlockForeground
                                                    : ed_.editorText);
    if (ln.block == RichBlock::CodeBlock)
        s.bgColor(ed_.codeBlockBackground);
    return s;
}

inline TextStyle RichTextEditorState::styleForRun(const RichRun& r,
                                                  const RichLine& ln) const {
    TextStyle s = baseStyleFor(ln);
    const RichInline& f = r.fmt;
    if (f.bold)
        s.weight(std::max(s.fontWeight, 700.0));
    if (f.italic)
        s.setItalic(true);
    if (f.underline)
        s.setUnderline(true);
    if (f.strike)
        s.setStrikethrough(true);
    if (ln.block != RichBlock::CodeBlock && f.code) {
        s.family(richMonoFamily().c_str());
        s.bgColor(ed_.inlineCodeBackground).fgColor(ed_.inlineCodeForeground);
    }
    if (!f.link.empty()) {
        s.setUnderline(true);
        s.fgColor(ed_.linkColor);
    }
    if (f.color.a >= 0)
        s.fgColor(f.color);
    if (f.background.a > 0)
        s.bgColor(f.background);
    if (!f.font.empty() && ln.block != RichBlock::CodeBlock) {
        const std::string fam = richResolveFont(f.font);
        s.family(fam.c_str());
    }
    if (f.size)
        s.size(richSizePx(*f.size, ed_.bodySize));
    if (f.script != RichScript::Normal)
        s.size(std::round(s.fontSize * 0.62));  // 见文件头偏离 2：没有基线偏移
    return s;
}

inline double RichTextEditorState::prefixWidth(const RichLine& ln, std::size_t col) const {
    double acc = 0.0;
    std::size_t pos = 0;
    for (const auto& r : ln.runs) {
        if (pos >= col)
            break;
        const TextStyle st = styleForRun(r, ln);
        if (pos + r.text.size() <= col) {
            acc += richWidthFor(st, r.text);
            pos += r.text.size();
        } else {
            acc += richWidthFor(st, r.text.substr(0, col - pos));
            pos = col;
            break;
        }
    }
    return acc;
}

inline std::size_t RichTextEditorState::colForX(const RichLine& ln, double dx) const {
    if (dx <= 0.0 || ln.runs.empty())
        return 0;
    double acc = 0.0;
    std::size_t pos = 0;
    for (const auto& r : ln.runs) {
        const TextStyle st = styleForRun(r, ln);
        std::size_t i = 0;
        while (i < r.text.size()) {
            const std::size_t ni = richNextBoundary(r.text, i + 1);
            const std::string piece = r.text.substr(i, ni - i);
            const double cw = std::max(0.5, richWidthFor(st, piece));
            if (acc + cw >= dx) {
                // 落在码点内：取更近的一侧
                return (dx - acc <= acc + cw - dx) ? pos : pos + (ni - i);
            }
            acc += cw;
            pos += ni - i;
            i = ni;
        }
    }
    return ln.byteLength();
}

inline RichPos RichTextEditorState::posAtLocal(double lx, double ly) const {
    if (geom_.empty())
        return ctl_->caret();
    std::size_t idx = geom_.size() - 1;
    for (std::size_t i = 0; i < geom_.size(); ++i) {
        if (ly < geom_[i].y + geom_[i].h) {
            idx = i;
            break;
        }
    }
    const LineGeom& g = geom_[idx];
    return RichPos{g.line, colForX(ctl_->document().line(g.line), lx - g.textX)};
}

inline RichPos RichTextEditorState::posAtLine(std::size_t lineIdx, double xInContent) const {
    for (const auto& g : geom_) {
        if (g.line == lineIdx)
            return RichPos{lineIdx, colForX(ctl_->document().line(lineIdx), xInContent - g.textX)};
    }
    return ctl_->document().lineStart(lineIdx);
}

inline double RichTextEditorState::caretXInContent() const {
    const RichPos p = ctl_->caret();
    for (const auto& g : geom_) {
        if (g.line == p.line)
            return g.textX + prefixWidth(ctl_->document().line(p.line), p.col);
    }
    return 0.0;
}

inline void RichTextEditorState::placeCaret(const PointerEvent& e, bool extend) {
    if (!ctl_ || !node_)
        return;
    const RichPos p = posAtLocal(e.position.dx, e.position.dy);
    if (extend)
        ctl_->moveTo(p, true);
    else
        ctl_->setSelection(p, p);
    node_->requestFocus();
    restartBlink();
}

inline void RichTextEditorState::scrollCaretIntoView() {
    if (!focused_ || geom_.empty())
        return;
    const double cx = caretXInContent();
    if (visibleW_ > 0.0) {
        if (cx < hScroll_.offset())
            hScroll_.jumpTo(std::max(0.0, cx - 8.0));
        else if (cx > hScroll_.offset() + visibleW_)
            hScroll_.jumpTo(cx - visibleW_ + 8.0);
    }
    if (visibleH_ > 0.0) {
        const std::size_t li = ctl_->caret().line;
        for (const auto& g : geom_) {
            if (g.line != li)
                continue;
            if (g.y < vScroll_.offset())
                vScroll_.jumpTo(std::max(0.0, g.y - 8.0));
            else if (g.y + g.h > vScroll_.offset() + visibleH_)
                vScroll_.jumpTo(g.y + g.h - visibleH_ + 8.0);
            break;
        }
    }
}

// ── 键盘（Focus → FocusNode::onKeyEvent）───────────────────────────────────

inline KeyEventResult RichTextEditorState::onKey(const KeyEvent& ev) {
    if (!ctl_ || !node_ || !node_->hasFocus())
        return KeyEventResult::Ignored;
    const bool ctrl = (ev.modifiers & (ModCtrl | ModMeta)) != 0;
    const bool shift = (ev.modifiers & ModShift) != 0;
    const bool alt = (ev.modifiers & ModAlt) != 0;

    if (ev.isPreedit) {
        // 组合进行中：把 preedit 画在光标处，事件继续交给宿主/IME。
        preedit_ = ev.text;
        setState();
        return KeyEventResult::Ignored;
    }

    if (ev.key == 0 && !ev.text.empty()) {
        if (ctrl) {
            const char c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(ev.text[0])));
            switch (c) {
            case 'b': ctl_->toggleFlag(RichAttr::Bold); restartBlink(); return KeyEventResult::Handled;
            case 'i': ctl_->toggleFlag(RichAttr::Italic); restartBlink(); return KeyEventResult::Handled;
            case 'u': ctl_->toggleFlag(RichAttr::Underline); restartBlink(); return KeyEventResult::Handled;
            case 'a': ctl_->selectAll(); restartBlink(); return KeyEventResult::Handled;
            case 'z': shift ? ctl_->redo() : ctl_->undo(); restartBlink(); return KeyEventResult::Handled;
            case 'y': ctl_->redo(); restartBlink(); return KeyEventResult::Handled;
            case 'x': ctl_->deleteSelection(); restartBlink(); return KeyEventResult::Handled;
            default: return KeyEventResult::Ignored;
            }
        }
        preedit_.clear();
        if (ev.text == "\n" || ev.text == "\r")
            ctl_->insertLineBreak();
        else
            ctl_->insertText(ev.text);
        restartBlink();
        return KeyEventResult::Handled;
    }

    const KeyCode k = static_cast<KeyCode>(ev.key);
    switch (k) {
    case KeyCode::Return:
        if (ctrl)
            ctl_->toggleCodeBlock();  // Quill：Ctrl+Enter 切代码块
        else if (alt)
            ctl_->toggleBlockquote();
        else
            ctl_->insertLineBreak();
        break;
    case KeyCode::Backspace:
        ctl_->deleteBackward();
        break;
    case KeyCode::Delete:
        ctl_->deleteForward();
        break;
    case KeyCode::Tab:
        if (ctl_->listAt() != RichListKind::None) {
            ctl_->changeIndent(shift ? -1 : 1);
            break;
        }
        if (ctl_->blockAt() == RichBlock::CodeBlock) {
            ctl_->insertText("    ");
            break;
        }
        if (ctl_->blockAt() == RichBlock::Blockquote) {
            ctl_->insertText("  ");
            break;
        }
        return KeyEventResult::Ignored;  // 交给焦点遍历（与网页手感一致）
    case KeyCode::Left:
        if (ctrl || alt)
            ctl_->moveHome(false, true);
        else
            ctl_->moveLeft(shift);
        break;
    case KeyCode::Right:
        if (ctrl || alt)
            ctl_->moveEnd(false, true);
        else
            ctl_->moveRight(shift);
        break;
    case KeyCode::Home:
        ctl_->moveHome(shift, ctrl);
        break;
    case KeyCode::End:
        ctl_->moveEnd(shift, ctrl);
        break;
    case KeyCode::Up:
    case KeyCode::Down: {
        const double x = caretXInContent();
        const int delta = k == KeyCode::Down ? 1 : -1;
        ctl_->moveVertical(delta, x,
                           [this](std::size_t l, double xx) { return posAtLine(l, xx); });
        break;
    }
    case KeyCode::PageUp:
        ctl_->moveVertical(-6, caretXInContent(),
                           [this](std::size_t l, double xx) { return posAtLine(l, xx); });
        break;
    case KeyCode::PageDown:
        ctl_->moveVertical(6, caretXInContent(),
                           [this](std::size_t l, double xx) { return posAtLine(l, xx); });
        break;
    case KeyCode::Escape:
        if (popover_)
            closePopover();
        else
            ctl_->setCaret(ctl_->caret());
        return KeyEventResult::Handled;
    default:
        return KeyEventResult::Ignored;
    }
    restartBlink();
    return KeyEventResult::Handled;
}

// ── 浮层 ────────────────────────────────────────────────────────────────────

/// 全屏透明 barrier：点浮层外任意处收起（口径同 ContextMenu）。
inline SharedPtr<Widget> richPopoverBarrier(std::function<void()> onClose) {
    auto box = cui::make_shared<Container>();
    box->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0)});
    auto gd = cui::make_shared<GestureDetector>(box, [onClose] { if (onClose) onClose(); });
    auto fill = cui::make_shared<Positioned>(gd);
    fill->hasLeft = fill->hasTop = fill->hasRight = fill->hasBottom = true;
    return fill;
}

inline void RichTextEditorState::closePopover() {
    if (popover_ && popover_->mounted())
        popover_->remove();
    popover_.reset();
    urlCtl_.reset();
}

inline Offset RichTextEditorState::popoverOrigin(BuildContext& entryContext, double panelW,
                                                 double panelH) const {
    RenderBox* overlay = nullptr;
    if (Element* host = entryContext.element()) {
        if (auto* roHost = host->ancestorRenderObjectElement())
            overlay = dynamic_cast<RenderBox*>(roHost->findRenderObject());
    }
    Offset origin = overlay ? overlay->globalToLocal(popoverGlobal_) : popoverGlobal_;
    if (overlay) {
        origin.dx = std::min(origin.dx, std::max(0.0, overlay->size().width - panelW));
        origin.dy = std::min(origin.dy, std::max(0.0, overlay->size().height - panelH));
    }
    return origin;
}

inline void RichTextEditorState::openPalette(const Offset& global, bool background) {
    if (!context())
        return;
    closePopover();
    popoverGlobal_ = global;
    popoverBackground_ = background;
    popover_ = cui::make_shared<OverlayEntry>(
        [this](BuildContext& ctx) { return buildPalette(ctx); }, false, false);
    OverlayState::of(*context())->insert(popover_);
}

inline void RichTextEditorState::openLinkEditor(const Offset& global) {
    if (!context())
        return;
    closePopover();
    popoverGlobal_ = global;
    urlCtl_ = cui::make_shared<TextEditingController>(ctl_->formatAtSelection().link);
    popover_ = cui::make_shared<OverlayEntry>(
        [this](BuildContext& ctx) { return buildLinkEditor(ctx); }, false, false);
    OverlayState::of(*context())->insert(popover_);
}

inline SharedPtr<Widget> RichTextEditorState::buildPalette(BuildContext& entryContext) {
    static constexpr double kPanelW = 150.0;
    static constexpr double kSwatch = 14.0, kGap = 3.0;
    const int rows = static_cast<int>((richPaletteColors().size() + 7) / 8);
    const double kPanelH = 12.0 + static_cast<double>(rows) * (kSwatch + kGap) + 26.0;
    const Offset origin = popoverOrigin(entryContext, kPanelW, kPanelH);

    std::vector<SharedPtr<Widget>> stack;
    stack.push_back(richPopoverBarrier([this] { closePopover(); }));

    std::vector<SharedPtr<Widget>> swatches;
    for (const char* hex : richPaletteColors()) {
        const Color c = colorFromHex(hex);
        auto cell = cui::make_shared<Container>();
        cell->width(kSwatch).height(kSwatch)
            .decoration(BoxDecoration{c, 2.0, 1.0, ed_.popoverBorder});
        swatches.push_back(cui::make_shared<GestureDetector>(cell, [this, c] {
            if (popoverBackground_)
                ctl_->setBackground(c);
            else
                ctl_->setColor(c);
            closePopover();
            if (node_)
                node_->requestFocus();
        }));
    }
    auto grid = cui::make_shared<Wrap>(Axis::Horizontal, std::move(swatches),
                                       WrapAlignment::Start, kGap, WrapAlignment::Start, kGap,
                                       WrapCrossAlignment::Center);
    grid->setSpacing(kGap);

    auto resetBox = cui::make_shared<Container>(cui::make_shared<Text>(
        popoverBackground_ ? "reset background" : "reset color",
        TextStyle().size(11.5).fgColor(ed_.buttonForeground)));
    resetBox->padding(EdgeInsets::symmetric(6.0, 4.0))
        .decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.05), 3.0});
    auto reset = cui::make_shared<GestureDetector>(resetBox, [this] {
        if (popoverBackground_)
            ctl_->setBackground(std::nullopt);
        else
            ctl_->setColor(std::nullopt);
        closePopover();
        if (node_)
            node_->requestFocus();
    });

    std::vector<SharedPtr<Widget>> panelKids{grid, cui::make_shared<SizedBox>(0.0, 6.0), reset};
    auto col = cui::make_shared<Column>(std::move(panelKids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    auto panel = cui::make_shared<Container>(col);
    panel->width(kPanelW).padding(EdgeInsets::all(6.0))
        .decoration(BoxDecoration{ed_.popoverBackground, 4.0, 1.0, ed_.popoverBorder});
    auto pos = cui::make_shared<Positioned>(panel);
    pos->left = origin.dx;
    pos->top = origin.dy;
    pos->hasLeft = true;
    pos->hasTop = true;
    stack.push_back(pos);
    return cui::make_shared<Stack>(std::move(stack));
}

inline SharedPtr<Widget> RichTextEditorState::buildLinkEditor(BuildContext& entryContext) {
    static constexpr double kPanelW = 250.0, kPanelH = 108.0;
    const Offset origin = popoverOrigin(entryContext, kPanelW, kPanelH);

    std::vector<SharedPtr<Widget>> stack;
    stack.push_back(richPopoverBarrier([this] {
        closePopover();
        if (node_)
            node_->requestFocus();
    }));

    InputDecoration dec;
    dec.hintText = "https://example.com";
    dec.borderColor = ed_.popoverBorder;
    auto input = cui::make_shared<TextField>(urlCtl_, dec,
                                             TextStyle().size(12.5).fgColor(ed_.editorText),
                                             TextAlign::Left, false, true, nullptr, nullptr,
                                             nullptr, [this](const std::string&) {
        const std::string url = urlCtl_->text();
        ctl_->setLink(url);
        closePopover();
        if (node_)
            node_->requestFocus();
    });

    auto applyChip = [this](const std::string& label, std::function<void()> fn) {
        auto box = cui::make_shared<Container>(
            cui::make_shared<Text>(label, TextStyle().size(12.0).fgColor(ed_.buttonForeground)));
        box->padding(EdgeInsets::symmetric(8.0, 4.0))
            .decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.06), 3.0});
        return cui::make_shared<GestureDetector>(box, [fn] { if (fn) fn(); });
    };
    std::vector<SharedPtr<Widget>> btns;
    btns.push_back(applyChip("Apply", [this] {
        ctl_->setLink(urlCtl_ ? urlCtl_->text() : std::string());
        closePopover();
        if (node_)
            node_->requestFocus();
    }));
    btns.push_back(cui::make_shared<SizedBox>(6.0, 0.0));
    btns.push_back(applyChip("Remove link", [this] {
        ctl_->setLink(std::string());
        closePopover();
        if (node_)
            node_->requestFocus();
    }));
    auto btnRow = cui::make_shared<Row>(std::move(btns));
    btnRow->setMainAxisSize(MainAxisSize::Min);
    btnRow->setCrossAxisAlignment(CrossAxisAlignment::Center);

    std::vector<SharedPtr<Widget>> panelKids{input, cui::make_shared<SizedBox>(0.0, 8.0), btnRow};
    auto col = cui::make_shared<Column>(std::move(panelKids));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto panel = cui::make_shared<Container>(col);
    panel->width(kPanelW).padding(EdgeInsets::all(8.0))
        .decoration(BoxDecoration{ed_.popoverBackground, 4.0, 1.0, ed_.popoverBorder});
    auto pos = cui::make_shared<Positioned>(panel);
    pos->left = origin.dx;
    pos->top = origin.dy;
    pos->hasLeft = true;
    pos->hasTop = true;
    stack.push_back(pos);
    return cui::make_shared<Stack>(std::move(stack));
}

// ── 渲染：snow 工具栏 + 逐行编辑区 ──────────────────────────────────────────

inline SharedPtr<Widget> RichTextEditorState::build(BuildContext& context) {
    auto* self = w();
    const RichDocument& doc = ctl_->document();
    const RichRange sel = ctl_->selection();
    const RichPos caret = ctl_->caret();
    const RichInline fmt = ctl_->formatAtSelection();
    const RichBlock blk = ctl_->blockAt();
    const RichListKind lst = ctl_->listAt();
    const RichAlign alg = ctl_->alignAt();

    static constexpr double kPadX = 15.0;       ///< Quill `.ql-editor` 的 1.5em 内边距
    static constexpr double kPadY = 12.0;
    static constexpr double kBorder = 1.0;
    static constexpr double kCellPadX = 1.0;    ///< 给行内 code 的背景留一点出血
    static constexpr double kMarkerW = 24.0;    ///< 列表符号列（bullet / "1."）
    static constexpr double kMarkerGap = 6.0;
    static constexpr double kQuoteBarW = 4.0;   ///< Quill blockquote 的 border-left
    static constexpr double kQuotePad = 20.0;   ///< …和它的 padding-left
    static constexpr double kCaretW = 1.6;
    static constexpr double kChipH = 22.0;
    static constexpr double kMinLineH = 20.0;

    const double innerW = std::max(80.0, self->width() - 2.0 * kPadX - 2.0 * kBorder);
    const bool composing = !preedit_.empty();
    const bool showPlaceholder =
        !composing && !self->placeholder().empty() && doc.plainText() == "\n";
    const Color selColor = focused_ ? ed_.selection : ed_.selectionInactive;

    // ── pass 1：逐行度量。口径与 RenderParagraph::measureNatural 完全一致 ──
    struct Metric {
        std::size_t line = 0;
        double indentPx = 0.0, markerW = 0.0, markerGap = 0.0, quotePad = 0.0;
        double textW = 0.0, textH = 0.0, lineH = 0.0;
        double alignX = -1.0;
        int number = 0;
    };
    std::vector<Metric> ms;
    ms.reserve(doc.lines.size());
    double contentW = innerW;
    double contentH = 0.0;
    {
        // 有序列表重新计数的粒度：连续同类型同缩进才算一列（Quill 同款观感）。
        std::vector<int> ordinal(doc.lines.size(), 0);
        int run = 0;
        for (std::size_t i = 0; i < doc.lines.size(); ++i) {
            const RichLine& ln = doc.lines[i];
            if (ln.list != RichListKind::Ordered) {
                run = 0;
                continue;
            }
            if (i > 0 && ordinal[i - 1] > 0 && doc.lines[i - 1].indent == ln.indent)
                ++run;
            else
                run = 1;
            ordinal[i] = run;
        }
        for (std::size_t i = 0; i < doc.lines.size(); ++i) {
            const RichLine& ln = doc.lines[i];
            Metric m;
            m.line = i;
            m.indentPx = std::max(0, ln.indent) * ed_.indentUnit;
            m.markerW = ln.list == RichListKind::None ? 0.0 : kMarkerW;
            m.markerGap = m.markerW > 0.0 ? kMarkerGap : 0.0;
            m.quotePad = ln.block == RichBlock::Blockquote ? kQuotePad : 0.0;
            m.number = ordinal[i];
            m.alignX = ln.align == RichAlign::Center  ? 0.0
                       : ln.align == RichAlign::Right ? 1.0
                                                      : -1.0;  // justify 见文件头偏离 5
            for (const auto& r : ln.runs) {
                const TextStyle st = styleForRun(r, ln);
                m.textW += richWidthFor(st, r.text);
                m.textH = std::max(m.textH, richHeightFor(st));
            }
            if (composing && i == caret.line) {
                const TextStyle pst = baseStyleFor(ln);
                m.textW += richWidthFor(pst, preedit_);
                m.textH = std::max(m.textH, richHeightFor(pst));
            }
            if (m.textH <= 0.0)
                m.textH = richHeightFor(baseStyleFor(ln));  // 空行走 strut（同 RenderParagraph）
            m.lineH = std::max(kMinLineH, std::ceil(m.textH * ed_.lineHeightFactor));
            contentW = std::max(contentW, m.indentPx + m.markerW + m.markerGap + m.quotePad +
                                              2.0 * kCellPadX + m.textW);
            contentH += m.lineH;
            ms.push_back(m);
        }
    }

    // ── 一行的 span 树：run × 选区 × preedit 三方切分 ──────────────────────
    // 本仓 RenderParagraph 只认「叶子 span 自带的 style」，不做父子样式继承，
    // 所以每个叶子都得拿一份完整解析后的 TextStyle。
    auto spanFor = [&](const RichLine& ln, std::size_t li, std::size_t n) -> SharedPtr<TextSpan> {
        auto root = cui::make_shared<TextSpan>();
        root->style = baseStyleFor(ln);

        std::size_t sb = 0, se = 0;
        if (!sel.collapsed() && li >= sel.start.line && li <= sel.end.line) {
            sb = li == sel.start.line ? sel.start.col : 0;
            se = li == sel.end.line ? sel.end.col : n;
        }
        const std::size_t peCol = (composing && li == caret.line) ? caret.col : n + 1;

        std::vector<std::size_t> cuts{0, n};
        auto addCut = [&cuts, n](std::size_t c) {
            if (c > 0 && c < n)
                cuts.push_back(c);
        };
        // run 边界必须全部切开：`RenderParagraph` 每段叶子只认一份 style，漏切
        // 就会把「选区之后的所有 run」画成同一种格式（粗体/等宽一路染到行尾）。
        {
            std::size_t acc = 0;
            for (const auto& r : ln.runs) {
                acc += r.text.size();
                addCut(acc);
            }
        }
        addCut(sb);
        addCut(se);
        addCut(peCol);
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

        auto styleAt = [&](std::size_t col) {
            std::size_t p = 0;
            for (const auto& r : ln.runs) {
                p += r.text.size();
                if (col < p)
                    return styleForRun(r, ln);
            }
            return styleForRun(ln.runs.empty() ? RichRun{} : ln.runs.back(), ln);
        };
        auto push = [&](const std::string& text, TextStyle st) {
            if (text.empty())
                return;
            root->children.push_back(TextSpan::make(text, st));
        };

        std::size_t todoPreedit = peCol;
        for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
            const std::size_t b = cuts[i], e = cuts[i + 1];
            if (b == todoPreedit) {
                TextStyle pst = baseStyleFor(ln);
                pst.fgColor(ed_.placeholder).setUnderline(true);
                push(preedit_, pst);
                todoPreedit = n + 1;
            }
            TextStyle st = styleAt(b);
            if (se > sb && b >= sb && e <= se)
                st.bgColor(selColor);
            push(doc.textInLine(li, b, e), st);
        }
        if (todoPreedit <= n) {
            TextStyle pst = baseStyleFor(ln);
            pst.fgColor(ed_.placeholder).setUnderline(true);
            push(preedit_, pst);
        }
        // 占位符只在「整篇为空」的第一行出现（Quill 的 data-placeholder）；
        // 它不计入 textW，所以比正文长时会溢出到横向滚动区外，属于可见即可。
        if (showPlaceholder && li == 0 && root->children.empty()) {
            TextStyle st = baseStyleFor(ln);
            st.fgColor(ed_.placeholder);
            push(self->placeholder(), st);
        }
        return root;
    };

    // ── pass 2：逐行出控件，同时记下几何（命中/光标共用这一份）─────────────
    std::vector<SharedPtr<Widget>> rows;
    rows.reserve(ms.size());
    geom_.clear();
    geom_.reserve(ms.size());
    double y = 0.0;
    for (const Metric& m : ms) {
        const RichLine& ln = doc.lines[m.line];
        const std::size_t n = ln.byteLength();
        const double cellX = m.indentPx + m.markerW + m.markerGap;
        const double cellW = std::max(0.0, contentW - cellX);
        // Align 负责真正的摆放，这里算的是「文本左端在内容坐标里的 x」，两边同一式子。
        const double avail = std::max(0.0, cellW - m.quotePad - 2.0 * kCellPadX - m.textW);
        const double alignExtra = avail * (m.alignX + 1.0) / 2.0;
        const double textX = cellX + m.quotePad + alignExtra + kCellPadX;
        const double textTop = (m.lineH - m.textH) / 2.0;
        geom_.push_back(LineGeom{m.line, y, m.lineH, textX, m.textW, textTop, m.textH});

        std::vector<SharedPtr<Widget>> stackKids;
        auto padded = cui::make_shared<Padding>(
            EdgeInsets::only(m.quotePad + kCellPadX, 0.0, kCellPadX, 0.0),
            cui::make_shared<RichText>(spanFor(ln, m.line, n)));
        stackKids.push_back(cui::make_shared<Align>(m.alignX, 0.0, padded));

        if (m.quotePad > 0.0) {
            auto bar = cui::make_shared<Container>();
            bar->decoration(BoxDecoration{ed_.blockquoteBar});
            auto pos = cui::make_shared<Positioned>(bar);
            pos->left = 0.0;
            pos->top = 0.0;
            pos->width = kQuoteBarW;
            pos->height = m.lineH;
            pos->hasLeft = pos->hasTop = pos->hasWidth = pos->hasHeight = true;
            stackKids.push_back(pos);
        }
        if (caretVisible() && caret.line == m.line) {
            auto caretBox = cui::make_shared<Container>();
            caretBox->decoration(BoxDecoration{ed_.caret});
            auto pos = cui::make_shared<Positioned>(caretBox);
            pos->left = textX - cellX + prefixWidth(ln, caret.col);
            pos->top = textTop;
            pos->width = kCaretW;
            pos->height = m.textH;
            pos->hasLeft = pos->hasTop = pos->hasWidth = pos->hasHeight = true;
            stackKids.push_back(pos);
        }
        auto cell = cui::make_shared<Container>(cui::make_shared<Stack>(std::move(stackKids)));
        cell->width(cellW).height(m.lineH);

        std::vector<SharedPtr<Widget>> cells;
        if (m.indentPx > 0.0)
            cells.push_back(cui::make_shared<SizedBox>(m.indentPx, m.lineH));
        if (m.markerW > 0.0) {
            const std::string mark =
                ln.list == RichListKind::Bullet ? std::string("\xE2\x80\xA2")
                                                 : std::to_string(m.number) + ".";
            auto markBox = cui::make_shared<Container>(cui::make_shared<Text>(
                mark, TextStyle().size(ed_.bodySize).fgColor(ed_.editorText)));
            markBox->width(m.markerW).height(m.lineH).alignment(1.0, 0.0);
            cells.push_back(markBox);
            cells.push_back(cui::make_shared<SizedBox>(m.markerGap, m.lineH));
        }
        cells.push_back(cell);

        auto row = cui::make_shared<Row>(std::move(cells));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto rowBox = cui::make_shared<Container>(row);
        rowBox->width(contentW).height(m.lineH)
            .decoration(BoxDecoration{ln.block == RichBlock::CodeBlock
                                          ? ed_.codeBlockBackground
                                          : Color::fromRGBA(0, 0, 0, 0.0)});
        rows.push_back(rowBox);
        y += m.lineH;
    }
    caretGlyphWLine_ = caret.line;

    auto col = cui::make_shared<Column>(std::move(rows));
    col->setMainAxisSize(MainAxisSize::Min);
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);

    // 按下点光标、拖动扩展选区（GestureDetector 把 down→move 合成 pan 语义）。
    auto content = cui::make_shared<GestureDetector>(
        col, nullptr, nullptr, nullptr,
        [this](const PointerEvent& e) { placeCaret(e, false); },
        [this](const PointerEvent& e) { placeCaret(e, true); });
    auto contentBox = cui::make_shared<Container>(content);
    contentBox->width(contentW).height(contentH).alignment(-1.0, -1.0);

    // 横向滚动需要确定高度（同 CodeEditor）：外层页面常是竖向无界 ScrollView。
    auto hBox = cui::make_shared<Container>(cui::make_shared<SingleChildScrollView>(
        &hScroll_, contentBox, Axis::Horizontal, false));
    hBox->height(contentH);
    visibleW_ = innerW;

    SharedPtr<Widget> scroller = hBox;
    if (self->maxHeight() && *self->maxHeight() > 0.0) {
        auto vBox = cui::make_shared<Container>(cui::make_shared<SingleChildScrollView>(
            &vScroll_, scroller, Axis::Vertical, false));
        vBox->height(*self->maxHeight());
        visibleH_ = *self->maxHeight();
        scroller = cui::make_shared<Scrollbar>(&vScroll_, vBox, nullptr, 6.0);
    } else {
        visibleH_ = 0.0;
    }
    auto body = cui::make_shared<Container>(scroller);
    body->padding(EdgeInsets::symmetric(kPadX, kPadY))
        .decoration(BoxDecoration{ed_.editorBackground});

    // ── 工具栏：snow 的模块顺序，图标集没有 Quill 那套字形 ⇒ 文字标签 ──────
    const TextStyle chipStyle = TextStyle().size(12.5);
    auto chipBox = [&](const std::string& label, bool active, bool enabled) {
        TextStyle st = chipStyle;
        st.fgColor(!enabled ? ed_.placeholder
                            : active ? ed_.buttonActiveForeground
                                     : ed_.buttonForeground);
        if (active)
            st.weight(700);
        const double cw = std::max(24.0, std::ceil(richWidthFor(chipStyle, label) + 14.0));
        auto box = cui::make_shared<Container>(
            cui::make_shared<Center>(cui::make_shared<Text>(label, st)));
        box->width(cw).height(kChipH)
            .decoration(BoxDecoration{active ? ed_.buttonActiveBackground
                                             : Color::fromRGBA(0, 0, 0, 0.0),
                                       3.0});
        return box;
    };
    auto tapChip = [&](const std::string& label, bool active, std::function<void()> fn,
                       bool enabled = true) -> SharedPtr<Widget> {
        auto box = chipBox(label, active, enabled);
        if (!fn || !enabled)
            return box;
        return cui::make_shared<GestureDetector>(box, [fn] { if (fn) fn(); });
    };
    // 取色板 / 链接浮层要按按下点定位 ⇒ 回调得拿到事件本身。
    auto downChip = [&](const std::string& label, bool active,
                        std::function<void(const PointerEvent&)> fn) -> SharedPtr<Widget> {
        return cui::make_shared<GestureDetector>(chipBox(label, active, true), nullptr,
                                                 std::move(fn));
    };
    auto menuChip = [&](const std::string& label, bool active, std::vector<PopupMenuItem> items,
                        std::function<void(int)> fn) -> SharedPtr<Widget> {
        return cui::make_shared<PopupMenuButton>(std::move(items), std::move(fn),
                                                 chipBox(label, active, true));
    };
    auto divider = [this]() -> SharedPtr<Widget> {
        auto d = cui::make_shared<Container>();
        d->width(1.0).height(16.0).margin(EdgeInsets::only(0.0, 0.0, 3.0, 0.0))
            .decoration(BoxDecoration{ed_.toolbarDivider});
        return d;
    };

    const char* headerLabel = blk == RichBlock::Header1   ? "H1"
                              : blk == RichBlock::Header2 ? "H2"
                              : blk == RichBlock::Header3 ? "H3"
                                                          : "Aa";
    const char* alignLabel = alg == RichAlign::Center    ? "center"
                             : alg == RichAlign::Right   ? "right"
                             : alg == RichAlign::Justify ? "justify"
                                                         : "left";

    std::vector<SharedPtr<Widget>> tb;
    tb.push_back(menuChip(
        headerLabel, blk != RichBlock::Normal && blk != RichBlock::Blockquote &&
                         blk != RichBlock::CodeBlock,
        {{"Normal text", 0}, {"Heading 1", 1}, {"Heading 2", 2}, {"Heading 3", 3}},
        [this](int v) { ctl_->setHeader(v); }));
    tb.push_back(tapChip("\"", blk == RichBlock::Blockquote,
                         [this] { ctl_->toggleBlockquote(); }));
    tb.push_back(tapChip("{ }", blk == RichBlock::CodeBlock,
                         [this] { ctl_->toggleCodeBlock(); }));
    tb.push_back(divider());
    tb.push_back(tapChip("B", fmt.bold, [this] { ctl_->toggleFlag(RichAttr::Bold); }));
    tb.push_back(tapChip("I", fmt.italic, [this] { ctl_->toggleFlag(RichAttr::Italic); }));
    tb.push_back(tapChip("U", fmt.underline, [this] { ctl_->toggleFlag(RichAttr::Underline); }));
    tb.push_back(tapChip("S", fmt.strike, [this] { ctl_->toggleFlag(RichAttr::Strike); }));
    tb.push_back(tapChip("</>", fmt.code, [this] { ctl_->toggleFlag(RichAttr::InlineCode); }));
    tb.push_back(divider());
    tb.push_back(tapChip("\xE2\x80\xA2", lst == RichListKind::Bullet,
                         [this] { ctl_->toggleList(RichListKind::Bullet); }));
    tb.push_back(tapChip("1.", lst == RichListKind::Ordered,
                         [this] { ctl_->toggleList(RichListKind::Ordered); }));
    // 缩进按钮用 ASCII：U+21E4/U+21E5 在本机的字体里没有字形，会渲染成空白。
    tb.push_back(tapChip("|<", false, [this] { ctl_->changeIndent(-1); }));
    tb.push_back(tapChip(">|", false, [this] { ctl_->changeIndent(1); }));
    tb.push_back(menuChip(
        alignLabel, alg != RichAlign::Left,
        {{"Left", 0}, {"Center", 1}, {"Right", 2}, {"Justify", 3}},
        [this](int v) { ctl_->setAlign(static_cast<RichAlign>(v)); }));
    tb.push_back(divider());
    tb.push_back(menuChip(
        "size", fmt.size.has_value(),
        {{"Small", 0}, {"Normal", 1}, {"Large", 2}, {"Huge", 3}},
        [this](int v) {
            if (v == 1)
                ctl_->setSize(std::nullopt);
            else
                ctl_->setSize(static_cast<RichSize>(v < 1 ? 0 : v - 1));
        }));
    tb.push_back(menuChip(
        "font", !fmt.font.empty(),
        {{"Sans-serif", 0}, {"Serif", 1}, {"Monospace", 2}},
        [this](int v) { ctl_->setFont(v == 0 ? std::string() : v == 1 ? "serif" : "monospace"); }));
    tb.push_back(menuChip(
        "script", fmt.script != RichScript::Normal,
        {{"Normal", 0}, {"Superscript", 1}, {"Subscript", 2}},
        [this](int v) { ctl_->setScript(static_cast<RichScript>(v)); }));
    tb.push_back(divider());
    tb.push_back(downChip("A", fmt.color.a >= 0,
                          [this](const PointerEvent& e) { openPalette(e.globalPosition, false); }));
    tb.push_back(downChip("ab", fmt.background.a > 0,
                          [this](const PointerEvent& e) { openPalette(e.globalPosition, true); }));
    tb.push_back(downChip("link", !fmt.link.empty(),
                          [this](const PointerEvent& e) { openLinkEditor(e.globalPosition); }));
    tb.push_back(divider());
    tb.push_back(tapChip("clean", false, [this] { ctl_->clean(); }));
    tb.push_back(tapChip("\xE2\x86\xBA", false, [this] { ctl_->undo(); }, ctl_->canUndo()));
    tb.push_back(tapChip("\xE2\x86\xBB", false, [this] { ctl_->redo(); }, ctl_->canRedo()));

    auto wrap = cui::make_shared<Wrap>(Axis::Horizontal, std::move(tb), WrapAlignment::Start, 0.0,
                                       WrapAlignment::Start, 4.0, WrapCrossAlignment::Center);
    auto toolbarBox = cui::make_shared<Container>(wrap);
    toolbarBox->padding(EdgeInsets::symmetric(5.0, 5.0))
        .decoration(BoxDecoration{ed_.toolbarBackground});
    auto rule = cui::make_shared<Container>();
    rule->height(1.0).decoration(BoxDecoration{ed_.toolbarBorder});

    std::vector<SharedPtr<Widget>> chrome{toolbarBox, rule, body};
    auto shell = cui::make_shared<Column>(std::move(chrome));
    shell->setMainAxisSize(MainAxisSize::Min);
    shell->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto outer = cui::make_shared<Container>(cui::make_shared<ClipRRect>(4.0, shell));
    outer->width(self->width())
        .decoration(BoxDecoration{ed_.editorBackground, 4.0, kBorder, ed_.editorBorder});

    // 光标滚进可视区要等本帧布局完（extents 才新鲜）⇒ 推到帧末，且带存活哨兵。
    BuildOwner* owner = context.element() ? context.element()->owner() : nullptr;
    if (alive_ && owner) {
        auto alive = alive_;
        owner->addPostFrameCallback([this, alive](Duration) {
            if (!*alive || !mounted())
                return;
            scrollCaretIntoView();
        });
    }
    return cui::make_shared<Focus>(
        node_, outer, [this](bool f) { onFocusChanged(f); }, self->autofocus(), nullptr,
        [this](const KeyEvent& ev) { return onKey(ev); });
}

}  // namespace cui_core
