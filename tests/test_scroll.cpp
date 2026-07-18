/// @file test_scroll.cpp
/// @brief 滚动系列单元测试（ScrollController / SingleChildViewport / Scrollable /
///        SingleChildScrollView / ListView / GridView / Scrollbar）。
///
/// 覆盖：
///   1. ScrollController：jumpTo clamp、监听器通知、applyExtents 回写与静默 clamp。
///   2. SingleChildViewport 测量：maxScrollExtent = 内容 - 视口；交叉轴撑满。
///   2c. 嵌套视口：横滚嵌在纵滚的可滚主轴里，交叉轴无界时按内容收缩
///       （否则视口自称 1e18 高，外层滚到底只剩空白）。
///   2b. SingleChildScrollView 反向轴（reverse ⇒ AxisDirection::Up 的绘制偏移公式）
///       与 padding（视口内部包 Padding，内边距随内容滚动）。
///   3. SingleChildScrollView：pan 拖拽跟手（offset -= delta）、边界 clamp。
///   4. ListView：内容纵向排列、交叉轴 stretch、滚动。
///   5. GridView.count：单元格尺寸与行排列。
///   6. Scrollbar：thumb 几何（长度比例、位置随 offset）。
///   6b. Scrollbar 交互：拖 thumb 与滚动方向一致、轨道点击翻页、轨道外仍拖内容。
///   7. 命中测试：滚动后的子控件仍可点（坐标回补）。
///   8. 内容缩短（侧栏底部 folder 收起）⇒ 视口绘制偏移在重排时夹回 [0, max]，
///      底部不留「剩余空间」；反向轴同口径。
///   9. 两层滚动条的滚轮归属：命中链里最深的「真能吸收增量」的 Scrollable 独占
///      本次滚轮；里层到底/顶（或 NeverScrollable / 内容不超高）才让给外层。
///
/// 全部 headless，通过 PipelineOwner::dispatchPointerEvent 驱动指针事件。

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_scroll.h"
#include "cui/rendering/render_view.h"
#include "cui/rendering/sliver.h"       // SliverPhysicalParentData
#include "cui/rendering/sliver_grid.h"  // RenderSliverGrid（sliver 流水线探针）
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/scroll.h"

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

