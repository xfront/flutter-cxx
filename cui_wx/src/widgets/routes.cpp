/// @file widgets/routes.cpp
/// @brief 上游路由转场层的实现：OverlayRoute / TransitionRoute /
///        LocalHistoryEntry / ModalRoute / ModalScope / RawDialogRoute。
///
/// 对应 上游源码：
///   上游 widgets/routes.dart
///
/// 与上游的偏离逐条列在头文件（routes.h）里；这里只标注各函数对应的上游行号。

#include "cui/widgets/routes.h"

#include <algorithm>
#include <utility>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"

namespace cui {

// ── OverlayRoute ────────────────────────────────────────────────────────────

void OverlayRoute::install() {
    // 上游 OverlayRoute.install（routes.dart:83-90）：`_overlayEntries =
    // createOverlayEntries()`。本仓的 Route 直接持有 overlayEntries_
    // （navigator.h 结构性偏离），所以这里只是把工厂结果搬进去。
    overlayEntries_ = createOverlayEntries();
    Route::install();
}

void OverlayRoute::dispose() {
    // 上游 OverlayRoute.dispose（routes.dart:96-103）逐个调
    // `entry.dispose()`；本仓的 OverlayEntry 没有 dispose 方法（它的拆除由
    // Navigator 的 `removeRouteEntries` 完成），所以这里只放开引用。
    overlayEntries_.clear();
    Route::dispose();
}

// ── TransitionRoute ─────────────────────────────────────────────────────────

TransitionRoute::TransitionRoute(SharedPtr<const RouteSettings> settings)
    : OverlayRoute(std::move(settings)) {
    // 上游 的 `_secondaryAnimation = ProxyAnimation(kAlwaysDismissedAnimation)`。
    secondaryProxy_ = cui::make_shared<ProxyAnimation>(kAlwaysDismissedAnimation());
    secondaryAnimation_ = secondaryProxy_;
}

SharedPtr<AnimationController> TransitionRoute::createAnimationController() {
    // 上游 routes.dart:253-266：duration = transitionDuration、
    // reverseDuration = reverseTransitionDuration、vsync = navigator。
    auto controller = cui::make_shared<AnimationController>(transitionDuration());
    controller->setReverseDuration(reverseTransitionDuration());
    return controller;
}

SharedPtr<Animation<double>> TransitionRoute::createAnimation() {
    // 上游 routes.dart:268-274 返回 `_controller.view`；本仓的
    // AnimationController 本身就是 Animation<double>（且默认曲线为线性，
    // 与 `view` 同口径），所以直接返回它。
    return controller_;
}

bool TransitionRoute::finishedWhenPopped() const {
    // 上游 routes.dart:181：`_controller.isDismissed && !_popFinalized`。
    return controller_ && controller_->isDismissed() && !popFinalized_;
}

bool TransitionRoute::settlesWhenPushed() const {
    // 上游 `didPush()` 返回的 TickerFuture 完成前，历史条目停在 pushing。
    return pushTransitionFinished_;
}

void TransitionRoute::handleTransitionStatus(AnimationStatus status) {
    // 上游 的 `_handleStatusChanged`（routes.dart:311-338）。
    switch (status) {
        case AnimationStatus::Completed:
            // 入场这一段走完了：条目从 pushing 转 idle（上游 由 TickerFuture
            // 的 whenCompleteOrCancel 做，本仓改成显式调一次）。
            pushTransitionFinished_ = true;
            if (!overlayEntries_.empty() && overlayEntries_.front())
                overlayEntries_.front()->setOpaque(opaque());
            if (navigator())
                navigator()->didFinishPushTransition(this);
            break;
        case AnimationStatus::Forward:
        case AnimationStatus::Reverse:
            if (!overlayEntries_.empty() && overlayEntries_.front())
                overlayEntries_.front()->setOpaque(false);
            break;
        case AnimationStatus::Dismissed:
            // 「已到场外」但路由仍在场时**不** finalize：iOS 侧滑那类手势会
            // 先把动画拖到下界，之后才真正 pop（上游 同一条注释）。
            if (navigator() && !isActive()) {
                navigator()->finalizeRoute(this);
                popFinalized_ = true;
            }
            break;
    }
}

void TransitionRoute::install() {
    // 上游 routes.dart:348-357。
    controller_ = createAnimationController();
    animation_ = createAnimation();
    if (animation_)
        statusHandle_ = animation_->addStatusListener(
                [this](AnimationStatus status) { handleTransitionStatus(status); });
    OverlayRoute::install();
    if (animation_ && animation_->isCompleted() && !overlayEntries_.empty() &&
        overlayEntries_.front()) {
        overlayEntries_.front()->setOpaque(opaque());
    }

    // 上游 的 Ticker 由 Navigator（TickerProvider）在造 controller 时创建；
    // 本仓的 controller 不自带 Ticker（animation.h 偏离 2），故由路由自己持一条，
    // 并把环境 TickerMode 接上去（routes.h 偏离 11）。
    NavigatorState* nav = navigator();
    BuildContext* navContext = nav ? nav->context() : nullptr;
    if (navContext) {
        frameOwner_ = navContext->element() ? navContext->element()->owner() : nullptr;
        ticker_ = std::make_unique<Ticker>(
                [this](Duration elapsed) { onTransitionTick(elapsed); });
        tickerMode_.bind(*navContext, *ticker_);
    }
}

void TransitionRoute::didPush() {
    // 上游 routes.dart:339-351：`super.didPush()` 然后 `_controller.forward()`。
    Route::didPush();
    if (!controller_)
        return;
    controller_->forward();
    ensureTicking();
}

void TransitionRoute::didAdd() {
    // 上游 routes.dart:353-361：`_controller.value = _controller.upperBound`。
    if (controller_)
        controller_->setValue(controller_->upperBound());
}

void TransitionRoute::didReplace(Route* oldRoute) {
    // 上游 routes.dart:363-372：接上被替换那条动画的当前值。
    if (controller_) {
        if (auto* previous = dynamic_cast<TransitionRoute*>(oldRoute);
            previous && previous->controller_) {
            controller_->setValue(previous->controller_->value());
        }
    }
    Route::didReplace(oldRoute);
}

bool TransitionRoute::didPop(SharedPtr<void> result) {
    // 上游 routes.dart:374-388：记结果、`_controller.reverse()`、交回基类。
    result_ = result;
    if (controller_) {
        controller_->reverse();
        ensureTicking();
    }
    return Route::didPop(result);
}

void TransitionRoute::didPopNext(Route* nextRoute) {
    updateSecondaryAnimation(nextRoute);
    Route::didPopNext(nextRoute);
}

void TransitionRoute::didChangeNext(Route* nextRoute) {
    updateSecondaryAnimation(nextRoute);
    Route::didChangeNext(nextRoute);
}

void TransitionRoute::updateSecondaryAnimation(Route* nextRoute) {
    // 上游 routes.dart:427-491。「换车」（TrainHoppingAnimation）段未接
    // （routes.h 偏离 4）⇒ 两条动画值不同步时也直接切到新的一条。
    auto* next = dynamic_cast<TransitionRoute*>(nextRoute);
    if (next && next->animation_ && canTransitionTo(*next) &&
        next->canTransitionFrom(*this)) {
        setSecondaryAnimation(next->animation_, next);
        return;
    }
    setSecondaryAnimation(kAlwaysDismissedAnimation(), nullptr);
}

void TransitionRoute::setSecondaryAnimation(SharedPtr<Animation<double>> animation,
                                            const Route* sourceRoute) {
    // 上游 routes.dart:493-505。`disposed?.then(...)` 那半段改由本路由
    // dispose 时反向清理（routes.h 偏离 10）。
    if (!secondaryProxy_)
        return;
    secondaryProxy_->setParent(std::move(animation));
    secondarySource_ = sourceRoute;
}

void TransitionRoute::onTransitionTick(Duration elapsed) {
    // 在「本次 tick 里把自己 dispose 掉」的场景下，这条回调所在的
    // `std::function` 对象属于本路由 ⇒ 必须先把最后一份引用转交帧末
    // （routes.h 偏离 16）。
    SharedPtr<Route> keepAlive = shared_from_this();

    // Ticker 给的是「自本 ticker 上次 start() 起的绝对时间」，重启会回头；
    // controller 的 tickBaseMs_ 却记着上一次喂进去的值，所以这里累加出一条
    // **单调**的帧时间线喂给 controller（同 ImplicitlyAnimatedWidgetState）。
    const double now = elapsed.count();
    double dt = now - lastTickMs_;
    lastTickMs_ = now;
    if (dt < 0.0)
        dt = 0.0;
    tickTimelineMs_ += dt;

    if (controller_ && !controller_->tick(Duration(tickTimelineMs_)) && ticker_) {
        ticker_->stop();
        lastTickMs_ = 0.0;
    }
    if (disposed_)
        deferRelease(std::move(keepAlive));
}

void TransitionRoute::ensureTicking() {
    if (!ticker_ || !controller_ || !controller_->isAnimating())
        return;
    if (ticker_->isActive())
        return;
    // 重启 ⇒ Ticker 的 elapsed 归零，本路由的 delta 基准跟着归零（时间线不回头）。
    lastTickMs_ = 0.0;
    ticker_->start();
}

void TransitionRoute::deferRelease(SharedPtr<Route> self) {
    // 没有 BuildOwner 时（脱离树的脚手架）只能就地释放：那种场景下本路由
    // 也没起过 Ticker（install 里 navigator 的 context 为空 ⇒ ticker_ 为空），
    // 不存在「在自己的回调栈上析构自己」。
    if (!frameOwner_)
        return;
    BuildOwner* owner = frameOwner_;
    owner->addPostFrameCallback(
            [self = std::move(self)](Duration) mutable { self.reset(); });
}

void TransitionRoute::dispose() {
    if (disposed_)
        return;
    if (animation_ && statusHandle_ != 0) {
        animation_->removeStatusListener(statusHandle_);
        statusHandle_ = 0;
    }
    if (ticker_) {
        // 只停表、不 reset：`onTransitionTick` 可能正在栈上，
        // 拆掉那条 std::function 就是拆掉正在执行的代码。
        tickerMode_.unbind();
        if (ticker_->isActive())
            ticker_->stop();
    }
    // 偏离 10：把「转场源是自己」的兄弟路由的次级动画放回常量动画。
    // 必须**先**做这一步再去 dispose controller，否则对方会在下一帧读到
    // 已经拆听的动画。
    if (navigator()) {
        const std::vector<SharedPtr<Route>> history = navigator()->history();
        for (const SharedPtr<Route>& route : history) {
            auto* sibling = dynamic_cast<TransitionRoute*>(route.get());
            if (sibling && sibling->secondarySource_ == static_cast<const Route*>(this))
                sibling->setSecondaryAnimation(kAlwaysDismissedAnimation(), nullptr);
        }
    }
    if (secondaryProxy_)
        secondaryProxy_->setParent(kAlwaysDismissedAnimation());
    secondarySource_ = nullptr;

    if (willDisposeAnimationController_ && controller_)
        controller_->disposeController();

    transitionCompleted_ = true;
    disposed_ = true;
    std::vector<std::function<void(SharedPtr<void>)>> handlers;
    handlers.swap(completedHandlers_);
    for (const auto& handler : handlers) {
        if (handler)
            handler(result_);
    }
    OverlayRoute::dispose();
}

// ── LocalHistoryEntry ───────────────────────────────────────────────────────

void LocalHistoryEntry::remove() {
    // 上游 routes.dart:730-733。
    if (owner_)
        owner_->removeLocalHistoryEntry(this);
}

// ── ModalScope / ModalScopeState ────────────────────────────────────────────

std::unique_ptr<State> ModalScope::createState() {
    return std::make_unique<ModalScopeState>();
}

void ModalScopeState::initState() {
    State::initState();
    auto* scope = static_cast<const ModalScope*>(widget());
    if (scope && scope->route())
        scope->route()->attachScope(this);
    subscribe();
}

void ModalScopeState::subscribe() {
    // 上游 `_ModalScopeState.initState` 用 `Listenable.merge([animation,
    // secondaryAnimation])`；本仓直接向两条动画订阅（少一层，见 routes.h 的
    // 类注释），摘除一律走句柄通道。
    auto* scope = static_cast<const ModalScope*>(widget());
    ModalRoute* route = scope ? scope->route() : nullptr;
    if (!route)
        return;
    animationSub_ = route->animation();
    secondarySub_ = route->secondaryAnimation();
    if (animationSub_)
        animationHandle_ = animationSub_->addListenerWithHandle([this] { setState(); });
    if (secondarySub_)
        secondaryHandle_ = secondarySub_->addListenerWithHandle([this] { setState(); });
}

void ModalScopeState::unsubscribe() {
    if (animationSub_ && animationHandle_ != 0)
        animationSub_->removeListenerByHandle(animationHandle_);
    if (secondarySub_ && secondaryHandle_ != 0)
        secondarySub_->removeListenerByHandle(secondaryHandle_);
    animationHandle_ = 0;
    secondaryHandle_ = 0;
    animationSub_.reset();
    secondarySub_.reset();
}

void ModalScopeState::didChangeDependencies() {
    State::didChangeDependencies();
    // 上游 routes.dart:1108-1113：page 缓存作废。
    pageCache_.reset();
}

void ModalScopeState::dispose() {
    unsubscribe();
    auto* scope = static_cast<const ModalScope*>(widget());
    if (scope && scope->route())
        scope->route()->detachScope(this);
    subtreeContext_ = nullptr;
    State::dispose();
}

void ModalScopeState::forceRebuildPage() {
    // 上游 routes.dart:1126-1129。
    setState([this] { pageCache_.reset(); });
}

void ModalScopeState::routeSetState(std::function<void()> fn) {
    // 上游 routes.dart:1144-1150 的 `_routeSetState`：本仓无 FocusScope，
    // 剩下就是 setState（焦点抢占那半段见 routes.h 偏离 7）。
    setState(std::move(fn));
}

bool ModalScopeState::shouldIgnoreFocusRequest() const {
    // 上游 routes.dart:1132-1135。
    auto* scope = static_cast<const ModalScope*>(widget());
    ModalRoute* route = scope ? scope->route() : nullptr;
    if (!route)
        return false;
    const SharedPtr<Animation<double>> animation = route->animation();
    if (animation && animation->status() == AnimationStatus::Reverse)
        return true;
    return route->navigator() && route->navigator()->userGestureInProgress();
}

SharedPtr<Widget> ModalScopeState::build(BuildContext& context) {
    // 上游 routes.dart:1162-1225。省略的五层包装（RestorationScope /
    // PageStorage / PrimaryScrollController / FocusScope / RepaintBoundary）
    // 与 AnimatedBuilder(restorationScopeId) 见 routes.h 偏离 7。
    auto* scope = static_cast<const ModalScope*>(widget());
    ModalRoute* route = scope ? scope->route() : nullptr;
    if (!route)
        return nullptr;

    const SharedPtr<Animation<double>> animation = route->animation();
    const SharedPtr<Animation<double>> secondaryAnimation = route->secondaryAnimation();

    if (!pageCache_) {
        // 上游 `_page ??= RepaintBoundary(key: _subtreeKey, child: Builder(
        //   builder: route.buildPage))`：缓存的是**控件实例**，因此
        // `buildPage` 只在依赖变化时才重新调；而这条 Builder 的 Element
        // 在缓存换实例时被复用 ⇒ `subtreeContext_` 稳定。
        pageCache_ = cui::make_shared<Builder>(
                [this, route, animation, secondaryAnimation](
                        BuildContext& pageContext) -> SharedPtr<Widget> {
                    subtreeContext_ = &pageContext;
                    return route->buildPage(pageContext, animation, secondaryAnimation);
                });
    }

    SharedPtr<Widget> child =
            cui::make_shared<IgnorePointer>(shouldIgnoreFocusRequest(), pageCache_);
    SharedPtr<Widget> transitions =
            route->buildTransitions(context, animation, secondaryAnimation,
                                    std::move(child));
    SharedPtr<Widget> offstaged =
            cui::make_shared<Offstage>(route->offstage(), std::move(transitions));
    return cui::make_shared<ModalScopeStatus>(
            route->isCurrent(), route->canPop(), route->impliesAppBarDismissal(),
            route->opaque(), route, std::move(offstaged));
}

// ── ModalRoute ──────────────────────────────────────────────────────────────

ModalRoute::ModalRoute(SharedPtr<const RouteSettings> settings)
    : TransitionRoute(std::move(settings)) {}

SharedPtr<Animation<double>> ModalRoute::animation() const {
    return animationProxy_;
}

SharedPtr<Animation<double>> ModalRoute::secondaryAnimation() const {
    return secondaryProxyWrapper_;
}

void ModalRoute::install() {
    // 上游 routes.dart:1683-1688。
    TransitionRoute::install();
    animationProxy_ = cui::make_shared<ProxyAnimation>(TransitionRoute::animation());
    secondaryProxyWrapper_ =
            cui::make_shared<ProxyAnimation>(TransitionRoute::secondaryAnimation());
}

bool ModalRoute::didPop(SharedPtr<void> result) {
    // 上游 LocalHistoryRoute.didPop（routes.dart:931-949）：有本地历史时
    // **就地消化**这次 pop（返回 false ⇒ 条目回到 idle，路由留在栈上）。
    if (!localHistory_.empty()) {
        SharedPtr<LocalHistoryEntry> entry = localHistory_.back();
        localHistory_.pop_back();
        LocalHistoryEntry* raw = entry.get();
        raw->owner_ = nullptr;
        raw->notifyRemoved();
        bool internalStateChanged = false;
        if (raw->impliesAppBarDismissal()) {
            --entriesImpliesAppBarDismissal_;
            internalStateChanged = entriesImpliesAppBarDismissal_ == 0;
        }
        if (localHistory_.empty() || internalStateChanged)
            changedInternalState();
        return false;
    }
    return TransitionRoute::didPop(result);
}

bool ModalRoute::willHandlePopInternally() const {
    // 上游 LocalHistoryRoute.willHandlePopInternally（routes.dart:951-954）。
    return !localHistory_.empty();
}

RoutePopDisposition ModalRoute::popDisposition() const {
    // 上游 LocalHistoryRoute.popDisposition（routes.dart:925-930）。
    if (willHandlePopInternally())
        return RoutePopDisposition::pop;
    return Route::popDisposition();
}

bool ModalRoute::canPop() const {
    // 上游 routes.dart:2250。
    return hasActiveRouteBelow() || willHandlePopInternally();
}

bool ModalRoute::impliesAppBarDismissal() const {
    // 上游 routes.dart:2258。
    return hasActiveRouteBelow() || entriesImpliesAppBarDismissal_ > 0;
}

void ModalRoute::setOffstage(bool value) {
    // 上游 routes.dart:1951-1964。
    if (offstage_ == value)
        return;
    setState([this, value] { offstage_ = value; });
    if (animationProxy_) {
        animationProxy_->setParent(offstage_ ? kAlwaysCompleteAnimation()
                                             : TransitionRoute::animation());
    }
    if (secondaryProxyWrapper_) {
        secondaryProxyWrapper_->setParent(
                offstage_ ? kAlwaysDismissedAnimation()
                          : TransitionRoute::secondaryAnimation());
    }
    changedInternalState();
}

BuildContext* ModalRoute::subtreeContext() const {
    // 上游 routes.dart:1967：`_subtreeKey.currentContext`。
    return scopeState_ ? scopeState_->subtreeContext() : nullptr;
}

void ModalRoute::setState(std::function<void()> fn) {
    // 上游 routes.dart:1406-1416。
    if (scopeState_)
        scopeState_->routeSetState(std::move(fn));
    else if (fn)
        fn();
}

SharedPtr<Widget> ModalRoute::buildTransitions(BuildContext&, SharedPtr<Animation<double>>,
                                               SharedPtr<Animation<double>>,
                                               SharedPtr<Widget> child) {
    // 上游 routes.dart:1594 的默认实现：直接返回 child。
    return child;
}

SharedPtr<Widget> ModalRoute::generateModalBarrier() {
    // 上游 `_buildModalBarrier`（routes.dart:2273-2290）。
    // `filter`（BackdropFilter）与 `Semantics(sortKey:)` 未接（偏离 12 / 6）。
    SharedPtr<Widget> barrier = buildModalBarrier();
    const SharedPtr<Animation<double>> animation = animationProxy_;
    const bool ignoring = !animation || !animation->isForwardOrCompleted();
    return cui::make_shared<IgnorePointer>(ignoring, std::move(barrier));
}

SharedPtr<Widget> ModalRoute::buildModalBarrier() {
    // 上游 routes.dart:2298-2330。
    const std::optional<Color> color = barrierColor();
    if (color && color->a != 0.0 && !offstage_) {
        // `ColorTween(begin: barrierColor.withOpacity(0.0), end: barrierColor)
        //  .chain(CurveTween(curve: barrierCurve))` —— 本仓没有 drive/chain，
        // 用 CurvedAnimation 作被驱动的那条动画，效果同形。
        auto curved =
                cui::make_shared<CurvedAnimation>(animation(), barrierCurve());
        Tween<Color> tween(colorWithOpacity(*color, 0.0), *color);
        SharedPtr<Animation<Color>> colorAnimation = tween.animate(curved);
        return cui::make_shared<AnimatedModalBarrier>(std::move(colorAnimation),
                                                     barrierDismissible());
    }
    return cui::make_shared<ModalBarrier>(std::nullopt, barrierDismissible());
}

std::vector<SharedPtr<OverlayEntry>> ModalRoute::createOverlayEntries() {
    // 上游 routes.dart:2350-2361：`[_modalBarrier, _modalScope]`（barrier 在
    // 下、scope 在上）。`canSizeOverlay` 未接（偏离 13），`Semantics(sortKey)`
    // 未接（偏离 6）。
    std::vector<SharedPtr<OverlayEntry>> entries;
    auto barrier = cui::make_shared<OverlayEntry>(
            [this](BuildContext&) { return generateModalBarrier(); });
    auto scope = cui::make_shared<OverlayEntry>(
            [this](BuildContext&) { return generateModalScope(); }, /*opaque=*/false,
            maintainState());
    modalBarrierEntry_ = barrier.get();
    modalScopeEntry_ = scope.get();
    entries.push_back(std::move(barrier));
    entries.push_back(std::move(scope));
    return entries;
}

SharedPtr<Widget> ModalRoute::generateModalScope() {
    // 上游 `_buildModalScope`（routes.dart:2335-2347）的 `_modalScopeCache`。
    if (!scopeCache_)
        scopeCache_ = cui::make_shared<ModalScope>(this);
    return scopeCache_;
}

void ModalRoute::attachScope(ModalScopeState* state) {
    scopeState_ = state;
}

void ModalRoute::detachScope(ModalScopeState* state) {
    if (scopeState_ == state)
        scopeState_ = nullptr;
}

void ModalRoute::addLocalHistoryEntry(const SharedPtr<LocalHistoryEntry>& entry) {
    // 上游 LocalHistoryRoute.addLocalHistoryEntry（routes.dart:852-869）。
    if (!entry || entry->owner_)
        return;
    entry->owner_ = this;
    const bool wasEmpty = localHistory_.empty();
    localHistory_.push_back(entry);
    bool internalStateChanged = false;
    if (entry->impliesAppBarDismissal()) {
        internalStateChanged = entriesImpliesAppBarDismissal_ == 0;
        ++entriesImpliesAppBarDismissal_;
    }
    if (wasEmpty || internalStateChanged)
        changedInternalState();
}

void ModalRoute::removeLocalHistoryEntry(LocalHistoryEntry* entry) {
    // 上游 LocalHistoryRoute.removeLocalHistoryEntry（routes.dart:876-905）。
    if (!entry)
        return;
    auto it = std::find_if(localHistory_.begin(), localHistory_.end(),
                           [entry](const SharedPtr<LocalHistoryEntry>& item) {
                               return item.get() == entry;
                           });
    if (it == localHistory_.end())
        return;
    localHistory_.erase(it);
    bool internalStateChanged = false;
    if (entry->impliesAppBarDismissal()) {
        --entriesImpliesAppBarDismissal_;
        internalStateChanged = entriesImpliesAppBarDismissal_ == 0;
    }
    entry->owner_ = nullptr;
    entry->notifyRemoved();
    if (!localHistory_.empty() && !internalStateChanged)
        return;
    // 上游：构建期（persistentCallbacks）不能当场通知——本地历史可能是
    // finalizeTree 拆除不活跃 Element 时被摘掉的；推到帧末。
    BuildOwner* owner = frameOwner_;
    if (owner && owner->inBuildPhase()) {
        owner->addPostFrameCallback([this](Duration) {
            if (isActive())
                changedInternalState();
        });
        return;
    }
    changedInternalState();
}

void ModalRoute::didChangePrevious(Route* previousRoute) {
    // 上游 routes.dart:2187-2191。
    Route::didChangePrevious(previousRoute);
    changedInternalState();
}

void ModalRoute::didChangeNext(Route* nextRoute) {
    // 上游 routes.dart:2193-2203（`delegatedTransition` 未接，偏离见头文件）。
    TransitionRoute::didChangeNext(nextRoute);
    changedInternalState();
}

void ModalRoute::didPopNext(Route* nextRoute) {
    // 上游 routes.dart:2205-2216（NavigationNotification 未接：偏离 6）。
    TransitionRoute::didPopNext(nextRoute);
    changedInternalState();
}

void ModalRoute::handleTransitionStatus(AnimationStatus status) {
    TransitionRoute::handleTransitionStatus(status);
    // 上游 靠 `_animationProxy` 的注释「changedInternalState is called when
    // animation.status updates」：barrier 外层 IgnorePointer 的忽略位要在状态
    // 迁移时重算。dismissed 之后本路由可能已经 dispose，不再通知。
    if (!disposed_)
        changedInternalState();
}

void ModalRoute::changedInternalState() {
    // 上游 routes.dart:2221-2232。
    Route::changedInternalState();
    if (disposed_)
        return;
    BuildOwner* owner = frameOwner_;
    if (!owner || !owner->inBuildPhase()) {
        setState([] {});
        if (modalBarrierEntry_)
            modalBarrierEntry_->markNeedsBuild();
    }
    if (modalScopeEntry_)
        modalScopeEntry_->setMaintainState(maintainState());
}

void ModalRoute::changedExternalState() {
    // 上游 routes.dart:2234-2241。
    Route::changedExternalState();
    if (disposed_)
        return;
    if (modalBarrierEntry_)
        modalBarrierEntry_->markNeedsBuild();
    if (scopeState_)
        scopeState_->forceRebuildPage();
}

void ModalRoute::dispose() {
    // 两条 Overlay 条目马上要随基类一起放开引用，裸指针先失效掉，免得
    // changedInternalState / changedExternalState 之后打到悬挂指针。
    modalBarrierEntry_ = nullptr;
    modalScopeEntry_ = nullptr;
    scopeCache_.reset();
    TransitionRoute::dispose();
}

ModalRoute* ModalRoute::maybeOf(BuildContext& context) {
    // 上游 `ModalRoute._of`（routes.dart:1316-1319）：`InheritedModel
    // .inheritFrom<_ModalScopeStatus>(context)?.route as ModalRoute`。
    auto* status = context.dependOnInheritedWidgetOfExactType<ModalScopeStatus>();
    if (!status)
        return nullptr;
    auto data = status->data();
    if (!data)
        return nullptr;
    return dynamic_cast<ModalRoute*>(data->route);
}

namespace {

/// `ModalScopeStatus` 的查询公共路径：不建立依赖时也要能读到（`settingsOf`
/// 之类上游 全部走 inheritFrom ⇒ 这里统一用 dependOn）。
ModalRoute* routeOf(BuildContext& context) {
    return ModalRoute::maybeOf(context);
}

}  // namespace

std::optional<bool> ModalRoute::isCurrentOf(BuildContext& context) {
    if (ModalRoute* route = routeOf(context))
        return route->isCurrent();
    return std::nullopt;
}

std::optional<bool> ModalRoute::canPopOf(BuildContext& context) {
    if (ModalRoute* route = routeOf(context))
        return route->canPop();
    return std::nullopt;
}

const RouteSettings* ModalRoute::settingsOf(BuildContext& context) {
    if (ModalRoute* route = routeOf(context))
        return &route->settings();
    return nullptr;
}

std::optional<bool> ModalRoute::isActiveOf(BuildContext& context) {
    if (ModalRoute* route = routeOf(context))
        return route->isActive();
    return std::nullopt;
}

std::optional<bool> ModalRoute::isFirstOf(BuildContext& context) {
    if (ModalRoute* route = routeOf(context))
        return route->isFirst();
    return std::nullopt;
}

std::optional<bool> ModalRoute::opaqueOf(BuildContext& context) {
    if (ModalRoute* route = routeOf(context))
        return route->opaque();
    return std::nullopt;
}

std::optional<RoutePopDisposition> ModalRoute::popDispositionOf(BuildContext& context) {
    if (ModalRoute* route = routeOf(context))
        return route->popDisposition();
    return std::nullopt;
}

std::function<bool(Route*)> ModalRoute::withName(const std::string& name) {
    // 上游 routes.dart:1424-1430。
    return [name](Route* route) -> bool {
        if (!route || route->willHandlePopInternally())
            return false;
        auto* modal = dynamic_cast<ModalRoute*>(route);
        return modal && modal->settings().name() == name;
    };
}

// ── RawDialogRoute ──────────────────────────────────────────────────────────

SharedPtr<Widget> RawDialogRoute::buildPage(BuildContext& context,
                                            SharedPtr<Animation<double>> animation,
                                            SharedPtr<Animation<double>> secondaryAnimation) {
    // 上游 routes.dart:2655-2666 的 `Semantics` 与 `DisplayFeatureSubScreen`
    // 未接（偏离 6 / 本仓无折叠屏概念）。
    if (!pageBuilder_)
        return nullptr;
    return pageBuilder_(context, animation, secondaryAnimation);
}

SharedPtr<Widget> RawDialogRoute::buildTransitions(
        BuildContext& context, SharedPtr<Animation<double>> animation,
        SharedPtr<Animation<double>> secondaryAnimation, SharedPtr<Widget> child) {
    // 上游 routes.dart:2668-2680：没给 transitionBuilder 时给一条淡入。
    if (!transitionBuilder_)
        return cui::make_shared<FadeTransition>(animation ? animation : secondaryAnimation,
                                                std::move(child));
    return transitionBuilder_(context, animation, secondaryAnimation, std::move(child));
}

}  // namespace cui_core
