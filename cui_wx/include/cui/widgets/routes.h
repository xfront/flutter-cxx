#pragma once
/// @file widgets/routes.h
/// @brief 上游的「路由转场层」移植：OverlayRoute / TransitionRoute /
///        LocalHistoryEntry / ModalRoute / PopupRoute / RawDialogRoute /
///        RouteObserver / RouteAware。
///
/// 对应 上游源码：
///   上游 widgets/routes.dart
///
/// 这一层坐在 `widgets/navigator.h` 的 Route 之上：Navigator 的状态机只管
/// 「条目什么时候该走哪一步」，而**转场怎么动**（时间线、secondaryAnimation、
/// modal barrier、_ModalScope 那棵子树）全在这里。`ContentRoute` /
/// `DialogRoute` 这类「一装一卸」的简单路由不必继承本层。
///
/// ── 与上游的有意偏离（勿当 bug 排查）────────────────────────────────────
///   1. **LocalHistoryRoute 折进 ModalRoute**。上游 是
///      `mixin LocalHistoryRoute<T> on Route<T>`，C++ 没有 mixin，且本仓只有
///      ModalRoute 一处用它 ⇒ 直接并成 ModalRoute 的成员与方法。
///   2. **泛型 T（返回值类型）压成 `SharedPtr<void>`**。口径与
///      `navigator.h` 的 Route 一致。
///   3. **`TransitionRoute.completed` 是 Future ⇒ `whenTransitionCompleted(cb)`**。
///      派发点仍是上游 的 `dispose()`（转场彻底结束、Overlay 条目已摘）。
///   4. **Simulation / TrainHoppingAnimation 未接**。上游 的
///      `createSimulation({forward})` + `animateWith` / `animateBackWith`
///      需要 controller 收 Simulation；本仓 `AnimationController` 的
///      SpringSimulation 只服务于 `fling()`，没有这条通道。连带
///      `_updateSecondaryAnimation` 里的「换车」三段（两条动画值不同步时挂
///      TrainHoppingAnimation）也没翻：`secondaryAnimation` 只在
///      「下一条的 animation」与「kAlwaysDismissedAnimation」之间切换。
///   5. **PredictiveBackRoute 未接**。`handleStartBackGesture` /
///      `handleUpdateBackGestureProgress` / `handleCommit(_cancel)BackGesture` /
///      `popGestureEnabled` 整条手势通道不在本层（本仓的返回手势走
///      `NavigatorState::didStartUserGesture`，已可用）。
///   6. **语义/无障碍整段未接**：`Semantics`、`BlockSemantics`、
///      `semanticsDismissible`、`barrierLabel`、`_DismissModalAction` +
///      `DismissIntent`、`NavigationNotification`、`PopEntry` 家族
///      （`registerPopEntry` / `willPop` / `scopedWillPopCallback`）。
///      `popDisposition` 因此只保留 Page.canPop / 本地历史 / 栈底三条判据。
///   7. **`GlobalKey<_ModalScopeState>` ⇒ `ModalRoute::attachScope`**。
///      本仓没有 GlobalKey 注册表；`ModalScopeState` 在 `initState` 把自己
///      登记到路由、`dispose` 里注销，等价于上游 的 `_scopeKey.currentState`。
///      `_ModalScopeState.build` 外层那五层包装里，
///      `RestorationScope` / `PageStorage` / `PrimaryScrollController` /
///      `FocusScope` / `RepaintBoundary` 未接（本仓分别没有对应体系，或——
///      RepaintBoundary——还没有接到中间层节点）。
///      `AnimatedBuilder(animation: restorationScopeId)` 那层随之去掉。
///   8. **`InheritedModel<_ModalRouteAspect>` ⇒ 普通 InheritedWidget**。
///      `ModalScopeStatus` 一次给出整份数据，依赖者不再按 aspect 精确失效；
///      `ModalRoute::isCurrentOf` / `canPopOf` / `settingsOf` / `isActiveOf` /
///      `isFirstOf` / `opaqueOf` / `popDispositionOf` 七个静态查询因此口径相同
///      （上游 靠 aspect 只重建相关的那部分）。
///   9. **`schedulerPhase` ⇒ `BuildOwner` 的两个阶段标志**。上游 的
///      `persistentCallbacks` 对应 `inBuildPhase()`；本地历史 removal 的
///      「构建期不能当场通知」由此翻译成帧末回调。
///  10. **`_setSecondaryAnimation(animation, nextRoute.completed)`** 的
///      「等下一条路由 dispose 再把引用放回常量动画」，本仓改成
///      `TransitionRoute::dispose()` 里扫一遍 Navigator 历史，复位所有
///      「转场源是自己」的次级动画。
///  11. **时间线归路由自己**。上游 的 controller 由 Navigator
///      （TickerProvider）造 Ticker 推进；本仓 `AnimationController` 不带
///      Ticker（见 animation/animation.h 偏离 2），故每条 TransitionRoute
///      自持一条 Ticker，并用 `TickerModeBinding` 接环境 TickerMode。
///      喂给 controller 的是「路由自己累加的帧时间线」。静音期间 Ticker 不再
///      回调（画面冻结），但 `Ticker::elapsed()` 仍按真实时钟走，解除静音的
///      那一帧 dt 会把整段静音补上 ⇒ 跳到该在的那一格（与上游 一致）。
///  12. **`filter`（BackdropFilter）/ `allowSnapshotting` / performance-mode
///      请求未接**；上游 在 forward/reverse 时 `requestPerformanceMode`，本仓
///      没有该通道。
///  13. **`canSizeOverlay`（OverlayEntry 的新参数）未接**。
///  14. **`impliesAppBarDismissal` 保留计数与谓词**（本地历史条目 +
///      `hasActiveRouteBelow`），但本仓没有 AppBar 消费者。
///  15. **`Curves.ease` 没有对应物**。`barrierCurve` 上游 是
///      Cubic(0.4, 0.0, 1.0, 1.0)，本仓的 `Easing` 枚举里没有三次贝塞尔，
///      取最接近的 `Easing::EaseOutQuad`。
///  16. **路由析构时机**：退场动画走到下界 ⇒ 同一次 tick 里
///      `finalizeRoute` ⇒ 历史擦除 ⇒ 路由本该在「自己的 Ticker 回调栈上」
///      析构（连它自己的 `std::function` 一起拆掉）。本仓用
///      `EnableSharedFromThis<Route>`（见 navigator.h）在 tick 开头保一份活，
///      发现 `disposed_` 就把最后一份引用转交帧末回调。上游 靠 GC 天然没有
///      这个问题。

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/modal_barrier.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/ticker_mode.h"
#include "cui/widgets/transition.h"

