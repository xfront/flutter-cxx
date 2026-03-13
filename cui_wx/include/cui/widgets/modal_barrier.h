#pragma once
/// @file widgets/modal_barrier.h
/// @brief 上游的 ModalBarrier / AnimatedModalBarrier 移植。
///
/// 对应 上游源码：
///   上游 widgets/modal_barrier.dart
///
/// 「barrier」= 挡在模态路由下面的那层遮罩：它吃掉自身以下的全部命中，
/// 并且（`dismissible == true` 时）点一下就弹掉当前路由。
///
/// 与上游的有意差异（勿当 bug 排查）：
///   1. **语义层未接**。上游 用 `Semantics` / `BlockSemantics` /
///      `ExcludeSemantics` / `_SemanticsClipper` 把 barrier 报给无障碍工具，
///      本仓还没有 Semantics 体系 ⇒ `semanticsLabel` /
///      `barrierSemanticsDismissible` / `clipDetailsNotifier` /
///      `semanticsOnTapHint` 四个参数未采。
///   2. **鼠标指针未接**。上游 包了一层
///      `MouseRegion(cursor: SystemMouseCursors.basic)`；本仓 `MouseRegion`
///      还没有 cursor 通道（见 widgets/basic.h 的同名类注释），故省略。
///   3. **不可 dismissible 时不再响一声**。上游 走
///      `SystemSound.play(SystemSoundType.alert)`，本仓没有平台声音通道。
///   4. **平台差异未采**。上游 只在
///      android/iOS/macOS 上把「无障碍可关闭」打开（
///      `platformSupportsDismissingBarrier`），这条与 1. 一起去掉。
///
/// 上游 的 `handleDismiss` 语义完整保留：`dismissible` 为真时优先调
/// `onDismiss`，没有就给 `Navigator.maybePop(context)`；为假时点击无效果。

#include <functional>
#include <memory>
#include <optional>

#include "cui/framework/framework.h"
#include "cui/foundation/color.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/navigator.h"

namespace cui {

/// 阻止用户与其下方组件交互的遮罩。上游的 `ModalBarrier`。
class ModalBarrier : public StatelessWidget {
public:
    using DismissCallback = std::function<void()>;

    explicit ModalBarrier(std::optional<Color> color = std::nullopt,
                          bool dismissible = true,
                          DismissCallback onDismiss = nullptr,
                          KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), color_(color), dismissible_(dismissible),
          onDismiss_(std::move(onDismiss)) {}

    const std::optional<Color>& color() const { return color_; }
    bool dismissible() const { return dismissible_; }
    const DismissCallback& onDismiss() const { return onDismiss_; }

    /// 上游的 `ModalBarrier.build` 里那个局部函数 `handleDismiss`：
    /// `dismissible` 为真时优先调 `onDismiss`，没有就给 `Navigator.maybePop`。
    ///
    /// 上游在 `dismissible == false` 时只响一声（`SystemSound.play`）；本仓没有
    /// 平台声音通道，所以那条分支就是「什么都不做」——但**点击依然被吞掉**，
    /// 因为吞噬来自 `MouseRegion(opaque: true)`，不来自这条回调。
    ///
    /// 注意这里没有直接复用 `handleDismiss(context)`：那段逻辑按值捕获进
    /// `GestureDetector` 的回调，闭包就不依赖本 Widget 对象还活着
    /// （`StatelessWidget::build` 的 `this` 会在父节点重建时被换掉）。
    SharedPtr<Widget> build(BuildContext& context) override {
        // 上游的形状：RawGestureDetector(opaque) → Semantics → MouseRegion →
        // ConstrainedBox(expand) → ColoredBox?。语义层整段省略（见文件头差异 1），
        // MouseRegion 保留下来但不带 cursor（差异 2）——它同时充当「吞掉身后兄弟」
        // 的那个节点：本仓的命中链一旦有子节点返回 true 就不再问下一个兄弟，
        // 而 RenderMouseRegion::hitTest 的 `hit && opaque_` 正是上游
        // `RenderProxyBoxWithHitTestBehavior(opaque)` 的返回值语义。
        SharedPtr<Widget> fill =
                color_ ? cui::make_shared<ColoredBox>(*color_) : nullptr;
        auto expanded = cui::make_shared<ConstrainedBox>(
                BoxConstraints::expand(std::nullopt, std::nullopt), std::move(fill));
        SharedPtr<Widget> barrier =
                cui::make_shared<MouseRegion>(std::move(expanded), nullptr, nullptr,
                                              nullptr, /*opaque=*/true);

        const bool dismissible = dismissible_;
        DismissCallback onDismiss = onDismiss_;
        // 这里就把 Navigator 取出来按值捕获，而不是在回调里用 &context：
        // `context` 是本控件自己的 Element，闭包的生命周期可以比它长（父节点
        // 重建后旧 widget 还会在 lambda 里活着），捕获引用就是悬挂指针。
        NavigatorState* navigator = NavigatorState::maybeOf(context);
        auto onTap = [dismissible, onDismiss, navigator] {
            if (!dismissible)
                return;
            if (onDismiss) {
                onDismiss();
                return;
            }
            if (navigator)
                navigator->maybePop();
        };
        return cui::make_shared<GestureDetector>(std::move(barrier), std::move(onTap));
    }

private:
    std::optional<Color> color_;
    bool dismissible_ = true;
    DismissCallback onDismiss_;
};

/// 同 `ModalBarrier`，但颜色由一条动画驱动。上游的 `AnimatedModalBarrier`。
///
/// 上游 让它继承 `AnimatedWidget`（订阅 `Animation<Color?>` 后重建）；本仓的
/// `AnimatedWidget` 只支持 `Animation<double>`，所以这里按同一个语义自建订阅
/// —— 走句柄通道（`addListenerWithHandle` / `removeListenerByHandle`），
/// 这是本仓对「捕获 this 的监听器」的硬要求。
class AnimatedModalBarrier : public StatefulWidget {
public:
    using DismissCallback = std::function<void()>;

