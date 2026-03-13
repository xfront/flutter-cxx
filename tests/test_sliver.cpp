/// @file test_sliver.cpp
/// @brief Sliver 渲染层单元测试（纯渲染对象，不经控件树，headless 不 flushPaint）。
///
/// 覆盖：
///   1. 方向枚举与 上游 的六个换算函数。
///   2. SliverConstraints：asBoxConstraints 的主轴无界/交叉轴紧锁、isNormalized。
///   3. SliverGeometry：layoutExtent/hitTestExtent/visible/cacheExtent 的 ?? 回退链。
///   4. RenderSliverToBoxAdapter：三个滚动位置下的 paint/cache/scrollExtent
///      与子节点绘制偏移（含水平轴）。
///   5. RenderSliverPadding：子约束改写、几何合成、paintOffset、无子分支。
///   6. RenderSliverFixedExtentList：index 数学 + 托管人驱动的懒加载窗口
///      （按需造子、掐头去尾回收、缓存扩窗回补、越出末尾的 scrollExtent）。
///   6b. RenderSliverList：变高子节点逐项累加、前导补造、
///       拿不到头部子节点时上报 scrollOffsetCorrection。
///   7. RenderViewport：layoutChildSequence 的逐子推进、内容区间
///      （scrollExtent 之和减视口）、越界 offset 自纠正、命中测试穿透。

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cui/foundation/geometry.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/sliver.h"

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

/// 固定理想尺寸的测试盒子；交叉轴被约束夹紧，主轴由 sliver 决定。
/// 覆写 hitTestSelf —— 本仓约定「只有覆写 hitTestSelf 的节点可命中」。
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

/// 上游 `RenderSliverBoxChildManager` 契约的最小实现：布局期间按需造盒子、
/// 入列时把序号写进 parentData、摘除即销毁（本仓 keepAlive 桶未移植）。
class FakeBoxChildManager : public RenderSliverBoxChildManager {
public:
    FakeBoxChildManager(RenderSliverMultiBoxAdaptor& host, int total, double itemExtent)
        : host_(&host), total_(total), itemExtent_(itemExtent) {}

    std::vector<int> materialized() const {
        std::vector<int> out;
        for (const auto& kv : boxes_)
            out.push_back(kv.first);
        return out;
    }
    TestBox* boxAt(int index) const {
        auto it = boxes_.find(index);
        return it == boxes_.end() ? nullptr : it->second.get();
    }
    /// 累计创建次数（回收后再造算新的一次）。
    int created() const { return created_; }
    bool underflow() const { return underflow_; }
    int finishLayoutCalls() const { return finishLayoutCalls_; }
    /// 变高列表：按序号给理想高度（不设则用 300，靠固定行高夹紧）。
    void setHeightFn(std::function<double(int)> fn) { heightFor_ = std::move(fn); }
    /// 模拟「头部取不到子节点」的托管人：小于该序号一律造不出来。
    void setMinIndex(int minIndex) { minIndex_ = minIndex; }

    void createChild(int index, RenderBox* after) override {
        if (index < minIndex_ || index >= total_ || boxes_.count(index))
            return;
        const double h = heightFor_ ? heightFor_(index) : 300.0;
        auto box = std::make_unique<TestBox>(300, h);
        TestBox* raw = box.get();
        boxes_[index] = std::move(box);
        ++created_;
        adopting_ = index;
        host_->insertChild(raw, after);
        adopting_ = std::nullopt;
    }
    void removeChild(RenderBox* child) override {
        const int index = host_->indexOf(child);
        host_->removeChild(child);
        if (index >= 0)
            boxes_.erase(index);
    }
    double estimateMaxScrollOffset(const SliverConstraints&, int, int, double,
                                   double) override {
        if (!heightFor_)
            return static_cast<double>(total_) * itemExtent_;
        double sum = 0.0;
        for (int i = 0; i < total_; ++i)
            sum += heightFor_(i);
        return sum;
    }
    int childCount() override { return total_; }
    void didAdoptChild(RenderBox* child) override {
        if (!adopting_)
            return;
        if (auto* pd = dynamic_cast<SliverMultiBoxAdaptorParentData*>(child->parentData()))
            pd->index = adopting_;
    }
    void setDidUnderflow(bool value) override { underflow_ = value; }
    void didFinishLayout() override { ++finishLayoutCalls_; }

private:
    RenderSliverMultiBoxAdaptor* host_;
    int total_;
    double itemExtent_;
    std::function<double(int)> heightFor_;
    int minIndex_ = 0;
    std::map<int, std::unique_ptr<TestBox>> boxes_;
    std::optional<int> adopting_;
    bool underflow_ = false;
    int created_ = 0;
    int finishLayoutCalls_ = 0;
};

