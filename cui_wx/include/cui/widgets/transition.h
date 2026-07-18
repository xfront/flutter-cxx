#pragma once
/// @file widgets/transition.h
/// @brief 显式动画控件族 —— 对齐上游 widgets/transitions.dart。
///
/// 与隐式动画（implicit_animation.h）的分工：隐式控件自己持有插值器，
/// 目标值变化时自动补间；本头文件的控件全部由**外部** Animation 驱动，
/// 取值即 `animation.value`，时序完全交给调用方的控制器。
///
/// 上游对应关系（widgets/transitions.dart）：
///   AnimatedWidget      本头文件的 AnimatedWidget / AnimatedWidgetState
///   MatrixTransition    MatrixTransition（onTransform 回调产矩阵）
///   ScaleTransition     ScaleTransition   —— MatrixTransition + 缩放矩阵
///   RotationTransition  RotationTransition —— MatrixTransition + rotationZ
///   SlideTransition     SlideTransition   —— FractionalTranslation
///   SizeTransition      SizeTransition    —— ClipRect + Align(宽/高因子)
///   FadeTransition      FadeTransition    —— Opacity
///   AlignTransition     AlignTransition   —— Align
///   ListenableBuilder   ListenableBuilder —— 订阅 foundation 的 Listenable
///   ValueListenableBuilder  同上，把新值交给 builder
///
/// 上游的 transitions.dart 里 ListenableBuilder / AnimatedBuilder 与过渡族
/// 同处一文件，这里保持一致的归属。
///
/// 取值区间：上游用 `Tween(begin, end).animate(controller)` 得到
/// Animation<double>；本框架的 Animation 只产出 [0,1] 进度，因此每个控件
/// 额外收一个 range（Tween<double>），`animatedValue()` 即上游的
/// `animation.value`。
///
/// 用法：
///   auto ctrl = cui_core::make_shared<AnimationController>(300);
///   ctrl->start();
///   auto t = cui_core::make_shared<FadeTransition>(ctrl, child);

#include <cmath>
#include <functional>
#include <memory>
#include <optional>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/matrix4.h"
#include "cui/framework/framework.h"
#include "cui/reactive/observable.h"
#include "cui/rendering/render_flex.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/paint.h"

namespace cui {

// ── AnimatedWidget ──────────────────────────────────────────────────────────

/// 由外部 Animation 驱动的控件基类。上游的 `AnimatedWidget`。
///
/// 上游把「订阅动画 → 逐帧重建」收在 `_AnimatedState` 里；本框架同样收在
/// AnimatedWidgetState，动画每 tick 一次 progressChanged 就 setState 一次。
class AnimatedWidget : public StatefulWidget {
public:
    explicit AnimatedWidget(SharedPtr<Animation<double>> animation, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), animation_(std::move(animation)) {}

    SharedPtr<Animation<double>> animation() const { return animation_; }

    /// 取值区间（上游 `Animation<double>` 的 begin/end），默认 0..1。
    const Tween<double>& range() const { return range_; }
    void setRange(const Tween<double>& r) { range_ = r; }

    /// 上游的 `animation.value`。
    double animatedValue() const {
        if (!animation_)
            return range_.begin();
        return range_.evaluate(*animation_);
    }

    std::unique_ptr<State> createState() override;

protected:
    SharedPtr<Animation<double>> animation_;
    Tween<double> range_{0.0, 1.0};
};

/// AnimatedWidget 的状态：订阅动画并在每帧重建子树。
class AnimatedWidgetState : public State {
public:
    void initState() override;
    void dispose() override;
    void didUpdateWidget(cui::Widget* oldWidget) override;

    /// 基类不产出子树；子类必须覆写。
    SharedPtr<Widget> build(BuildContext& context) override { return nullptr; }

protected:
    const AnimatedWidget* animatedWidget() const {
        return static_cast<const AnimatedWidget*>(widget());
    }

    /// 当前动画值（上游的 `animation.value`）。
    double value() const { return animatedWidget() ? animatedWidget()->animatedValue() : 0.0; }

private:
    void subscribe();
    void unsubscribe();

