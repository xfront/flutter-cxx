/// @file widgets/navigator.cpp
/// @brief Navigator / Route / Page / NavigatorObserver 实现。
///
/// 语义对齐 上游官方源码：
///   上游 widgets/navigator.dart

#include "cui/widgets/navigator.h"

#include <algorithm>
#include <stdexcept>

#include "cui/framework/element.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {

// ── Route ───────────────────────────────────────────────────────────────────

bool Route::isCurrent() const {
    // 上游的 Route.isCurrent（navigator.dart:585-598）：
    // 找到「最后一个在场条目」，看它是不是自己。
    if (!navigator_)
        return false;
    return navigator_->currentRoute() == this;
}

bool Route::isFirst() const {
    // 上游的 Route.isFirst（navigator.dart:602-612）。
    if (!navigator_)
        return false;
    return navigator_->firstRoute() == this;
}

bool Route::isActive() const {
    // 上游的 Route.isActive（navigator.dart:642）：历史里「在场」条目中包含自己。
    // 注意是**现算**在场判定，不读 history() 缓存：冲刷进行中时缓存是旧的，
    // 而 TransitionRoute 的 finishedWhenPopped / dismissed 分支要的就是即时值。
    if (!navigator_)
        return false;
    return navigator_->isRoutePresent(this);
}

bool NavigatorState::entryIsPresent(const RouteEntry& entry) {
    // 上游：add ≤ state ≤ remove。正在退场（popping）的条目**不在场**，
    // 但它仍留在历史里（Overlay 条目还在，动画还要放）。
    return rank(entry.state) >= rank(RouteLifecycle::add) &&
           rank(entry.state) <= rank(RouteLifecycle::remove);
}

bool NavigatorState::entryWillBePresent(const RouteEntry& entry) {
    // 上游：add ≤ state ≤ idle。
    return rank(entry.state) >= rank(RouteLifecycle::add) &&
           rank(entry.state) <= rank(RouteLifecycle::idle);
}

bool NavigatorState::entrySuitableForAnnouncement(const RouteEntry& entry) {
    // 上游：push ≤ state ≤ removing。
    return rank(entry.state) >= rank(RouteLifecycle::push) &&
           rank(entry.state) <= rank(RouteLifecycle::removing);
}

bool NavigatorState::entrySuitableForTransitionAnimation(const RouteEntry& entry) {
    // 上游：push ≤ state ≤ remove。
    return rank(entry.state) >= rank(RouteLifecycle::push) &&
           rank(entry.state) <= rank(RouteLifecycle::remove);
}

int NavigatorState::rank(RouteLifecycle state) { return static_cast<int>(state); }

bool Route::didPop(SharedPtr<void> result) {
    // 上游 Route.didPop = `didComplete(result); return true;`，
    // OverlayRoute.didPop 再在外面判 finishedWhenPopped 决定要不要当场
    // finalizeRoute（= 不做退场动画）。本仓的 Route 本身就是 overlay 路由，
    // 所以两层合成一条。
    didComplete(result);
    if (finishedWhenPopped() && navigator_)
        navigator_->finalizeRoute(this);
    return true;
}

void Route::didComplete(SharedPtr<void> result) {
    // 上游 在这里 `_popCompleter.complete(result ?? currentResult)`。Completer
    // 只生效一次，所以本仓用 popCompleted_ 把重复调用挡在门外（否则
    // whenPopped 会跑两遍 —— 旧实现正是 pop() 里补发一次的那种重复）。
    if (popCompleted_)
        return;
    popCompleted_ = true;
    notifyPopped(result);
}

bool Route::isLast() const {
    // 上游的 Route.isLast：自己是最后一个在场条目。
    if (!navigator_)
        return false;
    return navigator_->currentRoute() == this;
}

bool Route::hasActiveRouteBelow() const {
    // 上游的 Route.hasActiveRouteBelow（navigator.dart:617-631）：
    // `_getRouteBefore(index - 1, isPresentPredicate) != null`。
    if (!navigator_)
        return false;
    const long index = navigator_->entryIndexOf(this);
    if (index <= 0)
        return false;
    return navigator_->routeBefore(index - 1, NavigatorState::entryIsPresent) != nullptr;
}

bool Route::isPageBased() const {
    // 上游的 Route._isPageBased：settings 是 Page 的子类。
    if (!settings_)
        return false;
    return dynamic_cast<const Page*>(settings_.get()) != nullptr;
}

RoutePopDisposition Route::popDisposition() const {
    // 上游的 Route.popDisposition（navigator.dart:383-390）：
    //   1. page-based 且 page.canPop == false → doNotPop
    //   2. 是栈底路由 → bubble
    //   3. 否则 → pop
    if (isPageBased()) {
        auto* page = dynamic_cast<const Page*>(settings_.get());
        if (page && !page->canPop())
            return RoutePopDisposition::doNotPop;
    }
    return isFirst() ? RoutePopDisposition::bubble : RoutePopDisposition::pop;
}

// ── NavigatorState ──────────────────────────────────────────────────────────

Route* NavigatorState::currentRoute() const {
    // 上游的 _lastRouteEntryWhereOrNull(isPresentPredicate)：
    // 取历史中最后一个「在场」的路由。走 entries_ 现算，因为 routes_ 是缓存、
    // 只在冲刷末尾同步。
    const long i = lastEntryIndexWhere(entryIsPresent);
    return i >= 0 ? entries_[static_cast<size_t>(i)].route.get() : nullptr;
}

Route* NavigatorState::firstRoute() const {
    // 上游的 _firstRouteEntryWhereOrNull(isPresentPredicate)。
    for (const auto& entry : entries_) {
        if (entry.route && entryIsPresent(entry))
            return entry.route.get();
    }
    return nullptr;
}

bool NavigatorState::isRoutePresent(const Route* route) const {
    if (!route)
        return false;
    for (const auto& entry : entries_) {
        if (entry.route && entryIsPresent(entry) && entry.route.get() == route)
            return true;
    }
    return false;
}