// ── 1. 方向换算 ─────────────────────────────────────────────────────────────

void testEnums() {
    std::printf("[sliver] 方向枚举与换算\n");

    check(axisDirectionToAxis(AxisDirection::Down) == Axis::Vertical &&
              axisDirectionToAxis(AxisDirection::Up) == Axis::Vertical &&
              axisDirectionToAxis(AxisDirection::Right) == Axis::Horizontal &&
              axisDirectionToAxis(AxisDirection::Left) == Axis::Horizontal,
          "axisDirectionToAxis 四向");
    check(!axisDirectionIsReversed(AxisDirection::Down) &&
              !axisDirectionIsReversed(AxisDirection::Right) &&
              axisDirectionIsReversed(AxisDirection::Up) &&
              axisDirectionIsReversed(AxisDirection::Left),
          "axisDirectionIsReversed：Up/Left 反向");
    check(flipAxisDirection(AxisDirection::Down) == AxisDirection::Up &&
              flipAxisDirection(AxisDirection::Left) == AxisDirection::Right,
          "flipAxisDirection");
    check(axisDirectionToGrowthDirection(AxisDirection::Up) == GrowthDirection::Reverse &&
              axisDirectionToGrowthDirection(AxisDirection::Down) == GrowthDirection::Forward,
          "axisDirectionToGrowthDirection");
    // 上游表：apply(Down, forward)=Down，apply(Down, reverse)=Up，
    //        apply(Up, forward)=Up，apply(Up, reverse)=Down。
    check(applyGrowthDirectionToAxisDirection(AxisDirection::Down, GrowthDirection::Forward) ==
              AxisDirection::Down &&
              applyGrowthDirectionToAxisDirection(AxisDirection::Down, GrowthDirection::Reverse) ==
                  AxisDirection::Up &&
              applyGrowthDirectionToAxisDirection(AxisDirection::Up, GrowthDirection::Forward) ==
                  AxisDirection::Up &&
              applyGrowthDirectionToAxisDirection(AxisDirection::Up, GrowthDirection::Reverse) ==
                  AxisDirection::Down,
          "applyGrowthDirectionToAxisDirection 四组真值");
    check(applyGrowthDirectionToScrollDirection(ScrollDirection::Forward,
                                                GrowthDirection::Reverse) ==
                  ScrollDirection::Reverse &&
              applyGrowthDirectionToScrollDirection(ScrollDirection::Idle,
                                                    GrowthDirection::Reverse) ==
                  ScrollDirection::Idle &&
              applyGrowthDirectionToScrollDirection(ScrollDirection::Forward,
                                                    GrowthDirection::Forward) ==
                  ScrollDirection::Forward,
          "applyGrowthDirectionToScrollDirection（Idle 不翻转）");
    check(near(clampDouble(-5, 0, 10), 0.0) && near(clampDouble(50, 0, 10), 10.0) &&
              near(clampDouble(4, 0, 10), 4.0),
          "clampDouble");
}

// ── 2. SliverConstraints ────────────────────────────────────────────────────

