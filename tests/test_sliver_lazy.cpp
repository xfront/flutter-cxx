/// @file test_sliver_lazy.cpp
/// @brief Sliver 懒加载控件层测试：真托管人 = SliverMultiBoxAdaptorElement。
///
/// 与 tests/test_sliver.cpp 的分工：那边用假托管人只验渲染层窗口算法；这里经
/// Widget/Element/RenderObject 三树，验 delegate 契约与 Element 生命周期
/// （headless 帧泵，不 flushPaint）。
///
/// 覆盖：
///   1. SliverFixedExtentList.builder：只造窗口内的子节点、滚动时窗口内
///      Element 原样复用、窗口外 Element 随渲染对象一起卸载。
///   2. SliverList.builder 变高：逐项累加补造、大幅滚动整段换血、
///      回滚时前导重造（keepAlive 桶未移植的代价）。
///   3. SliverChildListDelegate.shouldRebuild：内容未变不重扫，内容变了整表重建。
///   4. findIndexByKey：带 Key 换序后 Element 随新序号搬移、渲染顺序跟随控件顺序。
///   5. 无限/未报数的 builder：长度先估成无穷、二分找末尾，定高列表尾部
///      拿到空项后把长度收回精确值（视口随之把偏移夹回可滚末尾）。
///   6. 卸载：整个 sliver 摘掉时子 Element 与渲染对象一并离开树。

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

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/key.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
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

/// 探针：测试改配置 + setState，观察懒加载子节点表随重建/滚动怎么变。
struct Cfg {
    ScrollController* controller = nullptr;
    std::vector<SharedPtr<Widget>> slivers;
    /// 非空则每次 build 现造一个 sliver（新 Widget 实例，走 update 分支）。
    std::function<SharedPtr<Widget>()> makeSliver;
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
    std::vector<SharedPtr<Widget>> slivers = c->slivers;
    if (c->makeSliver)
        slivers = {c->makeSliver()};
    auto body = cui::make_shared<CustomScrollView>(slivers, Axis::Vertical, false, c->controller);
    // 外层固定：根渲染对象不随子树类型易主（RenderView 只挂一次）。
    return cui::make_shared<SizedBox>(300, 400, std::move(body));
}

ProbeState* probeState(Element* root) {
    auto* se = dynamic_cast<StatefulElement*>(root);
    return se ? dynamic_cast<ProbeState*>(se->state()) : nullptr;
}

/// 一行内容：SizedBox 定高（变高列表靠它区分项）。
SharedPtr<Widget> row(double height, KeyPtr key = nullptr) {
    BoxDecoration deco;
    deco.color = Color::fromRGBA(40, 90, 160, 255);
    return cui::make_shared<SizedBox>(300, height, cui::make_shared<DecoratedBox>(deco),
                                      std::move(key));
}

// ── 懒加载观察工具 ──────────────────────────────────────────────────────────

SliverMultiBoxAdaptorElement* elementOf(RenderSliverMultiBoxAdaptor* ro) {
    return ro ? dynamic_cast<SliverMultiBoxAdaptorElement*>(ro->childManager()) : nullptr;
}

/// 渲染树上的子节点序号（按渲染顺序）。
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

/// Element 表快照：序号 → Element 指针（比身份，不比内容）。
std::map<int, Element*> snapshot(SliverMultiBoxAdaptorElement* el) {
    std::map<int, Element*> out;
    if (!el)
        return out;
    for (const auto& kv : el->childElements())
        out[kv.first] = kv.second.get();
    return out;
}

KeyPtr strKey(const std::string& s) { return cui::make_shared<ValueKey<std::string>>(s); }

/// 子 Element 挂的 Widget 上的 ValueKey<std::string> 值（无 Key 返回空串）。
std::string keyOf(const std::unique_ptr<Element>& element) {
    Widget* w = element ? element->widget() : nullptr;
    const KeyPtr key = w ? w->key() : nullptr;
    auto* vk = key ? dynamic_cast<const ValueKey<std::string>*>(key.get()) : nullptr;
    return vk ? vk->value() : std::string();
}

/// Key → Element 指针（跨重排追同一份状态）。
std::map<std::string, Element*> snapshotByKey(SliverMultiBoxAdaptorElement* el) {
    std::map<std::string, Element*> out;
    for (const auto& kv : el->childElements()) {
        const std::string key = keyOf(kv.second);
        if (!key.empty())
            out[key] = kv.second.get();
    }
    return out;
}

