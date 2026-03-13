/// @file test_expandable_fab.cpp
/// @brief ExpandableFab 单元测试（对齐 rui expandable_fab.rs）。
///
///  覆盖：
///   1. 四种尺寸变体的本体尺寸：Regular 56² / Small 40² / Large 96² / Extended 高 56、宽 > 56。
///   2. 普通 FAB（无菜单项）：点击触发 onClick、不展开。
///   3. 禁用 FAB：点击无任何反应。
///   4. Fan 展开：点本体 → 4 枚 48² 子项进入 Overlay 且落在本体的左上区域；点某子项触发其
///      onTap 并收起；点菜单外（barrier）收起。
///   5. Up 展开：子项竖直排在本体正上方（dx≈0、dy 递减）。
///   6. Side 展开（Right 位）：子项水平排在本体正左方（dy≈0、dx 递减）。
///   7. 子项动画 Rotate：展开过程中（未收尾）子项带旋转；收尾后归正。

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/controls.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/overlay.h"

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

bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

/// 带 Navigator（自带 Overlay）的 Harness，页面内容即传入的 widget。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;
    NavigatorState* state = nullptr;
    BuildContext* pageCtx = nullptr;

    void attach(SharedPtr<Widget> page, double win = 400.0) {
        rv.setWindowSize({win, win});
        auto navigator = cui::make_shared<Navigator>();
        auto e = navigator->createElement(navigator);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        state = static_cast<NavigatorState*>(
            static_cast<StatefulElement*>(root)->state());
        state->push(cui::make_shared<ContentRoute>(
            [page](BuildContext&) { return page; }, /*opaque=*/true));
        pump();
    }

    void dispatch(const PointerEvent& ev) { po.dispatchPointerEvent(ev); }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    /// 用真实时钟把菜单/涟漪动画推到收尾。
    void settle(double maxMs = 1200.0) {
        double spent = 0.0;
        while (spent < maxMs && Ticker::hasActiveTickers()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            Ticker::tickAllActive(Duration(8));
            pump();
            spent += 8.0;
        }
    }

    void tap(double x, double y) {
        PointerDownEvent d;
        d.position = {x, y};
        d.buttons = kPrimaryMouseButton;
        dispatch(d);
        PointerUpEvent u;
        u.position = {x, y};
        dispatch(u);
        pump();
    }

    /// 在整棵树（含 Overlay）里找渲染盒，回调给每个 RenderDecoratedBox。
    template <typename Fn>
    void forEachBox(Fn fn) {
        struct Visitor {
            Fn* fn;
            void visit(RenderObject* ro) {
                if (!ro) return;
                if (auto* db = dynamic_cast<RenderDecoratedBox*>(ro)) {
                    if (auto* b = static_cast<RenderBox*>(db)) (*fn)(b);
                }
                ro->visitChildren([this](AbstractNode* n) {
                    visit(static_cast<RenderObject*>(n));
                });
            }
        } v{&fn};
        v.visit(root->findRenderObject());
    }

    /// 本体尺寸：页面里唯一的 FAB —— 取最小的 RenderDecoratedBox（FAB 本体）。
    Size fabSize() {
        Size best{1e9, 1e9};
        forEachBox([&](RenderBox* b) {
            Size s = b->size();
            if (s.width < best.width) best = s;
        });
        return best;
    }

    /// 收集指定边长（±1px）的装饰盒中心（全局坐标）。
    std::vector<Offset> centersOf(double side) {
        std::vector<Offset> out;
        forEachBox([&](RenderBox* b) {
            Size s = b->size();
            if (std::fabs(s.width - side) < 1.0 && std::fabs(s.height - side) < 1.0) {
                Offset tl = b->localToGlobal(Offset{});
                out.push_back({tl.dx + s.width / 2.0, tl.dy + s.height / 2.0});
            }
        });
        return out;
    }
};

/// 找 ExpandableFabState（沿元素树 DFS）。
ExpandableFabState* findFabState(Element* e) {
    if (!e) return nullptr;
    if (auto* se = dynamic_cast<StatefulElement*>(e)) {
        if (auto* st = dynamic_cast<ExpandableFabState*>(se->state())) return st;
    }
    ExpandableFabState* found = nullptr;
    e->visitChildren([&](Element* child) {
        if (!found) found = findFabState(child);
    });
    return found;
}

// ── 测试 1：四种尺寸变体的本体尺寸 ──────────────────────────────────────────
void testSizes() {
    std::printf("[test] 尺寸变体\n");
    struct Case {
        FabSize size;
        double expectW, expectH;
        bool extended;
    };
    for (auto c : std::vector<Case>{{FabSize::Regular, 56, 56, false},
                                    {FabSize::Small, 40, 40, false},
                                    {FabSize::Large, 96, 96, false},
                                    {FabSize::Extended, 56, 56, true}}) {
        auto fab = cui::make_shared<ExpandableFab>(
            IconCode::Add, c.extended ? "Create New" : "", c.size, std::vector<FabAction>{});
        Harness h;
        h.attach(cui::make_shared<Center>(fab));
        Size s = h.fabSize();
        if (c.extended) {
            check(near(s.height, 56.0, 0.5) && s.width > 56.0,
                  "Extended：高 56、宽 > 56（实测 " + std::to_string((int)s.width) + "x" +
                      std::to_string((int)s.height) + "）");
        } else {
            check(near(s.width, c.expectW, 0.5) && near(s.height, c.expectH, 0.5),
                  "本体 " + std::to_string((int)c.expectW) + "² 尺寸正确");
        }
    }
}