void testConstraints() {
    std::printf("[sliver] SliverConstraints\n");

    SliverConstraints c = vc(0, 200, 300, 200);
    check(c.axis() == Axis::Vertical, "axis() 由 axisDirection 推出");
    check(c.isNormalized(), "scrollOffset>=0 && cacheOrigin<=0 && overlap<=0 → isNormalized");
    check(!vc(-1, 200, 300, 200).isNormalized() && !vc(0, 200, 300, 200, -1, 0,
                                                     AxisDirection::Down, 5)
                                                     .isNormalized(),
          "负滚动偏移 / 正 overlap 非规范化");

    // 主轴无界、交叉轴紧锁（上游 asBoxConstraints 的默认分支）。
    BoxConstraints b = c.asBoxConstraints();
    check(near(b.minWidth, 300) && near(b.maxWidth, 300), "竖直：交叉轴（宽）紧锁 300");
    check(near(b.minHeight, 0) && b.maxHeight > 1e17, "竖直：主轴（高）无界");

    // 主轴上下界都给出 → 该轴紧约束（固定行高 sliver 的子约束）。
    BoxConstraints t = c.asBoxConstraints(50, 50);
    check(near(t.minHeight, 50) && near(t.maxHeight, 50) && near(t.maxWidth, 300),
          "竖直：minExtent==maxExtent 时主轴紧锁");

    SliverConstraints h = vc(0, 200, 120, 200, 0, 0, AxisDirection::Right);
    BoxConstraints bh = h.asBoxConstraints(30, 40);
    check(near(bh.minWidth, 30) && near(bh.maxWidth, 40) && near(bh.minHeight, 120) &&
              near(bh.maxHeight, 120),
          "水平：主轴=宽、交叉轴=高");
    check(h.axis() == Axis::Horizontal, "水平轴 axis()");
}

// ── 3. SliverGeometry 回退链 ────────────────────────────────────────────────

void testGeometryResolution() {
    std::printf("[sliver] SliverGeometry 的 ?? 回退\n");

    SliverGeometry g;
    g.paintExtent = 60;
    check(near(g.resolvedLayoutExtent(), 60), "layoutExtent ?? paintExtent");
    check(near(g.resolvedCacheExtent(), 60), "cacheExtent ?? layoutExtent ?? paintExtent");
    check(near(g.resolvedHitTestExtent(), 60), "hitTestExtent ?? paintExtent");
    check(g.resolvedVisible(), "visible ?? paintExtent > 0");

    g.layoutExtent = 40;
    check(near(g.resolvedCacheExtent(), 40), "layoutExtent 就位后 cacheExtent 跟随");
    g.paintExtent = 0;
    check(!g.resolvedVisible(), "paintExtent 归零则不可见（visible 仍为 ?? 回退）");
    g.visible = true;
    check(g.resolvedVisible(), "显式 visible 覆盖回退值");

    SliverGeometry z = SliverGeometry::zero();
    check(near(z.scrollExtent, 0) && near(z.paintExtent, 0) && near(z.resolvedCacheExtent(), 0) &&
              !z.scrollOffsetCorrection,
          "SliverGeometry::zero");
}

// ── 4. RenderSliverToBoxAdapter ─────────────────────────────────────────────

void testToBoxAdapter() {
    std::printf("[sliver] RenderSliverToBoxAdapter\n");

    auto box = std::make_unique<TestBox>(200, 100);
    RenderSliverToBoxAdapter sl;
    sl.setChild(box.get());

    // 全部可见
    sl.layout(vc(0, 250, 300, 250, 0, 250), true);
    check(!sl.needsLayout(), "layout(SliverConstraints) 走查后清脏");
    check(near(sl.scrollExtent(), 100) && near(sl.paintExtent(), 100) &&
              near(sl.maxPaintExtent(), 100) && near(sl.cacheExtent(), 100),
          "滚动偏移 0：scroll/paint/cache/maxPaint 皆等于内容高度");
    check(sl.geometry().hasVisualOverflow == false, "内容未超出绘制窗口 → 无越界");
    check(near(sl.childScrollOffset(sl.child()), 0), "单子节点滚动起点为 0");
    Offset at0 = childPaintOffset(sl.child());
    check(near(at0.dx, 0) && near(at0.dy, 0), "子盒绘制偏移 (0,0)");

    // 部分滚出：100 高内容滚掉 60 → 只剩 40 可见
    sl.layout(vc(60, 250, 300, 250, 0, 250), true);
    check(near(sl.scrollExtent(), 100), "scrollExtent 与滚动偏移无关");
    check(near(sl.paintExtent(), 40) && near(sl.hitTestExtent(), 40),
          "paint/hitTestExtent = 100-60");
    check(sl.visible(), "仍有可见部分");
    check(sl.geometry().hasVisualOverflow, "scrollOffset>0 → 有视觉越界");
    Offset at60 = childPaintOffset(sl.child());
    check(near(at60.dx, 0) && near(at60.dy, -60), "Down：子盒上移 60（paintExtent+scroll-scrollExtent）");

    // 完全滚过：绘制与命中长度归零
    sl.layout(vc(150, 250, 300, 250, 0, 250), true);
    check(near(sl.paintExtent(), 0) && !sl.visible(), "超出内容总长 → 不可见");
    check(!sl.geometry().scrollOffsetCorrection, "不请求视口修正滚动偏移");

    // 缓存窗口前置：cacheOrigin=-50 → 从滚动点前 50 起缓存
    sl.layout(vc(60, 250, 300, 250, -50, 300), true);
    check(near(sl.paintExtent(), 40) && near(sl.cacheExtent(), 90),
          "cacheOrigin 把缓存窗口向前扩 50");

    // 水平轴
    auto hbox = std::make_unique<TestBox>(200, 100);
    RenderSliverToBoxAdapter hsl;
    hsl.setChild(hbox.get());
    hsl.layout(vc(0, 150, 100, 150, 0, 150, AxisDirection::Right), true);
    check(near(hbox->size().width, 200) && near(hbox->size().height, 100),
          "水平：交叉轴紧锁 100，主轴无界 → 盒子取 200x100");
    check(near(hsl.scrollExtent(), 200) && near(hsl.paintExtent(), 150),
          "水平：主轴量宽，绘制窗口截断到 150");
    Offset hAt = childPaintOffset(hbox.get());
    check(near(hAt.dx, 0) && near(hAt.dy, 0), "Right：绘制偏移 (-scrollOffset, 0)");

    // 无子节点
    RenderSliverToBoxAdapter empty;
    empty.layout(vc(0, 250, 300, 250), true);
    check(!empty.visible() && near(empty.scrollExtent(), 0), "无子 → SliverGeometry.zero");
}