    explicit AnimatedModalBarrier(SharedPtr<Animation<Color>> color,
                                  bool dismissible = true,
                                  DismissCallback onDismiss = nullptr,
                                  KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), color_(std::move(color)),
          dismissible_(dismissible), onDismiss_(std::move(onDismiss)) {}

    const SharedPtr<Animation<Color>>& color() const { return color_; }
    bool dismissible() const { return dismissible_; }
    const DismissCallback& onDismiss() const { return onDismiss_; }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Animation<Color>> color_;
    bool dismissible_ = true;
    DismissCallback onDismiss_;
};

/// AnimatedModalBarrier 的状态。上游的 `_AnimatedModalBarrierState`（继承
/// AnimatedWidget 的那部分）。
class AnimatedModalBarrierState : public State {
public:
    void initState() override;
    void dispose() override;
    void didUpdateWidget(Widget* oldWidget) override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    void subscribe();
    void unsubscribe();

    Animation<Color>* subscribed_ = nullptr;
    int handle_ = 0;
};

// ── 内联实现 ────────────────────────────────────────────────────────────────

inline std::unique_ptr<State> AnimatedModalBarrier::createState() {
    return std::make_unique<AnimatedModalBarrierState>();
}

inline void AnimatedModalBarrierState::initState() {
    State::initState();
    subscribe();
}

inline void AnimatedModalBarrierState::dispose() {
    unsubscribe();
    State::dispose();
}

inline void AnimatedModalBarrierState::didUpdateWidget(Widget* oldWidget) {
    State::didUpdateWidget(oldWidget);
    auto* old = static_cast<const AnimatedModalBarrier*>(oldWidget);
    auto* current = static_cast<AnimatedModalBarrier*>(widget());
    const bool same = old && current && old->color().get() == current->color().get();
    if (same)
        return;
    unsubscribe();
    subscribe();
}

inline void AnimatedModalBarrierState::subscribe() {
    auto* w = static_cast<AnimatedModalBarrier*>(widget());
    if (!w || !w->color())
        return;
    subscribed_ = w->color().get();
    handle_ = subscribed_->addListenerWithHandle([this] { setState(); });
}

inline void AnimatedModalBarrierState::unsubscribe() {
    if (subscribed_ != nullptr && handle_ != 0)
        subscribed_->removeListenerByHandle(handle_);
    subscribed_ = nullptr;
    handle_ = 0;
}

inline SharedPtr<Widget> AnimatedModalBarrierState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedModalBarrier*>(widget());
    if (!w)
        return nullptr;
    // 上游 用 `color.value`（`Animation<Color?>`）—— 本仓的 Color 无「可空
    // 动画」，因此 a == 0 视作透明：与上游 `barrierColor!.alpha != 0` 的
    // 判据同源，透明时干脆不铺色块。
    std::optional<Color> color;
    if (w->color()) {
        const Color value = w->color()->value();
        if (value.a != 0.0)
            color = value;
    }
    return cui::make_shared<ModalBarrier>(color, w->dismissible(), w->onDismiss());
}

}  // namespace cui_core