    /// 记住实际订阅到的对象：dispose 时 widget_ 可能已被换掉，不能靠 widget() 反查。
    Animation<double>* subscribed_ = nullptr;
    int handle_ = 0;  // 0 = 未订阅
};

inline std::unique_ptr<State> AnimatedWidget::createState() {
    return std::make_unique<AnimatedWidgetState>();
}

inline void AnimatedWidgetState::initState() { subscribe(); }

inline void AnimatedWidgetState::dispose() { unsubscribe(); }

inline void AnimatedWidgetState::didUpdateWidget(cui::Widget* oldWidget) {
    const auto* old = static_cast<const AnimatedWidget*>(oldWidget);
    AnimatedWidget* current = static_cast<AnimatedWidget*>(widget());
    const bool same = old && current && old->animation().get() == current->animation().get();
    if (same)
        return;
    // 换了 animation 就要换订阅对象，否则旧 controller 的 tick 会继续打这棵树。
    unsubscribe();
    subscribe();
}

inline void AnimatedWidgetState::subscribe() {
    AnimatedWidget* w = static_cast<AnimatedWidget*>(widget());
    if (w == nullptr || !w->animation())
        return;
    subscribed_ = w->animation().get();
    // 走句柄通道：捕获 this 的 lambda 无法靠 removeListener 比较摘除。
    handle_ = subscribed_->addListenerWithHandle([this] { setState(); });
}

inline void AnimatedWidgetState::unsubscribe() {
    if (subscribed_ != nullptr && handle_ != 0)
        subscribed_->removeListenerByHandle(handle_);
    subscribed_ = nullptr;
    handle_ = 0;
}

// ── FadeTransition ──────────────────────────────────────────────────────────

/// 淡入淡出。上游的 `FadeTransition`。
///
/// 上游这里是 SingleChildRenderObjectWidget + RenderAnimatedOpacity（渲染层
/// 直接读动画值、不重建子树）；本框架的渲染对象不订阅响应式流，因此走与
/// AnimatedOpacity 相同的重建路径，外部行为一致。
class FadeTransition : public AnimatedWidget {
public:
    explicit FadeTransition(SharedPtr<Animation<double>> opacity,
                            SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : AnimatedWidget(std::move(opacity), std::move(key)), child_(std::move(child)) {}

    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Widget> child_;
};

class FadeTransitionState : public AnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── MatrixTransition ────────────────────────────────────────────────────────

/// 由动画值产出矩阵的回调。上游的 `TransformCallback`。
using TransformCallback = std::function<Matrix4(double)>;

/// 动画驱动矩阵变换。上游的 `MatrixTransition`。
class MatrixTransition : public AnimatedWidget {
public:
    MatrixTransition(SharedPtr<Animation<double>> animation, TransformCallback onTransform,
                     Alignment alignment = Alignment::center(),
                     SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : AnimatedWidget(std::move(animation), std::move(key)),
          onTransform_(std::move(onTransform)),
          alignment_(alignment),
          child_(std::move(child)) {}

    const TransformCallback& onTransform() const { return onTransform_; }
    Alignment alignment() const { return alignment_; }
    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    std::unique_ptr<State> createState() override;

protected:
    TransformCallback onTransform_;
    Alignment alignment_;
    SharedPtr<Widget> child_;
};

class MatrixTransitionState : public AnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── ScaleTransition ─────────────────────────────────────────────────────────

/// 缩放过渡。上游的 `ScaleTransition`（MatrixTransition 的缩放特化）。
///
/// 值为 v 时子节点按 v 倍绘制；默认绕盒子中心（上游 alignment 默认
/// Alignment.center）。
class ScaleTransition : public MatrixTransition {
public:
    explicit ScaleTransition(SharedPtr<Animation<double>> scale,
                             SharedPtr<Widget> child = nullptr,
                             Alignment alignment = Alignment::center(), KeyPtr key = nullptr)
        : MatrixTransition(std::move(scale), handleScaleMatrix, alignment, std::move(child),
                           std::move(key)) {}