long NavigatorState::lastEntryIndexWhere(bool (*predicate)(const RouteEntry&)) const {
    // 上游的 _lastRouteEntryWhereOrNull：自顶向下找第一条满足谓目的条目，返回
    // 下标；没有则 -1。
    for (long i = static_cast<long>(entries_.size()) - 1; i >= 0; --i) {
        const RouteEntry& entry = entries_[static_cast<size_t>(i)];
        if (entry.route && predicate(entry))
            return i;
    }
    return -1;
}

Route* NavigatorState::routeBefore(long index,
                                   bool (*predicate)(const RouteEntry&)) const {
    // 上游的 _getRouteBefore：从 index（含）向下找第一条满足谓目的路由。
    for (long i = index; i >= 0; --i) {
        if (i >= static_cast<long>(entries_.size()))
            continue;
        const RouteEntry& entry = entries_[static_cast<size_t>(i)];
        if (entry.route && predicate(entry))
            return entry.route.get();
    }
    return nullptr;
}

Route* NavigatorState::routeAfter(long index,
                                  bool (*predicate)(const RouteEntry&)) const {
    // 上游的 _getRouteAfter：从 index 的下一条向上找。
    const long size = static_cast<long>(entries_.size());
    for (long i = index + 1; i < size; ++i) {
        const RouteEntry& entry = entries_[static_cast<size_t>(i)];
        if (entry.route && predicate(entry))
            return entry.route.get();
    }
    return nullptr;
}

void NavigatorState::syncPresentRoutes() {
    // 上游没有这份缓存（每次用 isPresentPredicate 过滤 _history）；本仓的
    // history() 要返回引用，所以在每次冲刷末尾重算一次。
    routes_.clear();
    for (const auto& entry : entries_) {
        if (entry.route && entryIsPresent(entry))
            routes_.push_back(entry.route);
    }
}

OverlayState* NavigatorState::overlayState() const {
    // 上游的 NavigatorState.overlay（navigator.dart:4136）：
    // 通过 _overlayKey 拿到内部 Overlay 的 State。
    if (!overlayKey_)
        return nullptr;
    Element* element = overlayKey_->currentElement<Element>();
    if (!element)
        return nullptr;
    auto* stateful = dynamic_cast<StatefulElement*>(element);
    if (!stateful || !stateful->state())
        return nullptr;
    return dynamic_cast<OverlayState*>(stateful->state());
}

std::vector<SharedPtr<OverlayEntry>> NavigatorState::allRouteOverlayEntries() const {
    // 上游的 _allRouteOverlayEntries（navigator.dart:4138-4140）：
    // 按历史顺序把每条路由的 Overlay 条目串起来。注意遍历的是**整个历史**，
    // 正在做退场动画（popping）的路由也在其中 —— 它的条目还没在场，但画面
    // 上还留着，重排时必须带上。
    std::vector<SharedPtr<OverlayEntry>> result;
    for (const auto& entry : entries_) {
        if (!entry.route)
            continue;
        for (const auto& overlayEntry : entry.route->overlayEntries())
            result.push_back(overlayEntry);
    }
    return result;
}

long NavigatorState::entryIndexOf(const Route* route) const {
    // 上游的 _history.indexWhere(_RouteEntry.isRoutePredicate(route))。
    for (long i = 0; i < static_cast<long>(entries_.size()); ++i) {
        if (entries_[static_cast<size_t>(i)].route.get() == route)
            return i;
    }
    return -1;
}

void NavigatorState::initState() {
    State::initState();

    auto* navigator = dynamic_cast<Navigator*>(widget());
    if (!navigator)
        return;

    onGenerateRoute_ = navigator->onGenerateRoute();
    onUnknownRoute_ = navigator->onUnknownRoute();
    onPopPage_ = navigator->onPopPage();
    observers_ = navigator->observers();
    pages_ = navigator->pages();
    overlayKey_ = cui::make_shared<GlobalKey>();
    gestureNotifier_ = cui::make_shared<ValueNotifier<bool>>(false);

    if (declarativeMode()) {
        // 声明式模式：pages 直接映射成路由栈。
        updatePages(pages_);
    } else {
        setupInitialRoutes();
    }
}

void NavigatorState::didChangeDependencies() {
    // 上游的 NavigatorState.didChangeDependencies（navigator.dart:3958-3967）：
    // 依赖（例如 MaterialApp 的 Theme）变了，逐条告诉路由「可以重算 barrier
    // 颜色这类要用 navigator.context 查的东西了」。
    State::didChangeDependencies();
    for (auto& entry : entries_) {
        if (entry.route && entry.route->navigator() == this)
            entry.route->changedExternalState();
    }
}

void NavigatorState::dispose() {
    // 上游的 NavigatorState.dispose（navigator.dart:3939-3962）：Navigator 自己
    // 被摘掉时，历史里剩下的条目一律 forcedDispose（Overlay 由父链负责，这里
    // 只负责路由对象），否则路由的 dispose 永远不会跑。
    State::dispose();
    for (auto& entry : entries_) {
        if (!entry.route)
            continue;
        entry.route->dispose();
        entry.route->navigator_ = nullptr;
    }
    entries_.clear();
    routes_.clear();
    observedAdditions_.clear();
    observedDeletions_.clear();
    lastTopmostRoute_ = nullptr;
    if (gestureNotifier_) {
        gestureNotifier_->setValue(false);
        gestureNotifier_->dispose();
    }
}

void NavigatorState::setupInitialRoutes() {
    // 上游的 NavigatorState 对命令式模式的处理：
    // 初始路由由 initialRoute（默认 "/"）经 onGenerateRoute 生成；
    // 没有 onGenerateRoute 时生成一个「空白路由」。
    auto* navigator = dynamic_cast<Navigator*>(widget());
    std::string routeName = "/";
    if (navigator && !navigator->initialRoute().empty())
        routeName = navigator->initialRoute();

    auto route = generateRoute(routeName, nullptr);
    if (!route)
        return;

    RouteEntry entry;
    entry.route = route;
    entry.state = RouteLifecycle::add;
    entries_.push_back(entry);

    flushHistoryUpdates();
}

