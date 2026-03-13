/// @file widgets/router.cpp
/// @brief 上游的 Router 体系实现。
///
/// 全部语义对齐 上游 widgets/router.dart，
/// 注释中的行号均指向该文件。

#include "cui/widgets/router.h"

#include <algorithm>
#include <stdexcept>

#include "cui/framework/element.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {

// ── RouteInformationProvider ────────────────────────────────────────────────

int RouteInformationProvider::addListenerWithHandle(std::function<void()> listener) {
    int handle = nextHandle_++;
    listeners_.emplace_back(handle, std::move(listener));
    return handle;
}

void RouteInformationProvider::removeListenerByHandle(int handle) {
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        if (it->first == handle) {
            listeners_.erase(it);
            return;
        }
    }
}

void RouteInformationProvider::notifyListeners() {
    // 上游的 ChangeNotifier 派发使用快照，监听器内部可安全增删。
    auto snapshot = listeners_;
    for (const auto& l : snapshot)
        l.second();
}

// ── PlatformRouteInformationProvider ────────────────────────────────────────

void PlatformRouteInformationProvider::routerReportsNewRouteInformation(
    const RouteInformation& routeInformation, RouteInformationReportingType type) {
    // 上游的 PlatformRouteInformationProvider.routerReportsNewRouteInformation
    // （router.dart:1521-1538）：
    //   replace = neglect ? true : navigate ? false : uri 是否相等。
    bool replace = false;
    switch (type) {
        case RouteInformationReportingType::neglect:
            replace = true;
            break;
        case RouteInformationReportingType::navigate:
            replace = false;
            break;
        case RouteInformationReportingType::none:
            replace = value_.uri() == routeInformation.uri();
            break;
    }
    value_ = routeInformation;
    if (engineCallback_)
        engineCallback_(routeInformation, replace);
}

void PlatformRouteInformationProvider::didPushRouteInformation(
    const RouteInformation& routeInformation) {
    // 上游的 _platformReportsNewRouteInformation（router.dart:1545-1553）：
    // 值相同则忽略，否则更新并通知监听器（Router 会被唤醒重新解析）。
    if (value_ == routeInformation)
        return;
    value_ = routeInformation;
    notifyListeners();
}

// ── BackButtonDispatcher ────────────────────────────────────────────────────

int BackButtonDispatcher::addCallback(Callback callback) {
    int handle = nextHandle_++;
    callbacks_.emplace_back(handle, std::move(callback));
    return handle;
}

void BackButtonDispatcher::removeCallbackByHandle(int handle) {
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
        if (it->first == handle) {
            callbacks_.erase(it);
            return;
        }
    }
}

void BackButtonDispatcher::removeCallback(const Callback& callback) {
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
        if (sameCallbackTarget(it->second, callback)) {
            callbacks_.erase(it);
            return;
        }
    }
}

bool BackButtonDispatcher::invokeCallback(bool defaultValue) {
    // 上游的 BackButtonDispatcher.invokeCallback（router.dart:990-1018）：
    //   1. 逆序遍历子节点（越靠后 = 优先级越高，因为 deferTo 会把子节点
    //      移到集合末尾）；
    //   2. 第一个返回 true 的子节点吞掉事件，立即返回；
    //   3. 全部未处理，才由自己处理（上游 调 super.invokeCallback，
    //      即 _CallbackHookProvider：调用**最后注册**的回调）。
    if (!children_.empty()) {
        for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
            if ((*it)->notifiedByParent(defaultValue))
                return true;
        }
    }
    if (callbacks_.empty())
        return defaultValue;
    return callbacks_.back().second();
}

SharedPtr<ChildBackButtonDispatcher> BackButtonDispatcher::createChildBackButtonDispatcher() {
    // 上游的 createChildBackButtonDispatcher（router.dart:1023-1027）。
    // 父引用用裸指针：父分发器（尤其是 RootBackButtonDispatcher）
    // 允许在栈上构造。
    auto child = cui::make_shared<ChildBackButtonDispatcher>(this);
    children_.push_back(child);
    return child;
}

void BackButtonDispatcher::takePriority() {
    // 上游的 BackButtonDispatcher.takePriority（router.dart:1030-1035）：
    //   `void takePriority() => _children.clear();`
    // 即不再向下委托，由自己处理。
    children_.clear();
}

void BackButtonDispatcher::deferTo(const SharedPtr<ChildBackButtonDispatcher>& child) {
    // 上游的 deferTo（router.dart:1046-1057）：先移除再追加，
    // 使子节点位于集合末尾 —— 逆序遍历时它最先被询问。
    children_.erase(std::remove(children_.begin(), children_.end(), child), children_.end());
    children_.push_back(child);
}

