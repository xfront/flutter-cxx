/// @file test_tabs.cpp
/// @brief 上游 TabController / TabBar / TabBarView / ExpansionTile / Stepper
///        单元测试（第三批 port 验证）。
///
/// 覆盖：
///   1. TabController::setIndex 出界 clamp + 监听通知
///   2. TabBarView：IndexedStack 只显示 controller.index() 指向的子页；
///      setIndex 驱动重建后可见页切换
///   3. TabBar：选中项高亮（只有当前项带装饰）；点击第 N 个标签 → 设 index=N
///   4. ExpansionTile：折叠不渲染 children；点头部展开（箭头是矢量图标、贴右端，
///      主体按 childrenPadding 左对齐落在头部下方），再点收起且主体等动画收尾才摘
///   5. Stepper：只渲染当前步内容，已完成/未来步不渲染内容；所有步标题渲染
///   6. Tabs 复合控件（Stack + Positioned 标签栏）放在竖向滚动页里点击切页
///   7. 标签溢出下拉菜单：挂在 Overlay 条目上，菜单项可选中、barrier 可点
///
/// 全部 headless（无窗口）。使用 PipelineOwner::dispatchPointerEvent 驱动点击；
/// 只验证布局/子树结构/命中（确定性），不验证像素绘制——真实绘制由生产环境
/// 持有画布时驱动。注意 RenderView 给的是紧约束（窗口尺寸），本测试大多直接
/// attach 根控件，由 Row/Column 的 MainAxisSize::Min 决定自身尺寸。

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_icon.h"
#include "cui/rendering/render_inkwell.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/controls.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/material.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/tabs.h"
#include "cui/widgets/text.h"

using namespace cui;

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

/// 把 Widget 树接入 BuildOwner + RenderView + PipelineOwner，并布局好。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;  // 必须最后声明，保证 FocusManager 销毁时仍存活
    Element* root = nullptr;
    RenderObject* rootRO = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        rootRO = root->findRenderObject();
        rv.setChild(static_cast<RenderBox*>(rootRO));
        po.setRootNode(&rv);
        pump();
    }

    void dispatch(const PointerEvent& ev) { po.dispatchPointerEvent(ev); }

    /// 手势/状态回调触发 setState 后：冲刷 build scope + 布局（不 flushPaint）。
    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    /// 深度优先取树中第一个指定类型的 RenderObject。
    template <typename T>
    T* findFirst() {
        struct Visitor {
            T* found = nullptr;
            void visit(RenderObject* ro) {
                if (!ro || found)
                    return;
                if (auto* hit = dynamic_cast<T*>(ro))
                    found = hit;
                ro->visitChildren([this](AbstractNode* n) {
                    visit(static_cast<RenderObject*>(n));
                });
            }
        } v;
        v.visit(rootRO);
        return v.found;
    }

    /// DFS 收集树中所有指定类型的 RenderObject。
    template <typename T>
    std::vector<T*> findAll() {
        std::vector<T*> out;
        struct Visitor {
            std::vector<T*>* out;
            void visit(RenderObject* ro) {
                if (!ro)
                    return;
                if (auto* hit = dynamic_cast<T*>(ro))
                    out->push_back(hit);
                ro->visitChildren([this](AbstractNode* n) {
                    visit(static_cast<RenderObject*>(n));
                });
            }
        } v{&out};
        v.visit(rootRO);
        return out;
    }

    /// DFS 找文本内容包含 sub 的 RenderParagraph（子树结构检测用）。
    RenderParagraph* findParagraphContaining(const std::string& sub) {
        struct Visitor {
            const std::string* sub;
            RenderParagraph* found = nullptr;
            void visit(RenderObject* ro) {
                if (!ro || found)
                    return;
                if (auto* p = dynamic_cast<RenderParagraph*>(ro)) {
                    if (p->text() && p->text()->toPlainText().find(*sub) !=
                                         std::string::npos)
                        found = p;
                }
                ro->visitChildren([this](AbstractNode* n) {
                    visit(static_cast<RenderObject*>(n));
                });
            }
        } v{&sub};
        v.visit(rootRO);
        return v.found;
    }

    /// 文本 sub 是否“可见”——即其祖先链上没有 offstage=true 的 RenderOffstage。
    bool paragraphVisible(const std::string& sub) {
        struct Ctx {
            const std::string* sub;
            bool found = false;
        } ctx{&sub};
        std::function<void(RenderObject*, bool)> walk =
                [&](RenderObject* n, bool hidden) {
            if (!n || ctx.found)
                return;
            if (auto* off = dynamic_cast<RenderOffstage*>(n))
                hidden = hidden || off->offstage();
            if (auto* p = dynamic_cast<RenderParagraph*>(n)) {
                if (!hidden && p->text() &&
                    p->text()->toPlainText().find(*ctx.sub) != std::string::npos)
                    ctx.found = true;
            }
            n->visitChildren([&](AbstractNode* c) {
                walk(static_cast<RenderObject*>(c), hidden);
            });
        };
        walk(rootRO, false);
        return ctx.found;
    }

    /// 计算某个 RenderObject 中心点的窗口全局坐标（用于 dispatch 点击）。
    Offset centerGlobal(RenderObject* ro) {
        if (auto* box = dynamic_cast<RenderBox*>(ro))
            return box->localToGlobal(
                    {box->size().width / 2.0, box->size().height / 2.0});
        return {0.0, 0.0};
    }
};