// ── 5. RenderSliverPadding ──────────────────────────────────────────────────

void testPadding() {
    std::printf("[sliver] RenderSliverPadding\n");

    const EdgeInsets pad = EdgeInsets::only(/*l=*/5, /*t=*/10, /*r=*/15, /*b=*/20);

    auto box = std::make_unique<TestBox>(300, 300);
    auto inner = std::make_unique<RenderSliverToBoxAdapter>();
    inner->setChild(box.get());

    RenderSliverPadding sliver(pad, inner.get());
    sliver.layout(vc(0, 250, 200, 250, 0, 250), true);

    // 子 sliver 拿到的交叉轴被左右内缩 20；上内缩 10 从绘制窗口扣掉。
    check(near(box->size().width, 180) && near(box->size().height, 300),
          "子盒交叉轴 = 200-(5+15) = 180，主轴无界 → 高取 300");
    check(near(sliver.scrollExtent(), 330), "scrollExtent = 主轴内缩 30 + 内容 300");
    check(near(sliver.paintExtent(), 250), "paintExtent 被绘制窗口 250 截断");
    check(near(sliver.layoutExtent(), 250), "layoutExtent = 内缩绘制 10 + 子 layout 240");
    check(near(sliver.maxPaintExtent(), 330), "maxPaintExtent = 30 + 300");
    check(near(sliver.hitTestExtent(), 250), "hitTestExtent 覆盖内缩 + 子命中");
    check(sliver.geometry().hasVisualOverflow, "子内容越界透传到外层");
    Offset at = childPaintOffset(inner.get());
    check(near(at.dx, 5) && near(at.dy, 10), "子 sliver 绘制偏移 = (left, beforePadding)");
    check(near(sliver.childMainAxisPosition(inner.get()), 10), "childMainAxisPosition = 上内缩绘制长度");
    check(near(sliver.childCrossAxisPosition(inner.get()), 5), "childCrossAxisPosition = left");
    check(near(sliver.childScrollOffset(inner.get()), 10), "childScrollOffset = beforePadding");

    // 上内缩已滚过：scrollOffset=10 → 前置内缩不再占绘制窗口，只剩内容前 240。
    sliver.layout(vc(10, 240, 200, 250, 0, 240), true);
    check(near(sliver.scrollExtent(), 330), "scrollExtent 与滚动无关");
    check(near(sliver.paintExtent(), 240), "上内缩滚尽 → paintExtent = 子可见 240");
    Offset at2 = childPaintOffset(inner.get());
    check(near(at2.dx, 5) && near(at2.dy, 0), "前置内缩滚尽 → 子贴顶");

    // 无子：只留下内缩本身。
    RenderSliverPadding empty(pad);
    empty.layout(vc(0, 250, 200, 250, 0, 250), true);
    check(near(empty.scrollExtent(), 30), "无子 → scrollExtent = 主轴内缩");
    check(near(empty.paintExtent(), 10), "无子 → paintExtent = 前置内缩的可见长度");
    check(near(empty.maxPaintExtent(), 30), "无子 → maxPaintExtent = 主轴内缩");
}