SharedPtr<Route> NavigatorState::generateRoute(const std::string& routeName,
                                                      SharedPtr<void> arguments) {
    // 上游的 _routeNamed / Navigator._generateRoute：
    // 先试 onGenerateRoute，再试 onUnknownRoute，都没有则报错。
    RouteSettings settings(routeName, std::move(arguments));
    if (onGenerateRoute_) {
        if (auto route = onGenerateRoute_(settings))
            return route;
    }
    if (onUnknownRoute_) {
        if (auto route = onUnknownRoute_(settings))
            return route;
    }
    return nullptr;
}

void NavigatorState::updatePages(const std::vector<SharedPtr<Page>>& pages) {
    // 上游的 NavigatorState._updatePages（navigator.dart:4148-4449）。
    //
    // 上游 把这件事拆成「自底向上扫一遍 → 中间段按 page.key 认领 → 自顶向下
    // 扫一遍」，剩下的空白交给 `TransitionDelegate` 决定每条路由怎么进场/退场
    // （`markForPush` / `markForPop` / `markForReplace`）。本仓还没有 transition
    // delegate（这是有意的偏离，见 navigator.h 文件头），所以按「能认领就原地
    // 复用、不能就新建 `add`、没被认领的在场 page 路由标记 `pop`」做一遍，
    // 进场/退场都走状态机本身。
    //
    // 认领条件与上游 `canUpdateFrom`（navigator.dart:3234-3243）同口径：
    // 条目必须还 `willBePresent`（正在退场的不能复用）、必须是 page-based、
    // 且 `Page::canUpdate`（同类型 + 同 key）成立。
    std::vector<RouteEntry> rebuilt;
    rebuilt.reserve(pages.size() + entries_.size());
    std::vector<bool> claimed(entries_.size(), false);

    for (const auto& page : pages) {
        if (!page)
            continue;
        long found = -1;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (claimed[i])
                continue;
            const RouteEntry& candidate = entries_[i];
            if (!candidate.route || !candidate.route->isPageBased() ||
                !entryWillBePresent(candidate))
                continue;
            auto* existing = dynamic_cast<const Page*>(&candidate.route->settings());
            if (existing && page->canUpdate(*existing)) {
                found = static_cast<long>(i);
                break;
            }
        }
        if (found >= 0) {
            claimed[static_cast<size_t>(found)] = true;
            rebuilt.push_back(entries_[static_cast<size_t>(found)]);
            continue;
        }
        auto route = page->createRoute(*context());
        if (!route)
            continue;
        RouteEntry entry;
        entry.route = route;
        entry.state = RouteLifecycle::add;
        rebuilt.push_back(std::move(entry));
    }

    // 没被认领的条目：在场的 page 路由标记为 pop（imperativeRemoval = false，
    // 因为这不是 `Navigator::pop` 触发的，是 pages 变短了）；pageless 路由与
    // 已经在退场的条目保持原样追加在后面（上游 的 `_updatePages` 把过渡中的
    // page 条目留在原位，本仓简化成「追加」，两者只在「中间页被删掉且它上面
    // 还压着 pageless 路由」这种组合下有差异）。
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (claimed[i])
            continue;
        RouteEntry& candidate = entries_[i];
        if (candidate.route && candidate.route->isPageBased() &&
            entryWillBePresent(candidate)) {
            candidate.state = RouteLifecycle::pop;
            candidate.pendingResult = nullptr;
            candidate.imperativeRemoval = false;
        }
        rebuilt.push_back(candidate);
    }

    entries_ = std::move(rebuilt);
    pages_ = pages;
    flushHistoryUpdates();
}

// ── 上游 _RouteEntry 的状态迁移助手 ─────────────────────────────────────────
//
// 对应 上游 navigator.dart:3245-3444 的 handleAdd / didAdd / handlePush /
// handleDidPopNext / handlePop / handleComplete / handleRemoval。
// 上游 把这些方法挂在 _RouteEntry 上（它自己持有 currentState）；本仓的状态存在
// RouteEntry 里、由 NavigatorState 迁移，所以这些助手收在 NavigatorState 上。

void NavigatorState::handleAdd(RouteEntry& entry, Route* previousPresent) {
    // 上游的 _RouteEntry.handleAdd（navigator.dart:3245-3250）：只登记观测、把
    // 状态推到 adding —— install 要到 case adding（且上面不再挡着）才发生。
    entry.state = RouteLifecycle::adding;
    observedAdditions_.push_back(
            {ObserverObservation::Kind::push, entry.route.get(), previousPresent});
}

void NavigatorState::handleDidAdd(RouteEntry& entry, RouteEntry* next) {
    // 上游的 _RouteEntry.didAdd（navigator.dart:3406-3416）。
    entry.route->navigator_ = this;
    entry.route->install();
    entry.route->didAdd();
    entry.state = RouteLifecycle::idle;
    if (next == nullptr)
        entry.route->didChangeNext(nullptr);
}

void NavigatorState::handlePush(RouteEntry& entry, Route* previous,
                                Route* previousPresent, RouteEntry* next) {
    // 上游的 _RouteEntry.handlePush（navigator.dart:3252-3310）。
    const bool wasPush = entry.state == RouteLifecycle::push ||
                         entry.state == RouteLifecycle::pushReplace;
    entry.route->navigator_ = this;
    entry.route->install();
    if (wasPush) {
        // 上游：`final TickerFuture routeFuture = route.didPush(); currentState =
        // pushing; routeFuture.whenCompleteOrCancel(() → idle + 再冲刷)`。本仓
        // 没有 future，改由路由自己在入场动画结束时调
        // NavigatorState::didFinishPushTransition（见 settlesWhenPushed 注释）。
        entry.route->didPush();
        entry.state = RouteLifecycle::pushing;
        if (entry.route->settlesWhenPushed())
            entry.state = RouteLifecycle::idle;
    } else {
        entry.route->didReplace(previous);
        entry.state = RouteLifecycle::idle;
    }
    if (next == nullptr)
        entry.route->didChangeNext(nullptr);
    // pushReplace 与 replace 一样向观察者报 didReplace（navigator.dart:3300-3309）。
    observedAdditions_.push_back(
            {wasPush ? ObserverObservation::Kind::push : ObserverObservation::Kind::replace,
             entry.route.get(), previousPresent});
}

