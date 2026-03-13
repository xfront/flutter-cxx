/// @file test_implicit_animation.cpp
/// @brief 隐式动画控件族（widgets/implicit_animation.h）单元测试。
///
/// 覆盖：
///   1. ImplicitTweenDriver：目标变化即从「当前插值结果」重启动画、缓动、
///      重定向、jumpToTarget、零时长。
///   2. 动画结束时必须把终点值发布给子树（ticker 停掉前的最后一帧）。
///   3. AnimatedOpacity / AnimatedPadding / AnimatedAlign / AnimatedSlide /
///      AnimatedPositioned / AnimatedContainer 的收敛值。
///   4. AnimatedScale / AnimatedRotation 的枢轴：上游 alignment 默认
///      Alignment.center（绕盒子中心），且可覆写。
///   5. AnimatedSwitcher：对齐 animated_switcher.dart 的 _ChildEntry 骨架 ——
///      每子一份 driver + KeyedSubtree 身份、canUpdate 判定换子、多个旧子并行
///      淡出、dismissed 摘除、child 变 null 只淡出、自定义 transition/layout
///      builder、同 key 旧子去重。
///   5b. AnimatedCrossFade：对齐 animated_cross_fade.dart —— 单 controller 驱动
///       firstCurve（取反）/secondCurve/sizeCurve，build 出
///       ClipRect → AnimatedSize → defaultLayoutBuilder(Stack + 两个按 key 定位的
///       Positioned)，淡出者套 IgnorePointer，onEnd 在动画收尾时各发一次。
///   6. TweenAnimationBuilder：builder 收到插值结果。
///
/// 全部 headless（无窗口）。隐式动画由 Ticker 驱动，而 Ticker 读真实时钟，
/// 因此 widget 级用例只能断言「中途值在区间内」+「收敛后精确等于目标」；
/// 纯数值语义（缓动、重定向）在无 widget 的 ImplicitTweenDriver 上做确定性断言。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/implicit_animation.h"
#include "cui/widgets/layout.h"

using namespace cui;

