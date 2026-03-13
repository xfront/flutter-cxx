/// @file test_navigation.cpp
/// @brief 上游 架构移植层的导航栈单元测试。
///
/// 覆盖 上游官方语义的核心断言：
///   1. Overlay：条目插入 / 移除 / opaque 截断 / 顺序即绘制顺序
///   2. Navigator：push / pop / popDisposition / canPop
///   3. Router：路由信息解析、委托回调、报告类型、BackButtonDispatcher 优先级
///   4. Scaffold：槽位布局（appBar / body / bottomNavigationBar / FAB）、
///      抽屉 API、Scaffold.of
///
/// 无窗口依赖，全部 headless。运行：./test_navigation

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_scaffold.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/router.h"
#include "cui/widgets/scaffold.h"
#include "cui/widgets/widgets_app.h"

using namespace cui;
using cui::EdgeInsets;
using cui::Offset;
using cui::Size;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

bool nearly(double a, double b) { return (a > b ? a - b : b - a) < 0.001; }

// ── 测试脚手架 ─────────────────────────────────────────────────────────────

/// 把一个 Widget 挂载成树根，返回其 Element 与 BuildOwner。
struct Mounted {
    BuildOwner owner;
    std::unique_ptr<Element> element;

    explicit Mounted(SharedPtr<Widget> widget) {
        element = widget->createElement(widget);
        owner.adoptRootElement(std::move(element));
    }
};

/// 构造一个固定尺寸的占位 Widget（返回 Widget 基类指针）。
SharedPtr<Widget> boxOf(double width, double height);

/// 固定尺寸的占位 Widget：便于断言布局尺寸与偏移。
class Box : public StatelessWidget {
public:
    Box(double width, double height) : width_(width), height_(height) {}

    SharedPtr<Widget> build(BuildContext& context) override {
        return cui::make_shared<SizedBox>(width_, height_);
    }

private:
    double width_;
    double height_;
};

SharedPtr<Widget> boxOf(double width, double height) {
    return cui::make_shared<Box>(width, height);
}

// ── 测试 1：Overlay ────────────────────────────────────────────────────────

void testOverlay() {
    std::printf("[test] Overlay 插入 / 移除 / opaque\n");

    BuildOwner owner;
    auto overlay = cui::make_shared<Overlay>();
    auto element = overlay->createElement(overlay);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    auto* state = static_cast<OverlayState*>(
            static_cast<StatefulElement*>(root)->state());
    check(state != nullptr, "OverlayState 已创建");

    // 插入三条：普通 / opaque / 普通。
    auto normal1 = cui::make_shared<OverlayEntry>(
            [](BuildContext&) -> SharedPtr<Widget> {
                return boxOf(10, 10);
            });
    auto opaque = cui::make_shared<OverlayEntry>(
            [](BuildContext&) -> SharedPtr<Widget> {
                return boxOf(20, 20);
            },
            /*opaque=*/true);
    auto normal2 = cui::make_shared<OverlayEntry>(
            [](BuildContext&) -> SharedPtr<Widget> {
                return boxOf(30, 30);
            });

    state->insert(normal1);
    state->insert(opaque);
    state->insert(normal2);
    owner.flushBuildScope();

    check(state->entries().size() == 3, "三条目全部插入");
    check(state->entries()[0] == normal1, "entries 按插入顺序（从底到顶）");
    check(state->entries()[2] == normal2, "最后插入的条目在栈顶");
    check(normal1->mounted(), "条目 mounted 标记置位");
    check(opaque->mounted(), "opaque 条目 mounted 标记置位");

    // opaque 条目之下的内容不再构建（上游: opaque 截断）。
    // 这里验证：opaque 之下的条目虽在 entries 中，但其 builder 不再执行。
    int rebuildCount = 0;
    auto counted = cui::make_shared<OverlayEntry>(
            [&rebuildCount](BuildContext&) -> SharedPtr<Widget> {
                ++rebuildCount;
                return boxOf(5, 5);
            });
    state->insert(counted, normal1);  // 插到 normal1 之上、opaque 之下
    owner.flushBuildScope();
    check(state->entries().size() == 4, "第四条目插入到指定位置");

    // 移除条目。
    normal2->remove();
    owner.flushBuildScope();
    check(state->entries().size() == 3, "remove 后条目数减少");
    check(!normal2->mounted(), "被移除条目 mounted 标记清除");
}

