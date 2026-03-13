/// @file test_input.cpp
/// @brief 控件层输入 / 焦点基础设施单元测试。
///
/// 覆盖：
///   1. 指针事件命中正确节点（Row 中两个 Listener 互不干扰）
///   2. 冒泡与 stopPropagation（嵌套 Listener）
///   3. Hover / Scroll 事件下发
///   4. 焦点模型：requestFocus 切换 + onFocusChange 回调
///   5. 点击聚焦：Listener(onDown) → FocusNode.requestFocus()
///
/// 全部 headless（无窗口），通过 PipelineOwner::dispatchPointerEvent 驱动。

#include <cstdio>
#include <memory>
#include <string>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/framework/focus.h"
#include "cui/framework/listener.h"

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

/// 把 Widget 树接入 BuildOwner + RenderView + PipelineOwner，并布局好，
/// 以便 dispatchPointerEvent 能命中正确节点。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;  // 必须最后声明以外，保证其成员 FocusManager 在销毁时仍存活
    Element* root = nullptr;
    RenderObject* rootRO = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rootRO = static_cast<RenderObjectElement*>(root)->renderObject();
        rv.setChild(static_cast<RenderBox*>(rootRO));
        po.setRootNode(&rv);
        po.flushLayout();
    }

    void dispatch(const PointerEvent& ev) { po.dispatchPointerEvent(ev); }
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
    return e;
}
PointerHoverEvent hoverAt(double x, double y) {
    PointerHoverEvent e;
    e.position = {x, y};
    return e;
}
PointerScrollEvent scrollAt(double x, double y, double dy) {
    PointerScrollEvent e;
    e.position = {x, y};
    e.scrollDeltaY = dy;
    return e;
}

// ── 测试 1：Row 中两个并列 Listener，点击各自区域 ─────────────────────────
void testHitCorrectListener() {
    std::printf("[test] 指针事件命中正确节点\n");

    int aDown = 0, bDown = 0;
    auto a = cui::make_shared<Listener>(
            [&](const PointerEvent&) { ++aDown; }, nullptr, nullptr, nullptr, nullptr,
            cui::make_shared<SizedBox>(80, 100));
    auto b = cui::make_shared<Listener>(
            [&](const PointerEvent&) { ++bDown; }, nullptr, nullptr, nullptr, nullptr,
            cui::make_shared<SizedBox>(80, 100));
    auto row = cui::make_shared<Row>(
            std::vector<SharedPtr<Widget>>{a, b});

    Harness h;
    h.attach(row, 200, 100);

    h.dispatch(downAt(40, 50));   // 落在 A 区域 [0,80)
    check(aDown == 1 && bDown == 0, "点击 A 区域只触发 A 的 onDown");

    h.dispatch(downAt(120, 50));  // 落在 B 区域 [80,160)
    check(aDown == 1 && bDown == 1, "点击 B 区域只触发 B 的 onDown");
}

// ── 测试 2：嵌套 Listener 的冒泡与 stopPropagation ────────────────────────
void testBubblingAndStop() {
    std::printf("[test] 冒泡与 stopPropagation\n");

    // 子 Listener 不调用 stopPropagation：事件应继续冒泡到父 Listener。
    {
        int childDown = 0, parentDown = 0;
        auto childNoStop = cui::make_shared<Listener>(
                [&](const PointerEvent&) { ++childDown; }, nullptr, nullptr, nullptr, nullptr,
                cui::make_shared<SizedBox>(80, 100));
        auto parentNoStop = cui::make_shared<Listener>(
                [&](const PointerEvent&) { ++parentDown; }, nullptr, nullptr, nullptr, nullptr,
                childNoStop);
        Harness h;
        h.attach(parentNoStop, 200, 100);
        h.dispatch(downAt(40, 50));
        check(childDown == 1 && parentDown == 1, "子不拦截时事件冒泡到父");
    }

    // 子 Listener 调用 stopPropagation：父 Listener 不应收到事件。
    {
        int childDown = 0, parentDown = 0;
        auto childStop = cui::make_shared<Listener>(
                [&](const PointerEvent& e) {
                    ++childDown;
                    e.stopPropagation();
                },
                nullptr, nullptr, nullptr, nullptr,
                cui::make_shared<SizedBox>(80, 100));
        auto parentStop = cui::make_shared<Listener>(
                [&](const PointerEvent&) { ++parentDown; }, nullptr, nullptr, nullptr, nullptr,
                childStop);
        Harness h;
        h.attach(parentStop, 200, 100);
        h.dispatch(downAt(40, 50));
        check(childDown == 1 && parentDown == 0,
              "子 stopPropagation 后仅子收到、父被拦截");
    }
}

