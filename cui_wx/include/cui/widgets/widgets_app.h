/// @file widgets_app.h
/// @brief WidgetsApp —— 应用壳。上游的 `WidgetsApp`（widgets/app.dart:290）。
///
/// 官方语义要点（以 上游 widgets/app.dart 为准）：
///
/// 1. **三种路由模式**（app.dart:1487-1516 `_updateRouting`）
///    - Router 委托模式：`routerDelegate != null` → 构造 `Router`；
///    - Navigator 模式：`home != null || routes 非空 || onGenerateRoute
///      != null || onUnknownRoute != null` → 构造 `Navigator`；
///    - 纯 builder 模式：仅 `builder` → 不构造 routing 子树。
///
/// 2. **默认路由名**（app.dart:1429-1433 `_initialRouteName`）
///    官方优先取 `platformDispatcher.defaultRouteName`；本移植无平台
///    调度器，降级为 `initialRoute` 为空时取 `Navigator.defaultRouteName`。
///
/// 3. **默认路由生成**（app.dart:1595-1612 `_onGenerateRoute`）
///    查找顺序：`'/'` 且有 `home` → home；`routes` 表；
///    `pageRouteBuilder` 包裹；再退到 `onGenerateRoute` 回调。
///
/// 4. **未知路由**（app.dart:1614-1633 `_onUnknownRoute`）
///    `onUnknownRoute` 回调**不得返回 null**（官方断言，这里抛异常）。
///
/// 5. **系统回退**（app.dart:1637-1655 `didPopRoute`）
///    Router 委托模式不处理（返回 false）；Navigator 模式转发给
///    `NavigatorState.maybePop`。
///
/// 降级说明（无 Material / Localizations / Shortcuts / Semantics 层）：
/// - 不含 `DefaultTextStyle` / `Title` / `CheckedModeBanner` / `Localizations`
///   / `Shortcuts` / `Actions` / `FocusTraversalGroup` / `TapRegionSurface`；
///   build 只产出 routing 子树（或经 `builder` 包装后的结果）。
/// - `title` / `color` 仅作为配置保存，不渲染窗口标题。

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/key.h"
#include "cui/framework/framework.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/router.h"

namespace cui {

class WidgetsApp;

// ── WidgetsApp ──────────────────────────────────────────────────────────────

/// 应用壳 Widget。上游的 `WidgetsApp`（app.dart:290-1425）。
class WidgetsApp : public StatefulWidget {
public:
    /// 页面内容构造器。上游的 `WidgetBuilder`。
    using WidgetBuilder = std::function<SharedPtr<Widget>(BuildContext&)>;

    /// 路由工厂。上游的 `RouteFactory`。
    using RouteFactory = std::function<SharedPtr<Route>(const RouteSettings&)>;

    /// 页面路由工厂。上游的 `pageRouteBuilder`
    /// （app.dart:364-365）：用给定 settings 与内容构造器创建路由。
    using PageRouteFactory =
            std::function<SharedPtr<Route>(const RouteSettings&, WidgetBuilder)>;

    /// 应用级包装器。上游的 `builder`（app.dart:375）。
    using AppBuilder =
            std::function<SharedPtr<Widget>(BuildContext&, const SharedPtr<Widget>&)>;

    /// 初始路由列表工厂。上游的 `onGenerateInitialRoutes`（app.dart:347）。
    using InitialRoutesFactory = std::function<std::vector<std::string>(const std::string&)>;

    /// 路由表。上游的 `Map<String, WidgetBuilder> routes`（app.dart:368）。
    using RoutesMap = std::map<std::string, WidgetBuilder>;

    /// 命令式（Navigator）模式构造。上游的 `WidgetsApp` 主构造
    /// （app.dart:336-467）。
    explicit WidgetsApp(KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)) {}

    // ── Navigator 模式配置 ──────────────────────────────────────────────

    /// Navigator 的 GlobalKey。上游的 `navigatorKey`（app.dart:338）。
    const SharedPtr<GlobalKey>& navigatorKey() const { return navigatorKey_; }
    void setNavigatorKey(SharedPtr<GlobalKey> key) { navigatorKey_ = std::move(key); }

