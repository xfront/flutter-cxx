#pragma once
/// @file widgets/navigator.h
/// @brief 上游的 Navigator / Route / Page / NavigatorObserver 移植。
///
/// 对应 上游源码：
///   上游 widgets/navigator.dart
///
/// Navigator 是 Overlay 之上的「路由栈」：
///   - NavigatorState 持有一条**路由历史**（上游的 `_history`）；
///   - 每条 Route 通过 `install()` 提供自己的 OverlayEntry；
///   - NavigatorState 把这些条目交给内部的 Overlay 统一渲染；
///   - push / pop 只是往历史里增删条目，然后「冲刷」历史
///     （上游的 `_flushHistoryUpdates`）驱动生命周期。
///
/// 与官方实现的差异（本仓已有 Animation / Ticker 体系，转场动画由
/// `widgets/routes.h` 的 `TransitionRoute` 驱动；这里只列**结构性**偏离）：
///   - `Future` 结果改用回调（`Route::whenPopped`）表达；上游
///     `didPush()` 返回的 `TickerFuture` 改由路由自己调
///     `NavigatorState::didFinishPushTransition`（见 `Route::settlesWhenPushed`）。
///   - `_flushHistoryUpdates` 保留 上游 的自顶向下单次行走与每条 case 的
///     就地重跑（上游 的 `continue` ⇒ 本仓的 `again`）；唯一一处结构性偏离是
///     `case dispose` 把「从历史里擦除」推到整轮走完之后（`entries_` 是
///     vector，中途擦除会让 index / next / previous 全部错位）。擦除推迟不改
///     任何谓词的结果：`dispose` 既不在 `isPresent` / `willBePresent` 里，也不在
///     `suitableForAnnouncement` / `suitableForTransitionAnimation` 里。
///   - 上游 的 Graceful dispose（`disposing` / `disposed` 两态等 Overlay 条目
///     先卸载）简化为立即 `route->dispose()`，因此这两个状态不会真正进入。
///   - 没有 `TransitionDelegate`：声明式 `pages` 的转场交接由「复用同类型同 key
///     的旧条目」实现（见 `NavigatorState::updatePages`）。
///
/// 生命周期状态机（保留 上游的 `_RouteLifecycle` 名称与次序，
/// 见 navigator.dart:3126-3160；**次序即语义**，集合判定全靠序号比较）：
///   add → adding → idle
///   push → pushing →（入场动画结束）idle
///   pushReplace / replace →（didReplace）idle
///   idle → pop →（无退场动画）complete → remove → removing → dispose
///                →（有退场动画）popping →（finalizeRoute）dispose

#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/widgets/overlay.h"

namespace cui {

class NavigatorState;
class Route;
class Page;
class NavigatorObserver;

// ── RouteSettings ───────────────────────────────────────────────────────────

/// 构造 Route 时可用的数据。上游的 `RouteSettings`（navigator.dart:671）。
///
/// 与 上游 一致：`name` 为路由名（如 "/settings"），`arguments` 为
/// 传给路由的附加数据（在 `onGenerateRoute` 中使用）。
class RouteSettings {
public:
    RouteSettings() = default;
    explicit RouteSettings(std::string name,
                           SharedPtr<void> arguments = nullptr)
        : name_(std::move(name)), hasName_(true), arguments_(std::move(arguments)) {}

    virtual ~RouteSettings() = default;

    /// 路由名。为空表示匿名路由。
    const std::string& name() const { return name_; }
    /// 是否显式给了名字。
    bool hasName() const { return hasName_; }
    /// 附加数据。
    SharedPtr<void> arguments() const { return arguments_; }

    void setName(std::string name) {
        name_ = std::move(name);
        hasName_ = true;
    }
    void setArguments(SharedPtr<void> arguments) { arguments_ = std::move(arguments); }

private:
    std::string name_;
    bool hasName_ = false;
    SharedPtr<void> arguments_;
};

// ── Page ────────────────────────────────────────────────────────────────────

/// 描述一个「页面」的配置。上游的 `Page<T>`（navigator.dart:708）。
///
/// **Page 继承 RouteSettings**（与 上游 完全一致），因此一个 Page 本身
/// 就可以作为 Route 的 `settings`。此外 Page 携带：
///   - `key`      —— 用于 `canUpdate` 判断两个 Page 是否可复用；
///   - `canPop`   —— 为 false 时阻止关联路由被弹出；
///   - `onPopInvoked` —— 弹栈处理完成后的回调（无论是否真的弹了）。
class Page : public RouteSettings {
public:
    Page(KeyPtr key = nullptr, std::string name = "",
         SharedPtr<void> arguments = nullptr, bool canPop = true)
        : RouteSettings(std::move(name), std::move(arguments)), key_(std::move(key)),
          canPop_(canPop) {}