PointerDownEvent downAt(double x, double y) {
    PointerDownEvent e;
    e.position = {x, y};
    e.buttons = kPrimaryMouseButton;
    return e;
}
PointerUpEvent upAt(double x, double y) {
    PointerUpEvent e;
    e.position = {x, y};
    return e;
}

/// 按真实时钟把隐式动画推到收尾（ExpansionTile 的高度补间 + 箭头旋转）。
/// 上限 maxMs 兜底，避免意外不收敛时卡死测试。
void settle(Harness& h, double maxMs = 800.0) {
    double spent = 0.0;
    while (spent < maxMs && Ticker::hasActiveTickers()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        Ticker::tickAllActive(Duration(8));
        h.pump();
        spent += 8.0;
    }
}

// ── 测试 1：TabController 行为 ──────────────────────────────────────────────
void testTabController() {
    std::printf("[test] TabController 行为\n");

    TabController c(3);
    check(c.index() == 0, "初始 index = 0");

    int fires = 0;
    auto unsub = c.addListener([&]() { ++fires; });
    c.setIndex(2);
    check(c.index() == 2, "setIndex(2) → index = 2");
    check(fires == 1, "监听被通知一次");

    c.setIndex(2);  // 相同值不触发
    check(fires == 1, "相同 index 不重复通知");

    c.setIndex(99);  // 出界 clamp 到 length-1
    check(c.index() == 2, "出界 setIndex 被 clamp 到 length-1");

    c.setIndex(-5);
    check(c.index() == 0, "负 index 被 clamp 到 0");

    int firesBeforeUnsub = fires;  // 至此已因若干次实际变化触发
    unsub();  // 注销后不再通知
    c.setIndex(1);
    check(c.index() == 1, "注销后 index 仍更新");
    check(fires == firesBeforeUnsub, "注销后监听不再触发（次数不再增加）");
}

// ── 测试 2：TabBarView IndexedStack 切换 ────────────────────────────────────
void testTabBarView() {
    std::printf("[test] TabBarView + IndexedStack\n");

    TabController controller(3);
    std::vector<SharedPtr<Widget>> pages = {
        cui::make_shared<Text>("Page0"),
        cui::make_shared<Text>("Page1"),
        cui::make_shared<Text>("Page2"),
    };
    auto view = cui::make_shared<TabBarView>(&controller, pages);

    Harness h;
    h.attach(view, 800, 600);

    // index=0：Page0 可见，Page1/Page2 被 Offstage 包裹（不可见）。
    check(h.paragraphVisible("Page0"), "index=0：Page0 可见");
    check(!h.paragraphVisible("Page1"), "index=0：Page1 不可见（Offstage）");
    check(!h.paragraphVisible("Page2"), "index=0：Page2 不可见（Offstage）");

    // 非当前页应被包进 RenderOffstage。
    auto* off = h.findFirst<RenderOffstage>();
    check(off != nullptr, "存在 RenderOffstage 包裹非当前页");

    // 切到 index=2：Page2 可见，Page0 不可见。
    controller.setIndex(2);
    h.pump();
    check(controller.index() == 2, "controller.index() = 2");
    check(h.paragraphVisible("Page2"), "切页后 Page2 可见");
    check(!h.paragraphVisible("Page0"), "切页后 Page0 不可见");

    // 切回 index=1。
    controller.setIndex(1);
    h.pump();
    check(h.paragraphVisible("Page1"), "再次切页后 Page1 可见");
    check(!h.paragraphVisible("Page0"), "Page0 仍不可见");
}