void NavigatorState::handleDidPopNext(RouteEntry& entry, Route* poppedRoute) {
    // 上游的 _RouteEntry.handleDidPopNext（navigator.dart:3312-3314）。
    entry.route->didPopNext(poppedRoute);
    entry.lastAnnouncedPoppedNextRoute = poppedRoute;
}

bool NavigatorState::handlePop(RouteEntry& entry) {
    // 上游的 _RouteEntry.handlePop（navigator.dart:3357-3379）。
    Route* route = entry.route.get();
    // 「先置 popping，再问路由」：上游 就是这个顺序 —— Route.didPop 里可能
    // 立刻 finalizeRoute（无退场动画），也可能是 TransitionRoute 起退场动画，
    // 动画回调里判的是「我已经不在场了」。
    entry.state = RouteLifecycle::popping;
    if (route->popCompleted()) {
        // 上游：`if (route._popCompleter.isCompleted)` ⇒ 这是经 `Navigator::pop`
        // 的 onPopPage 弹掉的 page 路由，didPop 已经调过，不能再调一遍。
        return true;
    }
    if (!route->didPop(entry.pendingResult)) {
        entry.state = RouteLifecycle::idle;
        return false;
    }
    invokePopInvoked(route, true, entry.pendingResult);
    entry.pendingResult = nullptr;
    return true;
}

void NavigatorState::handleComplete(RouteEntry& entry) {
    // 上游的 _RouteEntry.handleComplete（navigator.dart:3381-3386）。
    entry.route->didComplete(entry.pendingResult);
    entry.pendingResult = nullptr;
    entry.state = RouteLifecycle::remove;
}

void NavigatorState::handleRemoval(RouteEntry& entry, Route* previousPresent) {
    // 上游的 _RouteEntry.handleRemoval（navigator.dart:3388-3404）。
    if (entry.route->installed() && entry.route->navigator() == this)
        entry.state = RouteLifecycle::removing;
    else
        entry.state = RouteLifecycle::dispose;
    if (entry.reportRemovalToObserver)
        observedDeletions_.push_back(
                {ObserverObservation::Kind::remove, entry.route.get(), previousPresent});
}

void NavigatorState::entryPop(RouteEntry& entry, SharedPtr<void> result,
                              bool imperativeRemoval) {
    // 上游的 _RouteEntry.pop（navigator.dart:3420-3425）。
    entry.pendingResult = std::move(result);
    entry.state = RouteLifecycle::pop;
    entry.imperativeRemoval = imperativeRemoval;
}

void NavigatorState::entryComplete(RouteEntry& entry, SharedPtr<void> result,
                                   bool isReplaced, bool imperativeRemoval) {
    // 上游的 _RouteEntry.complete（navigator.dart:3430-3439）：已经走过 remove
    // 的条目不再回退；被替换掉的那条不向观察者单独报 didRemove。
    if (rank(entry.state) >= rank(RouteLifecycle::remove))
        return;
    entry.reportRemovalToObserver = !isReplaced;
    entry.pendingResult = std::move(result);
    entry.state = RouteLifecycle::complete;
    entry.imperativeRemoval = imperativeRemoval;
}

void NavigatorState::invokePopInvoked(Route* route, bool didPop,
                                      SharedPtr<void> result) {
    // 上游的 Route.onPopInvokedWithResult（navigator.dart:411-415）：基类只对
    // page-based 路由把事件转发给 Page.onPopInvoked。本仓的 Route 没有这个虚
    // 钩子（Page 的回调是 std::function），所以在状态机里直接判类型。
    if (!route || !route->isPageBased())
        return;
    if (auto* page = dynamic_cast<const Page*>(&route->settings()))
        page->invokeOnPopInvoked(didPop, std::move(result));
}

