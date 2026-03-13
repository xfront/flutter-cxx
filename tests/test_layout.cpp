/// @file test_layout.cpp
/// @brief 上游 第二批布局 / 绘制 Widget 单元测试。
///
/// 覆盖：
///   1. Matrix4：translate / scale / rotateZ / 乘法 / 逆变换
///   2. Transform：布局透传、变换字段、命中测试逆变换
///   3. RotatedBox：约束与尺寸翻轴（quarterTurns 奇数）
///   4. FittedBox：BoxFit.Contain 按比例缩放
///   5. Baseline：子节点基线顶到指定位置
///   6. LimitedBox：父约束无界时才顶到 maxWidth
///   7. OverflowBox / SizedOverflowBox / UnconstrainedBox：溢出与解除约束
///   8. Offstage / Visibility：不占空间、不命中
///   9. RepaintBoundary：isRepaintBoundary + 尺寸透传
///  10. IndexedStack / Spacer：只显示 index 项 / 占满剩余空间
///  11. Wrap：单行与换行（run 切分 + runSpacing）
///  12. ClipRect / ClipOval / CustomPaint / Image：尺寸与字段
///  13. CrossAxisAlignment::Stretch 的交叉轴紧约束
///  14. RenderFlex 主轴无界时 flex 子按非 flex 处理（上游 canFlex）
///
/// 全部 headless（无窗口）。只验证布局 / 尺寸 / 命中（确定性），不验证像素。

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/paint.h"
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

bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

/// 把 Widget 树接入 BuildOwner + RenderView + PipelineOwner，并布局好。
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

    /// 深度优先取树中第一个指定类型的 RenderObject（root 自身优先）。
    template <typename T>
    T* findRenderObject() {
        RenderObject* ro = root ? root->findRenderObject() : nullptr;
        if (auto* hit = dynamic_cast<T*>(ro))
            return hit;
        if (!ro)
            return nullptr;
        T* found = nullptr;
        std::vector<RenderObject*> stack{ro};
        while (!stack.empty() && !found) {
            RenderObject* n = stack.back();
            stack.pop_back();
            if (auto* hit = dynamic_cast<T*>(n)) {
                found = hit;
                break;
            }
            n->visitChildren([&](AbstractNode* c) {
                if (c)
                    stack.push_back(static_cast<RenderObject*>(c));
            });
        }
        return found;
    }

    /// 取树中第 index 个（先序）指定类型的 RenderObject。
    template <typename T>
    T* findRenderObjectAt(int index) {
        RenderObject* ro = root ? root->findRenderObject() : nullptr;
        if (!ro)
            return nullptr;
        int seen = 0;
        std::vector<RenderObject*> stack{ro};
        while (!stack.empty()) {
            RenderObject* n = stack.back();
            stack.pop_back();
            if (auto* hit = dynamic_cast<T*>(n)) {
                if (seen == index)
                    return hit;
                ++seen;
            }
            std::vector<AbstractNode*> kids;
            n->visitChildren([&](AbstractNode* c) {
                if (c)
                    kids.push_back(c);
            });
            for (auto it = kids.rbegin(); it != kids.rend(); ++it)
                stack.push_back(static_cast<RenderObject*>(*it));
        }
        return nullptr;
    }
};

// ── 1. Matrix4 ──────────────────────────────────────────────────────────────
void testMatrix4() {
    std::printf("[test] Matrix4\n");

    Matrix4 t = Matrix4::translation(10, 20);
    Offset p = t.transformOffset({1, 2});
    check(near(p.dx, 11) && near(p.dy, 22), "translation 平移生效");

    Matrix4 s = Matrix4::scale(2, 3);
    Offset ps = s.transformOffset({1, 1});
    check(near(ps.dx, 2) && near(ps.dy, 3), "scale 缩放生效");

    Matrix4 r = Matrix4::rotationZ(M_PI / 2.0);
    Offset pr = r.transformOffset({1, 0});
    check(near(pr.dx, 0, 1e-9) && near(pr.dy, 1, 1e-9), "rotationZ(90°) 把 (1,0) 转到 (0,1)");

    // 右乘：this = this * other → 先缩放再平移。
    Matrix4 m = Matrix4::identity();
    m.translate(5, 7);
    m.scaleBy(2, 2);
    Offset pm = m.transformOffset({1, 1});
    check(near(pm.dx, 7) && near(pm.dy, 9), "multiply 是右乘（先缩放后平移）");

    // 往返：逆变换回到原点。
    Matrix4 inv = m.inverse();
    Offset back = inv.transformOffset(m.transformOffset({3, 4}));
    check(near(back.dx, 3) && near(back.dy, 4), "inverse 往返一致");

    check(Matrix4::identity().isIdentity(), "identity 判定");
}