namespace {

int g_failures = 0;

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

/// 把 Widget 树接入 BuildOwner + RenderView + PipelineOwner 并布局好。
/// 一棵树只能挂一次，所以每次变更后复用同一 Harness 重建。
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

/// 深度优先取挂载了指定 Widget 实例的 Element（用于标记脏而不必重建整棵树）。
Element* findElementFor(Element* root, const Widget* w) {
    if (!root)
        return nullptr;
    Element* found = nullptr;
    std::function<void(Element*)> walk = [&](Element* e) {
        if (found || !e)
            return;
        if (e->widget() == w) {
            found = e;
            return;
        }
        e->visitChildren(walk);
    };
    walk(root);
    return found;
}

/// 目标值变更后重建，并按真实时钟把动画跑到结束。
///
/// 隐式动画的 ticker 只在「仍有动画在跑」时 setState，故这里以 hasActiveTickers()
/// 为结束条件；上限 maxMs 兜底，避免意外不收敛时卡死测试。
void settle(Harness& h, Element* target, double maxMs = 800.0) {
    if (target)
        target->markNeedsBuild();
    h.rebuildAndLayout();
    double spent = 0.0;
    while (spent < maxMs && Ticker::hasActiveTickers()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        Ticker::tickAllActive(Duration(8));
        h.rebuildAndLayout();
        spent += 8.0;
    }
}

/// 只推进一帧（用于采集中途值）。
void pumpOne(Harness& h) {
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    Ticker::tickAllActive(Duration(8));
    h.rebuildAndLayout();
}

BoxDecoration solidColor(double g = 0.6) {
    BoxDecoration deco;
    deco.color = Color::fromRGB(g, g, g);
    return deco;
}

// ── 1. ImplicitTweenDriver 纯数值语义 ───────────────────────────────────────
void testTweenDriver() {
    std::printf("[test] ImplicitTweenDriver 插值语义\n");

    // 1a. 线性：三段推进精确落在 t=0.25/0.75/1.0。
    ImplicitTweenDriver d(0.0, 200.0, Easing::Linear);
    check(!d.isRunning() && near(d.value(), 0.0), "初始即停在初值，无动画");
    check(d.setTarget(1.0), "setTarget 变更返回 true");
    check(d.isRunning() && near(d.value(), 0.0), "刚设目标时仍处起点");
    d.advance(50.0);
    check(near(d.value(), 0.25), "Linear 推进 50/200 → 0.25");
    d.advance(100.0);
    check(near(d.value(), 0.75), "累计 150/200 → 0.75");
    d.advance(50.0);
    check(near(d.value(), 1.0) && !d.isRunning(), "累计 200/200 → 精确到终点且结束");
    check(!d.setTarget(1.0), "同值 setTarget 返回 false（不重启动画）");

    // 1b. 中途重定向：新段的起点是当前插值结果，而非最初起点。
    ImplicitTweenDriver r(0.0, 200.0, Easing::Linear);
    r.setTarget(1.0);
    r.advance(100.0);
    check(near(r.value(), 0.5), "重定向前 0.5");
    r.setTarget(0.0);
    check(near(r.begin(), 0.5), "重定向后起点 = 当前值 0.5");
    r.advance(100.0);
    check(near(r.value(), 0.25), "0.5 → 0.0 过半 → 0.25");
    r.advance(100.0);
    check(near(r.value(), 0.0), "收敛到新目标 0.0");

    // 1c. 缓动：EaseInOutCubic 中点对称，前 1/4 慢于线性。
    ImplicitTweenDriver c(0.0, 200.0, Easing::EaseInOutCubic);
    c.setTarget(1.0);
    c.advance(50.0);
    check(c.value() < 0.25, "EaseInOutCubic 前 1/4 慢于线性");
    c.advance(150.0);
    check(near(c.value(), 1.0), "终点精确为 1.0");
    ImplicitTweenDriver m(0.0, 200.0, Easing::EaseInOutCubic);
    m.setTarget(1.0);
    m.advance(100.0);
    check(near(m.value(), 0.5, 1e-9), "EaseInOutCubic 中点对称 → 0.5");

    // 1d. jumpToTarget / resetTo / 零时长。
    ImplicitTweenDriver j(0.0, 200.0, Easing::Linear);
    j.setTarget(3.0);
    j.advance(10.0);
    j.jumpToTarget();
    check(near(j.value(), 3.0) && !j.isRunning(), "jumpToTarget 立即到位并停表");
    j.resetTo(1.5);
    check(near(j.value(), 1.5) && near(j.target(), 1.5) && !j.isRunning(),
          "resetTo 无动画落在指定值");
    ImplicitTweenDriver z(0.0, 0.0, Easing::Linear);
    z.setTarget(2.0);
    z.advance(0.0);
    check(near(z.value(), 2.0) && !z.isRunning(), "零时长动画当帧到位");
}

// ── 2. 收敛语义：动画结束那一帧的终点值必须发布给子树 ──────────────────────
void testCompletionPublishesFinalValue() {
    std::printf("[test] 动画结束时发布终点值\n");

    auto leaf = cui::make_shared<SizedBox>(40, 40, cui::make_shared<DecoratedBox>(solidColor()));
    auto op = cui::make_shared<AnimatedOpacity>(0.0, leaf, 60.0, Easing::Linear);

    Harness h;
    h.attach(op, 200, 200);
    auto* ro = findRo<RenderOpacity>(h.root);
    check(ro != nullptr && near(ro->opacity(), 0.0), "首次构建直接落在目标值（无入场动画）");

    op->setOpacity(1.0);
    settle(h, h.root);
    ro = findRo<RenderOpacity>(h.root);
    check(ro != nullptr && near(ro->opacity(), 1.0),
          "动画结束后子树拿到精确终点值 1.0（最后一帧不被吞）");

    // 反向再走一遍，确认插值器可复用且仍精确收敛。
    op->setOpacity(0.0);
    settle(h, h.root);
    check(ro != nullptr && near(ro->opacity(), 0.0), "收敛回 0.0");

    // 中途采样：动画期间值应严格介于两端之间（真实时钟，不断言具体数值）。
    op->setOpacity(1.0);
    h.root->markNeedsBuild();
    h.rebuildAndLayout();
    pumpOne(h);
    double mid = findRo<RenderOpacity>(h.root)->opacity();
    check(mid > 0.0 && mid < 1.0, "动画中途值落在 (0,1) 开区间");
    settle(h, nullptr);
}

// ── 3. AnimatedScale / AnimatedRotation 的枢轴 ──────────────────────────────
void testTransformPivots() {
    std::printf("[test] AnimatedScale / AnimatedRotation 枢轴\n");

    // 3a. 默认 alignment = center：缩放中心是不动点，左上角向外扩。
    {
        auto leaf = cui::make_shared<SizedBox>(60, 60, cui::make_shared<DecoratedBox>(solidColor()));
        auto scale = cui::make_shared<AnimatedScale>(1.0, leaf, 60.0, Easing::Linear);
        Harness h;
        h.attach(scale, 200, 200);
        auto* rt = findRo<RenderTransform>(h.root);
        check(rt != nullptr, "AnimatedScale 产出 RenderTransform");
        if (!rt)
            return;
        const double cx = rt->size().width / 2.0, cy = rt->size().height / 2.0;

        scale->setScale(2.0);
        settle(h, h.root);
        const Matrix4 m = rt->effectiveTransform();
        Offset center = m.transformOffset(Offset(cx, cy));
        check(near(center.dx, cx) && near(center.dy, cy), "默认 alignment=center：中心是不动点");
        Offset tl = m.transformOffset(Offset(0, 0));
        check(near(tl.dx, -cx) && near(tl.dy, -cy), "左上角按 2 倍绕中心外扩");
    }

    // 3b. 覆写 alignment = topLeft：左上角成为不动点（上游 AnimatedScale.alignment）。
    {
        auto leaf = cui::make_shared<SizedBox>(60, 60, cui::make_shared<DecoratedBox>(solidColor()));
        auto scale = cui::make_shared<AnimatedScale>(1.0, leaf, 60.0, Easing::Linear, nullptr,
                                                    Alignment::topLeft());
        Harness h;
        h.attach(scale, 200, 200);
        auto* rt = findRo<RenderTransform>(h.root);
        if (!rt) {
            check(false, "alignment=topLeft 用例（未取得 RenderTransform）");
        } else {
            scale->setScale(3.0);
            settle(h, h.root);
            const Matrix4 m = rt->effectiveTransform();
            Offset tl = m.transformOffset(Offset(0, 0));
            check(near(tl.dx, 0) && near(tl.dy, 0), "alignment=topLeft：左上角是不动点");
            const double cx = rt->size().width / 2.0;
            Offset c = m.transformOffset(Offset(cx, cx));
            check(near(c.dx, cx * 3.0) && near(c.dy, cx * 3.0), "alignment=topLeft：中心按 3 倍外移");
        }
    }

    // 3c. 旋转：0.25 圈 = 90°，默认绕中心。
    {
        auto leaf = cui::make_shared<SizedBox>(60, 60, cui::make_shared<DecoratedBox>(solidColor()));
        auto rot = cui::make_shared<AnimatedRotation>(0.0, leaf, 60.0, Easing::Linear);
        Harness h;
        h.attach(rot, 200, 200);
        auto* rt = findRo<RenderTransform>(h.root);
        if (!rt) {
            check(false, "AnimatedRotation 用例（未取得 RenderTransform）");
        } else {
            const double cx = rt->size().width / 2.0, cy = rt->size().height / 2.0;
            rot->setTurns(0.25);
            settle(h, h.root);
            const Matrix4 m = rt->effectiveTransform();
            Offset p = m.transformOffset(Offset(cx + 40, cy));
            check(near(p.dx, cx) && near(p.dy, cy + 40), "0.25 圈 → 绕中心旋转 90°");
            Offset c = m.transformOffset(Offset(cx, cy));
            check(near(c.dx, cx) && near(c.dy, cy), "旋转中心是不动点");
        }
    }
}

// ── 4. 其余隐式动画的收敛值 ─────────────────────────────────────────────────
void testRemainingWidgets() {
    std::printf("[test] AnimatedSlide / Padding / Align / Positioned / Container\n");

    // 4a. AnimatedSlide：位移按子节点自身尺寸比例（FractionalTranslation）。
    {
        auto leaf = cui::make_shared<SizedBox>(100, 50, cui::make_shared<DecoratedBox>(solidColor()));
        auto slide = cui::make_shared<AnimatedSlide>(Offset(0, 0), leaf, 60.0, Easing::Linear);
        Harness h;
        h.attach(slide, 200, 200);
        auto* ft = findRo<RenderFractionalTranslation>(h.root);
        check(ft != nullptr, "AnimatedSlide 产出 RenderFractionalTranslation");
        if (ft) {
            const double w = ft->size().width, hh = ft->size().height;
            check(near(ft->translationOffset().dx, 0), "起始位移 0");
            slide->setOffset(Offset(0.5, -0.25));
            settle(h, h.root);
            check(near(ft->translationOffset().dx, 0.5 * w) &&
                          near(ft->translationOffset().dy, -0.25 * hh),
                  "位移收敛为自身尺寸的比例（0.5×宽、-0.25×高）");
        }
    }

    // 4b. AnimatedPadding：四边内缩收敛，子节点偏移到 (20,20)。
    {
        auto leaf = cui::make_shared<SizedBox>(10, 10, cui::make_shared<DecoratedBox>(solidColor()));
        auto pad = cui::make_shared<AnimatedPadding>(EdgeInsets::all(0), leaf, 60.0, Easing::Linear);
        Harness h;
        h.attach(pad, 200, 200);
        auto* rp = findRo<RenderPadding>(h.root);
        check(rp != nullptr && near(rp->padding().left, 0), "初始 padding 全 0");
        if (rp) {
            pad->setPadding(EdgeInsets::all(20));
            settle(h, h.root);
            check(near(rp->padding().left, 20) && near(rp->padding().bottom, 20),
                  "padding 收敛到 20");
            auto* kid = rp->child();
            check(kid && kid->parentData() && near(kid->parentData()->offset.dx, 20) &&
                          near(kid->parentData()->offset.dy, 20),
                  "子节点偏移到 (20,20)");
        }
    }

    // 4c. AnimatedAlign：alignment 两端插值 → 子节点偏移随之收敛。
    {
        auto leaf = cui::make_shared<SizedBox>(50, 30, cui::make_shared<DecoratedBox>(solidColor()));
        auto align =
                cui::make_shared<AnimatedAlign>(Alignment(0, 0), leaf, 60.0, Easing::Linear);
        Harness h;
        h.attach(align, 200, 200);
        auto* ra = findRo<RenderAlign>(h.root);
        check(ra != nullptr, "AnimatedAlign 产出 RenderAlign");
        if (ra) {
            check(near(ra->child()->parentData()->offset.dx, 75) &&
                          near(ra->child()->parentData()->offset.dy, 85),
                  "Alignment(0,0) → 子节点居中 (75,85)");
            align->setAlignment(Alignment(1, 1));
            settle(h, h.root);
            check(near(ra->child()->parentData()->offset.dx, 150) &&
                          near(ra->child()->parentData()->offset.dy, 170),
                  "Alignment(1,1) → 子节点右下 (150,170)");
        }
    }

    // 4d. AnimatedPositioned：作为 Stack 子节点，left/top 收敛。
    {
        auto leaf = cui::make_shared<SizedBox>(40, 40, cui::make_shared<DecoratedBox>(solidColor()));
        auto ap = cui::make_shared<AnimatedPositioned>(leaf, 60.0, Easing::Linear);
        ap->left = 0.0;
        ap->top = 0.0;
        auto stack = cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{ap});
        Harness h;
        h.attach(stack, 200, 200);
        Element* target = findElementFor(h.root, ap.get());
        check(target != nullptr, "在 Stack 内找到 AnimatedPositioned 的 Element");
        ap->left = 30.0;
        ap->top = 50.0;
        settle(h, target);
        auto* kidRo = target ? static_cast<RenderBox*>(target->findRenderObject()) : nullptr;
        check(kidRo != nullptr && kidRo->parentData() &&
                      near(kidRo->parentData()->offset.dx, 30) &&
                      near(kidRo->parentData()->offset.dy, 50),
              "left/top 动画收敛到 (30,50)");
    }

