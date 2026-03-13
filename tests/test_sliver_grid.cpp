/// @file test_sliver_grid.cpp
/// @brief 网格 sliver 测试：布局度量 + delegate 口径 + 控件层懒加载窗口。
///
/// 与其它 sliver 测试的分工：test_sliver.cpp 验渲染层协议与各 sliver 的窗口
/// 算法（假托管人）；test_sliver_lazy.cpp 验多盒子 sliver 的托管人契约；这里
/// 只补「序号 → 位置/尺寸」这一步交给 gridDelegate 之后的网格专属口径。
///
/// 覆盖：
///   1. SliverGridLayout 六个查询（行/列换算、reverseCrossAxis、边界与夹取）。
///   2. 两族 delegate 的 getLayout（spacing / aspectRatio / mainAxisExtent 三条
///      尺寸来源）+ shouldRelayout + validateMessage。
///   3. RenderSliverGrid 的 eager 走法（无托管人）：瓦片紧约束、交叉轴偏移、
///      水平轴的主/交叉换算；顺带记录「不回收存量子节点」这条既有偏离。
///   4. SliverGrid.builder 懒加载窗口：只造缓存窗内的瓦片、滚动时重叠区间
///      Element 复用、滚过末尾由视口把偏移夹回。
///   5. 换 gridDelegate：同序号 Element 原地改尺寸，窗口按新列数重算。

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_scroll.h"
#include "cui/rendering/render_view.h"
#include "cui/rendering/sliver.h"
#include "cui/rendering/sliver_grid.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/grid.h"
#include "cui/widgets/scroll.h"
#include "cui/widgets/sliver.h"

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

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

/// 固定理想尺寸的测试盒子；被瓦片紧约束夹紧。
class TestBox : public RenderBox {
public:
    TestBox(double w, double h) : w_(w), h_(h) {}

protected:
    void performLayout() override {
        const BoxConstraints c = constraints();
        setSize({std::min(w_, c.maxWidth), std::min(h_, c.maxHeight)});
    }
    bool hitTestSelf(const Offset&) const override { return true; }

private:
    double w_;
    double h_;
};

/// 竖直（Down）sliver 约束的便捷构造；水平传 dir=Right。
SliverConstraints vc(double scrollOffset, double remainingPaintExtent, double crossExtent,
                     double viewportMain, double cacheOrigin = 0.0,
                     double remainingCache = 0.0,
                     AxisDirection dir = AxisDirection::Down, double overlap = 0.0,
                     double preceding = 0.0) {
    SliverConstraints c;
    c.axisDirection = dir;
    c.growthDirection = axisDirectionToGrowthDirection(dir);
    c.scrollOffset = scrollOffset;
    c.remainingPaintExtent = remainingPaintExtent;
    c.crossAxisExtent = crossExtent;
    c.viewportMainAxisExtent = viewportMain;
    c.cacheOrigin = cacheOrigin;
    c.remainingCacheExtent = remainingCache;
    c.overlap = overlap;
    c.precedingScrollExtent = preceding;
    return c;
}

const Offset& childPaintOffset(RenderObject* child) {
    static const Offset kZero{0, 0};
    auto* pd = child && child->parentData()
                   ? dynamic_cast<SliverPhysicalParentData*>(child->parentData())
                   : nullptr;
    return pd ? pd->paintOffset : kZero;
}

// ── 控件层宿主（与 test_sliver_lazy.cpp 同款）───────────────────────────────

/// 测试用宿主：BuildOwner + RenderView + PipelineOwner（headless 帧泵）。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;
    RenderObject* rootRO = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        po.setRootNode(&rv);
        pump();
    }

    void syncRoot() {
        RenderObject* cur = root->findRenderObject();
        if (cur && cur != rootRO) {
            rootRO = cur;
            rv.setChild(static_cast<RenderBox*>(rootRO));
        }
    }

    void pump() {
        owner.flushBuildScope();
        syncRoot();
        po.flushLayout();
    }

    template <typename T>
    std::vector<T*> findAll() {
        std::vector<T*> out;
        struct Visitor {
            std::vector<T*>* out;
            void visit(RenderObject* ro) {
                if (!ro)
                    return;
                if (dynamic_cast<T*>(ro))
                    out->push_back(static_cast<T*>(ro));
                ro->visitChildren([this](AbstractNode* n) {
                    visit(static_cast<RenderObject*>(n));
                });
            }
        } v{&out};
        v.visit(rootRO);
        return out;
    }

    template <typename T>
    T* find() {
        auto all = findAll<T>();
        return all.empty() ? nullptr : all.front();
    }
};