// ── 测试 3：TabBar 点击切页 ────────────────────────────────────────────────
void testTabBarTap() {
    std::printf("[test] TabBar 点击切页\n");

    TabController controller(3);
    std::vector<SharedPtr<Widget>> tabs = {
        cui::make_shared<Text>("TabA"),
        cui::make_shared<Text>("TabB"),
        cui::make_shared<Text>("TabC"),
    };
    auto bar = cui::make_shared<TabBar>(&controller, tabs);

    Harness h;
    h.attach(bar, 1000, 1000);
    check(controller.index() == 0, "初始选中 TabA (index 0)");

    // 默认只有 TabA（index 0）带高亮装饰（RenderDecoratedBox）。
    auto* deco = h.findFirst<RenderDecoratedBox>();
    check(deco != nullptr, "当前选中项带高亮装饰");

    // 找到 Row（RenderFlex）的第二个直接子节点（TabB 的 InkWell）。
    auto* flex = h.findFirst<RenderFlex>();
    check(flex != nullptr, "TabBar 渲染为 Row(RenderFlex)");
    const auto& kids = flex->children();
    check(kids.size() == 3, "Row 含 3 个标签单元");

    Offset c2 = h.centerGlobal(kids[1]);  // TabB 中心
    h.dispatch(downAt(c2.dx, c2.dy));
    h.dispatch(upAt(c2.dx, c2.dy));
    check(controller.index() == 1, "点击 TabB → index = 1");

    // TabB 现在高亮：重建后其单元应带 RenderDecoratedBox。
    h.pump();
    auto* decos = h.findFirst<RenderDecoratedBox>();
    check(decos != nullptr, "切到 TabB 后高亮装饰仍然存在");

    // 再点第三个标签 TabC。
    auto* flex2 = h.findFirst<RenderFlex>();
    Offset c3 = h.centerGlobal(flex2->children()[2]);
    h.dispatch(downAt(c3.dx, c3.dy));
    h.dispatch(upAt(c3.dx, c3.dy));
    check(controller.index() == 2, "点击 TabC → index = 2");
}

// ── 测试 4：ExpansionTile 展开/折叠 ────────────────────────────────────────
void testExpansionTile() {
    std::printf("[test] ExpansionTile 展开/折叠\n");

    // childrenPadding/expandedAlignment/expandedCrossAxisAlignment 显式给值：
    // 上游默认都是 center（主体整块居中），story 与本页演示取左上。
    auto tile = cui::make_shared<ExpansionTile>(
            cui::make_shared<Text>("标题X"),
            std::vector<SharedPtr<Widget>>{
                cui::make_shared<Text>("子项1"),
                cui::make_shared<Text>("子项2"),
            },
            false, nullptr, EdgeInsets::only(16.0, 0.0, 16.0, 8.0),
            Alignment::topLeft(), CrossAxisAlignment::Start);

    Harness h;
    h.attach(tile, 400, 600);

    check(h.paragraphVisible("标题X"), "标题始终可见");
    check(!h.paragraphVisible("子项1"), "初始折叠：子项1 不渲染");
    check(!h.paragraphVisible("子项2"), "初始折叠：子项2 不渲染");
    check(h.findFirst<RenderIcon>() != nullptr, "折叠态也画得出箭头（矢量图标，不靠字体字形）");

    // 点击头部（Column 第一个子节点 = 头部 InkWell）展开。
    auto* col = h.findFirst<RenderFlex>();  // ExpansionTile → Column
    check(col != nullptr, "ExpansionTile 渲染为 Column");
    RenderObject* header = col->children()[0];
    Offset head = h.centerGlobal(header);
    h.dispatch(downAt(head.dx, head.dy));
    h.dispatch(upAt(head.dx, head.dy));
    h.pump();
    check(h.paragraphVisible("子项1"), "点击头部后：子项1 渲染（展开）");
    check(h.paragraphVisible("子项2"), "点击头部后：子项2 渲染（展开）");
    settle(h);

    // 展开态几何：主体在头部下方，且按 childrenPadding 贴在左边缘（不是整页居中）。
    Offset headCenter = h.centerGlobal(header);
    auto* body1 = h.findParagraphContaining("子项1");
    check(body1 != nullptr, "展开态找到子项1 段落");
    if (body1) {
        Offset c1 = h.centerGlobal(body1);
        check(c1.dy > headCenter.dy, "子项1 在头部下方");
        check(c1.dx < 100.0, "子项1 左对齐（expandedAlignment=topLeft）");
    }
    // 箭头在瓦片右端：Expanded 把标题撑开，所以 trailing 永远贴右边。
    if (auto* icon = h.findFirst<RenderIcon>()) {
        Offset ic = h.centerGlobal(icon);
        check(ic.dx > 340.0, "箭头贴在最右（trailing）");
        check(std::abs(ic.dy - headCenter.dy) <= 2.0, "箭头与标题同一行");
    }

    // 再次点击头部折叠：点行尾（远离文字）同样命中，主体要等淡出收尾才摘掉
    // —— 对应上游 maintainState=false 的 shouldRemoveBody。
    auto* col2 = h.findFirst<RenderFlex>();
    Offset headRight = h.centerGlobal(col2->children()[0]) + Offset{150.0, 0.0};
    h.dispatch(downAt(headRight.dx, headRight.dy));
    h.dispatch(upAt(headRight.dx, headRight.dy));
    h.pump();
    check(h.paragraphVisible("子项1"), "收起那一帧：主体仍在树上（动画进行中）");
    settle(h);
    check(!h.paragraphVisible("子项1"), "收起动画收尾：子项1 消失（折叠）");
    check(!h.paragraphVisible("子项2"), "收起动画收尾：子项2 消失（折叠）");

    // initiallyExpanded=true：第一帧就带着主体，不需要先跑一段动画。
    auto open = cui::make_shared<ExpansionTile>(
            cui::make_shared<Text>("标题Y"),
            std::vector<SharedPtr<Widget>>{cui::make_shared<Text>("子项3")}, true);
    Harness h2;
    h2.attach(open, 400, 600);
    check(h2.paragraphVisible("子项3"), "initiallyExpanded：首帧即展开");
}

