#pragma once
/// @file rendering/render_material.h
/// @brief Material 装饰/进度渲染对象。
///
/// 对应 上游源码（painting / rendering）：
///   - RenderLinearProgressIndicator（progress_indicator.dart）
///   - RenderCircularProgressIndicator（progress_indicator.dart）
///
/// 本协议只实现「静态外观」：value 给定时按百分比绘制填充，value 为空
/// （indeterminate）时画满整条/整圈。动画（上游的 AnimationController +
/// CurvedAnimation）留待动画系统接入后补充，因此本移植与 上游的
/// 无 value 形态一致地表现为「填满」。

#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/rendering/render_box.h"

namespace cui {

/// 线性进度条。上游的 `RenderLinearProgressIndicator` 子集。
///
/// 布局：横向撑满父约束宽度，高度取 minHeight（除非被父约束强制更高）。
class RenderLinearProgressIndicator : public RenderBox {
public:
    RenderLinearProgressIndicator(
        double value = -1.0, double minHeight = 4.0,
        Color background = Color::fromRGBA(0, 0, 0, 0.12),
        Color valueColor = Color::fromRGB(0, 0, 0))
        : value_(value), minHeight_(minHeight), backgroundColor_(background),
          valueColor_(valueColor) {}

    double value() const { return value_; }
    /// 上游 的 `RenderProgressIndicator.value`：等值早退，否则标脏。
    /// 这里原先是静默赋值，LinearProgressIndicator 的 setState 改值不会重绘。
    void setValue(double v) {
        if (v == value_)
            return;
        value_ = v;
        markNeedsPaint();
    }
    double minHeight() const { return minHeight_; }
    void setMinHeight(double h) {
        if (h == minHeight_)
            return;
        minHeight_ = h;
        markNeedsLayout();
    }
    Color backgroundColor() const { return backgroundColor_; }
    void setBackgroundColor(Color c) {
        if (c == backgroundColor_)
            return;
        backgroundColor_ = c;
        markNeedsPaint();
    }
    Color valueColor() const { return valueColor_; }
    void setValueColor(Color c) {
        if (c == valueColor_)
            return;
        valueColor_ = c;
        markNeedsPaint();
    }

    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    double value_;  ///< < 0 表示 indeterminate（无 value）。
    double minHeight_;
    Color backgroundColor_;
    Color valueColor_;
};

/// 环形进度条。上游的 `RenderCircularProgressIndicator` 子集。
///
/// 布局：无约束时取直径 36（上游的 _kMinCircularProgressIndicatorSize）；
/// 有约束时取 min(maxWidth, maxHeight)。
class RenderCircularProgressIndicator : public RenderBox {
public:
    RenderCircularProgressIndicator(
        double value = -1.0, double strokeWidth = 4.0,
        Color background = Color::fromRGBA(0, 0, 0, 0.12),
        Color valueColor = Color::fromRGB(0, 0, 0))
        : value_(value), strokeWidth_(strokeWidth), backgroundColor_(background),
          valueColor_(valueColor) {}

    double value() const { return value_; }
    void setValue(double v) {
        if (v == value_)
            return;
        value_ = v;
        markNeedsPaint();
    }
    double strokeWidth() const { return strokeWidth_; }
    /// 直径只由约束决定，故线宽变化只需重绘。
    void setStrokeWidth(double w) {
        if (w == strokeWidth_)
            return;
        strokeWidth_ = w;
        markNeedsPaint();
    }
    Color backgroundColor() const { return backgroundColor_; }
    void setBackgroundColor(Color c) {
        if (c == backgroundColor_)
            return;
        backgroundColor_ = c;
        markNeedsPaint();
    }
    Color valueColor() const { return valueColor_; }
    void setValueColor(Color c) {
        if (c == valueColor_)
            return;
        valueColor_ = c;
        markNeedsPaint();
    }

    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    double value_;  ///< < 0 表示 indeterminate（无 value）。
    double strokeWidth_;
    Color backgroundColor_;
    Color valueColor_;
};

}  // namespace cui_core