namespace cui {

class ModalRoute;
class ModalScopeState;
class TransitionRoute;

// ── 回调类型（上游 的 RoutePageBuilder / RouteTransitionsBuilder）──────────

/// 上游的 `RoutePageBuilder`：产出路由的主内容。
using RoutePageBuilder = std::function<SharedPtr<Widget>(
        BuildContext&, SharedPtr<Animation<double>>, SharedPtr<Animation<double>>)>;

/// 上游的 `RouteTransitionsBuilder`：把主内容包成转场动画。
using RouteTransitionsBuilder = std::function<SharedPtr<Widget>(
        BuildContext&, SharedPtr<Animation<double>>, SharedPtr<Animation<double>>,
        SharedPtr<Widget>)>;

// ── OverlayRoute ────────────────────────────────────────────────────────────

/// 在 Navigator 的 Overlay 上显示内容的路由。上游的 `OverlayRoute<T>`
/// （routes.dart:55）。
///
/// 上游 把它做成抽象类、只为了 `createOverlayEntries()` 这一条工厂方法；本仓
/// 的 `Route` 已经直接持有 `overlayEntries_`（navigator.h 结构性偏离），
/// 所以这里只补「install 时填充、dispose 时清空」两条编排。
class OverlayRoute : public Route {
public:
    explicit OverlayRoute(SharedPtr<const RouteSettings> settings = nullptr)
        : Route(std::move(settings)) {}

    /// 子类覆写它来产出自己的 Overlay 条目。上游的 `createOverlayEntries()`。
    ///
    /// 顺序即叠放顺序：靠前的在下。
    virtual std::vector<SharedPtr<OverlayEntry>> createOverlayEntries() = 0;

    void install() override;
    void dispose() override;
};

// ── TransitionRoute ─────────────────────────────────────────────────────────

/// 有入场/退场转场的路由。上游的 `TransitionRoute<T>`（routes.dart:111）。
///
/// 用法约定（与上游 一致）：子类给出 `transitionDuration()` 与 `opaque()`，
/// 并实现 `ModalRoute` 那一层的 `buildPage`。本类自己负责：
///   - 造 controller / animation 并挂上状态监听（install）；
///   - didPush→forward、didAdd→跳到上界、didPop→reverse、didReplace→接续
///     旧路由的值；
///   - 退场走到下界时替 Navigator 调 `finalizeRoute`（上游 的 dismissed 分支）；
///   - 入场走到上界时替 Navigator 调 `didFinishPushTransition`（上游 由
///     `didPush()` 的 TickerFuture 承担，见 `settlesWhenPushed`）；
///   - 维护 `secondaryAnimation`（本路由之上那条路由的转场进度）。
class TransitionRoute : public OverlayRoute {
public:
    explicit TransitionRoute(SharedPtr<const RouteSettings> settings = nullptr);

