#pragma once
/// @file foundation/text_style.h
/// @brief TextStyle — font/colour/decoration descriptor for rich text spans.
///
/// TextStyle is the per-span styling applied by RichText.  It carries font
/// metrics, colour, weight, and decoration.  Combined with RichText's
/// TextSpan, it enables multi-styled text rendering without SkParagraph.
///
/// Usage:
///   TextStyle bold24{.fontSize = 24, .fontWeight = 700, .color = colors::blue()};
///   TextStyle italic16{.fontSize = 16, .italic = true};

#include <memory>
#include <string>
#include <vector>

#include "../utils/shared_ptr.h"
#include "cui/foundation/color.h"

namespace cui {

struct TextStyle {
    double fontSize = 14.0;
    double fontWeight = 400.0;   // 100..900
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    std::string fontFamily;
    /// 颜色默认「未指定」（a = -1 哨兵）。mergeFrom 仅在显式设置（a >= 0）
    /// 时覆盖，从而避免默认黑色把继承来的 DefaultTextStyle 颜色顶掉。
    /// 绘制时若 a < 0 归一为不透明黑。
    Color color = Color::fromRGBA(0, 0, 0, -1);
    Color backgroundColor = Color::fromRGBA(0, 0, 0, 0);  // transparent

    /// Fluent builders.
    TextStyle& size(double s)            { fontSize = s; return *this; }
    TextStyle& weight(double w)          { fontWeight = w; return *this; }
    TextStyle& bold()                    { fontWeight = 700; return *this; }
    TextStyle& setItalic(bool v = true)  { italic = v; return *this; }
    TextStyle& setUnderline(bool v = true) { underline = v; return *this; }
    TextStyle& setStrikethrough(bool v = true) { strikethrough = v; return *this; }
    TextStyle& family(const char* f)     { fontFamily = f ? f : ""; return *this; }
    TextStyle& fgColor(const Color& c)   { color = c; return *this; }
    TextStyle& bgColor(const Color& c)   { backgroundColor = c; return *this; }

    /// Merge another style on top (non-default values override).
    TextStyle& mergeFrom(const TextStyle& other) {
        if (other.fontSize > 0) fontSize = other.fontSize;
        if (other.fontWeight > 0) fontWeight = other.fontWeight;
        if (other.italic) italic = other.italic;
        if (other.underline) underline = other.underline;
        if (other.strikethrough) strikethrough = other.strikethrough;
        if (!other.fontFamily.empty()) fontFamily = other.fontFamily;
        if (other.color.a >= 0) color = other.color;
        if (other.backgroundColor.a > 0) backgroundColor = other.backgroundColor;
        return *this;
    }

    /// Equality (used by InheritedWidget::updateShouldNotify for DefaultTextStyle).
    bool operator==(const TextStyle& o) const {
        return fontSize == o.fontSize && fontWeight == o.fontWeight && italic == o.italic &&
               underline == o.underline && strikethrough == o.strikethrough &&
               fontFamily == o.fontFamily && color == o.color && backgroundColor == o.backgroundColor;
    }
    bool operator!=(const TextStyle& o) const { return !(*this == o); }

    /// 全字段「未指定」：mergeFrom 不会覆盖继承值，RenderParagraph 归一为
    /// 14 / 400 / 黑。`Text` 的无参 style 即此，从而完整继承 DefaultTextStyle。
    static TextStyle unset() {
        TextStyle s;
        s.fontSize = 0;
        s.fontWeight = 0;
        s.color = Color::fromRGBA(0, 0, 0, -1);
        return s;
    }
};

/// 富文本片段树。上游的 `TextSpan`（text.dart）。
///
/// 一个 `TextSpan` 要么携带一段 `text`（叶节点），要么携带若干 `children`
/// （内部节点，自身 text 通常为空）。`RichText` 把根 `TextSpan` 交给
/// `RenderParagraph` 布局 / 绘制——本移植为单行、不换行。
class TextSpan {
public:
    std::string text;
    TextStyle style;
    std::vector<SharedPtr<TextSpan>> children;

    TextSpan() = default;
    explicit TextSpan(std::string text_, TextStyle style_ = TextStyle{})
        : text(std::move(text_)), style(style_) {}
    static SharedPtr<TextSpan> make(std::string text_, TextStyle style_ = TextStyle{}) {
        return cui::make_shared<TextSpan>(std::move(text_), style_);
    }

    /// 拼接所有叶节点的纯文本（调试 / 命中测试用）。
    std::string toPlainText() const {
        std::string out = text;
        for (const auto& c : children)
            if (c) out += c->toPlainText();
        return out;
    }
};

}  // namespace cui_core