/// 测试用 Harness：接入 Widget 树并暴露 dispatch + 帧泵。
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

    /// 手势触发 setState 后需要先冲刷 build scope 再布局。
    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    void dispatch(const PointerEvent& ev) {
        po.dispatchPointerEvent(ev);
        pump();
    }

    template <typename T>
    T* findRenderObject() {
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

// ── 测试 1：ScrollController ────────────────────────────────────────────────
void testScrollController() {
    std::printf("[test] ScrollController\n");

    ScrollController c;
    int notified = 0;
    c.addListener([&] { ++notified; });

    // 未测量前 maxExtent=0 → clamp 到 0（无变化不通知）。
    c.jumpTo(100.0);
    check(near(c.offset(), 0.0) && notified == 0, "maxExtent=0 时 jumpTo(100) clamp 到 0");

    // 测量回写：maxExtent=300，静默（不通知）。
    c.applyExtents(300.0, 200.0);
    check(near(c.maxScrollExtent(), 300.0) && notified == 0,
          "applyExtents 回写 maxExtent 且不通知");

    c.jumpTo(50.0);
    check(near(c.offset(), 50.0) && notified == 1, "jumpTo(50) → offset=50 且通知");

    c.jumpTo(1000.0);
    check(near(c.offset(), 300.0) && notified == 2, "jumpTo(1000) clamp 到 300 并通知");
}

// ── 测试 2：SingleChildViewport 测量 ───────────────────────────────────────────────────
void testViewportMeasure() {
    std::printf("[test] SingleChildViewport 测量\n");

    ScrollController c;
    auto h = std::make_unique<Harness>();
    // 内容 200×800，视口 200×300 → maxExtent=500。
    h->attach(cui::make_shared<SingleChildScrollView>(
                  &c, cui::make_shared<SizedBox>(200.0, 800.0)),
              200, 300);
    check(near(c.maxScrollExtent(), 500.0), "maxScrollExtent = 800 - 300 = 500");
    check(near(c.viewportExtent(), 300.0), "viewportExtent = 300");

    auto* vp = h->findRenderObject<RenderSingleChildViewport>();
    check(vp != nullptr, "生成 RenderSingleChildViewport");
    check(vp && near(vp->size().width, 200.0) && near(vp->size().height, 300.0),
          "SingleChildViewport 尺寸 = 视口尺寸");

    // 子节点交叉轴撑满：SizedBox(200,800) 在 200 宽紧约束下仍为 200×800。
    check(vp && vp->child() && near(vp->child()->size().height, 800.0),
          "内容自然高度 800");
}

// ── 测试 2b：SingleChildScrollView 的反向轴 + padding ───────────────────────
/// 对齐上游 `_RenderSingleChildViewport._paintOffsetForPosition`：
///   Down → `-offset`；Up → `offset - 内容长 + 视口长`。
/// 于是 Up 的首帧贴的是内容**尾边**，且 offset 变大时内容往下走。
void testSingleChildScrollViewReverseAndPadding() {
    std::printf("[test] SingleChildScrollView 反向轴 / padding\n");

    // 内容三段：300 空白 + 100 可点标记 + 400 空白 = 800；视口 200×300 ⇒ maxExtent 500。
    int taps = 0;
    auto makeContent = [&taps] {
        return cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            cui::make_shared<SizedBox>(200.0, 300.0),
            cui::make_shared<GestureDetector>(
                cui::make_shared<DecoratedBox>(
                    BoxDecoration{Color::fromRGBA(0.8, 0.2, 0.2, 1.0)},
                    cui::make_shared<SizedBox>(200.0, 100.0)),
                [&taps] { ++taps; }),
            cui::make_shared<SizedBox>(200.0, 400.0)});
    };

    // 正向对照：offset=50 时标记落在 viewport y=250..300。
    {
        ScrollController c;
        auto h = std::make_unique<Harness>();
        h->attach(cui::make_shared<SingleChildScrollView>(&c, makeContent()), 200, 300);
        check(near(c.maxScrollExtent(), 500.0), "Down：maxExtent = 800 - 300");
        c.jumpTo(50.0);
        h->pump();
        h->dispatch(downAt(100, 100));
        h->dispatch(upAt(100, 100));
        check(taps == 0, "Down：viewport y=100 → 内容 y=150，落在空白段");
        h->dispatch(downAt(100, 270));
        h->dispatch(upAt(100, 270));
        check(taps == 1, "Down：viewport y=270 → 内容 y=320，命中标记");
    }

    taps = 0;
    {
        ScrollController c;
        auto h = std::make_unique<Harness>();
        h->attach(cui::make_shared<SingleChildScrollView>(
                      &c, makeContent(), Axis::Vertical, /*keyboardScroll=*/true,
                      /*reverse=*/true),
                  200, 300);
        check(near(c.maxScrollExtent(), 500.0), "Up：maxExtent 同样是 500");
        // 绘制偏移 = offset - 800 + 300 = offset - 500 ⇒ offset 0 时可见内容 500..800。
        h->dispatch(downAt(100, 100));
        h->dispatch(upAt(100, 100));
        check(taps == 0, "Up：首帧贴内容尾边，viewport y=100 → 内容 y=600（空白段）");
        c.jumpTo(250.0);
        h->pump();
        h->dispatch(downAt(100, 100));
        h->dispatch(upAt(100, 100));
        check(taps == 1, "Up：offset=250 → 绘制偏移 -250，viewport y=100 命中内容 y=350");
        c.jumpTo(50.0);
        h->pump();
        h->dispatch(downAt(100, 100));  // viewport y=100 → 内容 y=550
        h->dispatch(upAt(100, 100));
        h->dispatch(downAt(100, 270));  // viewport y=270 → 内容 y=720
        h->dispatch(upAt(100, 270));
        check(taps == 1, "Up：offset=50 时标记已在视口上方，两处点击都不命中");
    }

    // padding：上游在视口**内部**包 Padding ⇒ 内边距随内容一起滚。
    {
        ScrollController c;
        auto h = std::make_unique<Harness>();
        h->attach(cui::make_shared<SingleChildScrollView>(&c,
                                                          cui::make_shared<SizedBox>(200.0, 800.0),
                                                          Axis::Vertical, true, false,
                                                          EdgeInsets::all(20.0)),
                  200, 300);
        check(near(c.maxScrollExtent(), 840.0 - 300.0),
              "padding=all(20)：内容长 800+40 ⇒ maxExtent 540");
        auto* vp = h->findRenderObject<RenderSingleChildViewport>();
        check(vp && vp->child() && near(vp->child()->size().height, 840.0),
              "视口子节点（Padding）高 840");
    }
}

// ── 测试 3：SingleChildScrollView pan 拖拽 ──────────────────────────────────
void testSingleChildScroll() {
    std::printf("[test] SingleChildScrollView pan\n");

    ScrollController c;
    auto h = std::make_unique<Harness>();
    h->attach(cui::make_shared<SingleChildScrollView>(
                  &c, cui::make_shared<SizedBox>(200.0, 800.0)),
              200, 300);

    // 向上拖（内容上移）：dy = -100 → offset += 100。
    h->dispatch(downAt(100, 200));
    h->dispatch(moveAt(100, 100));  // delta = -100 → offset = 100
    h->dispatch(upAt(100, 100));
    check(near(c.offset(), 100.0), "上拖 100px → offset=100");

    // 再上拖 400 → offset=500（=maxExtent）。
    h->dispatch(downAt(100, 290));
    h->dispatch(moveAt(100, 190));  // delta = -100 → 200
    h->dispatch(moveAt(100, 90));   // delta = -100 → 300
    h->dispatch(moveAt(100, 10));   // delta = -80 → 380
    h->dispatch(upAt(100, 10));
    check(near(c.offset(), 380.0), "累计上拖 380 → offset=380");

    h->dispatch(downAt(100, 290));
    h->dispatch(moveAt(100, 190));  // -100 → 480
    h->dispatch(moveAt(100, 90));   // -100 → 580 → clamp 500
    h->dispatch(upAt(100, 90));
    check(near(c.offset(), 500.0), "继续上拖 clamp 到 maxExtent=500");

    // 向下拖两段回顶部：-280 - 280 = -560 → clamp 0。
    h->dispatch(downAt(100, 10));
    h->dispatch(moveAt(100, 290));  // +280 → 220
    h->dispatch(upAt(100, 290));
    h->dispatch(downAt(100, 10));
    h->dispatch(moveAt(100, 290));  // +280 → -60 → clamp 0
    h->dispatch(upAt(100, 290));
    check(near(c.offset(), 0.0), "下拖过头 clamp 到 0");
}

