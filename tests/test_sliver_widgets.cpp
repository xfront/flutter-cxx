/// @file test_sliver_widgets.cpp
/// @brief Sliver 控件层测试：Widget/Element 三层接上 sliver 渲染协议（headless，不 flushPaint）。
///
/// 覆盖：
///   1. CustomScrollView 接线：约束下发（方向/交叉轴/前序长度）、几何回写
///      ScrollController（内容区间 = ΣscrollExtent − 视口）。
///   2. jumpTo → setState → 视口重排：子 sliver 的 scrollOffset / paintExtent /
///      绘制偏移随滚动变化，边界 clamp。
///   3. 命中测试穿透：滚动后的行仍可点，链尾是对应的 DecoratedBox。
///   4. SliverPadding：交叉轴内缩、子约束改写、绘制偏移。
///   5. SliverList 变高：layoutOffset 累加、绘制偏移。
///   6. SliverFixedExtentList：固定行高 + paintsChild 窗口剔除。
///   7. 子节点 reconcile：换序 / 增删后渲染对象顺序跟随控件顺序。
///   8. 卸载：ScrollableState 的 controller 监听被注销（jumpTo 不再排建）。
///   9. 水平轴 + reverse（轴朝向 Up）。
///  10. 独立挂载 Viewport（不经 Scrollable）。

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/key.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_scroll.h"
#include "cui/rendering/render_view.h"
#include "cui/rendering/sliver.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
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

/// 测试用宿主：BuildOwner + RenderView + PipelineOwner（headless 帧泵，不刷绘制）。
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

    /// 探针换掉内层子树后根渲染对象仍是同一个（外层 SizedBox 固定），
    /// 这里只在首次挂载时将根挂到 RenderView。
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

    /// 帧末：卸载 inactive 元素（上游的 finalizeTree）。
    void frame() {
        pump();
        owner.finalizeTree();
    }

    /// 按树序收集某类型的渲染对象。
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

/// 探针：测试直接改配置 + setState，观察控件树的重排。
struct Cfg {
    ScrollController* controller = nullptr;
    std::vector<SharedPtr<Widget>> slivers;
    Axis scrollDirection = Axis::Vertical;
    bool reverse = false;
    bool standaloneViewport = false;  ///< 直接挂 Viewport（不经 Scrollable）
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
    SharedPtr<Widget> body;
    if (c->slivers.empty()) {
        body = cui::make_shared<SizedBox>(10, 10);
    } else if (c->standaloneViewport) {
        body = cui::make_shared<Viewport>(c->controller,
                                                axisDirectionFromAxis(c->scrollDirection,
                                                                      c->reverse),
                                                AxisDirection::Right, 250.0, c->slivers);
    } else {
        body = cui::make_shared<CustomScrollView>(c->slivers, c->scrollDirection, c->reverse,
                                                  c->controller);
    }
    // 外层保持同一种控件：根渲染对象不随子树类型变化易主（RenderView 只挂一次）。
    return cui::make_shared<SizedBox>(300, 400, std::move(body));
}

ProbeState* probeState(Element* root) {
    auto* se = dynamic_cast<StatefulElement*>(root);
    return se ? dynamic_cast<ProbeState*>(se->state()) : nullptr;
}

/// 一行内容：SizedBox 定高 + DecoratedBox（本仓约定：只有覆写 hitTestSelf 的
/// 节点可命中，DecoratedBox 是其一）。
SharedPtr<Widget> row(double height, KeyPtr key = nullptr) {
    BoxDecoration deco;
    deco.color = Color::fromRGBA(40, 90, 160, 255);
    return cui::make_shared<SizedBox>(300, height, cui::make_shared<DecoratedBox>(deco),
                                      std::move(key));
}

/// 一个包盒子内容的 sliver。
SharedPtr<Widget> adapterRow(double height, KeyPtr key = nullptr) {
    return cui::make_shared<SliverToBoxAdapter>(row(height, std::move(key)));
}

