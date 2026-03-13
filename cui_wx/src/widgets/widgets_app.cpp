/// @file widgets_app.cpp
/// @brief WidgetsApp 实现。语义对齐官方 widgets/app.dart（行号注释标注）。

#include "cui/widgets/widgets_app.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/platform/services/system_navigator.h"

#include <optional>
#include <stdexcept>
#include <utility>

namespace cui {

// ── WidgetsApp ──────────────────────────────────────────────────────────────

std::unique_ptr<State> WidgetsApp::createState() {
    return std::make_unique<WidgetsAppState>();
}

// ── WidgetsAppState：生命周期 ───────────────────────────────────────────────

void WidgetsAppState::initState() {
    State::initState();
    // 上游的 initState（app.dart:1460-1464）：initState 即确定路由模式。
    updateRouting(nullptr);
    // 平台返回键（Android onBackPressed）→ 先问本应用。WidgetsBinding.
    // handlePopRoute 在本仓没有绑定层，改成 SystemNavigator 的处理器注册表；
    // 新注册的（最内层应用）优先，见 system_navigator.h。
    backHandle_ = services::SystemNavigator::addBackHandler(
            [this] { return handleBackButton(); });
}

void WidgetsAppState::didUpdateWidget(cui::Widget* oldWidget) {
    State::didUpdateWidget(oldWidget);
    // 上游的 didUpdateWidget（app.dart:1466-1470）：重新评估路由模式。
    updateRouting(dynamic_cast<const WidgetsApp*>(oldWidget));
}

void WidgetsAppState::dispose() {
    if (backHandle_ != 0) {
        services::SystemNavigator::removeBackHandler(backHandle_);
        backHandle_ = 0;
    }
    State::dispose();
}

bool WidgetsAppState::handleBackButton() {
    // 上游 WidgetsBinding.handlePopRoute（binding.dart）：
    // Router 委托模式由 BackButtonDispatcher 决定（RootBackButtonDispatcher
    // 直接 invokeCallback），Navigator 模式走 didPopRoute。
    if (usesRouterWithDelegates()) {
        auto dispatcher = effectiveBackButtonDispatcher();
        return dispatcher && dispatcher->invokeCallback(false);
    }
    return didPopRoute();
}

// ── WidgetsAppState：路由模式 ───────────────────────────────────────────────

bool WidgetsAppState::usesRouterWithDelegates() const {
    auto* app = dynamic_cast<const WidgetsApp*>(widget());
    // 上游：widget.routerDelegate != null（app.dart:1518）。
    return app && app->routerDelegate() != nullptr;
}

bool WidgetsAppState::usesNavigator() const {
    auto* app = dynamic_cast<const WidgetsApp*>(widget());
    if (!app)
        return false;
    // 上游的 _usesNavigator（app.dart:1522-1526）。
    return app->home() != nullptr || !app->routes().empty() ||
           app->onGenerateRoute() != nullptr || app->onUnknownRoute() != nullptr;
}

std::string WidgetsAppState::initialRouteName() const {
    auto* app = dynamic_cast<const WidgetsApp*>(widget());
    // 上游的 _initialRouteName（app.dart:1429-1433）优先取
    // platformDispatcher.defaultRouteName；本移植无平台调度器，
    // 降级为 initialRoute 为空时取 Navigator.defaultRouteName。
    if (app && !app->initialRoute().empty())
        return app->initialRoute();
    return Navigator::defaultRouteName;
}

void WidgetsAppState::updateRouting(const WidgetsApp* oldWidget) {
    auto* app = dynamic_cast<const WidgetsApp*>(widget());
    if (!app)
        return;

    if (usesRouterWithDelegates()) {
        // 上游（app.dart:1492-1507）：清理 Navigator 资源；
        // 用户未提供 provider 且有 parser 时创建默认 provider；
        // 用户未提供 dispatcher 时创建默认 RootBackButtonDispatcher。
        navigatorKey_ = nullptr;
        defaultRouteInformationProvider_ = nullptr;
        if (!app->routeInformationProvider() && app->routeInformationParser()) {
            if (!defaultRouteInformationProvider_) {
                defaultRouteInformationProvider_ =
                        cui::make_shared<PlatformRouteInformationProvider>(
                                RouteInformation(initialRouteName()));
            }
        }
        if (!app->backButtonDispatcher() && !defaultBackButtonDispatcher_)
            defaultBackButtonDispatcher_ = cui::make_shared<RootBackButtonDispatcher>();
    } else if (usesNavigator()) {
        // 上游（app.dart:1508-1515）：清理 Router 资源；
        // navigator key 首次创建或用户更换 key 时重建。
        defaultRouteInformationProvider_ = nullptr;
        defaultBackButtonDispatcher_ = nullptr;
        bool needNewKey = navigatorKey_ == nullptr;
        if (oldWidget) {
            const auto& oldKey = oldWidget->navigatorKey();
            auto* app2 = static_cast<const WidgetsApp*>(widget());
            if (oldKey != app2->navigatorKey())
                needNewKey = true;
        }
        if (app->navigatorKey()) {
            navigatorKey_ = app->navigatorKey();
        } else if (needNewKey) {
            // 对齐 上游的 GlobalObjectKey<NavigatorState>(this)（app.dart:1509）。
            navigatorKey_ = cui::make_shared<GlobalKey>("WidgetsApp.navigator");
        }
    } else {
        // 纯 builder 模式：清理全部路由资源（app.dart:1516-1521）。
        navigatorKey_ = nullptr;
        defaultRouteInformationProvider_ = nullptr;
        defaultBackButtonDispatcher_ = nullptr;
    }
}

SharedPtr<RouteInformationProvider>
WidgetsAppState::effectiveRouteInformationProvider() const {
    auto* app = dynamic_cast<const WidgetsApp*>(widget());
    // 上游的 _effectiveRouteInformationProvider（app.dart:1532-1534）。
    if (app && app->routeInformationProvider())
        return app->routeInformationProvider();
    return defaultRouteInformationProvider_;
}

SharedPtr<BackButtonDispatcher> WidgetsAppState::effectiveBackButtonDispatcher() const {
    auto* app = dynamic_cast<const WidgetsApp*>(widget());
    // 上游的 _effectiveBackButtonDispatcher（app.dart:1535-1537）。
    if (app && app->backButtonDispatcher())
        return app->backButtonDispatcher();
    return defaultBackButtonDispatcher_;
}

// ── WidgetsAppState：路由生成 ───────────────────────────────────────────────

SharedPtr<Route> WidgetsAppState::onGenerateRoute(const RouteSettings& settings) {
    auto* app = dynamic_cast<const WidgetsApp*>(widget());
    if (!app)
        return nullptr;

    // 上游的 _onGenerateRoute（app.dart:1595-1612）：
    //   '/' 且有 home → home builder；否则查 routes 表；
    //   命中后用 pageRouteBuilder 包裹；未命中退到 onGenerateRoute 回调。
    const std::string& name = settings.name();
    std::optional<WidgetsApp::WidgetBuilder> pageContentBuilder;
    if (name == Navigator::defaultRouteName && app->home()) {
        SharedPtr<Widget> home = app->home();
        pageContentBuilder = [home](BuildContext&) { return home; };
    } else {
        const auto& routes = app->routes();
        auto it = routes.find(name);
        if (it != routes.end())
            pageContentBuilder = it->second;
    }

    if (pageContentBuilder) {
        // 官方断言：home/routes 非空时必须有 pageRouteBuilder（app.dart:1600-1604）。
        if (!app->pageRouteBuilder()) {
            throw std::logic_error(
                    "WidgetsApp: pageRouteBuilder must be set when home or routes is used "
                    "(上游 app.dart:1600-1604 assertion)");
        }
        WidgetsApp::WidgetBuilder builder = std::move(*pageContentBuilder);
        return app->pageRouteBuilder()(settings, std::move(builder));
    }
    if (app->onGenerateRoute())
        return app->onGenerateRoute()(settings);
    return nullptr;
}

SharedPtr<Route> WidgetsAppState::onUnknownRoute(const RouteSettings& settings) {
    auto* app = dynamic_cast<const WidgetsApp*>(widget());
    if (!app || !app->onUnknownRoute()) {
        // 上游断言：onUnknownRoute 未设置时抛 上游Error（app.dart:1617-1630）。
        throw std::logic_error(
                "WidgetsApp: could not find a generator for route '" + settings.name() +
                "'. Make sure the root app widget provides home, routes, onGenerateRoute, "
                "or onUnknownRoute (上游 app.dart:1617-1630)");
    }
    auto route = app->onUnknownRoute()(settings);
    if (!route) {
        // 上游断言：回调不得返回 null（app.dart:1631-1637）。
        throw std::logic_error(
                "WidgetsApp: onUnknownRoute returned null for route '" + settings.name() +
                "' (上游 app.dart:1631-1637)");
    }
    return route;
}

// ── WidgetsAppState：系统回退 / 推送 ────────────────────────────────────────

NavigatorState* WidgetsAppState::navigatorState() const {
    if (!navigatorKey_)
        return nullptr;
    // 通过 GlobalKey 定位 Navigator 的 Element，再取其 State。
    Element* element = navigatorKey_->currentElement<Element>();
    if (!element)
        return nullptr;
    auto* stateful = dynamic_cast<StatefulElement*>(element);
    if (!stateful)
        return nullptr;
    return dynamic_cast<NavigatorState*>(stateful->state());
}

bool WidgetsAppState::didPopRoute() {
    // 上游的 didPopRoute（app.dart:1637-1655）：
    // Router 委托模式不处理（返回 false，交给 BackButtonDispatcher）。
    if (usesRouterWithDelegates())
        return false;
    NavigatorState* navigator = navigatorState();
    if (!navigator)
        return false;
    return navigator->maybePop();
}

bool WidgetsAppState::didPushRouteInformation(const RouteInformation& routeInformation) {
    // 上游的 didPushRouteInformation（app.dart:1657-1677）：
    // Router 委托模式不处理。
    if (usesRouterWithDelegates())
        return false;
    NavigatorState* navigator = navigatorState();
    if (!navigator)
        return false;
    navigator->pushNamed(routeInformation.uri());
    return true;
}

// ── WidgetsAppState：build ──────────────────────────────────────────────────

SharedPtr<Widget> WidgetsAppState::build(BuildContext& context) {
    auto* app = dynamic_cast<const WidgetsApp*>(widget());
    if (!app)
        return nullptr;

    // 上游的 build（app.dart:1704-1712）：按模式构造 routing 子树。
    SharedPtr<Widget> routing;
    if (usesRouterWithDelegates()) {
        // 上游（app.dart:1713-1720）：Router<Object>，restoreScopeId 'router'。
        routing = cui::make_shared<Router>(
                app->routerDelegate(), app->routeInformationParser(),
                effectiveRouteInformationProvider(), effectiveBackButtonDispatcher(),
                /*restorationScopeId=*/"router");
    } else if (usesNavigator()) {
        // 上游（app.dart:1721-1742）：Navigator，key = _navigator，
        // initialRoute / onGenerateRoute / onUnknownRoute / observers。
        auto navigator = cui::make_shared<Navigator>(
                std::vector<SharedPtr<Page>>{}, navigatorKey_);
        navigator->setInitialRoute(initialRouteName());
        navigator->setOnGenerateRoute(
                [this](const RouteSettings& settings) { return onGenerateRoute(settings); });
        navigator->setOnUnknownRoute(
                [this](const RouteSettings& settings) { return onUnknownRoute(settings); });
        navigator->setObservers(app->navigatorObservers());
        routing = std::move(navigator);
    }
    // 纯 builder 模式：routing 保持为空（app.dart:1743-1745）。

    // 上游（app.dart:1750-1760）：builder 包装 routing 子树。
    SharedPtr<Widget> result;
    if (app->builder()) {
        result = app->builder()(context, routing);
    } else {
        // 官方断言：无 builder 时 routing 必须非空（app.dart:1753-1755）。
        if (!routing) {
            throw std::logic_error(
                    "WidgetsApp: either home/routes/onGenerateRoute/onUnknownRoute must be "
                    "specified, or a builder must be provided (上游 app.dart:361-371)");
        }
        result = routing;
    }

    // 上游 后续的 DefaultTextStyle / PerformanceOverlay / SemanticsDebugger /
    // CheckedModeBanner / Shortcuts / Actions / Localizations / Title 包装
    // 均属 Material·Localizations·Semantics 层，本移植无对应基础设施，
    // 降级为直接返回 result（app.dart:1761-1857 的降级）。
    return result;
}

}
