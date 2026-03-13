#pragma once
/// @file rendering/animated_size.h
/// @brief RenderAnimatedSize — 尺寸跟随子节点、用动画补间的渲染对象。
///
/// 对应 上游源码：
///   上游 rendering/animated_size.dart（`RenderAnimatedSize` +
///   `RenderAnimatedSizeState`，430 行）
/// 基类对应关系：上游 `RenderAnimatedSize extends RenderAligningShiftedBox`，
/// 本仓没有独立的 AligningShiftedBox 层（对齐逻辑在 `RenderAlign` =
/// 上游 `RenderPositionedBox` 里），故直接落 `RenderProxyBox` + 自带的
/// `alignChild`，两者的偏移公式一致。
///
/// 与上游的有意偏离（三条，都在实现处有对应注释）：
///   1. **没有 `vsync`/`TickerProvider`**。上游用 `AnimationController(vsync:)`
///      且 `set vsync` 走 `resync`；本仓渲染层直接持有 foundation `Ticker`
///      （`RenderCheckbox` 的 `detail::RenderTransition` 同款做法），
///      挂接/摘除时机改由 `attachedToRoot`/`detachedFromRoot` 承担。
///   2. **`curve` 是 `std::function<double(double)>`**（进度 0..1 → 缓动值）。
///      上游是 `Curve` 对象；`applyEasing` 在 cui_widgets 里，渲染层不能反向
///      依赖，故由控件层把 `Easing` 包成函数传进来（默认等价 `Curves.linear`）。
///   3. **裁剪用 canvas save/clipRect/restore**，不用 `pushClipRect` +
///      `LayerHandle`（与 `RenderClipRect` 同一简化，见其 performPaint）。
///
/// 未移植的上游成员：`reverseDuration`（上游全程只 `forward()`/`stop()`，从不
/// `reverse()`，是条死参数）、`computeDryLayout`/`computeDryBaseline`（本仓无
/// dry 布局）、`debugController`/`debugAnimation`/`debugFillProperties`。

#include <functional>
#include <memory>
#include <optional>

#include "cui/foundation/geometry.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_box.h"

namespace cui {

/// 上游的 `Clip`（dart:ui）在本仓的对应枚举。AnimatedSize 只用到
/// none / hardEdge / antiAlias（后两者行为相同：都裁，差别仅抗锯齿）。
enum class Clip {
    None,
    HardEdge,
    AntiAlias,
    AntiAliasWithSaveLayer,
};

/// 上游的 `RenderAnimatedSizeState`。
enum class RenderAnimatedSizeState {
    Start,    ///< 还没测过子节点尺寸。
    Stable,   ///< 认为子节点尺寸已稳定，让动画跑完。
    Changed,  ///< 子节点刚换过一次尺寸（被当作「一次性跳变」来补间）。
    Unstable, ///< 子节点连续两帧都在换尺寸 ⇒ 不再补间，直接跟随。
};

/// 尺寸补间过渡控制器（AnimationController 子集，见文件头偏离 1）。
///
/// 只做三件事：按真实时钟把 progress 从 `from` 推到 1.0、每步在值确实变化时
/// 通知宿主标脏布局、到界时发一次 completed 事件。
class SizeTransitionController {
public:
    using Tick = std::function<void()>;
    using Completed = std::function<void()>;

    SizeTransitionController(Tick onTick, Completed onCompleted)
        : onTick_(std::move(onTick)), onCompleted_(std::move(onCompleted)) {}
    ~SizeTransitionController() { stop(); }

    void setDuration(double ms) { durationMs_ = ms > 0.0 ? ms : 0.0; }
    double duration() const { return durationMs_; }

    double value() const { return value_; }
    bool isAnimating() const { return ticker_ && ticker_->isActive(); }

    /// 上游的 `forward({double? from})`：from 有值先归位，再起跑。
    /// duration ≤ 0 时直达 1.0（等价上游 `Duration.zero` 那一路）。
    void forward(std::optional<double> from = std::nullopt);

    /// 上游的 `stop()`。
    void stop();

    /// 立即到界（不发 completed 事件之外的通知），测试用。
    void jumpTo1();

private:
    void step();

