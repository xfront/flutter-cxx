/// @file test_virtual_list.cpp
/// @brief 真正虚拟列表：O(可见行) 绘制、零逐行节点、惰性前缀和、滚动与命中。
///
/// 全部 headless：建树 + flushLayout + dispatchPointerEvent，不做光栅。

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/virtual_list.h"

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
        owner.flushBuildScope();
        po.flushLayout();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    void dispatch(const PointerEvent& ev) {
        po.dispatchPointerEvent(ev);
        pump();
    }

    int childCount() const {
        int n = 0;
        rootRO->visitChildren([&n](AbstractNode*) { ++n; });
        return n;
    }
};

SharedPtr<VirtualListState> fixedState(std::size_t count, double rowHeight) {
    return cui::make_shared<VirtualListState>(
        cui::make_shared<FixedListModel>(count, rowHeight, [](std::size_t i) {
            return "Row " + std::to_string(i + 1);
        }));
}

void testVisibleRangeAndNoPerRowNodes() {
    std::printf("[test] VirtualList visible range / zero nodes\n");
    auto state = fixedState(1'000'000, 26.0);
    Harness h;
    h.attach(cui::make_shared<VirtualList>(state), 300.0, 300.0);
    auto* ro = dynamic_cast<RenderVirtualList*>(h.rootRO);
    check(ro != nullptr, "根 RenderObject 是 RenderVirtualList");

    const auto range = ro->visibleRange();
    check(range.first == 0 && range.second > 0, "初始可见区间从 0 开始");
    check(range.second - range.first <= 13,
          "300px 视口只覆盖约 12 行，实际 " +
              std::to_string(range.second - range.first));
    check(h.childCount() == 0, "100 万行不应产生任何逐行 RenderObject");
    check(near(state->totalHeight(), 26'000'000.0), "固定行高总长走 O(1) 计算");
}

void testJumpToDeepRow() {
    std::printf("[test] VirtualList jumpTo deep row\n");
    auto state = fixedState(1'000'000, 26.0);
    auto widget = cui::make_shared<VirtualList>(state);
    Harness h;
    h.attach(widget, 300.0, 300.0);
    auto* ro = static_cast<RenderVirtualList*>(h.rootRO);

    widget->scrollToIndex(500'000);
    h.pump();
    check(near(ro->scrollOffset(), 500'000 * 26.0), "scrollToIndex 跳到第 50 万行");
    const auto range = ro->visibleRange();
    check(range.first >= 499'999 && range.first <= 500'000,
          "可见区间落在目标行附近，first=" + std::to_string(range.first));
    check(h.childCount() == 0, "跳到深处仍无逐行节点");
}

void testWheelScrolls() {
    std::printf("[test] VirtualList wheel scroll\n");
    auto state = fixedState(1'000'000, 26.0);
    Harness h;
    h.attach(cui::make_shared<VirtualList>(state), 300.0, 300.0);
    auto* ro = static_cast<RenderVirtualList*>(h.rootRO);

    PointerScrollEvent ev;
    ev.pointerId = 1;
    ev.position = ev.globalPosition = {120.0, 150.0};
    ev.scrollDeltaY = 260.0;
    h.dispatch(ev);
    check(near(ro->scrollOffset(), 260.0), "滚轮增量按像素推进偏移");
    check(ro->visibleRange().first >= 10, "滚动后可见首行随之更新");
}

void testClickSelectsRow() {
    std::printf("[test] VirtualList click selects row\n");
    auto state = fixedState(1000, 26.0);
    Harness h;
    h.attach(cui::make_shared<VirtualList>(state), 300.0, 300.0);

    PointerDownEvent down;
    down.pointerId = 2;
    down.position = down.globalPosition = {50.0, 2.0 * 26.0 + 5.0};
    down.buttons = kPrimaryMouseButton;
    h.dispatch(down);
    check(state->selected() == 2, "点击第 3 行应选中 index=2");
}

void testVariableHeightsPrefixSums() {
    std::printf("[test] VirtualList variable heights / prefix sums\n");
    auto state = cui::make_shared<VirtualListState>(
        cui::make_shared<VariableListModel>(
            1000,
            [](std::size_t i) { return i % 2 == 0 ? 20.0 : 40.0; },
            [](std::size_t i) { return "V" + std::to_string(i); }));
    check(near(state->offsetAt(0), 0.0) && near(state->offsetAt(1), 20.0) &&
              near(state->offsetAt(2), 60.0),
          "前缀和 offsets = 0,20,60,...");
    check(near(state->totalHeight(), 30'000.0), "变高总长 = 500 * 60");
    check(state->indexAtY(59.0) == 1 && state->indexAtY(60.0) == 2,
          "二分定位在边界上取后半开区间");
}

void testScrollbarDrag() {
    std::printf("[test] VirtualList scrollbar drag\n");
    auto state = fixedState(1'000'000, 26.0);
    Harness h;
    h.attach(cui::make_shared<VirtualList>(state), 300.0, 300.0);
    auto* ro = static_cast<RenderVirtualList*>(h.rootRO);

    PointerDownEvent down;
    down.pointerId = 3;
    down.position = down.globalPosition = {296.0, 5.0};
    down.buttons = kPrimaryMouseButton;
    h.dispatch(down);
    PointerMoveEvent move;
    move.pointerId = 3;
    move.position = move.globalPosition = {296.0, 55.0};
    move.buttons = kPrimaryMouseButton;
    h.dispatch(move);
    check(ro->scrollOffset() > 0.0,
          "拖动 thumb 应改变偏移，实际 " + std::to_string(ro->scrollOffset()));
}

}  // namespace

int main() {
    testVisibleRangeAndNoPerRowNodes();
    testJumpToDeepRow();
    testWheelScrolls();
    testClickSelectsRow();
    testVariableHeightsPrefixSums();
    testScrollbarDrag();
    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILED\n", g_failures);
    return 1;
}