    /// 初始路由名。上游的 `initialRoute`（app.dart:346）。
    const std::string& initialRoute() const { return initialRoute_; }
    void setInitialRoute(std::string name) { initialRoute_ = std::move(name); }

    /// 初始路由列表工厂。上游的 `onGenerateInitialRoutes`（app.dart:347）。
    const InitialRoutesFactory& onGenerateInitialRoutes() const {
        return onGenerateInitialRoutes_;
    }
    void setOnGenerateInitialRoutes(InitialRoutesFactory factory) {
        onGenerateInitialRoutes_ = std::move(factory);
    }

    /// 路由生成回调。上游的 `onGenerateRoute`（app.dart:339）。
    const RouteFactory& onGenerateRoute() const { return onGenerateRoute_; }
    void setOnGenerateRoute(RouteFactory factory) { onGenerateRoute_ = std::move(factory); }

    /// 未知路由兜底回调。上游的 `onUnknownRoute`（app.dart:340）。
    const RouteFactory& onUnknownRoute() const { return onUnknownRoute_; }
    void setOnUnknownRoute(RouteFactory factory) { onUnknownRoute_ = std::move(factory); }

    /// 页面路由工厂。上游的 `pageRouteBuilder`（app.dart:349）。
    const PageRouteFactory& pageRouteBuilder() const { return pageRouteBuilder_; }
    void setPageRouteBuilder(PageRouteFactory factory) { pageRouteBuilder_ = std::move(factory); }

    /// 首页。上游的 `home`（app.dart:350）。
    const SharedPtr<Widget>& home() const { return home_; }
    void setHome(SharedPtr<Widget> home) { home_ = std::move(home); }

    /// 路由表。上游的 `routes`（app.dart:368）。
    const RoutesMap& routes() const { return routes_; }
    void setRoutes(RoutesMap routes) { routes_ = std::move(routes); }

    /// Navigator 观察者。上游的 `navigatorObservers`（app.dart:343）。
    const std::vector<NavigatorObserver*>& navigatorObservers() const {
        return navigatorObservers_;
    }
    void setNavigatorObservers(std::vector<NavigatorObserver*> observers) {
        navigatorObservers_ = std::move(observers);
    }

    // ── Router 模式配置 ─────────────────────────────────────────────────

    /// 路由信息提供者。上游的 `routeInformationProvider`（app.dart:430）。
    const SharedPtr<RouteInformationProvider>& routeInformationProvider() const {
        return routeInformationProvider_;
    }
    void setRouteInformationProvider(SharedPtr<RouteInformationProvider> provider) {
        routeInformationProvider_ = std::move(provider);
    }

    /// 路由信息解析器。上游的 `routeInformationParser`（app.dart:431）。
    const SharedPtr<RouteInformationParser>& routeInformationParser() const {
        return routeInformationParser_;
    }
    void setRouteInformationParser(SharedPtr<RouteInformationParser> parser) {
        routeInformationParser_ = std::move(parser);
    }

    /// 路由委托。上游的 `routerDelegate`（app.dart:432）。
    const SharedPtr<RouterDelegate>& routerDelegate() const { return routerDelegate_; }
    void setRouterDelegate(SharedPtr<RouterDelegate> delegate) {
        routerDelegate_ = std::move(delegate);
    }

    /// 返回键分发器。上游的 `backButtonDispatcher`（app.dart:434）。
    const SharedPtr<BackButtonDispatcher>& backButtonDispatcher() const {
        return backButtonDispatcher_;
    }
    void setBackButtonDispatcher(SharedPtr<BackButtonDispatcher> dispatcher) {
        backButtonDispatcher_ = std::move(dispatcher);
    }

    // ── 通用配置 ────────────────────────────────────────────────────────

    /// 应用级包装器。上游的 `builder`（app.dart:375）。
    const AppBuilder& builder() const { return builder_; }
    void setBuilder(AppBuilder builder) { builder_ = std::move(builder); }

    /// 窗口标题（本移植仅保存，不渲染 Title widget）。
    const std::string& title() const { return title_; }
    void setTitle(std::string title) { title_ = std::move(title); }

