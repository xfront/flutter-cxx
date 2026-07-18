/// @file test_hero.cpp
/// @brief 上游 widgets/heroes.dart 移植（Hero / HeroController / HeroMode）的
///        单元测试。
///
/// 覆盖口径：
///   1. push 飞行：帧末起飞、两端换占位、Overlay 顶层插 shuttle 条目、
///      矩形从 fromHero 补到 toHero、完成后条目摘除且 fromHero 继续藏住
///   2. pop 飞行：起点是上层的 hero、目标恢复显示、路由 dispose 的完成回调
///      能结束飞行（hero.h 偏离 3：本仓没有 weak_ptr，靠共享 token 兜底）
///   3. HeroMode(false)：整棵子树不参与收集 ⇒ 不起飞行
///   4. placeholderBuilder：占位换成用户给的控件
///   5. createRectTween：控制器的补间回调真的接管 shuttle 矩形
///   6. divert：push 飞行途中 pop ⇒ 同一条飞行换车，不重开
///   7. 用户手势：transitionOnUserGestures 开关 + 没推进的手势抬手按
///      dismissed 收尾
///
/// 帧泵与 Application 同序：`Ticker::tickAllActive` → `flushBuildScope` →
/// `flushLayout` → `flushCompositingBits` → `finalizeTree` → 帧末回调
/// （Hero 的推迟起飞靠最后一步）。时长靠真实 sleep（本仓 Ticker 的 elapsed
/// 用 steady_clock 现算，忽略传入的帧时间）。headless，不做 flushPaint。

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/hero.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/pages.h"
#include "cui/widgets/routes.h"

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

bool nearly(double a, double b) { return std::fabs(a - b) < 0.001; }

// ── 帧泵脚手架 ─────────────────────────────────────────────────────────────

template <typename W>
W* findWidget(Element* root) {
    if (!root)
        return nullptr;
    if (auto* widget = dynamic_cast<W*>(root->widget()))
        return widget;
    W* found = nullptr;
    root->visitChildren([&](Element* child) {
        if (!found && child)
            found = findWidget<W>(child);
    });
    return found;
}

NavigatorState* findNavigatorState(Element* root) {
    if (!root)
        return nullptr;
    if (auto* stateful = dynamic_cast<StatefulElement*>(root)) {
        if (auto* state = dynamic_cast<NavigatorState*>(stateful->state()))
            return state;
    }
    NavigatorState* found = nullptr;
    root->visitChildren([&](Element* child) {
        if (!found && child)
            found = findNavigatorState(child);
    });
    return found;
}

/// 子树里第一个同 tag 的 HeroState。
HeroState* findHeroState(Element* root, const std::string& tag) {
    if (!root)
        return nullptr;
    if (auto* stateful = dynamic_cast<StatefulElement*>(root)) {
        if (auto* state = dynamic_cast<HeroState*>(stateful->state())) {
            if (state->tag() == tag)
                return state;
        }
    }
    HeroState* found = nullptr;
    root->visitChildren([&](Element* child) {
        if (!found && child)
            found = findHeroState(child, tag);
    });
    return found;
}

/// 飞行条目的 Positioned。页面本身用 Align 摆 hero（不用 Positioned），
/// 所以「带 left/top/width/height 的 Positioned」只会是 shuttle 那一个。
Positioned* findFlightPositioned(Element* root) {
    if (!root)
        return nullptr;
    if (auto* positioned = dynamic_cast<Positioned*>(root->widget())) {
        if (positioned->hasLeft && positioned->hasTop && positioned->hasWidth &&
            positioned->hasHeight)
            return positioned;
    }
    Positioned* found = nullptr;
    root->visitChildren([&](Element* child) {
        if (!found && child)
            found = findFlightPositioned(child);
    });
    return found;
}

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;  // 最后声明：元素树先于渲染树拆
    Element* root = nullptr;
    NavigatorState* nav = nullptr;

    void attach(SharedPtr<Widget> tree, double ww = 800, double hh = 600) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(tree->createElement(tree));
        root = owner.rootElement();
        if (auto* box = dynamic_cast<RenderBox*>(root->findRenderObject()))
            rv.setChild(box);
        po.setRootNode(&rv);
        frame();
        nav = findNavigatorState(root);
    }

    void frame() {
        owner.flushBuildScope();
        po.flushLayout();
        po.flushCompositingBits();
        owner.finalizeTree();
        owner.flushPostFrameCallbacks();
    }
    void tick(int sleepMs = 16) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        Ticker::tickAllActive(Duration(sleepMs));
        frame();
    }
    void ticks(int n, int sleepMs = 16) {
        for (int i = 0; i < n; ++i)
            tick(sleepMs);
    }
    bool until(const std::function<bool()>& pred, int maxTicks = 120) {
        for (int i = 0; i < maxTicks; ++i) {
            if (pred())
                return true;
            tick();
        }
        return pred();
    }
};

