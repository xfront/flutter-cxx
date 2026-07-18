/// @file test_animation.cpp
/// @brief 控件层动画驱动 + 液态玻璃切换 单元测试。
///
/// 覆盖：
///   1. AnimatedBuilder：AnimationController 每帧推进 → 订阅触发 setState → 子树重建
///   2. SharedAnimation 自驱动（全局 Ticker）+ FrameScheduler 帧泵
///   3. Tween<double>/<Color> 与 Easing 插值正确性
///   4. LiquidGlass：setEnabled 切换 → 动画驱动 t 0→1 淡入淡出
///   5. CurvedAnimation：reverseCurve 方向锁定、端点保护、值/状态转发与句柄摘除
///   6. ReverseAnimation：值取反 + 状态四向镜像
///   7. ProxyAnimation：换 parent 的重订阅 / 补发判定 / null 快照兜底
///   8. Tween::animate 产物（EvaluatedAnimation）可订阅且析构即摘除
///   9. loop / autoReverse 的 tick() 语义
///
/// 全部 headless（无窗口）。动画推进用 AnimationController::tick(Duration) 确定性驱动；
/// 自驱动用例用真实时间（sleep）验证全局 Ticker 路径。
///
/// 注意：本测试的帧泵刻意跳过 flushPaint()——RenderView 是一个 repaint
/// boundary，flushPaint 会触发 RenderView::repaintBoundaryPaint() 需要一个真实的
/// SkCanvas，在无窗口的 headless 环境下不可用。动画→重建→布局这条链路不需要
/// 真实绘制即可验证；真实绘制由生产环境的 FrameScheduler（pump 含 flushPaint）
/// 在持有画布时驱动。

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/animated_builder.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/animation/frame_scheduler.h"
#include "cui/widgets/animation/shared_animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/liquid_glass.h"

using namespace cui;

namespace {

int g_failures = 0;

/// 浮点近似比较。
bool near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

/// 把 Widget 树接入 BuildOwner + RenderView + PipelineOwner，并布局好。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;  // 必须最后声明，保证其成员 FocusManager 销毁时仍存活
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        po.flushLayout();
    }

    /// 仅重建脏 Element（动画订阅回调在 flush 前已 markNeedsBuild）。
    void flushBuild() { owner.flushBuildScope(); }

    /// 一帧（headless 安全版）：tick 所有活跃 Ticker → 重建 → layout。
    /// 省略 flushPaint —— 它需要一个真实 SkCanvas，headless 不可用。
    void pumpFrame(Duration dt) {
        Ticker::tickAllActive(dt);
        owner.flushBuildScope();
        po.flushLayout();
        po.flushCompositingBits();
    }
};

// ── 测试 1：AnimatedBuilder 随动画重建子树 ─────────────────────────────────
void testAnimatedBuilderRebuild() {
    std::printf("[test] AnimatedBuilder 随动画重建子树\n");

    std::vector<double> captured;
    auto anim = cui::make_shared<AnimationController>(300.0);  // 默认 Linear 缓动
    auto builder = [&](BuildContext&, const SharedPtr<Animation<double>>& a)
            -> SharedPtr<Widget> {
        captured.push_back(a ? a->value() : 0.0);
        return cui::make_shared<SizedBox>(100, 100);
    };
    auto ab = cui::make_shared<AnimatedBuilder>(anim, builder);

    Harness h;
    h.attach(ab, 200, 200);

    check(captured.size() >= 1 && std::abs(captured[0]) < 1e-9, "初始构建 t=0");

    // 确定性推进：start() 后每帧 tick 累计时间（绝对 elapsed）。
    anim->start();
    double ms = 50;
    for (int i = 0; i < 6; ++i) {
        anim->tick(Duration(ms));
        h.flushBuild();
        ms += 50;
    }
    check(captured.size() >= 6, "动画每帧触发重建（≥6 次）");
    check(captured.back() > 0.99, "末帧 t≈1（动画完成）");

    bool mono = true;
    for (size_t i = 1; i < captured.size(); ++i)
        if (captured[i] < captured[i - 1] - 1e-9)
            mono = false;
    check(mono, "t 随动画单调递增");
}

