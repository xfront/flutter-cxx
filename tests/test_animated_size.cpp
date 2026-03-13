/// @file test_animated_size.cpp
/// @brief AnimatedSize / RenderAnimatedSize 的上游算法对齐回归。
///
/// 对应 上游 rendering/animated_size.dart + widgets/animated_size.dart。钉住：
///   1. 四态状态机：Start → Stable →（子尺寸跳变）Changed →（同一段补间没跑完
///      又跳变）Unstable →（稳定一帧）Stable；以及补间端点 begin/end 的取值
///      （begin 用「自身当前尺寸」而不是子尺寸，正是动画视觉上接续的关键）。
///   2. 短路分支：紧约束或没有 RenderBox 子节点 ⇒ 停动画、尺寸恒为
///      smallest()、状态回 Start（子节点再怎么变都不补间）。
///   3. 重排必带重绘：换尺寸那一帧必须有 damage。
///   4. 只有「补间终点比自身大」（= 真有溢出）才裁剪：HardEdge 下溢出像素画不
///      出来，Clip.none 下画得出来。
///   5. alignChild 偏移公式与上游 RenderAligningShiftedBox 一致。
///   6. curve 由控件层把 Easing 包成函数下发；同一帧内 size 必须等于
///      lerp(begin, end, applyEasing(value))。
///   7. onEnd 只在动画到界时发一次。
///   8. setter 契约：等值不脏、变更必脏；等参数重建无脏区。
///   9. detach 停表；attach 仅在 changed/unstable 补标一次布局脏以续跑
///      （上游 attach 的 switch 就只管这两态，stable 中途不自动续）。
///
/// 结构变更（AnimatedSize 换到不同的父槽）会换掉 Element 连带换掉渲染对象，
/// 所以每个用例都在结构变更后重新 `animOf(h)` 取指针，不留悬空引用。

#include <chrono>
#include <cmath>
#include <cstdint>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/rendering/animated_size.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/animated_size.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"

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

constexpr SkColor kRed = 0xFFFF0000;
constexpr SkColor kWhite = 0xFFFFFFFF;
constexpr double kDur = 200.0;

bool nearEq(double a, double b, double eps = 0.6) { return std::abs(a - b) <= eps; }

SharedPtr<Widget> redBox(double side) {
    BoxDecoration deco;
    deco.color = Color{1, 0, 0, 1};
    return cui::make_shared<SizedBox>(side, side, cui::make_shared<DecoratedBox>(deco));
}

// ── 被测控件 ─────────────────────────────────────────────────────────────────

/// 一棵「AnimatedSize 包一个可换尺寸的红方块」的树。根是 Align(-1,-1)：把
/// RenderView 的紧约束变成松约束（紧约束下 AnimatedSize 会走短路分支）。
class SizerState;

class Sizer : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;

private:
    friend class SizerState;
};

class SizerState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        SharedPtr<Widget> inner = cui::make_shared<AnimatedSize>(
            childless_ ? nullptr : redBox(side_), alignment_, durationMs_, curve_, clip_,
            [this] { ++endCount_; });
        if (tightWrap_)
            inner = cui::make_shared<SizedBox>(120, 80, std::move(inner));
        return cui::make_shared<Align>(-1.0, -1.0, std::move(inner));
    }

    /// 换子节点尺寸 —— 上游 AnimatedSize 的用法就是「子树自然尺寸变了」。
    void setSide(double v) {
        if (side_ == v)
            return;
        side_ = v;
        setState();
    }
    void setClip(Clip c) {
        if (c == clip_)
            return;
        clip_ = c;
        setState();
    }
    void setAlignment(Alignment a) {
        if (a == alignment_)
            return;
        alignment_ = a;
        setState();
    }
    void setCurve(Easing e) {
        if (e == curve_)
            return;
        curve_ = e;
        setState();
    }
    void setTightWrap(bool v) {
        if (tightWrap_ == v)
            return;
        tightWrap_ = v;
        setState();
    }
    void setChildless(bool v) {
        if (childless_ == v)
            return;
        childless_ = v;
        setState();
    }

    int endCount() const { return endCount_; }
    /// 参数一个都不改，只是重跑 build + updateRenderObject。
    void rebuildSame() { setState(); }