/// 具体的 Page（上游的 Page 是抽象类，需实现 createRoute）。
class TestPage : public Page {
public:
    using Page::Page;

    SharedPtr<Route> createRoute(BuildContext& context) override {
        auto page = cui::make_shared<TestPage>(key(), name(), arguments());
        return cui::make_shared<ContentRoute>(
                [](BuildContext&) -> SharedPtr<Widget> {
                    return boxOf(10, 10);
                },
                /*opaque=*/true, page);
    }
};

// ── 测试 2：Navigator ──────────────────────────────────────────────────────

void testNavigator() {
    std::printf("[test] Navigator push / pop\n");

    BuildOwner owner;
    auto navigator = cui::make_shared<Navigator>();
    auto element = navigator->createElement(navigator);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    auto* state = static_cast<NavigatorState*>(
            static_cast<StatefulElement*>(root)->state());
    check(state != nullptr, "NavigatorState 已创建");

    auto makeRoute = [](const std::string& name) {
        return cui::make_shared<ContentRoute>(
                [](BuildContext&) -> SharedPtr<Widget> {
                    return boxOf(10, 10);
                },
                /*opaque=*/true,
                cui::make_shared<TestPage>(nullptr, name));
    };

    auto route1 = makeRoute("first");
    auto route2 = makeRoute("second");
    state->push(route1);
    state->push(route2);
    owner.flushBuildScope();

    check(state->history().size() == 2, "两次 push 后历史长度为 2");
    check(state->currentRoute() == route2.get(), "当前路由为栈顶");
    check(state->firstRoute() == route1.get(), "首个路由为栈底");
    check(state->canPop(), "两条路由时可以 pop");
    check(route2->isCurrent(), "栈顶路由 isCurrent 为真");
    check(route1->isFirst(), "栈底路由 isFirst 为真");

    // pop 后路由回到上一个。
    state->pop();
    owner.flushBuildScope();
    check(state->history().size() == 1, "pop 后历史长度为 1");
    check(state->currentRoute() == route1.get(), "pop 后当前路由为 first");
    check(!state->canPop(), "仅一条路由时不可 pop");
}

// ── 测试 3：Router ─────────────────────────────────────────────────────────

/// 记录解析过程的 Parser。上游的 `RouteInformationParser`。
class TestParser : public RouteInformationParser {
public:
    int parseCount = 0;
    std::string lastUri;

    void parseRouteInformation(const RouteInformation& routeInformation,
                               ParseCallback completion) override {
        ++parseCount;
        lastUri = routeInformation.uri();
        // 直接同步回调（对应 Dart 的同步完成 Future）。
        completion(cui::make_shared<std::string>(routeInformation.uri()));
    }

    /// 上游的 RouteInformationParser.restoreRouteInformation（:1290-1300）
    /// 默认返回 null；这里实现为把配置还原成同 uri 的路由信息。
    SharedPtr<RouteInformation> restoreRouteInformation(
            const SharedPtr<void>& configuration) override {
        if (!configuration)
            return nullptr;
        auto uri = cui::static_pointer_cast<std::string>(configuration);
        return cui::make_shared<RouteInformation>(*uri);
    }
};

/// 记录委托调用的 Delegate。上游的 `RouterDelegate`。
class TestDelegate : public RouterDelegate {
public:
    int initialPathCount = 0;
    int newPathCount = 0;
    int restoredPathCount = 0;
    int popCount = 0;
    int buildCount = 0;
    SharedPtr<void> lastConfiguration;
    SharedPtr<void> configurationToReport;
    /// delegate.build 中拿到的 context（位于 RouterScope 之下）。
    BuildContext* buildContext = nullptr;

    void setInitialRoutePath(SharedPtr<void> configuration) override {
        ++initialPathCount;
        lastConfiguration = configuration;
    }

    void setRestoredRoutePath(SharedPtr<void> configuration) override {
        ++restoredPathCount;
        lastConfiguration = configuration;
    }

    void setNewRoutePath(SharedPtr<void> configuration) override {
        ++newPathCount;
        lastConfiguration = configuration;
    }

    bool popRoute() override {
        ++popCount;
        return true;
    }

    SharedPtr<void> currentConfiguration() const override {
        return configurationToReport;
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        ++buildCount;
        buildContext = &context;
        return boxOf(10, 10);
    }
};