// ── 测试 2：SharedAnimation 自驱动（全局 Ticker）+ FrameScheduler ───────────
void testSharedAnimationSelfDrive() {
    std::printf("[test] SharedAnimation 自驱动 + FrameScheduler\n");

    std::vector<double> captured;
    auto anim = cui::make_shared<SharedAnimation>(300.0, Easing::EaseOutCubic);
    // 直接订阅底层 AnimationController 的 progressChanged，验证全局 Ticker 真的在驱动它。
    auto unsub = anim->animation().progressChanged().subscribe(
        [&](double v) { captured.push_back(v); });

    Harness h;
    h.attach(cui::make_shared<SizedBox>(100, 100), 200, 200);
    int initial = static_cast<int>(captured.size());

    anim->forward();  // 启动全局 Ticker（自驱动）
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        h.pumpFrame(Duration(16));
    }
    check(captured.size() - initial >= 2, "自驱动动画触发多次重建（progressChanged 多次触发）");
    check(anim->value() > 0.3, "约 320ms 后进度 > 0.3");
    check(anim->value() <= 1.0001, "进度不超过 1");
    anim->stop();  // 清理全局 Ticker
    unsub();
}

// ── 测试 3：Tween 插值正确性 ───────────────────────────────────────────────
void testTweens() {
    std::printf("[test] Tween 插值正确性\n");

    Tween<double> td(0.0, 100.0);
    check(std::abs(td.lerp(0.5) - 50.0) < 1e-9, "Tween<double>(0,100).lerp(0.5)==50");
    check(std::abs(td.lerp(0.0)) < 1e-9, "lerp(0)==begin");
    check(std::abs(td.lerp(1.0) - 100.0) < 1e-9, "lerp(1)==end");

    Tween<Color> tc(Color::fromRGB(1, 0, 0), Color::fromRGB(0, 0, 1));
    Color mid = tc.lerp(0.5);
    check(std::abs(mid.r - 0.5) < 1e-9 && std::abs(mid.b - 0.5) < 1e-9,
          "Tween<Color> 中点为紫 (r=b=0.5)");

    check(applyEasing(0.0, Easing::EaseInCubic) < 1e-9, "EaseInCubic(0)=0");
    check(std::abs(applyEasing(1.0, Easing::EaseInCubic) - 1.0) < 1e-9, "EaseInCubic(1)=1");
    // EaseInCubic 在 0.5 处应小于线性 0.5（缓入）。
    check(applyEasing(0.5, Easing::EaseInCubic) < 0.5, "EaseInCubic(0.5) < 0.5（缓入）");
}

// ── 测试 4：LiquidGlass 切换动画 ───────────────────────────────────────────
void testLiquidGlassToggle() {
    std::printf("[test] LiquidGlass 切换动画\n");

    std::vector<double> captured;
    auto glass = cui::make_shared<LiquidGlass>(
            cui::make_shared<SizedBox>(200, 100), 300.0, Easing::EaseOutCubic,
            GlassConfig::standard(), [&](double t) { captured.push_back(t); });

    Harness h;
    h.attach(glass, 200, 100);

    check(std::abs(captured[0]) < 1e-9, "初始关闭：t=0（玻璃不可见）");
    check(!glass->enabled(), "初始 enabled=false");

    // 确定性驱动底层动画（绕过真实时间 Ticker）。
    auto& a = glass->sharedAnimation()->animation();
    a.start();
    double ms = 50;
    for (int i = 0; i < 6; ++i) {
        a.tick(Duration(ms));
        h.flushBuild();
        ms += 50;
    }
    check(captured.back() > 0.99, "开启后动画推进到 t≈1（玻璃全显）");
    check(captured.size() >= 6, "玻璃随动画重建（≥6 次）");

    // 切换 API 接线：setEnabled 启动 / 停止动画。
    glass->setEnabled(true);
    check(glass->enabled() && glass->sharedAnimation()->isRunning(),
          "setEnabled(true) 启动动画");
    glass->sharedAnimation()->stop();
    glass->setEnabled(false);
    check(!glass->enabled(), "setEnabled(false) 关闭");
}

