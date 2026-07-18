/// @file test_rich_text_editor.cpp
/// @brief RichTextEditor（对齐 Quill）的文档模型、控制器命令与可编辑渲染测试。
///
/// 分八段：
///   1. 文档模型：`fromPlainText` / `plainText`（Quill `getText()` 收尾换行）/
///      `deltaJson`（`getContents()`）/ `optimize` 合 run / `textInRange` / `clamp`。
///   2. 插入与 pending 格式：折叠光标下 `applyFormat` 落进 pending，下一批输入带上；
///      `formatAtSelection` 折叠时读左侧字符。
///   3. 回车语义：列表延续、空列表行退格式、标题不延续、对齐继承、代码块整进整出。
///   4. 退格语义：选区 → 字符 → 行首「先降级块格式再并线」→ 文档首返回 false。
///   5. 选区移动：左右跨行、Home/End、Shift 扩展、Ctrl+A、上下行走渲染侧 hit。
///   6. 格式命令：`applyInline` / `clean`（块格式保留）/ `toggleCodeBlock` 清行内 /
///      `changeIndent` 夹范围 / `setHeader` 与列表互斥 / 历史深度与监听句柄。
///   7. 渲染与交互（headless 三树）：逐行几何、真实键入、点击放置光标、拖选、
///      工具栏 chip、长行横向滚动把光标滚进可视区、宿主回调。
///   8. 卸载后控制器监听摘干净（句柄式摘除铁律）。
///
/// 全部 headless：帧泵只做 flushBuildScope → flushLayout →（帧末回调）→ 再泵一次。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/focus.h"
#include "cui/framework/framework.h"
#include "cui/platform/event.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/rich_text_editor.h"
#include "cui/widgets/scroll.h"
#include "cui/widgets/text.h"

using namespace cui;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

/// 挂载 Widget 树到 BuildOwner + RenderView + PipelineOwner（headless）。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    /// 根外包 pinned Align：RenderView 给的是窗口大小的紧约束，代理盒子会一路
    /// biggest() 撑满，区域尺寸既测不出来也点不准。
    void attach(SharedPtr<Widget> body, double ww = 640, double hh = 480) {
        rv.setWindowSize({ww, hh});
        auto pinned = cui::make_shared<Align>(-1.0, -1.0, std::move(body));
        auto e = pinned->createElement(pinned);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        pump();
    }

    /// 一帧：建树 → 布局 → 帧末回调（编辑器把「光标滚进可视区」推到这里）→ 再布局。
    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
        owner.flushPostFrameCallbacks();
        owner.flushBuildScope();
        po.flushLayout();
    }

    FocusNode* primary() { return owner.focusManager().primaryFocus(); }

    bool key(KeyCode code, const std::string& text = std::string(), int mods = 0) {
        KeyEvent ev{static_cast<int>(code), text, mods, false};
        bool handled = owner.focusManager().sendKeyEvent(ev);
        pump();
        return handled;
    }
    bool type(const std::string& s) {
        bool all = true;
        for (char c : s)
            all = key(KeyCode::None, std::string(1, c)) && all;
        return all;
    }

    void down(double x, double y) {
        PointerDownEvent d;
        d.position = {x, y};
        d.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(d);
        pump();
    }
    void movePointer(double x, double y) {
        PointerMoveEvent m;
        m.position = {x, y};
        m.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(m);
        pump();
    }
    void up(double x, double y) {
        PointerUpEvent u;
        u.position = {x, y};
        po.dispatchPointerEvent(u);
        pump();
    }
    void tap(double x, double y) {
        down(x, y);
        up(x, y);
    }

    template <typename WidgetT, typename StateT>
    StateT* findState() {
        StateT* found = nullptr;
        std::function<void(Element*)> visit = [&](Element* e) {
            if (!e || found)
                return;
            if (auto* se = dynamic_cast<StatefulElement*>(e))
                if (dynamic_cast<WidgetT*>(se->widget())) {
                    found = static_cast<StateT*>(se->state());
                    return;
                }
            e->visitChildren([&](Element* child) { visit(child); });
        };
        visit(root);
        return found;
    }

    /// 整段文本相等的段落。正文一行的段落 = 该行纯文本，工具栏 chip 的标签也是
    /// 独立段落，所以「相等」能把它们从彼此的兄弟里挑出来。
    RenderParagraph* findParagraphExact(const std::string& text) {
        RenderParagraph* found = nullptr;
        std::function<void(RenderObject*)> visit = [&](RenderObject* ro) {
            if (!ro || found)
                return;
            if (auto* p = dynamic_cast<RenderParagraph*>(ro))
                if (p->text() && p->text()->toPlainText() == text)
                    found = p;
            ro->visitChildren(
                [&](AbstractNode* n) { visit(static_cast<RenderObject*>(n)); });
        };
        visit(root->findRenderObject());
        return found;
    }

    /// 点段落中心（chip 用）/ 点段落左端外一格（正文用）。
    Offset paraCenter(RenderParagraph* p) {
        return p->localToGlobal(Offset(p->size().width / 2.0, p->size().height / 2.0));
    }
    Offset paraGlobalLeft(RenderParagraph* p) { return p->localToGlobal(Offset(0.0, 0.0)); }
};