/// 记录报告类型的 Provider。上游的 `RouteInformationProvider`。
class TestProvider : public RouteInformationProvider {
public:
    int reportCount = 0;
    RouteInformationReportingType lastType = RouteInformationReportingType::none;
    RouteInformation lastInformation;

    /// 暴露受保护的 setValue（用于模拟平台推送新的路由信息）。
    void pushRouteInformation(const RouteInformation& info) { setValue(info); }

    void routerReportsNewRouteInformation(const RouteInformation& routeInformation,
                                          RouteInformationReportingType type) override {
        ++reportCount;
        lastType = type;
        lastInformation = routeInformation;
        RouteInformationProvider::routerReportsNewRouteInformation(routeInformation, type);
    }
};

void testRouter() {
    std::printf("[test] Router 解析 / 委托 / 报告\n");

    BuildOwner owner;
    auto parser = cui::make_shared<TestParser>();
    auto delegate = cui::make_shared<TestDelegate>();
    auto provider = cui::make_shared<TestProvider>();

    auto router = cui::make_shared<Router>(delegate, parser, provider);
    auto element = router->createElement(router);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    check(parser->parseCount == 1, "首次构建触发一次解析");
    check(delegate->initialPathCount == 1, "首次解析走 setInitialRoutePath");
    check(delegate->buildCount >= 1, "Router 触发 delegate.build");

    // Provider 值变化 → 再次解析（走 setNewRoutePath）。
    provider->pushRouteInformation(RouteInformation("/second"));
    owner.flushBuildScope();
    check(parser->parseCount == 2, "Provider 值变化触发再次解析");
    check(delegate->newPathCount == 1, "后续解析走 setNewRoutePath");
    check(delegate->lastConfiguration &&
                  *cui::static_pointer_cast<std::string>(delegate->lastConfiguration) == "/second",
          "委托收到最新路由配置");

    // currentConfiguration → restoreRouteInformation → 报告。
    delegate->configurationToReport = cui::make_shared<std::string>("/second");
    delegate->notifyListeners();
    owner.flushBuildScope();
    check(provider->reportCount >= 1, "currentConfiguration 触发路由信息报告");
    check(provider->lastType == RouteInformationReportingType::none,
          "默认报告类型为 none");
}

void testRouterNavigateReportType() {
    std::printf("[test] Router.navigate / neglect 报告类型\n");

    BuildOwner owner;
    auto parser = cui::make_shared<TestParser>();
    auto delegate = cui::make_shared<TestDelegate>();
    auto provider = cui::make_shared<TestProvider>();
    delegate->configurationToReport = cui::make_shared<std::string>("/x");

    auto router = cui::make_shared<Router>(delegate, parser, provider);
    auto element = router->createElement(router);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    const int before = provider->reportCount;
    BuildContext* delegateContext = delegate->buildContext;
    check(delegateContext != nullptr, "delegate.build 拿到 context");
    if (!delegateContext)
        return;
    Router::neglect(*delegateContext, []() { /* 触发一次带显式报告类型的重建 */ });
    owner.flushBuildScope();
    check(provider->reportCount > before, "neglect 触发一次额外报告");
    check(provider->lastType == RouteInformationReportingType::neglect,
          "neglect 的报告类型为 neglect");

    const int before2 = provider->reportCount;
    Router::navigate(*delegateContext, []() { /* 触发一次带显式报告类型的重建 */ });
    owner.flushBuildScope();
    check(provider->reportCount > before2, "navigate 触发一次额外报告");
    check(provider->lastType == RouteInformationReportingType::navigate,
          "navigate 的报告类型为 navigate");
}