private:
    double side_ = 50.0;
    double durationMs_ = kDur;
    Alignment alignment_ = Alignment::topLeft();
    Easing curve_ = Easing::Linear;
    Clip clip_ = Clip::HardEdge;
    bool tightWrap_ = false;
    bool childless_ = false;
    int endCount_ = 0;
};

inline std::unique_ptr<State> Sizer::createState() { return std::make_unique<SizerState>(); }

// ── 宿主 ─────────────────────────────────────────────────────────────────────

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    SchedulerBinding sb;
    Element* root = nullptr;
    sk_sp<SkSurface> surface;

    void attach(SharedPtr<Widget> w, double ww = 400, double hh = 400) {
        rv.setWindowSize({ww, hh});
        owner.setRootRenderObjectHost(&rv);
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        po.setRootNode(&rv);
        sb.setBuildOwner(&owner);
        sb.setPipelineOwner(&po);
        sb.setRenderView(&rv);
        surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul((int)ww, (int)hh));
        surface->getCanvas()->clear(kWhite);
        frame();
    }

    SchedulerBinding::FrameDamage frame() {
        SchedulerBinding::FrameDamage d = sb.runPipelineAndComputeDamage();
        sb.compositeTo(surface->getCanvas());
        return d;
    }

    /// 推进真实时间并让所有活动 Ticker 走一步（等价 App 的 handleBeginFrame）。
    void advance(double ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds((long)ms));
        Ticker::tickAllActive(Duration(0));
    }

    SkColor at(int x, int y) const {
        uint32_t px = 0;
        surface->readPixels(SkImageInfo::MakeN32Premul(1, 1), &px, 4, x, y);
        return px;
    }

    SizerState* state() {
        return dynamic_cast<SizerState*>(static_cast<StatefulElement*>(root)->state());
    }
};

/// 沿唯一的单子链找 RenderAnimatedSize（外面可能还套了 Align / SizedBox）。
RenderAnimatedSize* findAnim(RenderObject* ro) {
    while (ro) {
        if (auto* a = dynamic_cast<RenderAnimatedSize*>(ro))
            return a;
        auto* p = dynamic_cast<RenderProxyBox*>(ro);
        ro = p ? p->child() : nullptr;
    }
    return nullptr;
}

RenderAnimatedSize* animOf(Harness& h) { return findAnim(h.rv.child()); }

// ── 用例 ─────────────────────────────────────────────────────────────────────