    ~Page() override = default;

    /// 页面的 Key。
    const KeyPtr& key() const { return key_; }

    /// 是否允许弹出。上游的 `Page.canPop`。
    bool canPop() const { return canPop_; }
    void setCanPop(bool canPop) { canPop_ = canPop; }

    /// 弹栈处理完成后的回调。上游的 `Page.onPopInvoked`。
    ///
    /// 参数：`didPop`（是否真的弹了）、`result`（弹出的结果）。
    void setOnPopInvoked(std::function<void(bool, SharedPtr<void>)> handler) {
        onPopInvoked_ = std::move(handler);
    }
    void invokeOnPopInvoked(bool didPop, SharedPtr<void> result) const {
        if (onPopInvoked_)
            onPopInvoked_(didPop, result);
    }

    /// 是否可以用 `other` 更新本页。上游的 `Page.canUpdate`：
    /// **运行时类型相同 && key 相同**。
    bool canUpdate(const Page& other) const {
        return typeid(*this) == typeid(other) && key_ == other.key_;
    }

    /// 创建与本页对应的路由。上游的 `Page.createRoute`。
    ///
    /// 创建出的路由其 `settings()` 必须指向本页（或等价的 RouteSettings）。
    virtual SharedPtr<Route> createRoute(BuildContext& context) = 0;

private:
    KeyPtr key_;
    bool canPop_ = true;
    std::function<void(bool, SharedPtr<void>)> onPopInvoked_;
};

// ── RoutePopDisposition ─────────────────────────────────────────────────────

/// 是否应当弹出当前路由。上游的 `RoutePopDisposition`（navigator.dart:119）。
enum class RoutePopDisposition {
    /// 弹出。
    pop,
    /// 不弹出（忽略这次返回）。
    doNotPop,
    /// 交给上一层处理（如退出应用）。
    bubble,
};

// ── Route ───────────────────────────────────────────────────────────────────

/// 由 Navigator 管理的条目。上游的 `Route<T>`（navigator.dart:163）。
///
/// 生命周期钩子（全部对应 上游 同名方法）：
///   install()      —— 被插入 Navigator 时调用；需填充 overlayEntries()
///   didPush()      —— 以「压栈」方式插入后调用
///   didAdd()       —— 以「直接出现」方式插入后调用
///   didReplace()   —— 替换掉旧路由后调用
///   didPop()       —— 被弹出时调用
///   didComplete()  —— 弹出流程完成（拿到结果）时调用
///
/// `EnableSharedFromThis` 是为 `TransitionRoute` 的 Ticker 回调加的：那条回调
/// 可能在自己的 tick 里一路走到「历史擦除 ⇒ 本路由析构」（dismissed ⇒
/// finalizeRoute ⇒ flush ⇒ disposeEntry），而 tick 的 `std::function` 闭包帧
/// 还活着。回调里先取一份 `shared_from_this()` 保活，析构就落到帧末之后。
/// 上游 靠 Dart 的 GC + `assert(() { _debugTickInProgress = false; ... })`
/// 天然没有这个问题。
class Route : public EnableSharedFromThis<Route> {
public:
    explicit Route(SharedPtr<const RouteSettings> settings = nullptr)
        : settings_(std::move(settings)) {}
    virtual ~Route() = default;

    /// 本路由的配置。
    const RouteSettings& settings() const {
        static const RouteSettings kEmpty;
        return settings_ ? *settings_ : kEmpty;
    }

    /// 所属的 Navigator（未安装时为 nullptr）。
    NavigatorState* navigator() const { return navigator_; }

    /// 是否已安装到某个 Navigator。
    bool installed() const { return navigator_ != nullptr; }

    /// 本路由提供的 Overlay 条目。上游的 `Route.overlayEntries`。
    const std::vector<SharedPtr<OverlayEntry>>& overlayEntries() const {
        return overlayEntries_;
    }

    /// 是否是栈顶路由。上游的 `Route.isCurrent`（navigator.dart:585）。
    bool isCurrent() const;
    /// 是否是栈底（最底部）路由。上游的 `Route.isFirst`（navigator.dart:602）。
    bool isFirst() const;
    /// 是否处于「在场」状态（位于栈中且未被移除）。
    bool isActive() const;
    /// 是否是最后一个在场路由。
    bool isLast() const;
    /// 下方是否还有在场路由。上游的 `Route.hasActiveRouteBelow`。
    bool hasActiveRouteBelow() const;

    /// 是否自行处理 pop（上游的 `Route.willHandlePopInternally`）。
    virtual bool willHandlePopInternally() const { return false; }