// ── 测试 4：滚轮（Scroll 事件）按像素步进 ────────────────────────────────────
void testWheelScroll() {
    std::printf("[test] 滚轮滚动\n");

    ScrollController c;
    auto h = std::make_unique<Harness>();
    h->attach(cui::make_shared<SingleChildScrollView>(
                  &c, cui::make_shared<SizedBox>(200.0, 800.0)),
              200, 300);

    auto scrollAt = [&](double dy) {
        PointerScrollEvent e;
        e.position = {100, 150};
        e.scrollDeltaY = dy;  // 像素增量（Application 已把「格」换算成像素）
        h->dispatch(e);
    };

    // 一格滚轮 = 56px（Application::setWheelPixelsPerNotch 的默认值）。
    scrollAt(56.0);
    check(near(c.offset(), 56.0), "滚轮 +56px → offset=56");
    scrollAt(56.0);
    check(near(c.offset(), 112.0), "再滚一格 +56px → offset=112");
    scrollAt(-112.0);
    check(near(c.offset(), 0.0), "反向滚 -112px → offset=0");
    scrollAt(1000.0);
    check(near(c.offset(), 500.0), "一次滚到底 → clamp 到 maxExtent=500");
}

// ── 测试 4b：嵌套视口 —— 横滚嵌在纵滚的可滚主轴里 ──────────────────────────
/// CodeEditor 的形状：纵向视口 → 横向视口 → 内容。横滚的交叉轴（高）从纵滚
/// 拿到的是无界约束；过去这里用 biggest()（= kInfinity 哨兵）当尺寸，横滚视图
/// 自称 1e18 高，纵滚 maxScrollExtent 随之爆掉（滚到底只剩空白）。
void testNestedAxisViewport() {
    std::printf("[test] 嵌套视口：横滚嵌在纵滚里\n");

    ScrollController inner;
    ScrollController outer;
    auto innerScroll = cui::make_shared<SingleChildScrollView>(
        &inner, cui::make_shared<SizedBox>(1200.0, 480.0), Axis::Horizontal, false);
    auto h = std::make_unique<Harness>();
    h->attach(cui::make_shared<SingleChildScrollView>(&outer, innerScroll, Axis::Vertical,
                                                      /*keyboardScroll=*/false),
              400, 200);

    check(near(inner.viewportExtent(), 400.0), "横滚视口宽 = 400（纵滚交叉轴紧锁）");
    check(near(inner.maxScrollExtent(), 800.0), "横滚范围 = 1200 - 400");
    check(near(outer.viewportExtent(), 200.0), "纵滚视口高 = 200");
    check(near(outer.maxScrollExtent(), 280.0), "纵滚范围按内容高算 = 480 - 200");

    outer.jumpTo(280.0);
    h->pump();
    check(near(outer.offset(), 280.0), "纵滚到底 = 内容末行贴住视口底边");
}

// ── 测试 5：ListView ────────────────────────────────────────────────────────
void testListView() {
    std::printf("[test] ListView\n");

    ScrollController c;
    std::vector<SharedPtr<Widget>> items;
    for (int i = 0; i < 4; ++i)
        items.push_back(cui::make_shared<SizedBox>(200.0, 100.0));

    auto h = std::make_unique<Harness>();
    h->attach(cui::make_shared<ListView>(&c, items), 200, 300);

    check(near(c.viewportExtent(), 300.0), "viewportExtent = 300");
    check(near(c.maxScrollExtent(), 100.0), "maxScrollExtent = 4*100 - 300 = 100");

    // ListView 已迁到 sliver 流水线：children → SliverList(SliverChildListDelegate)，
    // 不再有 Column（RenderFlex）；交叉轴由 asBoxConstraints 锁到视口宽（stretch）。
    auto* list = h->findRenderObject<RenderSliverList>();
    check(list != nullptr, "生成 RenderSliverList（sliver 流水线）");
    if (list) {
        std::vector<RenderBox*> boxes;
        for (RenderObject* ch = list->firstChild(); ch; ch = list->childAfter(ch))
            if (auto* b = dynamic_cast<RenderBox*>(ch))
                boxes.push_back(b);
        check(boxes.size() == 4, "4 个子节点全部在场（list delegate 急切）");
        bool stretched = !boxes.empty();
        for (RenderBox* b : boxes)
            stretched = stretched && near(b->size().width, 200.0) && near(b->size().height, 100.0);
        check(stretched, "每个子节点 = 视口宽 × 自身高（交叉轴紧约束）");
        if (boxes.size() == 4) {
            auto* pd = dynamic_cast<SliverPhysicalParentData*>(boxes[3]->parentData());
            check(pd && near(pd->offset.dy, 300.0), "第 4 项绘制偏移 = layoutOffset 300 - offset 0");
        }
    }

    // 拖拽滚动。
    h->dispatch(downAt(100, 250));
    h->dispatch(moveAt(100, 200));  // delta=-50 → offset=50
    h->dispatch(upAt(100, 200));
    check(near(c.offset(), 50.0), "ListView 上拖 50px → offset=50");
}