    /// 上游的 `transitionDuration` / `reverseTransitionDuration`（单位 ms）。
    ///
    /// 本仓的时长一律用 double 毫秒表达（`AnimationController` 的口径），
    /// 不引入 `Duration` 值类型参数。
    virtual double transitionDuration() const = 0;
    virtual double reverseTransitionDuration() const { return transitionDuration(); }

    /// 上游的 `opaque`：转场完成后是否遮住下面的路由（决定 Overlay 条目
    /// 能否让下层停建）。
    virtual bool opaque() const = 0;

    /// 上游 的 `finishedWhenPopped => _controller.isDismissed && !_popFinalized`。
    ///
    /// 「已经走到下界但还没 finalize」= 需要 finalize；
    /// 「已经 finalize 过」= 不要重复 finalize（iOS 侧滑那类手势会先把动画
    /// 拖到下界，之后才真正 pop）。
    bool finishedWhenPopped() const override;

    /// 上游 的 `didPush()` 返回 TickerFuture；本仓转成「入场动画报 Completed
    /// 之前一直返回 false」，条目因此停在 `pushing`。
    bool settlesWhenPushed() const override;

    /// 上游的 `animation` / `secondaryAnimation` / `controller`。
    ///
    /// ModalRoute 覆写这两个 getter 换成正事「对外可见的 ProxyAnimation」
    /// （上游 的 `_animationProxy` / `_secondaryAnimationProxy`），所以它们是
    /// virtual；路由内部的转场逻辑一直读 `animation_` 本身。
    virtual SharedPtr<Animation<double>> animation() const { return animation_; }
    virtual SharedPtr<Animation<double>> secondaryAnimation() const {
        return secondaryAnimation_;
    }
    /// 上游的 `TransitionRoute._animation` 本体。
    ///
    /// `ModalRoute` 把对外的 `animation()` 换成了 `ProxyAnimation`（供
    /// `offstage` 换源），而**上一条**路由的 `secondaryAnimation` 上游 接的是
    /// 这条内部动画（`nextRoute._animation`），所以这里必须能拿到它。
    const SharedPtr<Animation<double>>& rawAnimation() const { return animation_; }
    const SharedPtr<AnimationController>& controller() const { return controller_; }

    /// 上游的 `willDisposeAnimationController`：为 false 时 controller 由
    /// 外部（如 Material 的 `_PageTransition`）负责释放。
    bool willDisposeAnimationController() const { return willDisposeAnimationController_; }
    void setWillDisposeAnimationController(bool value) {
        willDisposeAnimationController_ = value;
    }

    /// 上游的 `createAnimationController()` / `createAnimation()`。
    ///
    /// 本仓没有 `controller.view` 这一层（`AnimationController` 本身就是
    /// `Animation<double>`），所以 `createAnimation()` 默认返回同一个对象；
    /// 子类想换曲线可以覆写它返回一条 `CurvedAnimation`。
    virtual SharedPtr<AnimationController> createAnimationController();
    virtual SharedPtr<Animation<double>> createAnimation();

    void install() override;
    void didPush() override;
    void didAdd() override;
    void didReplace(Route* oldRoute) override;
    bool didPop(SharedPtr<void> result) override;
    void didPopNext(Route* nextRoute) override;
    void didChangeNext(Route* nextRoute) override;
    void dispose() override;

    /// 上游的 `canTransitionTo` / `canTransitionFrom`：两条路由是否愿意互相
    /// 联动 secondaryAnimation。默认都 true。
    virtual bool canTransitionTo(const TransitionRoute& nextRoute) const { return true; }
    virtual bool canTransitionFrom(const TransitionRoute& previousRoute) const { return true; }