    /// 上游的 `ScaleTransition._handleScaleMatrix`。
    static Matrix4 handleScaleMatrix(double value) { return Matrix4::scale(value, value, 1.0); }
};

// ── RotationTransition ──────────────────────────────────────────────────────

/// 旋转过渡。上游的 `RotationTransition`（MatrixTransition 的旋转特化）。
///
/// 值为 v 圈（turns），子节点旋转 v * 2 * pi 弧度；默认绕盒子中心。
class RotationTransition : public MatrixTransition {
public:
    explicit RotationTransition(SharedPtr<Animation<double>> turns,
                                SharedPtr<Widget> child = nullptr,
                                Alignment alignment = Alignment::center(), KeyPtr key = nullptr)
        : MatrixTransition(std::move(turns), handleTurnsMatrix, alignment, std::move(child),
                           std::move(key)) {}

    /// 上游的 `RotationTransition._handleTurnsMatrix`。
    static Matrix4 handleTurnsMatrix(double value) {
        return Matrix4::rotationZ(value * M_PI * 2.0);
    }
};

// ── SlideTransition ─────────────────────────────────────────────────────────

/// 按子节点自身尺寸比例平移的过渡。上游的 `SlideTransition`。
///
/// 上游收 `Animation<Offset>`；本框架的 Animation 只产 [0,1] 进度，故显式
/// 给出平移区间 begin → end（position.value 即两者的线性插值）。
///
/// 上游的 textDirection 只影响 x 的正负号；本框架暂无文字方向体系，
/// x 恒按画布坐标（正向右）。
class SlideTransition : public AnimatedWidget {
public:
    SlideTransition(SharedPtr<Animation<double>> position, Offset begin, Offset end,
                    SharedPtr<Widget> child = nullptr, bool transformHitTests = true,
                    KeyPtr key = nullptr)
        : AnimatedWidget(std::move(position), std::move(key)),
          begin_(begin), end_(end),
          transformHitTests_(transformHitTests), child_(std::move(child)) {}

    Offset begin() const { return begin_; }
    Offset end() const { return end_; }
    void setPosition(Offset begin, Offset end) { begin_ = begin; end_ = end; }

    bool transformHitTests() const { return transformHitTests_; }
    void setTransformHitTests(bool v) { transformHitTests_ = v; }

    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    /// 上游的 `position.value`。
    Offset positionValue() const {
        const double t = animatedValue();
        return Offset(begin_.dx + (end_.dx - begin_.dx) * t,
                      begin_.dy + (end_.dy - begin_.dy) * t);
    }

    std::unique_ptr<State> createState() override;

private:
    Offset begin_;
    Offset end_;
    bool transformHitTests_ = true;
    SharedPtr<Widget> child_;
};

class SlideTransitionState : public AnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── SizeTransition ──────────────────────────────────────────────────────────

/// 动画自身尺寸并裁剪/对齐子节点。上游的 `SizeTransition`。
///
/// sizeFactor 变化的那根轴取「子尺寸 × max(value, 0)」，交叉轴取
/// fixedCrossAxisSizeFactor（为空则尽量撑满）。上游的 axisAlignment 已被
/// alignment 取代，这里只提供后者。
class SizeTransition : public AnimatedWidget {
public:
    SizeTransition(SharedPtr<Animation<double>> sizeFactor,
                   SharedPtr<Widget> child = nullptr, Axis axis = Axis::Vertical,
                   std::optional<Alignment> alignment = std::nullopt,
                   std::optional<double> fixedCrossAxisSizeFactor = std::nullopt,
                   KeyPtr key = nullptr)
        : AnimatedWidget(std::move(sizeFactor), std::move(key)),
          axis_(axis), alignment_(alignment),
          fixedCrossAxisSizeFactor_(fixedCrossAxisSizeFactor), child_(std::move(child)) {}

    Axis axis() const { return axis_; }
    void setAxis(Axis a) { axis_ = a; }

    const std::optional<Alignment>& alignment() const { return alignment_; }
    void setAlignment(std::optional<Alignment> a) { alignment_ = a; }

    const std::optional<double>& fixedCrossAxisSizeFactor() const {
        return fixedCrossAxisSizeFactor_;
    }

    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    /// 上游 build 里的默认 alignment：水平轴向 (0,-1)、垂直轴向 (-1,0)。
    Alignment effectiveAlignment() const {
        if (alignment_)
            return *alignment_;
        return axis_ == Axis::Horizontal ? Alignment(0.0, -1.0) : Alignment(-1.0, 0.0);
    }

