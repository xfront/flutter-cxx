#pragma once
/// @file widgets/router.h
/// @brief 上游的 Router 体系移植。
///
/// 对应 上游源码：
///   上游 widgets/router.dart
///
/// 覆盖范围：
///   RouteInformation / RouteInformationParser / RouterDelegate
///   RouteInformationProvider / PlatformRouteInformationProvider
///   BackButtonDispatcher / RootBackButtonDispatcher / ChildBackButtonDispatcher
///   Router / RouterState / RouterScope
///   RouteInformationReportingType
///
/// 官方语义要点（全部以 上游 widgets/router.dart 为准）：
///
/// 1. **Router 是「四件套」的接线盒**（router.dart:358-470）
///    `routeInformationProvider` / `routeInformationParser` /
///    `routerDelegate` / `backButtonDispatcher`。
///    Provider 与 Parser 必须同时提供或同时不提供。
///
/// 2. **数据流向**（router.dart:608-860）
///    Provider 通知 → `_processRouteInformation` → Parser 解析成配置 T
///    → `RouterDelegate.setNewRoutePath(T)` → `_rebuild()` → 重建子树。
///    首次解析走 `setInitialRoutePath`（router.dart:634-651）。
///
/// 3. **事务令牌丢弃过期结果**（router.dart:771-800）
///    每次解析都会生成一个新的 `_currentRouterTransaction`；
///    异步解析回来时若令牌已变，结果被丢弃（router.dart:788-790）。
///
/// 4. **Delegate 通知即重建**（router.dart:834-840）
///    `RouterDelegate` 是 `Listenable`；它通知监听器即意味着「我准备好重建了」。
///
/// 5. **回退按钮的冒泡顺序**（router.dart:990-1030）
///    `BackButtonDispatcher.invokeCallback` **逆序**遍历子 dispatcher，
///    第一个返回 true 的子节点吞掉事件；全部不处理才由自己处理。
///    `deferTo` 把子节点移到集合末尾（= 优先级最高，因为逆序遍历）。
///
/// 6. **回传路由信息**（router.dart:660-700）
///    重建后取 `RouterDelegate.currentConfiguration`，
///    经 `Parser.restoreRouteInformation` 得到 `RouteInformation`，
///    再交给 `Provider.routerReportsNewRouteInformation`。
///    `Router.neglect` / `Router.navigate`（router.dart:524-556）
///    强制本次回传的报告类型为 neglect / navigate。
///
/// ── 与 Dart 泛型的差异 ──
/// Dart 的 `RouteInformationParser<T>` / `RouterDelegate<T>` / `Router<T>`
/// 以泛型 `T`（路由配置类型）串联。本移植沿用 Navigator 移植的
/// **类型擦除** 做法：配置对象统一为 `SharedPtr<void>`，
/// 由使用方自行 downcast，语义与 Dart 的 `T` 等价。

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/change_notifier.h"
#include "cui/framework/framework.h"

namespace cui {

class RouterState;
class RouterScopeData;
class ChildBackButtonDispatcher;

// ── RouteInformation ────────────────────────────────────────────────────────

/// 一次路由信息快照。上游的 `RouteInformation`（router.dart:49-135）。
///
/// 上游 用 `Uri` 描述位置；本移植用 `std::string` 保存 URI 字面量
/// （含 path / query / fragment），语义等价。
class RouteInformation {
public:
    RouteInformation() = default;
    explicit RouteInformation(std::string uri, SharedPtr<void> state = nullptr)
        : uri_(std::move(uri)), state_(std::move(state)) {}

    /// 位置。上游的 `RouteInformation.uri`。
    const std::string& uri() const { return uri_; }
    void setUri(std::string uri) { uri_ = std::move(uri); }

    /// 位置的字符串形式（上游 已废弃的 `location`）。
    const std::string& location() const { return uri_; }