/// 1 + 3 + 7：一次尺寸跳变的完整生命周期。
void testStateMachineAndTween() {
    std::printf("--- 四态状态机与补间端点 ---\n");
    Harness h;
    h.attach(cui::make_shared<Sizer>());

    auto* anim = animOf(h);
    check(anim != nullptr, "树里有 RenderAnimatedSize");
    if (!anim)
        return;

    check(anim->state() == RenderAnimatedSizeState::Stable, "首帧后状态是 Stable（Start→Stable）");
    check(anim->size() == Size(50, 50), "首帧自身尺寸 = 子尺寸");
    check(anim->sizeTweenBegin().has_value() && *anim->sizeTweenBegin() == Size(50, 50),
          "begin 落在子尺寸");
    check(anim->sizeTweenEnd().has_value() && *anim->sizeTweenEnd() == Size(50, 50),
          "end 落在子尺寸");
    check(!anim->hasVisualOverflow(), "尺寸贴合时无溢出");
    check(h.at(25, 25) == kRed, "首帧红方块画出来了");

    // 子尺寸 50 → 100：begin 取「自身当前尺寸」，不是子尺寸。
    h.state()->setSide(100);
    SchedulerBinding::FrameDamage d = h.frame();
    check(d.any(), "换尺寸那一帧有脏区（重排必带重绘）");
    check(anim->state() == RenderAnimatedSizeState::Changed, "跳变后进入 Changed");
    check(anim->isAnimating(), "跳变后动画在跑");
    check(anim->sizeTweenBegin().has_value() && *anim->sizeTweenBegin() == Size(50, 50),
          "begin = 上一帧自身尺寸");
    check(anim->sizeTweenEnd().has_value() && *anim->sizeTweenEnd() == Size(100, 100),
          "end = 新的子尺寸");
    check(anim->animationValue() == 0.0, "restartAnimation 把进度归 0");
    check(anim->size() == Size(50, 50), "进度 0 ⇒ 自身仍是旧尺寸");
    check(anim->hasVisualOverflow(), "子(100) 比自身(50) 大 ⇒ 判定溢出");

    // 中段：线性曲线 ⇒ size = lerp(begin, end, value)。
    h.advance(60);
    h.frame();
    check(anim->animationValue() > 0.0 && anim->animationValue() < 1.0, "进度推进到中段");
    check(anim->size().width > 50.5 && anim->size().width < 99.5, "自身尺寸补在中段");
    check(nearEq(anim->size().width, 50.0 + 50.0 * anim->animationValue()),
          "size = lerp(begin, end, value)（默认线性）");

    // 到界：停表 + 回 Stable + onEnd 恰好一次。
    h.advance(kDur);
    h.frame();
    check(!anim->isAnimating(), "动画跑完即停");
    check(anim->state() == RenderAnimatedSizeState::Stable, "跑完回到 Stable");
    check(anim->size() == Size(100, 100), "收尾精确落在子尺寸");
    check(!anim->hasVisualOverflow(), "贴合后无溢出");
    check(nearEq(anim->animationValue(), 1.0), "进度落在 upperBound");
    check(h.state()->endCount() == 1, "onEnd 只发一次");
    check(h.at(75, 75) == kRed, "放大后新区域被画上");
}

/// 1（续）：一段补间没跑完就再跳变 ⇒ Unstable（不再补间，直接跟随子节点）。
void testUnstableFollowsChild() {
    std::printf("--- 连续跳变进入 Unstable，稳定一帧后回 Stable ---\n");
    Harness h;
    h.attach(cui::make_shared<Sizer>());
    auto* anim = animOf(h);
    check(anim != nullptr, "拿到 RenderAnimatedSize");
    if (!anim)
        return;

    h.state()->setSide(100);
    h.frame();
    check(anim->state() == RenderAnimatedSizeState::Changed, "第一次跳变 → Changed");

    // Changed 还没被下一次布局「验收」，子尺寸又变了 ⇒ 判定不稳定。
    h.state()->setSide(150);
    h.frame();
    check(anim->state() == RenderAnimatedSizeState::Unstable, "第二次跳变 → Unstable");
    check(anim->sizeTweenBegin().has_value() && *anim->sizeTweenBegin() == Size(150, 150),
          "Unstable 时 begin 直接对齐子尺寸（不再补间）");
    check(anim->size() == Size(150, 150), "Unstable 下自身尺寸立刻等于子尺寸");

    // 这一帧子尺寸没再变 ⇒ 停表回 Stable。
    h.advance(40);
    h.frame();
    check(anim->state() == RenderAnimatedSizeState::Stable, "稳定一帧后回 Stable");

    h.advance(kDur);
    h.frame();
    check(anim->size() == Size(150, 150), "最终尺寸 = 子尺寸");
    check(anim->isAnimating() == false || anim->animationValue() >= 1.0,
          "回到 Stable 后要么已停要么已跑完");
}