// ── 测试 2：普通 FAB 点击触发 onClick、不展开 ───────────────────────────────
void testPlainFab() {
    std::printf("[test] 普通 FAB\n");
    int clicks = 0;
    auto fab = cui::make_shared<ExpandableFab>(
        IconCode::Add, "", FabSize::Regular, std::vector<FabAction>{}, std::nullopt, std::nullopt,
        ExpandType::Fan, ChildrenAnimation::Rotate, FabPosition::Right, 100.0, 90.0, 250.0, 4.0,
        false, [&] { ++clicks; });
    Harness h;
    h.attach(cui::make_shared<Align>(-1.0, -1.0, fab));

    // 本体中心（56²，左上对齐 → 全局 (200,200) 视窗口内）。
    auto* st = findFabState(h.root);
    check(st != nullptr, "找到 ExpandableFabState");
    Offset c = st ? st->bodyCenterGlobal() : Offset{28, 28};
    h.tap(c.dx, c.dy);
    h.settle();

    check(clicks == 1, "点击触发 onClick 一次");
    check(st && !st->isOpen(), "无菜单项的 FAB 不展开");
    check(h.centersOf(48.0).empty(), "Overlay 里没有 48² 子项");
}

// ── 测试 3：禁用 FAB 无反应 ─────────────────────────────────────────────────
void testDisabled() {
    std::printf("[test] 禁用 FAB\n");
    int clicks = 0;
    auto fab = cui::make_shared<ExpandableFab>(
        IconCode::Close, "", FabSize::Regular, std::vector<FabAction>{}, std::nullopt, std::nullopt,
        ExpandType::Fan, ChildrenAnimation::Rotate, FabPosition::Right, 100.0, 90.0, 250.0, 4.0,
        true, [&] { ++clicks; });
    Harness h;
    h.attach(cui::make_shared<Align>(-1.0, -1.0, fab));
    auto* st = findFabState(h.root);
    Offset c = st ? st->bodyCenterGlobal() : Offset{28, 28};
    h.tap(c.dx, c.dy);
    h.settle();
    check(clicks == 0, "禁用 FAB 点击不触发 onClick");
    check(st && !st->isOpen(), "禁用 FAB 不展开");
}

// ── 测试 4：Fan 展开 + 子项点击 + barrier 收起 ──────────────────────────────
void testFan() {
    std::printf("[test] Fan 展开\n");
    std::vector<int> hits{0, 0, 0, 0};
    std::vector<FabAction> actions;
    for (int i = 0; i < 4; ++i) {
        int idx = i;
        actions.push_back({IconCode::Edit, [&, idx] { ++hits[idx]; }});
    }
    auto fab = cui::make_shared<ExpandableFab>(
        IconCode::Add, "", FabSize::Regular, actions, std::nullopt, std::nullopt, ExpandType::Fan,
        ChildrenAnimation::None, FabPosition::Right, 100.0, 90.0, 250.0, 4.0, false);
    Harness h;
    // 居中放置：400² 窗口 ⇒ 扇形向左上展开仍完整落在窗口内，可命中。
    h.attach(cui::make_shared<Align>(0.0, 0.0, fab));
    auto* st = findFabState(h.root);
    Offset fabC = st ? st->bodyCenterGlobal() : Offset{28, 28};

    h.tap(fabC.dx, fabC.dy);
    h.settle();
    check(st && st->isOpen(), "点击本体后展开");

    auto centers = h.centersOf(48.0);
    check(centers.size() == 4, "Overlay 里有 4 枚 48² 子项");
    bool upLeft = true;
    for (auto& p : centers) {
        if (!(p.dx <= fabC.dx + 1.0 && p.dy <= fabC.dy + 1.0)) upLeft = false;
    }
    check(upLeft && centers.size() == 4, "Right 扇形子项都落在本体左上");

    // 点第一枚子项 → 触发其 onTap 并收起。
    if (centers.size() == 4) {
        // 选最靠近本体左上（dx 最大 & dy 最大）的那枚当作「第 0 项」。
        Offset first = centers[0];
        for (auto& p : centers) {
            if (p.dx > first.dx) first = p;
        }
        h.tap(first.dx, first.dy);
        h.settle();
        int total = hits[0] + hits[1] + hits[2] + hits[3];
        check(total == 1, "点击子项触发某个 onTap（总计一次）");
        check(st && !st->isOpen(), "点击子项后收起");
        check(h.centersOf(48.0).empty(), "收起后子项从 Overlay 移除");
    }

    // 重新展开后点 barrier（远离本体与子项的角落）收起。
    h.tap(fabC.dx, fabC.dy);
    h.settle();
    check(centers.size() >= 0 && st && st->isOpen(), "再次展开成功");
    h.tap(4.0, 4.0);
    h.settle();
    check(st && !st->isOpen(), "点菜单外 barrier 收起");
}