/// 探针：测试改配置 + setState，观察网格窗口随重建/滚动怎么变。
struct Cfg {
    ScrollController* controller = nullptr;
    std::vector<SharedPtr<Widget>> slivers;
    int builds = 0;
};

class Probe;

class ProbeState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;
    void rebuildNow() { setState(); }
};

class Probe : public StatefulWidget {
public:
    explicit Probe(SharedPtr<Cfg> cfg) : cfg_(std::move(cfg)) {}
    const SharedPtr<Cfg>& cfg() const { return cfg_; }
    std::unique_ptr<State> createState() override { return std::make_unique<ProbeState>(); }

private:
    SharedPtr<Cfg> cfg_;
};

inline SharedPtr<Widget> ProbeState::build(BuildContext&) {
    const SharedPtr<Cfg>& c = static_cast<Probe*>(widget())->cfg();
    ++c->builds;
    auto body = cui::make_shared<CustomScrollView>(c->slivers, Axis::Vertical, false, c->controller);
    // 外层固定：根渲染对象不随子树类型易主（RenderView 只挂一次）。
    return cui::make_shared<SizedBox>(300, 400, std::move(body));
}

ProbeState* probeState(Element* root) {
    auto* se = dynamic_cast<StatefulElement*>(root);
    return se ? dynamic_cast<ProbeState*>(se->state()) : nullptr;
}

/// 一行内容：SizedBox 定高（尺寸最终被瓦片紧约束顶掉）。
SharedPtr<Widget> cell(double height) {
    BoxDecoration deco;
    deco.color = Color::fromRGBA(40, 90, 160, 255);
    return cui::make_shared<SizedBox>(300, height, cui::make_shared<DecoratedBox>(deco));
}

SliverMultiBoxAdaptorElement* elementOf(RenderSliverMultiBoxAdaptor* ro) {
    return ro ? dynamic_cast<SliverMultiBoxAdaptorElement*>(ro->childManager()) : nullptr;
}

std::vector<int> indicesOf(RenderSliverMultiBoxAdaptor* ro) {
    std::vector<int> out;
    if (!ro)
        return out;
    for (RenderObject* c = ro->firstChild(); c; c = ro->childAfter(c))
        out.push_back(ro->indexOf(c));
    return out;
}

std::vector<RenderBox*> boxesOf(RenderSliverMultiBoxAdaptor* ro) {
    std::vector<RenderBox*> out;
    if (!ro)
        return out;
    for (RenderObject* c = ro->firstChild(); c; c = ro->childAfter(c))
        out.push_back(static_cast<RenderBox*>(c));
    return out;
}

double layoutOffsetOf(RenderObject* ro) {
    auto* pd = ro && ro->parentData()
                   ? dynamic_cast<SliverMultiBoxAdaptorParentData*>(ro->parentData())
                   : nullptr;
    return pd && pd->layoutOffset ? *pd->layoutOffset : -1.0;
}

double gridMainExtentOf(RenderObject* ro) {
    auto* pd = ro && ro->parentData()
                   ? dynamic_cast<SliverGridParentData*>(ro->parentData())
                   : nullptr;
    return pd && pd->mainAxisExtent ? *pd->mainAxisExtent : -1.0;
}

double gridCrossOffsetOf(RenderObject* ro) {
    auto* pd = ro && ro->parentData()
                   ? dynamic_cast<SliverGridParentData*>(ro->parentData())
                   : nullptr;
    return pd && pd->crossAxisOffset ? *pd->crossAxisOffset : -1.0;
}

std::map<int, Element*> snapshot(SliverMultiBoxAdaptorElement* el) {
    std::map<int, Element*> out;
    if (!el)
        return out;
    for (const auto& kv : el->childElements())
        out[kv.first] = kv.second.get();
    return out;
}

std::vector<int> range(int from, int to) {
    std::vector<int> out;
    for (int i = from; i <= to; ++i)
        out.push_back(i);
    return out;
}

SharedPtr<SliverGridDelegate> fixed4() {
    return cui::make_shared<SliverGridDelegateWithFixedCrossAxisCount>(
        4, 0.0, 0.0, 1.0, std::optional<double>(100.0));
}

// ── 1. SliverGridLayout 的行/列数学 ─────────────────────────────────────────

