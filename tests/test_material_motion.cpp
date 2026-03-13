/// @file test_material_motion.cpp
/// @brief Material Motion 转场族（widgets/material_motion.h）单元测试。
///
/// 覆盖：
///   1. 曲线辅助：Cubic bezier 求解端点、Interval 映射、FadeThrough 帧函数。
///   2. DualTransitionBuilder：有效方向状态机（Forward/Reverse 中途打断沿用
///      上一次方向，端点才切换）与两个 ProxyAnimation 的取值。
///   3. FadeScaleTransition：入场前 30% 淡入 + 0.8→1.0 缩放，退场只淡出。
///   4. FadeThroughTransition：新内容隐藏/旧内容可见的初始态与互换后的取值。
///   5. SharedAxisTransition：Horizontal/Vertical 的 ±30px 平移、Scaled 的
///      0.8/1.1 缩放，以及 fillColor 与退场前 30% 淡出。
///   6. PageTransitionSwitcher：换子时新旧两条目并存、播完自动摘除；
///      reverse 模式同一路径。
///   7. OpenContainer：closed/open 交叉淡入、背景色补间、toggle() 起播到收敛。
///
/// 全部 headless。controller 的 tick 走「Ticker 绝对时间 - start 基准」，测试
/// 里用真实 sleep + tickAllActive 推帧；纯数值用例直接 setValue + tick 断言。

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
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/material_motion.h"
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

/// Widget → BuildOwner + RenderView + PipelineOwner。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww = 300.0, double hh = 200.0) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        po.flushLayout();
    }

    void rebuild() {
        if (root)
            root->markNeedsBuild();
        owner.flushBuildScope();
        po.flushLayout();
    }

    /// 只重建根的直接子（宿主持有同一个 Widget 实例、原地改参数时的路径）。
    /// 根 rebuild 只会把同一个 shared_ptr 交回去，`updateChild` 见到同一个
    /// 实例直接跳过，StatefulWidget 的 build 根本不会被调用。
    void rebuildChild() {
        if (!root)
            return;
        Element* child = nullptr;
        root->visitChildElements([&child](Element* e) { child = e; });
        if (child)
            child->markNeedsBuild();
        owner.flushBuildScope();
        po.flushLayout();
    }
};

/// 先序遍历收集指定类型的 RenderObject（Stack/Fade 的嵌套顺序就是绘制顺序）。
template <typename T>
std::vector<T*> collectRo(Element* root) {
    std::vector<T*> out;
    RenderObject* ro = root ? root->findRenderObject() : nullptr;
    if (!ro)
        return out;
    std::vector<RenderObject*> stack{ro};
    while (!stack.empty()) {
        RenderObject* n = stack.back();
        stack.pop_back();
        if (auto* hit = dynamic_cast<T*>(n))
            out.push_back(hit);
        std::vector<RenderObject*> kids;
        n->visitChildren([&](AbstractNode* c) {
            if (c)
                kids.push_back(static_cast<RenderObject*>(c));
        });
        // visitChildren 是正序；压栈后弹出会反向，所以逆序压。
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.push_back(*it);
    }
    return out;
}

/// 跑真实帧直到所有 Ticker 停下（上限 maxMs 兜底）。
void settle(Harness& h, double maxMs = 1500.0) {
    double spent = 0.0;
    while (spent < maxMs && Ticker::hasActiveTickers()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        Ticker::tickAllActive(Duration(8));
        h.owner.flushBuildScope();
        h.po.flushLayout();
        spent += 8.0;
    }
}

SharedPtr<Widget> box(double w, double h, Color c = Color::fromRGB(0.8, 0.8, 0.8)) {
    return cui::make_shared<SizedBox>(
            w, h, cui::make_shared<DecoratedBox>(BoxDecoration{c, 4.0}));
}

/// 松散约束的宿主：RenderView 给的是窗口紧约束，转场子树要自己定尺寸。
SharedPtr<Widget> loose(SharedPtr<Widget> child) {
    return cui::make_shared<Align>(-1.0, -1.0, std::move(child));
}