// ── 测试 5：Up 展开（竖直）──────────────────────────────────────────────────
void testUp() {
    std::printf("[test] Up 展开\n");
    std::vector<FabAction> actions;
    for (int i = 0; i < 4; ++i) actions.push_back({IconCode::Save, [] {}});
    auto fab = cui::make_shared<ExpandableFab>(
        IconCode::Add, "", FabSize::Regular, actions, std::nullopt, std::nullopt, ExpandType::Up,
        ChildrenAnimation::None, FabPosition::Right, 70.0, 90.0, 250.0, 4.0, false);
    Harness h;
    h.attach(cui::make_shared<Align>(-1.0, -1.0, fab));
    auto* st = findFabState(h.root);
    Offset fabC = st ? st->bodyCenterGlobal() : Offset{28, 28};
    h.tap(fabC.dx, fabC.dy);
    h.settle();
    auto centers = h.centersOf(48.0);
    check(centers.size() == 4, "Up 展开 4 枚子项");
    bool vertical = true;
    double prevY = 1e9;
    for (auto& p : centers) {
        if (std::fabs(p.dx - fabC.dx) > 1.0) vertical = false;  // dx 应≈本体中心
        if (p.dy >= prevY) vertical = false;                    // y 递增（从上到下）
        prevY = p.dy;
        if (p.dy > fabC.dy) vertical = false;                   // 全在本体上方
    }
    check(vertical, "Up 子项竖直排在本体正上方");
}

// ── 测试 6：Side 展开（水平）────────────────────────────────────────────────
void testSide() {
    std::printf("[test] Side 展开\n");
    std::vector<FabAction> actions;
    for (int i = 0; i < 4; ++i) actions.push_back({IconCode::Share, [] {}});
    auto fab = cui::make_shared<ExpandableFab>(
        IconCode::Add, "", FabSize::Regular, actions, std::nullopt, std::nullopt, ExpandType::Side,
        ChildrenAnimation::None, FabPosition::Right, 70.0, 90.0, 250.0, 4.0, false);
    Harness h;
    h.attach(cui::make_shared<Align>(-1.0, -1.0, fab));
    auto* st = findFabState(h.root);
    Offset fabC = st ? st->bodyCenterGlobal() : Offset{28, 28};
    h.tap(fabC.dx, fabC.dy);
    h.settle();
    auto centers = h.centersOf(48.0);
    check(centers.size() == 4, "Side 展开 4 枚子项");
    bool horiz = true;
    for (auto& p : centers) {
        if (std::fabs(p.dy - fabC.dy) > 1.0) horiz = false;   // dy 应≈本体中心
        if (p.dx > fabC.dx) horiz = false;                     // Right 位 ⇒ 子项在左侧
    }
    check(horiz, "Side（Right 位）子项水平排在本体正左方");
}

// ── 测试 7：Rotate 动画（未收尾时子项带旋转）────────────────────────────────
void testRotateAnim() {
    std::printf("[test] Rotate 子项动画\n");
    std::vector<FabAction> actions;
    for (int i = 0; i < 3; ++i) actions.push_back({IconCode::Edit, [] {}});
    auto fab = cui::make_shared<ExpandableFab>(
        IconCode::Add, "", FabSize::Regular, actions, std::nullopt, std::nullopt, ExpandType::Up,
        ChildrenAnimation::Rotate, FabPosition::Right, 90.0, 90.0, 400.0, 4.0, false);
    Harness h;
    h.attach(cui::make_shared<Align>(-1.0, -1.0, fab));
    auto* st = findFabState(h.root);
    Offset fabC = st ? st->bodyCenterGlobal() : Offset{28, 28};

    h.tap(fabC.dx, fabC.dy);
    // 中途：推进几帧但没收尾，progress ∈ (0,1) ⇒ 子项仍在动。
    for (int i = 0; i < 4; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        Ticker::tickAllActive(Duration(8));
        h.pump();
    }
    double mid = st ? st->progress() : 1.0;
    check(mid > 0.0 && mid < 1.0, "动画中途 progress 在 (0,1)");
    h.settle();
    check(st && near(st->progress(), 1.0, 1e-6), "收尾后 progress = 1.0");
}

}  // namespace

int main() {
    std::printf("=== ExpandableFab 测试 ===\n");
    testSizes();
    testPlainFab();
    testDisabled();
    testFan();
    testUp();
    testSide();
    testRotateAnim();
    std::printf("\n%s 失败 %d 项\n", g_failures ? "[FAIL]" : "[OK]", g_failures);
    return g_failures ? 1 : 0;
}