void NavigatorState::flushHistoryUpdates(bool rearrangeOverlay) {
    // 上游的 _flushHistoryUpdates（navigator.dart:4451-4619）：**自顶向下单次
    // 行走** + 每条 case 用 `continue` 就地重跑。
    //
    // 移植时最容易搞错的一点：Dart 的 switch case **不贯穿**（非空 case 体末尾
    // 隐式 break）。上游 那段代码看起来像「pushing 掉进 idle 的体」，实际不是 ——
    // `case _RouteLifecycle.pushing` 只置 seenTopActiveRoute，不会把
    // canRemoveOrAdd 也置上（正在做入场动画的顶层**不该**允许下面偷偷移除，
    // 这正是要区别对待的两条）。所以下面每条 case 各自 break，只有上游 写了
    // `continue` 的地方才用 `again` 重新处理同一条目。
    //
    // 与上游 的一处结构性偏离：上游在 `case dispose` 里就 `_history.removeAt(index)`，
    // 本仓把擦除推到整轮走完之后（entries_ 是 vector，中途擦除会让 index 与
    // next/previous 全部错位）。擦除推迟不影响任何谓词：`dispose` 既不在
    // isPresent(add..remove) 里，也不在 suitableForAnnouncement(push..removing)
    // 或 suitableForTransitionAnimation(push..remove) 里；而 Overlay 重排在擦除
    // 之后才做，`_allRouteOverlayEntries` 看到的集合与上游 完全一致。
    flushingHistory_ = true;

    long index = static_cast<long>(entries_.size()) - 1;
    RouteEntry* entry = index >= 0 ? &entries_[static_cast<size_t>(index)] : nullptr;
    RouteEntry* next = nullptr;
    RouteEntry* previous = index > 0 ? &entries_[static_cast<size_t>(index - 1)] : nullptr;
    // 上面是否已经有「完全遮挡」的在场路由，可以静默增删下面的条目。
    bool canRemoveOrAdd = false;
    // 应该触发下面那条路由 didPopNext 的那条（上游 的 poppedRoute）。
    Route* poppedRoute = nullptr;
    // 是否已经走到过「新的顶层」（上游 的 seenTopActiveRoute）。
    bool seenTopActiveRoute = false;
    bool disposedSomething = false;

    while (index >= 0 && entry) {
        bool again = false;
        switch (entry->state) {
            case RouteLifecycle::add:
                handleAdd(*entry, routeBefore(index - 1, entryIsPresent));
                again = true;
                break;

            case RouteLifecycle::adding:
                if (canRemoveOrAdd || next == nullptr) {
                    handleDidAdd(*entry, next);
                    again = true;
                }
                break;

            case RouteLifecycle::push:
            case RouteLifecycle::pushReplace:
            case RouteLifecycle::replace:
                handlePush(*entry, previous ? previous->route.get() : nullptr,
                           routeBefore(index - 1, entryIsPresent), next);
                if (entry->state == RouteLifecycle::idle)
                    again = true;
                break;

            case RouteLifecycle::pushing:
                // 停在 pushing 等入场动画（上游 等 didPush 的 TickerFuture）。
                if (!seenTopActiveRoute && poppedRoute)
                    handleDidPopNext(*entry, poppedRoute);
                seenTopActiveRoute = true;
                break;

            case RouteLifecycle::idle:
                if (!seenTopActiveRoute && poppedRoute)
                    handleDidPopNext(*entry, poppedRoute);
                seenTopActiveRoute = true;
                // 这一条已经稳定 ⇒ 允许静默移除它下面（更早）的条目。
                canRemoveOrAdd = true;
                break;

            case RouteLifecycle::pop:
                if (!handlePop(*entry)) {
                    again = true;
                    break;
                }
                if (!seenTopActiveRoute) {
                    if (poppedRoute)
                        handleDidPopNext(*entry, poppedRoute);
                    poppedRoute = entry->route.get();
                }
                // 上游 在 pop 的**那一刻**就向观察者报 didPop（不是等到移除）。
                observedDeletions_.push_back({ObserverObservation::Kind::pop,
                                             entry->route.get(),
                                             routeBefore(index, entryWillBePresent)});
                if (entry->state == RouteLifecycle::dispose) {
                    // 弹出同步完成（转场时长为 0）⇒ 同一条目直接进 dispose 分支。
                    again = true;
                    break;
                }
                canRemoveOrAdd = true;
                break;

            case RouteLifecycle::popping:
                // 等路由自己的退场动画结束时调 finalizeRoute。
                break;

            case RouteLifecycle::complete:
                handleComplete(*entry);
                again = true;
                break;

            case RouteLifecycle::remove:
                // 还没 install 过的条目当作「从没上过 Navigator」，不宣告存在感
                // （上游 的 `entry.route._installed` 判定）。
                if (!seenTopActiveRoute && entry->route->installed()) {
                    if (poppedRoute)
                        handleDidPopNext(*entry, poppedRoute);
                    poppedRoute = nullptr;
                }
                handleRemoval(*entry, routeBefore(index, entryWillBePresent));
                again = true;
                break;

            case RouteLifecycle::removing:
                if (!canRemoveOrAdd && next != nullptr)
                    break;  // 上面还在动画，暂时不能销毁。
                entry->state = RouteLifecycle::dispose;
                again = true;
                break;

            case RouteLifecycle::dispose:
                // 上游：`toBeDisposed.add(_history.removeAt(index)); entry = next;`
                // —— 宣告（didChangeNext/Previous）发完之前不销毁。
                disposedSomething = true;
                break;

            case RouteLifecycle::disposing:
            case RouteLifecycle::disposed:
                // 上游 assert(false)：正常流程不会停留在这两个状态（本仓把
                // 「等 Overlay 条目先卸载」的 Graceful dispose 简化成了立即销毁，
                // 所以连这两个状态都不会真正进入）。
                break;
        }
        if (again)
            continue;

        --index;
        next = entry;
        entry = previous;
        previous = index > 0 ? &entries_[static_cast<size_t>(index - 1)] : nullptr;
    }

    flushObserverNotifications();
    flushRouteAnnouncement();

    // 上游的 didChangeTop（navigator.dart:4591-4597）。
    const long topIndex = lastEntryIndexWhere(entryIsPresent);
    SharedPtr<Route> newTop = topIndex >= 0
            ? entries_[static_cast<size_t>(topIndex)].route
            : nullptr;
    if (newTop && newTop != lastTopmostRoute_) {
        for (auto* observer : observers_) {
            if (observer)
                observer->didChangeTop(newTop.get(), lastTopmostRoute_.get());
        }
    }
    lastTopmostRoute_ = newTop;

    // 最后才真正摘掉 Overlay 条目并 dispose（上游 的
    // `for (entry in toBeDisposed) _disposeRouteEntry(entry, graceful: true)`）。
    if (disposedSomething) {
        std::vector<SharedPtr<Route>> doomed;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->route && it->state == RouteLifecycle::dispose) {
                doomed.push_back(it->route);
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& route : doomed) {
            RouteEntry temp;
            temp.route = route;
            disposeEntry(temp);
        }
    }

    syncPresentRoutes();

    if (rearrangeOverlay) {
        // 上游的 flushHistoryUpdates 末尾：overlay?.rearrange(_allRouteOverlayEntries)。
        if (OverlayState* overlay = overlayState())
            overlay->rearrange(allRouteOverlayEntries());
    }
    flushingHistory_ = false;
}