// ── 1. 曲线辅助 ─────────────────────────────────────────────────────────────
void testCurves() {
    std::printf("[test] 曲线辅助\n");
    using namespace material_motion;
    check(near(cubicTransform(0.4, 0.0, 0.2, 1.0, 0.0), 0.0), "Cubic 端点 x=0 → y=0");
    check(near(cubicTransform(0.4, 0.0, 0.2, 1.0, 1.0), 1.0), "Cubic 端点 x=1 → y=1");
    const double a = legacy()(0.25);
    const double b = legacy()(0.75);
    check(a > 0.0 && a < b && b < 1.0, "Easing.legacy 单调递增且落在开区间内");

    check(near(intervalValue(0.3, 1.0, 0.2), 0.0), "Interval 左端外 → 0");
    check(near(intervalValue(0.3, 1.0, 0.3), 0.0), "Interval 左端点 → 0");
    check(near(intervalValue(0.3, 1.0, 0.65), 0.5), "Interval 中点 → 0.5");
    check(near(intervalValue(0.3, 1.0, 1.0), 1.0), "Interval 右端点 → 1");

    check(near(fadeThroughInOpacity(0.0), 0.0), "FadeThrough 入场：前 30% 不透明 0");
    check(near(fadeThroughInOpacity(0.3), 0.0), "FadeThrough 入场：0.3 仍为 0");
    check(near(fadeThroughInOpacity(1.0), 1.0), "FadeThrough 入场：终点 1");
    check(near(fadeThroughInScale(0.0), 0.92) && near(fadeThroughInScale(0.3), 0.92),
          "FadeThrough 缩放：前 30% 保持 0.92");
    check(near(fadeThroughInScale(1.0), 1.0), "FadeThrough 缩放：终点 1.0");
    check(near(fadeThroughOutOpacity(0.0), 1.0), "FadeThrough 退场：起点 1");
    check(near(fadeThroughOutOpacity(0.3), 0.0) && near(fadeThroughOutOpacity(1.0), 0.0),
          "FadeThrough 退场：前 30% 内淡到 0 并保持");
}

// ── 2. DualTransitionBuilder ───────────────────────────────────────────────
void testDualTransitionBuilder() {
    std::printf("[test] DualTransitionBuilder 有效方向\n");

    auto ctrl = cui::make_shared<AnimationController>(100.0);
    ctrl->setEasing(Easing::Linear);

    // 两个 builder 都包成 FadeTransition（订阅各自 proxy 的逐帧值），并把
    // 上游约定必须消费的 child 串起来：reverse 的结果就是 forward 的 child。
    auto branch = [](BuildContext&, SharedPtr<Animation<double>> anim,
                     SharedPtr<Widget> child) -> SharedPtr<Widget> {
        SharedPtr<Widget> kid = child ? child : box(40.0, 20.0);
        return cui::make_shared<FadeTransition>(anim, kid);
    };
    auto dual = cui::make_shared<DualTransitionBuilder>(ctrl, branch, branch, box(10.0, 10.0));

    Harness h;
    h.attach(loose(dual));
    auto opacities = [&] { return collectRo<RenderOpacity>(h.root); };

    auto v = opacities();
    check(v.size() == 2, "结构：forward / reverse 各一层 Opacity");
    if (v.size() == 2) {
        check(near(v[0]->opacity(), 0.0), "Dismissed：forward 拿真实动画（0）");
        check(near(v[1]->opacity(), 0.0), "Dismissed：reverse 拿 kAlwaysDismissed（0）");
    }

    ctrl->setValue(0.5);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacities();
    if (v.size() == 2) {
        check(near(v[0]->opacity(), 0.5), "中途值：forward 跟随父动画 0.5");
        check(near(v[1]->opacity(), 0.0), "中途值：reverse 仍是 0");
    }

    ctrl->setValue(1.0);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacities();
    if (v.size() == 2) {
        check(near(v[0]->opacity(), 1.0), "Completed：forward 拿 kAlwaysComplete（1）");
        check(near(v[1]->opacity(), 0.0), "Completed：reverse 拿 ReverseAnimation（1-1=0）");
    }

    // 反向：从 Completed 出发 ⇒ 有效方向切到 Reverse。
    ctrl->reverse();
    ctrl->tick(Duration(40.0));
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacities();
    if (v.size() == 2) {
        check(near(v[0]->opacity(), 1.0), "Reverse 中途：forward 固定在 1");
        check(near(v[1]->opacity(), 0.4), "Reverse 中途：reverse = 1-0.6 = 0.4");
    }

    // 反向途中转正向：上一次有效方向是 Reverse ⇒ 继续沿用 Reverse。
    ctrl->forward();
    ctrl->tick(Duration(80.0));
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacities();
    if (v.size() == 2) {
        check(near(v[0]->opacity(), 1.0), "打断后：forward 仍固定在 1（沿用 Reverse）");
        check(near(v[1]->opacity(), 0.24), "打断后：reverse = 1-0.76 = 0.24");
    }

    ctrl->setValue(0.0);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacities();
    if (v.size() == 2) {
        check(near(v[0]->opacity(), 0.0) && near(v[1]->opacity(), 0.0),
              "回到 Dismissed：两侧都回到 0");
    }
}