    /// 上游的 `completed` Future 的 C++ 表达（见文件头偏离 3）。
    ///
    /// 派发点是 `dispose()`，参数是退场时拿到的结果。
    void whenTransitionCompleted(std::function<void(SharedPtr<void>)> handler) {
        completedHandlers_.push_back(std::move(handler));
    }
    /// 上游的 `debugTransitionCompleted()`。
    bool transitionCompleted() const { return transitionCompleted_; }
    /// 是否已经 dispose（测试与延迟释放用）。
    bool routeDisposed() const { return disposed_; }

protected:
    /// 上游的 `_handleStatusChanged`。
    ///
    /// 做成 virtual 是给 `ModalRoute` 挂钩的：上游 的 barrier 外层那个
    /// `IgnorePointer` 注释就写着「animation.status 更新时会调
    /// changedInternalState」，本仓的 `ProxyAnimation` 不会自己通知 barrier
    /// 条目重建，所以状态迁移必须在这里顺带通知一次。
    virtual void handleTransitionStatus(AnimationStatus status);
    /// 上游的 `_updateSecondaryAnimation`（无换车段，见文件头偏离 4）。
    void updateSecondaryAnimation(Route* nextRoute);
    /// 上游的 `_setSecondaryAnimation`。`sourceRoute` 是这条动画的主人，
    /// 只用于指针比较（见偏离 10）。
    void setSecondaryAnimation(SharedPtr<Animation<double>> animation,
                               const Route* sourceRoute);

    /// Ticker 回调：推进 controller，并在「本次 tick 里被 dispose」时把最后一份
    /// 引用转交帧末（见文件头偏离 16）。
    void onTransitionTick(Duration elapsed);
    /// controller 正在跑而 ticker 没跑 ⇒ 起表。
    void ensureTicking();
    void deferRelease(SharedPtr<Route> self);

    SharedPtr<AnimationController> controller_;
    /// 上游的 `_animation`（= controller 本身，或子类给的曲线动画）。
    SharedPtr<Animation<double>> animation_;
    /// 上游的 `_secondaryAnimation`（ProxyAnimation），`secondaryAnimation_`
    /// 是同一个对象的上转句柄，供 getter 返回。
    SharedPtr<ProxyAnimation> secondaryProxy_;
    SharedPtr<Animation<double>> secondaryAnimation_;
    const Route* secondarySource_ = nullptr;

    std::unique_ptr<Ticker> ticker_;
    TickerModeBinding tickerMode_;

    std::vector<std::function<void(SharedPtr<void>)>> completedHandlers_;
    /// install 时记下的 BuildOwner：退场动画把自己 dispose 掉的那一帧，
    /// 用它把析构推到帧末。
    BuildOwner* frameOwner_ = nullptr;

    /// 本路由自己的帧时间线（ms，单调累加；Ticker 重启不回头）。
    double tickTimelineMs_ = 0.0;
    double lastTickMs_ = 0.0;

    bool popFinalized_ = false;
    bool pushTransitionFinished_ = false;
    bool transitionCompleted_ = false;
    bool disposed_ = false;
    bool willDisposeAnimationController_ = true;
    int statusHandle_ = 0;
    /// 上游的 `_result`：退场结果，`dispose` 时交给 `completedHandlers_`。
    SharedPtr<void> result_;
};

// ── LocalHistoryEntry ───────────────────────────────────────────────────────

/// 路由内部「迷你导航栈」的一条记录。上游的 `LocalHistoryEntry`
/// （routes.dart:708）。
///
/// 有本地历史时，`Navigator::pop` 被路由就地消化：弹出最后一条 entry、调它的
/// `onRemove`，路由本身留在栈上。
class LocalHistoryEntry {
public:
    using RemoveCallback = std::function<void()>;

    explicit LocalHistoryEntry(RemoveCallback onRemove = nullptr,
                               bool impliesAppBarDismissal = true)
        : onRemove_(std::move(onRemove)),
          impliesAppBarDismissal_(impliesAppBarDismissal) {}

    const RemoveCallback& onRemove() const { return onRemove_; }
    bool impliesAppBarDismissal() const { return impliesAppBarDismissal_; }
    /// 所属路由（未挂或已摘为 nullptr）。
    ModalRoute* owner() const { return owner_; }

    /// 从所属路由的本地历史里移除自己。上游的 `LocalHistoryEntry.remove()`。
    void remove();

private:
    friend class ModalRoute;

    void notifyRemoved() {
        if (onRemove_)
            onRemove_();
    }

    RemoveCallback onRemove_;
    bool impliesAppBarDismissal_;
    ModalRoute* owner_ = nullptr;
};

// ── ModalScopeStatus ────────────────────────────────────────────────────────

/// `ModalScopeStatus` 向下发的数据。上游 私有的 `_ModalScopeStatus` 的字段集
/// （aspect 粒度未接，见文件头偏离 8）。
struct ModalScopeStatusData {
    bool isCurrent = false;
    bool canPop = false;
    bool impliesAppBarDismissal = false;
    bool opaque = false;
    /// 上游 的 `_ModalScopeStatus.route`（`ModalRoute::of` 就是从这里取路由）。
    /// 非 const：`maybeOf` 要返回可写的 `ModalRoute*`，指针在 const 数据对象里
    /// 仍然是「指向非 const 的指针」，但 `dynamic_cast` 到 `ModalRoute*` 需要
    /// 起点不是 const 限定类型。
    Route* route = nullptr;