// ── 1. 定高懒加载：窗口、复用、换血 ─────────────────────────────────────────

void testFixedExtentLazy() {
    std::printf("[sliver-lazy] SliverFixedExtentList.builder 窗口与复用\n");

    ScrollController ctrl;
    auto calls = cui::make_shared<int>(0);
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    cfg->slivers = {SliverFixedExtentList::builder(
        50.0,
        [calls](BuildContext&, int) {
            ++*calls;
            return row(50);
        },
        1000)};

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* list = h.find<RenderSliverFixedExtentList>();
    if (!list) {
        check(false, "定高懒加载列表渲染对象存在");
        return;
    }
    auto* el = elementOf(list);
    check(el && el == list->childManager(), "托管人由子节点托管 Element 回填");
    if (!el)
        return;

    // 视口 400 + cacheExtent 250 → 缓存窗口 [0,650] → 13 项。
    check(list->childCount() == 13 && *calls == 13, "只造窗口内的 0..12（13 项）");
    check((int)el->childElements().size() == 13, "Element 表与渲染树同宽");
    check(list->totalChildren() == 1000, "总项数问 delegate.estimatedChildCount");
    check(near(list->scrollExtent(), 50000), "定高外推得精确内容 1000×50");
    check(near(ctrl.maxScrollExtent(), 49600), "可滚区间 = 50000-400");
    check(indicesOf(list) == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}),
          "序号连续");
    std::vector<RenderBox*> boxes = boxesOf(list);
    check(boxes.front()->size().height == 50 && boxes.back()->size().height == 50,
          "行高被 itemExtent 夹紧");
    check(list->paintsChild(boxes[7]) && !list->paintsChild(boxes[8]),
          "绘制窗口只盖 0..7，8..12 留在缓存窗口");

    const std::map<int, Element*> before = snapshot(el);
    // 滚 300 → 缓存窗口 [50, 950)：掐掉 0，尾部补 13..18（整段窗口都往后挪）。
    ctrl.jumpTo(300);
    h.pump();
    std::vector<int> shifted;
    for (int i = 1; i <= 18; ++i)
        shifted.push_back(i);
    check(indicesOf(list) == shifted, "滚 300：掐掉 0、补上 13..18");
    check(*calls == 19, "窗口重叠部分复用，只调 13..18 这 6 次 builder");
    bool reused = true;
    const std::map<int, Element*> after = snapshot(el);
    for (int i = 1; i <= 12; ++i)
        reused = reused && after.count(i) && after.at(i) == before.at(i);
    check(reused, "重叠区间的 Element 是同一批对象（状态没被搬一次就没）");
    bool allNew = !after.count(0);
    for (int i = 13; i <= 18; ++i)
        allNew = allNew && after.count(i) && !before.count(i);
    check(allNew, "0 号 Element 已卸载、13..18 全是新造的");

    // 大幅跳跃：整段窗口换血（缓存窗口 [9750, 10650) → 195..212）。
    ctrl.jumpTo(10000);
    h.pump();
    std::vector<int> far;
    for (int i = 195; i <= 212; ++i)
        far.push_back(i);
    check(indicesOf(list) == far, "跳到 10000 → 窗口换成 195..212");
    check(*calls == 19 + 18, "换血只造新窗口的 18 项");
    check(near(list->scrollExtent(), 50000), "内容长度与滚动位置无关");
    check(near(layoutOffsetOf(list->firstChild()), 195 * 50), "首项布局偏移 = 序号×行高");
}

// ── 2. 变高懒加载：累加、整段回收、回滚重造 ─────────────────────────────────