// ── 测试 5：Stepper 当前步 ──────────────────────────────────────────────────
void testStepper() {
    std::printf("[test] Stepper 当前步\n");

    std::vector<Step> steps = {
        {cui::make_shared<Text>("Step0 标题"), cui::make_shared<Text>("STEP0_CONTENT")},
        {cui::make_shared<Text>("Step1 标题"), cui::make_shared<Text>("STEP1_CONTENT")},
        {cui::make_shared<Text>("Step2 标题"), cui::make_shared<Text>("STEP2_CONTENT")},
    };
    auto stepper = cui::make_shared<Stepper>(1, steps);  // 当前步 = 1

    Harness h;
    h.attach(stepper, 600, 800);

    // 所有步标题都渲染。
    check(h.paragraphVisible("Step0 标题"), "第 0 步标题渲染");
    check(h.paragraphVisible("Step1 标题"), "第 1 步标题渲染");
    check(h.paragraphVisible("Step2 标题"), "第 2 步标题渲染");

    // 只有当前步（1）渲染内容。
    check(h.paragraphVisible("STEP1_CONTENT"), "当前步(1)内容可见");
    check(!h.paragraphVisible("STEP2_CONTENT"), "未达步(2)内容不渲染");
    check(!h.paragraphVisible("STEP0_CONTENT"), "已完成步(0)内容不渲染（仅活动步显示内容）");
}

// ── 测试 6：复合 Tabs 在滚动页面里点击切页 ─────────────────────────────────
// story 的页壳把每页放进竖向 SingleChildScrollView（主轴无限），图表页正是
// 这样用到 20 个标签 ⇒ 触发溢出下拉分支。以前无界主轴下的 Expanded 会吃掉
// 1e18 紧约束，标签栏虽画得出来却落在父盒命中框之外（点击无响应）。
void testTabsComposite() {
    std::printf("[test] Tabs 复合控件点击切页（滚动页 + 溢出下拉）\n");

    std::vector<std::string> labels;
    std::vector<SharedPtr<Widget>> pages;
    for (int i = 0; i < 20; ++i) {
        labels.push_back("Chart" + std::to_string(i));
        pages.push_back(cui::make_shared<Text>("PAGE-" + std::to_string(i)));
    }
    auto tabs = cui::make_shared<Tabs>(labels, pages);

    // Scrollable 需要真实控制器（本仓未接 PrimaryScrollController 兜底）。
    ScrollController pageScroll;
    Harness h;
    h.attach(cui::make_shared<SingleChildScrollView>(&pageScroll, tabs), 1000, 600);
    check(h.paragraphVisible("PAGE-0"), "初始显示第 0 页");

    auto wells = h.findAll<RenderInkWell>();
    check(wells.size() >= 2, "标签栏渲染出多个 RenderInkWell");
    if (wells.size() < 2)
        return;

    Offset c1 = h.centerGlobal(wells[1]);
    h.dispatch(downAt(c1.dx, c1.dy));
    h.dispatch(upAt(c1.dx, c1.dy));
    h.pump();
    check(h.paragraphVisible("PAGE-1"), "点击第 2 个标签 → 切到第 1 页");
    check(!h.paragraphVisible("PAGE-0"), "切页后第 0 页不再可见");
}

