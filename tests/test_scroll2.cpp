/// @file test_scroll2.cpp
/// @brief 滚动系列第二批单元测试（ScrollPhysics / ListView.builder / GridView.builder 懒加载）。
///
/// 覆盖：
///   1. ScrollPhysics：Clamping 越界钳制；Bouncing 越界阻尼 + 界外摩擦；
///      NeverScrollable 不动。
///   2. Scrollable + physics 集成：Bouncing 拖出上边界得阻尼偏移，
///      panEnd 回弹到 0；NeverScrollable 拖拽无效。
///   3. ListView.builder(itemExtent)（sliver 版）：初始只构建可见窗口 ±
///      cache 行；滚动后窗口平移，窗口外的行从未构建；内容总几何
///      （maxScrollExtent）与全量构建一致。
///   4. GridView.builder（sliver 版）：缓存窗口按行物化、滚过中段整段跳过、
///      几何与 SliverGridLayout 精确一致。
///   5. ListView.builder / ListView.separated（sliver 版）：定高窗口逐行精确；
///      separated 的 2n-1 槽位交替映射；变高列表长度先外推、触底后收回精确值。

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_scroll.h"
#include "cui/rendering/render_view.h"
#include "cui/rendering/sliver.h"       // RenderSliverMultiBoxAdaptor::indexOf
#include "cui/rendering/sliver_grid.h"  // RenderSliverGrid（窗口探针）
#include "cui/widgets/basic.h"
#include "cui/widgets/scroll.h"
#include "cui/widgets/scroll_physics.h"

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
        pump();
    }

    void dispatch(const PointerEvent& ev) { po.dispatchPointerEvent(ev); }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
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

// ── 测试 1：ScrollPhysics 单元 ──────────────────────────────────────────────
void testPhysics() {
    std::printf("[test] ScrollPhysics\n");

    ClampingScrollPhysics clamping;
    BouncingScrollPhysics bouncing;
    NeverScrollableScrollPhysics never;

    check(near(clamping.applyTo(150.0, 0.0, 0.0, 100.0), 100.0),
          "Clamping：越下界钳到 max");
    check(near(clamping.applyTo(-30.0, 0.0, 0.0, 100.0), 0.0),
          "Clamping：越上界钳到 0");
    check(near(clamping.applyTo(60.0, 0.0, 0.0, 100.0), 60.0),
          "Clamping：界内透传");

    check(near(bouncing.applyTo(150.0, 0.0, 0.0, 100.0), 125.0),
          "Bouncing：越 max 阻尼减半");
    check(near(bouncing.applyTo(-60.0, 0.0, 0.0, 100.0), -30.0),
          "Bouncing：越 0 阻尼减半");
    check(near(bouncing.applyTo(-20.0, -30.0, 0.0, 100.0), -25.0),
          "Bouncing：界外摩擦（delta 减半）");
    check(near(bouncing.applyTo(60.0, 0.0, 0.0, 100.0), 60.0),
          "Bouncing：界内透传");

    check(near(never.applyTo(50.0, 10.0, 0.0, 100.0), 10.0),
          "NeverScrollable：输入不动");
}