void testGridLayoutMath() {
    std::printf("[sliver-grid] SliverGridLayout 查询\n");

    // 4 列、每行 100 高、行间距 0、列宽 75、列间距 0。
    SliverGridLayout l;
    l.crossAxisCount = 4;
    l.mainAxisStride = 100.0;
    l.crossAxisStride = 75.0;
    l.childMainAxisExtent = 100.0;
    l.childCrossAxisExtent = 75.0;

    check(near(l.getStartScrollOffsetForIndex(3), 0) &&
              near(l.getStartScrollOffsetForIndex(4), 100) &&
              near(l.getStartScrollOffsetForIndex(12), 300),
          "序号 → 所在行起点（行 = index ~/ crossAxisCount，12 是第 3 行首项）");
    check(near(l.getEndScrollOffsetForIndex(39), 1000) && near(l.getExtentForIndex(7), 100),
          "尾部 = 行起点 + 瓦片主轴尺寸");
    check(near(l.getStartScrollOffsetForIndex(-1), 0), "负序号夹到 0");

    const SliverGridGeometry g5 = l.getGeometryForIndex(5);
    check(near(g5.scrollOffset, 100) && near(g5.crossAxisOffset, 75) &&
              near(g5.mainAxisExtent, 100) && near(g5.crossAxisExtent, 75),
          "getGeometryForIndex(5) = 第 2 行第 2 列");

    check(l.getMinIndexForScrollOffset(99) == 0 && l.getMinIndexForScrollOffset(100) == 4 &&
              l.getMinIndexForScrollOffset(1000) == 40 && l.getMinIndexForScrollOffset(-5) == 0,
          "minIndex：下取整到行首，负值夹 0");
    check(l.getMaxIndexForScrollOffset(650, 40) == 27 &&
              l.getMaxIndexForScrollOffset(1650, 40) == 39,
          "maxIndex：所在行末尾，且不超过总项数");
    check(l.getMaxIndexForScrollOffset(600, 40) == 23,
          "正好落在行首 → 上一行结束、不含本行（上游 ceil-1 口径）");
    check(l.getMaxIndexForScrollOffset(-1, 40) == 0, "缓存窗口在零之前 → 一个都不造");

    check(near(l.getMaxScrollOffset(0), 0) && near(l.getMaxScrollOffset(1), 100) &&
              near(l.getMaxScrollOffset(40), 1000),
          "内容长度 = 最后一项的尾部");

    SliverGridLayout rev = l;
    rev.reverseCrossAxis = true;
    check(near(rev.getCrossAxisOffsetForIndex(4), 225) &&
              near(rev.getCrossAxisOffsetForIndex(5), 150) &&
              near(rev.getCrossAxisOffsetForIndex(7), 0),
          "reverseCrossAxis：同一行从交叉轴另一端开始排");

    SliverGridLayout degenerate;  // crossAxisCount 默认 1、stride 默认 0
    check(degenerate.getMinIndexForScrollOffset(500) == 0 &&
              near(degenerate.getMaxScrollOffset(3), 0),
          "stride 为 0 时不除零：序号恒 0、长度恒 0");
}

// ── 2. 两族 delegate ────────────────────────────────────────────────────────