    // 4e. AnimatedContainer：尺寸补间收敛。
    {
        auto c = cui::make_shared<AnimatedContainer>(nullptr, 60.0, Easing::Linear);
        c->width = 40.0;
        c->height = 40.0;
        c->decoration = solidColor();
        Harness h;
        h.attach(cui::make_shared<Center>(c), 200, 200);
        auto* box = findRo<RenderConstrainedBox>(h.root);
        check(box != nullptr && near(box->size().width, 40) && near(box->size().height, 40),
              "首次构建即 40×40（无入场动画）");
        c->width = 120.0;
        c->height = 60.0;
        Element* target = findElementFor(h.root, c.get());
        settle(h, target);
        check(box != nullptr && near(box->size().width, 120) && near(box->size().height, 60),
              "尺寸动画收敛到 120×60");
    }
}

// ── 5. AnimatedSwitcher ─────────────────────────────────────────────────────

/// Stack 直接子里每一层的 (不透明度, 宽)。宽用来认出「这一层是哪个子」，
/// 顺序即上游 defaultLayoutBuilder 的 [previous…, current]。
struct Layer {
    double opacity;
    double width;
};

std::vector<Layer> switcherLayers(Element* root) {
    std::vector<Layer> out;
    if (auto* st = findRo<RenderStack>(root)) {
        st->visitChildren([&](AbstractNode* c) {
            if (auto* o = dynamic_cast<RenderOpacity*>(c))
                out.push_back({o->opacity(), static_cast<RenderBox*>(o)->size().width});
        });
    }
    return out;
}