// ── 测试 2：Scrollable + physics 集成 ──────────────────────────────────────
void testScrollablePhysics() {
    std::printf("[test] Scrollable + physics\n");

    // Bouncing：拖出上边界得阻尼偏移，panEnd 回弹到 0。
    {
        ScrollController c;
        std::vector<SharedPtr<Widget>> items;
        for (int i = 0; i < 10; ++i)
            items.push_back(cui::make_shared<SizedBox>(0.0, 50.0));
        auto scrollable = cui::make_shared<Scrollable>(
            &c, Axis::Vertical, cui::make_shared<Column>(items),
            cui::make_shared<BouncingScrollPhysics>());
        Harness h;
        h.attach(scrollable, 200, 200);

        h.dispatch(downAt(100, 100));
        h.dispatch(moveAt(100, 160));  // delta dy=+60 → target=-60 → 阻尼 -30
        check(near(c.offset(), -30.0), "Bouncing 越上界偏移 = -30");
        h.dispatch(upAt(100, 160));
        check(near(c.offset(), 0.0), "panEnd 回弹到边界 0");
    }

    // NeverScrollable：拖拽与滚轮均无效。
    {
        ScrollController c;
        std::vector<SharedPtr<Widget>> items;
        for (int i = 0; i < 10; ++i)
            items.push_back(cui::make_shared<SizedBox>(0.0, 50.0));
        auto scrollable = cui::make_shared<Scrollable>(
            &c, Axis::Vertical, cui::make_shared<Column>(items),
            cui::make_shared<NeverScrollableScrollPhysics>());
        Harness h;
        h.attach(scrollable, 200, 200);

        h.dispatch(downAt(100, 100));
        h.dispatch(moveAt(100, 40));
        h.dispatch(upAt(100, 40));
        check(near(c.offset(), 0.0), "NeverScrollable 拖拽不动");

        PointerScrollEvent scroll;
        scroll.position = {100, 100};
        scroll.scrollDeltaY = 60.0;
        h.dispatch(scroll);
        check(near(c.offset(), 0.0), "NeverScrollable 滚轮不动");
    }

    // 默认 Clamping：滚轮到底不越界。
    {
        ScrollController c;
        std::vector<SharedPtr<Widget>> items;
        for (int i = 0; i < 10; ++i)
            items.push_back(cui::make_shared<SizedBox>(0.0, 50.0));
        auto scrollable =
            cui::make_shared<Scrollable>(&c, Axis::Vertical,
                                         cui::make_shared<Column>(items));
        Harness h;
        h.attach(scrollable, 200, 200);

        PointerScrollEvent scroll;
        scroll.position = {100, 100};
        scroll.scrollDeltaY = 10000.0;
        h.dispatch(scroll);
        check(near(c.offset(), c.maxScrollExtent()), "Clamping 滚轮钳在 maxScrollExtent");
    }
}

// ── 测试 3：ListView.builder 懒加载（定高）──────────────────────────────────
/// 旧 ListViewLazy 的「固定行高 + 等高占位 + Ticker 补帧」近似已删，改成
/// SliverFixedExtentList。cacheExtent 从「行数」换成「像素」= 上游口径；
/// 传 2×40=80px 时窗口与旧实现逐行一致（左闭右开）。
void testListViewBuilder() {
    std::printf("[test] ListView.builder(itemExtent)\n");

    ScrollController c;
    std::vector<int> builds(100, 0);
    auto lazy = ListView::builder(
        &c,
        [&](BuildContext&, int i) {
            ++builds[i];
            return cui::make_shared<SizedBox>(0.0, 40.0);
        },
        100, 40.0, Axis::Vertical, false, /*cacheExtent=*/80.0);
    Harness h;
    h.attach(lazy, 200, 200);

    auto windowOf = [&]() {
        std::vector<int> idx;
        auto* l = h.findRenderObject<RenderSliverFixedExtentList>();
        for (RenderObject* ch = l ? l->firstChild() : nullptr; ch; ch = l->childAfter(ch))
            idx.push_back(l->indexOf(ch));
        return idx;
    };

    // 窗口 = [offset-80, offset+280)：offset 0 ⇒ 行 0..6（行 7 起点 280 落在右开界外）。
    std::vector<int> win = windowOf();
    check(win.size() == 7 && win.front() == 0 && win.back() == 6,
          "初始只构建可见窗口 ± cache（7 行）");
    check(builds[50] == 0, "窗口外的行未构建");
    check(near(c.maxScrollExtent(), 100.0 * 40.0 - 200.0),
          "maxScrollExtent = 总高 - 视口（几何与全量构建一致）");

    // 滚动到 offset 400：窗口 [320, 680) ⇒ 行 8..16（行 7 与 17 都差一点点）。
    c.jumpTo(400.0);
    h.pump();
    win = windowOf();
    check(win.size() == 9 && win.front() == 8 && win.back() == 16,
          "滚动后窗口平移到行 8..16");
    int distinct = 0;
    for (int i = 0; i < 100; ++i)
        if (builds[i] > 0)
            ++distinct;
    check(distinct == 16, "累计构建 {0..6} ∪ {8..16} 共 16 行");
    check(builds[7] == 0 && builds[17] == 0 && builds[99] == 0,
          "新窗口外（含恰被跳过的 index 7）仍未构建");
    check(builds[12] > 0, "窗口内行已构建");
}

