/// @file test_transition.cpp
/// @brief 显式动画控件族（widgets/transition.h）+ 上游 RenderPositionedBox
///        因子语义 单元测试。
///
/// 覆盖：
///   1. Align 的 widthFactor / heightFactor —— 上游 rendering/shifted_box.dart
///      的 RenderPositionedBox.performLayout：有因子按「子尺寸 × 因子」收缩，
///      无因子时有界撑满、无界收缩到子尺寸。
///   2. FadeTransition：外部 Animation 驱动 Opacity，含取值区间（range）。
///   3. ScaleTransition / RotationTransition：MatrixTransition 产矩阵 +
///      alignment 默认 center（绕盒子中心，而非左上角）。
///   4. SlideTransition：FractionalTranslation 平移区间 + transformHitTests。
///   5. SizeTransition：ClipRect + Align 因子（横/纵轴、fixedCrossAxisSizeFactor）。
///   6. AlignTransition：Alignment 区间插值。
///
/// 全部 headless。帧泵按约定跳过 flushPaint()（需真实 SkCanvas）；动画用
/// AnimationController::tick(Duration) 确定性推进（绝对时间语义，需先 start()）。

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/transition.h"

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
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;  // 最后声明：其 FocusManager 销毁时依赖仍存活
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        po.flushLayout();
    }

    /// 重建脏 Element → 布局（headless 安全：不做 flushPaint）。
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

/// 300ms 线性动画控制器：start() 后按绝对 elapsed 推进并重建布局。
struct Driver {
    SharedPtr<AnimationController> anim;
    Harness& h;

    Driver(Harness& harness, SharedPtr<AnimationController> a) : anim(std::move(a)), h(harness) {
        anim->start();
    }

    /// 推进到 elapsedMs 并跑一帧（线性缓动下 progress = elapsedMs / 300）。
    void advanceTo(double elapsedMs) {
        anim->tick(Duration(elapsedMs));
        h.rebuildAndLayout();
    }
};

BoxDecoration solidColor(double g = 0.6) {
    BoxDecoration deco;
    deco.color = Color::fromRGB(g, g, g);
    return deco;
}

// ── 1. Align 的 widthFactor / heightFactor ──────────────────────────────────
//
// 用 Stack 当父级：RenderStack 的 Fit::Loose 给子节点 loosen() 约束，Align
// 才拿到「有界但松」的 [0..200]，能同时验证撑满与因子收缩两条分支。
void testAlignFactors() {
    std::printf("[test] Align widthFactor / heightFactor（上游 RenderPositionedBox）\n");

    auto leaf = cui::make_shared<SizedBox>(40, 40);

    // 1a. 无因子 + 有界松约束 → 两轴撑满上限。
    {
        Harness h;
        h.attach(cui::make_shared<Stack>(
                          std::vector<SharedPtr<Widget>>{
                                  cui::make_shared<Align>(0.0, 0.0, leaf)}),
                  200, 200);
        RenderAlign* a = findRo<RenderAlign>(h.root);
        check(a != nullptr, "找到 RenderAlign");
        check(a && near(a->size().width, 200) && near(a->size().height, 200),
              "无因子：有界松约束下两轴撑满 max（200×200）");
    }

    // 1b. widthFactor=0.5 → 宽 = 40×0.5；高无因子 → 撑满 200。
    {
        Harness h;
        h.attach(cui::make_shared<Stack>(
                          std::vector<SharedPtr<Widget>>{cui::make_shared<Align>(
                                  0.0, -1.0, leaf, nullptr, 0.5, std::nullopt)}),
                  200, 200);
        RenderAlign* a = findRo<RenderAlign>(h.root);
        check(a && near(a->size().width, 20), "widthFactor=0.5：宽按子尺寸×因子收缩为 20");
        check(a && near(a->size().height, 200), "heightFactor 为空：高撑满 200");
    }

    // 1c. 双因子 → 两轴都收缩。
    {
        Harness h;
        h.attach(cui::make_shared<Stack>(
                          std::vector<SharedPtr<Widget>>{cui::make_shared<Align>(
                                  0.0, 0.0, leaf, nullptr, 0.5, 0.25)}),
                  200, 200);
        RenderAlign* a = findRo<RenderAlign>(h.root);
        check(a && near(a->size().width, 20) && near(a->size().height, 10),
              "双因子：20×10（40×0.5, 40×0.25）");
    }

    // 1d. heightFactor=0 → 该轴为 0（SizeTransition 动画末端）。
    {
        Harness h;
        h.attach(cui::make_shared<Stack>(
                          std::vector<SharedPtr<Widget>>{cui::make_shared<Align>(
                                  0.0, 0.0, leaf, nullptr, std::nullopt, 0.0)}),
                  200, 200);
        RenderAlign* a = findRo<RenderAlign>(h.root);
        check(a && near(a->size().height, 0), "heightFactor=0：高为 0");
        check(a && near(a->size().width, 200), "heightFactor=0 不牵动宽（仍撑满 200）");
    }

    // 1e. 渲染对象级改因子 → markNeedsLayout 生效（SizeTransition 每帧重建
    //     出的新 Align 走的就是这条 updateRenderObject 路径）。
    {
        Harness h;
        h.attach(cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{
                         cui::make_shared<Align>(0.0, 0.0, leaf, nullptr, 1.0, 1.0)}),
                 200, 200);
        RenderAlign* a = findRo<RenderAlign>(h.root);
        check(a && near(a->size().width, 40) && near(a->size().height, 40),
              "双因子=1.0：40×40");
        a->setWidthFactor(0.5);
        h.rebuildAndLayout();
        check(a && near(a->size().width, 20) && near(a->size().height, 40),
              "改 widthFactor → 20×40（markNeedsLayout 生效）");
    }
}