    bool operator!=(const ModalScopeStatusData& other) const {
        return isCurrent != other.isCurrent || canPop != other.canPop ||
               impliesAppBarDismissal != other.impliesAppBarDismissal ||
               opaque != other.opaque || route != other.route;
    }
    bool operator==(const ModalScopeStatusData& other) const { return !(*this != other); }
};

/// 把「本路由的存在状态」发给路由自己那棵子树。上游 私有的
/// `_ModalScopeStatus`（routes.dart:1015）；`ModalRoute::of` 就是找它。
class ModalScopeStatus : public InheritedWidget<ModalScopeStatusData> {
public:
    ModalScopeStatus(bool isCurrent, bool canPop, bool impliesAppBarDismissal,
                     bool opaque, Route* route, SharedPtr<Widget> child,
                     KeyPtr key = nullptr)
        : InheritedWidget<ModalScopeStatusData>(
                  std::move(key), std::move(child)),
          data_(cui::make_shared<ModalScopeStatusData>(ModalScopeStatusData{
                  isCurrent, canPop, impliesAppBarDismissal, opaque, route})) {}

    SharedPtr<const ModalScopeStatusData> data() const override { return data_; }

private:
    SharedPtr<const ModalScopeStatusData> data_;
};

// ── ModalScope ──────────────────────────────────────────────────────────────

/// 路由内容的宿主。上游 私有的 `_ModalScope`（routes.dart:1075）。
///
/// `route` 由路由自己持有缓存的 widget（`_modalScopeCache`），且路由活到帧末
/// （见偏离 16），而它的 Overlay 条目一定在 dispose 之前就被同步摘掉 ⇒ 本控件
/// 不会在路由死后还被重建。
class ModalScope : public StatefulWidget {
public:
    explicit ModalScope(ModalRoute* route, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), route_(route) {}

    ModalRoute* route() const { return route_; }

    std::unique_ptr<State> createState() override;

private:
    ModalRoute* route_ = nullptr;
};

/// 上游 私有的 `_ModalScopeState`（routes.dart:1084）。
///
/// 上游 用 `ListenableBuilder(listenable: merge([animation,
/// secondaryAnimation]))` 触发重建；本仓的 State 直接向这两条动画订阅（走
/// 句柄通道），少一层，效果一致（`child` 缓存 `_page` 的做法照旧）。
class ModalScopeState : public State {
public:
    void initState() override;
    void didChangeDependencies() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

    /// 上游的 `_forceRebuildPage`：丢掉缓存的 page，下次 build 重新调
    /// `buildPage`。`changedExternalState` 用它。
    void forceRebuildPage();

    /// 上游的 `_routeSetState`。
    void routeSetState(std::function<void()> fn);

    /// 上游的 `_shouldIgnoreFocusRequest`：反向播放中、或用户手势进行中 ⇒
    /// 路由内容不接受命中（FocusNode.canRequestFocus 那半段未接，见偏离 7）。
    bool shouldIgnoreFocusRequest() const;

    /// 上游的 `ModalRoute.subtreeContext`（`_subtreeKey.currentContext`）。
    ///
    /// 本仓没有 GlobalKey 注册表，所以在缓存的 page `Builder` 真正执行时把
    /// 收到的 context 记下来：那条 Element 在 `_page` 换实例时**不会**重建
    /// （同类型同 key ⇒ `update`），所以它和 State 一样长寿；`dispose` 里清掉。
    BuildContext* subtreeContext() const { return subtreeContext_; }

private:
    void subscribe();
    void unsubscribe();

    SharedPtr<Widget> pageCache_;
    SharedPtr<Animation<double>> animationSub_;
    SharedPtr<Animation<double>> secondarySub_;
    int animationHandle_ = 0;
    int secondaryHandle_ = 0;
    bool dependenciesChanged_ = false;
    BuildContext* subtreeContext_ = nullptr;
};

// ── ModalRoute ──────────────────────────────────────────────────────────────

/// 挡住下层交互的路由。上游的 `ModalRoute<T>`（routes.dart:1266），
/// `LocalHistoryRoute` 已折进来（见文件头偏离 1）。
///
/// 它的 Overlay 条目恰好两条：`[modalBarrier, modalScope]`。barrier 在下、
/// scope 在上；barrier 的颜色由 `animation` 经 `barrierCurve` 驱动，
/// scope 里的内容按 `buildTransitions` 包装。
class ModalRoute : public TransitionRoute {
public:
    explicit ModalRoute(SharedPtr<const RouteSettings> settings = nullptr);