// ── 测试 4：GridView.builder 懒加载（sliver 版）─────────────────────────────
/// GridView 走 CustomScrollView + SliverGrid，窗口/几何由 SliverGridLayout
/// 说了算；旧 GridViewLazy 的 LayoutBuilder + Ticker 补帧近似已整段删掉。
void testGridViewBuilder() {
    std::printf("[test] GridView.builder\n");

    ScrollController c;
    std::vector<int> builds(99, 0);
    auto grid = GridView::builder(
        &c, cui::make_shared<SliverGridDelegateWithFixedCrossAxisCount>(3, 4.0, 0.0, 1.0),
        [&](BuildContext&, int i) {
            ++builds[i];
            return cui::make_shared<SizedBox>(0.0, 0.0);
        },
        99);
    Harness h;
    h.attach(grid, 200, 200);

    auto distinctCount = [&]() {
        int n = 0;
        for (int i = 0; i < 99; ++i)
            if (builds[i] > 0)
                ++n;
        return n;
    };
    auto windowOf = [&]() {
        std::vector<int> idx;
        auto* g = h.findRenderObject<RenderSliverGrid>();
        for (RenderObject* ch = g ? g->firstChild() : nullptr; ch; ch = g->childAfter(ch))
            idx.push_back(g->indexOf(ch));
        return idx;
    };

    // 瓦片：crossAxisExtent = 200/3，mainAxisExtent 同（aspect 1），
    // mainAxisStride = 200/3 + 4 ≈ 70.667。
    // 缓存窗口 = [offset - 250, offset + 450)（cacheOrigin 负、
    // remainingCacheExtent = 视口 200 + 前后 2×250）。offset 0 时右界 450
    // 落在行 6（ceil-1 ⇒ 6*3+2 = 20），故 item 0..20 物化。
    std::vector<int> win = windowOf();
    check(win.size() == 21 && win.front() == 0 && win.back() == 20,
          "初始窗口 = 缓存区间覆盖的行 0..6（item 0..20）");
    check(distinctCount() == 21, "窗口外的单元格未构建");
    check(builds[21] == 0 && builds[50] == 0, "行 7 起从未构建");

    // 内容长度 = 末项尾边 = 行 32 起 2261.333 + 瓦片 66.667 = 2328；
    // maxScrollExtent = 2328 - 视口 200。
    const double stride = 200.0 / 3.0 + 4.0;
    check(near(c.maxScrollExtent(), 32.0 * stride + 200.0 / 3.0 - 200.0, 0.01),
          "maxScrollExtent = 末项尾边 - 视口");
    check(near(c.viewportExtent(), 200.0), "视口长度 = 200");

    // 滚到 2000：窗口 = [1750, 2450) → 行 24..34，右界夹到末项 98 ⇒ item 72..98。
    // 行 7..23（item 21..71）整段跳过，一次都没构建。
    c.jumpTo(2000.0);
    h.pump();
    win = windowOf();
    check(win.size() == 27 && win.front() == 72 && win.back() == 98,
          "滚动后窗口 = item 72..98（缓存右界过末尾，夹到最后一项）");
    check(distinctCount() == 48, "累计构建 0..20 ∪ 72..98 = 48 个");
    check(builds[50] == 0 && builds[71] == 0, "中间行从未构建");
    check(builds[72] > 0 && builds[98] > 0, "底部窗口已构建");
    check(near(c.offset(), 2000.0), "偏移未被夹（2000 < maxScrollExtent）");
}