// ── 可控的假动画：只给三个「转发件」当 parent ───────────────────────────────
///
/// CurvedAnimation / ReverseAnimation / ProxyAnimation 的行为只取决于 parent 的
/// 值、状态与通知节奏，用真 AnimationController 测会被时钟语义干扰，故用假件。
class FakeAnimation : public Animation<double> {
public:
    double value() const override { return value_; }
    bool isAnimating() const override { return animating_; }

    void setValue(double v) {
        value_ = v;
        notifyListeners();
    }
    /// 复刻 AnimationController 的节奏：每次都调 dispatchStatus（同状态自动早退）。
    void setStatus(AnimationStatus s) { dispatchStatus(s); }
    void setAnimating(bool a) { animating_ = a; }

private:
    double value_ = 0.0;
    bool animating_ = false;
};

// ── 测试 5：CurvedAnimation（reverseCurve / 端点保护 / 监听转发）─────────────
void testCurvedAnimation() {
    std::printf("[test] CurvedAnimation 曲线选择与监听\n");

    CurveFn fwd = makeCurveFn(Easing::EaseInCubic);  // t^3
    CurveFn rev = flipCurveFn(fwd);                  // 1 - (1-t)^3
    // 端点故意不落在 0/1 的曲线：上游 会短路返回端点，本仓必须一致。
    CurveFn overshoot = [](double t) { return t * t + 0.05; };

    auto parent = cui::make_shared<FakeAnimation>();
    parent->setStatus(AnimationStatus::Forward);
    CurvedAnimation noReverse(parent, overshoot);
    parent->setValue(0.0);
    check(near(noReverse.value(), 0.0), "端点保护：t=0 返回 0（不是 0.05）");
    parent->setValue(1.0);
    check(near(noReverse.value(), 1.0), "端点保护：t=1 返回 1（不是 1.05）");

    parent->setStatus(AnimationStatus::Reverse);
    parent->setValue(0.5);
    check(near(noReverse.value(), overshoot(0.5)), "reverseCurve 为空 ⇒ 反向也用正曲线");
    check(noReverse.progress() == noReverse.value(), "progress() 是 value() 的别名");

    // 方向锁定：一段播放期间不换曲线，只有回到端点才解锁（上游 _updateCurveDirection）。
    auto clock = cui::make_shared<FakeAnimation>();
    clock->setValue(0.5);
    clock->setStatus(AnimationStatus::Forward);
    CurvedAnimation both(clock, fwd, rev);
    check(near(both.value(), 0.125), "锁定 Forward ⇒ 正曲线 (0.5^3)");
    clock->setStatus(AnimationStatus::Reverse);
    check(near(both.value(), 0.125), "中途换向不换曲线（避免视觉跳变）");
    clock->setStatus(AnimationStatus::Dismissed);
    clock->setStatus(AnimationStatus::Reverse);
    check(near(both.value(), rev(0.5)), "回到端点解锁后重新进入 Reverse ⇒ 用 reverseCurve");
    check(near(flipCurveFn(makeCurveFn(Easing::Linear))(0.4), 0.4),
          "Linear 的 flipped 仍是 Linear");

    // 状态与值的转发 + 句柄式摘除。
    AnimationStatus last = AnimationStatus::Dismissed;
    int statusHits = 0;
    int valueHits = 0;
    int sh = both.addStatusListener([&](AnimationStatus s) {
        ++statusHits;
        last = s;
    });
    int vh = both.addListenerWithHandle([&] { ++valueHits; });
    check(both.status() == clock->status(), "status() 转发 parent");
    clock->setStatus(AnimationStatus::Completed);
    check(statusHits == 1 && last == AnimationStatus::Completed, "状态迁移派发给自己挂的监听者");
    clock->setValue(0.9);
    check(valueHits == 1, "parent 每帧通知转发给自己挂的监听者");
    check(both.isAnimating() == false, "isAnimating 转发 parent");
    clock->setAnimating(true);
    check(both.isAnimating(), "isAnimating 跟随 parent");

    check(clock->listenerCount() == 1 && clock->statusListenerCount() == 1,
          "parent 上恰好各挂一条监听");
    both.disposeListener();
    check(clock->listenerCount() == 0 && clock->statusListenerCount() == 0,
          "disposeListener 后 parent 干净");
    clock->setValue(0.2);
    check(valueHits == 1, "摘除后不再收到通知");
    both.removeListenerByHandle(vh);
    both.removeStatusListener(sh);

    // 便捷构造：直接给 Easing；空 parent 时 value 兜 0。
    CurvedAnimation byEasing(parent, Easing::EaseInCubic);
    check(near(byEasing.value(), 0.125), "Easing 便捷版等价（parent 停在 0.5）");
    CurvedAnimation orphan(SharedPtr<Animation<double>>{}, fwd);
    check(near(orphan.value(), 0.0) && orphan.status() == AnimationStatus::Dismissed,
          "无 parent ⇒ value=0 / status=Dismissed");
}

