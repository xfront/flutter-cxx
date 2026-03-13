/// @file test_intrinsic.cpp
/// @brief intrinsic 尺寸协议（rendering/box.dart 的 `_computeIntrinsics`）与
///        IntrinsicWidth / IntrinsicHeight 控件单元测试。
///
/// 覆盖：
///   A. 协议本身：基类四个 compute 钩子返回 0.0、代理盒原样转发、按
///      「维度 + 入参」记忆化、markNeedsLayout 清空缓存后重新计算。
///   B. 各渲染对象的公式（逐个对照上游 shifted_box.dart / proxy_box.dart /
///      stack.dart / paragraph.dart）：Padding、ConstrainedBox、Align 因子、
///      Stack 只算非 positioned 子、Paragraph 的 width-in-height-out、
///      RenderIntrinsicHeight / RenderIntrinsicWidth（含 step）。
///   C. RenderFlex 的 `_getIntrinsicSize` 两分支：主轴按 flex 权重折算
///      （maxFlexFractionSoFar × totalFlex + inflexibleSpace），交叉轴先复算
///      主轴分配再取子节点交叉轴 intrinsic 的最大值（无界 extent 时 flex
///      子退化为非 flex）。
///   D. intrinsic 依赖与重排的耦合：被查过 intrinsic 的子节点标脏时，脏标
///      必须越过 relayout boundary 交给父节点；没被查过的边界子节点则不会。
///   E. 控件层：IntrinsicHeight / IntrinsicWidth 在真实 Element 树里的效果。
///
/// 全部 headless：intrinsic 计算不产生绘制，A–D 直接在手工渲染树上跑，
/// E 只 flushBuildScope + flushLayout。

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"

using namespace cui;

namespace {

constexpr double kInf = BoxConstraints::kInfinity;

int g_failures = 0;

bool nearEq(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

// ── 测试用渲染对象 ──────────────────────────────────────────────────────────

/// 什么都不算：用于断言基类四个 compute 钩子的 0.0，以及「未建立 intrinsic
/// 依赖」的边界子节点。
class PlainBox : public RenderBox {
public:
    bool isRelayoutBoundary() const override { return boundary_; }
    void setBoundary(bool v) { boundary_ = v; }

private:
    bool boundary_ = false;
};

/// 固定尺寸叶子（min == max），对应控件层的 SizedBox(w, h)。
class FixedBox : public RenderBox {
public:
    FixedBox(double w, double h) : w_(w), h_(h) {}

protected:
    void performLayout() override { setSize(constraints_.constrain(Size{w_, h_})); }
    double computeMinIntrinsicWidth(double) override { return w_; }
    double computeMaxIntrinsicWidth(double) override { return w_; }
    double computeMinIntrinsicHeight(double) override { return h_; }
    double computeMaxIntrinsicHeight(double) override { return h_; }

private:
    double w_, h_;
};

/// min 与 max 不同的叶子：用来验证四个入口没有互相串用。
class RangeBox : public RenderBox {
public:
    RangeBox(double minW, double maxW, double minH, double maxH)
        : minW_(minW), maxW_(maxW), minH_(minH), maxH_(maxH) {}

protected:
    void performLayout() override {
        setSize(constraints_.constrain(Size{maxW_, maxH_}));
    }
    double computeMinIntrinsicWidth(double) override { return minW_; }
    double computeMaxIntrinsicWidth(double) override { return maxW_; }
    double computeMinIntrinsicHeight(double) override { return minH_; }
    double computeMaxIntrinsicHeight(double) override { return maxH_; }

private:
    double minW_, maxW_, minH_, maxH_;
};

/// 高度依赖宽度的叶子（模拟换行文本）：宽 ≥ 60 只需一行，否则三行。
/// 上游 intrinsic 里唯一真正「跨轴依赖」的情形，用它验证交叉轴分支把
/// 分配到的主轴长度真的传给了子节点。
class WrappingBox : public RenderBox {
public:
    WrappingBox(double maxW, double narrow, double wide, double threshold)
        : maxW_(maxW), narrow_(narrow), wide_(wide), threshold_(threshold) {}

protected:
    void performLayout() override {
        const double w = constraints_.constrainWidth(maxW_);
        setSize(constraints_.constrain(Size{w, heightFor(w)}));
    }
    double computeMinIntrinsicWidth(double) override { return 10.0; }
    double computeMaxIntrinsicWidth(double) override { return maxW_; }
    double computeMinIntrinsicHeight(double width) override { return heightFor(width); }
    double computeMaxIntrinsicHeight(double width) override { return heightFor(width); }

private:
    double heightFor(double w) const { return w >= threshold_ ? wide_ : narrow_; }

    double maxW_, narrow_, wide_, threshold_;
};

/// 统计四个 compute 钩子各自被调用了几次（记忆化与失效的判据）。
class CountingBox : public RenderBox {
public:
    int minWidthCalls = 0;
    int maxWidthCalls = 0;
    int minHeightCalls = 0;
    int maxHeightCalls = 0;

    bool isRelayoutBoundary() const override { return true; }

protected:
    void performLayout() override {}
    double computeMinIntrinsicWidth(double) override {
        ++minWidthCalls;
        return 7.0;
    }
    double computeMaxIntrinsicWidth(double) override {
        ++maxWidthCalls;
        return 11.0;
    }
    double computeMinIntrinsicHeight(double) override {
        ++minHeightCalls;
        return 3.0;
    }
    double computeMaxIntrinsicHeight(double) override {
        ++maxHeightCalls;
        return 5.0;
    }
};

/// 只负责挂子节点的容器（不布局），用于 D 组的父子链。
class ContainerBox : public RenderBoxContainer {
protected:
    void performLayout() override {}
};

/// 真正的 relayout boundary（root）。
class BoundaryBox : public RenderBoxContainer {
public:
    bool isRelayoutBoundary() const override { return true; }

protected:
    void performLayout() override {}
};

/// 把 Widget 树接入 BuildOwner + RenderView + PipelineOwner 并布局好。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        po.flushLayout();
    }