// ── 测试 7：溢出下拉经 Overlay 可选中折叠标签 ──────────────────────────────
// 菜单内联在 40 高的标签栏 Stack 里时，面板落在所有祖先盒子之外，
// RenderBox::hitTest 的 size 判定把它整棵子树挡在命中链之外 ⇒ 面板画得出来
// 却一个都点不到。改成挂到上层 Overlay 的条目后（同 ComboBox），条目在浮层
// 坐标系里定位，命中链从 Overlay 那层 Stack 走下来，菜单项与 barrier 都可达。
void testTabsOverflowMenu() {
    std::printf("[test] Tabs 溢出下拉（Overlay 浮层）\n");

    std::vector<std::string> labels;
    std::vector<SharedPtr<Widget>> pages;
    for (int i = 0; i < 20; ++i) {
        labels.push_back("Chart" + std::to_string(i));
        pages.push_back(cui::make_shared<Text>("PAGE-" + std::to_string(i)));
    }
    auto tabs = cui::make_shared<Tabs>(labels, pages);

    // Overlay 作为祖先：内容本身也走条目（上游的用法）。
    auto hostEntry = cui::make_shared<OverlayEntry>(
        [tabs](BuildContext&) { return tabs; });
    auto overlay = cui::make_shared<Overlay>(
        std::vector<SharedPtr<OverlayEntry>>{hostEntry});

    Harness h;
    h.attach(overlay, 1000, 600);
    auto* overlayState =
            static_cast<OverlayState*>(static_cast<StatefulElement*>(h.root)->state());

    check(!h.paragraphVisible("Chart19"), "菜单关闭时折叠标签不在树上");
    check(overlayState->entries().size() == 1, "初始 Overlay 只有内容条目");

    RenderParagraph* arrow = h.findParagraphContaining("\xe2\x96\xbc");  // ▼
    check(arrow != nullptr, "标签栏右侧渲染出 ▼ 溢出按钮");
    if (!arrow)
        return;
    Offset a = h.centerGlobal(arrow);
    h.dispatch(downAt(a.dx, a.dy));
    h.dispatch(upAt(a.dx, a.dy));
    h.pump();
    check(h.paragraphVisible("Chart19"), "点 ▼ → 浮层菜单里出现折叠标签");
    check(overlayState->entries().size() == 2, "菜单展开后 Overlay 上多了一个条目");

    // barrier：点面板之外（下方空白）收起菜单，且不改选中页。
    h.dispatch(downAt(500.0, 560.0));
    h.dispatch(upAt(500.0, 560.0));
    h.pump();
    check(overlayState->entries().size() == 1, "点 barrier → 浮层条目移除");
    check(!h.paragraphVisible("PAGE-19"), "点 barrier 不切页");

    h.dispatch(downAt(a.dx, a.dy));
    h.dispatch(upAt(a.dx, a.dy));
    h.pump();

    RenderParagraph* item = h.findParagraphContaining("Chart19");
    check(item != nullptr, "菜单项 Chart19 在树上");
    if (!item)
        return;
    Offset p = h.centerGlobal(item);
    std::vector<RenderObject*> chain;
    h.rv.hitTest(p, chain);
    check(chain.size() > 1, "菜单项中心点命中链非空（浮层在 Overlay 之上）");

    h.dispatch(downAt(p.dx, p.dy));
    h.dispatch(upAt(p.dx, p.dy));
    h.pump();
    check(h.paragraphVisible("PAGE-19"), "点菜单项 → 切到第 19 页");
    check(overlayState->entries().size() == 1, "选中后菜单自动收起");

    // 再打开一次并保持开着离开作用域：整棵树拆除时 TabBarState::dispose 的
    // removeMenu() 会在 Overlay 卸载之前跑，条目摘除不能打到已失效的宿主。
    h.dispatch(downAt(a.dx, a.dy));
    h.dispatch(upAt(a.dx, a.dy));
    h.pump();
    check(overlayState->entries().size() == 2, "菜单可再次打开（拆除路径就此验证）");
}

}  // namespace

int main() {
    std::printf("=== test_tabs ===\n");
    std::setbuf(stdout, nullptr);
    testTabController();
    testTabBarView();
    testTabBarTap();
    testExpansionTile();
    testStepper();
    testTabsComposite();
    testTabsOverflowMenu();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