// ── 6. RenderSliverFixedExtentList ──────────────────────────────────────────

void testFixedExtentList() {
    std::printf("[sliver] RenderSliverFixedExtentList（托管人懒加载）\n");

    RenderSliverFixedExtentList list(50.0);
    FakeBoxChildManager mgr(list, 10, 50.0);
    list.setChildManager(&mgr);

    // index 数学（上游 floor/round/ceil 三分支）
    check(list.indexToLayoutOffset(0) == 0 && near(list.indexToLayoutOffset(7), 350),
          "indexToLayoutOffset = itemExtent * index");
    check(list.getMinChildIndexForScrollOffset(0) == 0 &&
              list.getMinChildIndexForScrollOffset(75) == 1 &&
              list.getMinChildIndexForScrollOffset(100) == 2,
          "getMinChildIndex：整除取整、否则下取整");
    check(list.getMaxChildIndexForScrollOffset(250) == 4 &&
              list.getMaxChildIndexForScrollOffset(275) == 5 &&
              list.getMaxChildIndexForScrollOffset(20) == 0,
          "getMaxChildIndex：减一后取整，且不小于 0");
    check(list.totalChildren() == 10 && list.childCount() == 0,
          "总长问托管人（10），在树数量问子节点（0）");

    // 首屏：窗口 200 = 4 行
    list.layout(vc(0, 200, 100, 200, 0, 200), true);
    check(near(list.scrollExtent(), 500) && near(list.maxPaintExtent(), 500),
          "scrollExtent = 10 x 50（托管人估算）");
    check(near(list.paintExtent(), 200) && near(list.cacheExtent(), 200),
          "绘制窗口 200 装满 4 行");
    check(mgr.materialized() == std::vector<int>({0, 1, 2, 3}),
          "只造窗口内的 0..3 行，第 5 行从未创建");
    check(list.childCount() == 4 && mgr.created() == 4, "在树子节点 = 创建次数 = 4");
    check(list.paintsChild(mgr.boxAt(0)) && list.paintsChild(mgr.boxAt(3)),
          "paintsChild：窗口内首尾行都参与绘制");
    Offset a0 = childPaintOffset(mgr.boxAt(0));
    Offset a3 = childPaintOffset(mgr.boxAt(3));
    check(near(a0.dy, 0) && near(a3.dy, 150), "Down：子行绘制偏移 = index*50");
    check(near(mgr.boxAt(0)->size().height, 50) && near(mgr.boxAt(0)->size().width, 100),
          "子行受紧约束：主轴 50、交叉轴 100");
    check(near(list.childMainAxisPosition(mgr.boxAt(2)), 100),
          "childMainAxisPosition = childScrollOffset - scrollOffset");
    check(mgr.underflow() == false, "窗口被填满 → 未欠载");

    // 滚到 120：首行 index 2，窗口覆盖到 index 6；0/1 回收
    list.layout(vc(120, 200, 100, 200, 0, 200), true);
    check(near(list.paintExtent(), 200), "中段滚动仍填满绘制窗口");
    check(mgr.materialized() == std::vector<int>({2, 3, 4, 5, 6}),
          "掐头 2 行 + 向下补造到 6");
    check(mgr.boxAt(0) == nullptr && mgr.boxAt(1) == nullptr, "窗口外的首两行已销毁");
    Offset a2 = childPaintOffset(mgr.boxAt(2));
    Offset a6 = childPaintOffset(mgr.boxAt(6));
    check(near(a2.dy, -20) && near(a6.dy, 180), "滚动 120 后各行偏移 = index*50 - 120");
    check(list.paintsChild(mgr.boxAt(2)) && list.paintsChild(mgr.boxAt(6)),
          "存量子节点全部落在绘制窗口内");

    // 带缓存窗口：cacheOrigin=-100、remainingCacheExtent=400 → 缓存区间 [20,420]
    list.layout(vc(120, 200, 100, 200, -100, 400), true);
    check(near(list.cacheExtent(), 400), "缓存长度被 remainingCacheExtent 上限钳制");
    check(mgr.materialized() == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8}),
          "缓存窗口向前回补 0/1、向后扩到 8");
    check(mgr.created() == 11, "回收即销毁：回补的 0/1 重新创建（keepAlive 桶未移植）");
    check(mgr.finishLayoutCalls() == 3, "每轮布局收尾都通知托管人");

    // 内容不足窗口：向下补造到末尾即止
    RenderSliverFixedExtentList short_list(50.0);
    FakeBoxChildManager short_mgr(short_list, 10, 50.0);
    short_list.setChildManager(&short_mgr);
    short_list.layout(vc(0, 500, 100, 500, 0, 600), true);
    check(short_mgr.materialized() == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}),
          "缓存区间超出末尾 → 只造到最后一行");
    check(near(short_list.scrollExtent(), 500) && near(short_list.cacheExtent(), 500),
          "内容 500 短于缓存 600 → 缓存长度取内容");
    check(short_mgr.underflow(), "子节点用尽 → setDidUnderflow(true)");

    // 空托管人
    RenderSliverFixedExtentList empty(50.0);
    FakeBoxChildManager no_children(empty, 0, 50.0);
    empty.setChildManager(&no_children);
    empty.layout(vc(0, 200, 100, 200, 0, 200), true);
    check(near(empty.scrollExtent(), 0) && !empty.visible(), "无子 → 几何全零");
    check(no_children.underflow(), "造不出首子 → 欠载");

    // 滚过末尾：一个子也造不出来，用精确总长回推内容长度
    RenderSliverFixedExtentList skipped(50.0);
    FakeBoxChildManager past(skipped, 10, 50.0);
    skipped.setChildManager(&past);
    skipped.layout(vc(600, 200, 100, 200, 0, 200), true);
    check(past.materialized().empty(), "越出末尾 → 本轮没有子节点");
    check(near(skipped.scrollExtent(), 500) && near(skipped.maxPaintExtent(), 500),
          "无子且已过末尾 → scrollExtent = childCount x itemExtent");
    check(!skipped.visible() && near(skipped.paintExtent(), 0), "越出内容 → 无绘制长度");

    // 无托管人的 eager 便捷入口：整棵树一次性布局，长度按存量算。
    // 窗口外的子节点不会被回收（sliver.h 文件头偏离 3），故这里只验证
    // 「全部入窗」的场景。
    std::vector<std::unique_ptr<TestBox>> boxes;
    RenderSliverFixedExtentList eager(50.0);
    for (int i = 0; i < 4; ++i) {
        boxes.push_back(std::make_unique<TestBox>(300, 300));
        eager.insertChild(boxes.back().get(), i ? boxes[i - 1].get() : nullptr);
    }
    check(eager.childCount() == 4, "eager：4 个子盒子入列");
    eager.layout(vc(0, 200, 100, 200, 0, 200), true);
    check(near(eager.scrollExtent(), 200), "eager：scrollExtent = 存量 4 x 50");
    check(near(childPaintOffset(boxes[3].get()).dy, 150), "eager：绘制偏移仍按 index*50");
}