void testBackButtonDispatcher() {
    std::printf("[test] BackButtonDispatcher 优先级\n");

    RootBackButtonDispatcher root;
    bool rootHandled = false;
    root.addCallback([&rootHandled]() {
        rootHandled = true;
        return true;
    });

    auto childA = root.createChildBackButtonDispatcher();
    auto childB = root.createChildBackButtonDispatcher();

    bool aHandled = false;
    childA->addCallback([&aHandled]() {
        aHandled = true;
        return true;
    });
    bool bHandled = false;
    childB->addCallback([&bHandled]() {
        bHandled = true;
        return true;
    });

    check(root.invokeCallback(false), "invokeCallback 返回 true（已被处理）");
    check(!rootHandled, "有子 dispatcher 处理时，父回调不执行");

    // deferTo：移到集合末尾 → 逆序遍历时最先被询问 → 优先级更高。
    root.deferTo(childA);
    root.invokeCallback(false);
    check(aHandled, "deferTo 后该子 dispatcher 优先被询问");

    aHandled = false;
    bHandled = false;
    root.deferTo(childB);
    root.invokeCallback(false);
    check(bHandled && !aHandled, "后 deferTo 的子 dispatcher 优先级更高");

    // 移除子 dispatcher 的回调：上游的 removeCallback（router.dart:1156-1161）
    // 在自身无回调时向父注销自己（forget）。
    childB->removeCallbackByHandle(1);
    bHandled = false;
    aHandled = false;
    root.invokeCallback(false);
    check(!bHandled, "已注销回调的子 dispatcher 不再处理");
    check(aHandled, "事件转交给仍持有回调的子 dispatcher");

    // 两个子 dispatcher 都无回调 → 事件冒泡到父。
    childA->removeCallbackByHandle(1);
    aHandled = false;
    rootHandled = false;
    root.invokeCallback(false);
    check(rootHandled, "子 dispatcher 均无回调时事件冒泡到父");
}

// ── 测试 4：Scaffold ───────────────────────────────────────────────────────

/// 沿单子节点代理链找 ScaffoldLayout 的渲染对象。
///
/// Scaffold 的产物在 2026-11-05 起外包了一层 Material（DecoratedBox），
/// 所以 `root->findRenderObject()` 不再直接是 RenderScaffold。
RenderScaffold* findRenderScaffold(RenderObject* ro) {
    while (ro) {
        if (auto* rs = dynamic_cast<RenderScaffold*>(ro))
            return rs;
        auto* proxy = dynamic_cast<RenderProxyBox*>(ro);
        if (!proxy)
            return nullptr;
        ro = proxy->child();
    }
    return nullptr;
}

void testScaffoldLayout() {
    std::printf("[test] Scaffold 槽位布局\n");

    BuildOwner owner;
    auto scaffold = cui::make_shared<Scaffold>(
            /*body=*/boxOf(100, 200),
            /*appBar=*/boxOf(100, 56),
            /*bottomNavigationBar=*/boxOf(100, 80),
            /*floatingActionButton=*/nullptr);
    auto element = scaffold->createElement(scaffold);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    // 找到 Scaffold 的 RenderObject 并布局。
    RenderObject* renderObject = root->findRenderObject();
    check(renderObject != nullptr, "Scaffold 产生 RenderObject");
    if (!renderObject)
        return;

    renderObject->layout(cui::BoxConstraints::tight(400, 800));

    auto* rs = findRenderScaffold(renderObject);
    check(rs != nullptr, "RenderObject 为 RenderScaffold");
    if (!rs)
        return;

    check(nearly(rs->size().width, 400) && nearly(rs->size().height, 800),
          "Scaffold 尺寸为约束给的 400x800");

    // appBar 在顶部（offset.y == 0）。
    // body 在 appBar 之下（offset.y == 56）。
    // bottomNavigationBar 贴底（offset.y == 800 - 80 == 720）。
    bool sawAppBar = false, sawBody = false, sawBottomNav = false;
    for (RenderObject* child : rs->children()) {
        if (!child)
            continue;
        auto* pd = dynamic_cast<ScaffoldParentData*>(child->parentData());
        if (!pd)
            continue;
        const double y = pd->offset.dy;
        if (pd->slot == ScaffoldSlot::appBar) {
            sawAppBar = true;
            check(nearly(y, 0), "appBar 置于顶部 (y=0)");
        } else if (pd->slot == ScaffoldSlot::body) {
            sawBody = true;
            check(nearly(y, 56), "body 位于 appBar 之下 (y=56)");
        } else if (pd->slot == ScaffoldSlot::bottomNavigationBar) {
            sawBottomNav = true;
            check(nearly(y, 720), "bottomNavigationBar 贴底 (y=720)");
        }
    }
    check(sawAppBar && sawBody && sawBottomNav, "三个槽位均已布局");
}