// ── 测试 6：ReverseAnimation（值取反 + 状态镜像）────────────────────────────
void testReverseAnimation() {
    std::printf("[test] ReverseAnimation 值取反与状态镜像\n");

    auto parent = cui::make_shared<FakeAnimation>();
    ReverseAnimation ra(parent);
    check(near(ra.value(), 1.0), "parent=0 ⇒ 反动画=1");
    check(ra.status() == AnimationStatus::Completed, "parent Dismissed ⇒ 镜像 Completed");
    parent->setValue(0.3);
    check(near(ra.value(), 0.7), "parent=0.3 ⇒ 反动画=0.7");

    int valueHits = 0;
    int vh = ra.addListenerWithHandle([&] { ++valueHits; });
    parent->setValue(0.5);
    check(valueHits == 1 && near(ra.progress(), 0.5), "值变化转发（progress 是 value 别名）");

    AnimationStatus last = AnimationStatus::Dismissed;
    int statusHits = 0;
    int sh = ra.addStatusListener([&](AnimationStatus s) {
        ++statusHits;
        last = s;
    });
    parent->setStatus(AnimationStatus::Forward);
    check(statusHits == 1 && last == AnimationStatus::Reverse, "Forward 镜像成 Reverse");
    parent->setStatus(AnimationStatus::Reverse);
    check(last == AnimationStatus::Forward, "Reverse 镜像成 Forward");
    parent->setStatus(AnimationStatus::Completed);
    check(last == AnimationStatus::Dismissed, "Completed 镜像成 Dismissed");
    check(ra.status() == AnimationStatus::Dismissed, "status() 取 parent 的镜像");

    parent->setAnimating(true);
    check(ra.isAnimating(), "isAnimating 跟随 parent");
    ra.disposeListener();
    check(parent->listenerCount() == 0 && parent->statusListenerCount() == 0,
          "disposeListener 后 parent 干净");
    ra.removeListenerByHandle(vh);
    ra.removeStatusListener(sh);

    ReverseAnimation empty(SharedPtr<Animation<double>>{});
    check(near(empty.value(), 1.0) && empty.status() == AnimationStatus::Completed,
          "无 parent ⇒ value=1 / status=Completed");
}