// ── 2. Transform ────────────────────────────────────────────────────────────
void testTransform() {
    std::printf("[test] Transform\n");

    // 子节点必须自身可命中（RenderDecoratedBox 覆写了 hitTestSelf）；
    // 纯 SizedBox 在 上游 里同样不参与命中测试。
    BoxDecoration dec;
    dec.color = Color::fromRGB(1, 0, 0);
    auto child = cui::make_shared<DecoratedBox>(dec, cui::make_shared<SizedBox>(100, 60));
    auto tr = cui::make_shared<Transform>(Matrix4::scale(0.5, 0.5), child);
    Harness h;
    h.attach(cui::make_shared<Center>(tr), 400, 400);

    RenderTransform* rt = h.findRenderObject<RenderTransform>();
    check(rt != nullptr, "RenderTransform 已创建");
    check(rt && near(rt->size().width, 100) && near(rt->size().height, 60),
          "布局透传子节点尺寸 100×60");

    // 命中测试：缩放 0.5 → 盒内 (25,25) 对应子节点 (50,50)，(75,75) 越界。
    if (rt) {
        std::vector<RenderObject*> hit;
        bool in = rt->hitTest({25, 25}, hit);
        check(in, "缩放后 (25,25) 命中子节点");
        std::vector<RenderObject*> hit2;
        bool out = rt->hitTest({75, 75}, hit2);
        check(!out, "缩放后 (75,75) 落在子节点之外");
    }

    // transformHitTests = false 时按未变换的坐标系命中。
    auto tr2 = cui::make_shared<Transform>(
            Matrix4::scale(0.5, 0.5),
            cui::make_shared<DecoratedBox>(dec, cui::make_shared<SizedBox>(100, 60)));
    tr2->setTransformHitTests(false);
    Harness h2;
    h2.attach(cui::make_shared<Center>(tr2), 400, 400);
    RenderTransform* rt2 = h2.findRenderObject<RenderTransform>();
    std::vector<RenderObject*> hit3;
    check(rt2 && rt2->hitTest({75, 20}, hit3), "transformHitTests=false 时按原坐标命中");
}

// ── 3. RotatedBox ───────────────────────────────────────────────────────────
void testRotatedBox() {
    std::printf("[test] RotatedBox\n");

    auto rb = cui::make_shared<RotatedBox>(1, cui::make_shared<SizedBox>(100, 40));
    Harness h;
    h.attach(cui::make_shared<Center>(rb), 400, 400);

    RenderRotatedBox* r = h.findRenderObject<RenderRotatedBox>();
    check(r != nullptr, "RenderRotatedBox 已创建");
    // 奇数个 90°：尺寸翻轴 → 40×100。
    check(r && near(r->size().width, 40) && near(r->size().height, 100),
          "quarterTurns=1 时尺寸翻轴为 40×100");

    // 偶数：不翻轴。
    auto rb2 = cui::make_shared<RotatedBox>(2, cui::make_shared<SizedBox>(100, 40));
    Harness h2;
    h2.attach(cui::make_shared<Center>(rb2), 400, 400);
    RenderRotatedBox* r2 = h2.findRenderObject<RenderRotatedBox>();
    check(r2 && near(r2->size().width, 100) && near(r2->size().height, 40),
          "quarterTurns=2 时尺寸不变 100×40");
}