/// 渲染对象相对父节点的绘制/布局偏移（sliver 容器把 paintOffset 同步写进
/// 基类 offset 槽位，见 rendering/sliver.h 文件头偏离 2）。
Offset parentOffset(RenderObject* ro) {
    return ro && ro->parentData() ? ro->parentData()->offset : Offset{};
}

RenderBox* boxOf(RenderObject* sliver) {
    auto* adapter = dynamic_cast<RenderSliverSingleBoxAdapter*>(sliver);
    return adapter ? adapter->child() : nullptr;
}

/// 沿唯一子节点下行到叶子渲染对象（行内容：SizedBox → DecoratedBox）。
RenderObject* onlyChild(RenderObject* ro) {
    RenderObject* out = nullptr;
    bool first = true;
    if (ro) {
        ro->visitChildren([&](AbstractNode* n) {
            if (first) {
                out = static_cast<RenderObject*>(n);
                first = false;
            }
        });
    }
    return out;
}

RenderObject* leafOf(RenderObject* ro) {
    while (RenderObject* c = onlyChild(ro))
        ro = c;
    return ro;
}

std::vector<RenderObject*> childrenOf(RenderViewport* vp) {
    std::vector<RenderObject*> out;
    for (RenderObject* c = vp->firstChild(); c; c = vp->childAfter(c))
        out.push_back(c);
    return out;
}

std::vector<RenderObject*> childrenOf(RenderSliver* sl) {
    std::vector<RenderObject*> out;
    for (RenderObject* c = sl->firstChild(); c; c = sl->childAfter(c))
        out.push_back(c);
    return out;
}