void testGridDelegates() {
    std::printf("[sliver-grid] SliverGridDelegate 家族\n");

    SliverGridDelegateWithFixedCrossAxisCount f(4, 10.0, 5.0, 2.0);
    const SliverConstraints c = vc(0, 400, 215, 400, 0, 650);
    const SliverGridLayout fl = f.getLayout(c);
    check(fl.crossAxisCount == 4 && near(fl.childCrossAxisExtent, 50),
          "Fixed：childCross = (215 - 3*5) / 4");
    check(near(fl.childMainAxisExtent, 25) && near(fl.mainAxisStride, 35) &&
              near(fl.crossAxisStride, 55),
          "Fixed：aspect 2.0 → childMain 25；两个 stride 各加一份 spacing");
    check(!fl.reverseCrossAxis, "Down 滚动 → 交叉轴不反");

    SliverGridDelegateWithFixedCrossAxisCount ext(4, 10.0, 5.0, 2.0, std::optional<double>(40.0));
    const SliverGridLayout el = ext.getLayout(c);
    check(near(el.childMainAxisExtent, 40) && near(el.mainAxisStride, 50),
          "mainAxisExtent 指定后 aspect 让位（行高 40 + 间距 10）");

    SliverGridDelegateWithMaxCrossAxisExtent m(100.0);
    const SliverGridLayout ml250 = m.getLayout(vc(0, 400, 250, 400, 0, 650));
    check(ml250.crossAxisCount == 3 && near(ml250.childCrossAxisExtent, 250.0 / 3.0),
          "Max(100) 在 250 宽：3 列（(250+100)~/100）");
    const SliverGridLayout ml200 = m.getLayout(vc(0, 400, 200, 400, 0, 650));
    check(ml200.crossAxisCount == 3 && near(ml200.childCrossAxisExtent, 200.0 / 3.0),
          "Max(100) 在 200 宽：仍 3 列、每列 66.7");
    const SliverGridLayout ml300 = m.getLayout(vc(0, 400, 300, 400, 0, 650));
    check(ml300.crossAxisCount == 4, "Max(100) 在 300 宽：正好 4 列");

    SliverConstraints leftCross = vc(0, 400, 300, 400, 0, 650);
    leftCross.crossAxisDirection = AxisDirection::Left;
    check(f.getLayout(leftCross).reverseCrossAxis, "交叉轴为 Left → reverseCrossAxis 置真");

    check(!f.shouldRelayout(SliverGridDelegateWithFixedCrossAxisCount(4, 10.0, 5.0, 2.0)),
          "同族同参：不必重排");
    check(f.shouldRelayout(SliverGridDelegateWithFixedCrossAxisCount(5, 10.0, 5.0, 2.0)),
          "列数变了：要重排");
    check(f.shouldRelayout(m), "跨族：一律重排（上游换 runtimeType 即重排）");
    check(!m.shouldRelayout(SliverGridDelegateWithMaxCrossAxisExtent(100.0)),
          "Max 同参不重排");

    check(!f.validateMessage() && !m.validateMessage(), "合法参数无告警");
    check(SliverGridDelegateWithFixedCrossAxisCount(0).validateMessage().has_value() &&
              SliverGridDelegateWithFixedCrossAxisCount(4, -1.0).validateMessage().has_value() &&
              SliverGridDelegateWithFixedCrossAxisCount(4, 0.0, 0.0, 0.0)
                  .validateMessage()
                  .has_value() &&
              SliverGridDelegateWithFixedCrossAxisCount(4, 0.0, 0.0, 1.0, std::optional<double>(0.0))
                  .validateMessage()
                  .has_value() &&
              SliverGridDelegateWithMaxCrossAxisExtent(-5.0).validateMessage().has_value(),
          "validateMessage 覆盖上游 debugAssertIsValid 的四类非法参数");
}

// ── 3. RenderSliverGrid：无托管人的 eager 走法 ──────────────────────────────