    /// 上游的 `OverlayRoute.finishedWhenPopped`。
    ///
    /// 本仓的 Route 本身就带 Overlay 条目（上游 的 `Route` 没有），所以
    /// 上游 放在 `OverlayRoute` 的这条收在这里。
    ///
    /// true ⇒ `didPop` 当场调 `NavigatorState::finalizeRoute`（路由不再做退场
    /// 动画）。false ⇒ 路由停在 `popping`，由自己在动画结束时调
    /// `finalizeRoute`（上游 的 `TransitionRoute` 用
    /// `_controller.isDismissed && !_popFinalized` 表达同一件事）。
    virtual bool finishedWhenPopped() const { return true; }

    /// 上游 的 `Route.didPush()` 返回 `TickerFuture`，历史条目要等这个 future
    /// 完成才从 `pushing` 转 `idle`。本仓没有 future，改成一个判定：
    ///   - true（默认）⇒ 入场同步完成，flush 里直接进 `idle`；
    ///   - false ⇒ 停在 `pushing`，由路由在入场动画结束时调
    ///     `NavigatorState::didFinishPushTransition(this)`。
    virtual bool settlesWhenPushed() const { return true; }

    /// 上游 的 `Route._popCompleter.isCompleted`：`didComplete` 已经跑过。
    ///
    /// 上游 用 `Completer` 表达「弹出结果已交付」，`handlePop` 靠它判定
    /// 「page-based 路由已由 `Navigator::pop` 的 `onPopPage` 调过 didPop，
    /// 不必再来一遍」；本仓的对应物就是这个只置一次的标志（`didComplete`
    /// 因此天然幂等，复刻 `Completer.complete` 只生效一次的语义）。
    bool popCompleted() const { return popCompleted_; }

    /// 是否应当弹出。上游的 `Route.popDisposition`（navigator.dart:383）：
    ///   - 基于 Page 且 `page.canPop == false` → doNotPop
    ///   - 是栈底路由 → bubble（交由上层，例如退出应用）
    ///   - 否则 → pop
    ///
    /// 上游 是 getter，子类可覆写（`LocalHistoryRoute` 有本地历史时直接返回
    /// pop）；本仓用 virtual 兑现同一个扩展点。
    virtual RoutePopDisposition popDisposition() const;

    /// 是否是基于 Page 的路由。上游的 `Route._isPageBased`。
    bool isPageBased() const;

    /// 登记「弹出完成」回调。上游的 `route.popped` Future 的 C++ 表达。
    void whenPopped(std::function<void(SharedPtr<void>)> handler) {
        popHandlers_.push_back(std::move(handler));
    }

    /// 通知所有 `whenPopped` 回调。
    void notifyPopped(SharedPtr<void> result) const {
        for (const auto& handler : popHandlers_) {
            if (handler)
                handler(result);
        }
    }

    // ── 生命周期钩子（子类覆写）────────────────────────────────────────
    /// 被插入 Navigator 时调用。上游的 `Route.install`（navigator.dart:258）。
    ///
    /// 覆写者应当向 `overlayEntries_` 填充至少一个条目。
    virtual void install() {}
    /// 压栈插入后调用。上游的 `Route.didPush`（navigator.dart:269）。
    virtual void didPush() {}
    /// 直接出现式插入后调用。上游的 `Route.didAdd`（navigator.dart:288）。
    virtual void didAdd() {}
    /// 替换旧路由后调用。上游的 `Route.didReplace`（navigator.dart:323）。
    virtual void didReplace(Route* /*oldRoute*/) {}
    /// 被弹出时调用。上游的 `Route.didPop`（navigator.dart:459）。
    ///
    /// 返回 false 表示「拒绝弹出」，此时路由会回到 idle 状态。
    /// 参数是弹出的结果。
    ///
    /// 上游 的默认实现只做两件事：`didComplete(result)` 然后 return true；
    /// `OverlayRoute` 再在外面判 `finishedWhenPopped` 决定要不要当场
    /// `finalizeRoute`。本仓把这两层合成一个默认实现（见 `finishedWhenPopped`）。
    virtual bool didPop(SharedPtr<void> result);
    /// 弹出流程完成时调用。上游的 `Route.didComplete`（navigator.dart:481）。
    ///
    /// 上游 在这里 complete `popped` future ⇒ 本仓的默认实现是
    /// `notifyPopped(result)`，**时机因此从「路由被彻底移除」提前到了「弹出
    /// 那一刻」**（有退场动画时正是如此，与上游 一致）。子类覆写务必调基类。
    virtual void didComplete(SharedPtr<void> result);
    /// 上游的 `Route.didPushNext`（navigator.dart:517 附近）：本路由之上刚压入
    /// 了 `nextRoute`。`TransitionRoute` 用它来接 secondaryAnimation。
    virtual void didPushNext(Route* /*nextRoute*/) {}
    /// 上游的 `Route.didPopNext`：压在本路由之上的 `poppedRoute` 已弹出，
    /// 本路由重新成为栈顶。
    virtual void didPopNext(Route* /*poppedRoute*/) {}
    /// 上游的 `Route.didChangeNext`：本路由的下一个路由变了（nullptr = 没有）。
    /// 只在历史条目里、且已安装的路由会收到。
    virtual void didChangeNext(Route* /*nextRoute*/) {}
    /// 上游的 `Route.didChangePrevious`：本路由的上一个路由变了（nullptr = 栈底）。
    virtual void didChangePrevious(Route* /*previousRoute*/) {}
    /// 路由被彻底移除时调用。上游的 `Route.dispose`。
    virtual void dispose() {}

protected:
    friend class NavigatorState;