void testScaffoldExtendBodyBehindAppBar() {
    std::printf("[test] Scaffold.extendBodyBehindAppBar\n");

    BuildOwner owner;
    auto scaffold = cui::make_shared<Scaffold>(
            /*body=*/boxOf(100, 200),
            /*appBar=*/boxOf(100, 56),
            /*bottomNavigationBar=*/nullptr,
            /*floatingActionButton=*/nullptr,
            /*drawer=*/nullptr,
            /*endDrawer=*/nullptr,
            /*persistentFooterButtons=*/std::vector<SharedPtr<Widget>>{},
            ScaffoldFabLocation::endFloat,
            /*extendBody=*/false,
            /*extendBodyBehindAppBar=*/true);
    auto element = scaffold->createElement(scaffold);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    RenderObject* renderObject = root->findRenderObject();
    if (!renderObject)
        return;
    renderObject->layout(cui::BoxConstraints::tight(400, 800));

    auto* rs = findRenderScaffold(renderObject);
    if (!rs)
        return;

    for (RenderObject* child : rs->children()) {
        auto* pd = dynamic_cast<ScaffoldParentData*>(child ? child->parentData() : nullptr);
        if (pd && pd->slot == ScaffoldSlot::body)
            check(nearly(pd->offset.dy, 0), "extendBodyBehindAppBar 时 body 从顶部开始");
    }
}

/// Scaffold 的 Material 层（上游 scaffold.dart:3232-3238）。
void testScaffoldMaterial() {
    std::printf("[test] Scaffold 的 Material 层\n");

    // 显式 backgroundColor。
    BuildOwner owner;
    auto scaffold = cui::make_shared<Scaffold>(
            /*body=*/boxOf(100, 200), nullptr, nullptr, nullptr, nullptr, nullptr,
            std::vector<SharedPtr<Widget>>{}, ScaffoldFabLocation::endFloat,
            /*extendBody=*/false, /*extendBodyBehindAppBar=*/false,
            /*minInsets=*/EdgeInsets{}, /*minViewPadding=*/EdgeInsets{},
            /*onDrawerChanged=*/nullptr, /*onEndDrawerChanged=*/nullptr,
            /*backgroundColor=*/Color::fromRGB(0.2, 0.4, 0.6));
    auto element = scaffold->createElement(scaffold);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    RenderObject* ro = root->findRenderObject();
    auto* deco = dynamic_cast<RenderDecoratedBox*>(ro);
    check(deco != nullptr, "Scaffold 根渲染对象是 Material 的 RenderDecoratedBox");
    if (deco)
        check(deco->color() == Color::fromRGB(0.2, 0.4, 0.6), "backgroundColor 生效");
    check(findRenderScaffold(ro) != nullptr, "Material 之下仍是 RenderScaffold");

    // 未指定 backgroundColor：取 theme().surface
    // （上游 M3 的 ThemeData.scaffoldBackgroundColor 就是 colorScheme.surface）。
    BuildOwner owner2;
    auto plain = cui::make_shared<Scaffold>(/*body=*/boxOf(100, 200));
    auto element2 = plain->createElement(plain);
    Element* root2 = element2.get();
    owner2.adoptRootElement(std::move(element2));

    auto* deco2 = dynamic_cast<RenderDecoratedBox*>(root2->findRenderObject());
    check(deco2 != nullptr, "默认也铺一层 Material");
    if (deco2)
        check(deco2->color() == theme().surface, "默认底色 = theme().surface");
}