// ── 2. FadeTransition ───────────────────────────────────────────────────────
void testFadeTransition() {
    std::printf("[test] FadeTransition\n");

    auto anim = cui::make_shared<AnimationController>(300.0);
    auto fade = cui::make_shared<FadeTransition>(
            anim, cui::make_shared<SizedBox>(60, 60, cui::make_shared<DecoratedBox>(solidColor())));

    Harness h;
    h.attach(cui::make_shared<Center>(fade), 200, 200);
    RenderOpacity* ro = findRo<RenderOpacity>(h.root);
    check(ro != nullptr, "找到 RenderOpacity");
    check(ro && near(ro->opacity(), 0.0), "初始 opacity = 0（动画未走）");

    Driver d(h, anim);
    d.advanceTo(150);  // 进度 0.5
    check(ro && near(ro->opacity(), 0.5), "推进 150/300ms → opacity 0.5（动画驱动重建）");

    d.advanceTo(300);
    check(ro && near(ro->opacity(), 1.0), "推进到终点 → opacity 1.0");

    // 取值区间（上游 Tween.animate 的等价物）：0.2 .. 0.8。
    // 用全新控制器/全新树：AnimationController 是单向计时器，reset() 后不重新 start()
    // 就不会再产出进度。
    {
        auto anim2 = cui::make_shared<AnimationController>(300.0);
        anim2->setEasing(Easing::Linear);
        auto fade2 = cui::make_shared<FadeTransition>(
                anim2, cui::make_shared<SizedBox>(60, 60, cui::make_shared<DecoratedBox>(solidColor())));
        fade2->setRange(Tween<double>(0.2, 0.8));

        Harness h2;
        h2.attach(cui::make_shared<Center>(fade2), 200, 200);
        RenderOpacity* ro2 = findRo<RenderOpacity>(h2.root);
        check(ro2 && near(ro2->opacity(), 0.2), "range 下进度 0 → 区间起点 0.2");

        Driver d2(h2, anim2);
        d2.advanceTo(150);
        check(ro2 && near(ro2->opacity(), 0.5), "range=[0.2,0.8] 且进度 0.5 → opacity 0.5");
        d2.advanceTo(300);
        check(ro2 && near(ro2->opacity(), 0.8), "range 下进度 1 → 区间终点 0.8");
    }
}