// ── 造页面 ─────────────────────────────────────────────────────────────────

struct HeroSpec {
    double size = 60.0;
    double ax = -1.0;  // Align 对齐：-1 = 起点，1 = 终点
    double ay = -1.0;
    HeroPlaceholderBuilder placeholder;
    bool heroModeEnabled = true;
    bool transitionOnUserGestures = false;
};

/// 一条整屏路由：内容是一块固定 800×600 的舞台，hero 按 Align 摆位。
/// hero 的 child 是 size×size 的 SizedBox ⇒ hero 的包围盒 = 对齐后的 size 方块。
SharedPtr<PageRouteBuilder> heroRoute(const std::string& tag, const HeroSpec& spec,
                                      double durationMs = 600.0) {
    auto hero = cui::make_shared<Hero>(tag, cui::make_shared<SizedBox>(spec.size, spec.size),
                                       nullptr, nullptr, spec.placeholder,
                                       spec.transitionOnUserGestures, Easing::Linear,
                                       std::nullopt, nullptr);
    SharedPtr<Widget> content = hero;
    if (!spec.heroModeEnabled)
        content = cui::make_shared<HeroMode>(content, false);
    content = cui::make_shared<Align>(spec.ax, spec.ay, content);
    content = cui::make_shared<SizedBox>(800.0, 600.0, content);

    return cui::make_shared<PageRouteBuilder>(
            [content](BuildContext&, SharedPtr<Animation<double>>,
                      SharedPtr<Animation<double>>) -> SharedPtr<Widget> { return content; },
            nullptr, durationMs);
}

size_t overlayEntryCount(Harness& h) {
    OverlayState* overlay = h.nav ? h.nav->overlayState() : nullptr;
    return overlay ? overlay->entries().size() : 0;
}

HeroState* heroInRoute(Route* route, const std::string& tag) {
    auto* modal = dynamic_cast<ModalRoute*>(route);
    BuildContext* subtree = modal ? modal->subtreeContext() : nullptr;
    return subtree && subtree->element() ? findHeroState(subtree->element(), tag) : nullptr;
}

// ── 1. push 飞行 ───────────────────────────────────────────────────────────

void testPushFlight() {
    std::printf("[test] 1. push 飞行：起飞 → 插值 → 收尾\n");

    Harness h;
    HeroController hero;
    auto navigator = cui::make_shared<Navigator>();
    navigator->setObservers({&hero});
    h.attach(navigator);
    check(h.nav != nullptr, "NavigatorState 就位");

    auto first = heroRoute("avatar", HeroSpec{60.0, -1.0, -1.0});
    h.nav->push(first);
    check(h.until([&] { return first->controller()->isCompleted(); }), "第一条入场完成");
    const size_t baseEntries = overlayEntryCount(h);
    check(baseEntries == 2, "首条路由的 Overlay 条目数 = 2（barrier + scope）");

    auto second = heroRoute("avatar", HeroSpec{200.0, 1.0, 1.0});
    h.nav->push(second);
    h.frame();  // 目标路由以 offstage 完成首帧 ⇒ 帧末起飞
    check(hero.hasFlight("avatar"), "push 飞行已创建");

    HeroState* fromHero = heroInRoute(first.get(), "avatar");
    HeroState* toHero = heroInRoute(second.get(), "avatar");
    check(fromHero != nullptr && toHero != nullptr, "两条路由各有一个 hero");

    h.frame();  // overlay 条目与两端占位落地
    check(fromHero && fromHero->placeholderVisible(), "push：fromHero 换成占位");
    check(toHero && toHero->placeholderVisible(), "push：toHero 也占位（child 不参与）");
    check(overlayEntryCount(h) == baseEntries + 2 + 1,
          "飞行条目插在 Overlay 顶层（两条路由各 2 条 + 飞行 1 条）");

    Positioned* pos = findFlightPositioned(h.root);
    check(pos != nullptr, "飞行条目已构建 Positioned");
    if (pos) {
        check(nearly(pos->left, 0.0) && nearly(pos->top, 0.0), "起飞矩形 = fromHero 位置");
        check(nearly(pos->width, 60.0) && nearly(pos->height, 60.0), "起飞尺寸 = fromHero 尺寸");
    }

    h.tick(200);
    pos = findFlightPositioned(h.root);
    check(pos && pos->left > 10.0 && pos->left < 590.0, "中段 left 落在两端之间");
    check(pos && pos->width > 61.0 && pos->width < 199.0, "中段尺寸在插值中");

    check(h.until([&] { return !hero.hasFlight("avatar"); }), "入场动画走完 ⇒ 飞行结束");
    check(h.until([&] { return overlayEntryCount(h) == baseEntries + 2; }),
          "飞行条目已摘除（只剩两条路由的条目）");
    check(toHero && !toHero->placeholderVisible(), "toHero 恢复真实 child");
    check(fromHero && fromHero->placeholderVisible(), "fromHero 继续藏住（被上层整屏路由盖住）");
    check(hero.flightCount() == 0, "飞行表清空");
}

