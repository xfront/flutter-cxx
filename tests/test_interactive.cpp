/// @file test_interactive.cpp
/// @brief 交互式 Material 控件单元测试（GestureDetector / Checkbox / Switch /
///        Radio / Slider / Button）。
///
/// 覆盖：
///   1. GestureDetector：tap（down+up 触发 onTap）、禁用（无 onTap 不触发）、
///      pan（down+move+up 触发 onPanUpdate，且不触发 onTap）。
///   2. Checkbox：点击翻转 value 回写 onChanged；禁用时不响应；受控 value
///      正确驱动 RenderCheckbox。
///   3. Switch / Radio：点击回写 onChanged；value 正确驱动 RenderObject。
///   4. Slider：指针 down/move 直接改写 value 并回调 onChanged；thumb 位置随
///      value 变化。
///   5. Button：点击触发 onPressed；禁用（onPressed=null）不响应。
///
/// 全部 headless，通过 PipelineOwner::dispatchPointerEvent 驱动指针事件，
/// 并复用 framework 的 Element/Widget 树接入方式。

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_interactive.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/interactive.h"
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

bool near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

/// 测试用 Harness：接入 Widget 树并暴露 dispatch 驱动。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;
    RenderObject* rootRO = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rootRO = root->findRenderObject();
        rv.setChild(static_cast<RenderBox*>(rootRO));
        po.setRootNode(&rv);
        po.flushLayout();
    }

    void dispatch(const PointerEvent& ev) { po.dispatchPointerEvent(ev); }

    template <typename T>
    T* findRenderObject() {
        // 简单 DFS：把根 render object 当作 RenderBox 子树遍历找类型 T。
        struct Visitor {
            T* found = nullptr;
            void visit(RenderObject* ro) {
                if (!ro || found)
                    return;
                if (dynamic_cast<T*>(ro))
                    found = static_cast<T*>(ro);
                ro->visitChildren([this](AbstractNode* n) {
                    visit(static_cast<RenderObject*>(n));
                });
            }
        } v;
        v.visit(rootRO);
        return v.found;
    }
};

PointerDownEvent downAt(double x, double y) {
    PointerDownEvent e;
    e.position = {x, y};
    e.buttons = kPrimaryMouseButton;
    return e;
}
PointerMoveEvent moveAt(double x, double y) {
    PointerMoveEvent e;
    e.position = {x, y};
    e.buttons = kPrimaryMouseButton;
    return e;
}
PointerUpEvent upAt(double x, double y) {
    PointerUpEvent e;
    e.position = {x, y};
    return e;
}

