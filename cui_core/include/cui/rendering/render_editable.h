#pragma once
/// @file render_editable.h
/// @brief RenderEditable — 单行可编辑文本的渲染节点（上游的 RenderEditable）。
///
/// 负责：文本 / 占位符 / 选区高亮 / 光标的测量与绘制。字符级光标定位
/// （getPositionForX / offsetForIndex）供 EditableText 的点击落点使用。
///
/// 设计：RenderEditable 只持有「展示所需」的纯数据（text/selection/hasFocus/
/// style…），由 EditableText 的叶子 widget 每帧推送；不反向依赖
/// TextEditingController，从而避免 rendering↔widgets 循环链接。

#include <cstddef>
#include <string>

#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/enums.h"  // TextAlign
#include "cui/rendering/render_box.h"
#include "cui/foundation/text_style.h"

namespace cui {

class RenderEditable : public RenderBox {
public:
    RenderEditable(TextStyle style, TextAlign align,
                   std::string placeholder, bool obscure)
        : style_(std::move(style)),
          align_(align),
          placeholder_(std::move(placeholder)),
          obscure_(obscure) {
        measureFont();
    }

    void setText(std::string t) {
        if (t == text_) return;
        text_ = std::move(t);
        markNeedsLayout();
    }
    const std::string& text() const { return text_; }
    bool hasFocus() const { return hasFocus_; }

    void setSelection(std::size_t base, std::size_t extent) {
        if (base == selBase_ && extent == selExtent_)
            return;
        selBase_ = base;
        selExtent_ = extent;
        markNeedsPaint();
    }
    void setHasFocus(bool f) {
        if (f == hasFocus_)
            return;
        hasFocus_ = f;
        markNeedsPaint();
    }
    /// 上游 的 setter 全部「等值即早退」；这里若不等值，每次重建都会
    /// measureFont()（真实字体测量）+ 标脏整棵子树。
    void setStyle(TextStyle s) {
        if (style_ == s)
            return;
        style_ = std::move(s);
        measureFont();
        markNeedsLayout();
    }
    void setTextAlign(TextAlign a) {
        if (a == align_)
            return;
        align_ = a;
        markNeedsPaint();
    }
    void setPlaceholder(std::string p) {
        if (p == placeholder_)
            return;
        placeholder_ = std::move(p);
        markNeedsPaint();
    }
    /// 密文切换会改变测量宽度（每码点一个圆点）⇒ 需要重排。
    void setObscure(bool o) {
        if (o == obscure_)
            return;
        obscure_ = o;
        markNeedsLayout();
    }

    /// 局部 x（相对本节点原点）→ 字符字节索引（点击定位光标用）。
    std::size_t getPositionForX(double localX) const;
    /// 字符字节索引 → 局部 x 偏移。
    double offsetForIndex(std::size_t index) const;

    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;
    double getDistanceToBaseline(BaselineType type) const override;
    /// 命中测试必须做边界检查：基类 RenderObject::hitTest 不裁剪坐标，全靠
    /// hitTestSelf 自查。此前无条件返回 true，导致文本框会"接住"整屏任意
    /// 位置的点击（页面区域逆序先于侧栏被测试，TextField 无条件命中后侧栏
    /// 不再参与命中）——表现为聚焦文本框后点击其它 tab / 控件全部失效。
    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 &&
               position.dx < size_.width && position.dy < size_.height;
    }

private:
    void measureFont();
    double charX(std::size_t byteIndex) const;
    double paintFontSize() const { return style_.fontSize > 0 ? style_.fontSize : 14.0; }
    double paintFontWeight() const { return style_.fontWeight > 0 ? style_.fontWeight : 400.0; }
    const char* paintFamily() const { return style_.fontFamily.empty() ? nullptr : style_.fontFamily.c_str(); }
    std::string displayText() const;

    std::string text_;
    std::size_t selBase_ = 0;
    std::size_t selExtent_ = 0;
    bool hasFocus_ = false;
    TextStyle style_;
    TextAlign align_ = TextAlign::Left;
    std::string placeholder_;
    bool obscure_ = false;

    double ascent_ = 0;
    double descent_ = 0;
    double contentWidth_ = 0;
};

}  // namespace cui_core