// ── 2. pop 飞行 ────────────────────────────────────────────────────────────

void testPopFlight() {
    std::printf("[test] 2. pop 飞行：反向起落 + dispose 兜底\n");

    Harness h;
    HeroController hero;
    auto navigator = cui::make_shared<Navigator>();
    navigator->setObservers({&hero});
    h.attach(navigator);

    auto first = heroRoute("avatar", HeroSpec{60.0, -1.0, -1.0});
    auto second = heroRoute("avatar", HeroSpec{200.0, 1.0, 1.0});
    h.nav->push(first);
    check(h.until([&] { return first->controller()->isCompleted(); }), "第一条入场完成");
    h.nav->push(second);
    h.frame();
    h.frame();
    check(hero.hasFlight("avatar"), "push 飞行在飞");
    check(h.until([&] { return !hero.hasFlight("avatar"); }), "push 飞行结束");
    check(h.until([&] { return second->controller()->isCompleted(); }), "第二条入场完成");

    HeroState* firstHero = heroInRoute(first.get(), "avatar");
    check(firstHero && firstHero->placeholderVisible(), "push 完成后 fromHero 仍藏住");

    const size_t baseEntries = overlayEntryCount(h);

    h.nav->pop();
    h.frame();  // 历史更新（didChangeTop）+ 帧末起飞
    check(hero.hasFlight("avatar"), "pop 飞行已创建");
    h.frame();

    Positioned* pos = findFlightPositioned(h.root);
    check(pos != nullptr, "pop 飞行条目已构建");
    if (pos) {
        check(nearly(pos->left, 600.0) && nearly(pos->top, 400.0), "pop 起点 = 上层 hero 的位置");
        check(nearly(pos->width, 200.0) && nearly(pos->height, 200.0), "pop 起点尺寸");
    }
    HeroState* toHero = heroInRoute(first.get(), "avatar");
    check(toHero && toHero->placeholderVisible(), "pop：目标 hero 被占位");

    check(h.until([&] { return second->routeDisposed(); }), "退场动画走完并 dispose");
    h.frame();
    check(!hero.hasFlight("avatar"), "路由 dispose 的完成回调结束飞行（偏离 3）");
    check(overlayEntryCount(h) == baseEntries - 2, "第二条路由与飞行条目均已摘除");
    check(toHero && !toHero->placeholderVisible(), "目标 hero 恢复显示");
    check(first->controller()->value() == 1.0, "首条路由仍在场且已完成");
}

// ── 3. HeroMode(false) ────────────────────────────────────────────────────

void testHeroModeDisabled() {
    std::printf("[test] 3. HeroMode(false)：子树不参与\n");

    Harness h;
    HeroController hero;
    auto navigator = cui::make_shared<Navigator>();
    navigator->setObservers({&hero});
    h.attach(navigator);

    auto first = heroRoute("avatar", HeroSpec{60.0, -1.0, -1.0});
    h.nav->push(first);
    check(h.until([&] { return first->controller()->isCompleted(); }), "第一条入场完成");
    const size_t baseEntries = overlayEntryCount(h);

    HeroSpec spec{200.0, 1.0, 1.0};
    spec.heroModeEnabled = false;
    auto second = heroRoute("avatar", spec);
    h.nav->push(second);
    h.frame();
    h.frame();
    check(!hero.hasFlight("avatar"), "HeroMode(false) 的 hero 不参与飞行");
    check(hero.flightCount() == 0, "飞行表为空");
    check(overlayEntryCount(h) == baseEntries + 2, "只有第二条路由自己的两条条目");
    check(h.until([&] { return second->controller()->isCompleted(); }), "路由本身照常入场");

    HeroState* firstHero = heroInRoute(first.get(), "avatar");
    check(firstHero && !firstHero->placeholderVisible(), "没有飞行 ⇒ 首条 hero 保持可见");
}