// ── 测试 5：GridView.count ──────────────────────────────────────────────────
void testGridView() {
    std::printf("[test] GridView.count\n");

    ScrollController c;
    std::vector<SharedPtr<Widget>> cells;
    for (int i = 0; i < 6; ++i)
        cells.push_back(cui::make_shared<SizedBox>(0.0, 0.0));

    auto h = std::make_unique<Harness>();
    // 视口 200×200，2 列，无间距，方格 → 单元格 100×100，3 行 → 内容 300。
    h->attach(cui::make_shared<GridView>(&c, 2, cells), 200, 200);

    check(near(c.maxScrollExtent(), 100.0), "maxScrollExtent = 3*100 - 200 = 100");

    // GridView 现在走 CustomScrollView + SliverGrid（sliver 流水线）。
    auto* g = h->findRenderObject<RenderSliverGrid>();
    check(g != nullptr, "生成 RenderSliverGrid（sliver 流水线）");
    if (g) {
        std::vector<RenderBox*> boxes;
        for (RenderObject* ch = g->firstChild(); ch; ch = g->childAfter(ch))
            if (auto* b = dynamic_cast<RenderBox*>(ch))
                boxes.push_back(b);
        // list delegate 无托管人 ⇒ collectGarbage 直接返回，6 个瓦片全在场。
        check(boxes.size() == 6, "list delegate 急切物化全部 6 个子节点");
        check(boxes.size() == 6 && boxes.front() && near(boxes.front()->size().width, 100.0) &&
                  near(boxes.front()->size().height, 100.0),
              "delegate 给的紧约束 = 瓦片 100×100（子节点写的 0×0 被顶掉）");
        if (boxes.size() == 6) {
            auto* pd = dynamic_cast<SliverPhysicalParentData*>(boxes[5]->parentData());
            check(pd && near(pd->offset.dx, 100.0) && near(pd->offset.dy, 200.0),
                  "末瓦片绘制偏移 = (crossAxisOffset, layoutOffset - scrollOffset)");
        }
    }

    // 拖拽滚动。
    h->dispatch(downAt(100, 150));
    h->dispatch(moveAt(100, 100));  // delta=-50
    h->dispatch(upAt(100, 100));
    check(near(c.offset(), 50.0), "GridView 上拖 50px → offset=50");
}