    /// 本路由的 Overlay 条目集合。
    std::vector<SharedPtr<OverlayEntry>> overlayEntries_;

    /// 路由自身的状态变了。上游 的 `Route.changedInternalState`（
    /// navigator.dart:532，@protected）：`willHandlePopInternally` /
    /// `ModalRoute.offstage` 这类内部状态一改就该调用；上游 的基类实现是空的，
    /// `ModalRoute` 覆写它来重建自己的 Overlay 条目。
    virtual void changedInternalState() {}

    /// 外部（宿主 Navigator 及其依赖）变了。上游 的
    /// `Route.changedExternalState`（navigator.dart:559，@protected）：
    /// `NavigatorState.didChangeDependencies` 与 `didUpdateWidget` 逐条通知，
    /// `ModalRoute` 用它强制 barrier 重建。
    virtual void changedExternalState() {}

private:
    SharedPtr<const RouteSettings> settings_;
    NavigatorState* navigator_ = nullptr;
    std::vector<std::function<void(SharedPtr<void>)>> popHandlers_;
    bool popCompleted_ = false;
};

/// 一个「把子树放进 Overlay」的简单路由。
///
/// 上游 中承担这个角色的是 material 层的 `MaterialPageRoute`
/// （其 `buildContent` 把页面内容放进一个 OverlayEntry）。
/// 本移植层尚无 material 层，故提供这个通用实现，
/// 语义等价于「一个不透明的全屏路由」。
class ContentRoute : public Route {
public:
    /// 内容构造器。
    using Builder = std::function<SharedPtr<Widget>(BuildContext&)>;

    explicit ContentRoute(Builder builder, bool opaque = true,
                          SharedPtr<const RouteSettings> settings = nullptr)
        : Route(std::move(settings)), builder_(std::move(builder)), opaque_(opaque) {}

    /// 该路由对应的 Page（可为空）。
    const SharedPtr<Page>& page() const { return page_; }
    void setPage(SharedPtr<Page> page) { page_ = std::move(page); }

    void install() override {
        auto* self = this;
        overlayEntries_.push_back(cui::make_shared<OverlayEntry>(
                [self](BuildContext& context) { return self->builder_(context); }, opaque_));
    }

private:
    Builder builder_;
    bool opaque_ = true;
    SharedPtr<Page> page_;
};

// ── NavigatorObserver ───────────────────────────────────────────────────────

/// 观察 Navigator 行为的接口。上游的 `NavigatorObserver`（navigator.dart:778）。
class NavigatorObserver {
public:
    virtual ~NavigatorObserver() = default;

    /// 压入了 `route`。`previousRoute` 是它下面的那个路由。
    virtual void didPush(Route* /*route*/, Route* /*previousRoute*/) {}
    /// 弹出了 `route`。
    virtual void didPop(Route* /*route*/, Route* /*previousRoute*/) {}
    /// 移除了 `route`。
    virtual void didRemove(Route* /*route*/, Route* /*previousRoute*/) {}
    /// 用 `newRoute` 替换了 `oldRoute`。
    virtual void didReplace(Route* /*newRoute*/, Route* /*oldRoute*/) {}

    /// 栈顶路由换成了 `topRoute`（上一个栈顶是 `previousTopRoute`，可能为空）。
    /// 上游的 `NavigatorObserver.didChangeTop`（navigator.dart:827）：每次
    /// `_flushHistoryUpdates` 末尾、历史里最后一个「在场」条目变化时通知一次。
    virtual void didChangeTop(Route* /*topRoute*/, Route* /*previousTopRoute*/) {}

    /// 用户开始以手势驱动 `route` 的转场。
    /// 上游的 `NavigatorObserver.didStartUserGesture`（navigator.dart:833）。
    virtual void didStartUserGesture(Route* /*route*/, Route* /*previousRoute*/) {}

    /// 上一次 `didStartUserGesture` 的那段手势结束（不论弹出与否）。
    virtual void didStopUserGesture() {}
};

// ── NavigatorState ──────────────────────────────────────────────────────────

/// Navigator 的状态：路由历史的持有者。上游的 `NavigatorState`
/// （navigator.dart:3732）。
class NavigatorState : public State {
public:
    /// 路由历史快照（从底到顶）。上游的 `_history`。
    const std::vector<SharedPtr<Route>>& history() const { return routes_; }