// ── 3. FadeScaleTransition ─────────────────────────────────────────────────
void testFadeScale() {
    std::printf("[test] FadeScaleTransition\n");
    auto ctrl = cui::make_shared<AnimationController>(100.0);
    ctrl->setEasing(Easing::Linear);
    auto transition = cui::make_shared<FadeScaleTransition>(ctrl, box(60.0, 40.0));

    Harness h;
    h.attach(loose(transition));

    auto opacity = [&] { return collectRo<RenderOpacity>(h.root); };
    auto transform = [&] { return collectRo<RenderTransform>(h.root); };

    auto v = opacity();
    check(v.size() == 2, "结构：入场 Fade 包住退场 Fade");
    auto t = transform();
    check(t.size() == 1, "结构：只有入场一侧带 Scale");
    check(!t.empty() && near(t[0]->transform().at(0, 0), 0.80), "入场：缩放从 0.80 起");
    check(v.size() == 2 && near(v[0]->opacity(), 0.0), "入场：前 30% 之前不透明 0");
    // 正向段 reverse 侧拿 kAlwaysDismissed（0），Tween(1→0) 求值即 1。
    check(v.size() == 2 && near(v[1]->opacity(), 1.0), "正向段：退场侧固定为 Tween(1→0) 的起点 1");

    ctrl->setValue(0.5);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacity();
    check(v.size() == 2 && near(v[0]->opacity(), 1.0), "入场：过 30% 后完全淡入");
    check(v.size() == 2 && near(v[1]->opacity(), 1.0), "中途：退场侧仍是 kAlwaysDismissed（1）");
    t = transform();
    check(!t.empty() && t[0]->transform().at(0, 0) > 0.80 && t[0]->transform().at(0, 0) < 1.0,
          "入场：缩放介于 0.80 与 1.0 之间");

    ctrl->setValue(1.0);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacity();
    check(v.size() == 2 && near(v[0]->opacity(), 1.0), "Completed：入场侧保持 1");
    check(v.size() == 2 && near(v[1]->opacity(), 1.0),
          "Completed：退场侧 = Tween(1→0)∘ReverseAnimation(1) = 1（静息态可见）");
    t = transform();
    check(!t.empty() && near(t[0]->transform().at(0, 0), 1.0), "Completed：缩放回到 1.0");

    // 反向起播：reverse 侧拿 ReverseAnimation，值从 0 往 1 走 ⇒ 淡出。
    ctrl->reverse();
    ctrl->tick(Duration(40.0));
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacity();
    check(v.size() == 2 && near(v[0]->opacity(), 1.0), "Reverse：入场侧固定 kAlwaysComplete");
    check(v.size() == 2 && near(v[1]->opacity(), 0.6),
          "Reverse 40%：退场侧 = ReverseAnimation(0.6) ⇒ 0.6");
}