/// 带 id key 的彩色方块。上游文档里的坑：同类型 + 同 key（都为空也算同 key）
/// 的子被 `Widget.canUpdate` 判成同一个 ⇒ **不做过渡**，所以想测过渡必须给
/// 每个子一份唯一 key（宽同时用来在断言里认出这一层是谁）。
SharedPtr<Widget> keyedBox(double w, double gray, int id) {
    return cui::make_shared<SizedBox>(w, w, cui::make_shared<DecoratedBox>(solidColor(gray)),
                                      cui::make_shared<const ValueKey<int>>(id));
}

void testSwitcherAndBuilder() {
    std::printf("[test] AnimatedSwitcher（对齐 animated_switcher.dart）\n");

    // 5a. 默认过渡骨架：首子不入场 → 换子那帧两端 1/0 → 中途并行 → 淡到 0 摘除。
    {
        auto a = keyedBox(10, 0.2, 1);
        auto b = keyedBox(10, 0.8, 2);
        auto sw = cui::make_shared<AnimatedSwitcher>(a, 60.0, Easing::Linear);
        Harness h;
        h.attach(sw, 200, 200);
        Element* target = findElementFor(h.root, sw.get());

        auto born = switcherLayers(h.root);
        check(born.size() == 1 && near(born[0].opacity, 1.0),
              "首帧只有一个子且已到位（上游 initState 用 animate: false，入场不过渡）");
        check(!Ticker::hasActiveTickers(), "入场不起表");

        sw->setChild(b);
        target->markNeedsBuild();
        h.rebuildAndLayout();
        auto edge = switcherLayers(h.root);
        check(edge.size() == 2, "切换后 Stack 里有新旧两个淡入淡出子");
        check(edge.size() == 2 && near(edge[0].opacity, 1.0) && near(edge[1].opacity, 0.0),
              "切换那一帧两端是「旧 1.0 / 新 0.0」（controller 还没 tick）");

        pumpOne(h);
        auto mid = switcherLayers(h.root);
        check(mid.size() == 2 && mid[0].opacity > 0.0 && mid[0].opacity < 1.0 &&
                      mid[1].opacity > 0.0 && mid[1].opacity < 1.0,
                  "中途两个子同时在动（旧降、新升）");

        settle(h, target);
        auto done = switcherLayers(h.root);
        check(done.size() == 1 && near(done[0].opacity, 1.0),
              "旧子淡到 0 即被摘除（上游 AnimationStatus.dismissed 监听）");
        check(!Ticker::hasActiveTickers(), "收尾后停表");
    }

    // 5b. 连切：多个旧子并行淡出（旧实现只有一个 outgoing，这里钉住）。
    {
        auto c1 = keyedBox(10, 0.1, 11);
        auto c2 = keyedBox(20, 0.5, 12);
        auto c3 = keyedBox(30, 0.9, 13);
        auto sw = cui::make_shared<AnimatedSwitcher>(c1, 400.0, Easing::Linear);
        Harness h;
        h.attach(sw, 200, 200);
        Element* target = findElementFor(h.root, sw.get());

        sw->setChild(c2);
        target->markNeedsBuild();
        h.rebuildAndLayout();
        pumpOne(h);
        sw->setChild(c3);
        target->markNeedsBuild();
        h.rebuildAndLayout();

        auto kids = switcherLayers(h.root);
        check(kids.size() == 3, "切换够快时同时有三个子（两个旧的并行淡出）");
        check(kids.size() == 3 && near(kids[0].width, 10) && near(kids[1].width, 20) &&
                      near(kids[2].width, 30),
                  "previous 按最早在前排、current 在最后（上游 layoutBuilder 的顺序约定）");
        check(kids.size() == 3 && kids[0].opacity > 0.5 && kids[1].opacity < kids[0].opacity &&
                      near(kids[2].opacity, 0.0),
                  "最老的还在高位、被打断的子从当前值转头（controller.reverse）、新子从 0 起");

        settle(h, target);
        auto after = switcherLayers(h.root);
        check(after.size() == 1 && near(after[0].width, 30) && near(after[0].opacity, 1.0),
              "两个旧子各自淡到 0 后都摘除，只剩 current");
    }

    // 5c. 「换子」判定 = Widget::canUpdate（同类型同 key ⇒ 不过渡）。
    {
        auto key = cui::make_shared<const ValueKey<int>>(7);
        auto a = cui::make_shared<DecoratedBox>(solidColor(0.2), nullptr, key);
        auto b = cui::make_shared<DecoratedBox>(solidColor(0.8), nullptr, key);
        auto sw = cui::make_shared<AnimatedSwitcher>(a, 60.0, Easing::Linear);
        Harness h;
        h.attach(sw, 200, 200);
        Element* target = findElementFor(h.root, sw.get());

        sw->setChild(b);
        settle(h, target);
        auto kids = switcherLayers(h.root);
        check(kids.size() == 1 && near(kids[0].opacity, 1.0) && !Ticker::hasActiveTickers(),
              "同类型同 key 只换内容，不触发过渡（上游同款：要过渡得给不同 key）");

        // 上游文档点明的坑：两个都没 key 的同类型子同样「不算换」，
        // 想按内容切换必须自己挂 ValueKey。
        auto c = cui::make_shared<SizedBox>(10, 10, cui::make_shared<DecoratedBox>(solidColor(0.2)));
        auto d = cui::make_shared<SizedBox>(10, 10, cui::make_shared<DecoratedBox>(solidColor(0.8)));
        auto sw2 = cui::make_shared<AnimatedSwitcher>(c, 60.0, Easing::Linear);
        Harness h2;
        h2.attach(sw2, 200, 200);
        Element* target2 = findElementFor(h2.root, sw2.get());
        sw2->setChild(d);
        target2->markNeedsBuild();
        h2.rebuildAndLayout();
        check(switcherLayers(h2.root).size() == 1, "无 key + 同类型（如两个 SizedBox）也不过渡");
    }

    // 5d. child 变 null：只淡出旧的，淡完后 Stack 里没有子。
    {
        auto a = keyedBox(10, 0.6, 21);
        auto sw = cui::make_shared<AnimatedSwitcher>(a, 60.0, Easing::Linear);
        Harness h;
        h.attach(sw, 200, 200);
        Element* target = findElementFor(h.root, sw.get());

        sw->setChild(nullptr);
        target->markNeedsBuild();
        h.rebuildAndLayout();
        auto kids = switcherLayers(h.root);
        check(kids.size() == 1 && near(kids[0].opacity, 1.0),
              "child 变 null：旧子转为淡出（上游 _addEntryForNewChild 不起新 entry）");
        check(Ticker::hasActiveTickers(), "只剩淡出时也要起表（每个 entry 一个 controller 的等价）");

        settle(h, target);
        check(switcherLayers(h.root).empty() && findRo<RenderStack>(h.root) != nullptr,
              "淡完后是空 Stack（上游 layoutBuilder 仍要产出非空 Widget）");
    }

    // 5e. 自定义 transitionBuilder / layoutBuilder。
    {
        auto a = keyedBox(10, 0.2, 31);
        auto b = keyedBox(20, 0.8, 32);
        int layoutCalls = 0;
        size_t prevAtSwitch = 0;
        bool sawCurrent = false;
        AnimatedSwitcherTransitionBuilder tb =
            [](SharedPtr<Widget> child, double v) {
                // 半程淡入：收口后 current 应当正好是 0.5，用来证明 builder 吃的是本 entry 的值。
                return cui::make_shared<Opacity>(v * 0.5, std::move(child));
            };
        AnimatedSwitcherLayoutBuilder lb =
            [&](SharedPtr<Widget> current, std::vector<SharedPtr<Widget>> previous) {
                ++layoutCalls;
                prevAtSwitch = previous.size();
                sawCurrent = (current != nullptr);
                std::vector<SharedPtr<Widget>> kids = std::move(previous);
                if (current)
                    kids.push_back(std::move(current));
                return cui::make_shared<Align>(0.0, 0.0,
                                               cui::make_shared<Stack>(std::move(kids)));
            };
        auto sw = cui::make_shared<AnimatedSwitcher>(a, 60.0, Easing::Linear, Easing::Linear, tb, lb);
        Harness h;
        h.attach(sw, 200, 200);
        Element* target = findElementFor(h.root, sw.get());
        check(layoutCalls == 1 && sawCurrent && prevAtSwitch == 0,
              "layoutBuilder 每次 build 都调，初始只有 current");
        check(findRo<RenderAlign>(h.root) != nullptr, "自定义 layoutBuilder 产出的 Align 进了树");

        sw->setChild(b);
        target->markNeedsBuild();
        h.rebuildAndLayout();
        check(sawCurrent && prevAtSwitch == 1,
              "切换时 layoutBuilder 收到 (current, previous[1])（上游签名）");

        settle(h, target);
        auto kids = switcherLayers(h.root);
        check(kids.size() == 1 && near(kids[0].opacity, 0.5),
              "自定义 transitionBuilder 收到 current 的 1.0 → 半程淡入停在 0.5");
    }

    // 5f. 同一个 key 再次进来：与正在淡出的旧子同 key ⇒ 不进 previousChildren（上游 where 去重）。
    {
        auto key = cui::make_shared<const ValueKey<int>>(99);
        auto a = cui::make_shared<SizedBox>(10, 10, cui::make_shared<DecoratedBox>(solidColor(0.2)),
                                            key);
        // 不同类型 + 同 key ⇒ canUpdate 为假 ⇒ 换子；但两个 entry 的 transitionKey 相等。
        auto b = cui::make_shared<DecoratedBox>(solidColor(0.8),
                                                cui::make_shared<SizedBox>(20, 20), key);
        auto sw = cui::make_shared<AnimatedSwitcher>(a, 400.0, Easing::Linear);
        Harness h;
        h.attach(sw, 200, 200);
        Element* target = findElementFor(h.root, sw.get());

        sw->setChild(b);
        target->markNeedsBuild();
        h.rebuildAndLayout();
        auto kids = switcherLayers(h.root);
        check(kids.size() == 1 && near(kids[0].width, 20) && near(kids[0].opacity, 0.0),
              "同 key 的旧子被去重，列表里只剩 current（上游 build 的 where key != current.key）");
        check(Ticker::hasActiveTickers(),
              "去重只是不进列表，旧 entry 仍在后台跑表直到 dismissed");

        settle(h, target);
        auto after = switcherLayers(h.root);
        check(after.size() == 1 && near(after[0].width, 20) && near(after[0].opacity, 1.0),
              "收敛后只剩 current");
    }
}