    /// 应用状态（滚动位置、文本框内容等）。上游的 `RouteInformation.state`。
    SharedPtr<void> state() const { return state_; }
    void setState(SharedPtr<void> state) { state_ = std::move(state); }

    bool operator==(const RouteInformation& other) const {
        return uri_ == other.uri_ && state_ == other.state_;
    }
    bool operator!=(const RouteInformation& other) const { return !(*this == other); }

private:
    std::string uri_ = "/";
    SharedPtr<void> state_;
};

// ── RouteInformationParser ──────────────────────────────────────────────────

/// 把 `RouteInformation` 解析成路由配置。上游的
/// `RouteInformationParser<T>`（router.dart:1245-1310）。
///
/// 上游的解析是异步的（返回 `Future<T>`）。本移植用回调表达同一语义：
/// `completion` 可能在**稍后**被调用，Router 用事务令牌丢弃过期结果
/// （对应 router.dart:788-790）。
class RouteInformationParser {
public:
    /// 解析完成回调。参数为解析出的配置（对应 Dart 的 `T`）。
    using ParseCallback = std::function<void(SharedPtr<void>)>;

    virtual ~RouteInformationParser() = default;

    /// 把路由信息解析成配置。上游的 `parseRouteInformation`。
    virtual void parseRouteInformation(const RouteInformation& routeInformation,
                                       ParseCallback completion) = 0;

    /// 带依赖的解析。上游的 `parseRouteInformationWithDependencies`
    /// （router.dart:1290-1294）：默认转调 `parseRouteInformation`。
    virtual void parseRouteInformationWithDependencies(
        const RouteInformation& routeInformation, BuildContext& context,
        ParseCallback completion) {
        parseRouteInformation(routeInformation, std::move(completion));
    }

    /// 由配置反推路由信息。上游的 `restoreRouteInformation`
    /// （router.dart:1299-1300）：默认返回空（表示不上报、不参与状态恢复）。
    virtual SharedPtr<RouteInformation> restoreRouteInformation(
        const SharedPtr<void>& configuration) {
        (void)configuration;
        return nullptr;
    }
};

// ── RouterDelegate ──────────────────────────────────────────────────────────

/// 路由委托：消费解析结果并构建导航子树。
/// 上游的 `RouterDelegate<T>`（router.dart:1340-1450）。
///
/// 上游 中它是 `Listenable`：通知监听器即意味着「请重建」。
/// 本移植直接继承 `ChangeNotifier`（`Listenable` 的具体实现）。
class RouterDelegate : public ChangeNotifier {
public:
    ~RouterDelegate() override = default;

    /// 首次路由。上游的 `setInitialRoutePath`（router.dart:1380-1382）：
    /// 默认转调 `setNewRoutePath`。
    virtual void setInitialRoutePath(SharedPtr<void> configuration) {
        setNewRoutePath(std::move(configuration));
    }

    /// 状态恢复时的路由。上游的 `setRestoredRoutePath`
    /// （router.dart:1396-1398）：默认转调 `setNewRoutePath`。
    virtual void setRestoredRoutePath(SharedPtr<void> configuration) {
        setNewRoutePath(std::move(configuration));
    }

    /// 新的路由。上游的 `setNewRoutePath`（router.dart:1406）。
    virtual void setNewRoutePath(SharedPtr<void> configuration) = 0;

    /// 处理返回请求。上游的 `popRoute`（router.dart:1420）。
    /// 返回 true 表示已处理（不再向上冒泡）。
    virtual bool popRoute() = 0;

    /// 当前配置。上游的 `currentConfiguration`（router.dart:1439）。
    /// 返回空表示不参与 URL 回传与状态恢复。
    virtual SharedPtr<void> currentConfiguration() const { return nullptr; }

