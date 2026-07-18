#pragma once
/// @file widgets/animated_builder.h
/// @brief AnimatedBuilder — bridges an Animation's progress to rebuilds.
///
/// Mirrors the upstream's `AnimatedBuilder`: it subscribes to an Animation's
/// `progressChanged` stream and calls `setState()` on every tick, so the
/// subtree is rebuilt with the latest animation value. This is the missing
/// link that lets the three-layer architecture drive widget rebuilds from the
/// frame scheduler's Ticker pump — instead of the legacy "advance time in
/// paint()" pattern used by the legacy `AnimatedWidget`.

#include <functional>
#include <memory>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/framework/framework.h"
#include "cui/widgets/animation/animation.h"

namespace cui {

class AnimatedBuilder;

/// Builds the subtree from the current BuildContext and the driving Animation.
using AnimatedBuilderCallback =
    std::function<SharedPtr<Widget>(BuildContext& context,
                                         const SharedPtr<Animation<double>>& animation)>;

/// Rebuilds its subtree whenever the given Animation ticks.
///
/// 上游 equivalent: `AnimatedBuilder(animation: a, builder: (c, a) => …)`.
class AnimatedBuilder : public StatefulWidget {
public:
    AnimatedBuilder(SharedPtr<Animation<double>> animation, AnimatedBuilderCallback builder,
                    SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)),
          animation_(std::move(animation)),
          builder_(std::move(builder)),
          child_(std::move(child)) {}

    SharedPtr<Animation<double>> animation() const { return animation_; }
    SharedPtr<Widget> child() const { return child_; }

    std::unique_ptr<State> createState() override;

    friend class AnimatedBuilderState;

private:
    SharedPtr<Animation<double>> animation_;
    AnimatedBuilderCallback builder_;
    SharedPtr<Widget> child_;
};

class AnimatedBuilderState : public State {
public:
    void initState() override;
    void dispose() override;
    void didUpdateWidget(cui::Widget* oldWidget) override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    void subscribe();
    void unsubscribe();

    /// 记住实际订阅到的对象：dispose 时 widget_ 可能已被换掉，不能靠 widget() 反查。
    Animation<double>* subscribed_ = nullptr;
    int handle_ = 0;  // 0 = 未订阅
};

inline std::unique_ptr<State> AnimatedBuilder::createState() {
    return std::make_unique<AnimatedBuilderState>();
}

inline void AnimatedBuilderState::subscribe() {
    auto* w = static_cast<AnimatedBuilder*>(widget());
    if (w == nullptr || !w->animation_)
        return;
    subscribed_ = w->animation_.get();
    // 走句柄通道：捕获 this 的 lambda 无法靠 removeListener 比较摘除。
    handle_ = subscribed_->addListenerWithHandle([this] { setState(); });
}

inline void AnimatedBuilderState::unsubscribe() {
    if (subscribed_ != nullptr && handle_ != 0)
        subscribed_->removeListenerByHandle(handle_);
    subscribed_ = nullptr;
    handle_ = 0;
}

inline void AnimatedBuilderState::initState() { subscribe(); }

inline void AnimatedBuilderState::dispose() { unsubscribe(); }

inline void AnimatedBuilderState::didUpdateWidget(cui::Widget* oldWidget) {
    const auto* old = static_cast<const AnimatedBuilder*>(oldWidget);
    auto* current = static_cast<AnimatedBuilder*>(widget());
    if (old && current && old->animation_.get() == current->animation_.get())
        return;
    unsubscribe();
    subscribe();
}

inline SharedPtr<Widget> AnimatedBuilderState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedBuilder*>(widget());
    if (!w)
        return nullptr;
    if (!w->builder_)
        return w->child_;
    return w->builder_(context, w->animation_);
}

}  // namespace cui_core