void NavigatorState::flushObserverNotifications() {
    // 上游的 _flushObserverNotifications（navigator.dart:4621-4636）：
    // 新增的观测**倒序**发（后压的先通知），删除的观测**正序**发。
    if (observers_.empty()) {
        observedAdditions_.clear();
        observedDeletions_.clear();
        return;
    }
    auto notify = [this](const ObserverObservation& observation) {
        for (auto* observer : observers_) {
            if (!observer)
                continue;
            switch (observation.kind) {
                case ObserverObservation::Kind::push:
                    observer->didPush(observation.route, observation.previous);
                    break;
                case ObserverObservation::Kind::replace:
                    observer->didReplace(observation.route, observation.previous);
                    break;
                case ObserverObservation::Kind::pop:
                    observer->didPop(observation.route, observation.previous);
                    break;
                case ObserverObservation::Kind::remove:
                    observer->didRemove(observation.route, observation.previous);
                    break;
            }
        }
    };
    while (!observedAdditions_.empty()) {
        ObserverObservation observation = observedAdditions_.back();
        observedAdditions_.pop_back();
        notify(observation);
    }
    while (!observedDeletions_.empty()) {
        ObserverObservation observation = observedDeletions_.front();
        observedDeletions_.erase(observedDeletions_.begin());
        notify(observation);
    }
}

void NavigatorState::flushRouteAnnouncement() {
    // 上游的 _flushRouteAnnouncement（navigator.dart:4638-4667）。
    for (long index = static_cast<long>(entries_.size()) - 1; index >= 0; --index) {
        RouteEntry& entry = entries_[static_cast<size_t>(index)];
        if (!entry.route || !entrySuitableForAnnouncement(entry))
            continue;

        Route* next = routeAfter(index, entrySuitableForTransitionAnimation);
        if (next != entry.lastAnnouncedNextRoute) {
            // 上游 shouldAnnounceChangeToNext（navigator.dart:3541-3546）：
            // 「next 从刚弹出的那条变成 null」不再宣告 —— didPopNext 已经说过了。
            const bool suppress =
                    next == nullptr &&
                    entry.lastAnnouncedPoppedNextRoute == entry.lastAnnouncedNextRoute;
            if (!suppress)
                entry.route->didChangeNext(next);
            entry.lastAnnouncedNextRoute = next;
        }
        Route* previous = routeBefore(index - 1, entrySuitableForTransitionAnimation);
        if (previous != entry.lastAnnouncedPreviousRoute) {
            entry.route->didChangePrevious(previous);
            entry.lastAnnouncedPreviousRoute = previous;
        }
    }
}

void NavigatorState::finalizeRoute(Route* route) {
    // 上游的 NavigatorState.finalizeRoute（navigator.dart:5798-5834）。
    if (!route)
        return;
    const long index = entryIndexOf(route);
    if (index < 0)
        return;
    RouteEntry& entry = entries_[static_cast<size_t>(index)];
    // 零转场的 page 路由可能在任何状态之前被 finalize：那种情况补一条 pop 观测。
    if (entry.route->isPageBased() && rank(entry.state) < rank(RouteLifecycle::pop)) {
        observedDeletions_.push_back({ObserverObservation::Kind::pop, route,
                                     routeBefore(index - 1, entryWillBePresent)});
    }
    // 上游的 entry.finalize()。
    entry.state = RouteLifecycle::dispose;
    // 冲刷期间（pop 同步完成）不能再递归冲刷一次。
    if (!flushingHistory_)
        flushHistoryUpdates(false);
}

void NavigatorState::didFinishPushTransition(Route* route) {
    // 上游 由 `Route.didPush()` 返回的 TickerFuture 承担（navigator.dart:3276-3290）。
    if (!route)
        return;
    const long index = entryIndexOf(route);
    if (index < 0)
        return;
    RouteEntry& entry = entries_[static_cast<size_t>(index)];
    if (entry.state != RouteLifecycle::pushing)
        return;
    entry.state = RouteLifecycle::idle;
    if (!flushingHistory_)
        flushHistoryUpdates();
}

void NavigatorState::didStartUserGesture() {
    // 上游的 NavigatorState.didStartUserGesture（navigator.dart:5866-5884）。
    ++userGesturesInProgress_;
    userGestureInProgress_ = userGesturesInProgress_ > 0;
    if (gestureNotifier_)
        gestureNotifier_->setValue(userGestureInProgress_);
    if (userGesturesInProgress_ != 1)
        return;

    long routeIndex = static_cast<long>(entries_.size()) - 1;
    while (routeIndex >= 0 && !entryWillBePresent(entries_[static_cast<size_t>(routeIndex)]))
        --routeIndex;
    if (routeIndex < 0)
        return;
    Route* route = entries_[static_cast<size_t>(routeIndex)].route.get();
    Route* previousRoute = nullptr;
    if (!route->willHandlePopInternally() && routeIndex > 0)
        previousRoute = routeBefore(routeIndex - 1, entryWillBePresent);
    for (auto* observer : observers_) {
        if (observer)
            observer->didStartUserGesture(route, previousRoute);
    }
}

void NavigatorState::didStopUserGesture() {
    // 上游的 NavigatorState.didStopUserGesture（navigator.dart:5888-5898）：
    // 计数不为 0 时调用是上游 的 assert，本仓夹到 0。
    if (userGesturesInProgress_ > 0)
        --userGesturesInProgress_;
    userGestureInProgress_ = userGesturesInProgress_ > 0;
    if (gestureNotifier_)
        gestureNotifier_->setValue(userGestureInProgress_);
    if (userGesturesInProgress_ == 0) {
        for (auto* observer : observers_) {
            if (observer)
                observer->didStopUserGesture();
        }
    }
}

void NavigatorState::disposeEntry(RouteEntry& entry) {
    // 上游的 _disposeRouteEntry（navigator.dart:3964-3975）：
    // 先把该路由的 Overlay 条目摘掉，再 dispose 路由。
    if (!entry.route)
        return;
    removeRouteEntries(entry.route);
    entry.route->dispose();
    entry.route->navigator_ = nullptr;
}

void NavigatorState::removeRouteEntries(const SharedPtr<Route>& route) {
    if (!route)
        return;
    for (const auto& entry : route->overlayEntries()) {
        if (entry)
            entry->remove();
    }
}