    /// 构建导航子树。上游的 `build`（router.dart:1448）。
    virtual SharedPtr<Widget> build(BuildContext& context) = 0;
};

// ── RouteInformationProvider ────────────────────────────────────────────────

/// Router 报告新路由信息时的意图。
/// 上游的 `RouteInformationReportingType`（router.dart:585-606）。
enum class RouteInformationReportingType {
    /// 无特定意图（默认）：由 Router 自行判断是否算一次导航。
    none,
    /// 来自 `Router.neglect` 调用：更新当前历史条目，不新增。
    neglect,
    /// 来自 `Router.navigate` 调用：强制新增历史条目。
    navigate,
};

/// 路由信息提供者。上游的 `RouteInformationProvider`（router.dart:1469-1495）。
///
/// 上游 中它是 `ValueListenable<RouteInformation>`；本移植同样实现
/// `ValueListenable<RouteInformation>` 接口。
class RouteInformationProvider : public ValueListenable<RouteInformation> {
public:
    ~RouteInformationProvider() override = default;

    /// 当前路由信息。上游的 `value`。
    RouteInformation value() const override { return value_; }

    /// 注册监听器。上游的 `addListener`。
    void addListener(std::function<void()> listener) override {
        addListenerWithHandle(std::move(listener));
    }

    /// 移除监听器（按函数对象相等比较）。
    void removeListener(const std::function<void()>& listener) override {
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if (sameCallbackTarget(it->second, listener)) {
                listeners_.erase(it);
                return;
            }
        }
    }

    /// 注册监听器并返回句柄（用于精确移除）。
    int addListenerWithHandle(std::function<void()> listener);

    /// 按句柄移除监听器。
    void removeListenerByHandle(int handle);

    /// Router 报告了新的路由信息。上游的
    /// `routerReportsNewRouteInformation`（router.dart:1481-1491）：默认空实现。
    virtual void routerReportsNewRouteInformation(
        const RouteInformation& routeInformation,
        RouteInformationReportingType type = RouteInformationReportingType::none) {
        (void)routeInformation;
        (void)type;
    }

protected:
    /// 更新值并通知监听器。
    void setValue(RouteInformation routeInformation) {
        value_ = std::move(routeInformation);
        notifyListeners();
    }

    /// 通知所有监听器（快照派发）。
    void notifyListeners();

    RouteInformation value_;

private:
    std::vector<std::pair<int, std::function<void()>>> listeners_;
    int nextHandle_ = 1;
};

// ── PlatformRouteInformationProvider ────────────────────────────────────────

/// 平台路由信息提供者。上游的 `PlatformRouteInformationProvider`
/// （router.dart:1506-1583）。
///
/// 上游 版本通过 `SystemChannels.navigation` 与引擎双向通信；
/// 本移植保留「平台侧上报 → 通知 Router」与「Router 侧上报 → 记录新值」
/// 两条数据流，引擎通道以 `onRouteInformationUpdated` 回调替代。
class PlatformRouteInformationProvider : public RouteInformationProvider {
public:
    explicit PlatformRouteInformationProvider(RouteInformation initialRouteInformation)
        : RouteInformationProvider() {
        value_ = std::move(initialRouteInformation);
    }

    /// Router 报告新路由信息。上游的同名覆写（router.dart:1521-1538）：
    /// 更新内部值，并通知引擎（此处以回调替代引擎通道）。
    void routerReportsNewRouteInformation(
        const RouteInformation& routeInformation,
        RouteInformationReportingType type = RouteInformationReportingType::none) override;

    /// 平台侧上报新的路由信息。上游的 `_platformReportsNewRouteInformation`
    /// （router.dart:1545-1553）：值不同才更新并通知。
    void didPushRouteInformation(const RouteInformation& routeInformation);

    /// 引擎侧回传回调。参数：路由信息、是否替换当前历史条目。
    using EngineCallback = std::function<void(const RouteInformation&, bool replace)>;
    void setEngineCallback(EngineCallback callback) { engineCallback_ = std::move(callback); }

private:
    EngineCallback engineCallback_;
};