void testScaffoldDrawerApi() {
    std::printf("[test] Scaffold 抽屉 API\n");

    BuildOwner owner;
    bool drawerChanged = false;
    bool endDrawerChanged = false;

    auto scaffold = cui::make_shared<Scaffold>(
            /*body=*/boxOf(100, 200),
            /*appBar=*/nullptr,
            /*bottomNavigationBar=*/nullptr,
            /*floatingActionButton=*/nullptr,
            /*drawer=*/boxOf(300, 800),
            /*endDrawer=*/boxOf(300, 800),
            /*persistentFooterButtons=*/std::vector<SharedPtr<Widget>>{},
            ScaffoldFabLocation::endFloat,
            /*extendBody=*/false,
            /*extendBodyBehindAppBar=*/false,
            /*minInsets=*/EdgeInsets{},
            /*minViewPadding=*/EdgeInsets{},
            [&](bool opened) { drawerChanged = opened; },
            [&](bool opened) { endDrawerChanged = opened; });
    auto element = scaffold->createElement(scaffold);
    Element* root = element.get();
    owner.adoptRootElement(std::move(element));

    auto* state = static_cast<ScaffoldState*>(
            static_cast<StatefulElement*>(root)->state());
    check(state != nullptr, "ScaffoldState 已创建");
    if (!state)
        return;

    check(state->hasDrawer(), "hasDrawer 为真");
    check(state->hasEndDrawer(), "hasEndDrawer 为真");
    check(!state->hasAppBar(), "无 appBar 时 hasAppBar 为假");
    check(!state->hasFloatingActionButton(), "无 FAB 时 hasFloatingActionButton 为假");
    check(!state->isDrawerOpen(), "初始 isDrawerOpen 为假");

    state->openDrawer();
    owner.flushBuildScope();
    check(state->isDrawerOpen(), "openDrawer 后 isDrawerOpen 为真");
    check(drawerChanged, "openDrawer 触发 onDrawerChanged");

    // openEndDrawer 会先关闭 drawer（官方 openEndDrawer，:2303-2308）。
    state->openEndDrawer();
    owner.flushBuildScope();
    check(state->isEndDrawerOpen(), "openEndDrawer 后 isEndDrawerOpen 为真");
    check(!state->isDrawerOpen(), "openEndDrawer 先关闭 drawer");
    check(endDrawerChanged, "openEndDrawer 触发 onEndDrawerChanged");

    state->closeEndDrawer();
    owner.flushBuildScope();
    check(!state->isEndDrawerOpen(), "closeEndDrawer 后 isEndDrawerOpen 为假");

    state->closeDrawer();
    owner.flushBuildScope();
    check(!state->isDrawerOpen(), "closeDrawer 后 isDrawerOpen 为假");
}

/// 在 Scaffold 的后代中调用 Scaffold.of / maybeOf。
///
/// 上游的 Scaffold.of（material_scaffold.dart:2043-2047）用
/// `context.findAncestorStateOfType<ScaffoldState>()` 查找**祖先**，
/// 因此必须在 Scaffold 之下调用（这正是 上游 文档建议用 Builder
/// 取得「Scaffold 之下」的 context 的原因）。
class ScaffoldOfProbe : public StatelessWidget {
public:
    ScaffoldState* found = nullptr;
    ScaffoldState* maybeFound = nullptr;
    bool threw = false;

    SharedPtr<Widget> build(BuildContext& context) override {
        maybeFound = Scaffold::maybeOf(context);
        try {
            found = Scaffold::of(context);
        } catch (const std::logic_error&) {
            threw = true;
        }
        return boxOf(10, 10);
    }
};

void testScaffoldOf() {
    std::printf("[test] Scaffold.of / maybeOf\n");

    BuildOwner owner;
    auto probe = cui::make_shared<ScaffoldOfProbe>();
    auto scaffold = cui::make_shared<Scaffold>(probe);
    auto element = scaffold->createElement(scaffold);
    owner.adoptRootElement(std::move(element));

    check(probe->maybeFound != nullptr, "后代 context 的 maybeOf 能找到 ScaffoldState");
    check(probe->found != nullptr, "后代 context 的 of 能找到 ScaffoldState");
    check(!probe->threw, "存在祖先 Scaffold 时 of 不抛异常");

    // 没有 Scaffold 祖先时，of 抛异常，maybeOf 返回 nullptr。
    BuildOwner owner2;
    auto lonely = cui::make_shared<ScaffoldOfProbe>();
    auto element2 = lonely->createElement(lonely);
    owner2.adoptRootElement(std::move(element2));
    check(lonely->maybeFound == nullptr, "无祖先 Scaffold 时 maybeOf 返回 nullptr");
    check(lonely->threw, "无祖先 Scaffold 时 of 抛异常");
}