// ── 5b. AnimatedCrossFade：上游形状 ClipRect → AnimatedSize → Stack[bottom,top]
void testCrossFade() {
    std::printf("[test] AnimatedCrossFade（对齐 animated_cross_fade.dart）\n");
    auto f = cui::make_shared<SizedBox>(10, 10, cui::make_shared<DecoratedBox>(solidColor()));
    auto s = cui::make_shared<SizedBox>(40, 30, cui::make_shared<DecoratedBox>(solidColor()));
    int endCount = 0;
    auto cf = cui::make_shared<AnimatedCrossFade>(
        f, s, CrossFadeState::ShowFirst, 60.0, Easing::Linear, Easing::Linear, Easing::Linear,
        Alignment::topCenter(), Clip::HardEdge, nullptr, [&endCount] { ++endCount; });
    Harness h;
    // 外包 Align：RenderView 给的是窗口大小的紧约束，紧约束下 AnimatedSize 走
    // 短路分支（尺寸恒为 smallest、不补间），必须先把约束放松才测得到动画。
    h.attach(cui::make_shared<Align>(-1.0, -1.0, cf), 200, 200);

    auto* anim = findRo<RenderAnimatedSize>(h.root);
    auto* stack = findRo<RenderStack>(h.root);
    check(anim != nullptr, "CrossFade 下面接的是 AnimatedSize（上游 build 的骨架）");
    check(findRo<RenderClipRect>(h.root) != nullptr, "最外层是 ClipRect");
    check(stack != nullptr, "默认 layoutBuilder 产出 Stack");
    if (!anim || !stack) {
        check(false, "树形不符预期，跳过后续断言");
        return;
    }

    std::vector<double> opacities;
    auto collect = [&] {
        opacities.clear();
        std::function<void(RenderObject*)> walk = [&](RenderObject* o) {
            if (auto* op = dynamic_cast<RenderOpacity*>(o))
                opacities.push_back(op->opacity());
            o->visitChildren([&](AbstractNode* c) {
                if (c)
                    walk(static_cast<RenderObject*>(c));
            });
        };
        walk(stack);
        std::sort(opacities.begin(), opacities.end());
    };
    collect();
    check(opacities.size() == 2 && near(opacities.front(), 0.0) && near(opacities.back(), 1.0),
          "首帧即到位（上游 initState 直设 controller.value）：first 1.0 / second 0.0");
    check(anim->size() == Size(10, 10), "AnimatedSize 首帧尺寸 = 当前显示的 first");

    Element* target = findElementFor(h.root, cf.get());
    cf->setCrossFadeState(CrossFadeState::ShowSecond);
    if (target)
        target->markNeedsBuild();
    h.rebuildAndLayout();
    collect();
    check(opacities.size() == 2 && near(opacities.front(), 0.0) && near(opacities.back(), 1.0),
          "切换那一帧两端仍是 0/1（controller.value 还是 0，first 取反后=1）");
    check(anim->isAnimating(), "切换后 AnimatedSize 在补间尺寸");

    pumpOne(h);
    collect();
    check(opacities.size() == 2 && opacities.front() > 0.0 && opacities.back() < 1.0,
          "中途两子透明度互补地在动（不是瞬切）");
    check(anim->size().width > 10.0 && anim->size().width < 40.0,
          "中途自身尺寸补在 first↔second 之间");

    settle(h, nullptr);
    collect();
    check(opacities.size() == 2 && near(opacities.front(), 0.0) && near(opacities.back(), 1.0),
          "收敛后 first 淡到 0.0、second 淡到 1.0");
    check(anim->size() == Size(40, 30), "收敛后 AnimatedSize 精确落在 second 的尺寸");
    check(!anim->isAnimating(), "收敛后尺寸动画停表");
    check(endCount == 1, "onEnd 恰好发一次（上游挂在 controller 的 status 上）");

    // 反向切回：top/bottom 互换 ⇒ 淡出者（这次是 second）被 IgnorePointer 包住。
    cf->setCrossFadeState(CrossFadeState::ShowFirst);
    settle(h, findElementFor(h.root, cf.get()));
    check(findRo<RenderIgnorePointer>(h.root) != nullptr,
          "淡出者外面套了 IgnorePointer（上游同款）");
    check(anim->size() == Size(10, 10), "切回 first 后尺寸动画回 10×10");
    check(endCount == 2, "第二次动画再发一次 onEnd");

    // 曲线：firstCurve 取反 ⇒ EaseOutCubic（前段快）让淡出比线性更早掉下去。
    auto f2 = cui::make_shared<SizedBox>(10, 10, cui::make_shared<DecoratedBox>(solidColor()));
    auto s2 = cui::make_shared<SizedBox>(10, 10, cui::make_shared<DecoratedBox>(solidColor()));
    auto cf2 = cui::make_shared<AnimatedCrossFade>(f2, s2, CrossFadeState::ShowFirst, 200.0,
                                                   Easing::EaseOutCubic, Easing::Linear);
    Harness h2;
    h2.attach(cf2, 200, 200);
    auto* stack2 = findRo<RenderStack>(h2.root);
    auto* anim2 = findRo<RenderAnimatedSize>(h2.root);
    if (stack2 && anim2) {
        if (Element* e = findElementFor(h2.root, cf2.get()))
            e->markNeedsBuild();
        cf2->setCrossFadeState(CrossFadeState::ShowSecond);
        h2.rebuildAndLayout();
        pumpOne(h2);
        std::vector<double> ops;
        std::function<void(RenderObject*)> walk = [&](RenderObject* o) {
            if (auto* op = dynamic_cast<RenderOpacity*>(o))
                ops.push_back(op->opacity());
            o->visitChildren([&](AbstractNode* c) {
                if (c)
                    walk(static_cast<RenderObject*>(c));
            });
        };
        walk(stack2);
        std::sort(ops.begin(), ops.end());
        double p = anim2->animationValue();
        p = p > 0.0 && p < 1.0 ? p : 0.5;
        check(ops.size() == 2 && ops.front() < 1.0 - p - 1e-3,
              "firstCurve=EaseOutCubic：淡出比线性更快（first 取反施加的是 firstCurve）");
    }
}