// ── BackButtonDispatcher ────────────────────────────────────────────────────

/// 返回键分发器。上游的 `BackButtonDispatcher`（router.dart:971-1088）。
///
/// 上游的回调返回 `Future<bool>`（true = 已处理）。
/// 本移植用同步 `bool` 表达同一语义。
class BackButtonDispatcher {
public:
    /// 回调：返回 true 表示已处理该返回请求。
    using Callback = std::function<bool()>;

    virtual ~BackButtonDispatcher() = default;

    /// 是否已注册回调。上游的 `hasCallbacks`。
    bool hasCallbacks() const { return !callbacks_.empty() || !children_.empty(); }

    /// 注册回调并返回句柄。
    int addCallback(Callback callback);
    /// 按句柄移除回调（可被子类覆写）。
    virtual void removeCallbackByHandle(int handle);
    /// 按函数对象相等移除回调。
    void removeCallback(const Callback& callback);

    /// 处理返回请求。上游的 `invokeCallback`（router.dart:990-1018）。
    ///
    /// **逆序**遍历子 dispatcher，第一个返回 true 的子节点吞掉事件；
    /// 若都没有处理，则调用本 dispatcher 自己最后注册的回调。
    virtual bool invokeCallback(bool defaultValue = false);

    /// 创建子分发器。上游的 `createChildBackButtonDispatcher`
    /// （router.dart:1023-1027）。
    SharedPtr<ChildBackButtonDispatcher> createChildBackButtonDispatcher();

    /// 在同级中取得优先级。上游的 `takePriority`（router.dart:1030-1035）：
    /// 清空自身的子节点（不再向下委托）。
    virtual void takePriority();

    /// 把某个子节点提到最高优先级。上游的 `deferTo`（router.dart:1046-1057）：
    /// 子节点被移到集合末尾（逆序遍历时最先被询问）。
    virtual void deferTo(const SharedPtr<ChildBackButtonDispatcher>& child);

    /// 移除子节点。上游的 `forget`（router.dart:1064-1066）。
    void forget(const SharedPtr<ChildBackButtonDispatcher>& child);

protected:
    /// 子分发器（按「委托顺序」排列，越靠后优先级越高）。
    std::vector<SharedPtr<ChildBackButtonDispatcher>> children_;

    /// 自身注册的回调。
    std::vector<std::pair<int, Callback>> callbacks_;
    int nextHandle_ = 1;
};

/// 子返回键分发器。上游的 `ChildBackButtonDispatcher`（router.dart:1124-1163）。
///
/// 用于嵌套 Router：子 Router 把父 Router 的 dispatcher 作为 parent，
/// 通过 `takePriority` 抢占返回事件。
///
/// **所有权说明**：子分发器总是由 `createChildBackButtonDispatcher()`
/// 以 `shared_ptr` 创建，因此本类可以安全地用 `shared_from_this()`
/// 把自己交回给父节点；而父分发器（尤其是 `RootBackButtonDispatcher`）
/// 可能是在栈上构造的，因此父引用用**裸指针**保存。
class ChildBackButtonDispatcher : public BackButtonDispatcher,
                                  public EnableSharedFromThis<ChildBackButtonDispatcher> {
public:
    explicit ChildBackButtonDispatcher(BackButtonDispatcher* parent) : parent_(parent) {}

    /// 父分发器。上游的 `parent`（router.dart:1133）。
    BackButtonDispatcher* parent() const { return parent_; }

    /// 父节点通知本节点处理返回请求。上游的 `notifiedByParent`
    /// （router.dart:1139-1141）。
    bool notifiedByParent(bool defaultValue = false) { return invokeCallback(defaultValue); }

    /// 抢占父节点的优先级。上游的 `takePriority`（router.dart:1143-1147）。
    void takePriority() override;

    /// 把某个子节点提到最高优先级。上游的 `deferTo`（router.dart:1149-1154）。
    void deferTo(const SharedPtr<ChildBackButtonDispatcher>& child) override;

    /// 移除回调。上游的 `removeCallback`（router.dart:1156-1161）：
    /// 自身无回调后向父节点注销。
    void removeCallbackByHandle(int handle) override;

private:
    BackButtonDispatcher* parent_ = nullptr;
};