// ── 测试 7：ProxyAnimation（可换 parent，Hero 飞行中改挂路由动画靠它）────────
void testProxyAnimation() {
    std::printf("[test] ProxyAnimation 换 parent 语义\n");

    ProxyAnimation px;
    check(near(px.value(), 0.0) && px.status() == AnimationStatus::Dismissed,
          "空 proxy = 上游 kAlwaysDismissedAnimation");

    auto a = cui::make_shared<FakeAnimation>();
    auto b = cui::make_shared<FakeAnimation>();
    a->setValue(0.5);

    int hits = 0;
    int vh = px.addListenerWithHandle([&] { ++hits; });
    px.setParent(a);
    check(near(px.value(), 0.5) && px.parent().get() == a.get(), "挂上 a 后代理 a");
    check(a->listenerCount() == 1 && a->statusListenerCount() == 1, "a 上各挂一条");
    check(hits == 1, "从无 parent 到有 parent ⇒ 补发一次");

    a->setValue(0.6);
    check(hits == 2 && near(px.value(), 0.6), "a 每帧通知转发");

    // 换 parent：旧的一律摘干净、新的重新订阅、值不同才补发。
    b->setValue(0.25);
    px.setParent(b);
    check(hits == 3, "换 parent 且值不同 ⇒ 再补发一次");
    check(a->listenerCount() == 0 && a->statusListenerCount() == 0, "旧 parent 监听已摘除");
    check(b->listenerCount() == 1 && b->statusListenerCount() == 1, "新 parent 挂上");
    check(px.parent().get() == b.get(), "parent 指向 b");

    hits = 0;
    px.setParent(b);
    check(hits == 0 && b->listenerCount() == 1, "同一个 parent 重复 set ⇒ 不重订阅不补发");

    auto same = cui::make_shared<FakeAnimation>();
    same->setValue(0.25);
    px.setParent(same);
    check(hits == 0, "值相同 ⇒ 不补发");

    AnimationStatus last = AnimationStatus::Dismissed;
    int sh = px.addStatusListener([&](AnimationStatus s) {
        last = s;
        ++hits;
    });
    same->setStatus(AnimationStatus::Forward);
    check(last == AnimationStatus::Forward && px.status() == AnimationStatus::Forward,
          "状态迁移转发");

    // 换到 null：上游 用快照兜底，value/status 仍读得到最后一次的值。
    px.setParent(nullptr);
    check(near(px.value(), 0.25) && px.status() == AnimationStatus::Forward,
          "换到 null ⇒ 用换走那一刻的快照兜底");
    check(same->listenerCount() == 0 && same->statusListenerCount() == 0, "null 之后旧 parent 干净");
    px.setParent(nullptr);  // 幂等
    check(near(px.value(), 0.25), "重复 set(null) 不动快照");

    ProxyAnimation built(a);
    check(near(built.value(), a->value()) && a->listenerCount() == 1, "带 parent 的构造函数");
    a->setValue(0.8);
    check(near(built.value(), 0.8), "构造函数版同样订阅");
    built.disposeListener();
    check(a->listenerCount() == 0 && a->statusListenerCount() == 0, "析构前手动摘除");
    px.removeListenerByHandle(vh);
    px.removeStatusListener(sh);
}

// ── 测试 8：Tween::animate 产出的可订阅动画值 ───────────────────────────────
void testTweenAnimate() {
    std::printf("[test] Tween::animate / RectTween\n");

    auto ctrl = cui::make_shared<AnimationController>(100.0);
    Tween<double> tween(10.0, 20.0);
    SharedPtr<Animation<double>> driven = tween.animate(ctrl);
    check(near(driven->value(), 10.0), "animate：t=0 ⇒ begin");
    check(driven->status() == AnimationStatus::Dismissed, "animate：状态转发 parent");

    int hits = 0;
    int vh = driven->addListenerWithHandle([&] { ++hits; });
    ctrl->start();
    ctrl->tick(Duration(50.0));
    check(near(driven->value(), 15.0), "animate：t=0.5 ⇒ 中点");
    check(hits >= 1, "parent 每帧驱动 animate 产物");
    check(driven->isAnimating(), "isAnimating 跟随 parent");
    driven->removeListenerByHandle(vh);
    ctrl->tick(Duration(100.0));

    // 产物析构必须把挂在 controller 上的监听摘干净（句柄式摘除铁律）。
    check(ctrl->listenerCount() == 1, "controller 上只剩 animate 自己那条");
    driven.reset();
    check(ctrl->listenerCount() == 0, "animate 产物析构后 controller 干净");

    check(near(tween.flipped().begin(), 20.0) && near(tween.flipped().end(), 10.0),
          "flipped 交换端点");
    check(near(tween.evaluate(*ctrl), 20.0), "evaluate 收 Animation<double>&（播完 ⇒ end）");
    check(driven.get() == nullptr, "driven 已释放");

    // RectTween：Hero 的 createRectTween 需要它。
    RectTween rt(Rect(0, 0, 10, 10), Rect(0, 0, 30, 20));
    Rect mid = rt.lerp(0.5);
    check(near(mid.width, 20.0) && near(mid.height, 15.0), "RectTween 中点尺寸");
    check(rt.beginRect().has_value() && rt.endRect().has_value(), "RectTween 端点可选值保留");
    RectTween emptyTween(std::nullopt, std::nullopt);
    check(near(emptyTween.lerp(0.5).width, 0.0), "空端点回落 Rect{}");
}