// ── 4. FittedBox ────────────────────────────────────────────────────────────
void testFittedBox() {
    std::printf("[test] FittedBox\n");

    // 子节点 200×100，父约束 0..100 → contain 等比缩到 100×50。
    auto fb = cui::make_shared<FittedBox>(BoxFit::Contain, Alignment::center(),
                                          cui::make_shared<SizedBox>(200, 100));
    Harness h;
    h.attach(cui::make_shared<Center>(fb), 100, 100);

    RenderFittedBox* r = h.findRenderObject<RenderFittedBox>();
    check(r != nullptr, "RenderFittedBox 已创建");
    check(r && near(r->size().width, 100) && near(r->size().height, 50),
          "Contain 等比缩放到 100×50");

    // 自身尺寸由约束决定（与 fit 无关）；fit 只影响绘制缩放。
    // 父节点给紧约束 100×100 → 两种 fit 的自有尺寸都是 100×100，
    // 但 Contain 等比缩放（0.5/0.5）、Fill 非等比拉伸（0.5/1.0）。
    auto fitted = [](BoxFit fit, Harness& hh) {
        auto fb = cui::make_shared<FittedBox>(fit, Alignment::center(),
                                              cui::make_shared<SizedBox>(200, 100));
        hh.attach(cui::make_shared<Center>(cui::make_shared<SizedBox>(100, 100, fb)), 400, 400);
        return hh.findRenderObject<RenderFittedBox>();
    };
    Harness hFill;
    RenderFittedBox* rf = fitted(BoxFit::Fill, hFill);
    check(rf && near(rf->size().width, 100) && near(rf->size().height, 100),
          "紧约束下自身尺寸 = 100×100");
    check(rf && near(rf->childTransform().at(0, 0), 0.5) &&
                  near(rf->childTransform().at(1, 1), 1.0),
          "Fill 非等比拉伸：scaleX=0.5 / scaleY=1.0");
    Harness hContain;
    RenderFittedBox* rc = fitted(BoxFit::Contain, hContain);
    check(rc && near(rc->childTransform().at(0, 0), 0.5) &&
                  near(rc->childTransform().at(1, 1), 0.5),
          "Contain 等比缩放：scaleX=scaleY=0.5");
}

// ── 5. Baseline ─────────────────────────────────────────────────────────────
void testBaseline() {
    std::printf("[test] Baseline\n");

    // 子节点无基线信息 → 退化为「底边即基线」：top = 40 - 20 = 20。
    auto b = cui::make_shared<Baseline>(40.0, BaselineType::Alphabetic,
                                        cui::make_shared<SizedBox>(30, 20));
    Harness h;
    h.attach(cui::make_shared<Center>(b), 400, 400);

    RenderBaseline* r = h.findRenderObject<RenderBaseline>();
    check(r != nullptr, "RenderBaseline 已创建");
    check(r && near(r->size().width, 30), "宽度 = 子节点宽 30");
    check(r && near(r->size().height, 40), "高度 = top(20) + 子节点高(20) = 40");
    check(r && r->child() && r->child()->parentData() &&
                  near(r->child()->parentData()->offset.dy, 20),
          "子节点偏移 dy = 20（基线落在 40）");
}

// ── 6. LimitedBox ───────────────────────────────────────────────────────────
void testLimitedBox() {
    std::printf("[test] LimitedBox\n");

    // Row 给子节点的是主轴无界约束 → LimitedBox 顶到 maxWidth=50。
    // LimitedBox 给子节点 (0..50, 0..20) 的有界约束；SizedBox(200,20) 的
    // 紧约束会被 enforce clamp 进父区间 → 50×20（上游的 enforce 语义：
    // 父级上限优先，SizedBox 无法撑破有界父约束）。
    auto shrinkable = cui::make_shared<SizedBox>(200.0, 20.0);
    std::vector<SharedPtr<Widget>> kids{
        cui::make_shared<LimitedBox>(50.0, BoxConstraints::kInfinity, shrinkable)};
    auto row = cui::make_shared<Row>(kids);
    Harness h;
    h.attach(row, 400, 400);

    RenderLimitedBox* r = h.findRenderObject<RenderLimitedBox>();
    check(r != nullptr, "RenderLimitedBox 已创建");
    check(r && near(r->size().width, 50), "主轴无界时宽度被顶到 50");
    check(r && near(r->size().height, 20), "交叉轴不受影响 = 20");
}