    // ── 子类要给的配置（上游 的抽象 getter）──────────────────────────────
    /// 上游的 `barrierDismissible`：点 barrier 是否弹掉本路由。
    virtual bool barrierDismissible() const = 0;
    /// 上游的 `barrierColor`：为空 ⇒ barrier 完全透明（但仍吞命中，见
    /// modal_barrier.h）。
    virtual std::optional<Color> barrierColor() const = 0;
    /// 上游的 `barrierCurve`（默认 `Curves.ease`，本仓取 EaseOutQuad，
    /// 见文件头偏离 15）。
    virtual Easing barrierCurve() const { return Easing::EaseOutQuad; }
    /// 上游的 `maintainState`：被上层遮住时是否保持构建。
    virtual bool maintainState() const = 0;
    /// 上游的 `fullscreenDialog`。
    virtual bool fullscreenDialog() const { return false; }
    /// 上游的 `semanticsDismissible`：语义层未接，保留常量口径（偏离 6）。
    bool semanticsDismissible() const { return true; }

    // ── 对外可见的动画（上游 的 _animationProxy 那一层）───────────────────
    SharedPtr<Animation<double>> animation() const override;
    SharedPtr<Animation<double>> secondaryAnimation() const override;

    /// 上游的 `offstage`：为真时内容「建而不画、不命中」，且 barrier 不铺色。
    /// Hero 用它在不显示内容的情况下先量出目标的最终位置。
    bool offstage() const { return offstage_; }
    void setOffstage(bool value);

    /// 上游的 `canPop` / `impliesAppBarDismissal` / `willHandlePopInternally`。
    bool canPop() const;
    bool impliesAppBarDismissal() const;
    bool willHandlePopInternally() const override;
    RoutePopDisposition popDisposition() const override;

    /// 上游的 `subtreeContext`（`_subtreeKey.currentContext`）。本仓取
    /// ModalScope 的 context。
    BuildContext* subtreeContext() const;

    /// 上游的 `setState`：走 scope 的 `_routeSetState`；scope 不在树上时只
    /// 执行回调。
    void setState(std::function<void()> fn);

    // ── 子类覆写的构建（上游 的 buildPage / buildTransitions / buildModalBarrier）
    /// 产出路由的主内容。**每帧只调一次**：结果缓存在 ModalScopeState 里，
    /// 只有依赖变化（`didChangeDependencies`）或 `changedExternalState` 才重来。
    virtual SharedPtr<Widget> buildPage(BuildContext& context,
                                        SharedPtr<Animation<double>> animation,
                                        SharedPtr<Animation<double>> secondaryAnimation) = 0;
    /// 把主内容包成转场。默认实现直接返回 child（上游 同）。
    virtual SharedPtr<Widget> buildTransitions(BuildContext& context,
                                               SharedPtr<Animation<double>> animation,
                                               SharedPtr<Animation<double>> secondaryAnimation,
                                               SharedPtr<Widget> child);

    /// 上游的 `buildModalBarrier`：子类可覆写换整块遮罩（默认给
    /// AnimatedModalBarrier / ModalBarrier，与上游 同一判据）。
    virtual SharedPtr<Widget> buildModalBarrier();

    // ── 本地历史（上游 的 LocalHistoryRoute）──────────────────────────────
    /// 上游的 `addLocalHistoryEntry`。
    ///
    /// 引用由路由与调用方共享：调用方必须自己留一份 `SharedPtr`，否则条目
    /// 加进来的瞬间就随临时对象一起析构。
    void addLocalHistoryEntry(const SharedPtr<LocalHistoryEntry>& entry);
    /// 上游的 `removeLocalHistoryEntry`。
    void removeLocalHistoryEntry(LocalHistoryEntry* entry);
    /// 本地历史条数（调试/测试用）。
    size_t localHistoryCount() const { return localHistory_.size(); }

    // ── 生命周期 ────────────────────────────────────────────────────────
    void install() override;
    bool didPop(SharedPtr<void> result) override;
    void didChangePrevious(Route* previousRoute) override;
    void didChangeNext(Route* nextRoute) override;
    void didPopNext(Route* nextRoute) override;
    void dispose() override;

    /// 上游的 `_modalScope` / `_modalBarrier` 两个字段 + `createOverlayEntries`。
    std::vector<SharedPtr<OverlayEntry>> createOverlayEntries() override;