// ── 3. ScaleTransition / RotationTransition ─────────────────────────────────
void testMatrixTransitions() {
    std::printf("[test] ScaleTransition / RotationTransition（MatrixTransition）\n");

    // 3a. 缩放：60×60 的盒子，默认 alignment = center → 中心 (30,30)。
    {
        auto anim = cui::make_shared<AnimationController>(300.0);
        anim->setEasing(Easing::Linear);
        auto st = cui::make_shared<ScaleTransition>(
                anim, cui::make_shared<SizedBox>(60, 60, cui::make_shared<DecoratedBox>(solidColor())));
        st->setRange(Tween<double>(1.0, 3.0));  // 进度 0.5 → scale 2.0

        Harness h;
        h.attach(cui::make_shared<Center>(st), 200, 200);
        RenderTransform* rt = findRo<RenderTransform>(h.root);
        check(rt != nullptr, "找到 RenderTransform");
        check(rt && near(rt->size().width, 60) && near(rt->size().height, 60),
              "Transform 不参与布局：尺寸仍 60×60");

        Driver d(h, anim);
        d.advanceTo(150);
        if (rt) {
            const Matrix4 m = rt->effectiveTransform();
            Offset c = m.transformOffset(Offset(30, 30));
            check(near(c.dx, 30) && near(c.dy, 30), "缩放中心 (30,30) 是不动点（alignment=center）");
            Offset tl = m.transformOffset(Offset(0, 0));
            check(near(tl.dx, -30) && near(tl.dy, -30), "左上角按 2.0 倍绕中心外扩 → (-30,-30)");
        } else {
            check(false, "缩放矩阵断言（未取得 RenderTransform）");
        }
    }

    // 3b. 旋转：0.25 圈 = 90°，绕中心 (30,30)。
    {
        auto anim = cui::make_shared<AnimationController>(300.0);
        anim->setEasing(Easing::Linear);
        auto rot = cui::make_shared<RotationTransition>(
                anim, cui::make_shared<SizedBox>(60, 60, cui::make_shared<DecoratedBox>(solidColor())));

        Harness h;
        h.attach(cui::make_shared<Center>(rot), 200, 200);
        RenderTransform* rt = findRo<RenderTransform>(h.root);
        check(rt != nullptr, "旋转：找到 RenderTransform");

        Driver d(h, anim);
        d.advanceTo(75);  // 进度 0.25 → 0.25 圈 → 90°
        if (rt) {
            const Matrix4 m = rt->effectiveTransform();
            // (40,30) 相对中心 (10,0)，绕 z 轴 +90° → (0,10) → 绝对 (30,40)。
            Offset p = m.transformOffset(Offset(40, 30));
            check(near(p.dx, 30) && near(p.dy, 40), "0.25 圈 → 绕中心旋转 90°");
            Offset c = m.transformOffset(Offset(30, 30));
            check(near(c.dx, 30) && near(c.dy, 30), "旋转中心 (30,30) 是不动点");
        } else {
            check(false, "旋转矩阵断言（未取得 RenderTransform）");
        }
    }
}

// ── 4. SlideTransition ──────────────────────────────────────────────────────
void testSlideTransition() {
    std::printf("[test] SlideTransition + FractionalTranslation 命中测试\n");

    auto leaf = cui::make_shared<SizedBox>(100, 100, cui::make_shared<DecoratedBox>(solidColor()));

    // 4a. 平移区间 (0,0) → (1,0)，进度 0.5 → translation (0.5, 0)。
    auto anim = cui::make_shared<AnimationController>(300.0);
    anim->setEasing(Easing::Linear);
    auto slide = cui::make_shared<SlideTransition>(anim, Offset(0, 0), Offset(1, 0), leaf);

    Harness h;
    h.attach(cui::make_shared<Center>(slide), 400, 400);
    RenderFractionalTranslation* ft = findRo<RenderFractionalTranslation>(h.root);
    check(ft != nullptr, "找到 RenderFractionalTranslation");

    Driver d(h, anim);
    d.advanceTo(150);
    check(ft && near(ft->translation().dx, 0.5) && near(ft->translation().dy, 0.0),
          "进度 0.5 → translation (0.5, 0)");

    // 绘制偏移 = translation × size = (50, 0)：命中区随之右移到 [50,150]。
    if (ft) {
        std::vector<RenderObject*> hitIn;
        check(ft->hitTest(Offset(60, 50), hitIn), "平移后 (60,50) 命中子节点");
        std::vector<RenderObject*> hitOut;
        check(!ft->hitTest(Offset(10, 50), hitOut), "平移后 (10,50) 不再命中（原位置已空出）");

        ft->setTransformHitTests(false);
        std::vector<RenderObject*> hitRaw;
        check(ft->hitTest(Offset(10, 50), hitRaw),
              "transformHitTests=false → 按未平移坐标命中");
    }

    // 4b. 区间端点。
    d.advanceTo(300);
    check(ft && near(ft->translation().dx, 1.0), "进度 1 → translation dx = 1（区间终点）");
}