void testVariableListLazy() {
    std::printf("[sliver-lazy] SliverList.builder 变高懒加载\n");

    ScrollController ctrl;
    auto calls = cui::make_shared<int>(0);
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    // 偶序号 100 高、奇序号 60 高。
    cfg->slivers = {SliverList::builder(
        [calls](BuildContext&, int index) {
            ++*calls;
            return row(index % 2 == 0 ? 100 : 60);
        },
        100)};

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* list = h.find<RenderSliverList>();
    if (!list) {
        check(false, "变高懒加载列表渲染对象存在");
        return;
    }
    auto* el = elementOf(list);
    check(el && el == list->childManager(), "托管人由子节点托管 Element 回填");

    // 缓存窗口 [0,650]：累加到 0..8 才盖过（末项尾 740）。
    check(indicesOf(list) == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8}),
          "变高列表补造到 0..8 才覆盖缓存窗口");
    check(el && (int)el->childElements().size() == list->childCount(),
          "Element 表与渲染树同宽");
    check(*calls == 9, "builder 调 9 次");
    const double firstExtent = 740.0 + (740.0 / 9.0) * 91.0;
    check(near(list->scrollExtent(), firstExtent), "未报数时按已物化均值外推内容长度");
    check(near(ctrl.maxScrollExtent(), firstExtent - 400), "controller 收到外推区间");
    check(layoutOffsetOf(list->firstChild()) == 0 &&
              near(layoutOffsetOf(list->lastChild()), 640),
          "逐项布局偏移累加（首 0、末 640）");

    // 缓存窗口 [750, 1650)：首项 9（740..840 与窗口起点相交）、末项 20（1600..1700）。
    ctrl.jumpTo(1000);
    h.pump();
    std::vector<int> mid;
    for (int i = 9; i <= 20; ++i)
        mid.push_back(i);
    check(indicesOf(list) == mid, "滚 1000：0..8 整段回收、补到 9..20");
    check(*calls == 21, "新窗口 12 项全为新造（旧窗口 0..8 一个没剩）");
    const double secondExtent = 1700.0 + (960.0 / 12.0) * 79.0;
    check(near(list->scrollExtent(), secondExtent), "中段外推：窗口内均值 × 剩余项数");
    const std::map<int, Element*> midSnapshot = el ? snapshot(el) : std::map<int, Element*>{};

    // 回滚到 200：缓存窗口 [0, 900) 只需头部补造 8..0，9/10 原位复用。
    ctrl.jumpTo(200);
    h.pump();
    std::vector<int> head;
    for (int i = 0; i <= 10; ++i)
        head.push_back(i);
    check(indicesOf(list) == head, "回滚到 200：头部逐项重造、尾部回收");
    check(*calls == 30, "本仓 keepAlive 桶未移植：回滚重新调用 builder 造 0..8");
    const std::map<int, Element*> afterBack =
        el ? snapshot(el) : std::map<int, Element*>{};
    check(afterBack.count(9) && midSnapshot.count(9) && afterBack.at(9) == midSnapshot.at(9) &&
              afterBack.count(10) && afterBack.at(10) == midSnapshot.at(10),
          "窗口相交的 9/10 直接复用，不经 builder 重造");
    const double headExtent = 900.0 + (900.0 / 11.0) * 89.0;
    check(near(list->scrollExtent(), headExtent), "外推值随当前窗口重算（与首个窗口不同）");
}

// ── 3. SliverChildListDelegate.shouldRebuild ────────────────────────────────

void testListDelegateRebuild() {
    std::printf("[sliver-lazy] SliverChildListDelegate 的 shouldRebuild\n");

    ScrollController ctrl;
    auto items = cui::make_shared<std::vector<SharedPtr<Widget>>>();
    auto rowA = row(100);
    auto rowB = row(150);
    auto rowC = row(200);
    *items = {rowA, rowB};

    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    cfg->makeSliver = [items] {
        return SharedPtr<Widget>(
            new SliverList(cui::make_shared<SliverChildListDelegate>(*items)));
    };

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* list = h.find<RenderSliverList>();
    if (!list) {
        check(false, "列表型 delegate 挂上");
        return;
    }
    auto* el = elementOf(list);
    check(list->childCount() == 2, "两项都挂上");
    check(near(list->scrollExtent(), 250), "内容 = 100+150");

    const std::map<int, Element*> before = snapshot(el);
    const int buildsBefore = cfg->builds;
    // 同样的 children：shouldRebuild 为 false，但仍会走一轮常规 reconcile。
    ProbeState* st = probeState(h.root);
    if (!st) {
        check(false, "探针 State 可用");
        return;
    }
    st->rebuildNow();
    h.pump();
    check(cfg->builds == buildsBefore + 1, "setState 触发一次重建");
    const std::map<int, Element*> sameItems = snapshot(el);
    check(sameItems.size() == 2 && sameItems.at(0) == before.at(0) &&
              sameItems.at(1) == before.at(1),
          "内容未变 → Element 原样复用（同 Widget 实例直接跳过 update）");

    // 追加一项：delegate 内容变化 → 整表重建。
    items->push_back(rowC);
    st->rebuildNow();
    h.pump();
    check(list->childCount() == 3, "children 变 3 → 重扫后挂上三项");
    check(near(list->scrollExtent(), 450) && near(ctrl.maxScrollExtent(), 50),
          "内容 450 → 可滚 50");
    const std::map<int, Element*> afterAdd = snapshot(el);
    check(afterAdd.at(0) == before.at(0) && afterAdd.at(1) == before.at(1),
          "前两项 Element 仍在原位，新项追加在序号 2");
    check(near(layoutOffsetOf(list->lastChild()), 250), "末项布局偏移累加到 250");

    // 删掉中间项：无 Key 时按序号就地复用（第 1 号 Element 换内容）。
    *items = {rowA, rowC};
    st->rebuildNow();
    h.pump();
    check(list->childCount() == 2 && near(list->scrollExtent(), 300),
          "删中间项 → 两项、内容 100+200");
    const std::map<int, Element*> afterRemove = snapshot(el);
    check(afterRemove.size() == 2 && afterRemove.at(1) == before.at(1),
          "无 Key：1 号位 Element 复用、只换 Widget");
    check(near(boxesOf(list)[1]->size().height, 200), "1 号位现在是 200 高的那一项");
}