/// 2a：紧约束短路。
void testTightConstraintsShortCircuit() {
    std::printf("--- 紧约束走短路分支 ---\n");
    Harness h;
    h.attach(cui::make_shared<Sizer>());
    h.state()->setTightWrap(true);
    h.frame();
    // 结构变了（AnimatedSize 从 Align 的槽移到 SizedBox 的槽）⇒ 重新取指针。
    auto* anim = animOf(h);
    check(anim != nullptr, "紧约束树里拿到 RenderAnimatedSize");
    if (!anim)
        return;

    check(anim->state() == RenderAnimatedSizeState::Start, "紧约束下状态停在 Start");
    check(anim->size() == Size(120, 80), "紧约束下尺寸 = smallest() = 父给的定值");
    check(!anim->isAnimating(), "紧约束下没有动画");
    check(anim->sizeTweenEnd().has_value() && *anim->sizeTweenEnd() == Size(120, 80),
          "tween 端点被压成 smallest()");

    h.state()->setSide(200);
    h.frame();
    check(anim->state() == RenderAnimatedSizeState::Start, "紧约束下换尺寸仍停在 Start");
    check(!anim->isAnimating(), "紧约束下换尺寸也不起动画");
    check(anim->size() == Size(120, 80), "自身尺寸仍被紧约束钉住");
    auto* boxChild = dynamic_cast<RenderBox*>(anim->child());
    check(boxChild && boxChild->size() == Size(120, 80),
          "子节点收到的是同一套紧约束 ⇒ SizedBox 的 200 被顶掉（上游同行为）");
}

/// 2b：没有 RenderBox 子节点短路。
void testNoChildShortCircuit() {
    std::printf("--- 无子节点走短路分支 ---\n");
    Harness h;
    h.attach(cui::make_shared<Sizer>());
    h.state()->setChildless(true);
    h.frame();
    auto* anim = animOf(h);
    check(anim != nullptr, "拿到 RenderAnimatedSize");
    if (!anim)
        return;

    check(anim->state() == RenderAnimatedSizeState::Start, "无子节点时状态停在 Start");
    check(anim->child() == nullptr, "确实没有子节点");
    check(anim->size() == Size(0, 0), "无子节点时尺寸 = smallest()（松约束下为 0）");
    check(!anim->isAnimating(), "无子节点没有动画");
    check(!anim->hasVisualOverflow(), "无子节点没有溢出");
}

/// 4：只有真有溢出才裁剪。
void testClipOnVisualOverflow() {
    std::printf("--- 溢出裁剪：HardEdge 裁掉、Clip.none 放行 ---\n");
    Harness h;
    h.attach(cui::make_shared<Sizer>());
    auto* anim = animOf(h);
    check(anim != nullptr, "拿到 RenderAnimatedSize");
    if (!anim)
        return;

    // 停在补间起点：自身 50×50，子 100×100（左上角对齐 ⇒ 向右下溢出）。
    h.state()->setSide(100);
    h.frame();
    check(anim->hasVisualOverflow(), "本帧确有溢出");
    check(anim->size() == Size(50, 50), "本帧自身仍是 50×50");
    check(h.at(25, 25) == kRed, "裁剪框内可见");
    check(h.at(75, 75) == kWhite, "HardEdge：溢出部分被裁掉");

    h.state()->setClip(Clip::None);
    h.frame();
    check(anim->hasVisualOverflow(), "溢出判定与裁剪参数无关");
    check(h.at(25, 25) == kRed, "Clip.none 下框内照旧");
    check(h.at(75, 75) == kRed, "Clip.none：溢出部分照画");

    // 跑完动画：贴合 ⇒ 不再进裁剪分支。
    h.advance(kDur);
    h.frame();
    check(!anim->hasVisualOverflow(), "跑完后贴合，无溢出");
    h.state()->setClip(Clip::HardEdge);
    h.state()->setSide(40);
    h.frame();
    check(!anim->hasVisualOverflow(), "子(40) 比自身(100) 小 ⇒ 无溢出，不进取裁剪分支");
    check(h.at(15, 15) == kRed, "无溢出时子区域内正常绘制（裁剪分支没参与）");
}

