/// @file render_editable.cpp — 单行可编辑文本渲染节点实现。

#include "cui/rendering/render_editable.h"

#include <algorithm>
#include <cstring>

#include "cui/foundation/painter.h"
#include "cui/foundation/theme.h"
#include "cui/rendering/painting_context.h"

namespace cui {

namespace {

/// UTF-8 首字节 → 该码点字节数。
inline std::size_t utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;  // 非法首字节，按单字节处理避免死循环
}

}  // namespace

void RenderEditable::measureFont() {
    const char* fam = paintFamily();
    double fs = paintFontSize();
    double fw = paintFontWeight();
    Painter::fontBand(fam, fs, fw, style_.italic, ascent_, descent_);
    // fontBand 返回 ascent(正, 顶→基线) 与 descent(正, 基线→底)。
}

std::string RenderEditable::displayText() const {
    if (!obscure_)
        return text_;
    // 密码框：每个码点画一个圆点（U+2022）。
    std::string out;
    std::size_t i = 0;
    while (i < text_.size()) {
        out += "\xe2\x80\xa2";  // •
        i += utf8Len((unsigned char)text_[i]);
    }
    return out;
}

double RenderEditable::charX(std::size_t byteIndex) const {
    const std::string& t = text_;
    const char* fam = paintFamily();
    double fs = paintFontSize();
    double fw = paintFontWeight();
    bool italic = style_.italic;
    std::size_t i = 0;
    double x = 0.0;
    while (i < byteIndex && i < t.size()) {
        std::size_t len = utf8Len((unsigned char)t[i]);
        std::string cp = t.substr(i, len);
        double w = obscure_
                       ? Painter::measureTextWidth(fam, "\xe2\x80\xa2", fs, fw, italic)
                       : Painter::measureTextWidth(fam, cp, fs, fw, italic);
        x += w;
        i += len;
    }
    return x;
}

double RenderEditable::offsetForIndex(std::size_t index) const {
    return charX(index);
}

std::size_t RenderEditable::getPositionForX(double localX) const {
    const std::string& t = text_;
    const char* fam = paintFamily();
    double fs = paintFontSize();
    double fw = paintFontWeight();
    bool italic = style_.italic;
    double x = 0.0;
    std::size_t i = 0;
    while (i < t.size()) {
        std::size_t len = utf8Len((unsigned char)t[i]);
        std::string cp = t.substr(i, len);
        double w = obscure_
                       ? Painter::measureTextWidth(fam, "\xe2\x80\xa2", fs, fw, italic)
                       : Painter::measureTextWidth(fam, cp, fs, fw, italic);
        if (localX <= x + w * 0.5)
            return i;
        x += w;
        i += len;
    }
    return t.size();
}

void RenderEditable::performLayout() {
    BoxConstraints c = constraints();
    const std::string disp = displayText();
    contentWidth_ = disp.empty()
                         ? 0.0
                         : Painter::measureTextWidth(paintFamily(), disp, paintFontSize(),
                                                     paintFontWeight(), style_.italic);
    // 单行输入框语义（对齐上游 RenderEditable 的
    // TextPainter.layout(minWidth, maxWidth)）：在有界宽度约束下占满可用宽度；
    // 只有无界宽度（intrinsic 测量）时才按文本自然宽度收缩。
    // 否则空文本 contentWidth_=0 会把输入框坍缩成 0 宽——hintText 溢出到框外、
    // 边框挤成细高竖条、点击命中区域也随之退化（文本输入页回归）。
    double width = c.hasBoundedWidth() ? c.maxWidth : contentWidth_;
    width = std::max(width, c.minWidth);

    double lineH = ascent_ + descent_;
    double height = lineH;
    if (c.hasBoundedHeight())
        height = std::min(height, c.maxHeight);
    height = std::max(height, c.minHeight);

    setSize(Size(width, height));
}

double RenderEditable::getDistanceToBaseline(BaselineType) const {
    return ascent_;
}

void RenderEditable::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* canvas = context.canvas();
    Painter p(canvas, paintFamily());
    p.setFontFamily(paintFamily());
    p.setFontSize(paintFontSize());
    p.setFontWeight(paintFontWeight());
    p.setFontItalic(style_.italic);

    const double fs = paintFontSize();
    const double lineH = ascent_ + descent_;
    const double baseY = offset.dy + ascent_;  // 局部坐标基线 y

    // ── 选区高亮 ──
    if (selBase_ != selExtent_) {
        std::size_t a = std::min(selBase_, selExtent_);
        std::size_t b = std::max(selBase_, selExtent_);
        double x0 = offset.dx + charX(a);
        double x1 = offset.dx + charX(b);
        p.setSourceColor(Color(theme().primary.r, theme().primary.g, theme().primary.b, 0.35));
        p.rectangle(Rect{x0, offset.dy, x1 - x0, lineH});
        p.fill();
    }

    const std::string disp = displayText();
    if (disp.empty()) {
        // 占位符（仅未聚焦时显示，聚焦后空文本不画占位符）。
        if (!hasFocus_ && !placeholder_.empty()) {
            Color pc = style_.color.a >= 0 ? style_.color : theme().textSecondary;
            p.setSourceColor(pc);
            canvas->save();
            canvas->translate(SkScalar(offset.dx), SkScalar(baseY - fs));
            p.showText(placeholder_.c_str());
            canvas->restore();
        }
    } else {
        Color tc = style_.color.a >= 0 ? style_.color : theme().textPrimary;
        p.setSourceColor(tc);
        canvas->save();
        canvas->translate(SkScalar(offset.dx), SkScalar(baseY - fs));
        p.showText(disp.c_str());
        canvas->restore();
    }

    // ── 光标 ──
    if (hasFocus_ && selBase_ == selExtent_) {
        double cx = offset.dx + charX(selExtent_);
        p.setSourceColor(theme().primary);
        p.setLineWidth(1.0);
        p.moveTo(cx, offset.dy);
        p.lineTo(cx, offset.dy + lineH);
        p.stroke();
    }
}

}  // namespace cui_core
