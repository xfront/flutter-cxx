/// @file rendering/animated_size.cpp
/// @brief RenderAnimatedSize 实现（尺寸补间状态机 + 轻量过渡控制器）。
///
/// 逐段对应 上游的 rendering/animated_size.dart；有意偏离的三条（无 vsync、
/// curve 用 std::function、裁剪走 canvas save/clipRect）在头文件里已列出，
/// 下面各实现处再点一次。

#include "cui/rendering/animated_size.h"

#include <algorithm>

#include <include/core/SkCanvas.h>
#include <include/core/SkRect.h>

#include "cui/rendering/painting_context.h"

namespace cui {

namespace {

double steadyNowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ── SizeTransitionController ────────────────────────────────────────────────

void SizeTransitionController::forward(std::optional<double> from) {
    // 上游的 `AnimationController.forward({from})`：先 stop，再（可选）把
    // value 归到 from，然后向 upperBound=1.0 推进。
    stop();
    if (from)
        value_ = *from;
    startValue_ = value_;
    baseMs_ = steadyNowMs();
    if (!ticker_)
        ticker_ = std::make_unique<Ticker>([this](Duration) { step(); });
    ticker_->start();
}

void SizeTransitionController::stop() {
    // 上游的 `stop()`：停在当前 value（不插值、不发 completed）。value_ 由
    // 每次 step() 实时写回，所以这里只需摘掉 ticker。
    if (ticker_ && ticker_->isActive())
        ticker_->stop();
}

void SizeTransitionController::jumpTo1() {
    stop();
    value_ = 1.0;
}

void SizeTransitionController::step() {
    // 注意：progress 是线性的，缓动曲线在宿主侧（curvedValue）套用，
    // 与上游 `CurvedAnimation(parent: controller, curve: curve)` 分层一致。
    const double p =
        durationMs_ > 0.0 ? std::clamp((steadyNowMs() - baseMs_) / durationMs_, 0.0, 1.0) : 1.0;
    value_ = startValue_ + (1.0 - startValue_) * p;
    if (onTick_)
        onTick_();
    if (p >= 1.0 && ticker_ && ticker_->isActive()) {
        ticker_->stop();
        // 上游的 AnimationStatus listener：只在到界那一刻发一次。
        if (onCompleted_)
            onCompleted_();
    }
}

// ── RenderAnimatedSize ──────────────────────────────────────────────────────

RenderAnimatedSize::~RenderAnimatedSize() {
    // 析构顺序：先摘 ticker，成员（含 controller_）随后销毁，避免回调打到
    // 已析构的宿主上。
    controller_.stop();
}

void RenderAnimatedSize::attachedToRoot() {
    RenderProxyBox::attachedToRoot();
    // 上游的 `attach`：changed / unstable 表示「离开时动画还没跑完」，回来后
    // 只补一次 markNeedsLayout —— 接力点在那之后的布局里（layoutStable /
    // layoutChanged 的 `if (!isAnimating) forward()`，从冻结的进度接着走），
    // 这里不抢先重启。
    if (state_ == RenderAnimatedSizeState::Changed || state_ == RenderAnimatedSizeState::Unstable)
        markNeedsLayout();
}

void RenderAnimatedSize::detachedFromRoot() {
    // 上游的 `detach`：停动画 + 摘监听。
    controller_.stop();
    RenderProxyBox::detachedFromRoot();
}

void RenderAnimatedSize::restartAnimation() {
    // 上游的 `_restartAnimation`。
    lastValue_ = 0.0;
    controller_.forward(0.0);
}

Size RenderAnimatedSize::animatedSize() const {
    // 上游的 `_sizeTween.evaluate(_animation)`。
    if (!sizeTweenBegin_ || !sizeTweenEnd_)
        return currentSize_;
    const double t = curvedValue();
    const Size& a = *sizeTweenBegin_;
    const Size& b = *sizeTweenEnd_;
    return Size(a.width + (b.width - a.width) * t, a.height + (b.height - a.height) * t);
}

double RenderAnimatedSize::curvedValue() const {
    const double v = controller_.value();
    return curve_ ? curve_(v) : v;
}

void RenderAnimatedSize::onControllerTick() {
    // 上游的 `_animationListener`：`if (_controller.value != _lastValue)
    // markNeedsLayout();` —— 值没变（比如第一帧 elapsed=0）就不折腾。
    if (!lastValue_ || *lastValue_ != controller_.value())
        markNeedsLayout();
}

void RenderAnimatedSize::onControllerCompleted() {
    if (onEnd_)
        onEnd_();
}

// ── 四态布局（上游的 _layoutStart / _layoutStable / _layoutChanged /
//    _layoutUnstable）──────────────────────────────────────────────────────

void RenderAnimatedSize::layoutStart(RenderBox* boxChild) {
    sizeTweenBegin_ = sizeTweenEnd_ = boxChild->size();
    state_ = RenderAnimatedSizeState::Stable;
}

void RenderAnimatedSize::layoutStable(RenderBox* boxChild) {
    const Size childSize = boxChild->size();
    if (!sizeTweenEnd_ || *sizeTweenEnd_ != childSize) {
        // 子节点尺寸又变了 ⇒ 从「当前自身尺寸」补间到新的子尺寸，
        // 并把它记为一次性跳变（Changed）。begin 用 size 而不是 child.size，
        // 正是上游让动画「从视觉上接续」的关键。
        sizeTweenBegin_ = currentSize_;
        sizeTweenEnd_ = childSize;
        restartAnimation();
        state_ = RenderAnimatedSizeState::Changed;
        return;
    }
    if (controller_.value() >= 1.0) {
        // 已经正好落在终点：立刻把 tween 收成一点，省掉一次无谓的动画。
        sizeTweenBegin_ = sizeTweenEnd_ = childSize;
        return;
    }
    if (!controller_.isAnimating())
        controller_.forward();
}

void RenderAnimatedSize::layoutChanged(RenderBox* boxChild) {
    const Size childSize = boxChild->size();
    if (!sizeTweenEnd_ || *sizeTweenEnd_ != childSize) {
        // 上一段补间还没跑完，子节点又换了尺寸 ⇒ 判定为「不稳定」：
        // 不再补间，tween 直接对齐当前子尺寸（下一帧起紧跟）。
        sizeTweenBegin_ = sizeTweenEnd_ = childSize;
        restartAnimation();
        state_ = RenderAnimatedSizeState::Unstable;
        return;
    }
    state_ = RenderAnimatedSizeState::Stable;
    if (!controller_.isAnimating())
        controller_.forward();
}

void RenderAnimatedSize::layoutUnstable(RenderBox* boxChild) {
    const Size childSize = boxChild->size();
    if (!sizeTweenEnd_ || *sizeTweenEnd_ != childSize) {
        sizeTweenBegin_ = sizeTweenEnd_ = childSize;
        restartAnimation();
        return;  // 仍留在 Unstable
    }
    // 这一帧子尺寸没再变 ⇒ 回到 Stable，动画停在当前进度（不硬拉到 1.0），
    // 由 layoutStable 的「value==upperBound」分支收尾。
    controller_.stop();
    state_ = RenderAnimatedSizeState::Stable;
}

// ── Layout / paint 协议 ─────────────────────────────────────────────────────

void RenderAnimatedSize::performLayout() {
    // 上游 的 performLayout 开头两行。
    lastValue_ = controller_.value();
    hasVisualOverflow_ = false;

    auto* boxChild = dynamic_cast<RenderBox*>(child());
    if (!boxChild || constraints_.isTight()) {
        // 上游：没有 RenderBox 子节点，或被父节点用紧约束钉死 ⇒ 没有补间
        // 的余地，动画直接停掉，尺寸恒为 smallest()，状态回到 Start。
        controller_.stop();
        currentSize_ = constraints_.smallest();
        sizeTweenBegin_ = sizeTweenEnd_ = currentSize_;
        setSize(currentSize_);
        state_ = RenderAnimatedSizeState::Start;
        if (child())
            child()->layout(constraints_);
        return;
    }

    // 上游把约束**原样**下发（不像 RenderAlign 那样 loosen）：本节点的尺寸
    // 由补间值决定，子节点必须在同一套约束里自证尺寸。
    boxChild->layout(constraints_, true);

    switch (state_) {
        case RenderAnimatedSizeState::Start:
            layoutStart(boxChild);
            break;
        case RenderAnimatedSizeState::Stable:
            layoutStable(boxChild);
            break;
        case RenderAnimatedSizeState::Changed:
            layoutChanged(boxChild);
            break;
        case RenderAnimatedSizeState::Unstable:
            layoutUnstable(boxChild);
            break;
    }

    currentSize_ = constraints_.constrain(animatedSize());
    setSize(currentSize_);

    // 上游 的 alignChild()（RenderAligningShiftedBox）：与本仓 RenderAlign
    // 同款公式，alignment ∈ [-1,1]，0 为居中。
    if (auto* pd = boxChild->parentData()) {
        const Size childSize = boxChild->size();
        pd->offset = Offset((size_.width - childSize.width) / 2.0 * (1.0 + alignment_.x),
                            (size_.height - childSize.height) / 2.0 * (1.0 + alignment_.y));
    }

    // 上游：补间终点比当前可见尺寸大 ⇒ 子节点会溢出，需要裁剪。
    if (sizeTweenEnd_ &&
        (size_.width < sizeTweenEnd_->width || size_.height < sizeTweenEnd_->height))
        hasVisualOverflow_ = true;
}

void RenderAnimatedSize::performPaint(PaintingContext& context, const Offset& offset) {
    // 上游 的 paint：只有「真有溢出」且「确实要裁」才裁。
    if (!child() || !hasVisualOverflow_ || clipBehavior_ == Clip::None ||
        !context.canvas()) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    // 用 CanvasScope 而不是裸 save/restore：子树里推图层会换录制画布，裸
    // restore 落在退役画布上，裁剪会泄漏到作用域之外。
    PaintingContext::CanvasScope scope(context);
    scope.clipRect(SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                                    SkScalar(size_.width), SkScalar(size_.height)),
                   clipBehavior_ != Clip::HardEdge);
    Offset childOffset = offset;
    if (child()->parentData())
        childOffset = childOffset + child()->parentData()->offset;
    context.paintChild(child(), childOffset);
}

}  // namespace cui_core