SharedPtr<Route> NavigatorState::push(const SharedPtr<Route>& route) {
    // 上游的 NavigatorState.push → _pushEntry（navigator.dart:5050-5125）：
    // 新条目以 push 状态压在历史末尾，剩下交给冲刷。
    if (!route)
        return nullptr;
    RouteEntry entry;
    entry.route = route;
    entry.state = RouteLifecycle::push;
    entries_.push_back(std::move(entry));
    flushHistoryUpdates();
    return route;
}

SharedPtr<Route> NavigatorState::pushReplacement(const SharedPtr<Route>& oldRoute,
                                                 const SharedPtr<Route>& newRoute) {
    // 上游的 pushReplacement → _pushReplacementEntry（navigator.dart:5245-5267）：
    //   被替换的那条 complete(result, isReplaced:true, imperativeRemoval:true)，
    //   新条目以 pushReplace 状态**追加**在末尾，然后冲刷。
    // `isReplaced:true` 的含义是不向观察者单独报 didRemove（改报 didReplace，
    // 由 handlePush 登记）。oldRoute 为空时按上游 取最后一个在场条目。
    if (!newRoute)
        return nullptr;
    long index = oldRoute ? entryIndexOf(oldRoute.get()) : -1;
    if (index < 0)
        index = lastEntryIndexWhere(entryIsPresent);
    if (index >= 0)
        entryComplete(entries_[static_cast<size_t>(index)], nullptr, /*isReplaced=*/true,
                      /*imperativeRemoval=*/true);
    RouteEntry entry;
    entry.route = newRoute;
    entry.state = RouteLifecycle::pushReplace;
    entries_.push_back(std::move(entry));
    flushHistoryUpdates();
    return newRoute;
}

SharedPtr<Route> NavigatorState::pushAndRemoveUntil(
        const SharedPtr<Route>& newRoute, std::function<bool(Route*)> predicate) {
    // 上游的 _pushEntryAndRemoveUntil（navigator.dart:5348-5370）：先追加新条目，
    // 再从它下面往下走，predicate 不成立的**在场**条目一律 complete
    // （isReplaced:false ⇒ 报 didRemove；不是 pop ⇒ 不报 didPop、也没有结果回传），
    // 走到第一条成立的就停。
    if (!newRoute)
        return nullptr;
    const long startIndex = static_cast<long>(entries_.size()) - 1;
    RouteEntry entry;
    entry.route = newRoute;
    entry.state = RouteLifecycle::push;
    entries_.push_back(std::move(entry));
    for (long i = startIndex; i >= 0; --i) {
        RouteEntry& candidate = entries_[static_cast<size_t>(i)];
        if (!candidate.route)
            continue;
        if (predicate && predicate(candidate.route.get()))
            break;
        if (entryIsPresent(candidate))
            entryComplete(candidate, nullptr, /*isReplaced=*/false, /*imperativeRemoval=*/true);
    }
    flushHistoryUpdates();
    return newRoute;
}

void NavigatorState::pop(SharedPtr<void> result) {
    // 上游的 NavigatorState.pop（navigator.dart:5642-5672）。
    const long index = lastEntryIndexWhere(entryIsPresent);
    if (index < 0)
        return;
    RouteEntry& entry = entries_[static_cast<size_t>(index)];
    // 保活到函数结束：冲刷会把这条擦出历史并 dispose（上游 靠 GC 天然不会悬空）。
    SharedPtr<Route> route = entry.route;
    bool shouldFlush = false;

    if (route->isPageBased() && onPopPage_) {
        // 上游 的 page-based 分支：先问 onPopPage 允不允许这一页出栈。
        // 约定与上游 的 assert 一致 —— onPopPage 里通常已经调过 route->didPop
        // （那就是 `_popCompleter.isCompleted` 断言的内容），所以这里只补状态，
        // 不再重复 didPop / 不覆盖 pendingResult。
        if (onPopPage_(route.get(), result)) {
            if (rank(entry.state) <= rank(RouteLifecycle::idle)) {
                entry.state = RouteLifecycle::pop;
                shouldFlush = true;
            }
            // didPop 没被 onPopPage 调过时，handlePop 会自己调并顺带发这个回调，
            // 所以这里只在「已经调过」的场合补发，避免 onPopInvoked 跑两遍。
            if (route->popCompleted())
                invokePopInvoked(route.get(), true, result);
        }
    } else {
        entryPop(entry, result, /*imperativeRemoval=*/true);
        shouldFlush = true;
    }

    // 上游 用 rearrangeOverlay:false：条目的重排留给真正收尾的那次冲刷
    // （有退场动画时是 finalizeRoute 里那次）。
    if (shouldFlush)
        flushHistoryUpdates(false);
}

void NavigatorState::popUntil(std::function<bool(Route*)> predicate) {
    // 上游的 NavigatorState.popUntil（navigator.dart:5691-5704）。
    while (true) {
        Route* current = currentRoute();
        if (!current)
            return;
        if (predicate && predicate(current))
            return;
        pop();
    }
}

void NavigatorState::popUntilWithResult(std::function<bool(Route*)> predicate,
                                        SharedPtr<void> result) {
    // 上游的 NavigatorState.popUntilWithResult（navigator.dart:5707-5730）：
    // 结果只交给「最后一条被弹掉的路由」——即弹掉它之后栈顶就满足 predicate 的
    // 那一条。所以每一步先 peek 下一条在场路由，再决定 pop(result) 还是 pop()。
    // 本仓旧实现是「循环里每次都 pop(result)，最后再 notifyPopped 补一次」，
    // 既会给多条路由送结果，也会给最后那条发两次 whenPopped 回调。
    while (true) {
        const long candidateIndex = lastEntryIndexWhere(entryIsPresent);
        if (candidateIndex < 0)
            return;
        Route* candidate = entries_[static_cast<size_t>(candidateIndex)].route.get();
        if (predicate && predicate(candidate))
            return;

        long nextIndex = -1;
        for (long i = candidateIndex - 1; i >= 0; --i) {
            const RouteEntry& e = entries_[static_cast<size_t>(i)];
            if (e.route && entryIsPresent(e)) {
                nextIndex = i;
                break;
            }
        }
        Route* next = nextIndex >= 0 ? entries_[static_cast<size_t>(nextIndex)].route.get()
                                     : nullptr;
        if (next && !next->willHandlePopInternally() && predicate && predicate(next))
            pop(result);
        else
            pop();
    }
}