    std::unique_ptr<State> createState() override;

private:
    Axis axis_;
    std::optional<Alignment> alignment_;
    std::optional<double> fixedCrossAxisSizeFactor_;
    SharedPtr<Widget> child_;
};

class SizeTransitionState : public AnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── AlignTransition ─────────────────────────────────────────────────────────

/// 动画子节点对齐。上游的 `AlignTransition`。
class AlignTransition : public AnimatedWidget {
public:
    AlignTransition(SharedPtr<Animation<double>> alignment,
                    SharedPtr<Widget> child = nullptr,
                    Alignment begin = Alignment::center(), Alignment end = Alignment::center(),
                    KeyPtr key = nullptr)
        : AnimatedWidget(std::move(alignment), std::move(key)),
          begin_(begin), end_(end), child_(std::move(child)) {}

    Alignment begin() const { return begin_; }
    Alignment end() const { return end_; }
    void setAlignments(Alignment begin, Alignment end) { begin_ = begin; end_ = end; }

    SharedPtr<Widget> child() const { return child_; }

    /// 上游的 `animation.value`（Alignment 区间插值）。
    Alignment alignmentValue() const {
        return Alignment::lerp(begin_, end_, animatedValue());
    }

    std::unique_ptr<State> createState() override;

private:
    Alignment begin_;
    Alignment end_;
    SharedPtr<Widget> child_;
};

class AlignTransitionState : public AnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── 实现 ────────────────────────────────────────────────────────────────────

inline std::unique_ptr<State> FadeTransition::createState() {
    return std::make_unique<FadeTransitionState>();
}

inline SharedPtr<Widget> FadeTransitionState::build(BuildContext& context) {
    auto* w = static_cast<FadeTransition*>(widget());
    if (!w)
        return nullptr;
    return cui::make_shared<Opacity>(value(), w->child());
}

inline std::unique_ptr<State> MatrixTransition::createState() {
    return std::make_unique<MatrixTransitionState>();
}

inline SharedPtr<Widget> MatrixTransitionState::build(BuildContext& context) {
    auto* w = static_cast<MatrixTransition*>(widget());
    if (!w || !w->onTransform())
        return w ? w->child() : nullptr;
    // 上游的 Transform(transform: onTransform(value), alignment: alignment)。
    return cui::make_shared<Transform>(w->onTransform()(value()), w->child(), Offset(), true,
                                       nullptr, w->alignment());
}

inline std::unique_ptr<State> SlideTransition::createState() {
    return std::make_unique<SlideTransitionState>();
}

inline SharedPtr<Widget> SlideTransitionState::build(BuildContext& context) {
    auto* w = static_cast<SlideTransition*>(widget());
    if (!w)
        return nullptr;
    return cui::make_shared<FractionalTranslation>(w->positionValue(), w->child(), nullptr,
                                                   w->transformHitTests());
}

inline std::unique_ptr<State> SizeTransition::createState() {
    return std::make_unique<SizeTransitionState>();
}

inline SharedPtr<Widget> SizeTransitionState::build(BuildContext& context) {
    auto* w = static_cast<SizeTransition*>(widget());
    if (!w)
        return nullptr;
    // 上游的 ClipRect(child: Align(alignment: …, widthFactor / heightFactor: …))。
    const double factor = std::max(value(), 0.0);
    const bool horizontal = w->axis() == Axis::Horizontal;
    Alignment a = w->effectiveAlignment();
    return cui::make_shared<ClipRect>(
        cui::make_shared<Align>(a.x, a.y, w->child(), nullptr,
                                horizontal ? factor : w->fixedCrossAxisSizeFactor(),
                                horizontal ? w->fixedCrossAxisSizeFactor() : factor));
}

inline std::unique_ptr<State> AlignTransition::createState() {
    return std::make_unique<AlignTransitionState>();
}

inline SharedPtr<Widget> AlignTransitionState::build(BuildContext& context) {
    auto* w = static_cast<AlignTransition*>(widget());
    if (!w)
        return nullptr;
    Alignment a = w->alignmentValue();
    return cui::make_shared<Align>(a.x, a.y, w->child());
}

// ── DualTransitionBuilder ───────────────────────────────────────────────────

/// 入场/退场两个 builder 的签名。上游的 `AnimatedTransitionBuilder`
/// （widgets/dual_transition_builder.dart）。
using AnimatedTransitionBuilder = std::function<SharedPtr<Widget>(
    BuildContext& context, SharedPtr<Animation<double>> animation,
    SharedPtr<Widget> child)>;

class DualTransitionBuilderState;

/// 按动画的播放方向选择入场 / 退场转场。上游的 `DualTransitionBuilder`
/// （widgets/dual_transition_builder.dart）。
///
/// 两个 builder 收到的动画永远按「正向 0→1」走：
///   * 正向段：forwardBuilder 拿真实动画，reverseBuilder 拿
///     `kAlwaysDismissedAnimation`；
///   * 反向段：forwardBuilder 拿 `kAlwaysCompleteAnimation`，reverseBuilder
///     拿 `ReverseAnimation(animation)`。
///
/// 有效方向由 `_calculateEffectiveAnimationStatus` 计算：正向播到一半被打断
/// 转反向时仍沿用正向的有效方向（否则入场过渡会在一半突然换成完全不同的
/// 退场过渡，视觉断裂）；只有从两个端点出发时方向才真正切换。
class DualTransitionBuilder : public StatefulWidget {
public:
    DualTransitionBuilder(SharedPtr<Animation<double>> animation,
                          AnimatedTransitionBuilder forwardBuilder,
                          AnimatedTransitionBuilder reverseBuilder,
                          SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), animation_(std::move(animation)),
          forwardBuilder_(std::move(forwardBuilder)),
          reverseBuilder_(std::move(reverseBuilder)), child_(std::move(child)) {}