// ── 7. OverflowBox / SizedOverflowBox / UnconstrainedBox ────────────────────
void testOverflowBoxes() {
    std::printf("[test] OverflowBox / SizedOverflowBox / UnconstrainedBox\n");

    // OverflowBox：自身 400×400（父 biggest），子节点按 300×200 居中摆放。
    auto ob = cui::make_shared<OverflowBox>(std::nullopt, std::optional<double>(300.0),
                                            std::nullopt, std::optional<double>(200.0),
                                            Alignment::center(),
                                            cui::make_shared<SizedBox>(300, 200));
    Harness h;
    h.attach(cui::make_shared<Center>(ob), 400, 400);
    RenderOverflowBox* r = h.findRenderObject<RenderOverflowBox>();
    check(r != nullptr, "RenderOverflowBox 已创建");
    check(r && near(r->size().width, 400) && near(r->size().height, 400),
          "自身尺寸 = 父约束 biggest 400×400");
    check(r && r->child() && r->child()->parentData() &&
                  near(r->child()->parentData()->offset.dx, 50) &&
                  near(r->child()->parentData()->offset.dy, 100),
          "子节点居中偏移 (50,100)");

    // SizedOverflowBox：自身固定 100×100，子节点 200×200 溢出并居中 → (-50,-50)。
    auto sob = cui::make_shared<SizedOverflowBox>(Size(100, 100), Alignment::center(),
                                                  cui::make_shared<SizedBox>(200, 200));
    Harness h2;
    h2.attach(cui::make_shared<Center>(sob), 400, 400);
    RenderSizedOverflowBox* r2 = h2.findRenderObject<RenderSizedOverflowBox>();
    check(r2 && near(r2->size().width, 100) && near(r2->size().height, 100),
          "SizedOverflowBox 固定为 100×100");
    check(r2 && r2->child() && r2->child()->parentData() &&
                  near(r2->child()->parentData()->offset.dx, -50) &&
                  near(r2->child()->parentData()->offset.dy, -50),
          "溢出的子节点居中偏移 (-50,-50)");

    // UnconstrainedBox：紧约束父节点下子节点仍保持自然尺寸，并被居中摆放。
    auto ub = cui::make_shared<UnconstrainedBox>(std::nullopt, Alignment::center(),
                                                 cui::make_shared<SizedBox>(200, 100));
    Harness h3;
    h3.attach(ub, 800, 800);
    RenderUnconstrainedBox* r3 = h3.findRenderObject<RenderUnconstrainedBox>();
    check(r3 && near(r3->size().width, 800) && near(r3->size().height, 800),
          "自身尺寸仍受父约束（800×800）");
    check(r3 && r3->child() && near(r3->child()->size().width, 200) &&
                  near(r3->child()->size().height, 100),
          "子节点拿到无约束，保持 200×100");
    check(r3 && r3->child() && r3->child()->parentData() &&
                  near(r3->child()->parentData()->offset.dx, 300) &&
                  near(r3->child()->parentData()->offset.dy, 350),
          "子节点在自身盒子内居中 (300,350)");
}