    /// `ModalScopeState` 的登记口（替代上游 的 GlobalKey，见偏离 7）。
    void attachScope(ModalScopeState* state);
    void detachScope(ModalScopeState* state);

    // ── 上游 的静态查询（都建立在 ModalScopeStatus 上）────────────────────
    /// 上游的 `ModalRoute.of` / `maybeOf`。找不到时返回 nullptr（上游 的
    /// `of` 是 assert，C++ 侧统一交给调用方判空）。
    static ModalRoute* maybeOf(BuildContext& context);
    static ModalRoute* of(BuildContext& context) { return maybeOf(context); }
    /// 上游的 `isCurrentOf` / `canPopOf` / `settingsOf` / `isActiveOf` /
    /// `isFirstOf` / `opaqueOf` / `popDispositionOf`：找不到路由时为空。
    static std::optional<bool> isCurrentOf(BuildContext& context);
    static std::optional<bool> canPopOf(BuildContext& context);
    static const RouteSettings* settingsOf(BuildContext& context);
    static std::optional<bool> isActiveOf(BuildContext& context);
    static std::optional<bool> isFirstOf(BuildContext& context);
    static std::optional<bool> opaqueOf(BuildContext& context);
    static std::optional<RoutePopDisposition> popDispositionOf(BuildContext& context);

    /// 上游的 `ModalRoute.withName`：给 `popUntil` / `pushAndRemoveUntil` 用的
    /// 谓词 —— 「名字是 `name` 的 ModalRoute，且它不自行消化 pop」。
    static std::function<bool(Route*)> withName(const std::string& name);

protected:
    /// 上游的 `changedInternalState`：内部状态（isCurrent / canPop / offstage /
    /// 本地历史）变了 ⇒ 重建 barrier 并把 scope 标脏。
    void changedInternalState() override;
    /// 上游的 `changedExternalState`：宿主 Navigator 的依赖变了 ⇒ barrier
    /// 重建、page 缓存作废。
    void changedExternalState() override;

    /// 上游 的 barrier 外层 `IgnorePointer` 依赖「status 变化时调
    /// changedInternalState」，本仓在状态迁移处补这一次通知（见
    /// `TransitionRoute::handleTransitionStatus` 的注释）。
    void handleTransitionStatus(AnimationStatus status) override;

    /// 上游的 `_buildModalBarrier` / `_buildModalScope`（OverlayEntry 的 builder）。
    SharedPtr<Widget> generateModalBarrier();
    SharedPtr<Widget> generateModalScope();

    bool offstage_ = false;
    ModalScopeState* scopeState_ = nullptr;
    SharedPtr<ProxyAnimation> animationProxy_;
    SharedPtr<ProxyAnimation> secondaryProxyWrapper_;
    /// 上游 的 `_modalScopeCache`：scope 那棵子树的根控件缓存。
    SharedPtr<Widget> scopeCache_;
    OverlayEntry* modalBarrierEntry_ = nullptr;
    OverlayEntry* modalScopeEntry_ = nullptr;

private:
    std::vector<SharedPtr<LocalHistoryEntry>> localHistory_;
    int entriesImpliesAppBarDismissal_ = 0;
};

// ── PopupRoute ──────────────────────────────────────────────────────────────

/// 浮在当前路由之上的模态路由。上游的 `PopupRoute<T>`（routes.dart:2380）。
///
/// 与 PageRoute 的差别只有三条：不透明为假、maintainState 为真、
/// （上游 还有 `allowSnapshotting = false`，本仓未接）。
class PopupRoute : public ModalRoute {
public:
    using ModalRoute::ModalRoute;

    bool opaque() const override { return false; }
    bool maintainState() const override { return true; }
};

// ── RawDialogRoute ──────────────────────────────────────────────────────────

/// 用回调拼装对话框路由。上游的 `RawDialogRoute<T>`（routes.dart:2593）。
///
/// 上游 在 `buildPage` 里还包了 `Semantics` 与 `DisplayFeatureSubScreen`
/// （`anchorPoint`）：前者属语义层（偏离 6），后者本仓没有折叠屏概念，都不采。
class RawDialogRoute : public PopupRoute {
public:
    RawDialogRoute(RoutePageBuilder pageBuilder, bool barrierDismissible = true,
                   std::optional<Color> barrierColor = Color::fromRGBA(0, 0, 0, 0.5),
                   double transitionDurationMs = 200.0,
                   RouteTransitionsBuilder transitionBuilder = nullptr,
                   SharedPtr<const RouteSettings> settings = nullptr)
        : PopupRoute(std::move(settings)), pageBuilder_(std::move(pageBuilder)),
          barrierDismissible_(barrierDismissible), barrierColor_(barrierColor),
          transitionDurationMs_(transitionDurationMs),
          transitionBuilder_(std::move(transitionBuilder)) {}