// ── 6b. RenderSliverList（变高子节点的懒加载）──────────────────────────────

void testSliverList() {
    std::printf("[sliver] RenderSliverList（变高 + 前导补造）\n");

    // 偶序号 100 高、奇序号 60 高 → 十项共 800。
    auto heightFor = [](int i) { return i % 2 == 0 ? 100.0 : 60.0; };

    RenderSliverList list;
    FakeBoxChildManager mgr(list, 10, 0.0);
    mgr.setHeightFn(heightFor);
    list.setChildManager(&mgr);

    // 首屏：缓存窗口 [0,650]，逐项累加到盖过 650 才停 → 0..8
    list.layout(vc(0, 400, 300, 400, 0, 650), true);
    check(mgr.materialized() == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8}),
          "变高列表按累加布局偏移补造到盖过缓存窗口");
    check(near(list.scrollExtent(), 800), "托管人报精确总长 800");
    check(near(list.paintExtent(), 400) && near(list.cacheExtent(), 650),
          "绘制夹到视口、缓存夹到 650");
    const double expect[9] = {0, 100, 160, 260, 320, 420, 480, 580, 640};
    bool offsetsOk = true;
    for (int i = 0; i <= 8; ++i) {
        auto* pd = dynamic_cast<SliverMultiBoxAdaptorParentData*>(mgr.boxAt(i)->parentData());
        offsetsOk = offsetsOk && pd && pd->layoutOffset && near(*pd->layoutOffset, expect[i]);
    }
    check(offsetsOk, "逐项 layoutOffset = 前一项的头 + 前一项的高度");

    // 滚到 300：缓存窗口 [50,700]，0..8 全在窗口内（首项与窗口起点相交）
    list.layout(vc(300, 400, 300, 400, -250, 650), true);
    check(mgr.materialized() == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8}),
          "小步滚动不增不减子节点");
    check(near(list.paintExtent(), 400) && near(list.scrollExtent(), 800),
          "绘制窗口仍被填满，内容长度不变");
    check(!list.paintsChild(mgr.boxAt(0)) && list.paintsChild(mgr.boxAt(8)),
          "只在缓存窗口内的首项不绘制、末项仍绘制");
    check(near(childPaintOffset(mgr.boxAt(3)).dy, 260 - 300), "绘制偏移 = layoutOffset - scrollOffset");

    // 大幅跳跃：缓存窗口 [750,1400] → 掐头 0..8、补造末项 9，越出内容
    list.layout(vc(1000, 400, 300, 400, -250, 650), true);
    check(mgr.materialized() == std::vector<int>({9}), "整段窗口外子节点被回收");
    check(near(list.scrollExtent(), 800) && !list.visible(),
          "子节点用尽 → reachedEnd，内容长度取实际末尾 800");
    check(near(list.cacheExtent(), 50), "只有末项的尾部 50 落进缓存窗口");

    // 前导补造拿不到子节点 → 上报 scrollOffsetCorrection 让视口重来。
    // 末子（序号 9）留在树上，托管人从此造不出它前面的项。
    mgr.setMinIndex(9);
    list.layout(vc(700, 400, 300, 400, -250, 650), true);
    check(list.geometry().scrollOffsetCorrection != 0.0 &&
              near(list.geometry().scrollOffsetCorrection, -450),
          "首子布局偏移够不到滚动点 → 纠偏 -(scrollOffset + cacheOrigin)");
    check(list.childCount() == 1 && mgr.boxAt(9) != nullptr, "纠偏轮不改子节点集合");
    auto* pd9 = dynamic_cast<SliverMultiBoxAdaptorParentData*>(mgr.boxAt(9)->parentData());
    check(pd9 && pd9->layoutOffset && near(*pd9->layoutOffset, 0.0),
          "纠偏前先把首子布局偏移归零（上游同款写法）");
}