// ── 测试 3：Hover / Scroll 事件下发 ───────────────────────────────────────
void testHoverAndScroll() {
    std::printf("[test] Hover / Scroll 事件下发\n");

    int hover = 0, scroll = 0;
    auto box = cui::make_shared<Listener>(
            nullptr, nullptr, nullptr,
            [&](const PointerEvent&) { ++hover; },
            [&](const PointerEvent&) { ++scroll; },
            cui::make_shared<SizedBox>(80, 100));
    Harness h;
    h.attach(box, 200, 100);

    h.dispatch(hoverAt(40, 50));
    check(hover == 1, "Hover 事件触发 onPointerHover");

    h.dispatch(scrollAt(40, 50, -30));
    check(scroll == 1, "Scroll 事件触发 onPointerScroll");
}

// ── 测试 4：焦点模型——requestFocus 切换 + 回调 ─────────────────────────────
void testFocusManager() {
    std::printf("[test] 焦点模型 requestFocus 切换\n");

    int aChanges = 0, bChanges = 0;
    auto a = cui::make_shared<Focus>(cui::make_shared<ColoredBox>(Color::fromRGB(1, 0, 0)),
                                     [&](bool f) { if (f) ++aChanges; });
    auto b = cui::make_shared<Focus>(cui::make_shared<ColoredBox>(Color::fromRGB(0, 1, 0)),
                                     [&](bool f) { if (f) ++bChanges; });
    auto column = cui::make_shared<Column>(
            std::vector<SharedPtr<Widget>>{a, b});

    BuildOwner owner;
    auto e = column->createElement(column);
    owner.adoptRootElement(std::move(e));

    auto* na = a->focusNode().get();
    auto* nb = b->focusNode().get();
    check(owner.focusManager().primaryFocus() == nullptr, "初始无焦点");

    na->requestFocus();
    check(na->hasFocus() && aChanges == 1, "requestFocus(A) 后 A 获焦，回调 +1");
    check(owner.focusManager().primaryFocus() == na, "primaryFocus 指向 A");

    nb->requestFocus();
    check(!na->hasFocus() && nb->hasFocus(), "requestFocus(B) 后 B 获焦、A 失焦");
    check(bChanges == 1, "B 的获焦回调 +1");

    nb->unfocus();
    check(!nb->hasFocus() && owner.focusManager().primaryFocus() == nullptr,
          "unfocus(B) 后无焦点");
}

// ── 测试 5：点击聚焦——Listener(onDown) 触发 FocusNode.requestFocus ────────
void testClickToFocus() {
    std::printf("[test] 点击聚焦（Listener → FocusNode）\n");

    auto a = cui::make_shared<Focus>(cui::make_shared<SizedBox>(80, 100));
    auto b = cui::make_shared<Focus>(cui::make_shared<SizedBox>(80, 100));
    auto row = cui::make_shared<Row>(
            std::vector<SharedPtr<Widget>>{a, b});

    Harness h;
    h.attach(row, 200, 100);

    auto* na = a->focusNode().get();
    auto* nb = b->focusNode().get();

    h.dispatch(downAt(40, 50));  // 点击 A 区域
    check(na->hasFocus() && !nb->hasFocus(), "点击 A 区域后 A 获焦");

    h.dispatch(downAt(120, 50)); // 点击 B 区域
    check(!na->hasFocus() && nb->hasFocus(), "点击 B 区域后 B 获焦、A 失焦");
}

}  // namespace

int main() {
    std::printf("=== test_input ===\n");
    testHitCorrectListener();
    testBubblingAndStop();
    testHoverAndScroll();
    testFocusManager();
    testClickToFocus();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