void testEagerGrid() {
    std::printf("[sliver-grid] RenderSliverGrid（eager）\n");

    std::vector<std::unique_ptr<TestBox>> boxes;
    RenderSliverGrid grid(nullptr, fixed4());
    for (int i = 0; i < 12; ++i) {
        boxes.push_back(std::make_unique<TestBox>(300, 300));
        grid.insertChild(boxes.back().get(), i ? boxes[i - 1].get() : nullptr);
    }
    check(grid.totalChildren() == 12 && grid.childCount() == 12,
          "无托管人时总项数按在树数量算");

    // 300 宽 / 4 列 → 每格 75 宽；mainAxisExtent 100 → 12 项 3 行 = 300 长。
    grid.layout(vc(0, 400, 300, 400, 0, 650), true);
    check(near(grid.scrollExtent(), 300) && near(grid.maxPaintExtent(), 300),
          "内容长度 = 最后一行尾部 3×100");
    check(near(grid.paintExtent(), 300) && near(grid.cacheExtent(), 300),
          "内容短于绘制/缓存窗口 → 两者都取内容长度");
    check(grid.visible(), "有绘制长度即可见");

    bool allTiles = true;
    for (RenderBox* b : boxesOf(&grid))
        allTiles = allTiles && b->size() == Size(75, 100);
    check(allTiles, "瓦片走 delegate 给的紧约束 75×100（不是子节点理想的 300×300）");
    check(near(gridMainExtentOf(boxes[0].get()), 100) &&
              near(gridCrossOffsetOf(boxes[0].get()), 0),
          "parentData 记下主轴尺寸与交叉轴偏移");

    const Offset at5 = childPaintOffset(boxes[5].get());
    check(near(at5.dx, 75) && near(at5.dy, 100), "序号 5 = 第 2 行第 2 列 → 绘制偏移 (75,100)");
    const Offset at11 = childPaintOffset(boxes[11].get());
    check(near(at11.dx, 225) && near(at11.dy, 200), "序号 11 = 末行末列 → (225,200)");
    check(near(grid.childCrossAxisPosition(boxes[11].get()), 225) &&
              near(grid.childScrollOffset(boxes[8].get()), 200),
          "交叉轴位置 / 滚动偏移取自同一份 parentData");
    check(near(grid.paintExtentOf(boxes[3].get()), 100), "paintExtentOf 读瓦片主轴尺寸");

    // 滚动：eager 走法不回收存量（sliver.h 偏离 3），只把整片网格往上挪。
    grid.layout(vc(100, 300, 300, 400, 0, 300), true);
    check(grid.childCount() == 12 && indicesOf(&grid) == range(0, 11),
          "eager：滚动不掐头去尾");
    const Offset at0 = childPaintOffset(boxes[0].get());
    const Offset at9 = childPaintOffset(boxes[9].get());
    check(near(at0.dy, -100), "滚 100：序号 0 顶出上沿");
    check(near(at9.dx, 75) && near(at9.dy, 100), "序号 9 = 第 3 行第 2 列 → (75,100)");

    // 水平轴：主轴是宽、交叉轴是高，约束换算跟着翻。
    std::vector<std::unique_ptr<TestBox>> hbox;
    RenderSliverGrid hgrid(nullptr, cui::make_shared<
                          SliverGridDelegateWithFixedCrossAxisCount>(
                              2, 0.0, 0.0, 1.0, std::optional<double>(100.0)));
    for (int i = 0; i < 4; ++i) {
        hbox.push_back(std::make_unique<TestBox>(300, 300));
        hgrid.insertChild(hbox.back().get(), i ? hbox[i - 1].get() : nullptr);
    }
    // 300 宽视口、400 高即交叉轴：2 列 → 每格主轴 100、交叉轴 200。
    hgrid.layout(vc(0, 300, 400, 300, 0, 300, AxisDirection::Right), true);
    check(hbox[0]->size() == Size(100, 200), "Right 轴：紧约束换成 (宽=主轴, 高=交叉轴)");
    const Offset h2 = childPaintOffset(hbox[2].get());
    check(near(h2.dx, 100) && near(h2.dy, 0), "水平：序号 2 = 第 2 列首行 → (100,0)");
    const Offset h3 = childPaintOffset(hbox[3].get());
    check(near(h3.dx, 100) && near(h3.dy, 200), "水平：序号 3 同列下行 → (100,200)");
    check(near(hgrid.scrollExtent(), 200), "水平：2 列 × 100 = 200");
}

// ── 4. SliverGrid.builder：懒加载窗口 ───────────────────────────────────────

