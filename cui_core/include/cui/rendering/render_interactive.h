#pragma once
/// @file rendering/render_interactive.h
/// @brief 交互式 Material 控件的渲染对象。
///
/// 对应 上游源码：
///   - RenderCheckbox（widgets/checkbox.dart 的 _RenderCheckbox）
///   - RenderSwitch（widgets/switch.dart 的 _RenderSwitch）
///   - RenderRadio（widgets/radio.dart 的 _RenderRadio）
///   - RenderSlider（widgets/slider.dart 的 _RenderSlider）
///
/// 约定：
///   - 复选框/开关/单选均按固定逻辑尺寸布局，color 由 Widget 层传入；
///   - Slider 自带 handleEvent：指针 down/move 直接改写 value 并回调
///     onChanged，因此无需 GestureDetector 包裹（也避免与 tap 识别冲突）；
///   - 动画（勾选过渡、thumb 滑动缓动）本移植暂用静态外观近似，留待
///     AnimationController 接入。

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "cui/foundation/color.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_box.h"

namespace cui {

namespace detail {

/// 渲染对象内部的轻量过渡控制器（AnimationController 子集）：
/// foundation Ticker（真实时间）驱动 t 从当前值缓动到目标值，
/// 每步回调 onProgress（渲染对象里通常是 markNeedsPaint）。
/// headless 测试可用「sleep + Ticker::tickAllActive」确定性推进。
class RenderTransition {
public:
    using Notify = std::function<void()>;

    explicit RenderTransition(Notify onProgress) : onProgress_(std::move(onProgress)) {}
    ~RenderTransition() {
        if (ticker_)
            ticker_->stop();
    }

    void setDuration(double ms) { durationMs_ = std::max(1.0, ms); }
    double duration() const { return durationMs_; }

    /// 跳到指定值（不产生过渡）。
    void jump(double t) {
        stop();
        t_ = from_ = to_ = clamp01(t);
    }

    /// 缓动到目标值。目标与当前相同则无操作。
    void to(double target);

    double t() const { return t_; }
    double target() const { return to_; }
    bool animating() const { return ticker_ && ticker_->isActive(); }

private:
    static double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
    static double steadyNow() {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    /// EaseOutCubic（Material 标准减速曲线近似）。
    static double ease(double p) {
        double q = 1.0 - p;
        return 1.0 - q * q * q;
    }

    void stop() {
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
    }

    void step();

    Notify onProgress_;
    std::unique_ptr<Ticker> ticker_;
    double durationMs_ = 200.0;
    double t_ = 0.0, from_ = 0.0, to_ = 0.0;
    double startMs_ = 0.0;
};

}  // namespace detail

// ── RenderCheckbox ─────────────────────────────────────────────────────────
class RenderCheckbox : public RenderBox {
public:
    explicit RenderCheckbox(bool value, Color active, Color check, Color inactive,
                            double size)
        : value_(value), active_(active), check_(check), inactive_(inactive),
          size_(size),
          transition_([this] { markNeedsPaint(); }) {
        transition_.jump(value_ ? 1.0 : 0.0);
    }

    bool value() const { return value_; }
    void setValue(bool v);
    /// 当前过渡进度 t ∈ [0,1]（对勾/底色的动画插值）。
    double animT() const { return transition_.t(); }
    void setAnimDuration(double ms) { transition_.setDuration(ms); }
    Color activeColor() const { return active_; }
    void setActiveColor(Color c) {
        if (c != active_) {
            active_ = c;
            markNeedsPaint();
        }
    }
    Color checkColor() const { return check_; }
    void setCheckColor(Color c) {
        if (c != check_) {
            check_ = c;
            markNeedsPaint();
        }
    }
    Color inactiveColor() const { return inactive_; }
    void setInactiveColor(Color c) {
        if (c != inactive_) {
            inactive_ = c;
            markNeedsPaint();
        }
    }
    double boxSize() const { return size_; }
    void setSizePx(double s) {
        if (s != size_) {
            size_ = s;
            markNeedsLayout();
        }
    }

    double getDistanceToBaseline(BaselineType) const override { return size_; }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    bool value_;
    Color active_, check_, inactive_;
    double size_;
    detail::RenderTransition transition_;
};

// ── RenderSwitch ───────────────────────────────────────────────────────────
class RenderSwitch : public RenderBox {
public:
    explicit RenderSwitch(bool value, Color active, Color inactive, double width,
                          double height, double thumbRadius)
        : value_(value), active_(active), inactive_(inactive), width_(width),
          height_(height), thumbRadius_(thumbRadius),
          transition_([this] { markNeedsPaint(); }) {
        transition_.jump(value_ ? 1.0 : 0.0);
    }