    /// 当前栈顶路由（最上面的在场路由）。
    Route* currentRoute() const;

    /// 某条路由当前是否「在场」。上游 的 `Route.isActive` 每次都用
    /// `isPresentPredicate` 现算，所以冲刷进行中的那次 pop 已经让它变 false
    /// （`TransitionRoute` 的 dismissed 分支正是靠这个判据决定要不要
    /// finalizeRoute）；本仓的 `history()` 是缓存，不能用在这里。
    bool isRoutePresent(const Route* route) const;

    /// 当前栈底路由。
    Route* firstRoute() const;

    /// 压入一个路由。上游的 `NavigatorState.push`（navigator.dart:5050）。
    SharedPtr<Route> push(const SharedPtr<Route>& route);

    /// 用 `newRoute` 替换 `oldRoute`。上游的 `NavigatorState.pushReplacement`
    /// （navigator.dart:5183）。
    SharedPtr<Route> pushReplacement(const SharedPtr<Route>& oldRoute,
                                           const SharedPtr<Route>& newRoute);

    /// 压入新路由，并弹出 `predicate` 之前的全部路由。
    /// 上游的 `NavigatorState.pushAndRemoveUntil`（navigator.dart:5288）。
    SharedPtr<Route> pushAndRemoveUntil(const SharedPtr<Route>& newRoute,
                                               std::function<bool(Route*)> predicate);

    /// 弹出栈顶路由。上游的 `NavigatorState.pop`（navigator.dart:5632）。
    void pop(SharedPtr<void> result = nullptr);

    /// 反复弹出，直到 `predicate` 为真。上游的 `NavigatorState.popUntil`
    /// （navigator.dart:5681）。
    void popUntil(std::function<bool(Route*)> predicate);

    /// 反复弹出直到 `predicate` 为真，并把 `result` 交给最后一个被弹出的路由。
    /// 上游的 `NavigatorState.popUntilWithResult`（navigator.dart:5697）。
    void popUntilWithResult(std::function<bool(Route*)> predicate,
                            SharedPtr<void> result);

    /// 依据当前路由的 `popDisposition` 决定是否弹出。
    /// 上游的 `NavigatorState.maybePop`（navigator.dart:5572）。
    ///
    /// 返回值表示「这次 pop 请求是否已被处理」：
    ///   - `bubble` → 不弹，交给上层（返回 false）；
    ///   - `doNotPop` → 不弹（返回 true，已处理）；
    ///   - `pop` → 弹出（返回 true）。
    bool maybePop(SharedPtr<void> result = nullptr);

    /// 是否可以弹出（在场路由是否超过一个）。
    /// 上游的 `NavigatorState.canPop`（navigator.dart:5541）。
    bool canPop() const;

    /// 移除指定路由（不一定在栈顶）。上游的 `NavigatorState.removeRoute`
    /// （navigator.dart:5723）。
    void removeRoute(const SharedPtr<Route>& route,
                     SharedPtr<void> result = nullptr);

    /// 用 `newRoute` 替换 `oldRoute`。上游的 `NavigatorState.replace`
    /// （navigator.dart:5374）。
    void replace(const SharedPtr<Route>& oldRoute,
                 const SharedPtr<Route>& newRoute);

    /// 按名字压入路由（需 Navigator 提供 `onGenerateRoute`）。
    /// 上游的 `NavigatorState.pushNamed`（navigator.dart:4761）。
    SharedPtr<Route> pushNamed(const std::string& routeName,
                                     SharedPtr<void> arguments = nullptr);

    /// 结束一条已 `didPop` 路由的生命周期。
    /// 上游的 `NavigatorState.finalizeRoute`（navigator.dart:5798）。
    ///
    /// 做了退场动画的路由必须在动画走到下界时调用它（上游 由
    /// `TransitionRoute._handleStatusChanged` 的 dismissed 分支来做），
    /// 本方法把条目推进到 `dispose` 并再冲刷一次历史（不动 Overlay 顺序）。
    void finalizeRoute(Route* route);

    /// 入场动画结束的通知口（上游 由 `didPush()` 返回的 TickerFuture 承担）。
    ///
    /// 只有 `settlesWhenPushed()` 为 false 的路由需要调它：条目从 `pushing`
    /// 转 `idle` 并重新冲刷历史。同步完成的路由由冲刷本身推进，不用调。
    void didFinishPushTransition(Route* route);

    /// 用户开始以手势驱动转场（上游 的 `didStartUserGesture`，navigator.dart:5866）。
    /// 计数式：与 `didStopUserGesture` 配对，最外层那次才通知观察者。
    void didStartUserGesture();