bool NavigatorState::maybePop(SharedPtr<void> result) {
    // 上游的 NavigatorState.maybePop（navigator.dart:5572-5621）：
    //   bubble   → 不处理（返回 false，交给上层）
    //   pop      → 弹出（返回 true）
    //   doNotPop → 不弹但视为已处理（返回 true）
    // 上游 还有一步 `await route.willPop()`（已废弃的钩子），本仓未采。
    const long index = lastEntryIndexWhere(entryIsPresent);
    if (index < 0)
        return false;
    Route* current = entries_[static_cast<size_t>(index)].route.get();

    switch (current->popDisposition()) {
        case RoutePopDisposition::bubble:
            return false;
        case RoutePopDisposition::pop:
            pop(result);
            return true;
        case RoutePopDisposition::doNotPop:
            invokePopInvoked(current, false, result);
            return true;
    }
    return false;
}

bool NavigatorState::canPop() const {
    // 上游的 NavigatorState.canPop（navigator.dart:5550-5566）：
    //   没有在场路由 → false
    //   第一条路由自行处理 pop → true
    //   只有一条在场路由 → false
    //   否则 → true
    // 走 entries_ 现算：正在退场（popping）的路由不在场，但马上要弹掉它下面的
    // 那条时，history() 缓存还是旧的。
    const long last = lastEntryIndexWhere(entryIsPresent);
    if (last < 0)
        return false;
    long first = 0;
    while (first < last && !entryIsPresent(entries_[static_cast<size_t>(first)]))
        ++first;
    const RouteEntry& firstEntry = entries_[static_cast<size_t>(first)];
    if (!firstEntry.route)
        return false;
    if (firstEntry.route->willHandlePopInternally())
        return true;
    return first != last;
}

void NavigatorState::removeRoute(const SharedPtr<Route>& route,
                                 SharedPtr<void> result) {
    // 上游的 NavigatorState.removeRoute（navigator.dart:5733-5746）：
    // entry.complete(result, isReplaced:false, imperativeRemoval:true) +
    // 冲刷（rearrangeOverlay:false）。complete 而不是 pop ⇒ 没有 didPop、
    // 没有结果回传，直接向观察者报 didRemove。
    if (!route)
        return;
    const long index = entryIndexOf(route.get());
    if (index < 0)
        return;
    entryComplete(entries_[static_cast<size_t>(index)], result, /*isReplaced=*/false,
                  /*imperativeRemoval=*/true);
    flushHistoryUpdates(false);
}

void NavigatorState::replace(const SharedPtr<Route>& oldRoute,
                             const SharedPtr<Route>& newRoute) {
    // 上游的 NavigatorState.replace → _replaceEntry（navigator.dart:5425-5451）：
    // 与 pushReplacement 的差别只在**位置** —— 新条目插在旧条目上面（栈内位置
    // 不变），旧条目同样 complete(isReplaced:true)。
    if (!newRoute || !oldRoute || oldRoute == newRoute)
        return;
    const long index = entryIndexOf(oldRoute.get());
    if (index < 0)
        return;
    RouteEntry entry;
    entry.route = newRoute;
    entry.state = RouteLifecycle::replace;
    entries_.insert(entries_.begin() + static_cast<long>(index + 1), std::move(entry));
    entryComplete(entries_[static_cast<size_t>(index)], nullptr, /*isReplaced=*/true,
                  /*imperativeRemoval=*/true);
    flushHistoryUpdates();
}
SharedPtr<Route> NavigatorState::pushNamed(const std::string& routeName,
                                                 SharedPtr<void> arguments) {
    // 上游的 NavigatorState.pushNamed（navigator.dart:4761+）。
    auto route = generateRoute(routeName, std::move(arguments));
    if (!route)
        return nullptr;
    return push(route);
}

NavigatorState* NavigatorState::maybeOf(BuildContext& context) {
    // 上游的 Navigator.maybeOf：向上查找最近的 NavigatorState。
    return context.findAncestorStateOfType<NavigatorState>();
}

NavigatorState* NavigatorState::of(BuildContext& context) {
    NavigatorState* result = maybeOf(context);
    if (!result)
        throw std::logic_error("No Navigator widget found in the widget tree.");
    return result;
}

SharedPtr<Widget> NavigatorState::build(BuildContext& context) {
    // 上游的 NavigatorState.build（navigator.dart:5938-5990）：
    // 内部就是一个 Overlay，初始条目是全部路由的 Overlay 条目。
    auto entries = allRouteOverlayEntries();
    return cui::make_shared<Overlay>(std::move(entries), overlayKey_);
}

void NavigatorState::didUpdateWidget(Widget* oldWidget) {
    State::didUpdateWidget(oldWidget);
    auto* navigator = dynamic_cast<Navigator*>(widget());
    if (!navigator)
        return;

    onGenerateRoute_ = navigator->onGenerateRoute();
    onUnknownRoute_ = navigator->onUnknownRoute();
    onPopPage_ = navigator->onPopPage();
    observers_ = navigator->observers();

    if (navigator->pages() != pages_) {
        // 声明式 pages 变化 → 重新对齐路由栈。
        updatePages(navigator->pages());
    }

    // 上游 在 didUpdateWidget 末尾同样逐条通知（navigator.dart:4083-4087）：
    // 宿主 Widget 重建 ⇒ 路由里引用它的地方（如 barrierColor）可能要重算。
    for (auto& entry : entries_) {
        if (entry.route && entry.route->navigator() == this)
            entry.route->changedExternalState();
    }
}

// ── Navigator ───────────────────────────────────────────────────────────────

std::unique_ptr<State> Navigator::createState() { return std::make_unique<NavigatorState>(); }

}