// ── 测试 1：GestureDetector tap / 禁用 / pan ────────────────────────────────
void testGestureDetector() {
    std::printf("[test] GestureDetector tap / pan\n");

    {
        int tap = 0;
        auto gd = cui::make_shared<GestureDetector>(
            cui::make_shared<SizedBox>(80, 40), [&]() { ++tap; });
        Harness h;
        h.attach(gd, 200, 100);
        h.dispatch(downAt(40, 20));
        h.dispatch(upAt(40, 20));
        check(tap == 1, "down+up 触发一次 onTap");
    }
    {
        int tap = 0;
        auto gd = cui::make_shared<GestureDetector>(
            cui::make_shared<SizedBox>(80, 40), [&]() { ++tap; });
        Harness h;
        h.attach(gd, 200, 100);
        h.dispatch(downAt(40, 20));
        // 只 down 不 up：不应触发 tap。
        check(tap == 0, "仅 down 不触发 onTap");
    }
    {
        int pans = 0;
        double lastX = -1;
        Offset lastDelta{};
        auto gd = cui::make_shared<GestureDetector>(
            cui::make_shared<SizedBox>(200, 20), nullptr, nullptr, nullptr, nullptr,
            [&](const PointerEvent& e) {
                ++pans;
                lastDelta = e.delta;
            },
            [&](const PointerEvent& e) { lastX = e.position.dx; });
        Harness h;
        h.attach(gd, 200, 100);
        h.dispatch(downAt(10, 10));
        h.dispatch(moveAt(50, 10));
        check(near(lastDelta.dx, 40.0) && near(lastDelta.dy, 0.0),
              "move 的 delta = 相对上一事件的位移（10→50 ⇒ 40,0）");
        h.dispatch(moveAt(120, 30));
        check(near(lastDelta.dx, 70.0) && near(lastDelta.dy, 20.0),
              "连续 move 的 delta 逐段累加（50,10→120,30 ⇒ 70,20）");
        h.dispatch(upAt(120, 30));
        check(pans == 2, "down+两次 move 触发两次 onPanUpdate");
        check(near(lastX, 120.0), "最后一次 onPanUpdate 位置 x=120");
    }
    {
        // 无按键移动（hover）同样带 delta：首个 hover 没有上一位置 ⇒ 0，
        // 之后按两点差算（上游 PointerHoverEvent 的 delta 同款语义）。
        std::vector<Offset> hovers;
        auto listener = cui::make_shared<Listener>(
            nullptr, nullptr, nullptr,
            [&](const PointerEvent& e) { hovers.push_back(e.delta); }, nullptr,
            cui::make_shared<SizedBox>(200, 60));
        Harness h;
        h.attach(listener, 200, 100);
        PointerHoverEvent h1;
        h1.position = {10, 20};
        h.dispatch(h1);
        PointerHoverEvent h2;
        h2.position = {30, 20};
        h.dispatch(h2);
        check(hovers.size() == 2, "hover 事件到达 Listener");
        check(hovers.size() == 2 && near(hovers[0].dx, 0.0) && near(hovers[0].dy, 0.0),
              "首个 hover 无上一位置 ⇒ delta=0");
        check(hovers.size() == 2 && near(hovers[1].dx, 20.0) && near(hovers[1].dy, 0.0),
              "第二个 hover 的 delta = 20,0");
    }
}

// ── 测试 2：Checkbox ────────────────────────────────────────────────────────
void testCheckbox() {
    std::printf("[test] Checkbox\n");

    int changed = 0;
    bool last = false;
    auto cb = cui::make_shared<Checkbox>(
        false, [&](bool v) {
            ++changed;
            last = v;
        });
    Harness h;
    h.attach(cb, 200, 100);
    auto* rc = h.findRenderObject<RenderCheckbox>();
    check(rc != nullptr, "生成 RenderCheckbox");
    check(rc && !rc->value(), "初始 value=false → RenderCheckbox 未勾选");

    h.dispatch(downAt(9, 9));   // 点击复选框中心（18×18 在左上角）
    h.dispatch(upAt(9, 9));
    check(changed == 1 && last == true, "点击未勾选框 → onChanged(true)");

    // 禁用：onChanged 为 null 时不响应。
    auto cb2 = cui::make_shared<Checkbox>(false, nullptr);
    Harness h2;
    h2.attach(cb2, 200, 100);
    h2.dispatch(downAt(9, 9));
    h2.dispatch(upAt(9, 9));
    auto* rc2 = h2.findRenderObject<RenderCheckbox>();
    check(rc2 && !rc2->value(), "禁用 Checkbox 点击后仍为未勾选");
}

// ── 测试 3：Switch / Radio ──────────────────────────────────────────────────
void testSwitchAndRadio() {
    std::printf("[test] Switch / Radio\n");

    int swCount = 0;
    bool swv = false;
    auto sw = cui::make_shared<Switch>(false, [&](bool v) {
        ++swCount;
        swv = v;
    });
    Harness h;
    h.attach(sw, 200, 100);
    auto* rs = h.findRenderObject<RenderSwitch>();
    check(rs && !rs->value(), "Switch 初始关");
    h.dispatch(downAt(20, 12));
    h.dispatch(upAt(20, 12));
    check(swCount == 1 && swv == true, "点击 Switch → onChanged(true)");

    int rd = 0;
    auto radio = cui::make_shared<Radio>(false, [&](bool) { ++rd; });
    Harness h2;
    h2.attach(radio, 200, 100);
    auto* rr = h2.findRenderObject<RenderRadio>();
    check(rr && !rr->selected(), "Radio 初始未选中");
    h2.dispatch(downAt(9, 9));
    h2.dispatch(upAt(9, 9));
    check(rd == 1, "点击未选 Radio → onChanged(true)");
}