/// 根返回键分发器。上游的 `RootBackButtonDispatcher`（router.dart:1091-1122）。
///
/// 上游 版本监听平台的 pop route 通知；本移植以 `didPopRoute()`
/// 暴露给平台层调用。
class RootBackButtonDispatcher : public BackButtonDispatcher {
public:
    /// 平台请求返回。上游的 `didPopRoute`（router.dart:1120）。
    bool didPopRoute() { return invokeCallback(false); }
};

// ── RouterScope ─────────────────────────────────────────────────────────────

/// `_RouterScope` 承载的数据。上游的 `_RouterScope`（router.dart:860-880）。
struct RouterScopeData {
    SharedPtr<RouteInformationProvider> routeInformationProvider;
    SharedPtr<RouteInformationParser> routeInformationParser;
    SharedPtr<RouterDelegate> routerDelegate;
    SharedPtr<BackButtonDispatcher> backButtonDispatcher;
    /// 拥有本 Scope 的 RouterState（非拥有）。
    RouterState* routerState = nullptr;

    bool operator==(const RouterScopeData& o) const {
        return routeInformationProvider == o.routeInformationProvider &&
               routeInformationParser == o.routeInformationParser &&
               routerDelegate == o.routerDelegate &&
               backButtonDispatcher == o.backButtonDispatcher &&
               routerState == o.routerState;
    }
};

/// Router 把四个 delegate 暴露给子树的通道。
/// 上游的 `_RouterScope`（router.dart:860-880）。
class RouterScope : public InheritedWidget<RouterScopeData> {
public:
    explicit RouterScope(RouterScopeData data, SharedPtr<Widget> child = nullptr)
        : InheritedWidget<RouterScopeData>(nullptr, std::move(child)), data_(std::move(data)) {}

    SharedPtr<const RouterScopeData> data() const override {
        return cui::make_shared<RouterScopeData>(data_);
    }

private:
    RouterScopeData data_;
};

// ── RouterState ─────────────────────────────────────────────────────────────

/// Router 的状态。上游的 `_RouterState<T>`（router.dart:608-858）。
///
/// 职责：
///   - 订阅 Provider / Dispatcher / Delegate 三方的通知
///   - 驱动「解析 → 设置委托 → 重建」的数据流
///   - 维护事务令牌，丢弃过期的异步解析结果
///   - 重建后把新配置回传给 Provider
class RouterState : public State {
public:
    const SharedPtr<RouteInformationProvider>& routeInformationProvider() const {
        return provider_;
    }
    const SharedPtr<RouteInformationParser>& routeInformationParser() const {
        return parser_;
    }
    const SharedPtr<RouterDelegate>& routerDelegate() const { return delegate_; }
    const SharedPtr<BackButtonDispatcher>& backButtonDispatcher() const {
        return dispatcher_;
    }

    /// 本次回传的报告类型。上游的 `_currentIntentionToReport`。
    RouteInformationReportingType reportType() const { return reportType_; }

    /// 带明确报告状态执行一段逻辑。上游的
    /// `_setStateWithExplicitReportStatus`（router.dart:686-712）。
    void setStateWithExplicitReportStatus(RouteInformationReportingType status,
                                          const std::function<void()>& fn);

protected:
    void initState() override;
    void didChangeDependencies() override;
    void didUpdateWidget(Widget* oldWidget) override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    /// 处理 Provider 的新路由信息。上游的
    /// `_handleRouteInformationProviderNotification`（router.dart:802-807）。
    void handleRouteInformationProviderNotification();