// ── 4. FadeThroughTransition ───────────────────────────────────────────────
void testFadeThrough() {
    std::printf("[test] FadeThroughTransition\n");
    auto primary = cui::make_shared<AnimationController>(100.0);
    auto secondary = cui::make_shared<AnimationController>(100.0);
    primary->setEasing(Easing::Linear);
    secondary->setEasing(Easing::Linear);
    auto transition = cui::make_shared<FadeThroughTransition>(primary, secondary,
                                                             box(60.0, 40.0));

    Harness h;
    h.attach(loose(transition));

    auto opacities = [&] { return collectRo<RenderOpacity>(h.root); };
    auto v = opacities();
    // 两处 _ZoomedFadeInFadeOut：外层（animation）+ 内层（ReverseAnimation(
    // secondary)），每个 DualTransitionBuilder 同时挂入场/退场两侧 ⇒ 4 层 Fade。
    check(v.size() == 4, "结构：外层 + 内层各 forward/reverse 两层 Fade");
    if (v.size() != 4) {
        std::printf("    [debug] opacity count = %zu\n", v.size());
        return;
    }
    check(near(v[0]->opacity(), 0.0), "起点：新内容（外层入场侧）不可见");
    check(near(v[1]->opacity(), 1.0), "起点：外层退场侧固定在 Tween(1→0) 起点 1");
    check(near(v[2]->opacity(), 1.0), "起点：旧内容（内层入场侧 = ReverseAnimation(0)=1）可见");
    check(near(v[3]->opacity(), 1.0), "起点：内层退场侧固定 1（同一 kAlwaysDismissed 语义）");

    primary->setValue(1.0);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacities();
    check(near(v[0]->opacity(), 1.0), "终点：新内容完全可见");
    check(near(v[2]->opacity(), 1.0), "secondary 未动：旧内容仍可见");

    // 新内容压上来（secondary 走 50%）：内层退场侧按前 30% 加速淡出到 0。
    secondary->setValue(0.5);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacities();
    check(near(v[2]->opacity(), 1.0), "覆盖中途：内层入场侧固定 kAlwaysComplete");
    check(near(v[3]->opacity(), 0.0), "覆盖中途：内层退场侧已淡到 0");
}