// ── 7. RenderViewport ─────────────────────────────────────────────────

void testViewport() {
    std::printf("[sliver] RenderViewport\n");

    auto boxA = std::make_unique<TestBox>(200, 300);
    auto boxB = std::make_unique<TestBox>(200, 200);
    auto slA = std::make_unique<RenderSliverToBoxAdapter>();
    auto slB = std::make_unique<RenderSliverToBoxAdapter>();
    slA->setChild(boxA.get());
    slB->setChild(boxB.get());

    RenderViewport vp;  // 竖直 Down，视口 400x300
    vp.insertChild(slA.get(), nullptr);
    vp.insertChild(slB.get(), slA.get());

    int extentsCalls = 0;
    double reportedMin = -1, reportedMax = -1, reportedViewport = -1;
    vp.setOnExtentsChanged([&](double mn, double mx, double vpe) {
        ++extentsCalls;
        reportedMin = mn;
        reportedMax = mx;
        reportedViewport = vpe;
    });

    vp.layout(BoxConstraints::tight(300, 400), true);
    check(near(vp.size().width, 300) && near(vp.size().height, 400),
          "sizedByParent：视口尺寸 = 父约束 biggest");
    check(extentsCalls >= 1, "布局完成回写内容区间");
    check(near(reportedViewport, 400), "回写视口主轴尺寸 400");
    check(near(reportedMin, 0) && near(reportedMax, 100),
          "内容区间 = [0, 500-400]（scrollExtent 之和减视口）");
    check(near(vp.minScrollExtent(), 0) && near(vp.maxScrollExtent(), 100),
          "min/maxScrollExtent 与回写一致");
    check(near(vp.contentScrollExtent(), 500), "contentScrollExtent 是未减视口的内容总长");
    check(near(slA->paintExtent(), 300) && near(slB->paintExtent(), 100),
          "A 全见、B 只见前 100（layoutExtent 逐子推进）");
    Offset pA = childPaintOffset(slA.get());
    Offset pB = childPaintOffset(slB.get());
    check(near(pA.dy, 0) && near(pB.dy, 300), "子 sliver 绘制偏移 = 累计 layoutExtent");
    check(vp.childAfter(slA.get()) == slB.get() && vp.childBefore(slB.get()) == slA.get() &&
              vp.childAfter(slB.get()) == nullptr,
          "childAfter/childBefore 走查");

    // 滚到底
    vp.setOffset(100);
    vp.layout(BoxConstraints::tight(300, 400), true);
    check(near(slA->paintExtent(), 200) && near(slB->paintExtent(), 200),
          "滚动 100：A 剩 200、B 全见 200");
    check(near(childPaintOffset(slB.get()).dy, 200), "B 顶到 200 处");
    check(near(childPaintOffset(boxA.get()).dy, -100), "A 内盒子整体上移 100");
    check(slA->visible() && slB->visible(), "滚动 100 后两个 sliver 都可见");

    // 越界偏移自纠正：500 → 夹回 100，并用新偏移重排一次
    vp.setOffset(500);
    vp.layout(BoxConstraints::tight(300, 400), true);
    check(near(vp.offset(), 100), "超界滚动偏移在布局循环内夹回内容上界");
    check(near(slA->paintExtent(), 200), "重排后几何与滚动 100 一致");

    // 负偏移同理夹回下界
    vp.setOffset(-80);
    vp.layout(BoxConstraints::tight(300, 400), true);
    check(near(vp.offset(), 0), "负滚动偏移夹回 0");

    vp.setOffset(0);
    vp.layout(BoxConstraints::tight(300, 400), true);

    // 命中测试穿透：视口 → sliver → 盒子
    vp.setOffset(100);
    vp.layout(BoxConstraints::tight(300, 400), true);
    std::vector<RenderObject*> path;
    check(vp.hitTest(Offset{50, 250}, path), "视口内 (50,250) 命中 B 内盒子");
    check(path.size() == 1 && path.back() == boxB.get(),
          "命中链只有盒子（本仓约定：sliver/视口不覆写 hitTestSelf，不入链）");
    path.clear();
    check(vp.hitTest(Offset{50, 50}, path), "滚动 100 后 (50,50) 命中 A 内盒子");
    check(path.size() == 1 && path.back() == boxA.get(),
          "命中链 = boxA（坐标换算穿到滚动后的子盒）");
    path.clear();
    check(!vp.hitTest(Offset{350, 50}, path), "交叉轴越界点不命中");

    // 空视口
    RenderViewport empty;
    empty.layout(BoxConstraints::tight(300, 400), true);
    check(near(empty.maxScrollExtent(), 0) && !empty.hasVisualOverflow(), "空视口内容区间为 0");

    // 水平视口
    auto hbox = std::make_unique<TestBox>(500, 100);
    auto hsliver = std::make_unique<RenderSliverToBoxAdapter>();
    hsliver->setChild(hbox.get());
    RenderViewport hvp(AxisDirection::Right);
    hvp.insertChild(hsliver.get(), nullptr);
    hvp.layout(BoxConstraints::tight(400, 100), true);
    check(near(hvp.maxScrollExtent(), 100), "水平：内容 500 - 视口宽 400");
    check(near(hsliver->paintExtent(), 400), "水平：绘制窗口按宽度截断");
    check(near(childPaintOffset(hbox.get()).dx, 0), "水平：盒子绘制偏移 x 从 0 起");
}

}  // namespace

int main() {
    std::printf("=== test_sliver ===\n");
    testEnums();
    testConstraints();
    testGeometryResolution();
    testToBoxAdapter();
    testPadding();
    testFixedExtentList();
    testSliverList();
    testViewport();
    std::printf("%s: %d 个断言失败\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