    void rebuildAndLayout() {
        owner.flushBuildScope();
        po.flushLayout();
    }
};

/// 深度优先取第一个（先序）指定类型的 RenderObject。
template <typename T>
T* findRo(Element* root) {
    RenderObject* ro = root ? root->findRenderObject() : nullptr;
    if (!ro)
        return nullptr;
    std::vector<RenderObject*> stack{ro};
    while (!stack.empty()) {
        RenderObject* n = stack.back();
        stack.pop_back();
        if (auto* hit = dynamic_cast<T*>(n))
            return hit;
        n->visitChildren([&](AbstractNode* c) {
            if (c)
                stack.push_back(static_cast<RenderObject*>(c));
        });
    }
    return nullptr;
}

// ── A. 协议：基类值 / 代理转发 / 记忆化 / 失效 ──────────────────────────────
void testProtocol() {
    std::puts("[A] intrinsic 协议");

    PlainBox base;
    check(nearEq(base.getMinIntrinsicWidth(0.0), 0.0) &&
                  nearEq(base.getMaxIntrinsicWidth(0.0), 0.0) &&
                  nearEq(base.getMinIntrinsicHeight(0.0), 0.0) &&
                  nearEq(base.getMaxIntrinsicHeight(0.0), 0.0),
          "基类四个 compute 钩子返回 0.0");

    FixedBox leaf(40.0, 30.0);
    RenderProxyBox proxy;
    proxy.insertChild(&leaf, nullptr);
    check(nearEq(proxy.getMinIntrinsicWidth(kInf), 40.0) &&
                  nearEq(proxy.getMaxIntrinsicWidth(12.0), 40.0),
          "代理盒把宽度 intrinsic 原样转发给子节点");
    check(nearEq(proxy.getMinIntrinsicHeight(kInf), 30.0) &&
                  nearEq(proxy.getMaxIntrinsicHeight(kInf), 30.0),
          "代理盒把高度 intrinsic 原样转发给子节点");

    RenderProxyBox empty;
    check(nearEq(empty.getMaxIntrinsicWidth(kInf), 0.0) &&
                  nearEq(empty.getMaxIntrinsicHeight(kInf), 0.0),
          "无子节点的代理盒为 0.0");

    CountingBox c;
    c.getMinIntrinsicWidth(10.0);
    c.getMinIntrinsicWidth(10.0);
    check(c.minWidthCalls == 1, "同维度同入参第二次查询命中缓存（只算一次）");
    c.getMinIntrinsicWidth(20.0);
    check(c.minWidthCalls == 2, "入参变化后重新计算");
    c.getMinIntrinsicWidth(10.0);
    check(c.minWidthCalls == 2, "回到旧入参仍命中缓存");
    c.getMaxIntrinsicWidth(10.0);
    check(c.minWidthCalls == 2 && c.maxWidthCalls == 1,
          "缓存按维度分开（min/max 互不覆盖）");
    c.getMinIntrinsicHeight(10.0);
    c.getMaxIntrinsicHeight(10.0);
    check(c.minHeightCalls == 1 && c.maxHeightCalls == 1, "高度维度各自缓存");

    c.markNeedsLayout();
    c.getMinIntrinsicWidth(10.0);
    c.getMinIntrinsicHeight(10.0);
    check(c.minWidthCalls == 3 && c.minHeightCalls == 2,
          "markNeedsLayout 清空全部维度缓存");

    // 入参 kInfinity 是合法查询（上游用 double.infinity 表达「该轴无约束」）。
    FixedBox probe(40.0, 30.0);
    check(nearEq(probe.getMaxIntrinsicWidth(kInf), 40.0), "kInfinity 入参合法");
}