// ── 5. SharedAxisTransition ────────────────────────────────────────────────
void testSharedAxis() {
    std::printf("[test] SharedAxisTransition\n");

    // 5a. Horizontal：起点向右偏 30px。
    {
        auto ctrl = cui::make_shared<AnimationController>(100.0);
        ctrl->setEasing(Easing::Linear);
        auto secondary = cui::make_shared<AnimationController>(100.0);
        secondary->setEasing(Easing::Linear);
        auto transition = cui::make_shared<SharedAxisTransition>(
                ctrl, secondary, SharedAxisTransitionType::Horizontal, box(60.0, 40.0));

        Harness h;
        h.attach(loose(transition));
        auto t = collectRo<RenderTransform>(h.root);
        // 树形：外层(forward=enter → 内层(forward=enter → reverse=exit) → reverse=exit)
        // ⇒ Transform 前序为 [外层 enter, 外层 exit, 内层 enter, 内层 exit]。
        check(t.size() == 4, "Horizontal：入场/退场各两层 Transform");
        if (t.size() == 4) {
            check(near(t[0]->transform().at(0, 3), 30.0), "Horizontal 入场：x 位移 30");
            check(near(t[0]->transform().at(1, 3), 0.0), "Horizontal 入场：y 位移 0");
            check(near(t[1]->transform().at(0, 3), 0.0), "Horizontal 退场侧：secondary=0 → 归位");
        }

        ctrl->setValue(1.0);
        h.owner.flushBuildScope();
        h.po.flushLayout();
        t = collectRo<RenderTransform>(h.root);
        check(!t.empty() && near(t[0]->transform().at(0, 3), 0.0), "Horizontal 终点：x 回到 0");
    }

    // 5b. Vertical：起点向下偏 30px。
    {
        auto ctrl = cui::make_shared<AnimationController>(100.0);
        ctrl->setEasing(Easing::Linear);
        auto secondary = cui::make_shared<AnimationController>(100.0);
        secondary->setEasing(Easing::Linear);
        auto transition = cui::make_shared<SharedAxisTransition>(
                ctrl, secondary, SharedAxisTransitionType::Vertical, box(60.0, 40.0));
        Harness h;
        h.attach(loose(transition));
        auto t = collectRo<RenderTransform>(h.root);
        check(t.size() == 4, "Vertical：入场/退场各两层 Transform");
        if (t.size() == 4) {
            check(near(t[0]->transform().at(1, 3), 30.0), "Vertical 入场：y 位移 30");
            check(near(t[0]->transform().at(0, 3), 0.0), "Vertical 入场：x 位移 0");
        }
    }

    // 5c. Scaled：0.80 → 1.00（入场），退场 1.00 → 1.10。
    {
        auto ctrl = cui::make_shared<AnimationController>(100.0);
        ctrl->setEasing(Easing::Linear);
        auto secondary = cui::make_shared<AnimationController>(100.0);
        secondary->setEasing(Easing::Linear);
        auto transition = cui::make_shared<SharedAxisTransition>(
                ctrl, secondary, SharedAxisTransitionType::Scaled, box(60.0, 40.0));
        Harness h;
        h.attach(loose(transition));
        auto t = collectRo<RenderTransform>(h.root);
        check(t.size() == 4, "Scaled：入场/退场各两层 Scale");
        if (t.size() == 4) {
            check(near(t[0]->transform().at(0, 0), 0.80), "Scaled 入场：从 0.80 起");
            check(near(t[1]->transform().at(0, 0), 1.0), "Scaled 退场侧：secondary=0 → 1.0");
        }

        ctrl->setValue(1.0);
        h.owner.flushBuildScope();
        h.po.flushLayout();
        t = collectRo<RenderTransform>(h.root);
        check(!t.empty() && near(t[0]->transform().at(0, 0), 1.0), "Scaled 入场终点：1.0");
    }

    // 5d. 退场前 30% 淡出：secondary 走到 50% ⇒ 内层退场侧（v[3]）为 0，
    //     而内层入场侧仍固定 kAlwaysComplete（child 还在，只是被淡出盖住）。
    {
        auto ctrl = cui::make_shared<AnimationController>(100.0);
        ctrl->setEasing(Easing::Linear);
        auto secondary = cui::make_shared<AnimationController>(100.0);
        secondary->setEasing(Easing::Linear);
        auto transition = cui::make_shared<SharedAxisTransition>(
                ctrl, secondary, SharedAxisTransitionType::Horizontal, box(60.0, 40.0));
        Harness h;
        h.attach(loose(transition));
        secondary->setValue(0.5);
        h.owner.flushBuildScope();
        h.po.flushLayout();
        auto v = collectRo<RenderOpacity>(h.root);
        check(v.size() == 4, "Opacity：入场/退场各两层");
        if (v.size() == 4) {
            check(near(v[0]->opacity(), 0.0), "入场侧：primary=0 → 新内容尚不可见");
            check(near(v[2]->opacity(), 1.0), "旧内容：内层入场侧固定 kAlwaysComplete");
            check(near(v[3]->opacity(), 0.0), "覆盖 50%：内层退场侧淡到 0");
        }
    }
}

// ── 6. PageTransitionSwitcher ──────────────────────────────────────────────
void testPageTransitionSwitcher() {
    std::printf("[test] PageTransitionSwitcher\n");

    auto page = [](int i) {
        return cui::make_shared<KeyedSubtree>(
                cui::make_shared<const ValueKey<int>>(i), box(80.0, 50.0));
    };
    auto switcher = cui::make_shared<PageTransitionSwitcher>(
            page(0),
            [](SharedPtr<Widget> child, SharedPtr<Animation<double>> primary,
               SharedPtr<Animation<double>> secondary) {
                return cui::make_shared<FadeThroughTransition>(primary, secondary, child);
            },
            100.0);

    Harness h;
    h.attach(loose(switcher));
    auto stacks = [&] { return collectRo<RenderStack>(h.root); };
    check(stacks().size() == 1 && stacks()[0]->children().size() == 1,
          "初始：只有当前条目，且直接到位（不做入场动画）");

    switcher->setChild(page(1));
    h.rebuildChild();
    check(stacks().size() == 1 && stacks()[0]->children().size() == 2,
          "换子：旧条目淡出 + 新条目淡入同时存在");

    settle(h);
    check(stacks()[0]->children().size() == 1, "播完：旧条目自动摘除，只剩当前条目");

    // reverse 模式：旧子 primary 反放、新子 secondary 反放。
    auto revSwitcher = cui::make_shared<PageTransitionSwitcher>(
            page(0),
            [](SharedPtr<Widget> child, SharedPtr<Animation<double>> primary,
               SharedPtr<Animation<double>> secondary) {
                return cui::make_shared<SharedAxisTransition>(
                        primary, secondary, SharedAxisTransitionType::Scaled, child);
            },
            100.0, true);
    Harness h2;
    h2.attach(loose(revSwitcher));
    revSwitcher->setChild(page(1));
    h2.rebuildChild();
    auto s2 = collectRo<RenderStack>(h2.root);
    check(!s2.empty() && s2[0]->children().size() == 2, "reverse：新旧条目同时存在");
    settle(h2);
    s2 = collectRo<RenderStack>(h2.root);
    check(!s2.empty() && s2[0]->children().size() == 1, "reverse：旧条目同样被摘除");
}