// ── 1/2/3. CustomScrollView 接线 + 滚动 + 命中 ──────────────────────────────
void testScrollViewWiringAndScroll() {
    std::printf("[sliver-widgets] CustomScrollView 接线与滚动\n");

    ScrollController ctrl;
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    cfg->slivers = {adapterRow(250), adapterRow(250)};

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* vp = h.find<RenderViewport>();
    check(vp != nullptr && vp->childCount() == 2, "视口挂上两个子 sliver");
    if (!vp)
        return;
    check(vp->size() == Size(300, 400), "视口尺寸 = 窗口紧约束");
    check(vp->axis() == Axis::Vertical && vp->axisDirection() == AxisDirection::Down,
          "默认竖轴向下");
    check(near(vp->cacheExtent(), 250), "默认 cacheExtent 250");

    auto kids = childrenOf(vp);
    auto* s0 = static_cast<RenderSliver*>(kids[0]);
    auto* s1 = static_cast<RenderSliver*>(kids[1]);
    const SliverConstraints& c0 = s0->sliverConstraints();
    check(c0.axisDirection == AxisDirection::Down &&
              c0.crossAxisDirection == AxisDirection::Right &&
              c0.growthDirection == GrowthDirection::Forward,
          "约束方向 Down / Right / Forward");
    check(near(c0.crossAxisExtent, 300) && near(c0.viewportMainAxisExtent, 400),
          "交叉轴 300、主轴视口 400");
    check(near(c0.scrollOffset, 0) && near(c0.precedingScrollExtent, 0) &&
              near(c0.remainingPaintExtent, 400),
          "首个 sliver 从零滚动点开始");
    check(c0.isNormalized(), "下发的约束已规范化");
    check(near(s0->cacheExtent(), 250) && near(c0.remainingCacheExtent, 650),
          "缓存窗口 = 视口 400 + cacheExtent 250");
    check(near(s0->scrollExtent(), 250) && near(s1->scrollExtent(), 250),
          "每段 scrollExtent = 子盒高度 250");
    const SliverConstraints& c1 = s1->sliverConstraints();
    check(near(c1.precedingScrollExtent, 250) && near(c1.remainingPaintExtent, 150),
          "第二段记录前序 250、剩余绘制 150");
    check(near(ctrl.maxScrollExtent(), 100) && near(ctrl.viewportExtent(), 400),
          "controller 收到区间 500-400=100、视口 400");
    check(near(vp->contentScrollExtent(), 500), "内容总长（未减视口）500");
    check(vp->hasVisualOverflow(), "内容超出视口 → 有视觉溢出");
    check(parentOffset(kids[0]).dy == 0 && near(parentOffset(kids[1]).dy, 250),
          "子 sliver 绘制偏移 0 / 250");

    // 滚动：监听 → setState → 新的 offset 下发。
    ctrl.jumpTo(60);
    check(h.owner.dirtyCount() >= 1, "挂载时 jumpTo 触发 setState 排建");
    h.pump();
    check(near(vp->offset(), 60), "视口偏移跟到 60");
    check(near(s0->sliverConstraints().scrollOffset, 60) && near(s0->paintExtent(), 190),
          "第一段滚掉 60 → 可见 190");
    check(near(s1->sliverConstraints().scrollOffset, 0) && near(s1->paintExtent(), 210),
          "第二段仍在原位、可见 210");
    check(near(parentOffset(kids[1]).dy, 190), "第二段绘制偏移跟到 190");
    RenderBox* box0 = boxOf(kids[0]);
    RenderBox* box1 = boxOf(kids[1]);
    check(box0 && near(parentOffset(box0).dy, -60), "第一行盒子内偏移 -60");
    check(box1 && near(parentOffset(box1).dy, 0), "第二行盒子内偏移 0");

    // 边界 clamp（controller 侧与视口侧各一次）。
    ctrl.jumpTo(5000);
    h.pump();
    check(near(ctrl.offset(), 100) && near(vp->offset(), 100), "jumpTo 越上界夹到 100");
    ctrl.jumpTo(-50);
    h.pump();
    check(near(ctrl.offset(), 0) && near(vp->offset(), 0), "jumpTo 负值夹回 0");

    // 命中测试：滚动后 (50,100) 落在第一行、(50,250) 落在第二行。
    ctrl.jumpTo(60);
    h.pump();
    RenderObject* leaf0 = leafOf(box0);
    RenderObject* leaf1 = leafOf(box1);
    std::vector<RenderObject*> path;
    check(h.rootRO->hitTest(Offset{50, 100}, path) && path.back() == leaf0,
          "滚动 60 后 (50,100) 命中第一行（链尾 = 其 DecoratedBox）");
    path.clear();
    check(h.rootRO->hitTest(Offset{50, 250}, path) && path.back() == leaf1,
          "滚动 60 后 (50,250) 命中第二行");
    path.clear();
    check(!h.rootRO->hitTest(Offset{350, 100}, path), "交叉轴越界点不命中");
}

// ── 4. SliverPadding ────────────────────────────────────────────────────────
void testSliverPaddingWidget() {
    std::printf("[sliver-widgets] SliverPadding\n");

    ScrollController ctrl;
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    EdgeInsets pad;
    pad.left = 5;
    pad.top = 10;
    pad.right = 15;
    pad.bottom = 20;
    cfg->slivers = {cui::make_shared<SliverPadding>(pad, adapterRow(500))};

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* vp = h.find<RenderViewport>();
    check(vp != nullptr && vp->childCount() == 1, "视口只有一个 padding sliver");
    if (!vp)
        return;
    auto* pad0 = static_cast<RenderSliverPadding*>(vp->firstChild());
    check(near(pad0->scrollExtent(), 530), "scrollExtent = 500 + 上下内边距 30");
    const SliverConstraints& pc = pad0->child()->sliverConstraints();
    check(near(pc.crossAxisExtent, 280), "子 sliver 交叉轴内缩到 300-20");
    check(near(pc.scrollOffset, 0) && near(pc.remainingPaintExtent, 390),
          "未滚过 top 内边距时子 scrollOffset 仍为 0、剩余绘制 390");
    check(near(pad0->childScrollOffset(pad0->child()), 10), "childScrollOffset = beforePadding");
    check(parentOffset(pad0->child()) == Offset{5, 10}, "子 sliver 绘制偏移 (5,10)");
    RenderBox* box = boxOf(pad0->child());
    check(box && near(box->size().width, 280) && near(box->size().height, 500),
          "内容盒被交叉轴紧到 280 宽");
    check(near(ctrl.maxScrollExtent(), 130), "内容 530 → 可滚 130");

    // 滚过前导内边距：子 scrollOffset 只扣得到的那 10，绘制偏移里 top 归零。
    ctrl.jumpTo(30);
    h.pump();
    check(near(pc.scrollOffset, 20) && near(pc.remainingPaintExtent, 400),
          "滚过 top 后子 scrollOffset = 30-10、剩余绘制回到 400");
    check(parentOffset(pad0->child()) == Offset{5, 0}, "前导内边距滚掉后子绘制偏移 (5,0)");
}