// ── 测试 6：Scrollbar ───────────────────────────────────────────────────────
void testScrollbar() {
    std::printf("[test] Scrollbar\n");

    ScrollController c;
    auto content = cui::make_shared<SizedBox>(200.0, 800.0);
    auto h = std::make_unique<Harness>();
    h->attach(cui::make_shared<Scrollbar>(
                  &c, cui::make_shared<SingleChildScrollView>(&c, content)),
              200, 300);

    auto* sb = h->findRenderObject<RenderScrollbar>();
    check(sb != nullptr, "生成 RenderScrollbar");
    check(near(c.maxScrollExtent(), 500.0), "Scrollbar 内部滚动视图测量正确");

    if (sb) {
        // 默认 8px 厚、crossAxisMargin=0 ⇒ 轨道与 thumb 都贴住控件右边缘。
        Rect k = sb->trackRect();
        check(near(k.width, 8.0, 0.01), "轨道宽 = thickness");
        check(near(k.x, 192.0, 0.01), "轨道贴右边缘（x = W - thickness）");
        check(near(k.y, 0.0, 0.01) && near(k.height, 300.0, 0.01), "轨道占满视口全高");

        // thumb 长度 = traversable * viewport/(viewport+max) = 300 * 300/800 = 112.5。
        Rect t = sb->thumbRect();
        check(near(t.height, 300.0 * 300.0 / 800.0, 0.01), "thumb 长度按视口比例");
        check(near(t.y, 0.0, 0.01), "offset=0 → thumb 顶端贴顶");
        check(near(t.x, 192.0, 0.01) && near(t.width, 8.0, 0.01), "thumb 与轨道同宽同位");

        // 滚动到一半：thumb 下移 (offset/max)*(track-len)。
        h->dispatch(downAt(100, 250));
        h->dispatch(moveAt(100, 100));  // delta=-150 → offset=150
        h->dispatch(upAt(100, 100));
        Rect t2 = sb->thumbRect();
        double track = 300.0;
        double len = track * 300.0 / 800.0;
        double expectY = (150.0 / 500.0) * (track - len);
        check(near(t2.y, expectY, 0.01), "offset=150 → thumb 位置按比例下移");

        // mainAxisMargin 只在主轴内缩、crossAxisMargin 只加宽轨道：thumb
        // 始终在轨道宽度方向居中，轨道右缘恒贴控件（上游同名参数口径）。
        ScrollController c2;
        auto h2 = std::make_unique<Harness>();
        h2->attach(cui::make_shared<Scrollbar>(
                       &c2, cui::make_shared<SingleChildScrollView>(
                                &c2, cui::make_shared<SizedBox>(200.0, 800.0)),
                       nullptr, 8.0, 10.0, 3.0),
                   200, 300);
        auto* sb2 = h2->findRenderObject<RenderScrollbar>();
        if (sb2) {
            check(near(sb2->trackRect().x, 186.0, 0.01),
                  "crossAxisMargin=3 → 轨道左移 6px（右边仍贴边）");
            check(near(sb2->trackRect().width, 14.0, 0.01),
                  "轨道宽 = thickness + 2·crossAxisMargin");
            check(near(sb2->thumbRect().x, 189.0, 0.01), "thumb 在轨道宽度方向居中");
            check(near(sb2->thumbRect().y, 10.0, 0.01), "mainAxisMargin=10 → thumb 顶端内缩");
            check(near(sb2->thumbRect().height, 280.0 * 300.0 / 800.0, 0.01),
                  "thumb 长度按可行区间（视口高 - 2·mainAxisMargin）算");
        }

        // 内容极短（thumb 比例低于下限）时夹到 kMinThumbExtent。
        ScrollController c3;
        auto h3 = std::make_unique<Harness>();
        h3->attach(cui::make_shared<Scrollbar>(
                       &c3, cui::make_shared<SingleChildScrollView>(
                                &c3, cui::make_shared<SizedBox>(200.0, 4000.0))),
                   200, 300);
        auto* sb3 = h3->findRenderObject<RenderScrollbar>();
        if (sb3) {
            check(near(c3.maxScrollExtent(), 3700.0, 0.01), "4000 高内容 → max=3700");
            check(near(sb3->thumbRect().height, RenderScrollbar::kMinThumbExtent, 0.01),
                  "比例过小时 thumb 夹到最小长度");
        }
    }
}

// ── 测试 6b：Scrollbar 的 thumb 拖拽 / 轨道点击 ─────────────────────────────
/// 滚动条是「画在内容之上」的覆盖层，但拖动 thumb 必须**向下拖 = 内容向下滚**
/// （offset 增大）。上游 `handleThumbPress` 用 `getTrackToScroll` 做等比换算；
/// 按在轨道空白处则向点击方向翻一页（`handleTrackTapDown`）。
void testScrollbarDrag() {
    std::printf("[test] Scrollbar 拖拽\n");

    ScrollController c;
    auto h = std::make_unique<Harness>();
    h->attach(cui::make_shared<Scrollbar>(
                  &c, cui::make_shared<SingleChildScrollView>(
                          &c, cui::make_shared<SizedBox>(200.0, 800.0))),
              200, 300);
    auto* sb = h->findRenderObject<RenderScrollbar>();
    check(sb != nullptr && near(c.maxScrollExtent(), 500.0), "准备：max=500、thumb 112.5");
    if (!sb)
        return;

    // thumb：y∈[0, 112.5]、x∈[192, 200]；可行走距离 = 300 - 112.5 = 187.5。
    const double movable = 300.0 - sb->thumbRect().height;
    h->dispatch(downAt(196, 56));
    check(near(c.offset(), 0.0), "按在 thumb 上不改变偏移");
    h->dispatch(moveAt(196, 56 + 93.75));
    check(near(c.offset(), 93.75 / movable * 500.0, 0.01),
          "向下拖 93.75px → offset 增至 250（方向正确）");
    h->dispatch(moveAt(196, 400));
    check(near(c.offset(), 500.0, 0.01), "继续向下拖 → 夹到 max");
    h->dispatch(upAt(196, 400));
    h->dispatch(moveAt(196, 56));
    check(near(c.offset(), 500.0, 0.01), "抬手后移动不再拖动");

    // 反向：拖回上方 → offset 减小，到头夹回 0。
    h->dispatch(downAt(196, 240));
    h->dispatch(moveAt(196, 240 - 93.75));
    check(near(c.offset(), 250.0, 0.01), "向上拖 93.75px → offset 回到 250");
    h->dispatch(moveAt(196, 0));
    check(near(c.offset(), 0.0, 0.01), "拖出轨道顶端 → 夹到 0");
    h->dispatch(upAt(196, 0));

    // 轨道点击：向点击方向翻一页（页长 = max(40, 视口 - 16)），且不会连带
    // 触发内容拖拽（同一段手势里再上移 100px 不应额外加偏移）。
    h->dispatch(downAt(196, 250));
    check(near(c.offset(), 284.0, 0.01), "点 thumb 下方的轨道 → 向下一页");
    h->dispatch(moveAt(196, 150));
    check(near(c.offset(), 284.0, 0.01), "轨道点击后的移动不再拖内容");
    h->dispatch(upAt(196, 150));
    h->dispatch(downAt(196, 20));
    check(near(c.offset(), 0.0, 0.01), "点 thumb 上方的轨道 → 向上一页");
    h->dispatch(upAt(196, 20));

    // 轨道之外仍是内容拖拽（内容跟手 ⇒ 上移 50px → offset +50）。
    h->dispatch(downAt(100, 150));
    h->dispatch(moveAt(100, 100));
    h->dispatch(upAt(100, 100));
    check(near(c.offset(), 50.0, 0.01), "轨道外的拖动仍是内容拖拽");

    // 滚轮落在条带上同样要滚动内容（内容子树不在命中链里，由滚动条转交）。
    PointerScrollEvent wheel;
    wheel.position = {196, 150};
    wheel.scrollDeltaY = 56.0;
    h->dispatch(wheel);
    check(near(c.offset(), 106.0, 0.01), "滚轮落在条带上仍能滚动内容");

    // 内容不溢出时轨道不接管指针（max=0）。
    ScrollController c0;
    auto h0 = std::make_unique<Harness>();
    h0->attach(cui::make_shared<Scrollbar>(
                   &c0, cui::make_shared<SingleChildScrollView>(
                            &c0, cui::make_shared<SizedBox>(200.0, 100.0))),
               200, 300);
    auto* sb0 = h0->findRenderObject<RenderScrollbar>();
    check(sb0 && sb0->trackRect().width > 0.0 && !sb0->hitTestSelf({196, 150}),
          "max=0 时轨道不接管指针");
}