    const SharedPtr<Animation<double>>& animation() const { return animation_; }
    const AnimatedTransitionBuilder& forwardBuilder() const { return forwardBuilder_; }
    const AnimatedTransitionBuilder& reverseBuilder() const { return reverseBuilder_; }
    const SharedPtr<Widget>& child() const { return child_; }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Animation<double>> animation_;
    AnimatedTransitionBuilder forwardBuilder_;
    AnimatedTransitionBuilder reverseBuilder_;
    SharedPtr<Widget> child_;
};

class DualTransitionBuilderState : public State {
public:
    void initState() override;
    void didUpdateWidget(Widget* oldWidget) override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    /// 上游的 `_calculateEffectiveAnimationStatus`。
    static AnimationStatus calculateEffectiveStatus(AnimationStatus lastEffective,
                                                    AnimationStatus current);
    void subscribe(SharedPtr<Animation<double>> animation);
    void unsubscribe();
    void updateAnimations();
    void onStatus(AnimationStatus status);

    AnimationStatus effectiveStatus_ = AnimationStatus::Dismissed;
    /// 记住实际订阅到的对象：dispose / 换 animation 时不能靠 widget() 反查。
    SharedPtr<Animation<double>> subscribed_;
    int statusHandle_ = 0;
    SharedPtr<ProxyAnimation> forwardAnimation_ = cui::make_shared<ProxyAnimation>();
    SharedPtr<ProxyAnimation> reverseAnimation_ = cui::make_shared<ProxyAnimation>();
};

inline std::unique_ptr<State> DualTransitionBuilder::createState() {
    return std::make_unique<DualTransitionBuilderState>();
}

inline AnimationStatus DualTransitionBuilderState::calculateEffectiveStatus(
        AnimationStatus lastEffective, AnimationStatus current) {
    switch (current) {
        case AnimationStatus::Dismissed:
        case AnimationStatus::Completed:
            return current;
        case AnimationStatus::Forward:
            switch (lastEffective) {
                case AnimationStatus::Reverse:
                    return lastEffective;
                default:
                    return current;
            }
        case AnimationStatus::Reverse:
            switch (lastEffective) {
                case AnimationStatus::Forward:
                    return lastEffective;
                default:
                    return current;
            }
    }
    return current;
}

inline void DualTransitionBuilderState::initState() {
    auto* w = static_cast<DualTransitionBuilder*>(widget());
    if (!w || !w->animation())
        return;
    effectiveStatus_ = w->animation()->status();
    subscribe(w->animation());
    updateAnimations();
}

inline void DualTransitionBuilderState::didUpdateWidget(Widget* oldWidget) {
    auto* old = static_cast<DualTransitionBuilder*>(oldWidget);
    auto* current = static_cast<DualTransitionBuilder*>(widget());
    if (!old || !current)
        return;
    if (old->animation().get() == current->animation().get())
        return;
    unsubscribe();
    effectiveStatus_ = current->animation() ? current->animation()->status()
                                            : AnimationStatus::Dismissed;
    subscribe(current->animation());
    updateAnimations();
}

inline void DualTransitionBuilderState::subscribe(SharedPtr<Animation<double>> animation) {
    subscribed_ = std::move(animation);
    if (!subscribed_)
        return;
    // 走句柄通道：捕获 this 的 lambda 无法靠 removeStatusListener 比较摘除。
    statusHandle_ = subscribed_->addStatusListener([this](AnimationStatus s) { onStatus(s); });
}

inline void DualTransitionBuilderState::unsubscribe() {
    if (subscribed_ && statusHandle_ != 0)
        subscribed_->removeStatusListener(statusHandle_);
    subscribed_.reset();
    statusHandle_ = 0;
}

inline void DualTransitionBuilderState::onStatus(AnimationStatus status) {
    const AnimationStatus oldEffective = effectiveStatus_;
    effectiveStatus_ = calculateEffectiveStatus(effectiveStatus_, status);
    if (oldEffective != effectiveStatus_)
        updateAnimations();
}

inline void DualTransitionBuilderState::updateAnimations() {
    auto* w = static_cast<DualTransitionBuilder*>(widget());
    if (!w || !w->animation())
        return;
    switch (effectiveStatus_) {
        case AnimationStatus::Dismissed:
        case AnimationStatus::Forward:
            forwardAnimation_->setParent(w->animation());
            reverseAnimation_->setParent(kAlwaysDismissedAnimation());
            break;
        case AnimationStatus::Reverse:
        case AnimationStatus::Completed:
            forwardAnimation_->setParent(kAlwaysCompleteAnimation());
            reverseAnimation_->setParent(cui::make_shared<ReverseAnimation>(w->animation()));
            break;
    }
}

inline void DualTransitionBuilderState::dispose() { unsubscribe(); }

inline SharedPtr<Widget> DualTransitionBuilderState::build(BuildContext& context) {
    auto* w = static_cast<DualTransitionBuilder*>(widget());
    if (!w)
        return nullptr;
    // 宿主原地改同一个 Widget 对象的 animation 时 didUpdateWidget 走不到
    // （gallery / 测试都这么驱动），build 里补一次重绑。
    if (w->animation().get() != subscribed_.get()) {
        unsubscribe();
        effectiveStatus_ = w->animation() ? w->animation()->status()
                                          : AnimationStatus::Dismissed;
        subscribe(w->animation());
        updateAnimations();
    }
    SharedPtr<Widget> inner = w->reverseBuilder()
                                      ? w->reverseBuilder()(context, reverseAnimation_, w->child())
                                      : w->child();
    if (!w->forwardBuilder())
        return inner;
    return w->forwardBuilder()(context, forwardAnimation_, std::move(inner));
}

// ── ListenableBuilder ───────────────────────────────────────────────────────

/// 订阅任意 Listenable、变更即重建子树的构建器。上游的 `ListenableBuilder`
/// （同定义在 widgets/transitions.dart）。
///
/// 与 AnimatedWidget 的分工：这里订阅的是 foundation 的 `Listenable`
/// （ChangeNotifier / ValueNotifier），而非 Animation 的 progressChanged 流。
///
/// 本仓的 `Listenable` 目前只有 `ChangeNotifier` 一族实现，且
/// `removeListener` 靠 `sameCallbackTarget` 比较目标 —— 捕获了 this 的 lambda
/// 永远比不上（见 foundation/change_notifier.h），因此订阅走句柄通道
/// `addListenerWithHandle` / `removeListenerByHandle`，否则 dispose 摘不掉监听，
/// State 释放后仍会被通知（ASan: heap-use-after-free）。
class ListenableBuilder;

/// ListenableBuilder 的 State（先声明：createState 需要完整类型）。
class ListenableBuilderState : public State {
public:
    void initState() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    int handle_ = 0;  // 0 = 未订阅
};

class ListenableBuilder : public StatefulWidget {
public:
    using Builder = std::function<SharedPtr<Widget>(BuildContext&,
                                                          SharedPtr<Widget>)>;