// ── B. 各渲染对象的公式 ─────────────────────────────────────────────────────
void testBoxFormulas() {
    std::puts("[B] 各盒的 compute*Intrinsic*");

    // ── Padding：子节点 intrinsic + padding，入参先扣掉自身占用 ──
    FixedBox content(40.0, 30.0);
    RenderPadding pad(EdgeInsets{10.0, 20.0, 30.0, 40.0});  // 横 40 / 纵 60
    pad.insertChild(&content, nullptr);
    check(nearEq(pad.getMinIntrinsicWidth(kInf), 80.0), "Padding 宽 = 子宽 + 左右内缩");
    check(nearEq(pad.getMaxIntrinsicWidth(100.0), 80.0),
          "Padding 宽不受入参高度影响（子节点与高度无关）");
    check(nearEq(pad.getMaxIntrinsicHeight(200.0), 90.0), "Padding 高 = 子高 + 上下内缩");
    check(nearEq(pad.getMaxIntrinsicHeight(10.0), 90.0),
          "入参扣 padding 后为负时截到 0（上游 math.max(0, ...)）");

    RenderPadding bare(EdgeInsets{10.0, 20.0, 30.0, 40.0});
    check(nearEq(bare.getMaxIntrinsicWidth(kInf), 40.0) &&
                  nearEq(bare.getMaxIntrinsicHeight(kInf), 60.0),
          "无子节点的 Padding 取 padding 本身");

    // ── ConstrainedBox：紧约束直取，否则把子的 intrinsic 过一遍附加约束 ──
    FixedBox small(40.0, 30.0);
    RenderConstrainedBox tightBox(BoxConstraints::tight(50.0, 60.0));
    tightBox.insertChild(&small, nullptr);
    check(nearEq(tightBox.getMinIntrinsicWidth(kInf), 50.0) &&
                  nearEq(tightBox.getMaxIntrinsicHeight(kInf), 60.0),
          "附加约束为紧约束时直接取该值（无视子节点）");

    FixedBox big(40.0, 30.0);
    RenderConstrainedBox clampBox(BoxConstraints(0.0, 20.0, 0.0, 100.0));
    clampBox.insertChild(&big, nullptr);
    check(nearEq(clampBox.getMaxIntrinsicWidth(kInf), 20.0) &&
                  nearEq(clampBox.getMaxIntrinsicHeight(kInf), 30.0),
          "附加约束有界非紧时夹住子的 intrinsic");

    FixedBox pass(40.0, 30.0);
    RenderConstrainedBox looseBox(BoxConstraints::unbounded());
    looseBox.insertChild(&pass, nullptr);
    check(nearEq(looseBox.getMaxIntrinsicWidth(kInf), 40.0), "附加约束无界时原样透传");

    // ── Align：转发后乘本轴因子 ──
    FixedBox kid(40.0, 30.0);
    RenderAlign shrink(0.0, 0.0, std::nullopt, 0.5);
    shrink.insertChild(&kid, nullptr);
    check(nearEq(shrink.getMaxIntrinsicHeight(kInf), 15.0) &&
                  nearEq(shrink.getMaxIntrinsicWidth(kInf), 40.0),
          "heightFactor 只缩放高度 intrinsic");

    FixedBox kid2(40.0, 30.0);
    RenderAlign wide(0.0, 0.0, 2.0, std::nullopt);
    wide.insertChild(&kid2, nullptr);
    check(nearEq(wide.getMinIntrinsicWidth(0.0), 80.0), "widthFactor 缩放宽度 intrinsic");

    FixedBox plain(40.0, 30.0);
    RenderAlign noFactor;
    noFactor.insertChild(&plain, nullptr);
    check(nearEq(noFactor.getMaxIntrinsicWidth(kInf), 40.0),
          "未给因子时因子按 1 计");

    // ── Stack：只算非 positioned 子，取最大值 ──
    FixedBox a(40.0, 30.0), b(20.0, 50.0), oversized(900.0, 900.0);
    RenderStack stack;
    stack.insertChild(&a, nullptr);
    stack.insertChild(&b, &a);
    stack.insertChild(&oversized, &b);
    stack.setPositioned(2, true);
    check(nearEq(stack.getMaxIntrinsicWidth(kInf), 40.0) &&
                  nearEq(stack.getMaxIntrinsicHeight(kInf), 50.0),
          "Stack 取非 positioned 子节点的最大值");
    check(nearEq(stack.getMaxIntrinsicWidth(kInf), 40.0),
          "positioned 子节点完全不参与 intrinsic");

    // ── Paragraph：单行 = width-in-height-out ──
    RenderParagraph para(TextSpan::make("Hello World"));
    para.layout(BoxConstraints::loose(400.0, 400.0));
    check(nearEq(para.getMinIntrinsicWidth(0.0), para.getMaxIntrinsicWidth(kInf)) &&
                  nearEq(para.getMaxIntrinsicWidth(kInf), para.contentWidth()),
          "Paragraph 宽度：min == max == 不换行的自然宽");
    check(nearEq(para.getMinIntrinsicHeight(10.0), para.getMaxIntrinsicHeight(kInf)) &&
                  nearEq(para.getMaxIntrinsicHeight(kInf), para.size().height),
          "Paragraph 高度：min == max == 布局出来的行高");

    // ── RenderIntrinsicHeight ──
    WrappingBox wrap(100.0, 40.0, 10.0, 60.0);
    RenderIntrinsicHeight ih;
    ih.insertChild(&wrap, nullptr);
    check(nearEq(ih.getMaxIntrinsicWidth(kInf), 100.0),
          "IntrinsicHeight 问宽时先把无界高度换成子的最大 intrinsic 高");
    check(nearEq(ih.getMinIntrinsicHeight(50.0), 40.0),
          "IntrinsicHeight 的 min 高等于 max 高（上游只覆写 min）");
    check(nearEq(ih.getMaxIntrinsicHeight(50.0), 40.0),
          "IntrinsicHeight 的 max 高转发子节点");

    // ── RenderIntrinsicWidth（含 step）──
    FixedBox iwKid(40.0, 30.0);
    RenderIntrinsicWidth iw;
    iw.insertChild(&iwKid, nullptr);
    check(nearEq(iw.getMaxIntrinsicWidth(kInf), 40.0) &&
                  nearEq(iw.getMinIntrinsicWidth(kInf), 40.0),
          "IntrinsicWidth 无 step 时 min == max == 子的 max 宽");

    FixedBox stepKid(40.0, 30.0);
    RenderIntrinsicWidth iwStep(30.0, 25.0);
    iwStep.insertChild(&stepKid, nullptr);
    check(nearEq(iwStep.getMaxIntrinsicWidth(kInf), 60.0),
          "stepWidth 把宽度 intrinsic 向上取整到步长倍数");
    check(nearEq(iwStep.getMaxIntrinsicHeight(kInf), 50.0),
          "stepHeight 把高度 intrinsic 向上取整到步长倍数");
}