// ── 测试 5：ListView.separated 与变高懒加载 ─────────────────────────────────
/// `separated` = 上游 的 `2*n-1` 个槽位交替映射（偶数 item、奇数 separator）；
/// 不写 itemExtent 则走 `SliverList`，行高由子节点自己撑出——这是旧
/// ListViewLazy（固定行高近似）做不到的。
void testListViewSeparatedAndVariable() {
    std::printf("[test] ListView.separated / 变高\n");

    ScrollController c;
    std::vector<int> items(3, 0), seps(3, 0);
    auto list = ListView::separated(
        &c,
        [&](BuildContext&, int i) {
            ++items[i];
            return cui::make_shared<SizedBox>(0.0, 50.0);
        },
        [&](BuildContext&, int i) {
            ++seps[i];
            return cui::make_shared<SizedBox>(0.0, 10.0);
        },
        3, std::nullopt, Axis::Vertical, false, /*cacheExtent=*/0.0);
    Harness h;
    // 视口 170 = 50+10+50+10+50：正好装下全部 5 个槽位（2*3-1）。
    h.attach(list, 200, 170);

    auto* sl = h.findRenderObject<RenderSliverList>();
    check(sl != nullptr, "不写 itemExtent ⇒ 走 RenderSliverList（变高）");
    int kids = 0;
    if (sl) {
        for (RenderObject* ch = sl->firstChild(); ch; ch = sl->childAfter(ch))
            ++kids;
    }
    check(kids == 5, "槽位数 = 2*3-1 = 5（item/separator 交替）");
    check(items[0] == 1 && items[1] == 1 && items[2] == 1, "三个 item 各构建一次");
    check(seps[0] == 1 && seps[1] == 1 && seps[2] == 0, "分隔线只有 n-1 条");
    check(near(c.maxScrollExtent(), 0.0), "内容恰好铺满视口 ⇒ 无可滚距离");

    // ── 变高懒加载：60 行按 20/30/40 循环，cacheExtent 0 ⇒ 窗口=视口 ──
    ScrollController c2;
    std::vector<int> built(60, 0);
    auto variable = ListView::builder(
        &c2,
        [&](BuildContext&, int i) {
            ++built[i];
            return cui::make_shared<SizedBox>(0.0, 20.0 + (i % 3) * 10.0);
        },
        60, std::nullopt, Axis::Vertical, false, /*cacheExtent=*/0.0);
    Harness h2;
    h2.attach(variable, 200, 100);

    // 窗口 [0,100)：行高 20,30,40,20 → 前 4 行的起点 0/20/50/90 都 <100，
    // 第 5 行起点 110 越界 ⇒ 只构建 0..3。
    auto* sl2 = h2.findRenderObject<RenderSliverList>();
    int window = 0;
    for (RenderObject* ch = sl2 ? sl2->firstChild() : nullptr; ch; ch = sl2->childAfter(ch))
        ++window;
    check(window == 4, "变高列表只构建窗口内的 4 行（左闭右开）");
    check(built[4] == 0 && built[59] == 0, "窗口外从未构建");
    // 长度未知时先按均值外推：4 行铺了 110 ⇒ 平均 27.5 ⇒ 60 行 ≈ 1650。
    check(c2.maxScrollExtent() > 0.0 && c2.maxScrollExtent() < 1800.0,
          "未滚到底时长度按已布局行的均值外推（小于精确值）");

    // 未滚到时长度是外推值，所以 jumpTo 一次到不了真正的末尾（上游同款：
    // 每轮布局把均值外推往前推一截）。逐轮跳到自己不再动 ⇒ 末行被抓到、
    // reachedEnd 把长度收回精确值 20 组 ×(20+30+40) = 1800。
    for (int i = 0; i < 60 && c2.offset() < c2.maxScrollExtent(); ++i) {
        c2.jumpTo(c2.maxScrollExtent());
        h2.pump();
    }
    check(near(c2.maxScrollExtent(), 1800.0 - 100.0, 0.01),
          "滚到底后长度收敛到精确值（1800 - 视口）");
    check(built[59] > 0, "末行已构建");
    check(near(c2.offset(), 1700.0), "偏移夹到可滚末尾");
}

}  // namespace

int main() {
    testPhysics();
    testScrollablePhysics();
    testListViewBuilder();
    testGridViewBuilder();
    testListViewSeparatedAndVariable();

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILURES: %d\n", g_failures);
    return 1;
}