// ── 5. SizeTransition ───────────────────────────────────────────────────────
void testSizeTransition() {
    std::printf("[test] SizeTransition（ClipRect + Align 因子）\n");

    auto leaf = cui::make_shared<SizedBox>(40, 40);

    // 5a. 默认纵轴：高 = 40 × factor，宽无因子 → 撑满交叉轴。
    {
        auto anim = cui::make_shared<AnimationController>(300.0);
        anim->setEasing(Easing::Linear);
        auto st = cui::make_shared<SizeTransition>(anim, leaf);

        // 父级用 Stack 而非 Center：Center 本身就是 RenderAlign，会先被
        // findRo<RenderAlign> 命中、遮挡 SizeTransition 内部的那个。
        Harness h;
        h.attach(cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{st}), 200, 200);
        RenderAlign* a = findRo<RenderAlign>(h.root);
        RenderClipRect* cr = findRo<RenderClipRect>(h.root);
        check(cr != nullptr, "SizeTransition 产出 ClipRect");
        check(a != nullptr, "SizeTransition 产出 Align");
        check(a && a->heightFactor().has_value() && !a->widthFactor().has_value(),
              "纵轴：只有 heightFactor 非空");

        Driver d(h, anim);
        d.advanceTo(150);  // factor 0.5
        check(a && near(a->size().height, 20), "进度 0.5 → 高 = 40×0.5 = 20");
        check(a && near(a->size().width, 200), "交叉轴无因子 → 宽撑满 200");

        d.advanceTo(300);
        check(a && near(a->size().height, 40), "进度 1 → 高 = 40（完整尺寸）");
    }

    // 5b. 横轴 + fixedCrossAxisSizeFactor：两轴都按因子收缩。
    {
        auto anim = cui::make_shared<AnimationController>(300.0);
        anim->setEasing(Easing::Linear);
        auto st = cui::make_shared<SizeTransition>(anim, leaf, Axis::Horizontal, std::nullopt,
                                                   0.5);
        Harness h;
        h.attach(cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{st}), 200, 200);
        RenderAlign* a = findRo<RenderAlign>(h.root);
        check(a && a->widthFactor().has_value() && a->heightFactor().has_value() &&
                      near(*a->heightFactor(), 0.5),
              "横轴：widthFactor 取动画值，heightFactor 取 fixedCrossAxisSizeFactor=0.5");

        Driver d(h, anim);
        d.advanceTo(150);
        check(a && near(a->size().width, 20) && near(a->size().height, 20),
              "横轴 0.5 + 交叉因子 0.5 → 20×20");
    }

    // 5d. 横轴、不设交叉因子 → 只有 widthFactor 非空，高撑满。
    {
        auto anim = cui::make_shared<AnimationController>(300.0);
        anim->setEasing(Easing::Linear);
        auto st = cui::make_shared<SizeTransition>(anim, leaf, Axis::Horizontal);
        Harness h;
        h.attach(cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{st}), 200, 200);
        RenderAlign* a = findRo<RenderAlign>(h.root);
        check(a && a->widthFactor().has_value() && !a->heightFactor().has_value(),
              "横轴 + 无交叉因子：仅 widthFactor 非空");

        Driver d(h, anim);
        d.advanceTo(150);
        check(a && near(a->size().width, 20) && near(a->size().height, 200),
              "横轴 0.5 → 20×200（高撑满交叉轴）");
    }

    // 5c. 显式 alignment 覆盖默认的按轴推导。
    {
        auto anim = cui::make_shared<AnimationController>(300.0);
        anim->setEasing(Easing::Linear);
        auto st = cui::make_shared<SizeTransition>(anim, leaf, Axis::Vertical,
                                                   Alignment(1.0, 1.0));
        Harness h;
        h.attach(cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{st}), 200, 200);
        RenderAlign* a = findRo<RenderAlign>(h.root);
        Driver d(h, anim);
        d.advanceTo(150);
        check(a && near(a->size().height, 20), "显式 alignment 下因子仍生效 → 高 20");
    }
}