void BackButtonDispatcher::forget(const SharedPtr<ChildBackButtonDispatcher>& child) {
    children_.erase(std::remove(children_.begin(), children_.end(), child), children_.end());
}

// ── ChildBackButtonDispatcher ───────────────────────────────────────────────

void ChildBackButtonDispatcher::takePriority() {
    // 上游的 ChildBackButtonDispatcher.takePriority（router.dart:1143-1147）：
    //   parent.deferTo(this); super.takePriority();
    if (parent_)
        parent_->deferTo(shared_from_this());
    BackButtonDispatcher::takePriority();
}

void ChildBackButtonDispatcher::deferTo(const SharedPtr<ChildBackButtonDispatcher>& child) {
    // 上游的 ChildBackButtonDispatcher.deferTo（router.dart:1149-1154）。
    if (parent_)
        parent_->deferTo(shared_from_this());
    BackButtonDispatcher::deferTo(child);
}

void ChildBackButtonDispatcher::removeCallbackByHandle(int handle) {
    // 上游的 ChildBackButtonDispatcher.removeCallback（router.dart:1156-1161）：
    //   自身不再有回调时，向父节点注销自己。
    BackButtonDispatcher::removeCallbackByHandle(handle);
    if (!hasCallbacks() && parent_)
        parent_->forget(shared_from_this());
}

// ── RouterState ─────────────────────────────────────────────────────────────

void RouterState::initState() {
    // 上游的 _RouterState.initState（router.dart:616-622）：
    //   订阅 Provider / Dispatcher / Delegate 三方的通知。
    auto* router = dynamic_cast<Router*>(widget());
    if (router) {
        provider_ = router->routeInformationProvider();
        parser_ = router->routeInformationParser();
        delegate_ = router->routerDelegate();
        dispatcher_ = router->backButtonDispatcher();
    }

    if (provider_) {
        providerListenerHandle_ = provider_->addListenerWithHandle(
            [this]() { handleRouteInformationProviderNotification(); });
    }
    if (delegate_) {
        delegateListenerHandle_ =
            delegate_->addListenerWithHandle([this]() { handleRouterDelegateNotification(); });
    }
    if (dispatcher_) {
        dispatcherCallbackHandle_ = dispatcher_->addCallback(
            [this]() { return handleBackButtonDispatcherNotification(); });
    }
}

void RouterState::didChangeDependencies() {
    // 上游的 _RouterState.didChangeDependencies（router.dart:711-724）配合
    // restoreState（router.dart:634-651）：
    //   首次拿到 Provider 的当前值时，走 **setInitialRoutePath**。
    State::didChangeDependencies();
    if (!initialParsed_ && provider_) {
        initialParsed_ = true;
        processRouteInformation(provider_->value(), /*initial=*/true);
    }
    maybeNeedToReportRouteInformation();
}

void RouterState::didUpdateWidget(Widget* oldWidget) {
    // 上游的 _RouterState.didUpdateWidget（router.dart:738-763）：
    //   delegate 变化时重新挂监听并重新报告路由信息。
    State::didUpdateWidget(oldWidget);
    auto* oldRouter = dynamic_cast<Router*>(oldWidget);
    auto* router = dynamic_cast<Router*>(widget());
    if (!router)
        return;

    if (oldRouter && oldRouter->routeInformationProvider() != router->routeInformationProvider()) {
        if (provider_ && providerListenerHandle_)
            provider_->removeListenerByHandle(providerListenerHandle_);
        provider_ = router->routeInformationProvider();
        providerListenerHandle_ = 0;
        if (provider_) {
            providerListenerHandle_ = provider_->addListenerWithHandle(
                [this]() { handleRouteInformationProviderNotification(); });
            if (oldRouter && oldRouter->routeInformationProvider() &&
                oldRouter->routeInformationProvider()->value() != provider_->value()) {
                handleRouteInformationProviderNotification();
            }
        }
    }

    if (oldRouter && oldRouter->backButtonDispatcher() != router->backButtonDispatcher()) {
        if (dispatcher_ && dispatcherCallbackHandle_)
            dispatcher_->removeCallbackByHandle(dispatcherCallbackHandle_);
        dispatcher_ = router->backButtonDispatcher();
        dispatcherCallbackHandle_ = 0;
        if (dispatcher_) {
            dispatcherCallbackHandle_ = dispatcher_->addCallback(
                [this]() { return handleBackButtonDispatcherNotification(); });
        }
    }

    if (oldRouter && oldRouter->routerDelegate() != router->routerDelegate()) {
        if (delegate_ && delegateListenerHandle_)
            delegate_->removeListenerByHandle(delegateListenerHandle_);
        delegate_ = router->routerDelegate();
        delegateListenerHandle_ = 0;
        if (delegate_) {
            delegateListenerHandle_ =
                delegate_->addListenerWithHandle([this]() { handleRouterDelegateNotification(); });
        }
        maybeNeedToReportRouteInformation();
    }

    parser_ = router->routeInformationParser();
}