// ── 8. Offstage / Visibility ────────────────────────────────────────────────
void testOffstageVisibility() {
    std::printf("[test] Offstage / Visibility\n");

    auto off = cui::make_shared<Offstage>(true, cui::make_shared<SizedBox>(50, 30));
    Harness h;
    h.attach(cui::make_shared<Center>(off), 400, 400);
    RenderOffstage* r = h.findRenderObject<RenderOffstage>();
    check(r != nullptr, "RenderOffstage 已创建");
    check(r && near(r->size().width, 0) && near(r->size().height, 0),
          "offstage 时不占空间（0×0）");
    std::vector<RenderObject*> hit;
    check(r && !r->hitTest({10, 10}, hit), "offstage 时不参与命中测试");

    auto on = cui::make_shared<Offstage>(false, cui::make_shared<SizedBox>(50, 30));
    Harness h2;
    h2.attach(cui::make_shared<Center>(on), 400, 400);
    RenderOffstage* r2 = h2.findRenderObject<RenderOffstage>();
    check(r2 && near(r2->size().width, 50) && near(r2->size().height, 30),
          "offstage=false 时透传 50×30");

    // Visibility：不可见 → 子树是 Offstage。
    auto vis = cui::make_shared<Visibility>(false, cui::make_shared<SizedBox>(50, 30));
    Harness h3;
    h3.attach(cui::make_shared<Center>(vis), 400, 400);
    check(h3.findRenderObject<RenderOffstage>() != nullptr, "Visibility(false) 产出 Offstage");

    // 可见 → 不含 Offstage，直接是子节点。
    auto vis2 = cui::make_shared<Visibility>(true, cui::make_shared<SizedBox>(50, 30));
    Harness h4;
    h4.attach(cui::make_shared<Center>(vis2), 400, 400);
    check(h4.findRenderObject<RenderOffstage>() == nullptr, "Visibility(true) 不含 Offstage");
    check(h4.findRenderObject<RenderConstrainedBox>() != nullptr,
          "Visibility(true) 直接是子节点 SizedBox");
}

// ── 9. RepaintBoundary ──────────────────────────────────────────────────────
void testRepaintBoundary() {
    std::printf("[test] RepaintBoundary\n");

    auto rb = cui::make_shared<RepaintBoundary>(cui::make_shared<SizedBox>(80, 40));
    Harness h;
    h.attach(cui::make_shared<Center>(rb), 400, 400);
    RenderRepaintBoundary* r = h.findRenderObject<RenderRepaintBoundary>();
    check(r != nullptr, "RenderRepaintBoundary 已创建");
    check(r && r->isRepaintBoundary(), "isRepaintBoundary() = true");
    check(r && near(r->size().width, 80) && near(r->size().height, 40),
          "尺寸透传 80×40");
}

// ── 10. IndexedStack / Spacer ───────────────────────────────────────────────
void testIndexedStackSpacer() {
    std::printf("[test] IndexedStack / Spacer\n");

    std::vector<SharedPtr<Widget>> kids{cui::make_shared<SizedBox>(10, 10),
                                              cui::make_shared<SizedBox>(20, 20),
                                              cui::make_shared<SizedBox>(30, 30)};
    auto is = cui::make_shared<IndexedStack>(1, kids);
    Harness h;
    h.attach(cui::make_shared<Center>(is), 400, 400);
    RenderStack* s = h.findRenderObject<RenderStack>();
    check(s != nullptr, "IndexedStack 产出 RenderStack");
    // 非 index 项被 Offstage（0×0），Stack 尺寸 = 当前项的 20×20。
    check(s && near(s->size().width, 20) && near(s->size().height, 20),
          "Stack 尺寸 = 第 1 项的 20×20");

    // Spacer：Row 里两个 20 宽的孩子中间夹一个 Spacer，剩余 760 全给它。
    std::vector<SharedPtr<Widget>> rowKids{
        cui::make_shared<SizedBox>(20, 10), cui::make_shared<Spacer>(1),
        cui::make_shared<SizedBox>(20, 10)};
    auto row = cui::make_shared<Row>(rowKids);
    Harness h2;
    h2.attach(row, 800, 800);
    // Spacer 自身是零尺寸的 SizedBox.shrink()，但它**占掉的份额**决定后续
    // 兄弟节点的落点：最后一个子节点应被推到 x = 20 + 760 = 780。
    RenderConstrainedBox* lastBox = h2.findRenderObjectAt<RenderConstrainedBox>(2);
    check(lastBox != nullptr, "Row 里最后一个 SizedBox 已挂载");
    check(lastBox && lastBox->parentData() && near(lastBox->parentData()->offset.dx, 780),
          "Spacer 占满剩余 760，末项被推到 x=780");
}