// ── 6. AlignTransition ──────────────────────────────────────────────────────
void testAlignTransition() {
    std::printf("[test] AlignTransition\n");

    auto anim = cui::make_shared<AnimationController>(300.0);
    anim->setEasing(Easing::Linear);
    auto at = cui::make_shared<AlignTransition>(
            anim, cui::make_shared<SizedBox>(40, 40), Alignment(-1.0, -1.0), Alignment(1.0, 1.0));

    Harness h;
    h.attach(cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{at}), 200, 200);
    RenderAlign* a = findRo<RenderAlign>(h.root);
    check(a != nullptr, "找到 RenderAlign");

    Driver d(h, anim);
    d.advanceTo(0);
    check(a && near(a->size().width, 200), "AlignTransition 无因子 → 撑满交叉轴");

    d.advanceTo(150);  // 中点 → Alignment(0,0)
    // alignment 落在 RenderAlign 内部；用子节点偏移验证：盒 200×200、子 40×40，
    // 居中时偏移 (80,80)。
    RenderObject* child = nullptr;
    if (a)
        a->visitChildren([&](AbstractNode* c) { child = static_cast<RenderObject*>(c); });
    check(child && child->parentData() && near(child->parentData()->offset.dx, 80) &&
                  near(child->parentData()->offset.dy, 80),
          "进度 0.5 → Alignment(0,0)：子节点偏移 (80,80)");

    d.advanceTo(300);
    check(child && child->parentData() && near(child->parentData()->offset.dx, 160) &&
                  near(child->parentData()->offset.dy, 160),
          "进度 1 → Alignment(1,1)：子节点偏移 (160,160)");
}

// ── 7. ListenableBuilder / ValueListenableBuilder ──────────────────────────
// ── 7. 监听通道（AnimatedWidget 族 + ListenableBuilder 族）───────────────────

/// 探针：build 里现造一个 FadeTransition，setAnimation 换掉驱动它的控制器，
/// 用来走 AnimatedWidgetState::didUpdateWidget 的「换订阅对象」分支。
class SwapProbeState;

class SwapProbe : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;

private:
    friend class SwapProbeState;
};

class SwapProbeState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<FadeTransition>(
                anim, cui::make_shared<SizedBox>(10, 10, cui::make_shared<DecoratedBox>(solidColor())));
    }

    void setAnimation(SharedPtr<AnimationController> a) {
        anim = std::move(a);
        setState();
    }

    /// 测试直接读写：attach 前塞进去，让 initState 那条订阅路径也被钉到。
    SharedPtr<AnimationController> anim;
};

inline std::unique_ptr<State> SwapProbe::createState() {
    return std::make_unique<SwapProbeState>();
}