    std::unique_ptr<State> createState() override;

private:
    // Navigator 模式
    SharedPtr<GlobalKey> navigatorKey_;
    std::string initialRoute_;
    InitialRoutesFactory onGenerateInitialRoutes_;
    RouteFactory onGenerateRoute_;
    RouteFactory onUnknownRoute_;
    PageRouteFactory pageRouteBuilder_;
    SharedPtr<Widget> home_;
    RoutesMap routes_;
    std::vector<NavigatorObserver*> navigatorObservers_;

    // Router 模式
    SharedPtr<RouteInformationProvider> routeInformationProvider_;
    SharedPtr<RouteInformationParser> routeInformationParser_;
    SharedPtr<RouterDelegate> routerDelegate_;
    SharedPtr<BackButtonDispatcher> backButtonDispatcher_;

    // 通用
    AppBuilder builder_;
    std::string title_;
};

// ── WidgetsAppState ─────────────────────────────────────────────────────────

/// 应用壳状态。上游的 `_WidgetsAppState`（app.dart:1427-1857）。
class WidgetsAppState : public State {
public:
    void initState() override;
    void didUpdateWidget(cui::Widget* oldWidget) override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

    /// 系统回退请求。上游的 `didPopRoute`（app.dart:1637-1655）。
    /// 返回 true 表示已处理。
    bool didPopRoute();

    /// 系统推送路由信息。上游的 `didPushRouteInformation`
    /// （app.dart:1657-1677）。返回 true 表示已处理。
    bool didPushRouteInformation(const RouteInformation& routeInformation);

private:
    friend class WidgetsApp;

    /// 初始路由名。上游的 `_initialRouteName`（app.dart:1429-1433）。
    /// 无平台调度器，降级为 `initialRoute` 为空时取 `'/'`。
    std::string initialRouteName() const;

    /// 是否使用 Router 委托模式。上游的 `_usesRouterWithDelegates`
    /// （app.dart:1518）。
    bool usesRouterWithDelegates() const;

    /// 是否使用 Navigator 模式。上游的 `_usesNavigator`
    /// （app.dart:1522-1526）。
    bool usesNavigator() const;

    /// 默认路由生成。上游的 `_onGenerateRoute`（app.dart:1595-1612）。
    SharedPtr<Route> onGenerateRoute(const RouteSettings& settings);

    /// 未知路由兜底。上游的 `_onUnknownRoute`（app.dart:1614-1633）。
    SharedPtr<Route> onUnknownRoute(const RouteSettings& settings);

    /// 路由模式切换。上游的 `_updateRouting`（app.dart:1487-1516）。
    void updateRouting(const WidgetsApp* oldWidget);

    /// 生效的路由信息提供者：用户提供的优先，否则用内部默认。
    /// 上游的 `_effectiveRouteInformationProvider`（app.dart:1532-1534）。
    SharedPtr<RouteInformationProvider> effectiveRouteInformationProvider() const;

    /// 生效的返回键分发器。上游的 `_effectiveBackButtonDispatcher`
    /// （app.dart:1535-1537）。
    SharedPtr<BackButtonDispatcher> effectiveBackButtonDispatcher() const;

    /// 取 NavigatorState（通过 navigator key 定位）。无则返回 nullptr。
    NavigatorState* navigatorState() const;

    /// 平台返回键处理器（services::SystemNavigator 注册）。
    /// Router 模式交给 BackButtonDispatcher，其余模式走 didPopRoute()。
    bool handleBackButton();

    /// 返回键注册句柄；0 = 未注册。
    int backHandle_ = 0;

    /// Navigator 的 key：用户提供的优先，否则用内部创建的 GlobalKey。
    /// 对齐 上游的 `GlobalObjectKey<NavigatorState>(this)`（app.dart:1509）。
    SharedPtr<GlobalKey> navigatorKey_;

    /// 内部默认的路由信息提供者（仅 Router 模式且用户未提供时创建）。
    SharedPtr<PlatformRouteInformationProvider> defaultRouteInformationProvider_;

    /// 内部默认的返回键分发器。上游的 `_defaultBackButtonDispatcher`。
    SharedPtr<RootBackButtonDispatcher> defaultBackButtonDispatcher_;
};

}