    /// 手势结束（上游 的 `didStopUserGesture`，navigator.dart:5888）。
    void didStopUserGesture();

    /// 是否正处于用户手势驱动中。上游的 `NavigatorState.userGestureInProgress`。
    bool userGestureInProgress() const { return userGestureInProgress_; }

    /// 上游的同名 `ValueNotifier<bool>`：手势状态变化的订阅源。
    const SharedPtr<ValueNotifier<bool>>& userGestureInProgressNotifier() const {
        return gestureNotifier_;
    }

    /// 查找最近的上层 Navigator。上游的 `Navigator.maybeOf`。
    static NavigatorState* maybeOf(BuildContext& context);
    /// 查找最近的上层 Navigator，找不到时抛异常。
    static NavigatorState* of(BuildContext& context);

    /// 观察者列表。上游的 `Navigator.observers`。
    void setObservers(std::vector<NavigatorObserver*> observers) {
        observers_ = std::move(observers);
    }
    const std::vector<NavigatorObserver*>& observers() const { return observers_; }

    /// 内部 Overlay 的 State（用于插入/移除条目）。
    OverlayState* overlayState() const;

protected:
    void initState() override;
    SharedPtr<Widget> build(BuildContext& context) override;
    void didUpdateWidget(Widget* oldWidget) override;
    /// 上游的 `NavigatorState.didChangeDependencies`（navigator.dart:3958）：
    /// 逐条通知历史里属于本 Navigator 的路由「外部状态变了」。
    void didChangeDependencies() override;
    /// 上游的 `NavigatorState.dispose`：forcedDispose 掉历史里所有还没走完的条目。
    void dispose() override;

private:
    friend class Route;

    /// 路由生命周期状态。上游的 `_RouteLifecycle`（navigator.dart:3139）。
    ///
    /// **次序即语义**：上游 用 index 比较来定义「在场 / 将被在场」等集合，
    /// 所以这里必须保持与上游 相同的相对次序（`popping` / `removing` 排在
    /// `remove` **之后**，所以正在做退场动画的路由不再「在场」，但它的
    /// Overlay 条目仍留在历史里）。上游 另有 `staging`（transition delegate）
    /// 与 `pushReplace`（本仓 `replace` 一态两用），本仓未采。
    enum class RouteLifecycle {
        add, adding, push, pushReplace, pushing, replace, idle,
        pop, complete, remove,
        popping, removing,
        dispose, disposing, disposed,
    };

    /// 路由历史条目。上游的 `_RouteEntry`（navigator.dart:3164）。
    struct RouteEntry {
        SharedPtr<Route> route;
        RouteLifecycle state = RouteLifecycle::idle;
        SharedPtr<void> pendingResult;
        bool imperativeRemoval = false;

        /// 上游的 `_reportRemovalToObserver`：被 replace 掉的路由不再向观察者
        /// 报 didRemove（只报 didReplace）。
        bool reportRemovalToObserver = true;

        // 上游 `_RouteEntry` 的三个「上次已通告」字段，用于
        // `_flushRouteAnnouncement` 的去重。
        Route* lastAnnouncedNextRoute = nullptr;
        Route* lastAnnouncedPreviousRoute = nullptr;
        Route* lastAnnouncedPoppedNextRoute = nullptr;
    };

    /// 冲刷历史，驱动生命周期。上游的 `_flushHistoryUpdates`
    /// （navigator.dart:4451）。
    void flushHistoryUpdates(bool rearrangeOverlay = true);

    // ── 上游 _RouteEntry 的谓词（navigator.dart:3519-3546）────────────────
    // 「在场」= add…remove；正在做退场动画的条目（popping）**不在场**，
    // 于是 pop 之后栈顶立刻变成下面的路由，但它的 Overlay 条目仍留在历史里。
    /// enum class 的序号（上游 用 `currentState.index` 做集合比较）。
    static int rank(RouteLifecycle state);
    static bool entryIsPresent(const RouteEntry& entry);
    static bool entryWillBePresent(const RouteEntry& entry);
    static bool entrySuitableForAnnouncement(const RouteEntry& entry);
    static bool entrySuitableForTransitionAnimation(const RouteEntry& entry);

    /// 上游的 `_getRouteBefore` / `_getRouteAfter` / `_lastRouteEntryWhereOrNull`。
    ///
    /// 下标用 `long`（不是 size_t）：`routeBefore(index - 1, …)` 在 index==0 时
    /// 必须得到「没有前一条」，而 size_t 会绕成最大值。-1 表示栈底之下。
    Route* routeBefore(long index, bool (*predicate)(const RouteEntry&)) const;
    Route* routeAfter(long index, bool (*predicate)(const RouteEntry&)) const;
    long lastEntryIndexWhere(bool (*predicate)(const RouteEntry&)) const;
    /// 某条路由在历史里的下标（不在历史中 ⇒ -1）。上游的 `_history.indexWhere`。
    long entryIndexOf(const Route* route) const;