// ── C. RenderFlex 的 _getIntrinsicSize ──────────────────────────────────────
void testFlexIntrinsics() {
    std::puts("[C] RenderFlex intrinsic");

    // 主轴 = 非 flex 子的自然尺寸累加；交叉轴 = 子的最大交叉尺寸。
    FixedBox c1(40.0, 30.0), c2(20.0, 50.0);
    RenderFlex column(Axis::Vertical);
    column.insertChild(&c1, nullptr);
    column.insertChild(&c2, &c1);
    check(nearEq(column.getMaxIntrinsicHeight(kInf), 80.0),
          "Column 主轴（高）intrinsic = 各子高度之和");
    check(nearEq(column.getMaxIntrinsicWidth(kInf), 40.0),
          "Column 交叉轴（宽）intrinsic = 各子宽度最大");

    // min 与 max 走的是不同的子节点入口，不能混。
    RangeBox r1(10.0, 100.0, 5.0, 50.0);
    RenderFlex rangeCol(Axis::Vertical);
    rangeCol.insertChild(&r1, nullptr);
    check(nearEq(rangeCol.getMinIntrinsicHeight(kInf), 5.0) &&
                  nearEq(rangeCol.getMaxIntrinsicHeight(kInf), 50.0),
          "Column 主轴 min/max 分别取子的 min/max");
    check(nearEq(rangeCol.getMinIntrinsicWidth(kInf), 10.0) &&
                  nearEq(rangeCol.getMaxIntrinsicWidth(kInf), 100.0),
          "Column 交叉轴 min/max 分别取子的 min/max");

    // flex 子折算成「单位 flex 所需长度的最大值 × totalFlex」。
    FixedBox f1(10.0, 10.0), f2(10.0, 30.0);
    RenderFlex weighted(Axis::Vertical);
    weighted.insertChild(&f1, nullptr);
    weighted.insertChild(&f2, &f1);
    static_cast<FlexParentData*>(f1.parentData())->flex = 2;
    check(weighted.flexOf(0) == 2 && weighted.flexOf(1) == 0,
          "flex 权重写在子节点的 FlexParentData 里");
    // 10/2 = 5 → 5 × 2 + 30 = 40
    check(nearEq(weighted.getMaxIntrinsicHeight(kInf), 40.0),
          "主轴 intrinsic：maxFlexFraction × totalFlex + inflexibleSpace");

    // 交叉轴：先复算主轴分配，再在该主轴长度下问子的交叉轴 intrinsic。
    WrappingBox w1(100.0, 40.0, 10.0, 60.0);
    RenderFlex row(Axis::Horizontal);
    row.insertChild(&w1, nullptr);
    static_cast<FlexParentData*>(w1.parentData())->flex = 1;
    check(nearEq(row.getMaxIntrinsicHeight(50.0), 40.0),
          "交叉轴按分配到的主轴长度问子节点（50 → 换行 → 高 40）");
    check(nearEq(row.getMaxIntrinsicHeight(100.0), 10.0),
          "主轴给到 100 时子节点一行放下（高 10）");
    check(nearEq(row.getMaxIntrinsicHeight(kInf), 10.0),
          "主轴无界时 flex 子按自身最大 intrinsic 处理（上游 canFlex 规则）");

    // 混合 flex / 非 flex 的主轴求和。
    FixedBox hx(30.0, 10.0);
    WrappingBox hw(100.0, 40.0, 10.0, 60.0);
    RenderFlex mixed(Axis::Horizontal);
    mixed.insertChild(&hx, nullptr);
    mixed.insertChild(&hw, &hx);
    static_cast<FlexParentData*>(hw.parentData())->flex = 2;
    // 100/2 = 50 → 50 × 2 + 30 = 130
    check(nearEq(mixed.getMaxIntrinsicWidth(kInf), 130.0),
          "Row 主轴：flex 折算 + 非 flex 原样累加");

    // 空 flex
    RenderFlex bare(Axis::Vertical);
    check(nearEq(bare.getMaxIntrinsicHeight(kInf), 0.0) &&
                  nearEq(bare.getMaxIntrinsicWidth(kInf), 0.0),
          "空 Flex 的 intrinsic 为 0");
}