// ── 11. Wrap ────────────────────────────────────────────────────────────────
void testWrap() {
    std::printf("[test] Wrap\n");

    // 单行：3 个 60×30，spacing 10 → 主 200，交叉 30。
    std::vector<SharedPtr<Widget>> kids{cui::make_shared<SizedBox>(60, 30),
                                              cui::make_shared<SizedBox>(60, 30),
                                              cui::make_shared<SizedBox>(60, 30)};
    auto w = cui::make_shared<Wrap>(Axis::Horizontal, kids, WrapAlignment::Start, 10.0,
                                    WrapAlignment::Start, 10.0, WrapCrossAlignment::Start);
    Harness h;
    h.attach(cui::make_shared<Center>(w), 400, 400);
    RenderWrap* r = h.findRenderObject<RenderWrap>();
    check(r != nullptr, "RenderWrap 已创建");
    check(r && r->runCount() == 1, "放得下时只有 1 个 run");
    check(r && near(r->size().width, 200) && near(r->size().height, 30),
          "单行尺寸 200×30（60*3 + 10*2）");

    // 换行：窗口 260，3 个 100×50 + spacing 10 → 第 1 行 2 个，第 2 行 1 个。
    std::vector<SharedPtr<Widget>> kids2{cui::make_shared<SizedBox>(100, 50),
                                               cui::make_shared<SizedBox>(100, 50),
                                               cui::make_shared<SizedBox>(100, 50)};
    auto w2 = cui::make_shared<Wrap>(Axis::Horizontal, kids2, WrapAlignment::Start, 10.0,
                                     WrapAlignment::Start, 10.0, WrapCrossAlignment::Start);
    Harness h2;
    h2.attach(cui::make_shared<Center>(w2), 260, 260);
    RenderWrap* r2 = h2.findRenderObject<RenderWrap>();
    check(r2 && r2->runCount() == 2, "放不下时切成 2 个 run");
    check(r2 && near(r2->size().width, 210) && near(r2->size().height, 110),
          "两行尺寸 210×110（交叉 = 50 + 10 + 50）");
    // 第 3 个子节点落在第 2 行：dy = 50 + 10 = 60。
    check(r2 && r2->children().size() == 3 && r2->children()[2]->parentData() &&
                  near(r2->children()[2]->parentData()->offset.dy, 60),
          "第 3 项换到第 2 行（dy = 60）");
}

// ── 12. ClipRect / ClipOval / CustomPaint / Image ───────────────────────────
void testPaintWidgets() {
    std::printf("[test] ClipRect / ClipOval / CustomPaint / Image\n");

    auto cr = cui::make_shared<ClipRect>(cui::make_shared<SizedBox>(50, 30));
    Harness h;
    h.attach(cui::make_shared<Center>(cr), 400, 400);
    RenderClipRect* r = h.findRenderObject<RenderClipRect>();
    check(r != nullptr, "RenderClipRect 已创建");
    check(r && near(r->size().width, 50) && near(r->size().height, 30), "ClipRect 透传 50×30");

    auto co = cui::make_shared<ClipOval>(cui::make_shared<SizedBox>(40, 40));
    Harness h2;
    h2.attach(cui::make_shared<Center>(co), 400, 400);
    RenderClipOval* r2 = h2.findRenderObject<RenderClipOval>();
    check(r2 && near(r2->size().width, 40) && near(r2->size().height, 40),
          "ClipOval 透传 40×40");

    // CustomPaint：无子节点 → 尺寸 = preferredSize。
    auto cp = cui::make_shared<CustomPaint>(nullptr, nullptr, Size(60, 40));
    Harness h3;
    h3.attach(cui::make_shared<Center>(cp), 400, 400);
    RenderCustomPaint* r3 = h3.findRenderObject<RenderCustomPaint>();
    check(r3 != nullptr, "RenderCustomPaint 已创建");
    check(r3 && near(r3->size().width, 60) && near(r3->size().height, 40),
          "CustomPaint 尺寸 = preferredSize 60×40");

    // Image：无图像且无显式尺寸 → 取约束 smallest。
    auto img = cui::make_shared<Image>(nullptr);
    Harness h4;
    h4.attach(cui::make_shared<Center>(img), 400, 400);
    RenderImage* r4 = h4.findRenderObject<RenderImage>();
    check(r4 != nullptr, "RenderImage 已创建");
    check(r4 && near(r4->size().width, 0) && near(r4->size().height, 0),
          "无图像时尺寸 = smallest（0×0）");

    // 显式 width/height 生效。
    auto img2 = cui::make_shared<Image>(nullptr);
    img2->setWidth(80.0);
    img2->setHeight(60.0);
    Harness h5;
    h5.attach(cui::make_shared<Center>(img2), 400, 400);
    RenderImage* r5 = h5.findRenderObject<RenderImage>();
    check(r5 && near(r5->size().width, 80) && near(r5->size().height, 60),
          "显式 80×60 生效");
}