void testGridLazyWindow() {
    std::printf("[sliver-grid] SliverGrid.builder 窗口与复用\n");

    ScrollController ctrl;
    auto calls = cui::make_shared<int>(0);
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    cfg->slivers = {SliverGrid::builder(
        fixed4(),
        [calls](BuildContext&, int) {
            ++*calls;
            return cell(100);
        },
        100)};

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* grid = h.find<RenderSliverGrid>();
    if (!grid) {
        check(false, "SliverGrid 渲染对象存在");
        return;
    }
    auto* el = elementOf(grid);
    check(el && el == grid->childManager(), "托管人由 SliverMultiBoxAdaptorElement 担任");
    if (!el)
        return;

    // 缓存窗口 [0,650) → 行 0..6（第 6 行起点 600 < 650）→ 4 列共 28 项。
    check(indicesOf(grid) == range(0, 27) && *calls == 28,
          "只造缓存窗内的 0..27（28 个瓦片），builder 不被整表调用");
    check((int)el->childElements().size() == 28, "Element 表与渲染树同宽");
    check(grid->totalChildren() == 100, "总项数问 delegate.estimatedChildCount");
    check(near(grid->scrollExtent(), 2500), "内容长度 = 25 行 × 100（精确，不外推）");
    check(near(ctrl.maxScrollExtent(), 2100), "可滚区间 = 2500-400");
    bool tiles = true;
    for (RenderBox* b : boxesOf(grid))
        tiles = tiles && b->size() == Size(75, 100);
    check(tiles, "懒加载路径同样按瓦片夹紧子节点");

    const std::map<int, Element*> before = snapshot(el);
    // 滚 500 → 缓存窗口 [250,1150)：行 2..11 → 序号 8..47。
    ctrl.jumpTo(500);
    h.pump();
    check(indicesOf(grid) == range(8, 47), "滚 500：掐掉 0..7、补上 28..47");
    check(*calls == 48, "重叠区间 8..27 复用，只补造 20 项");
    const std::map<int, Element*> after = snapshot(el);
    bool reused = true;
    for (int i = 8; i <= 27; ++i)
        reused = reused && after.count(i) && after.at(i) == before.at(i);
    check(reused, "重叠序号的 Element 是同一批对象");
    check(near(layoutOffsetOf(grid->firstChild()), 200), "新首子序号 8 → 布局偏移 = 行 2 起点");
    check(near(childPaintOffset(grid->firstChild()).dy, -300),
          "首瓦片绘制偏移 = 200-500（顶到视口上方）");

    // 滚 2000（未越界）：缓存窗口 [1750,2650) → 行 17..26 → 夹到末项 99。
    ctrl.jumpTo(2000);
    h.pump();
    check(indicesOf(grid) == range(68, 99), "接近末尾：窗口掐到最后一项 99");
    check(*calls == 80, "48..67 整段跳过不造；targetLastIndex 夹到 99 后不多问一项");

    // 滚过末尾：视口把偏移夹回 2100，窗口停在末尾 28 项。
    ctrl.jumpTo(3000);
    h.pump();
    check(near(ctrl.offset(), 2100), "越界滚动被夹回可滚末尾");
    check(indicesOf(grid) == range(72, 99), "末尾窗口 72..99（第 18..24 行）");
    check(near(grid->scrollExtent(), 2500) && near(grid->paintExtent(), 400),
          "末尾仍报精确长度、绘制窗口装满");
    check(near(layoutOffsetOf(grid->lastChild()), 2400), "末项序号 99 → 行 24 起点 2400");
    check(near(grid->childScrollOffset(grid->lastChild()), 2400), "末项滚动偏移 = 其布局偏移");
}

// ── 5. 换 gridDelegate ──────────────────────────────────────────────────────

void testGridDelegateSwap() {
    std::printf("[sliver-grid] 换 gridDelegate 后窗口重算\n");

    ScrollController ctrl;
    auto calls = cui::make_shared<int>(0);
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    auto builder = [calls](BuildContext&, int) {
        ++*calls;
        return cell(100);
    };
    cfg->slivers = {SliverGrid::builder(fixed4(), builder, 100)};

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* grid = h.find<RenderSliverGrid>();
    if (!grid) {
        check(false, "换 delegate 前要先有网格");
        return;
    }
    check(indicesOf(grid) == range(0, 27), "4 列首屏 28 项");
    const int beforeCalls = *calls;

    cfg->slivers = {SliverGrid::builder(
        cui::make_shared<SliverGridDelegateWithFixedCrossAxisCount>(
            2, 0.0, 0.0, 1.0, std::optional<double>(100.0)),
        builder, 100)};
    probeState(h.root)->rebuildNow();
    h.pump();

    // 2 列 → 每格 150 宽；行窗口不变仍是 0..6 → 序号 0..13。
    check(indicesOf(grid) == range(0, 13), "换 2 列：窗口按新列数重算成 0..13");
    check(grid->childCount() == 14 && (int)elementOf(grid)->childElements().size() == 14,
          "窗口外的 14..27 随渲染对象一起卸载");
    bool wide = true;
    for (RenderBox* b : boxesOf(grid))
        wide = wide && b->size() == Size(150, 100);
    check(wide, "同序号 Element 原地改尺寸：瓦片 150×100");
    check(near(gridCrossOffsetOf(grid->lastChild()), 150), "奇数序号落到第 2 列 → 交叉轴偏移 150");
    check(near(grid->scrollExtent(), 5000), "内容长度跟着列数变：100 项 / 2 列 = 50 行 × 100");
    check(near(ctrl.maxScrollExtent(), 4600), "可滚区间 = 5000-400");
    // super.update 里的 _performRebuild 已经按新 delegate 重扫一轮存量子节点，
    // delegate 变化后再补一次 —— 上游同款两遍，故 28 + 28 + 28。
    check(*calls == beforeCalls + 56, "换 delegate：存量子节点重扫（窗口内，不整表物化）");
}

}  // namespace

int main() {
    std::printf("=== test_sliver_grid ===\n");
    testGridLayoutMath();
    testGridDelegates();
    testEagerGrid();
    testGridLazyWindow();
    testGridDelegateSwap();
    std::printf("%s: %d 个断言失败\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