void RouterState::dispose() {
    // 上游的 _RouterState.dispose（router.dart:761-770）：解绑三方监听。
    if (provider_ && providerListenerHandle_)
        provider_->removeListenerByHandle(providerListenerHandle_);
    if (delegate_ && delegateListenerHandle_)
        delegate_->removeListenerByHandle(delegateListenerHandle_);
    if (dispatcher_ && dispatcherCallbackHandle_)
        dispatcher_->removeCallbackByHandle(dispatcherCallbackHandle_);
    providerListenerHandle_ = 0;
    delegateListenerHandle_ = 0;
    dispatcherCallbackHandle_ = 0;
    State::dispose();
}

SharedPtr<Widget> RouterState::build(BuildContext& context) {
    // 上游的 _RouterState.build（router.dart:840-856）：
    //   _RouterScope(child: Builder(builder: widget.routerDelegate.build))
    //
    // `Builder` 的作用是让 delegate.build 拿到的 context 位于 scope 之下，
    // 从而可以 dependOnInheritedWidgetOfExactType<_RouterScope>()。
    RouterScopeData data;
    data.routeInformationProvider = provider_;
    data.routeInformationParser = parser_;
    data.routerDelegate = delegate_;
    data.backButtonDispatcher = dispatcher_;
    data.routerState = this;

    auto* self = this;
    auto builder = cui::make_shared<Builder>(
        [self](BuildContext& ctx) -> SharedPtr<Widget> {
            if (!self->delegate_)
                return nullptr;
            return self->delegate_->build(ctx);
        });

    return cui::make_shared<RouterScope>(std::move(data), builder);
}

void RouterState::handleRouteInformationProviderNotification() {
    // 上游的 _handleRouteInformationProviderNotification（router.dart:802-807）：
    //   Provider 报告新路由 → 重新解析 → setNewRoutePath。
    if (!provider_)
        return;
    processRouteInformation(provider_->value(), /*initial=*/false);
}

bool RouterState::handleBackButtonDispatcherNotification() {
    // 上游的 _handleBackButtonDispatcherNotification（router.dart:809-815）：
    //   把返回请求交给 routerDelegate.popRoute()，并重建。
    //   返回 false 表示未处理 → 继续向上冒泡（router.dart:818-824）。
    if (!delegate_)
        return false;
    // 新的事务令牌：正在进行中的解析结果将被丢弃。
    ++transaction_;
    bool handled = delegate_->popRoute();
    rebuild();
    return handled;
}

void RouterState::handleRouterDelegateNotification() {
    // 上游的 _handleRouterDelegateNotification（router.dart:834-840）：
    //   delegate 通知监听器 = 「我准备好重建了」。
    rebuild();
}

void RouterState::processRouteInformation(const RouteInformation& information, bool initial) {
    // 上游的 _processRouteInformation（router.dart:771-780）：
    //   生成新事务令牌 → 异步解析 → 令牌仍有效才应用结果 → 重建。
    if (!parser_ || !delegate_)
        return;

    const int txn = ++transaction_;
    auto* self = this;
    RouteInformationParser::ParseCallback completion =
        [self, txn, initial](SharedPtr<void> configuration) {
            // 过期结果直接丢弃（router.dart:788-790）。
            if (self->transaction_ != txn)
                return;
            if (initial)
                self->delegate_->setInitialRoutePath(configuration);
            else
                self->delegate_->setNewRoutePath(configuration);
            if (self->transaction_ == txn)
                self->rebuild();
        };

    parser_->parseRouteInformationWithDependencies(information, *context(), completion);
}

void RouterState::rebuild() {
    // 上游的 _rebuild（router.dart:828-832）：
    //   setState(...) + _maybeNeedToReportRouteInformation()。
    setState();
    maybeNeedToReportRouteInformation();
}