// ── 测试 7：滚动后的命中测试 ────────────────────────────────────────────────
void testHitTestAfterScroll() {
    std::printf("[test] 滚动后命中测试\n");

    ScrollController c;
    int taps[4] = {0, 0, 0, 0};
    std::vector<SharedPtr<Widget>> items;
    for (int i = 0; i < 4; ++i) {
        int* slot = &taps[i];
        items.push_back(cui::make_shared<GestureDetector>(
            cui::make_shared<DecoratedBox>(
                BoxDecoration{Color::fromRGBA(0.8, 0.2, 0.2, 1.0)},
                cui::make_shared<SizedBox>(200.0, 100.0)),
            [slot] { ++(*slot); }));
    }

    auto h = std::make_unique<Harness>();
    h->attach(cui::make_shared<ListView>(&c, items), 200, 300);

    // 未滚动：点击 viewport y=50 → 第 1 项（内容 y 0..100）。
    h->dispatch(downAt(100, 50));
    h->dispatch(upAt(100, 50));
    check(taps[0] == 1 && taps[1] == 0, "未滚动点击 → 第 1 项命中");

    // 滚动 100px：内容上移 100 → viewport y=50 对应内容 y=150 → 第 2 项。
    h->dispatch(downAt(100, 250));
    h->dispatch(moveAt(100, 150));  // delta=-100 → offset=100
    h->dispatch(upAt(100, 150));
    check(near(c.offset(), 100.0), "滚动到 offset=100");

    h->dispatch(downAt(100, 50));
    h->dispatch(upAt(100, 50));
    check(taps[1] == 1 && taps[0] == 1, "滚动后同一点位命中第 2 项（坐标回补正确）");
    check(taps[2] == 0 && taps[3] == 0, "未命中的项计数不变");
}

// ── 测试 8：内容缩短 ⇒ 视口偏移自动夹回（侧栏底部 folder 收起）──────────────
/// 上游 `_RenderSingleChildViewport.performLayout` 在量完内容后先把当前位置
/// `correctBy` 夹回 `[min, max]`，再 `applyContentDimensions`。本仓的绘制偏移由
/// widget 在 build 期推给渲染对象，若只在 `ScrollController::applyExtents` 里静默
/// 夹 controller，视口仍按旧偏移绘制 ⇒ 内容变短后底部空出一条「剩余空间」。
class SidebarProbe : public StatefulWidget {
public:
    SidebarProbe(ScrollController* ctrl, int rows, bool reverse = false,
                 KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), ctrl_(ctrl), rows_(rows), reverse_(reverse) {}

    ScrollController* ctrl() const { return ctrl_; }
    int rows() const { return rows_; }
    bool reverse() const { return reverse_; }

    std::unique_ptr<State> createState() override;

private:
    ScrollController* ctrl_;
    int rows_;
    bool reverse_;
};

class SidebarProbeState : public State {
public:
    void initState() override { rows_ = probe()->rows(); }