    /// 处理返回键事件。上游的
    /// `_handleBackButtonDispatcherNotification`（router.dart:809-815）。
    bool handleBackButtonDispatcherNotification();

    /// 处理 Delegate 的重建通知。上游的
    /// `_handleRouterDelegateNotification`（router.dart:834-840）。
    void handleRouterDelegateNotification();

    /// 解析并应用路由信息。上游的 `_processRouteInformation`
    /// （router.dart:771-780）。
    void processRouteInformation(const RouteInformation& information, bool initial);

    /// 重建。上游的 `_rebuild`（router.dart:828-832）。
    void rebuild();

    /// 重建后回传路由信息。上游的 `_maybeNeedToReportRouteInformation`
    /// （router.dart:698-702）。
    void maybeNeedToReportRouteInformation();

    SharedPtr<RouteInformationProvider> provider_;
    SharedPtr<RouteInformationParser> parser_;
    SharedPtr<RouterDelegate> delegate_;
    SharedPtr<BackButtonDispatcher> dispatcher_;

    /// 事务令牌。上游的 `_currentRouterTransaction`（router.dart:612）。
    int transaction_ = 0;
    /// 是否已完成首次解析。
    bool initialParsed_ = false;
    /// 本次回传的报告意图。
    RouteInformationReportingType reportType_ = RouteInformationReportingType::none;

    int providerListenerHandle_ = 0;
    int delegateListenerHandle_ = 0;
    int dispatcherCallbackHandle_ = 0;
};

// ── Router ──────────────────────────────────────────────────────────────────

/// 路由部件。上游的 `Router<T>`（router.dart:358-556）。
///
/// 把 Provider / Parser / Delegate / Dispatcher 接到一起，
/// 构成一条完整的「平台路由 → 配置 → 导航子树」链路。
class Router : public StatefulWidget {
public:
    /// 构造一个 Router。
    ///
    /// `routeInformationProvider` 与 `routeInformationParser` 必须同时提供
    /// 或同时不提供（对齐 router.dart:367-374 的断言）。
    Router(SharedPtr<RouterDelegate> routerDelegate,
           SharedPtr<RouteInformationParser> routeInformationParser = nullptr,
           SharedPtr<RouteInformationProvider> routeInformationProvider = nullptr,
           SharedPtr<BackButtonDispatcher> backButtonDispatcher = nullptr,
           std::string restorationScopeId = "", KeyPtr key = nullptr);

    const SharedPtr<RouteInformationProvider>& routeInformationProvider() const {
        return routeInformationProvider_;
    }
    const SharedPtr<RouteInformationParser>& routeInformationParser() const {
        return routeInformationParser_;
    }
    const SharedPtr<RouterDelegate>& routerDelegate() const { return routerDelegate_; }
    const SharedPtr<BackButtonDispatcher>& backButtonDispatcher() const {
        return backButtonDispatcher_;
    }
    const std::string& restorationScopeId() const { return restorationScopeId_; }

    /// 取最近的祖先 Router。上游的 `Router.of`（router.dart:479-497）。
    static Router* of(BuildContext& context);

    /// 取最近的祖先 Router，没有则返回空。上游的 `Router.maybeOf`
    /// （router.dart:508-511）。
    static Router* maybeOf(BuildContext& context);

    /// 强制本次状态变化产生一条新的历史条目。
    /// 上游的 `Router.navigate`（router.dart:524-534）。
    static void navigate(BuildContext& context, const std::function<void()>& callback);

    /// 强制本次状态变化不产生新的历史条目。
    /// 上游的 `Router.neglect`（router.dart:545-556）。
    static void neglect(BuildContext& context, const std::function<void()>& callback);

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<RouteInformationProvider> routeInformationProvider_;
    SharedPtr<RouteInformationParser> routeInformationParser_;
    SharedPtr<RouterDelegate> routerDelegate_;
    SharedPtr<BackButtonDispatcher> backButtonDispatcher_;
    std::string restorationScopeId_;
};

}