/// 5：alignChild 偏移公式。
void testAlignmentOffset() {
    std::printf("--- alignChild 偏移与上游 RenderAligningShiftedBox 一致 ---\n");
    Harness h;
    h.attach(cui::make_shared<Sizer>());
    auto* anim = animOf(h);
    check(anim != nullptr, "拿到 RenderAnimatedSize");
    if (!anim)
        return;

    // 溢出 + bottomRight ⇒ 子的右下与自身右下齐平：offset = size - child.size。
    h.state()->setAlignment(Alignment::bottomRight());
    h.state()->setSide(100);
    h.frame();
    auto* boxChild = dynamic_cast<RenderBox*>(anim->child());
    check(boxChild != nullptr, "子节点是 RenderBox");
    if (!boxChild)
        return;
    const Offset off = boxChild->parentData()->offset;
    check(nearEq(off.dx, -50.0) && nearEq(off.dy, -50.0),
          "bottomRight：offset = 自身(50) - 子(100) = (-50,-50)");

    h.state()->setAlignment(Alignment::topLeft());
    h.frame();
    const Offset tl = boxChild->parentData()->offset;
    check(nearEq(tl.dx, 0.0) && nearEq(tl.dy, 0.0), "topLeft：offset = (0,0)（溢出朝右下）");
}

/// 6：Easing 由控件层包成函数下发。
void testCurve() {
    std::printf("--- 曲线：Easing → CurveFn ---\n");
    Harness h;
    h.attach(cui::make_shared<Sizer>());
    auto* anim = animOf(h);
    check(anim != nullptr, "拿到 RenderAnimatedSize");
    if (!anim)
        return;

    h.state()->setCurve(Easing::EaseOutCubic);
    h.frame();
    h.state()->setSide(100);
    h.frame();
    check(anim->state() == RenderAnimatedSizeState::Changed, "换曲线后仍走 Changed");
    h.advance(60);
    h.frame();
    double v = anim->animationValue();
    check(v > 0.0 && v < 1.0, "进度在中段");
    double expected = 50.0 + 50.0 * applyEasing(v, Easing::EaseOutCubic);
    check(nearEq(anim->size().width, expected), "size = lerp(begin, end, applyEasing(value))");
    check(anim->size().width > 50.0 + 50.0 * v + 0.5, "EaseOutCubic 前段快过线性");
}

/// 8：setter 契约（等值不脏、变更必脏）。
void testSetterContract() {
    std::printf("--- RenderAnimatedSize setter 契约 ---\n");
    Harness h;
    h.attach(cui::make_shared<Sizer>());
    auto* anim = animOf(h);
    check(anim != nullptr, "拿到 RenderAnimatedSize");
    if (!anim)
        return;

    check(!anim->needsLayout() && !anim->needsPaint(), "布局/绘制完成后自身是干净的");

    anim->setAlignment(anim->alignment());
    anim->setClipBehavior(anim->clipBehavior());
    anim->setDuration(kDur);
    check(!anim->needsLayout(), "等值 setAlignment 不标脏布局");
    check(!anim->needsPaint(), "等值 setClipBehavior 不标脏绘制");

    anim->setAlignment(Alignment::bottomCenter());
    check(anim->needsLayout(), "改 setAlignment 必须标脏布局");
    check(h.rv.needsLayout(), "布局脏要冒到根边界");
    h.frame();
    check(!anim->needsPaint(), "重绘完成后自身是干净的");

    anim->setClipBehavior(Clip::AntiAlias);
    check(anim->needsPaint(), "改 setClipBehavior 必须标脏绘制");
    check(h.rv.needsPaint(), "绘制脏要冒到根边界");
    SchedulerBinding::FrameDamage d = h.frame();
    check(d.any(), "换裁剪那一帧有脏区");
}

/// 8（续）：等参数重建不产生脏区。
void testEqualRebuildIsIdle() {
    std::printf("--- 等参数重建无脏区 ---\n");
    Harness h;
    h.attach(cui::make_shared<Sizer>());
    check(animOf(h) != nullptr, "拿到 RenderAnimatedSize");
    h.state()->rebuildSame();
    SchedulerBinding::FrameDamage idle = h.frame();
    check(!idle.any(), "参数没变的 setState 不该产生脏区（updateRenderObject 全走守卫 setter）");
}