    bool barrierDismissible() const override { return barrierDismissible_; }
    std::optional<Color> barrierColor() const override { return barrierColor_; }
    double transitionDuration() const override { return transitionDurationMs_; }

    SharedPtr<Widget> buildPage(BuildContext& context,
                                SharedPtr<Animation<double>> animation,
                                SharedPtr<Animation<double>> secondaryAnimation) override;
    SharedPtr<Widget> buildTransitions(BuildContext& context,
                                       SharedPtr<Animation<double>> animation,
                                       SharedPtr<Animation<double>> secondaryAnimation,
                                       SharedPtr<Widget> child) override;

private:
    RoutePageBuilder pageBuilder_;
    bool barrierDismissible_;
    std::optional<Color> barrierColor_;
    double transitionDurationMs_;
    RouteTransitionsBuilder transitionBuilder_;
};

// ── RouteObserver / RouteAware ──────────────────────────────────────────────

/// 关心自己所在路由变化的对象。上游的 `RouteAware`（routes.dart:2517）。
class RouteAware {
public:
    virtual ~RouteAware() = default;
    /// 压在本路由之上的那条已弹出，本路由重新成为栈顶。
    virtual void didPopNext() {}
    /// 本路由被压入。
    virtual void didPush() {}
    /// 本路由被弹出。
    virtual void didPop() {}
    /// 本路由之上新压了一条路由。
    virtual void didPushNext() {}
};

/// 把「同类型路由之间的切换」通知给 `RouteAware`。
/// 上游的 `RouteObserver<R extends Route>`（routes.dart:2433）。
///
/// C++ 没有 Dart 的 `Map<R, Set<RouteAware>>` 泛型约束，这里用 `R*` 作键；
/// `route is R` 的判定换成 `dynamic_cast`。
template <typename R>
class RouteObserver : public NavigatorObserver {
public:
    /// 上游的 `subscribe`：首次订阅立刻收一次 `didPush`。
    void subscribe(RouteAware* routeAware, R* route) {
        if (!routeAware || !route)
            return;
        auto it = listeners_.find(route);
        if (it == listeners_.end()) {
            listeners_.emplace(route, std::vector<RouteAware*>{routeAware});
            routeAware->didPush();
            return;
        }
        auto& subscribers = it->second;
        if (std::find(subscribers.begin(), subscribers.end(), routeAware) ==
            subscribers.end()) {
            subscribers.push_back(routeAware);
            routeAware->didPush();
        }
    }

    /// 上游的 `unsubscribe`：从**所有**路由的订阅表里摘掉（一次/每类）。
    void unsubscribe(RouteAware* routeAware) {
        for (auto it = listeners_.begin(); it != listeners_.end();) {
            auto& subscribers = it->second;
            subscribers.erase(
                    std::remove(subscribers.begin(), subscribers.end(), routeAware),
                    subscribers.end());
            if (subscribers.empty())
                it = listeners_.erase(it);
            else
                ++it;
        }
    }

    void didPop(Route* route, Route* previousRoute) override {
        auto* popped = route ? dynamic_cast<R*>(route) : nullptr;
        auto* previous = previousRoute ? dynamic_cast<R*>(previousRoute) : nullptr;
        if (!popped || !previous)
            return;
        // 上游 先通知「下面那条」的订阅者 didPopNext，再通知被弹的 didPop。
        if (auto it = listeners_.find(previous); it != listeners_.end()) {
            auto subscribers = it->second;
            for (auto* aware : subscribers)
                aware->didPopNext();
        }
        if (auto it = listeners_.find(popped); it != listeners_.end()) {
            auto subscribers = it->second;
            for (auto* aware : subscribers)
                aware->didPop();
        }
    }

    void didPush(Route* route, Route* previousRoute) override {
        auto* pushed = route ? dynamic_cast<R*>(route) : nullptr;
        auto* previous = previousRoute ? dynamic_cast<R*>(previousRoute) : nullptr;
        if (!pushed || !previous)
            return;
        auto it = listeners_.find(previous);
        if (it == listeners_.end())
            return;
        auto subscribers = it->second;
        for (auto* aware : subscribers)
            aware->didPushNext();
    }

    /// 上游的 `debugObservingRoute`。
    bool debugObservingRoute(R* route) const { return listeners_.count(route) > 0; }

private:
    std::map<R*, std::vector<RouteAware*>> listeners_;
};

}  // namespace cui_core