    /// rows 行 × 30px；末行挂点击标记，用来验「内容尾边贴视口底」。
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> rows;
        for (int i = 0; i < rows_; ++i) {
            SharedPtr<Widget> box = cui::make_shared<SizedBox>(200.0, 30.0);
            if (i == rows_ - 1) {
                box = cui::make_shared<GestureDetector>(
                    cui::make_shared<DecoratedBox>(
                        BoxDecoration{Color::fromRGBA(0.8, 0.2, 0.2, 1.0)}, box),
                    [this] { ++tailTaps_; });
            }
            rows.push_back(std::move(box));
        }
        auto col = cui::make_shared<Column>(std::move(rows));
        col->setMainAxisSize(MainAxisSize::Min);
        return cui::make_shared<SingleChildScrollView>(
            probe()->ctrl(), col, Axis::Vertical, /*keyboardScroll=*/false, probe()->reverse());
    }

    void setRows(int n) {
        if (n == rows_)
            return;
        rows_ = n;
        setState();
    }
    int tailTaps() const { return tailTaps_; }

private:
    SidebarProbe* probe() { return static_cast<SidebarProbe*>(widget()); }

    int rows_ = 0;
    int tailTaps_ = 0;
};

inline std::unique_ptr<State> SidebarProbe::createState() {
    return std::make_unique<SidebarProbeState>();
}

SidebarProbeState* probeState(Element* root) {
    return dynamic_cast<SidebarProbeState*>(static_cast<StatefulElement*>(root)->state());
}

void testShrinkContentClampsOffset() {
    std::printf("[test] 内容缩短后偏移夹回（侧栏折叠）\n");

    // 视口 200×300，20 行 ×30 = 600 ⇒ maxExtent 300。
    ScrollController c;
    auto h = std::make_unique<Harness>();
    h->attach(cui::make_shared<SidebarProbe>(&c, 20), 200, 300);
    auto* st = probeState(h->root);
    check(st != nullptr, "拿到侧栏探针 State");
    if (!st)
        return;
    check(near(c.maxScrollExtent(), 300.0), "20 行 ×30：maxExtent = 600 - 300");

    auto* vp = h->findRenderObject<RenderSingleChildViewport>();
    check(vp != nullptr, "生成 RenderSingleChildViewport");
    if (!vp)
        return;

    // 滚到底：偏移 300，末行贴视口底 ⇒ 视口最后一行像素命中末行。
    c.jumpTo(300.0);
    h->pump();
    check(near(vp->offset(), 300.0), "滚到底：视口绘制偏移 = 300");
    h->dispatch(downAt(100, 290));
    h->dispatch(upAt(100, 290));
    check(st->tailTaps() == 1, "折叠前：viewport y=290 命中末行（内容尾边贴视口底）");

    // 「折叠底部 folder」：20 行 → 13 行 ⇒ 内容 390 ⇒ maxExtent 90。
    // 旧偏移 300 越界 ⇒ 必须夹回 90，上面的内容顶下来把底部填满。
    st->setRows(13);
    h->pump();
    check(near(c.maxScrollExtent(), 90.0), "折叠后：maxExtent = 390 - 300 = 90");
    check(near(c.offset(), 90.0), "折叠后：controller 偏移夹到 90");
    check(near(vp->offset(), 90.0), "折叠后：视口绘制偏移同步夹到 90（修复点）");
    h->dispatch(downAt(100, 290));
    h->dispatch(upAt(100, 290));
    check(st->tailTaps() == 2, "折叠后：viewport y=290 仍命中新末行 ⇒ 底部没有留白");

    // 内容缩到比视口还短：偏移回到 0（无界可滚）。
    st->setRows(6);
    h->pump();
    check(near(c.maxScrollExtent(), 0.0), "6 行 ×30 = 180 < 300 ⇒ maxExtent 0");
    check(near(vp->offset(), 0.0), "内容短于视口 ⇒ 偏移回到 0");

    // 上边界（上游的 _minScrollExtent = 0）：越界偏移在重排时被夹回。
    c.forceOffset(-40.0);
    h->pump();
    check(near(vp->offset(), -40.0), "forceOffset 的越界值在纯重建里保留（回弹可见）");
    st->setRows(20);  // 内容变长的同一次重建里发生重排 ⇒ 夹回 [0, max]
    h->pump();
    check(near(vp->offset(), 0.0), "越上界 -40 在重排时夹回 0");

    // 反向轴（Up）：偏移语义同为 [0, max]，折叠同样要夹回。
    ScrollController rc;
    auto h2 = std::make_unique<Harness>();
    h2->attach(cui::make_shared<SidebarProbe>(&rc, 20, true), 200, 300);
    auto* rs = probeState(h2->root);
    rc.jumpTo(300.0);
    h2->pump();
    auto* rvp = h2->findRenderObject<RenderSingleChildViewport>();
    check(rvp && near(rvp->offset(), 300.0), "Up：滚到底偏移 300");
    rs->setRows(13);
    h2->pump();
    check(rvp && near(rvp->offset(), 90.0), "Up：折叠后同样夹回 90");
}