/// 9：detach 停表；attach 只在 changed/unstable 续跑（上游的 attach switch）。
void testDetachResume() {
    std::printf("--- detach 停表 / attach 续跑 ---\n");

    // 9a changed 态：滚出视口再回来，动画从冻结进度接着跑。
    Harness h;
    h.attach(cui::make_shared<Sizer>());
    auto* anim = animOf(h);
    check(anim != nullptr, "拿到 RenderAnimatedSize");
    if (!anim)
        return;

    h.state()->setSide(100);
    h.frame();
    check(anim->isAnimating() && anim->state() == RenderAnimatedSizeState::Changed,
          "换尺寸那一帧进入 Changed 且在补间");
    const Size begin = *anim->sizeTweenBegin();
    const Size end = *anim->sizeTweenEnd();

    // 整棵树摘掉（等价上游滚出视口）：动画必须停。
    h.po.clearRootNode(&h.rv);
    check(!anim->attached(), "渲染对象已脱离 owner");
    check(!anim->isAnimating(), "detach 把 Ticker 摘了");

    h.advance(60);
    check(anim->animationValue() == 0.0, "脱离期间进度不推进");

    h.po.setRootNode(&h.rv);
    check(anim->attached(), "重新接回 owner");
    check(anim->needsLayout(), "attach 时 Changed 态补标一次布局脏");
    h.frame();
    check(anim->isAnimating(), "接回后在布局里把动画续上");
    check(*anim->sizeTweenBegin() == begin && *anim->sizeTweenEnd() == end,
          "续跑而不是重来：补间端点未变");

    h.advance(kDur);
    h.frame();
    check(!anim->isAnimating() && anim->size() == Size(100, 100), "续跑后正常收尾");

    // 9b stable 态：上游的 attach 只处理 changed/unstable，所以「动画还在跑、
    // 状态已回 Stable」时接回不会自动续跑，要等下一次布局脏。钉住这个差异。
    Harness h2;
    h2.attach(cui::make_shared<Sizer>());
    auto* anim2 = animOf(h2);
    check(anim2 != nullptr, "第二棵树拿到 RenderAnimatedSize");
    if (!anim2)
        return;

    h2.state()->setSide(100);
    h2.frame();
    h2.advance(60);
    h2.frame();  // layoutChanged：子尺寸没再变 ⇒ 回 Stable，动画继续跑
    check(anim2->state() == RenderAnimatedSizeState::Stable && anim2->isAnimating() &&
              anim2->animationValue() > 0.0 && anim2->animationValue() < 1.0,
          "补间途中状态已回 Stable（Changed 只活一次布局）");
    const double frozen = anim2->animationValue();

    h2.po.clearRootNode(&h2.rv);
    h2.advance(60);
    h2.po.setRootNode(&h2.rv);
    check(!anim2->needsLayout(), "Stable 态 attach 不补标布局脏（与上游 attach 的 switch 一致）");
    check(nearEq(anim2->animationValue(), frozen), "脱离期间进度冻结");

    // 直到下一次真正的脏（这里用换尺寸）才继续跑完。
    h2.state()->setSide(120);
    h2.frame();
    check(anim2->isAnimating(), "换尺寸后动画重新在跑");
    h2.advance(kDur);
    h2.frame();
    check(anim2->size() == Size(120, 120), "最终落在新的子尺寸");
}

}  // namespace

int main() {
    std::printf("=== AnimatedSize / RenderAnimatedSize 对齐测试 ===\n");
    testStateMachineAndTween();
    testUnstableFollowsChild();
    testTightConstraintsShortCircuit();
    testNoChildShortCircuit();
    testClipOnVisualOverflow();
    testAlignmentOffset();
    testCurve();
    testSetterContract();
    testEqualRebuildIsIdle();
    testDetachResume();
    std::printf("%s: %d 处失败\n", g_failures ? "FAIL" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