    ListenableBuilder(SharedPtr<Listenable> listenable, Builder builder,
                      SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), listenable_(std::move(listenable)),
          builder_(std::move(builder)), child_(std::move(child)) {}

    SharedPtr<Listenable> listenable() const { return listenable_; }
    SharedPtr<Widget> child() const { return child_; }
    const Builder& builder() const { return builder_; }

    std::unique_ptr<State> createState() override {
        return std::make_unique<ListenableBuilderState>();
    }

private:
    SharedPtr<Listenable> listenable_;
    Builder builder_;
    SharedPtr<Widget> child_;
};

inline void ListenableBuilderState::initState() {
    auto* w = static_cast<ListenableBuilder*>(widget());
    if (!w || !w->listenable())
        return;
    auto* notifier = dynamic_cast<ChangeNotifier*>(w->listenable().get());
    if (!notifier)
        return;
    handle_ = notifier->addListenerWithHandle([this] { setState(); });
}

inline void ListenableBuilderState::dispose() {
    auto* w = static_cast<ListenableBuilder*>(widget());
    if (!w || !w->listenable() || handle_ == 0)
        return;
    if (auto* notifier = dynamic_cast<ChangeNotifier*>(w->listenable().get()))
        notifier->removeListenerByHandle(handle_);
    handle_ = 0;
}

inline SharedPtr<Widget> ListenableBuilderState::build(BuildContext& context) {
    auto* w = static_cast<ListenableBuilder*>(widget());
    if (!w || !w->builder())
        return w ? w->child() : nullptr;
    return w->builder()(context, w->child());
}

// ── ValueListenableBuilder ──────────────────────────────────────────────────

/// 订阅 ValueNotifier 并把新值交给 builder。上游的 `ValueListenableBuilder`。
///
///   auto count = cui_core::make_shared<ValueNotifier<int>>(0);
///   auto vlb = cui_core::make_shared<ValueListenableBuilder<int>>(
///       count, [](BuildContext&, int v, SharedPtr<Widget>) {
///           return cui_core::make_shared<Text>(std::to_string(v));
///       });
///   count->setValue(3);  // 自动重建
template <typename T>
class ValueListenableBuilder;

template <typename T>
class ValueListenableBuilderState : public State {
public:
    void initState() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    int handle_ = 0;  // 0 = 未订阅（同 ListenableBuilderState，必须走句柄通道）
};

template <typename T>
class ValueListenableBuilder : public StatefulWidget {
public:
    using Notifier = ValueNotifier<T>;
    using Builder = std::function<SharedPtr<Widget>(BuildContext&, const T&,
                                                          SharedPtr<Widget>)>;