// ── 5. SliverList 变高 ──────────────────────────────────────────────────────
void testSliverListWidget() {
    std::printf("[sliver-widgets] SliverList（变高子节点）\n");

    ScrollController ctrl;
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    cfg->slivers = {cui::make_shared<SliverList>(
        std::vector<SharedPtr<Widget>>{row(100), row(150), row(200)})};

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* vp = h.find<RenderViewport>();
    if (!vp)
        return;
    auto* list = static_cast<RenderSliverList*>(vp->firstChild());
    check(list && list->childCount() == 3, "SliverList 三个子盒子");
    if (!list)
        return;
    check(near(list->scrollExtent(), 450), "scrollExtent = 100+150+200");
    check(near(ctrl.maxScrollExtent(), 50), "内容 450 → 可滚 50");
    check(list->sliverConstraints().axis() == Axis::Vertical, "变高列表按主轴无界布局");

    std::vector<RenderObject*> kids = childrenOf(list);
    const double expect[3] = {0, 100, 250};
    for (int i = 0; i < 3; ++i) {
        auto* pd = dynamic_cast<SliverMultiBoxAdaptorParentData*>(kids[i]->parentData());
        check(pd && pd->layoutOffset && near(*pd->layoutOffset, expect[i]),
              "第 " + std::to_string(i) + " 项 layoutOffset = " + std::to_string(expect[i]));
        check(near(parentOffset(kids[i]).dy, expect[i]), "第 " + std::to_string(i) + " 项绘制偏移随累加");
    }

    ctrl.jumpTo(40);
    h.pump();
    for (int i = 0; i < 3; ++i)
        check(near(parentOffset(kids[i]).dy, expect[i] - 40),
              "滚动 40 后第 " + std::to_string(i) + " 项绘制偏移减 40");
    check(near(list->paintExtent(), 400), "整段可见长度夹到视口 400");
}

// ── 6. SliverFixedExtentList ────────────────────────────────────────────────
void testSliverFixedExtentWidget() {
    std::printf("[sliver-widgets] SliverFixedExtentList\n");

    ScrollController ctrl;
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    std::vector<SharedPtr<Widget>> items;
    for (int i = 0; i < 12; ++i)
        items.push_back(row(999));  // 行高由 itemExtent 强制，盒子理想尺寸被顶掉
    cfg->slivers = {cui::make_shared<SliverFixedExtentList>(50.0, std::move(items))};

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* vp = h.find<RenderViewport>();
    if (!vp)
        return;
    auto* list = static_cast<RenderSliverFixedExtentList*>(vp->firstChild());
    check(list && list->childCount() == 12, "12 行全部挂上（eager 版）");
    if (!list)
        return;
    check(near(list->itemExtent(), 50), "itemExtent 50 下发到渲染对象");
    check(near(list->scrollExtent(), 600), "scrollExtent = 12×50");
    check(near(ctrl.maxScrollExtent(), 200), "可滚区间 600-400=200");

    std::vector<RenderObject*> kids = childrenOf(list);
    check(kids.front() && static_cast<RenderBox*>(kids.front())->size().height == 50,
          "行高被紧约束改写成 50（不是盒子的理想 999）");

    ctrl.jumpTo(120);
    h.pump();
    check(near(parentOffset(kids[3]).dy, 30), "第 3 行绘制偏移 = 3×50-120");
    check(!list->paintsChild(kids[0]) && list->paintsChild(kids[3]),
          "滚出窗口上方的第 0 行被 paintsChild 剔除");
    check(near(list->paintExtent(), 400), "可见长度仍占满视口");
}