// ── 测试 9：两层滚动条的滚轮归属 ────────────────────────────────────────────
/// 上游 `PointerSignalResolver`：命中链里**第一个**（= 最深的那个）「真能吸收
/// 这份增量」的 Scrollable 独占本次滚轮，其余落空。本仓靠「认领才动 + 动完
/// 拦住冒泡」实现同一语义。
///
/// 钉的是**归属**而不是剩余量传递：里层能吃多少算多少，被边界吃掉的那部分
/// **不会**在同一次滚轮里转给外层（上游 `pointerScroll` 硬夹到 [min,max] 后
/// 直接丢弃），要等下一次滚轮 —— 也就是「滚到底之后外层才动」。
void testNestedWheelScroll() {
    std::printf("[test] 嵌套滚动视图的滚轮归属\n");

    // 里层 = 200×200 视口 + 内容 innerH；外层视口 200，内容 = 里层 + 400 标记。
    // 命中点恒取 (100,100)：外层偏移必须 < 100 才仍落在里层盒内，所以用例里
    // 外层最多滚到 20。
    auto buildTree = [](ScrollController& inner, ScrollController& outer, double innerH,
                        SharedPtr<ScrollPhysics> innerPhysics) {
        auto innerScroll = cui::make_shared<Scrollable>(
            &inner, Axis::Vertical, cui::make_shared<SizedBox>(200.0, innerH),
            std::move(innerPhysics));
        auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
            cui::make_shared<SizedBox>(200.0, 200.0, innerScroll),
            cui::make_shared<SizedBox>(200.0, 400.0)});
        auto h = std::make_unique<Harness>();
        h->attach(cui::make_shared<Scrollable>(&outer, Axis::Vertical, col), 200, 200);
        return h;
    };
    auto wheelOn = [](Harness* h, double dy) {
        PointerScrollEvent e;
        e.position = {100, 100};
        e.scrollDeltaY = dy;  // 像素增量（Application 已把「格」换算成像素）
        h->dispatch(e);
    };

    {
        ScrollController inner, outer;
        auto h = buildTree(inner, outer, 500.0, nullptr);
        check(near(inner.maxScrollExtent(), 300.0), "里层 maxExtent = 500-200 = 300");
        check(near(outer.maxScrollExtent(), 400.0), "外层 maxExtent = 600-200 = 400");

        wheelOn(h.get(), 56.0);
        check(near(inner.offset(), 56.0) && near(outer.offset(), 0.0),
              "里层能滚 ⇒ 里层独占，外层不再跟着滚同一份增量");

        wheelOn(h.get(), 1000.0);
        check(near(inner.offset(), 300.0) && near(outer.offset(), 0.0),
              "里层滚到底（夹到 300），本次剩余量不转给外层");

        wheelOn(h.get(), 20.0);
        check(near(inner.offset(), 300.0) && near(outer.offset(), 20.0),
              "到底后的下一次滚轮才交给外层（用户诉求）");

        wheelOn(h.get(), -20.0);
        check(near(inner.offset(), 280.0) && near(outer.offset(), 20.0),
              "反向增量里层还能吃 ⇒ 又归里层，外层不跟着反向滚");

        wheelOn(h.get(), -1000.0);
        check(near(inner.offset(), 0.0) && near(outer.offset(), 20.0),
              "里层滚到顶（0）");

        wheelOn(h.get(), -20.0);
        check(near(inner.offset(), 0.0) && near(outer.offset(), 0.0),
              "到顶后的下一次滚轮交给外层反向滚（外层已在顶边 ⇒ 就此打住）");
    }

    {
        // 里层禁止用户滚动 ⇒ 上游 `_receivedPointerSignal` 的第一道闸，
        // 里层既不滚也不认领，增量直接给外层。
        ScrollController inner, outer;
        auto h = buildTree(inner, outer, 500.0, cui::make_shared<NeverScrollableScrollPhysics>());
        wheelOn(h.get(), 60.0);
        check(near(inner.offset(), 0.0), "NeverScrollable 里层不动");
        check(near(outer.offset(), 60.0), "NeverScrollable 里层不认领 ⇒ 外层接手");
    }

    {
        // 里层内容不超高（maxExtent 0）⇒ 同样不该拦外层。
        ScrollController inner, outer;
        auto h = buildTree(inner, outer, 200.0, nullptr);
        check(near(inner.maxScrollExtent(), 0.0), "里层内容 = 视口高 ⇒ maxExtent 0");
        wheelOn(h.get(), 60.0);
        check(near(inner.offset(), 0.0) && near(outer.offset(), 60.0),
              "里层滚不动 ⇒ 外层接手");
    }
}

}  // namespace

int main() {
    std::printf("=== test_scroll ===\n");
    testScrollController();
    testViewportMeasure();
    testSingleChildScrollViewReverseAndPadding();
    testSingleChildScroll();
    testWheelScroll();
    testNestedAxisViewport();
    testListView();
    testGridView();
    testScrollbar();
    testScrollbarDrag();
    testHitTestAfterScroll();
    testShrinkContentClampsOffset();
    testNestedWheelScroll();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