    ValueListenableBuilder(SharedPtr<Notifier> notifier, Builder builder,
                           SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), notifier_(std::move(notifier)),
          builder_(std::move(builder)), child_(std::move(child)) {}

    SharedPtr<Notifier> notifier() const { return notifier_; }
    SharedPtr<Widget> child() const { return child_; }
    const Builder& builder() const { return builder_; }

    std::unique_ptr<State> createState() override {
        return std::make_unique<ValueListenableBuilderState<T>>();
    }

private:
    SharedPtr<Notifier> notifier_;
    Builder builder_;
    SharedPtr<Widget> child_;
};

template <typename T>
void ValueListenableBuilderState<T>::initState() {
    auto* w = static_cast<ValueListenableBuilder<T>*>(widget());
    if (!w || !w->notifier())
        return;
    handle_ = w->notifier()->addListenerWithHandle([this] { setState(); });
}

template <typename T>
void ValueListenableBuilderState<T>::dispose() {
    auto* w = static_cast<ValueListenableBuilder<T>*>(widget());
    if (!w || !w->notifier() || handle_ == 0)
        return;
    w->notifier()->removeListenerByHandle(handle_);
    handle_ = 0;
}

template <typename T>
SharedPtr<Widget> ValueListenableBuilderState<T>::build(BuildContext& context) {
    auto* w = static_cast<ValueListenableBuilder<T>*>(widget());
    if (!w || !w->builder())
        return w ? w->child() : nullptr;
    const T v = w->notifier() ? w->notifier()->value() : T{};
    return w->builder()(context, v, w->child());
}

}  // namespace cui_core