// ── 4. findIndexByKey：带 Key 换序 ──────────────────────────────────────────

void testKeyedReorder() {
    std::printf("[sliver-lazy] 带 Key 换序与 findIndexByKey\n");

    ScrollController ctrl;
    auto items = cui::make_shared<std::vector<SharedPtr<Widget>>>();
    auto a = row(100, strKey("A"));
    auto b = row(150, strKey("B"));
    auto c = row(200, strKey("C"));
    *items = {a, b, c};

    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    cfg->makeSliver = [items] {
        return SharedPtr<Widget>(
            new SliverList(cui::make_shared<SliverChildListDelegate>(*items)));
    };

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* list = h.find<RenderSliverList>();
    if (!list) {
        check(false, "带 Key 列表挂上");
        return;
    }
    auto* el = elementOf(list);
    check(list->childCount() == 3, "三项入列");
    const std::map<std::string, Element*> before = snapshotByKey(el);
    check(before.size() == 3, "A/B/C 各自占一个 Element");

    *items = {c, a, b};
    probeState(h.root)->rebuildNow();
    h.pump();

    const std::map<std::string, Element*> after = snapshotByKey(el);
    check(after.size() == 3 && after.at("A") == before.at("A") &&
              after.at("B") == before.at("B") && after.at("C") == before.at("C"),
          "换序后同一 Key 复用同一 Element（delegate.findIndexByKey 找到新家）");
    std::vector<RenderBox*> boxes = boxesOf(list);
    check(near(boxes[0]->size().height, 200) && near(boxes[1]->size().height, 100) &&
              near(boxes[2]->size().height, 150),
          "渲染顺序跟新控件顺序（C→A→B）");
    check(indicesOf(list) == std::vector<int>({0, 1, 2}), "序号重新连续");
    check(near(layoutOffsetOf(boxes[0]), 0) && near(layoutOffsetOf(boxes[1]), 200) &&
              near(layoutOffsetOf(boxes[2]), 300),
          "搬来的子节点布局偏移在下一轮布局里重算");
    check(near(list->scrollExtent(), 450), "换序不改内容长度");
    check(keyOf(el->childElements().at(0)) == "C" && keyOf(el->childElements().at(2)) == "B",
          "序号 0 挂 C、序号 2 挂 B（Element 随新序号安家）");
}

// ── 5. 无限 / 未报数 ────────────────────────────────────────────────────────