void RouterState::maybeNeedToReportRouteInformation() {
    // 上游的 _maybeNeedToReportRouteInformation（router.dart:698-702）
    // 与 _reportRouteInformation（router.dart:660-681）：
    //   取 delegate 的当前配置 → 经 parser 还原成 RouteInformation
    //   → 交给 provider 上报；本次报告后意图复位为 none。
    if (!provider_ || !parser_ || !delegate_)
        return;
    auto configuration = delegate_->currentConfiguration();
    SharedPtr<RouteInformation> info;
    if (configuration)
        info = parser_->restoreRouteInformation(configuration);
    if (info)
        provider_->routerReportsNewRouteInformation(*info, reportType_);
    reportType_ = RouteInformationReportingType::none;
}

void RouterState::setStateWithExplicitReportStatus(RouteInformationReportingType status,
                                                   const std::function<void()>& fn) {
    // 上游的 _setStateWithExplicitReportStatus（router.dart:686-712）：
    //   先记录本次的报告意图，再执行回调。
    reportType_ = status;
    if (fn)
        fn();
    maybeNeedToReportRouteInformation();
}

// ── Router ──────────────────────────────────────────────────────────────────

Router::Router(SharedPtr<RouterDelegate> routerDelegate,
               SharedPtr<RouteInformationParser> routeInformationParser,
               SharedPtr<RouteInformationProvider> routeInformationProvider,
               SharedPtr<BackButtonDispatcher> backButtonDispatcher,
               std::string restorationScopeId, KeyPtr key)
    : StatefulWidget(std::move(key)),
      routeInformationProvider_(std::move(routeInformationProvider)),
      routeInformationParser_(std::move(routeInformationParser)),
      routerDelegate_(std::move(routerDelegate)),
      backButtonDispatcher_(std::move(backButtonDispatcher)),
      restorationScopeId_(std::move(restorationScopeId)) {
    // 上游的 Router 构造断言（router.dart:367-374）：
    //   Provider 与 Parser 必须同时提供或同时不提供。
    if ((routeInformationProvider_ != nullptr) != (routeInformationParser_ != nullptr)) {
        throw std::logic_error(
                "Router: a routeInformationParser must be provided when a "
                "routeInformationProvider is specified (and vice versa).");
    }
    if (!routerDelegate_)
        throw std::logic_error("Router: routerDelegate must not be null.");
}

std::unique_ptr<State> Router::createState() {
    return std::make_unique<RouterState>();
}

Router* Router::of(BuildContext& context) {
    // 上游的 Router.of（router.dart:479-497）：
    //   scope.routerState.widget as Router<T>
    auto* scope = context.dependOnInheritedWidgetOfExactType<RouterScope>();
    if (!scope) {
        throw std::logic_error(
                "Router operation requested with a context that does not include a Router.\n"
                "The context used to retrieve the Router must be that of a widget that "
                "is a descendant of a Router widget.");
    }
    auto data = scope->data();
    if (!data || !data->routerState)
        return nullptr;
    return dynamic_cast<Router*>(data->routerState->widget());
}

Router* Router::maybeOf(BuildContext& context) {
    // 上游的 Router.maybeOf（router.dart:508-511）。
    auto* scope = context.dependOnInheritedWidgetOfExactType<RouterScope>();
    if (!scope)
        return nullptr;
    auto data = scope->data();
    if (!data || !data->routerState)
        return nullptr;
    return dynamic_cast<Router*>(data->routerState->widget());
}

void Router::navigate(BuildContext& context, const std::function<void()>& callback) {
    // 上游的 Router.navigate（router.dart:524-534）：
    //   强制本次状态变化产生一条新的历史条目。
    auto* scope = context.getInheritedWidgetOfExactType<RouterScope>();
    if (!scope)
        throw std::logic_error("Router.navigate: no Router found in the given context.");
    auto data = scope->data();
    if (!data || !data->routerState)
        throw std::logic_error("Router.navigate: no RouterState found in the given context.");
    data->routerState->setStateWithExplicitReportStatus(
            RouteInformationReportingType::navigate, callback);
}

void Router::neglect(BuildContext& context, const std::function<void()>& callback) {
    // 上游的 Router.neglect（router.dart:545-556）：
    //   强制本次状态变化不产生新的历史条目。
    auto* scope = context.getInheritedWidgetOfExactType<RouterScope>();
    if (!scope)
        throw std::logic_error("Router.neglect: no Router found in the given context.");
    auto data = scope->data();
    if (!data || !data->routerState)
        throw std::logic_error("Router.neglect: no RouterState found in the given context.");
    data->routerState->setStateWithExplicitReportStatus(
            RouteInformationReportingType::neglect, callback);
}

}