// ── D. intrinsic 依赖 × relayout boundary 耦合 ──────────────────────────────
void testIntrinsicInvalidation() {
    std::puts("[D] intrinsic 依赖与重排的耦合");

    PipelineOwner po;
    BoundaryBox root;
    ContainerBox mid;
    CountingBox queried;   // 边界叶子 + 被查过 intrinsic
    PlainBox untouched;    // 边界叶子 + 从未被查
    untouched.setBoundary(true);

    root.insertChild(&mid, nullptr);
    mid.insertChild(&queried, nullptr);
    mid.insertChild(&untouched, &queried);
    po.setRootNode(&root);

    // 起点：所有节点干净，脏标只可能由 markNeedsLayout 产生。
    mid.clearNeedsLayout();
    queried.clearNeedsLayout();
    untouched.clearNeedsLayout();
    root.clearNeedsLayout();

    queried.getMinIntrinsicHeight(10.0);
    queried.markNeedsLayout();
    check(mid.needsLayout(),
          "被查过 intrinsic 的边界子节点标脏时，脏标交给父节点");

    mid.clearNeedsLayout();
    root.clearNeedsLayout();
    untouched.markNeedsLayout();
    check(!mid.needsLayout(),
          "未建立 intrinsic 依赖的边界子节点不会把脏标上抛给父节点");
    check(untouched.needsLayout(), "该子节点自身仍然被标脏");

    po.clearRootNode(&root);
}