// ── 7/8. reconcile 与卸载 ───────────────────────────────────────────────────
void testReconcileAndUnmount() {
    std::printf("[sliver-widgets] 子树重排与卸载\n");

    ScrollController ctrl;
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    KeyPtr keyA = cui::make_shared<ValueKey<std::string>>("A");
    KeyPtr keyB = cui::make_shared<ValueKey<std::string>>("B");

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* st = probeState(h.root);
    auto* vp = h.find<RenderViewport>();
    check(vp == nullptr, "空 slivers 时不建视口");
    if (!st)
        return;

    cfg->slivers = {adapterRow(300, keyA), adapterRow(200, keyB)};
    st->rebuildNow();
    h.frame();
    vp = h.find<RenderViewport>();
    if (!vp) {
        check(false, "重排测试需要视口");
        return;
    }
    check(vp->childCount() == 2 && near(boxOf(vp->firstChild())->size().height, 300),
          "插入两个 sliver，顺序 A(300) → B(200)");
    check(near(ctrl.maxScrollExtent(), 100), "内容 500 → 可滚 100");

    cfg->slivers = {adapterRow(200, keyB), adapterRow(300, keyA)};
    st->rebuildNow();
    h.frame();
    check(vp->childCount() == 2 && near(boxOf(vp->firstChild())->size().height, 200),
          "带 Key 换序后渲染对象顺序跟随（B 在前）");

    cfg->slivers = {adapterRow(200, keyB)};
    st->rebuildNow();
    h.frame();
    check(vp->childCount() == 1 && near(boxOf(vp->firstChild())->size().height, 200),
          "删除 A 后只剩 B");
    check(near(ctrl.maxScrollExtent(), 0), "内容缩到 200 → 区间回写 0");

    cfg->slivers = {adapterRow(300, keyA), adapterRow(200, keyB)};
    st->rebuildNow();
    h.frame();
    check(vp->childCount() == 2 && near(boxOf(vp->lastChild())->size().height, 200),
          "重新加回 A，末子仍是 B");

    // 卸载整棵滚动子树：ScrollableState::dispose 必须注销 controller 监听。
    cfg->slivers.clear();
    st->rebuildNow();
    h.frame();
    check(h.find<RenderViewport>() == nullptr, "卸载后视口渲染对象不在树上");
    h.owner.flushBuildScope();
    const int buildsBefore = cfg->builds;
    ctrl.jumpTo(70);  // 监听若未注销 → 已销毁的 State 再次排建
    check(h.owner.dirtyCount() == 0, "jumpTo 不再排出任何待重建元素（监听已注销）");
    h.pump();
    check(cfg->builds == buildsBefore, "探针控件未被滚动通知牵连重建");
}