    Tick onTick_;
    Completed onCompleted_;
    std::unique_ptr<Ticker> ticker_;
    double durationMs_ = 0.0;
    double value_ = 0.0;
    double startValue_ = 0.0;
    double startMs_ = 0.0;
    double baseMs_ = 0.0;
};

/// 子节点尺寸变化时用动画过渡自身尺寸的渲染对象。上游的 `RenderAnimatedSize`。
class RenderAnimatedSize : public RenderProxyBox {
public:
    /// 进度缓动函数（0..1 → 0..1）。空 = 线性（上游默认 `Curves.linear`）。
    using CurveFn = std::function<double(double)>;
    using OnEnd = std::function<void()>;

    explicit RenderAnimatedSize(Alignment alignment = {0.0, 0.0}, double durationMs = 0.0,
                                CurveFn curve = nullptr, Clip clipBehavior = Clip::HardEdge,
                                OnEnd onEnd = nullptr)
        : alignment_(alignment),
          curve_(std::move(curve)),
          clipBehavior_(clipBehavior),
          onEnd_(std::move(onEnd)),
          controller_([this] { onControllerTick(); }, [this] { onControllerCompleted(); }) {
        controller_.setDuration(durationMs);
    }

    ~RenderAnimatedSize() override;

    // ── 参数（全部对齐上游 的 setter 形状：等值早退）──────────────────────

    Alignment alignment() const { return alignment_; }
    void setAlignment(Alignment a) {
        if (a == alignment_)
            return;
        alignment_ = a;
        markNeedsLayout();
    }

    double duration() const { return controller_.duration(); }
    /// 上游只把新值赋给 controller，不重启动画（本仓同）。
    void setDuration(double ms) { controller_.setDuration(ms); }

    const CurveFn& curve() const { return curve_; }
    void setCurve(CurveFn c) {
        curve_ = std::move(c);
        // 上游的 `CurvedAnimation.curve` 是就地替换，下一个 tick 生效，
        // 不重启动画，也不额外标脏（值仍会经 _lastValue 比对）。
    }

    Clip clipBehavior() const { return clipBehavior_; }
    void setClipBehavior(Clip c) {
        if (c == clipBehavior_)
            return;
        clipBehavior_ = c;
        markNeedsPaint();
    }

    const OnEnd& onEnd() const { return onEnd_; }
    void setOnEnd(OnEnd cb) { onEnd_ = std::move(cb); }

    // ── 观测（上游 `@visibleForTesting state` / `isAnimating`）──────────────

    RenderAnimatedSizeState state() const { return state_; }
    bool isAnimating() const { return controller_.isAnimating(); }
    /// 当前动画进度（上游 debugController.value）。
    double animationValue() const { return controller_.value(); }
    /// 本帧实际使用的补间端点（上游 _sizeTween.begin / .end）。
    const std::optional<Size>& sizeTweenBegin() const { return sizeTweenBegin_; }
    const std::optional<Size>& sizeTweenEnd() const { return sizeTweenEnd_; }
    bool hasVisualOverflow() const { return hasVisualOverflow_; }

    // ── RenderObject 协议 ───────────────────────────────────────────────────

    /// 上游的 `attach`：changed/unstable 时补一次 markNeedsLayout，
    /// 以便「滚出视口再回来」的动画能接着跑（本仓 detach 会 stop）。
    void attachedToRoot() override;
    void detachedFromRoot() override;

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    /// 上游的 `_restartAnimation`。
    void restartAnimation();
    void layoutStart(RenderBox* boxChild);
    void layoutStable(RenderBox* boxChild);
    void layoutChanged(RenderBox* boxChild);
    void layoutUnstable(RenderBox* boxChild);

    /// 上游的 `_sizeTween.evaluate(_animation)`。
    Size animatedSize() const;
    double curvedValue() const;
    void onControllerTick();
    void onControllerCompleted();

    Alignment alignment_{0.0, 0.0};
    CurveFn curve_;
    Clip clipBehavior_ = Clip::HardEdge;
    OnEnd onEnd_;

    SizeTransitionController controller_;
    RenderAnimatedSizeState state_ = RenderAnimatedSizeState::Start;

    std::optional<Size> sizeTweenBegin_;
    std::optional<Size> sizeTweenEnd_;
    Size currentSize_{};
    /// 上游的 `_lastValue`：布局时记下当前进度，tick 只在值真的变了时标脏。
    std::optional<double> lastValue_;
    bool hasVisualOverflow_ = false;
};

}  // namespace cui_core