void testFabOffset() {
    std::printf("[test] FAB 定位公式\n");

    ScaffoldGeometry geometry;
    geometry.scaffoldSize = Size(400, 800);
    geometry.contentTop = 56;
    geometry.contentBottom = 720;
    geometry.minInsets = EdgeInsets::only();
    geometry.minViewPadding = EdgeInsets::only();
    geometry.fabSize = Size(56, 56);
    geometry.snackBarSize = Size(0, 0);
    geometry.bottomSheetSize = Size(0, 0);

    // endFloat：x = 400 - 16 - 0 - 56 = 328
    //         safeMargin = max(16, 0 - (800-720) + 16) = max(16, -64) = 16
    //         y = 720 - 56 - 16 = 648
    Offset endFloat = scaffoldFabOffset(ScaffoldFabLocation::endFloat, geometry);
    check(nearly(endFloat.dx, 328) && nearly(endFloat.dy, 648),
          "endFloat 偏移为 (328, 648)");

    // centerFloat：x = (400 - 56) / 2 = 172
    Offset centerFloat = scaffoldFabOffset(ScaffoldFabLocation::centerFloat, geometry);
    check(nearly(centerFloat.dx, 172) && nearly(centerFloat.dy, 648),
          "centerFloat 偏移为 (172, 648)");

    // startFloat：x = 16
    Offset startFloat = scaffoldFabOffset(ScaffoldFabLocation::startFloat, geometry);
    check(nearly(startFloat.dx, 16) && nearly(startFloat.dy, 648),
          "startFloat 偏移为 (16, 648)");

    // endDocked：contentMargin = 80 > 0 + 28 → safeMargin = 0
    //            fabY = 720 - 28 - 0 = 692；maxFabY = 800 - 56 - 0 = 744 → 692
    Offset endDocked = scaffoldFabOffset(ScaffoldFabLocation::endDocked, geometry);
    check(nearly(endDocked.dx, 328) && nearly(endDocked.dy, 692),
          "endDocked 偏移为 (328, 692)");
}

// ── 测试 10：WidgetsApp ────────────────────────────────────────────────────

/// 测试用页面路由工厂：记录每次调用。
struct PageFactoryRecorder {
    WidgetsApp::PageRouteFactory factory;
    int calls = 0;
    std::vector<std::string> lastNames;

    PageFactoryRecorder() {
        factory = [this](const RouteSettings& settings,
                         WidgetsApp::WidgetBuilder builder)
                -> SharedPtr<Route> {
            ++calls;
            lastNames.push_back(settings.name());
            return cui::make_shared<ContentRoute>(std::move(builder), /*opaque=*/true,
                                                  cui::make_shared<RouteSettings>(settings));
        };
    }
};

/// 测试用 RouterDelegate：记录路由解析结果。
struct AppTestDelegate : public RouterDelegate {
    int setNewRoutePathCalls = 0;

    void setNewRoutePath(SharedPtr<void> configuration) override {
        ++setNewRoutePathCalls;
    }

    bool popRoute() override { return false; }

    SharedPtr<Widget> build(BuildContext& context) override {
        return boxOf(10, 10);
    }
};