// ── 9/10. 轴朝向与独立挂载 ──────────────────────────────────────────────────
void testAxisDirectionsAndStandaloneViewport() {
    std::printf("[sliver-widgets] 轴朝向 / 独立 Viewport\n");

    // 水平滚动。
    {
        ScrollController ctrl;
        auto cfg = cui::make_shared<Cfg>();
        cfg->controller = &ctrl;
        cfg->scrollDirection = Axis::Horizontal;
        BoxDecoration deco;
        deco.color = Color::fromRGBA(40, 90, 160, 255);
        auto hRow = [deco] {
            return cui::make_shared<SliverToBoxAdapter>(
                cui::make_shared<SizedBox>(250, 400, cui::make_shared<DecoratedBox>(deco)));
        };        cfg->slivers = {hRow(), hRow()};
        Harness h;
        h.attach(cui::make_shared<Probe>(cfg), 300, 400);
        auto* vp = h.find<RenderViewport>();
        if (vp) {
            check(vp->axis() == Axis::Horizontal && vp->axisDirection() == AxisDirection::Right,
                  "scrollDirection=horizontal → 轴朝右");
            auto kids = childrenOf(vp);
            const SliverConstraints& c0 = static_cast<RenderSliver*>(kids[0])->sliverConstraints();
            check(c0.axis() == Axis::Horizontal && near(c0.crossAxisExtent, 400),
                  "子约束交叉轴（纵向）紧到 400");
            check(boxOf(kids[0]) && boxOf(kids[0])->size() == Size(250, 400),
                  "行盒按交叉轴撑满（250×400）");
            check(near(ctrl.maxScrollExtent(), 200), "横向内容 500-300=200 可滚");
            check(near(parentOffset(kids[1]).dx, 250) &&
                      near(parentOffset(kids[0]).dy, 0),
                  "第二段绘制偏移落在 dx 上");
        } else {
            check(false, "横向视口存在");
        }
    }

    // 反向（Up）：内容贴底向上堆叠。
    {
        ScrollController ctrl;
        auto cfg = cui::make_shared<Cfg>();
        cfg->controller = &ctrl;
        cfg->reverse = true;
        cfg->slivers = {adapterRow(250), adapterRow(250)};
        Harness h;
        h.attach(cui::make_shared<Probe>(cfg), 300, 400);
        auto* vp = h.find<RenderViewport>();
        if (vp) {
            check(vp->axisDirection() == AxisDirection::Up, "reverse 把轴朝向翻成 Up");
            auto kids = childrenOf(vp);
            check(near(parentOffset(kids[0]).dy, 150), "第一段贴底（400-250）");
            check(near(ctrl.maxScrollExtent(), 100), "反向同样得到 500-400 的区间");
            // 已知偏离（rendering/sliver.h 文件头偏离 4）：非居中视口对 Up/Left
            // 只用 paintExtent 反算绘制偏移，第二段起会与第一段重叠；
            // Down / Right 路径精确，故这里只验轴向接线与首段贴底。
        } else {
            check(false, "反向视口存在");
        }
    }

    // 独立挂载：Viewport 直接作为根，不经 Scrollable。
    {
        ScrollController ctrl;
        auto cfg = cui::make_shared<Cfg>();
        cfg->controller = &ctrl;
        cfg->standaloneViewport = true;
        cfg->slivers = {adapterRow(120)};
        Harness h;
        h.attach(cui::make_shared<Probe>(cfg), 300, 400);
        auto* vp = h.find<RenderViewport>();
        if (vp) {
            check(vp->childCount() == 1, "独立视口挂载，只有一个子 sliver");
            check(near(vp->contentScrollExtent(), 120) && near(vp->maxScrollExtent(), 0),
                  "内容 120：裸累加 120、可滚区间 0");
            check(near(ctrl.viewportExtent(), 400) && near(ctrl.maxScrollExtent(), 0),
                  "无 Scrollable 也照样把区间回写 controller");
        } else {
            check(false, "独立视口挂载");
        }
    }
}

}  // namespace

int main() {
    std::printf("=== Sliver 控件层 ===\n");
    testScrollViewWiringAndScroll();
    testSliverPaddingWidget();
    testSliverListWidget();
    testSliverFixedExtentWidget();
    testReconcileAndUnmount();
    testAxisDirectionsAndStandaloneViewport();
    std::printf(g_failures ? "\nFAIL: %d\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