/// 用来把编辑器整棵子树摘掉的探针。
class SwapProbe : public StatefulWidget {
public:
    explicit SwapProbe(SharedPtr<Widget> content)
        : StatefulWidget(nullptr), content_(std::move(content)) {}
    const SharedPtr<Widget>& content() const { return content_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SwapProbe"; }

private:
    SharedPtr<Widget> content_;
};

class SwapProbeState : public State {
public:
    bool hidden = false;
    SharedPtr<Widget> build(BuildContext&) override {
        if (hidden)
            return cui::make_shared<SizedBox>(200.0, 20.0);
        return static_cast<SwapProbe*>(widget())->content();
    }
};

inline std::unique_ptr<State> SwapProbe::createState() {
    return std::make_unique<SwapProbeState>();
}

// ── 1. 文档模型 ─────────────────────────────────────────────────────────────
void testDocumentModel() {
    std::printf("[test] 文档模型（Delta / getText / optimize）\n");

    RichDocument doc = RichDocument::fromPlainText("hello\nworld");
    check(doc.lineCount() == 2, "两行文本 ⇒ 两个块");
    check(doc.plainText() == "hello\nworld\n", "plainText 以换行收尾（Quill getText）");
    check(doc.line(0).runs.size() == 1, "每行一个默认 run");
    check(doc.deltaJson() ==
              "[{\"insert\":\"hello\"},{\"insert\":\"\\n\"},{\"insert\":\"world\"},"
              "{\"insert\":\"\\n\"}]",
          "未加格式时 Delta 只有 insert");

    RichLine ln;
    ln.runs = {RichRun{"ab", RichInline{}}, RichRun{"", RichInline{}},
               RichRun{"cd", RichInline{}}};
    ln.optimize();
    check(ln.runs.size() == 1 && ln.runs[0].text == "abcd", "optimize 丢空 run 并合并同格式");

    RichInline bold;
    bold.bold = true;
    ln.runs = {RichRun{"x", bold}, RichRun{"y", RichInline{}}};
    ln.optimize();
    check(ln.runs.size() == 2, "格式不同 ⇒ 不合并");
    check(ln.runs[0].fmt.attributesJson() == "\"bold\":true", "run 属性序列化成 Quill 拼写");

    RichLine blk;
    blk.block = RichBlock::Header2;
    blk.list = RichListKind::Ordered;
    blk.indent = 2;
    blk.align = RichAlign::Center;
    check(blk.blockAttributesJson() ==
              "\"header\":2,\"list\":\"ordered\",\"indent\":2,\"align\":\"center\"",
          "块属性顺序与 Quill 的 attributes 对象一致");

    check(RichDocument::fromPlainText("x").deltaJson().find("\"insert\":\"x\"") !=
              std::string::npos,
          "单行文档也有 insert");

    RichDocument d2 = RichDocument::fromPlainText("hello\nworld");
    check(d2.textInRange(RichRange{{0, 1}, {1, 3}}) == "ello\nwor", "跨行选区纯文本带换行");
    check(d2.textInRange(RichRange{{0, 2}, {0, 2}}).empty(), "折叠选区无文本");
    check(d2.lineEnd(1) == RichPos{1, 5}, "lineEnd 用字节长度");
    check(d2.clamp(RichPos{99, 99}) == RichPos{1, 5}, "clamp 夹到文档末尾");
    check(d2.ordered(RichPos{1, 1}, RichPos{0, 1}) == RichRange{{0, 1}, {1, 1}},
          "ordered 把反向选区摆正");
}

// ── 2. 插入与 pending 格式 ──────────────────────────────────────────────────
void testInsertAndPending() {
    std::printf("[test] 插入 / pending 格式（Quill format 未落地语义）\n");

    RichTextEditorController c;
    check(c.plainText() == "\n", "空文档 = 一个空行 + 收尾换行");
    check(!c.insertText(""), "空串插入被拒");
    check(c.insertText("ab"), "插入 ab");
    check(c.caret() == RichPos{0, 2}, "光标落在插入末尾");

    RichInline v;
    v.bold = true;
    c.applyFormat(RichAttr::Bold, v);
    check(c.pending().bold, "折叠光标 ⇒ 格式进 pending");
    check(!c.document().line(0).runs[0].fmt.bold, "pending 不改文档");

    c.insertText("cd");
    const RichLine& ln = c.document().line(0);
    check(ln.runs.size() == 2, "插入后按格式切成两个 run");
    check(ln.runs[1].text == "cd" && ln.runs[1].fmt.bold, "新字符带上 pending 格式");
    check(!c.pending().bold, "落地后 pending 清空");

    c.setSelection(RichPos{0, 0}, RichPos{0, 2});
    check(!c.formatAtSelection().bold, "选区格式取一致值（这段没粗体）");
    c.toggleFlag(RichAttr::Bold);
    check(c.document().line(0).runs.size() == 1, "整行同格式 ⇒ optimize 合成一个 run");
    check(c.document().line(0).runs[0].text == "abcd", "文本未变");

    // 折叠光标读「左侧字符」的格式（Quill getFormat 的口径）
    c.setCaret(RichPos{0, 4});
    check(c.formatAtSelection().bold, "光标左侧是粗体 ⇒ 读到 bold");
    c.setCaret(RichPos{0, 0});
    check(!c.formatAtSelection().bold, "行首左侧无字符 ⇒ 无格式");

    // 代码块内输入不带行内格式（Quill 的 code blot 是纯净的）
    c.setPlainText("let x = 1");
    c.selectAll();
    c.toggleCodeBlock();
    c.setCaret(RichPos{0, 9});
    RichInline ital;
    ital.italic = true;
    c.applyFormat(RichAttr::Italic, ital);
    c.insertText("y");
    check(!c.document().line(0).runs.back().fmt.italic, "代码块内 pending 被忽略");
    check(c.document().line(0).runs.size() == 1, "代码块内仍是一个纯净 run");

    // 替换选区：光标在中间，选走一段再打字
    c.setPlainText("one two three");
    c.setSelection(RichPos{0, 4}, RichPos{0, 7});
    c.insertText("2");
    check(c.plainText() == "one 2 three\n", "打字先替换选区");
    check(c.caret() == RichPos{0, 5}, "替换后光标在插入末尾");
}

// ── 3. 回车语义 ─────────────────────────────────────────────────────────────
void testLineBreak() {
    std::printf("[test] 回车的块属性（Quill Enter）\n");

    RichTextEditorController c;
    c.setPlainText("one\ntwo");
    c.selectAll();
    c.toggleList(RichListKind::Bullet);
    check(c.document().line(0).list == RichListKind::Bullet, "选区覆盖两行 ⇒ 都进列表");
    check(c.document().line(1).list == RichListKind::Bullet, "第二行也是列表");

    c.setCaret(RichPos{0, 3});
    c.insertLineBreak();
    check(c.document().lineCount() == 3, "拆行");
    check(c.document().line(1).list == RichListKind::Bullet, "新行延续列表");
    check(c.caret() == RichPos{1, 0}, "光标到新行行首");

    c.insertLineBreak();
    check(c.document().lineCount() == 3, "空列表行回车不再拆行");
    check(c.document().line(1).list == RichListKind::None, "而是退出列表");
    check(c.plainText() == "one\n\ntwo\n", "文本未变");

    c.setPlainText("title");
    c.selectAll();
    c.setHeader(1);
    check(c.document().line(0).block == RichBlock::Header1, "设为标题");
    c.setCaret(RichPos{0, 5});
    c.insertLineBreak();
    check(c.document().line(1).block == RichBlock::Normal, "标题回车后新行回正文");

    c.setPlainText("centered");
    c.selectAll();
    c.setAlign(RichAlign::Center);
    c.setCaret(RichPos{0, 4});
    c.insertLineBreak();
    check(c.document().line(1).align == RichAlign::Center, "对齐继承（Quill align blot）");

    c.setPlainText("code");
    c.selectAll();
    c.toggleCodeBlock();
    check(c.document().line(0).block == RichBlock::CodeBlock, "整段进代码块");
    c.setCaret(RichPos{0, 4});
    c.insertLineBreak();
    check(c.document().line(1).block == RichBlock::CodeBlock, "代码块内回车延续");
    c.insertLineBreak();
    check(c.document().line(1).block == RichBlock::Normal, "代码块空行回车退出代码块");

    c.setPlainText("all code");
    c.selectAll();
    c.toggleCodeBlock();
    c.setCaret(RichPos{0, 2});
    RichInline u;
    u.underline = true;
    c.applyFormat(RichAttr::Underline, u);
    c.insertText("z");
    check(!c.document().line(0).runs.back().fmt.underline, "代码块内输入清行内格式");
}

// ── 4. 退格语义 ─────────────────────────────────────────────────────────────
void testDeleteBackward() {
    std::printf("[test] 退格：降级块格式再并线\n");

    RichTextEditorController c;
    c.setPlainText("abc");
    check(!c.deleteBackward(), "setDocument 后光标在文档首 ⇒ 退格无效");
    check(c.plainText() == "abc\n", "没改动");

    c.setCaret(RichPos{0, 3});
    c.deleteBackward();
    check(c.plainText() == "ab\n", "删掉一个字符");

    c.setSelection(RichPos{0, 0}, RichPos{0, 2});
    c.deleteBackward();
    check(c.plainText() == "\n", "有选区时先删选区");

    // 行首退格：块格式不同 ⇒ 第一次只降级，第二次才并线
    c.setPlainText("abc\ndef");
    c.setSelection(RichPos{0, 3}, RichPos{1, 0});
    c.setHeader(2);
    check(c.document().line(0).block == RichBlock::Header2, "选区止于次行行首 ⇒ 只改第一行");
    check(c.document().line(1).block == RichBlock::Normal, "第二行仍是正文");
    c.setCaret(RichPos{1, 0});
    c.deleteBackward();
    check(c.document().lineCount() == 2, "块格式不同 ⇒ 第一次退格不并线");
    check(c.document().line(1).block == RichBlock::Header2, "当前行先降级成上一行的块格式");
    check(c.plainText() == "abc\ndef\n", "文本未变");
    c.deleteBackward();
    check(c.document().lineCount() == 1, "再退格才并线");
    check(c.plainText() == "abcdef\n", "并线后文本相接");
    check(c.caret() == RichPos{0, 3}, "并线后落在接缝处");

    // 列表同样先降级
    c.setPlainText("item\nnext");
    c.setCaret(RichPos{1, 0});
    c.toggleList(RichListKind::Bullet);
    check(c.document().line(0).list == RichListKind::None, "折叠光标 ⇒ 只作用当前行");
    check(c.document().line(1).list == RichListKind::Bullet, "第二行进列表");
    c.deleteBackward();
    check(c.document().lineCount() == 2, "列表不同 ⇒ 第一次退格只去列表");
    check(c.document().line(1).list == RichListKind::None, "第二行退出列表");
    c.deleteBackward();
    check(c.document().lineCount() == 1, "第二次并线");

    // 码点：一次退格删掉整个多字节字符
    c.setPlainText("aé");
    c.setCaret(RichPos{0, 3});
    c.deleteBackward();
    check(c.plainText() == "a\n", "退格按码点回退，不留在续字节里");

    // deleteForward
    c.setPlainText("ab\ncd");
    c.setCaret(RichPos{0, 1});
    c.deleteForward();
    check(c.plainText() == "a\ncd\n", "行内向前删一个字符");
    c.setCaret(RichPos{0, 1});
    c.deleteForward();
    check(c.plainText() == "acd\n", "行尾向前删 ⇒ 并入下一行");
    c.setCaret(c.document().docEnd());
    check(!c.deleteForward(), "文档末尾向前删无效");
}

// ── 5. 选区移动 ─────────────────────────────────────────────────────────────
void testSelectionMoves() {
    std::printf("[test] 选区移动 / Ctrl+A\n");

    RichTextEditorController c;
    c.setPlainText("ab\ncd\nef");

    c.setCaret(RichPos{1, 0});
    c.moveLeft(false);
    check(c.caret() == RichPos{0, 2}, "行首向左 ⇒ 上一行行尾");
    c.moveRight(false);
    check(c.caret() == RichPos{1, 0}, "向右回来");
    c.moveRight(false);
    check(c.caret() == RichPos{1, 1}, "行内向右一格");

    c.setSelection(RichPos{1, 1}, RichPos{1, 1});
    c.moveLeft(true);
    check(c.selection() == RichRange{{1, 0}, {1, 1}}, "Shift+左扩展选区");
    check(c.hasSelection(), "选区非折叠");
    check(c.base() == RichPos{1, 1}, "base 留在锚点");
    check(c.caret() == RichPos{1, 0}, "caret 是移动端");

    c.setCaret(RichPos{1, 2});
    c.moveHome(false, false);
    check(c.caret() == RichPos{1, 0}, "Home ⇒ 行首");
    c.moveEnd(false, false);
    check(c.caret() == RichPos{1, 2}, "End ⇒ 行尾");
    c.moveHome(false, true);
    check(c.caret() == c.document().docStart(), "Ctrl+Home ⇒ 文档首");
    c.moveEnd(false, true);
    check(c.caret() == c.document().docEnd(), "Ctrl+End ⇒ 文档尾");

    c.selectAll();
    check(c.selection() == RichRange{{0, 0}, {2, 2}}, "selectAll = 全文");
    check(c.document().textInRange(c.selection()) == "ab\ncd\nef", "全选纯文本");

    c.setCaret(c.document().docEnd());
    c.moveRight(false);
    check(c.caret() == c.document().docEnd(), "文档尾向右不动");
    c.setCaret(c.document().docStart());
    c.moveLeft(false);
    check(c.caret() == c.document().docStart(), "文档首向左不动");

    // 上下移动走渲染侧的 hit 回调（保持像素 x）
    c.setCaret(RichPos{2, 1});
    int hits = 0;
    c.moveVertical(-1, 42.0, [&](std::size_t l, double x) {
        ++hits;
        return RichPos{l, x > 20.0 ? std::size_t(2) : std::size_t(0)};
    });
    check(hits == 1 && c.caret() == RichPos{1, 2}, "按 hit 回调的落点放光标");
    c.moveVertical(-9, 0.0, nullptr);
    check(c.caret() == RichPos{0, 0}, "向上越界夹到首行行首");
    c.moveVertical(9, 0.0, nullptr);
    check(c.caret() == RichPos{2, 0}, "向下越界夹到末行行首");
    c.setCaret(RichPos{1, 1});
    c.moveVertical(0, 5.0, nullptr);
    check(c.caret() == RichPos{1, 1}, "delta 0 不动");
}

// ── 6. 格式命令与历史 ───────────────────────────────────────────────────────
void testFormatCommandsAndHistory() {
    std::printf("[test] 格式命令 / 历史\n");

    RichTextEditorController c;
    c.setPlainText("hello world");
    c.setSelection(RichPos{0, 6}, RichPos{0, 11});
    c.toggleFlag(RichAttr::Bold);
    check(c.document().line(0).runs.size() == 2, "中间加粗 ⇒ 两个 run");
    check(c.document().line(0).runs[1].text == "world", "后半段是 world");

    c.setSelection(RichPos{0, 0}, RichPos{0, 11});
    c.setColor(Color::fromRGBA(0.4f, 0.4f, 0.4f, 1.0f));
    check(c.document().line(0).runs.size() == 2, "整段上色：粗体段与普通段仍分开");
    check(c.document().line(0).runs[0].fmt.color.a >= 0, "未加粗段有颜色");
    check(c.document().line(0).runs[1].fmt.bold &&
              c.document().line(0).runs[1].fmt.color.a >= 0,
          "加粗段同时获得颜色");

    // clean 只清行内格式
    c.setSelection(RichPos{0, 0}, RichPos{0, 11});
    c.clean();
    check(c.document().line(0).runs.size() == 1, "clean 后合成一个纯净 run");
    check(c.document().line(0).runs[0].fmt.isClean(), "行内格式清空");

    // 块格式在 clean 后保留
    c.selectAll();
    c.setHeader(3);
    c.toggleList(RichListKind::Ordered);
    c.selectAll();
    c.clean();
    check(c.document().line(0).block == RichBlock::Normal, "进列表时标题被顶掉");
    check(c.document().line(0).list == RichListKind::Ordered, "clean 保留块格式");

    // changeIndent 夹在 [0, kMaxIndent]
    for (int i = 0; i < 9; ++i)
        c.changeIndent(1);
    check(c.indentAt() == RichTextEditorController::kMaxIndent, "缩进上限 5");
    for (int i = 0; i < 9; ++i)
        c.changeIndent(-1);
    check(c.indentAt() == 0, "缩进下限 0");

    // toggleCodeBlock 进去时清行内格式、整段进整出
    c.setPlainText("fmt me");
    c.selectAll();
    RichInline b;
    b.bold = true;
    c.applyFormat(RichAttr::Bold, b);
    c.toggleCodeBlock();
    check(c.document().line(0).block == RichBlock::CodeBlock, "进代码块");
    check(c.document().line(0).runs[0].fmt.isClean(), "进代码块清行内格式");
    c.selectAll();
    c.toggleCodeBlock();
    check(c.document().line(0).block == RichBlock::Normal, "再切一次整段出代码块");

    // 引用与列表互斥
    c.setPlainText("q");
    c.selectAll();
    c.toggleList(RichListKind::Bullet);
    c.toggleBlockquote();
    check(c.document().line(0).block == RichBlock::Blockquote, "设为引用");
    check(c.document().line(0).list == RichListKind::None, "引用清掉列表");
    c.toggleBlockquote();
    check(c.document().line(0).block == RichBlock::Normal, "再切回正文");

    // 链接 / 字号 / 上下标 / 字体的 Delta 拼写
    c.setPlainText("visit");
    c.selectAll();
    c.setLink("https://quilljs.com");
    check(c.document().line(0).runs[0].fmt.link == "https://quilljs.com", "setLink 写入选区");
    check(c.deltaJson().find("\"link\":\"https://quilljs.com\"") != std::string::npos,
          "Delta 里有 link 属性");
    c.selectAll();
    c.setSize(RichSize::Large);
    check(c.deltaJson().find("\"size\":\"large\"") != std::string::npos, "size 用白名单拼写");
    c.selectAll();
    c.setScript(RichScript::Sub);
    check(c.deltaJson().find("\"script\":\"sub\"") != std::string::npos, "script 用白名单拼写");
    c.selectAll();
    c.setFont("serif");
    check(c.deltaJson().find("\"font\":\"serif\"") != std::string::npos, "font 三档之一");
    c.selectAll();
    c.clean();
    check(c.document().line(0).runs[0].fmt.isClean(), "clean 把 link/size/script/font 一起清");

    // 历史：每步可撤销
    RichTextEditorController h;
    h.setPlainText("a");
    h.setCaret(h.document().docEnd());
    const std::string docA = h.plainText();
    h.insertText("b");
    h.insertText("c");
    check(h.undoDepth() == 2, "两次插入 ⇒ 两级历史");
    h.undo();
    check(h.plainText() == "ab\n", "撤销回到 ab");
    check(h.redoDepth() == 1 && h.undoDepth() == 1, "撤销进 redo 栈");
    h.redo();
    check(h.plainText() == "abc\n", "重做回到 abc");
    h.insertText("d");
    check(h.redoDepth() == 0, "新操作清空 redo");
    while (h.canUndo())
        h.undo();
    check(h.plainText() == docA, "一路撤销到底 = 初始文档");
    check(!h.canUndo() && h.canRedo(), "撤销到底后 redo 可回");

    for (int i = 0; i < 300; ++i)
        h.insertText("x");
    check(h.undoDepth() == RichTextEditorController::kHistoryLimit, "历史上限 200");

    // 撤销同时恢复选区
    h.setPlainText("keep");
    h.insertText("!");
    const RichPos after = h.caret();
    h.undo();
    check(h.caret() != after, "撤销把光标也回滚");
    check(h.plainText() == "keep\n", "撤销恢复文档");

    // 监听句柄
    int n = 0;
    int id = h.addListener([&n] { ++n; });
    h.insertText("y");
    check(n == 1, "监听收到一次通知");
    h.removeListener(id);
    h.insertText("z");
    check(n == 1, "句柄摘除后不再通知");
    check(h.listenerCount() == 0, "监听计数归零");
}

// ── 7. 渲染与交互 ───────────────────────────────────────────────────────────
void testRenderAndEditing() {
    std::printf("[test] 渲染几何 / 键盘 / 鼠标 / 工具栏\n");

    RichDocument doc = RichDocument::fromPlainText("hello\nsecond line");
    int changed = 0, selChanged = 0;
    auto editor = cui::make_shared<RichTextEditor>(
        doc, "Start writing…", 420.0, std::nullopt, false, std::nullopt,
        [&](const RichDocument&) { ++changed; }, [&](const RichRange&) { ++selChanged; });

    Harness h;
    h.attach(editor, 640, 480);
    auto* st = h.findState<RichTextEditor, RichTextEditorState>();
    check(st != nullptr, "拿到 RichTextEditorState");
    if (!st)
        return;
    auto ctl = st->controller();
    check(ctl == editor->controller(), "State 复用 widget 的控制器");
    check(ctl->listenerCount() == 1, "挂载时控制器只有一个监听");
    check(ctl->plainText() == "hello\nsecond line\n", "State 采纳 widget 的文档");
    check(st->lineGeoms().size() == 2, "两行 ⇒ 两份行几何");
    check(h.findParagraphExact("hello") != nullptr, "第一行按整段文本上屏");
    check(h.findParagraphExact("second line") != nullptr, "第二行按整段文本上屏");
    check(st->lineGeoms()[1].y > st->lineGeoms()[0].y, "第二行在第一行下方");
    check(st->lineGeoms()[0].textH > 0.0, "文本高度非零（字体测量生效）");
    check(st->lineGeoms()[1].textW > st->lineGeoms()[0].textW, "长行更宽");
    check(st->lineGeoms()[0].textX > 0.0, "正文左边有内缩");
    check(h.findParagraphExact("Start writing…") == nullptr, "有内容 ⇒ 不画占位符");
    check(h.primary() == nullptr, "挂载不抢焦点");

    // 点击第一行最左 ⇒ 上焦 + 光标在 {0,0}
    RenderParagraph* p0 = h.findParagraphExact("hello");
    RenderParagraph* p1 = h.findParagraphExact("second line");
    if (!p0 || !p1) {
        check(false, "定位两行的段落（失败则后续交互用例跳过）");
        return;
    }
    Offset left = h.paraGlobalLeft(p0);
    h.tap(left.dx + 1.0, left.dy + p0->size().height / 2.0);
    check(h.primary() == editor->focusNode().get(), "点正文让编辑器上焦");
    check(ctl->caret() == RichPos{0, 0}, "点在最左 ⇒ 列 0");
    check(st->caretVisible(), "获焦后光标可见");

    h.tap(left.dx + p0->size().width + 3.0, left.dy + p0->size().height / 2.0);
    check(ctl->caret() == RichPos{0, 5}, "点在最右 ⇒ 行尾");

    Offset s = h.paraGlobalLeft(p1);
    h.tap(s.dx + 1.0, s.dy + p1->size().height / 2.0);
    check(ctl->caret() == RichPos{1, 0}, "点第二行最左 ⇒ {1,0}");
    check(ctl->base() == ctl->caret(), "单击是折叠光标");

    // 键盘：逐字符键入 + 退格 + 回车 + 左右上下
    ctl->setCaret(RichPos{0, 5});
    h.pump();
    check(h.type("XY"), "键入的字符被编辑器消费");
    check(ctl->plainText() == "helloXY\nsecond line\n", "键入进文档");
    check(ctl->caret() == RichPos{0, 7}, "键入后光标前进");
    check(h.findParagraphExact("helloXY") != nullptr, "上屏文本随输入更新");
    check(changed >= 2, "onChanged 逐次回调");
    check(h.key(KeyCode::Backspace), "退格键被消费");
    check(ctl->plainText() == "helloX\nsecond line\n", "退格删掉一个字符");
    check(h.key(KeyCode::Return), "回车被消费");
    check(ctl->document().lineCount() == 3, "回车拆行");
    check(ctl->caret() == RichPos{1, 0}, "回车后光标在新行首");
    check(st->lineGeoms().size() == 3, "行几何跟着变成三行");
    check(h.key(KeyCode::Left), "左键被消费");
    check(ctl->caret() == RichPos{0, 6}, "跨行进上一行行尾");
    check(h.key(KeyCode::Down), "下键被消费");
    check(ctl->caret().line == 1, "下行回到中间那行");
    check(h.key(KeyCode::Up), "上键被消费");
    check(ctl->caret().line == 0, "上行回到首行");
    check(h.key(KeyCode::Down), "再下行被消费");
    check(ctl->caret().line == 1, "回到空的那行准备键入");

    // Ctrl+B（折叠 ⇒ pending）+ 键入，然后 Ctrl+A 全选、Ctrl+Z 撤销
    check(h.key(KeyCode::None, "b", ModCtrl), "Ctrl+B 被消费");
    check(ctl->pending().bold, "Ctrl+B 落在 pending");
    check(h.type("Z"), "键入 Z");
    check(ctl->document().line(1).runs.size() == 1, "空行接住一个字符");
    check(ctl->document().line(1).runs[0].text == "Z", "键入的是 Z");
    check(ctl->document().line(1).runs[0].fmt.bold, "Ctrl+B 后的字符是粗体");
    check(h.key(KeyCode::None, "a", ModCtrl), "Ctrl+A 被消费");
    check(ctl->selection() ==
              RichRange{ctl->document().docStart(), ctl->document().docEnd()},
          "Ctrl+A = 整篇");
    check(selChanged >= 1, "onSelectionChanged 有回调");
    check(h.key(KeyCode::None, "z", ModCtrl), "Ctrl+Z 被消费");
    check(ctl->document().line(1).runs.empty(), "撤销掉加粗的 Z");

    // Alt+Enter 引用 / Ctrl+Enter 代码块（都作用在光标所在行）
    ctl->setCaret(RichPos{0, 0});
    h.pump();
    check(h.key(KeyCode::Return, std::string(), ModAlt), "Alt+Enter 被消费");
    check(ctl->document().line(0).block == RichBlock::Blockquote, "Alt+Enter 切引用");
    check(h.key(KeyCode::Return, std::string(), ModCtrl), "Ctrl+Enter 被消费");
    check(ctl->document().line(0).block == RichBlock::CodeBlock, "Ctrl+Enter 换代码块");
    check(h.key(KeyCode::Return, std::string(), ModCtrl), "再按一次退出代码块");
    check(ctl->document().line(0).block == RichBlock::Normal, "代码块整段进出");
    check(h.key(KeyCode::Tab, std::string(), ModCtrl) == false, "Ctrl+Tab 交回焦点遍历");

    // Tab 在列表行改缩进
    ctl->setCaret(RichPos{0, 0});
    ctl->toggleList(RichListKind::Bullet);
    h.pump();
    check(h.key(KeyCode::Tab), "Tab（列表行）被消费");
    check(ctl->indentAt() == 1, "Tab 在列表行加缩进");
    check(h.key(KeyCode::Tab, std::string(), ModShift), "Shift+Tab 被消费");
    check(ctl->indentAt() == 0, "Shift+Tab 减缩进");
    ctl->toggleList(RichListKind::Bullet);
    h.pump();
    check(ctl->document().line(0).list == RichListKind::None, "再点一次退出列表（给后面的 chip 用例留空）");
    check(h.key(KeyCode::Escape), "Esc 被消费");

    // 工具栏 chip：程序化选区后点 B，再点 clean
    ctl->setSelection(RichPos{0, 0}, RichPos{0, 5});
    h.pump();
    RenderParagraph* chipB = h.findParagraphExact("B");
    check(chipB != nullptr, "工具栏有 B 按钮");
    if (chipB) {
        Offset cc = h.paraCenter(chipB);
        h.tap(cc.dx, cc.dy);
        check(ctl->document().line(0).runs[0].fmt.bold, "点 B 让选区加粗");
        check(ctl->formatAtSelection().bold, "选区格式回读为粗体");
    }
    RenderParagraph* chipClean = h.findParagraphExact("clean");
    if (chipClean) {
        Offset cc = h.paraCenter(chipClean);
        h.tap(cc.dx, cc.dy);
        check(ctl->document().line(0).runs[0].fmt.isClean(), "点 clean 清行内格式");
    }
    RenderParagraph* chipList = h.findParagraphExact("•");
    if (chipList) {
        Offset cc = h.paraCenter(chipList);
        h.tap(cc.dx, cc.dy);
        check(ctl->document().line(0).list == RichListKind::Bullet, "点项目符号进列表");
        check(st->lineGeoms().size() == ctl->document().lineCount(), "行几何数与行数一致");
    }

    // 拖动扩展选区（Quill 的 mousedown + mousemove）
    RenderParagraph* pd = h.findParagraphExact("second line");
    Offset a = h.paraGlobalLeft(pd);
    h.down(a.dx + 1.0, a.dy + pd->size().height / 2.0);
    h.movePointer(a.dx + pd->size().width - 2.0, a.dy + pd->size().height / 2.0);
    h.up(a.dx + pd->size().width - 2.0, a.dy + pd->size().height / 2.0);
    check(ctl->selection().start.line == 2 && ctl->selection().end.line == 2,
          "拖选落在同一行");
    check(ctl->hasSelection(), "拖选出选区");
    check(ctl->document().textInRange(ctl->selection()).size() > 5, "选区覆盖多个字符");

    // 长行：内容比可见宽度宽 ⇒ 光标在末尾时被横向滚动带进可视区（要聚焦才滚）
    const std::string longText(160, 'x');
    ctl->setPlainText(longText);
    h.pump();
    check(st->lineGeoms().size() == 1, "长行仍是一行几何");
    check(st->lineGeoms()[0].textW > 600.0, "长行的内容宽远超可见宽度");
    RenderParagraph* pl = h.findParagraphExact(longText);
    check(pl != nullptr, "长行上屏");
    if (pl) {
        const double before = h.paraGlobalLeft(pl).dx;
        ctl->setCaret(RichPos{0, longText.size()});
        h.pump();
        h.pump();
        RenderParagraph* pl2 = h.findParagraphExact(longText);
        const double after = pl2 ? h.paraGlobalLeft(pl2).dx : before;
        check(after < before - 100.0, "帧末回调把光标滚进可视区（内容左移）");
    }

    // 失焦 ⇒ 光标不画（Quill 未聚焦没有光标），选区改用灰色
    editor->focusNode()->unfocus();
    h.pump();
    check(!st->caretVisible(), "失焦后光标隐藏");
}

// ── 8. span 切分 ────────────────────────────────────────────────────────────
// `RenderParagraph` 的每个叶子只认自己那份 TextStyle，所以 run 边界必须全部
// 切开；漏切会把「第一个 run 之后的所有格式」压成同一种（粗体一路染到行尾）。
void testSpanCuts() {
    std::printf("[test] run / 选区切分出的 span 叶子\n");

    RichInline b, i;
    b.bold = true;
    i.italic = true;
    RichLine ln;
    ln.runs = {RichRun{"ab", RichInline{}}, RichRun{"cd", b}, RichRun{"ef", i}};
    RichDocument doc;
    doc.lines = {ln, RichLine{}};

    auto editor = cui::make_shared<RichTextEditor>(doc);
    Harness h;
    h.attach(editor, 640, 480);
    auto* st = h.findState<RichTextEditor, RichTextEditorState>();
    check(st != nullptr, "拿到 State");
    if (!st)
        return;

    RenderParagraph* p = h.findParagraphExact("abcdef");
    check(p != nullptr, "三个 run 拼成一行上屏");
    if (!p)
        return;
    const std::vector<SharedPtr<TextSpan>>& k1 = p->text()->children;
    check(k1.size() == 3, "无选区 ⇒ 每个 run 一个叶子");
    if (k1.size() == 3) {
        check(k1[0]->text == "ab" && k1[0]->style.fontWeight < 700.0 && !k1[0]->style.italic,
              "第一段干净");
        check(k1[1]->text == "cd" && k1[1]->style.fontWeight >= 700.0, "第二段带粗体");
        check(k1[2]->text == "ef" && k1[2]->style.italic, "第三段带斜体");
    }

    // 选区 [1,5) 横跨三个 run ⇒ 叶子 = run 边界 ∪ 选区边界 = 5 段
    st->controller()->setSelection(RichPos{0, 1}, RichPos{0, 5});
    h.pump();
    RenderParagraph* p2 = h.findParagraphExact("abcdef");
    if (!p2) {
        check(false, "选区后仍能定位到该行");
        return;
    }
    const std::vector<SharedPtr<TextSpan>>& k2 = p2->text()->children;
    check(k2.size() == 5, "选区边界把 run 再切一刀");
    if (k2.size() == 5) {
        check(k2[0]->text == "a" && k2[0]->style.backgroundColor.a == 0.0, "选区外不上底色");
        check(k2[1]->text == "b" && k2[1]->style.backgroundColor.a > 0.0, "选区内上底色");
        check(k2[2]->text == "cd" && k2[2]->style.fontWeight >= 700.0 &&
                      k2[2]->style.backgroundColor.a > 0.0,
              "整段落在选区里：既保持粗体又加底色");
        check(k2[3]->text == "e" && k2[3]->style.italic &&
                      k2[3]->style.backgroundColor.a > 0.0,
              "斜体 + 底色共存，不互相顶掉");
        check(k2[4]->text == "f" && k2[4]->style.italic &&
                      k2[4]->style.backgroundColor.a == 0.0,
              "选区之后的 run 没有被连坐");
    }
}

// ── 9. 卸载 ─────────────────────────────────────────────────────────────────
void testTeardown() {
    std::printf("[test] 卸载摘干净监听\n");

    auto editor = cui::make_shared<RichTextEditor>(RichDocument::fromPlainText("bye"));
    auto probe = cui::make_shared<SwapProbe>(editor);
    Harness h;
    h.attach(probe, 640, 480);
    SharedPtr<RichTextEditorController> ctl = editor->controller();
    SharedPtr<FocusNode> node = editor->focusNode();
    check(ctl->listenerCount() == 1, "挂载时监听 1 条");
    node->requestFocus();
    h.pump();
    check(h.primary() == node.get(), "卸载前编辑器可上焦");

    auto* ps = h.findState<SwapProbe, SwapProbeState>();
    check(ps != nullptr, "拿到探针 State");
    if (!ps)
        return;
    ps->setState([ps] { ps->hidden = true; });
    h.pump();
    check(ctl->listenerCount() == 0, "卸载后控制器监听摘干净");
    check(h.primary() == nullptr, "卸载后焦点节点不再是 primaryFocus");
}

}  // namespace

int main() {
    std::printf("=== test_rich_text_editor ===\n");
    testDocumentModel();
    testInsertAndPending();
    testLineBreak();
    testDeleteBackward();
    testSelectionMoves();
    testFormatCommandsAndHistory();
    testRenderAndEditing();
    testSpanCuts();
    testTeardown();
    std::printf(g_failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