    /// 上游的 `_flushRouteAnnouncement`（navigator.dart:4638）：向每条路由发
    /// didChangeNext / didChangePrevious。
    void flushRouteAnnouncement();

    /// 上游的 `_flushObserverNotifications`（navigator.dart:4621）：把冲刷期间
    /// 攒下的观测（push / replace / pop / remove）按「新增倒序、删除正序」发出。
    void flushObserverNotifications();

    // ── 上游 _RouteEntry 的状态迁移助手（navigator.dart:3245-3444）─────────
    /// `handleAdd` + `didAdd`：上游在 `case add` 里只登记观测、把状态推到
    /// `adding`，真正 install 在 `case adding`（且上面没有挡住的路由时）发生。
    void handleAdd(RouteEntry& entry, Route* previousPresent);
    void handleDidAdd(RouteEntry& entry, RouteEntry* next);
    /// 上游的 `handlePush`：install → `didPush`（或 `didReplace`）→ `pushing`
    /// （或 `idle`），并登记 push / replace 观测。
    void handlePush(RouteEntry& entry, Route* previous, Route* previousPresent,
                    RouteEntry* next);
    /// 上游的 `handlePop`：返回 false 表示路由拒绝被弹（条目回到 idle）。
    bool handlePop(RouteEntry& entry);
    /// 上游的 `handleComplete`。
    void handleComplete(RouteEntry& entry);
    /// 上游的 `handleRemoval`。
    void handleRemoval(RouteEntry& entry, Route* previousPresent);
    /// 上游的 `handleDidPopNext`。
    void handleDidPopNext(RouteEntry& entry, Route* poppedRoute);
    /// 上游的 `_RouteEntry.pop`：记下结果并把条目置为 `pop`。
    void entryPop(RouteEntry& entry, SharedPtr<void> result, bool imperativeRemoval);
    /// 上游的 `_RouteEntry.complete`：已经走到 `remove` 之后的条目不再回退。
    void entryComplete(RouteEntry& entry, SharedPtr<void> result, bool isReplaced,
                       bool imperativeRemoval);

    /// 把上游 `Route.onPopInvokedWithResult` 的默认实现（只有 page-based 路由
    /// 才转发给 `Page.onPopInvoked`）搬到本仓：本仓的 Route 没有这个虚钩子。
    static void invokePopInvoked(Route* route, bool didPop, SharedPtr<void> result);

    /// 从历史中移除条目并释放其 Overlay 条目。
    void disposeEntry(RouteEntry& entry);

    /// 把路由的 Overlay 条目从内部 Overlay 移除。
    void removeRouteEntries(const SharedPtr<Route>& route);

    /// 全部在场路由的 Overlay 条目（自底向上）。上游的
    /// `_allRouteOverlayEntries`（navigator.dart:4138）。
    std::vector<SharedPtr<OverlayEntry>> allRouteOverlayEntries() const;

    /// 通知观察者。观测的攒与发在 `flushObserverNotifications`。

    /// 根据名字与参数生成路由（走 Navigator 的 onGenerateRoute）。
    SharedPtr<Route> generateRoute(const std::string& routeName,
                                         SharedPtr<void> arguments);

    /// 处理声明式 pages 更新。上游的 `NavigatorState._updatePages`。
    void updatePages(const std::vector<SharedPtr<Page>>& pages);

    /// 建立初始路由栈（命令式：initialRoute / onGenerateRoute）。
    void setupInitialRoutes();

    std::vector<RouteEntry> entries_;
    /// 历史里「在场」条目的路由快照（自底向上）。上游 没有这份缓存，每次都
    /// 用 `_RouteEntry.isPresentPredicate` 过滤 `_history`；本仓的 `history()`
    /// 要返回引用，故在每次冲刷末尾同步（见 `syncPresentRoutes`）。
    std::vector<SharedPtr<Route>> routes_;

    /// 由 entries_ 重算 routes_。
    void syncPresentRoutes();

    /// 上游的 `_flushingHistory`：转场动画在冲刷期间同步结束时，
    /// `finalizeRoute` 不能再递归冲刷一次。
    bool flushingHistory_ = false;

    /// 上游的 `_lastTopmostRoute`：上一次冲刷结束时「最后一个在场条目」的路由，
    /// 用来判 `didChangeTop`。持 shared_ptr 而不是裸指针：这条路由可能在本轮
    /// 冲刷里被彻底销毁，而 `didChangeTop` 要读它（上游 靠 GC 保活）。
    SharedPtr<Route> lastTopmostRoute_;