void testListenables() {
    std::printf("[test] 监听通道（AnimatedWidget / ListenableBuilder 族）\n");

    // 7e. AnimatedWidget 族（FadeTransition）挂载 / 卸载的监听表口径。
    //     State 订阅走 addListenerWithHandle，捕获 this 的 lambda 只有句柄能摘。
    {
        auto anim = cui::make_shared<AnimationController>(300.0);
        {
            Harness h;
            h.attach(cui::make_shared<Center>(
                             cui::make_shared<FadeTransition>(
                                     anim, cui::make_shared<SizedBox>(10, 10))),
                     200, 200);
            check(anim->listenerCount() == 1, "FadeTransition 挂载期间持有 1 个监听");
        }
        check(anim->listenerCount() == 0, "FadeTransition 卸载后监听表为空（dispose 已摘除）");
        anim->notifyListeners();  // 树已析构：不得打到悬挂 State
        check(anim->listenerCount() == 0, "卸载后通知不再挂回监听");
    }

    // 7f. 换 animation ⇒ 旧的必须回零，新的挂上（否则会同时被两条时钟驱动）。
    {
        auto a1 = cui::make_shared<AnimationController>(300.0);
        auto a2 = cui::make_shared<AnimationController>(300.0);

        Harness h;
        auto probe = cui::make_shared<SwapProbe>();
        h.attach(probe, 200, 200);
        auto* st = dynamic_cast<SwapProbeState*>(static_cast<StatefulElement*>(h.root)->state());
        check(st != nullptr, "取到探针 State");
        // 首建时 anim 还是空（initState 不订阅），这里才挂上第一条。
        st->setAnimation(a1);
        h.rebuildAndLayout();
        check(a1->listenerCount() == 1 && a2->listenerCount() == 0, "第一段动画挂上");

        st->setAnimation(a2);
        h.rebuildAndLayout();
        check(a1->listenerCount() == 0, "换掉后旧动画的监听归零");
        check(a2->listenerCount() == 1, "新动画挂上 1 个监听");

        // 同一个 controller 再 build 一次：不重订阅（句柄不会被覆盖成两条）。
        st->setAnimation(a2);
        h.rebuildAndLayout();
        check(a2->listenerCount() == 1, "同 animation 重建 ⇒ 不重复订阅");
    }

    // 7a. ListenableBuilder：ChangeNotifier 通知即重建，child 透传给 builder。
    {
        auto notifier = cui::make_shared<ChangeNotifier>();
        int builds = 0;
        auto child = cui::make_shared<SizedBox>(50, 30);
        auto lb = cui::make_shared<ListenableBuilder>(
                notifier,
                [&](BuildContext&, SharedPtr<Widget> c) {
                    ++builds;
                    return c;
                },
                child);

        Harness h;
        h.attach(lb, 200, 200);
        int afterMount = builds;
        check(afterMount == 1, "ListenableBuilder 挂载时构建一次");

        // builder 收到的 child 就是挂载进树的那个。SizedBox 对应
        // RenderConstrainedBox；紧约束下它会撑满窗口，因此只验证节点类型。
        check(findRo<RenderConstrainedBox>(h.root) != nullptr,
              "builder 返回的 child 已挂载进树");

        notifier->notifyListeners();
        h.rebuildAndLayout();
        check(builds == afterMount + 1, "notifyListeners → 重建一次");

        // 同值不再通知由 ValueNotifier 负责；这里连续两次通知应重建两次。
        notifier->notifyListeners();
        h.rebuildAndLayout();
        check(builds == afterMount + 2, "再次通知 → 再次重建");
    }

    // 7b. ValueListenableBuilder：builder 收到新值。
    {
        auto count = cui::make_shared<ValueNotifier<int>>(0);
        std::vector<int> seen;
        auto vlb = cui::make_shared<ValueListenableBuilder<int>>(
                count,
                [&](BuildContext&, int v, SharedPtr<Widget>) {
                    seen.push_back(v);
                    return cui::make_shared<SizedBox>(10 + v, 20);
                });

        // 外包 Center 让 SizedBox 拿到松约束，尺寸才反映 builder 的返回值。
        Harness h;
        h.attach(cui::make_shared<Center>(vlb), 200, 200);
        check(!seen.empty() && seen.front() == 0, "初始构建收到值 0");

        count->setValue(5);
        h.rebuildAndLayout();
        check(seen.size() == 2 && seen.back() == 5, "setValue(5) → 收到 5 并重建");

        count->setValue(5);  // 同值不通知（上游 ValueNotifier 语义）
        h.rebuildAndLayout();
        check(seen.size() == 2, "同值 setValue 不触发重建");

        auto* sb = findRo<RenderConstrainedBox>(h.root);
        check(sb != nullptr && near(sb->size().width, 15) && near(sb->size().height, 20),
              "重建后的子树尺寸随值变化（10+5=15 × 20）");
    }

    // 7c. 卸载时必须摘除监听：否则 notifyListeners 会打到已销毁的 State。
    {
        auto notifier = cui::make_shared<ChangeNotifier>();
        int builds = 0;
        {
            Harness h;
            h.attach(cui::make_shared<ListenableBuilder>(
                             notifier,
                             [&](BuildContext&, SharedPtr<Widget>) {
                                 ++builds;
                                 return cui::make_shared<SizedBox>(10, 10);
                             }),
                     200, 200);
            check(builds == 1, "作用域内挂载并构建");
            // 捕获 this 的 lambda 无法靠 removeListener 的目标比较摘除，必须走
            // 句柄通道；这里直接断言监听表已空（无需 ASan 即可发现泄漏）。
            check(notifier->listenerCount() == 1, "挂载期间持有 1 个监听");
        }
        check(notifier->listenerCount() == 0, "卸载后监听表为空（dispose 已摘除）");
        // 树已析构；此处通知不应崩溃，也不应再触发构建。
        notifier->notifyListeners();
        check(builds == 1, "卸载后 notifyListeners 不再触发重建（监听已摘除）");
    }

    // 7d. ValueListenableBuilder 同样要在卸载时摘除监听。
    {
        auto count = cui::make_shared<ValueNotifier<int>>(0);
        {
            Harness h;
            h.attach(cui::make_shared<ValueListenableBuilder<int>>(
                             count,
                             [](BuildContext&, int, SharedPtr<Widget>) {
                                 return cui::make_shared<SizedBox>(10, 10);
                             }),
                     200, 200);
            check(count->listenerCount() == 1, "VLB 挂载期间持有 1 个监听");
        }
        check(count->listenerCount() == 0, "VLB 卸载后监听表为空");
    }
}

}  // namespace

int main() {
    std::printf("=== test_transition ===\n");
    testAlignFactors();
    testFadeTransition();
    testMatrixTransitions();
    testSlideTransition();
    testSizeTransition();
    testAlignTransition();
    testListenables();
    std::printf("\n=== %s: %d failure(s) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