// ── 测试 13：CrossAxisAlignment::Stretch 的交叉轴紧约束 ─────────────────────
// 上游 `_constraintsForNonFlexChild` / `_constraintsForFlexChild` 里的
// `fillCrossAxis`：只有 stretch 会把交叉轴**下限**抬到 maxCross（⇒ 紧约束）。
// 这条决定了「一行能不能横向铺满去接住点击」——弹层里的选项行若铺不满，
// 空白处的命中就落到卡片自己身上（ComboBox 的选项行曾因此点不动）。
void testCrossAxisStretch() {
    std::printf("[test] CrossAxisAlignment::Stretch\n");

    // Center 让根上的紧窗口约束变松，SizedBox 的 200×300 才不会被顶掉。
    auto make = [](CrossAxisAlignment align, bool flexChild) {
        auto box = cui::make_shared<DecoratedBox>(BoxDecoration{});
        SharedPtr<Widget> child = box;
        if (flexChild)
            child = cui::make_shared<Expanded>(1, box);
        auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{child});
        col->setCrossAxisAlignment(align);
        return cui::make_shared<Center>(cui::make_shared<SizedBox>(200.0, 300.0, col));
    };

    Harness h1;
    h1.attach(make(CrossAxisAlignment::Start, false), 400, 400);
    RenderDecoratedBox* start = h1.findRenderObject<RenderDecoratedBox>();
    check(start != nullptr, "Start：子节点在场");
    if (start) {
        check(!start->constraints().hasTightWidth(), "Start：子交叉轴仍是松的");
        check(near(start->size().width, 0), "Start：子按自然宽（0）");
    }

    Harness h2;
    h2.attach(make(CrossAxisAlignment::Stretch, false), 400, 400);
    RenderDecoratedBox* stretched = h2.findRenderObject<RenderDecoratedBox>();
    check(stretched != nullptr, "Stretch：子节点在场");
    if (stretched) {
        check(stretched->constraints().hasTightWidth(), "Stretch：子收到紧宽度约束");
        check(near(stretched->size().width, 200), "Stretch：子被拉到 200 宽");
        check(near(stretched->size().height, 0), "Stretch：主轴不受影响（高仍 0）");
    }

    // flex 子走的是另一条分支（`_constraintsForFlexChild`），同样要吃到紧交叉轴。
    Harness h3;
    h3.attach(make(CrossAxisAlignment::Stretch, true), 400, 400);
    RenderDecoratedBox* flexed = h3.findRenderObject<RenderDecoratedBox>();
    check(flexed != nullptr, "Stretch + Expanded：子节点在场");
    if (flexed) {
        check(flexed->constraints().hasTightWidth(), "Stretch + Expanded：紧宽度约束");
        check(near(flexed->size().width, 200) && near(flexed->size().height, 300),
              "Stretch + Expanded：200×300 铺满");
    }

    // 交叉轴无界时拉不满（上游在 debug 里直接断言），这里必须退回松约束而不是
    // 造出 min > max 的非法约束：Row 里的非 flex Column 拿到的宽度上下限都是 inf。
    Harness h4;
    auto inner = cui::make_shared<Column>(
        std::vector<SharedPtr<Widget>>{cui::make_shared<DecoratedBox>(BoxDecoration{})});
    inner->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{inner});
    h4.attach(cui::make_shared<SizedBox>(200.0, 300.0, row), 400, 400);
    RenderDecoratedBox* unbounded = h4.findRenderObject<RenderDecoratedBox>();
    check(unbounded != nullptr, "交叉轴无界：子节点在场");
    if (unbounded) {
        check(!unbounded->constraints().hasTightWidth(),
              "交叉轴无界：stretch 不产出紧（更不产出非法）约束");
        check(unbounded->constraints().minWidth <= unbounded->constraints().maxWidth &&
                  unbounded->constraints().minHeight <= unbounded->constraints().maxHeight,
              "交叉轴无界：约束仍然合法（min <= max）");
    }
}

}  // namespace