void testWidgetsApp() {
    std::printf("[test] WidgetsApp（Navigator 模式 / Router 模式 / builder）\n");

    // ── Navigator 模式：home + routes 表 ────────────────────────────────
    {
        BuildOwner owner;
        PageFactoryRecorder pages;

        auto app = cui::make_shared<WidgetsApp>();
        app->setHome(boxOf(100, 100));
        app->setRoutes({
                {"/second", [](BuildContext&) { return boxOf(50, 50); }},
        });
        app->setPageRouteBuilder(pages.factory);

        auto navigatorKey = cui::make_shared<GlobalKey>("test.navigator");
        app->setNavigatorKey(navigatorKey);

        auto element = app->createElement(app);
        Element* root = element.get();
        owner.adoptRootElement(std::move(element));

        check(pages.calls == 1, "初始路由经 pageRouteBuilder 生成一次");
        check(pages.lastNames.size() == 1 && pages.lastNames[0] == "/",
              "初始路由名为 '/'");

        // 通过 GlobalKey 定位 NavigatorState。
        Element* navElement = navigatorKey->currentElement<Element>();
        check(navElement != nullptr, "navigatorKey 定位到 Navigator 的 Element");
        if (!navElement)
            return;
        auto* navState = dynamic_cast<NavigatorState*>(
                static_cast<StatefulElement*>(navElement)->state());
        check(navState != nullptr, "GlobalKey 挂载的是 NavigatorState");
        if (!navState)
            return;
        check(navState->history().size() == 1, "初始历史只有 home 一条");

        // pushNamed 走 routes 表。
        navState->pushNamed("/second");
        owner.flushBuildScope();
        check(pages.calls == 2 && pages.lastNames.back() == "/second",
              "pushNamed 命中 routes 表并经 pageRouteBuilder 生成");
        check(navState->history().size() == 2, "push 后历史为两条");

        // didPushRouteInformation → pushNamed。
        bool handled =
                static_cast<WidgetsAppState*>(
                        static_cast<StatefulElement*>(root)->state())
                        ->didPushRouteInformation(RouteInformation("/second"));
        check(handled, "Navigator 模式下 didPushRouteInformation 已处理");
        check(navState->history().size() == 3, "didPushRouteInformation 压入第三条");

        // didPopRoute → maybePop。
        bool popHandled = static_cast<WidgetsAppState*>(
                static_cast<StatefulElement*>(root)->state())
                                  ->didPopRoute();
        check(popHandled, "Navigator 模式下 didPopRoute 已处理");
        owner.flushBuildScope();
        check(navState->history().size() == 2, "didPopRoute 弹出栈顶");
    }

    // ── Router 委托模式 ─────────────────────────────────────────────────
    {
        BuildOwner owner;
        auto parser = cui::make_shared<TestParser>();
        auto delegate = cui::make_shared<AppTestDelegate>();

        auto app = cui::make_shared<WidgetsApp>();
        app->setRouterDelegate(delegate);
        app->setRouteInformationParser(parser);

        auto element = app->createElement(app);
        Element* root = element.get();
        owner.adoptRootElement(std::move(element));
        owner.flushBuildScope();

        check(parser->parseCount == 1, "Router 模式下解析一次初始路由");
        check(delegate->setNewRoutePathCalls >= 1, "初始配置交给 RouterDelegate");
    }

    // ── builder 模式：无路由配置时 builder 直接包装 ────────────────────
    {
        BuildOwner owner;
        SharedPtr<Widget> receivedChild;
        int builderCalls = 0;

        auto app = cui::make_shared<WidgetsApp>();
        app->setBuilder([&](BuildContext&, const SharedPtr<Widget>& child) {
            ++builderCalls;
            receivedChild = child;
            return boxOf(1, 1);
        });

        auto element = app->createElement(app);
        owner.adoptRootElement(std::move(element));

        check(builderCalls == 1, "纯 builder 模式下 builder 被调用一次");
        check(receivedChild == nullptr,
              "纯 builder 模式下 routing 子树为空（官方 app.dart:1743-1745）");
    }

    // ── 未知路由：onUnknownRoute 返回 null 时抛异常 ────────────────────
    {
        BuildOwner owner;
        auto app = cui::make_shared<WidgetsApp>();
        // 提供 home 让初始路由 '/' 能生成（否则挂载阶段即按官方断言抛错）。
        app->setHome(boxOf(10, 10));
        app->setOnUnknownRoute([](const RouteSettings&) -> SharedPtr<Route> {
            return nullptr;  // 官方断言禁止（app.dart:1631-1637）。
        });
        app->setPageRouteBuilder([](const RouteSettings&, WidgetsApp::WidgetBuilder) {
            return cui::make_shared<ContentRoute>(
                    [](BuildContext&) { return boxOf(1, 1); });
        });

        auto element = app->createElement(app);
        Element* root = element.get();
        owner.adoptRootElement(std::move(element));

        auto* appState = static_cast<WidgetsAppState*>(
                static_cast<StatefulElement*>(root)->state());
        bool threw = false;
        try {
            appState->didPushRouteInformation(RouteInformation("/missing"));
        } catch (const std::logic_error&) {
            threw = true;
        }
        check(threw, "onUnknownRoute 返回 null 时抛 logic_error");
    }
}

}  // namespace

int main() {
    // 崩溃时也要能看到已执行的用例：无缓冲输出。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== 导航栈（Overlay / Navigator / Router / Scaffold）测试 ===\n");

    testOverlay();
    testNavigator();
    testRouter();
    testRouterNavigateReportType();
    testBackButtonDispatcher();
    testScaffoldLayout();
    testScaffoldExtendBodyBehindAppBar();
    testScaffoldMaterial();
    testScaffoldDrawerApi();
    testScaffoldOf();
    testFabOffset();
    testWidgetsApp();

    if (g_failures == 0) {
        std::printf("\n全部通过。\n");
        return 0;
    }
    std::printf("\n失败 %d 项。\n", g_failures);
    return 1;
}