// ── 4. placeholderBuilder ─────────────────────────────────────────────────

class PlaceholderMarker : public StatelessWidget {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<SizedBox>(5, 5);
    }
};

void testPlaceholderBuilder() {
    std::printf("[test] 4. placeholderBuilder：占位换成用户控件\n");

    Harness h;
    HeroController hero;
    auto navigator = cui::make_shared<Navigator>();
    navigator->setObservers({&hero});
    h.attach(navigator);

    HeroSpec firstSpec{60.0, -1.0, -1.0};
    firstSpec.placeholder = [](BuildContext&, Size, SharedPtr<Widget>) -> SharedPtr<Widget> {
        return cui::make_shared<PlaceholderMarker>();
    };
    auto first = heroRoute("avatar", firstSpec);
    h.nav->push(first);
    check(h.until([&] { return first->controller()->isCompleted(); }), "第一条入场完成");
    check(findWidget<PlaceholderMarker>(h.root) == nullptr, "不在飞行中 ⇒ 没有占位标记");

    auto second = heroRoute("avatar", HeroSpec{200.0, 1.0, 1.0});
    h.nav->push(second);
    h.frame();
    h.frame();
    check(hero.hasFlight("avatar"), "飞行已创建");
    check(findWidget<PlaceholderMarker>(h.root) != nullptr,
          "fromHero 的占位来自 placeholderBuilder");
}

// ── 5. createRectTween ────────────────────────────────────────────────────

void testCreateRectTween() {
    std::printf("[test] 5. createRectTween：控制器的补间接管 shuttle\n");

    Harness h;
    std::vector<std::pair<std::optional<Rect>, std::optional<Rect>>> calls;
    HeroController hero([&calls](std::optional<Rect> begin,
                                 std::optional<Rect> end) -> SharedPtr<Tween<Rect>> {
        calls.emplace_back(begin, end);
        // 恒为一块 50×50 的固定框：默认的矩形插值绝不会是这个值。
        return cui::make_shared<RectTween>(std::optional<Rect>(Rect(10, 20, 50, 50)),
                                           std::optional<Rect>(Rect(10, 20, 50, 50)));
    });
    auto navigator = cui::make_shared<Navigator>();
    navigator->setObservers({&hero});
    h.attach(navigator);

    auto first = heroRoute("avatar", HeroSpec{60.0, -1.0, -1.0});
    h.nav->push(first);
    check(h.until([&] { return first->controller()->isCompleted(); }), "第一条入场完成");

    auto second = heroRoute("avatar", HeroSpec{200.0, 1.0, 1.0});
    h.nav->push(second);
    h.frame();
    h.frame();
    check(!calls.empty(), "createRectTween 被调用");
    if (!calls.empty()) {
        const auto& begin = calls.front().first;
        const auto& end = calls.front().second;
        check(begin && end, "两端矩形都给了");
        if (begin && end) {
            check(nearly(begin->x, 0.0) && nearly(begin->y, 0.0), "起端 = fromHero 位置");
            check(nearly(begin->width, 60.0), "起端尺寸");
            check(nearly(end->x, 600.0) && nearly(end->y, 400.0), "终端 = toHero 位置");
            check(nearly(end->width, 200.0), "终端尺寸");
        }
    }

    Positioned* pos = findFlightPositioned(h.root);
    check(pos && nearly(pos->left, 10.0) && nearly(pos->top, 20.0), "shuttle 用自定义补间的位置");
    check(pos && nearly(pos->width, 50.0) && nearly(pos->height, 50.0), "shuttle 用自定义补间的尺寸");

    h.tick(200);
    pos = findFlightPositioned(h.root);
    check(pos && nearly(pos->left, 10.0) && nearly(pos->width, 50.0),
          "中段仍是自定义补间（默认插值不可能恒定）");
}

// ── 6. divert ─────────────────────────────────────────────────────────────