    bool value() const { return value_; }
    void setValue(bool v);
    /// 当前过渡进度 t ∈ [0,1]（圆钮位置/轨道色的动画插值）。
    double animT() const { return transition_.t(); }
    void setAnimDuration(double ms) { transition_.setDuration(ms); }
    double width() const { return width_; }
    double height() const { return height_; }
    double thumbRadius() const { return thumbRadius_; }
    Color activeColor() const { return active_; }
    void setActiveColor(Color c) {
        if (c != active_) {
            active_ = c;
            markNeedsPaint();
        }
    }
    Color inactiveColor() const { return inactive_; }
    void setInactiveColor(Color c) {
        if (c != inactive_) {
            inactive_ = c;
            markNeedsPaint();
        }
    }

    double getDistanceToBaseline(BaselineType) const override { return height_; }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    bool value_;
    Color active_, inactive_;
    double width_, height_, thumbRadius_;
    detail::RenderTransition transition_;
};

// ── RenderRadio ────────────────────────────────────────────────────────────
class RenderRadio : public RenderBox {
public:
    explicit RenderRadio(bool selected, Color active, Color inactive, double size)
        : selected_(selected), active_(active), inactive_(inactive), size_(size) {}

    bool selected() const { return selected_; }
    void setSelected(bool v) {
        if (v != selected_) {
            selected_ = v;
            markNeedsPaint();
        }
    }
    Color activeColor() const { return active_; }
    void setActiveColor(Color c) {
        if (c != active_) {
            active_ = c;
            markNeedsPaint();
        }
    }
    Color inactiveColor() const { return inactive_; }
    void setInactiveColor(Color c) {
        if (c != inactive_) {
            inactive_ = c;
            markNeedsPaint();
        }
    }
    double boxSize() const { return size_; }
    void setSizePx(double s) {
        if (s != size_) {
            size_ = s;
            markNeedsLayout();
        }
    }

    double getDistanceToBaseline(BaselineType) const override { return size_; }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    bool selected_;
    Color active_, inactive_;
    double size_;
};

// ── RenderSlider ───────────────────────────────────────────────────────────
class RenderSlider : public RenderBox {
public:
    using ValueChanged = std::function<void(double)>;

    explicit RenderSlider(double value, double min, double max, Color active,
                          Color inactive, Color thumb, double height,
                          double thumbRadius)
        : value_(value), min_(min), max_(max), active_(active),
          inactive_(inactive), thumb_(thumb), height_(height),
          thumbRadius_(thumbRadius) {
        labelT_.setDuration(150.0);
    }

    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 &&
               position.dx < size_.width && position.dy < size_.height;
    }

    double value() const { return value_; }
    void setValue(double v);
    void setActiveColor(Color c) {
        if (c != active_) {
            active_ = c;
            markNeedsPaint();
        }
    }
    void setInactiveColor(Color c) {
        if (c != inactive_) {
            inactive_ = c;
            markNeedsPaint();
        }
    }
    void setThumbColor(Color c) {
        if (c != thumb_) {
            thumb_ = c;
            markNeedsPaint();
        }
    }
    /// 拖拽过程中每帧回调（由 Widget 层传入受控 value 的写回函数）。
    void setOnChanged(ValueChanged cb) { onChanged_ = std::move(cb); }

    // ── 标签气泡（上游 Slider.label：拖拽时显示在 thumb 上方）──────────
    void setLabel(std::string label) {
        if (label != label_) {
            label_ = std::move(label);
            markNeedsPaint();
        }
    }
    const std::string& label() const { return label_; }
    bool hasLabel() const { return !label_.empty(); }
    /// 气泡是否可见（拖拽中且 label 非空；带 150ms 淡入淡出）。
    bool labelVisible() const { return hasLabel() && labelT_.t() > 0.01; }
    double labelT() const { return labelT_.t(); }
    /// 气泡矩形（本地坐标）。宽按 12px 字体现测，底边距 thumb 顶 8px。
    Rect bubbleRect() const;

    /// 当前 thumb 中心 x（本地坐标）。按 value 现算——headless 测试不执行
    /// flushPaint，不能依赖 paint 期间写入的缓存。
    double thumbCenterX() const;

    double getDistanceToBaseline(BaselineType) const override { return height_; }

    void handleEvent(const PointerEvent& event, const HitTestEntry& entry) override;

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    void updateValueFromPosition(double localX);

    double value_, min_, max_;
    Color active_, inactive_, thumb_;
    double height_, thumbRadius_;
    double thumbX_ = 0.0;

    std::string label_;
    detail::RenderTransition labelT_{[this] { markNeedsPaint(); }};

    bool dragging_ = false;
    int64_t downId_ = -1;
    ValueChanged onChanged_;
};

}  // namespace cui_core