// ── 5c. TweenAnimationBuilder ───────────────────────────────────────────────
void testTweenAnimationBuilder() {
    std::printf("[test] TweenAnimationBuilder\n");
    {
        std::vector<double> seen;
        auto tb = cui::make_shared<TweenAnimationBuilder>(
                0.0,
                [&](BuildContext&, double v) {
                    seen.push_back(v);
                    return cui::make_shared<SizedBox>(10 + v * 10, 20);
                },
                60.0, Easing::Linear);
        Harness h;
        h.attach(cui::make_shared<Center>(tb), 200, 200);
        check(seen.size() == 1 && near(seen.front(), 0.0), "首次构建直接收到目标值 0");
        tb->setTarget(1.0);
        Element* target = findElementFor(h.root, tb.get());
        settle(h, target);
        check(seen.size() >= 2, "目标变化后 builder 被多次调用");
        check(near(seen.back(), 1.0), "末次收到精确终点 1.0");
        auto* box = findRo<RenderConstrainedBox>(h.root);
        check(box != nullptr && near(box->size().width, 20), "子树尺寸随终点值变为 20×20");
    }
}

}  // namespace

int main() {
    std::printf("=== 隐式动画控件族测试 ===\n");
    testTweenDriver();
    testCompletionPublishesFinalValue();
    testTransformPivots();
    testRemainingWidgets();
    testSwitcherAndBuilder();
    testCrossFade();
    testTweenAnimationBuilder();
    std::printf("\n=== %s: %d failure(s) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