void testDivert() {
    std::printf("[test] 6. divert：push 飞行被 pop 打断\n");

    Harness h;
    HeroController hero;
    auto navigator = cui::make_shared<Navigator>();
    navigator->setObservers({&hero});
    h.attach(navigator);

    auto first = heroRoute("avatar", HeroSpec{60.0, -1.0, -1.0});
    h.nav->push(first);
    check(h.until([&] { return first->controller()->isCompleted(); }), "第一条入场完成");

    auto second = heroRoute("avatar", HeroSpec{200.0, 1.0, 1.0});
    h.nav->push(second);
    h.frame();
    h.frame();
    check(hero.hasFlight("avatar"), "push 飞行在飞");

    // 让 push 动画离开 0（`_maybeStartHeroTransition` 在 pop 且动画值为 0
    // 时会直接早退，不停一拍就测不到 divert）。
    h.tick(120);
    h.nav->pop();  // 飞行途中弹栈
    h.frame();
    check(hero.hasFlight("avatar"), "飞行被换车而不是重开");
    check(hero.flightCount() == 1, "同一 tag 只有一条飞行");

    check(h.until([&] { return second->routeDisposed(); }), "中断后第二条仍能退场并 dispose");
    h.frame();
    check(hero.hasFlight("avatar") == false, "换车成 pop 的飞行也随 dispose 收尾");

    HeroState* firstHero = heroInRoute(first.get(), "avatar");
    check(firstHero && !firstHero->placeholderVisible(), "首条 hero 恢复显示");
    check(overlayEntryCount(h) == 2, "只剩首条路由的两条条目");
}

// ── 7. 用户手势 ────────────────────────────────────────────────────────────

void testUserGesture() {
    std::printf("[test] 7. 用户手势：transitionOnUserGestures 开关\n");

    // (a) 未声明 transitionOnUserGestures ⇒ 手势不参与。
    {
        Harness h;
        HeroController hero;
        auto navigator = cui::make_shared<Navigator>();
        navigator->setObservers({&hero});
        h.attach(navigator);

        auto first = heroRoute("avatar", HeroSpec{60.0, -1.0, -1.0});
        auto second = heroRoute("avatar", HeroSpec{200.0, 1.0, 1.0});
        h.nav->push(first);
        h.nav->push(second);
        check(h.until([&] { return second->controller()->isCompleted(); }), "两条入场完成");

        h.nav->didStartUserGesture();
        check(!hero.hasFlight("avatar"), "未声明的手势不收集 hero");
        h.nav->didStopUserGesture();
        h.frame();
        check(hero.flightCount() == 0, "手势结束后仍无飞行");
    }

    // (b) 声明了 ⇒ 手势立即起飞；没推进就抬手 ⇒ 按 dismissed 收尾。
    {
        Harness h;
        HeroController hero;
        auto navigator = cui::make_shared<Navigator>();
        navigator->setObservers({&hero});
        h.attach(navigator);

        HeroSpec firstSpec{60.0, -1.0, -1.0};
        firstSpec.transitionOnUserGestures = true;
        HeroSpec secondSpec{200.0, 1.0, 1.0};
        secondSpec.transitionOnUserGestures = true;
        auto first = heroRoute("avatar", firstSpec);
        auto second = heroRoute("avatar", secondSpec);
        h.nav->push(first);
        h.nav->push(second);
        check(h.until([&] { return second->controller()->isCompleted(); }), "两条入场完成");
        const size_t baseEntries = overlayEntryCount(h);

        h.nav->didStartUserGesture();
        check(hero.hasFlight("avatar"), "手势同步起飞（目标页 maintainState 且已布局）");
        h.frame();
        check(overlayEntryCount(h) == baseEntries + 1, "手势飞行同样插 Overlay 条目");

        HeroState* firstHero = heroInRoute(first.get(), "avatar");
        check(firstHero && firstHero->placeholderVisible(), "手势期间目标 hero 占位");

        h.nav->didStopUserGesture();
        check(!hero.hasFlight("avatar"), "没推进的手势抬手即按 dismissed 收尾");
        h.frame();
        check(firstHero && firstHero->placeholderVisible(),
              "dismissed ⇒ 目标 hero 继续藏住（等真正的 pop 结果）");
        HeroState* secondHero = heroInRoute(second.get(), "avatar");
        check(secondHero && !secondHero->placeholderVisible(), "源 hero 恢复显示");
    }
}

}  // namespace

int main() {
    testPushFlight();
    testPopFlight();
    testHeroModeDisabled();
    testPlaceholderBuilder();
    testCreateRectTween();
    testDivert();
    testUserGesture();

    std::printf("\n%s：失败 %d 项\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