/// 循环 / 自动反向：tick() 路径（由 Ticker 驱动）下的语义。
///
/// 此前 tick() 只把进度钳到 1.0 并不真正循环（注释自承 "let the caller
/// handle looping"），loop=true 的 demo 会卡在末帧不动。
void testLoopAndAutoReverse() {
    std::printf("[test] loop / autoReverse\n");

    // 单向循环：每 100ms 一个周期，t=150ms 时进度应为 0.5（第二圈）。
    {
        AnimationController anim(100.0);
        anim.setLoop(true);
        anim.start();
        anim.tick(Duration(50.0));
        check(near(anim.rawProgress(), 0.5, 1e-6), "loop：t=50/100 → 0.5");
        anim.tick(Duration(150.0));
        check(near(anim.rawProgress(), 0.5, 1e-6), "loop：跨过周期边界后回到 0.5");
        check(anim.isRunning(), "loop：动画持续运行");
        anim.tick(Duration(350.0));
        check(near(anim.rawProgress(), 0.5, 1e-6), "loop：多个周期后仍在循环");
    }

    // 自动反向（无 loop）：0 → 1 → 0，然后结束。
    {
        AnimationController anim(100.0);
        anim.setAutoReverse(true);
        anim.start();
        anim.tick(Duration(50.0));
        check(near(anim.rawProgress(), 0.5, 1e-6), "autoReverse 正向：t=50 → 0.5");
        anim.tick(Duration(100.0));
        check(near(anim.rawProgress(), 1.0, 1e-6), "autoReverse 转折点：→ 1.0");
        anim.tick(Duration(150.0));
        check(near(anim.rawProgress(), 0.5, 1e-6), "autoReverse 反向：→ 0.5");
        anim.tick(Duration(200.0));
        check(near(anim.rawProgress(), 0.0, 1e-6) && !anim.isRunning(),
              "autoReverse 结束：回到 0 并停止");
    }

    // loop + autoReverse：来回摆动，永不结束。
    {
        AnimationController anim(100.0);
        anim.setLoop(true);
        anim.setAutoReverse(true);
        anim.start();
        anim.tick(Duration(50.0));
        check(near(anim.rawProgress(), 0.5, 1e-6), "loop+autoReverse 正向：0.5");
        anim.tick(Duration(150.0));
        check(near(anim.rawProgress(), 0.5, 1e-6), "loop+autoReverse 反向：0.5");
        anim.tick(Duration(450.0));
        check(anim.isRunning(), "loop+autoReverse：持续摆动不停止");
    }

    // 非循环：播完即结束，进度为 1。
    {
        AnimationController anim(100.0);
        anim.start();
        anim.tick(Duration(100.0));
        check(near(anim.rawProgress(), 1.0, 1e-6) && !anim.isRunning(),
              "非循环：播完停在 1.0 并停止");
    }
}

}  // namespace

int main() {
    std::printf("=== test_animation ===\n");
    testAnimatedBuilderRebuild();
    testSharedAnimationSelfDrive();
    testTweens();
    testLiquidGlassToggle();
    testCurvedAnimation();
    testReverseAnimation();
    testProxyAnimation();
    testTweenAnimate();
    testLoopAndAutoReverse();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