// ── 7. OpenContainer ──────────────────────────────────────────────────────
void testOpenContainer() {
    std::printf("[test] OpenContainer\n");
    const Color closedColor = Color::fromRGB(0.90, 0.90, 1.0);
    const Color openColor = Color::fromRGB(0.20, 0.40, 0.85);
    auto ctrl = cui::make_shared<AnimationController>(100.0);
    ctrl->setEasing(Easing::Linear);
    auto container = cui::make_shared<OpenContainer>(
            ctrl, box(60.0, 40.0), box(80.0, 60.0), closedColor, openColor, 8.0);

    Harness h;
    h.attach(loose(container));

    auto opacities = [&] { return collectRo<RenderOpacity>(h.root); };
    auto decos = [&] { return collectRo<RenderDecoratedBox>(h.root); };

    auto v = opacities();
    check(v.size() == 2, "结构：closed/open 两层 Fade");
    check(near(v[0]->opacity(), 1.0) && near(v[1]->opacity(), 0.0),
          "初始：closed 可见、open 不可见");
    auto d = decos();
    check(!d.empty() && d[0]->color() == closedColor, "初始：容器背景 = closedColor");

    ctrl->setValue(0.5);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacities();
    check(near(v[0]->opacity(), 0.5) && near(v[1]->opacity(), 0.5),
          "中途：两层各 0.5 交叉淡入");
    d = decos();
    check(!d.empty() && near(d[0]->color().r, 0.55) && near(d[0]->color().b, 0.925),
          "中途：背景色按进度插值");

    ctrl->setValue(1.0);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    v = opacities();
    check(near(v[0]->opacity(), 0.0) && near(v[1]->opacity(), 1.0),
          "终点：open 完全可见、closed 淡出");
    d = decos();
    check(!d.empty() && near(d[0]->color().r, openColor.r) && near(d[0]->color().g, openColor.g) &&
                  near(d[0]->color().b, openColor.b),
          "终点：容器背景 = openColor");

    // toggle：0 → forward 到 1；再 toggle → reverse 回 0。
    ctrl->setValue(0.0);
    h.owner.flushBuildScope();
    h.po.flushLayout();
    check(near(container->progress(), 0.0), "toggle 前 progress = 0");
    container->toggle();
    check(ctrl->isRunning(), "toggle：从 0 出发 → forward 起播");
    settle(h);
    check(near(ctrl->value(), 1.0), "toggle：收敛到 1（打开）");
    check(container->isOpen(), "isOpen() 为真");

    container->toggle();
    check(ctrl->isRunning(), "再次 toggle：从 1 出发 → reverse");
    settle(h);
    check(near(ctrl->value(), 0.0), "再次 toggle：收敛到 0（关闭）");
    check(!container->isOpen(), "isOpen() 为假");
}

}  // namespace

int main() {
    // 控制台/重定向都按行刷新，崩溃时也能看到最后一条断言的进度。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_material_motion ===\n");
    testCurves();
    testDualTransitionBuilder();
    testFadeScale();
    testFadeThrough();
    testSharedAxis();
    testPageTransitionSwitcher();
    testOpenContainer();

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