// ── 测试 4：Slider ──────────────────────────────────────────────────────────
void testSlider() {
    std::printf("[test] Slider\n");

    int changed = 0;
    double last = -1;
    auto sl = cui::make_shared<Slider>(0.0, [&](double v) {
        ++changed;
        last = v;
    });
    Harness h;
    h.attach(sl, 200, 100);
    auto* rs = h.findRenderObject<RenderSlider>();
    check(rs != nullptr, "生成 RenderSlider");
    check(rs && near(rs->value(), 0.0), "初始 value=0");

    // 在宽度 200、thumbR=10 的轨道上：down 于 x=100（中点）→ value≈0.5。
    h.dispatch(downAt(100, 10));
    check(changed >= 1, "down 即回调 onChanged");
    check(near(last, 0.5, 1e-3), "中点 down → value≈0.5");
    check(rs && near(rs->thumbCenterX(), 100.0, 1e-3), "thumb 中心 x≈100");

    // move 到最右 → value≈1。
    h.dispatch(moveAt(190, 10));
    h.dispatch(upAt(190, 10));
    check(near(last, 1.0, 1e-3), "右端 move → value≈1");

    // 受控 value 正确驱动渲染（attach 一个 value=0.25 的 Slider）。
    auto sl2 = cui::make_shared<Slider>(0.25, nullptr);
    Harness h2;
    h2.attach(sl2, 200, 100);
    auto* rs2 = h2.findRenderObject<RenderSlider>();
    check(rs2 && near(rs2->value(), 0.25), "受控 value=0.25 → RenderSlider.value=0.25");
}

// ── 测试 5：Button ──────────────────────────────────────────────────────────
void testButtons() {
    std::printf("[test] Button (Elevated / Text / Outlined)\n");

    auto make = [](auto btn) {
        auto h = std::make_unique<Harness>();
        h->attach(btn, 200, 100);
        return h;
    };

    int pressed = 0;
    auto eb = cui::make_shared<ElevatedButton>(
        cui::make_shared<Text>("OK"), [&]() { ++pressed; });
    auto heb = make(eb);
    heb->dispatch(downAt(40, 20));
    heb->dispatch(upAt(40, 20));
    check(pressed == 1, "ElevatedButton 点击触发 onPressed");

    // 禁用按钮。
    auto db = cui::make_shared<ElevatedButton>(cui::make_shared<Text>("X"), nullptr);
    auto hdb = make(db);
    hdb->dispatch(downAt(40, 20));
    hdb->dispatch(upAt(40, 20));
    check(pressed == 1, "禁用 ElevatedButton 不触发 onPressed（计数不变）");

    int tp = 0;
    auto tb = cui::make_shared<TextButton>(cui::make_shared<Text>("T"),
                                           [&]() { ++tp; });
    auto htb = make(tb);
    htb->dispatch(downAt(40, 20));
    htb->dispatch(upAt(40, 20));
    check(tp == 1, "TextButton 点击触发 onPressed");

    int op = 0;
    auto ob = cui::make_shared<OutlinedButton>(cui::make_shared<Text>("O"),
                                               [&]() { ++op; });
    auto hob = make(ob);
    hob->dispatch(downAt(40, 20));
    hob->dispatch(upAt(40, 20));
    check(op == 1, "OutlinedButton 点击触发 onPressed");
}

}  // namespace

int main() {
    std::printf("=== test_interactive ===\n");
    testGestureDetector();
    testCheckbox();
    testSwitchAndRadio();
    testSlider();
    testButtons();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