    /// 上游的 `_observedRouteAdditions` / `_observedRouteDeletions`。
    struct ObserverObservation {
        enum class Kind { push, replace, pop, remove } kind = Kind::push;
        Route* route = nullptr;
        Route* previous = nullptr;
    };
    std::vector<ObserverObservation> observedAdditions_;
    std::vector<ObserverObservation> observedDeletions_;

    /// 上游的 `_userGesturesInProgress`（计数）+ `userGestureInProgressNotifier`。
    int userGesturesInProgress_ = 0;
    bool userGestureInProgress_ = false;
    SharedPtr<ValueNotifier<bool>> gestureNotifier_;

    /// 内部 Overlay 的 GlobalKey。上游的 `_overlayKey`。
    SharedPtr<GlobalKey> overlayKey_;

    std::vector<NavigatorObserver*> observers_;

    /// 声明式 pages 的当前快照。
    std::vector<SharedPtr<Page>> pages_;

    /// 路由生成的回调（来自 Navigator 的 onGenerateRoute）。
    std::function<SharedPtr<Route>(const RouteSettings&)> onGenerateRoute_;

    /// 未知路由的兜底生成器（来自 Navigator 的 onUnknownRoute）。
    std::function<SharedPtr<Route>(const RouteSettings&)> onUnknownRoute_;

    /// page 弹出时的确认回调（来自 Navigator 的 onPopPage）。
    std::function<bool(Route*, SharedPtr<void>)> onPopPage_;

    /// 是否处于声明式（pages）模式。
    bool declarativeMode() const { return !pages_.empty(); }
};

// ── Navigator ───────────────────────────────────────────────────────────────

/// 路由栈部件。上游的 `Navigator`（navigator.dart:1580 附近）。
///
/// 两种驱动方式：
///   - **命令式**：给定 `initialRoute`（或 `onGenerateRoute`）建立初始栈，
///     之后通过 `NavigatorState::push` / `pop` 等方法操作；
///   - **声明式**：给定 `pages`，Navigator 会按 `Page::canUpdate` 规则
///     增量更新路由栈（上游的 `Navigator.pages`）。
class Navigator : public StatefulWidget {
public:
    /// 默认路由名。上游的 `Navigator.defaultRouteName`
    /// （navigator.dart:52）：`'/'`。
    static constexpr const char* defaultRouteName = "/";

    /// 路由生成器。上游的 `Navigator.onGenerateRoute`。
    using GenerateRouteCallback =
            std::function<SharedPtr<Route>(const RouteSettings&)>;

    /// 页面弹出确认回调。上游的 `Navigator.onPopPage`。
    ///
    /// 返回 true 表示「允许这个页面从 `pages` 中被移除」。
    using PopPageCallback = std::function<bool(Route*, SharedPtr<void>)>;

    explicit Navigator(std::vector<SharedPtr<Page>> pages = {},
                       KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), pages_(std::move(pages)) {}

    /// 声明式页面列表。上游的 `Navigator.pages`。
    const std::vector<SharedPtr<Page>>& pages() const { return pages_; }
    void setPages(std::vector<SharedPtr<Page>> pages) { pages_ = std::move(pages); }

    /// 初始路由名。上游的 `Navigator.initialRoute`。
    const std::string& initialRoute() const { return initialRoute_; }
    void setInitialRoute(std::string name) { initialRoute_ = std::move(name); }

    /// 路由生成器。上游的 `Navigator.onGenerateRoute`。
    const GenerateRouteCallback& onGenerateRoute() const { return onGenerateRoute_; }
    void setOnGenerateRoute(GenerateRouteCallback callback) {
        onGenerateRoute_ = std::move(callback);
    }

    /// 未知路由时的兜底生成器。上游的 `Navigator.onUnknownRoute`。
    const GenerateRouteCallback& onUnknownRoute() const { return onUnknownRoute_; }
    void setOnUnknownRoute(GenerateRouteCallback callback) {
        onUnknownRoute_ = std::move(callback);
    }

    /// 页面弹出确认回调。上游的 `Navigator.onPopPage`。
    const PopPageCallback& onPopPage() const { return onPopPage_; }
    void setOnPopPage(PopPageCallback callback) { onPopPage_ = std::move(callback); }

    /// 观察者。上游的 `Navigator.observers`。
    const std::vector<NavigatorObserver*>& observers() const { return observers_; }
    void setObservers(std::vector<NavigatorObserver*> observers) {
        observers_ = std::move(observers);
    }

    std::unique_ptr<State> createState() override;

private:
    std::vector<SharedPtr<Page>> pages_;
    std::string initialRoute_;
    GenerateRouteCallback onGenerateRoute_;
    GenerateRouteCallback onUnknownRoute_;
    PopPageCallback onPopPage_;
    std::vector<NavigatorObserver*> observers_;
};

}