// ── E. 控件层 ───────────────────────────────────────────────────────────────
void testWidgets() {
    std::puts("[E] IntrinsicHeight / IntrinsicWidth 控件");

    auto twoBoxes = [] {
        return std::vector<SharedPtr<Widget>>{
                cui::make_shared<SizedBox>(40.0, 30.0),
                cui::make_shared<SizedBox>(20.0, 50.0)};
    };

    // 对照：Center 给松约束，Column 主轴占满（MainAxisSize.max）。
    {
        Harness h;
        h.attach(cui::make_shared<Align>(0.0, 0.0,
                                         cui::make_shared<Column>(twoBoxes())),
                 400.0, 400.0);
        h.rebuildAndLayout();
        auto* flex = findRo<RenderFlex>(h.root);
        check(flex && nearEq(flex->size().width, 40.0) && nearEq(flex->size().height, 400.0),
              "Column 在松约束下主轴撑满（40×400）");
    }
    {
        Harness h;
        h.attach(cui::make_shared<Align>(
                         0.0, 0.0, cui::make_shared<IntrinsicHeight>(
                                           cui::make_shared<Column>(twoBoxes()))),
                 400.0, 400.0);
        h.rebuildAndLayout();
        auto* ih = findRo<RenderIntrinsicHeight>(h.root);
        auto* flex = findRo<RenderFlex>(h.root);
        check(flex && nearEq(flex->size().width, 40.0) && nearEq(flex->size().height, 80.0),
              "IntrinsicHeight 把 Column 的高收成最大 intrinsic 高（40×80）");
        check(ih && nearEq(ih->size().height, 80.0), "IntrinsicHeight 自身尺寸 = 子节点尺寸");
    }

    // IntrinsicWidth：把 Row 的宽收成最大 intrinsic 宽。
    {
        Harness h;
        h.attach(cui::make_shared<Align>(0.0, 0.0,
                                         cui::make_shared<IntrinsicWidth>(
                                                 std::nullopt, std::nullopt,
                                                 cui::make_shared<Row>(twoBoxes()))),
                 400.0, 400.0);
        h.rebuildAndLayout();
        auto* iw = findRo<RenderIntrinsicWidth>(h.root);
        auto* flex = findRo<RenderFlex>(h.root);
        check(flex && nearEq(flex->size().width, 60.0) && nearEq(flex->size().height, 50.0),
              "IntrinsicWidth 把 Row 的宽收成 60（高仍取子节点最大 50）");
        check(iw && nearEq(iw->size().width, 60.0), "IntrinsicWidth 自身尺寸 = 子节点尺寸");
    }
    {
        Harness h;
        h.attach(cui::make_shared<Align>(0.0, 0.0,
                                         cui::make_shared<IntrinsicWidth>(
                                                 100.0, std::nullopt,
                                                 cui::make_shared<Row>(twoBoxes()))),
                 400.0, 400.0);
        h.rebuildAndLayout();
        auto* flex = findRo<RenderFlex>(h.root);
        check(flex && nearEq(flex->size().width, 100.0), "stepWidth 把宽抬到步长倍数（100）");
    }
    {
        Harness h;
        h.attach(cui::make_shared<Align>(0.0, 0.0,
                                         cui::make_shared<IntrinsicWidth>(
                                                 std::nullopt, 30.0,
                                                 cui::make_shared<Row>(twoBoxes()))),
                 400.0, 400.0);
        h.rebuildAndLayout();
        auto* flex = findRo<RenderFlex>(h.root);
        check(flex && nearEq(flex->size().width, 60.0) && nearEq(flex->size().height, 60.0),
              "stepHeight 把高抬到步长倍数（50 → 60），宽度不受影响");
    }

    // 控件层的 step 归一化：上游把 0.0 当作「不给」。
    {
        auto w = cui::make_shared<IntrinsicWidth>(0.0, 0.0, nullptr);
        check(!w->stepWidth().has_value() && !w->stepHeight().has_value(),
              "IntrinsicWidth 的 stepWidth/stepHeight = 0.0 归一化为 null（上游同义）");
    }

    // 父约束已是紧高度时，IntrinsicHeight 尊重它（上游 _childConstraints 早退）。
    {
        Harness h;
        h.attach(cui::make_shared<SizedBox>(
                         400.0, 400.0,
                         cui::make_shared<IntrinsicHeight>(cui::make_shared<Column>(twoBoxes()))),
                 400.0, 400.0);
        h.rebuildAndLayout();
        auto* flex = findRo<RenderFlex>(h.root);
        check(flex && nearEq(flex->size().height, 400.0),
              "紧高度约束下 IntrinsicHeight 原样下发（不覆写高度）");
    }
}

}  // namespace

int main() {
    std::puts("== intrinsic 尺寸协议测试 ==");
    testProtocol();
    testBoxFormulas();
    testFlexIntrinsics();
    testIntrinsicInvalidation();
    testWidgets();
    if (g_failures == 0) {
        std::printf("ALL PASS (%d failures)\n", g_failures);
        return 0;
    }
    std::printf("FAILED: %d checks\n", g_failures);
    return 1;
}