void testUnknownChildCount() {
    std::printf("[sliver-lazy] 无限列表与未报数的列表\n");

    // ① 真无限：builder 永远给得出内容。
    {
        ScrollController ctrl;
        auto cfg = cui::make_shared<Cfg>();
        cfg->controller = &ctrl;
        cfg->slivers = {SliverList::builder(
            [](BuildContext&, int index) { return row(index % 2 == 0 ? 100 : 60); })};
        Harness h;
        h.attach(cui::make_shared<Probe>(cfg), 300, 400);
        auto* list = h.find<RenderSliverList>();
        auto* el = elementOf(list);
        check(list && el, "无限 builder 列表挂上");
        if (!list || !el)
            return;
        check(!std::isfinite(list->scrollExtent()), "估不出总项数 → scrollExtent 为无穷");
        check(std::isfinite(list->paintExtent()) && near(list->paintExtent(), 400),
              "无穷内容长度不影响本帧绘制长度");
        check(el->estimateMaxScrollOffset(list->sliverConstraints(), 0, 8, 0, 740) ==
                  std::numeric_limits<double>::infinity(),
              "托管人估算直接给无穷");
        ctrl.jumpTo(300);
        h.pump();
        check(std::isfinite(list->paintExtent()) && list->childCount() > 0,
              "无限列表滚动照旧只维护窗口");
    }

    // ② 有限但不报数：builder 到 40 项返回空。
    {
        ScrollController ctrl;
        auto cfg = cui::make_shared<Cfg>();
        cfg->controller = &ctrl;
        cfg->slivers = {SliverFixedExtentList::builder(
            50.0, [](BuildContext&, int index) -> SharedPtr<Widget> {
                if (index >= 40)
                    return nullptr;
                return row(50);
            })};
        Harness h;
        h.attach(cui::make_shared<Probe>(cfg), 300, 400);
        auto* list = h.find<RenderSliverFixedExtentList>();
        auto* el = elementOf(list);
        check(list && el, "未报数列表挂上");
        if (!list || !el)
            return;
        check(el->childCount() == 40, "开放式二分找出精确项数 40");
        // 上游同款行为：builder 没报 itemCount 时估不出内容长度，长度先给无穷
        // （ListView.builder 少写 itemCount 就能无限滚）；二分得出的 40 只用于
        // 回收判定。一旦尾部抓取真拿到空项，定高列表就把长度收回精确值。
        check(std::isinf(list->scrollExtent()), "未报项数 → 内容长度先估成无穷");
        check(std::isinf(ctrl.maxScrollExtent()), "可滚区间同样无穷");
        check(list->totalChildren() == 40, "总项数经二分得出 40，供回收判定");
        ctrl.jumpTo(1980);
        h.pump();
        std::vector<int> tail;
        for (int i = 27; i <= 39; ++i)
            tail.push_back(i);
        check(indicesOf(list) == tail, "滚到末尾：夹回 1600 后窗口停在 27..39");
        check(near(list->scrollExtent(), 2000) && near(ctrl.offset(), 1600),
              "尾部抓到空项 → 长度收回 40×50，偏移被视口夹回可滚末尾");
        check(list->childCount() == 13, "缓存窗口 13 项，40 号起不再有意义");
        check(list->visible(), "末尾仍有可见行");
        check(near(layoutOffsetOf(list->firstChild()), 27 * 50), "首项布局偏移 = 27×行高");
    }
}

// ── 6. 卸载 ─────────────────────────────────────────────────────────────────

void testUnmount() {
    std::printf("[sliver-lazy] 懒加载子树整体卸载\n");

    ScrollController ctrl;
    auto calls = cui::make_shared<int>(0);
    auto cfg = cui::make_shared<Cfg>();
    cfg->controller = &ctrl;
    cfg->slivers = {SliverFixedExtentList::builder(
        50.0,
        [calls](BuildContext&, int) {
            ++*calls;
            return row(50);
        },
        1000)};

    Harness h;
    h.attach(cui::make_shared<Probe>(cfg), 300, 400);
    auto* list = h.find<RenderSliverFixedExtentList>();
    if (!list) {
        check(false, "卸载测试需要列表");
        return;
    }
    auto* el = elementOf(list);
    const size_t children = el ? el->childElements().size() : 0;
    check(children == 13, "先建立 13 个懒建子 Element");

    cfg->slivers.clear();
    probeState(h.root)->rebuildNow();
    h.pump();
    h.owner.finalizeTree();
    h.pump();
    check(h.find<RenderSliverFixedExtentList>() == nullptr, "sliver 渲染对象离开树");
    check(h.find<RenderViewport>() == nullptr, "视口一并卸载");
    const int callsAfter = *calls;
    ctrl.jumpTo(500);
    h.pump();
    check(*calls == callsAfter, "卸载后滚动不再调用 builder");
}

}  // namespace

int main() {
    std::printf("=== test_sliver_lazy ===\n");
    testFixedExtentLazy();
    testVariableListLazy();
    testListDelegateRebuild();
    testKeyedReorder();
    testUnknownChildCount();
    testUnmount();
    std::printf("%s: %d 个断言失败\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