// ── 14. RenderFlex：主轴无界时 flex 子按非 flex 处理（上游 canFlex）─────────
// 页面壳把内容放进竖向 SingleChildScrollView ⇒ Column 收到「高无界、宽紧」。
// 旧实现把 1e18 的剩余空间当**紧**约束塞给 Expanded，于是 Column 自身只按非
// flex 子求和（几十像素高）：Expanded 里的控件照样画出来，却被父盒
// `size().contains()` 挡在命中链之外（图表页标签点不动的根因）。
void testFlexUnboundedMainAxis() {
    std::printf("[test] Flex 主轴无界 / Expanded 退化\n");

    auto marker = cui::make_shared<Container>(
        cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{}));
    marker->width(200.0).height(120.0).decoration(BoxDecoration{Color::fromRGB(1, 1, 1)});

    std::vector<SharedPtr<Widget>> kids{
        cui::make_shared<SizedBox>(100.0, 30.0),
        cui::make_shared<Expanded>(1, marker)};
    auto col = cui::make_shared<Column>(kids);
    col->setMainAxisSize(MainAxisSize::Max);

    // Scrollable 需要一个真实控制器（本仓尚未接 PrimaryScrollController 兜底）。
    ScrollController ctrl;
    Harness h;
    h.attach(cui::make_shared<SingleChildScrollView>(&ctrl, col), 400, 800);

    auto* flex = h.findRenderObject<RenderFlex>();
    check(flex != nullptr, "Column 产出 RenderFlex");
    check(flex && near(flex->size().height, 150.0),
          "主轴无界：Column 高度 = 30 + 120（Expanded 取自然尺寸）");

    auto* deco = h.findRenderObject<RenderDecoratedBox>();
    check(deco != nullptr, "Expanded 里的装饰盒已挂载");
    check(deco && near(deco->size().height, 120.0),
          "Expanded 子节点拿到无界约束，高度是自己的 120（不是 1e18）");

    if (deco) {
        Offset c = deco->localToGlobal({100.0, 60.0});
        std::vector<RenderObject*> chain;
        h.rv.hitTest(c, chain);
        bool hit = false;
        for (auto* n : chain)
            hit = hit || (n == deco);
        check(hit, "Expanded 内容仍在命中链里（旧实现被父盒 30 高挡掉）");
    }
}

int main() {
    std::setbuf(stdout, nullptr);
    std::printf("=== test_layout ===\n");
    testMatrix4();
    testTransform();
    testRotatedBox();
    testFittedBox();
    testBaseline();
    testLimitedBox();
    testOverflowBoxes();
    testOffstageVisibility();
    testRepaintBoundary();
    testIndexedStackSpacer();
    testWrap();
    testPaintWidgets();
    testCrossAxisStretch();
    testFlexUnboundedMainAxis();

    if (g_failures == 0) {
        std::printf("all passed\n");
        return 0;
    }
    std::printf("%d FAILED\n", g_failures);
    return 1;
}
