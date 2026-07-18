/// @file test_routes.cpp
/// @brief 上游路由转场层（OverlayRoute / TransitionRoute / ModalRoute /
///        LocalHistoryEntry / PageRoute / PopupRoute）的单元测试。
///
/// 覆盖口径：
///   1. 入场转场：controller 走路由自己的帧时间线、条目停在 pushing、
///      入场完成后 barrier 条目转 opaque 并落位 idle
///   2. secondaryAnimation：上一条跟着下一条的动画走；非 PageRoute 不联动
///   3. 本地历史：pop 就地消化、canPop / popDisposition / willHandlePopInternally
///   4. ModalRoute 的静态查询（maybeOf / …Of / withName）
///   5. 退场转场：popping 期间已不在场、动画到下界才 finalize、
///      whenTransitionCompleted 拿到结果、Overlay 条目摘净
///   6. TickerMode：静音时不打 tick（routes.h 偏离 11），解除后接着走完
///   7. offstage：对外动画代理成常量（Hero 的前置条件）
///   8. PopupRoute / RawDialogRoute：opaque=false、maintainState=true、
///      barrier 用 AnimatedModalBarrier
///
/// 帧泵与 Application 同序：`Ticker::tickAllActive` → `flushBuildScope` →
/// `flushPostFrameCallbacks`（路由的延迟释放靠第三步，见 routes.h 偏离 16）。
/// headless，不做 flushPaint。时长靠真实 sleep（本仓 Ticker 的 elapsed 用
/// steady_clock 现算，忽略传入的帧时间）。

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/modal_barrier.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/pages.h"
#include "cui/widgets/routes.h"
#include "cui/widgets/ticker_mode.h"

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

/// 页内容的探针：记 buildPage 次数与它拿到的 BuildContext
/// （Element 就是 BuildContext，路由存活期间可用于 ModalRoute 的静态查询）。
struct PageProbe {
    int buildCalls = 0;
    BuildContext* context = nullptr;
};

// ── 帧泵脚手架 ─────────────────────────────────────────────────────────────

/// 先序找子树里第一个 T 类型 Widget。
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

struct Harness {
    BuildOwner owner;
    Element* root = nullptr;
    NavigatorState* state = nullptr;

    explicit Harness(SharedPtr<Widget> tree) { attach(std::move(tree)); }

    Harness() = default;

    void attach(SharedPtr<Widget> tree) {
        auto element = tree->createElement(tree);
        root = element.get();
        owner.adoptRootElement(std::move(element));
        pump();
        state = findNavigatorState(root);
    }

    /// 一帧的「非 tick」部分（与 Application 同序）。
    void pump() {
        owner.flushBuildScope();
        owner.flushPostFrameCallbacks();
    }
    void tick(int sleepMs = 16) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        Ticker::tickAllActive(Duration(sleepMs));
        pump();
    }
    void ticks(int n, int sleepMs = 16) {
        for (int i = 0; i < n; ++i)
            tick(sleepMs);
    }
    /// 推进到条件成立（或帧数用尽）；返回是否成立。
    bool until(const std::function<bool()>& pred, int maxTicks = 80) {
        for (int i = 0; i < maxTicks; ++i) {
            if (pred())
                return true;
            tick();
        }
        return pred();
    }
};

// ── 造路由 ─────────────────────────────────────────────────────────────────

SharedPtr<RouteSettings> named(const std::string& name) {
    return cui::make_shared<RouteSettings>(name);
}

/// 一条整屏路由：pageBuilder 记进 probe，默认「硬切」转场。
SharedPtr<PageRouteBuilder> pageRoute(PageProbe* probe, double durationMs = 300.0,
                                      std::string routeName = {}, bool opaque = true,
                                      SharedPtr<const RouteSettings> settings = nullptr) {
    return cui::make_shared<PageRouteBuilder>(
            [probe](BuildContext& context, SharedPtr<Animation<double>>,
                    SharedPtr<Animation<double>>) -> SharedPtr<Widget> {
                ++probe->buildCalls;
                probe->context = &context;
                return cui::make_shared<SizedBox>(20, 20);
            },
            nullptr, durationMs, std::nullopt, opaque, /*maintainState=*/true,
            std::nullopt, /*barrierDismissible=*/false, /*fullscreenDialog=*/false,
            settings ? std::move(settings)
                     : (routeName.empty() ? nullptr : named(routeName)));
}

/// 挂着 PageRouteBuilder 的 Page：用来走「页面式 Navigator」（didAdd 直现路径）。
class HolderPage : public Page {
public:
    HolderPage(PageProbe* probe, double durationMs)
        : Page(nullptr, "home"), probe_(probe), durationMs_(durationMs) {}

    SharedPtr<Route> createRoute(BuildContext&) override {
        // 上游 的 `settings: this` ⇒ 本路由是 page-based；Navigator 靠这条
        // 判定 popDisposition 里的 doNotPop 分支。
        auto self = cui::make_shared<HolderPage>(*this);
        created_ = pageRoute(probe_, durationMs_, /*routeName=*/{}, true, self);
        return created_;
    }

    const SharedPtr<PageRouteBuilder>& created() const { return created_; }

private:
    PageProbe* probe_ = nullptr;
    double durationMs_ = 0.0;
    SharedPtr<PageRouteBuilder> created_;
};

/// 一条淡入淡出的弹窗路由（RawDialogRoute 的缺省转场就是 FadeTransition）。
SharedPtr<RawDialogRoute> dialogRoute(PageProbe* probe, double durationMs = 200.0,
                                      bool barrierDismissible = true) {
    return cui::make_shared<RawDialogRoute>(
            [probe](BuildContext& context, SharedPtr<Animation<double>>,
                    SharedPtr<Animation<double>>) -> SharedPtr<Widget> {
                ++probe->buildCalls;
                probe->context = &context;
                return cui::make_shared<SizedBox>(30, 30);
            },
            barrierDismissible, Color::fromRGBA(0, 0, 0, 0.5), durationMs);
}

// ── 可改开关的 TickerMode 外壳 ─────────────────────────────────────────────

struct GateConfig {
    bool enabled = true;
};

/// 每帧按 config->enabled 现造一条 TickerMode ⇒ 测试里改配置 + markNeedsBuild
/// 就能翻转环境（TickerModeBinding 订阅的是 notifier，不靠子树重建）。
class Gate : public StatefulWidget {
public:
    Gate(SharedPtr<Widget> child, SharedPtr<GateConfig> config)
        : StatefulWidget(nullptr), child_(std::move(child)),
          config_(std::move(config)) {}

    const SharedPtr<Widget>& child() const { return child_; }
    const SharedPtr<GateConfig>& config() const { return config_; }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Widget> child_;
    SharedPtr<GateConfig> config_;
};

class GateState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        const auto* gate = static_cast<const Gate*>(widget());
        return cui::make_shared<TickerMode>(gate->child(), gate->config()->enabled);
    }
};

inline std::unique_ptr<State> Gate::createState() { return std::make_unique<GateState>(); }

Element* childElementOf(Element* root, const std::function<bool(Element*)>& pred) {
    if (!root)
        return nullptr;
    if (pred(root))
        return root;
    Element* found = nullptr;
    root->visitChildren([&](Element* child) {
        if (!found && child)
            found = childElementOf(child, pred);
    });
    return found;
}

// ── 1. 入场转场 ────────────────────────────────────────────────────────────

void testEntranceTransition() {
    std::printf("[test] 1. 入场转场：pushing → 动画走完 → idle\n");

    PageProbe probe;
    Harness h;
    auto navigator = cui::make_shared<Navigator>();
    h.attach(navigator);
    check(h.state != nullptr, "NavigatorState 就位");

    auto route = pageRoute(&probe, 300.0, "first");
    h.state->push(route);
    h.pump();

    check(route->installed(), "push 后路由已安装");
    check(route->overlayEntries().size() == 2, "ModalRoute 有两条 Overlay 条目（barrier + scope）");
    check(probe.buildCalls == 1, "buildPage 恰好调一次");
    check(!route->settlesWhenPushed(), "入场未完成 ⇒ settlesWhenPushed 为假（条目停在 pushing）");
    check(route->controller()->value() == 0.0, "起涨前动画值为 0");
    check(route->overlayEntries()[0]->opaque() == false, "转场进行中 barrier 条目不 opaque");

    h.ticks(3);
    const double mid = route->controller()->value();
    check(mid > 0.0 && mid < 1.0, "真实帧推进后动画落在中段");
    check(!route->settlesWhenPushed(), "中段仍未完成");

    bool done = h.until([&] { return route->controller()->isCompleted(); });
    check(done, "入场动画走完");
    check(route->settlesWhenPushed(), "完成后 settlesWhenPushed 为真");
    check(route->animation()->value() == 1.0, "对外动画值 = 上界");
    check(route->overlayEntries()[0]->opaque() == true, "整屏路由完成后 barrier 条目转 opaque");
    check(route->isCurrent(), "栈顶路由 isCurrent");
    check(h.state->history().size() == 1, "历史长度 1");

    // 再推进不应改变任何东西（动画已停在 Completed）。
    h.ticks(2);
    check(probe.buildCalls == 1, "稳态帧不重调 buildPage（_page 缓存生效）");
    check(route->controller()->value() == 1.0, "完成后值钉在上界");
}

// ── 2. secondaryAnimation 联动 ─────────────────────────────────────────────

void testSecondaryAnimation() {
    std::printf("[test] 2. secondaryAnimation 跟随下一条路由的动画\n");

    PageProbe p1;
    PageProbe p2;
    Harness h;
    h.attach(cui::make_shared<Navigator>());

    auto first = pageRoute(&p1, 300.0, "first");
    h.state->push(first);
    check(h.until([&] { return first->controller()->isCompleted(); }), "第一条入场完成");
    check(first->secondaryAnimation()->value() == 0.0, "无上层路由时次级动画恒 0");
    check(first->secondaryAnimation()->status() == AnimationStatus::Dismissed,
          "无上层路由时次级动画状态为 Dismissed");

    auto second = pageRoute(&p2, 300.0, "second");
    h.state->push(second);
    h.ticks(3);
    check(second->controller()->value() > 0.0, "第二条入场中");
    check(nearly(first->secondaryAnimation()->value(), second->controller()->value()),
          "上一条的次级动画 = 下一条的动画值");

    check(h.until([&] { return second->controller()->isCompleted(); }), "第二条入场完成");
    check(first->secondaryAnimation()->value() == 1.0, "下一条完成后次级动画钉在 1");
    check(!first->isCurrent(), "第一条不再是栈顶");

    // 弹出第二条：次级动画跟着退场回落，并在其 dispose 后复位成常量动画。
    h.state->pop();
    h.ticks(2);
    check(first->secondaryAnimation()->value() < 1.0, "退场中次级动画往下走");
    check(h.until([&] { return second->routeDisposed(); }), "第二条退场动画走完并 dispose");
    check(first->secondaryAnimation()->value() == 0.0,
          "下一条 dispose 后次级动画复位为常量 0（routes.h 偏离 10）");
    check(first->isCurrent(), "第一条重新成为栈顶");

    // 非 PageRoute 不联动：弹窗压在页面上时页面次级动画保持 0。
    PageProbe p3;
    auto dialog = dialogRoute(&p3, 200.0);
    h.state->push(dialog);
    h.ticks(3);
    check(dialog->controller()->value() > 0.0, "弹窗淡入中");
    check(first->secondaryAnimation()->value() == 0.0,
          "PopupRoute 不接次级动画（PageRoute::canTransitionTo 只认 PageRoute）");
    check(dialog->secondaryAnimation()->value() == 0.0, "弹窗上面没有路由 ⇒ 自己的次级动画为 0");
    check(h.until([&] { return dialog->controller()->isCompleted(); }), "弹窗淡入完成");
}

// ── 3. 本地历史 ────────────────────────────────────────────────────────────

void testLocalHistory() {
    std::printf("[test] 3. LocalHistoryEntry：pop 就地消化\n");

    PageProbe p1;
    PageProbe p2;
    Harness h;
    h.attach(cui::make_shared<Navigator>());

    auto first = pageRoute(&p1, 300.0, "first");
    auto second = pageRoute(&p2, 300.0, "second");
    h.state->push(first);
    h.state->push(second);
    check(h.until([&] { return second->controller()->isCompleted(); }), "两条路由入场完成");

    check(!first->canPop(), "栈底且无本地历史 ⇒ canPop 为假");
    check(first->popDisposition() == RoutePopDisposition::bubble,
          "栈底路由的 popDisposition 是 bubble");

    int removed = 0;
    auto entry = cui::make_shared<LocalHistoryEntry>([&] { ++removed; });
    second->addLocalHistoryEntry(entry);
    h.pump();

    check(second->localHistoryCount() == 1, "本地历史长度 1");
    check(entry->owner() == second.get(), "条目记下了所属路由");
    check(second->willHandlePopInternally(), "有本地历史 ⇒ willHandlePopInternally");
    check(second->popDisposition() == RoutePopDisposition::pop, "有本地历史 ⇒ popDisposition 为 pop");
    check(!second->isFirst() && second->canPop(), "压在别人上面 ⇒ canPop");

    // pop 被就地消化：路由留在栈上，只弹一条 entry。
    h.state->pop();
    h.pump();
    check(removed == 1, "onRemove 被调一次");
    check(second->localHistoryCount() == 0, "本地历史清空");
    check(h.state->history().size() == 2, "pop 被就地消化，历史长度不变");
    check(second->isCurrent(), "第二条仍在栈顶");
    check(!second->willHandlePopInternally(), "清空后不再自行消化 pop");
    check(!entry->owner(), "摘掉的条目 owner 已置空");

    // 空了之后再 pop ⇒ 真的退场。
    h.state->pop();
    h.ticks(2);
    check(h.until([&] { return second->routeDisposed(); }), "第二条随后正常退场");
    check(removed == 1, "退场不会重复触发 onRemove");
    check(h.state->history().size() == 1, "历史长度回到 1");

    // entry->remove() 自己摘（同一个 onRemove 只发一次）。
    int removed2 = 0;
    auto entry2 = cui::make_shared<LocalHistoryEntry>([&] { ++removed2; });
    first->addLocalHistoryEntry(entry2);
    check(first->localHistoryCount() == 1, "第一条挂上本地历史");
    entry2->remove();
    check(removed2 == 1, "remove() 触发 onRemove");
    check(first->localHistoryCount() == 0, "remove() 后历史清空");
    check(entry2->owner() == nullptr, "remove() 后 owner 置空");

    // 重复 add 同一条（仍挂在别处）被忽略。
    auto entry3 = cui::make_shared<LocalHistoryEntry>([&] { ++removed2; });
    first->addLocalHistoryEntry(entry3);
    first->addLocalHistoryEntry(entry3);
    check(first->localHistoryCount() == 1, "已挂在某路由上的条目不重复登记");
    first->removeLocalHistoryEntry(entry3.get());
    check(removed2 == 2, "removeLocalHistoryEntry 也发 onRemove");
}

// ── 4. ModalRoute 的静态查询 ───────────────────────────────────────────────

void testRouteQueries() {
    std::printf("[test] 4. ModalRoute::maybeOf / …Of / withName\n");

    PageProbe p1;
    PageProbe p2;
    Harness h;
    h.attach(cui::make_shared<Navigator>());

    auto first = pageRoute(&p1, 300.0, "first");
    auto second = pageRoute(&p2, 300.0, "second");
    h.state->push(first);
    h.state->push(second);
    check(h.until([&] { return second->controller()->isCompleted(); }), "两条入场完成");

    check(p1.context != nullptr && p2.context != nullptr, "两个页面都拿到了 BuildContext");
    if (!p1.context || !p2.context)
        return;

    check(ModalRoute::maybeOf(*p2.context) == second.get(), "maybeOf 找到本路由");
    check(ModalRoute::of(*p1.context) == first.get(), "of 找到下面那条");
    check(ModalRoute::isCurrentOf(*p2.context) == std::optional<bool>(true), "isCurrentOf 栈顶为真");
    check(ModalRoute::isCurrentOf(*p1.context) == std::optional<bool>(false), "isCurrentOf 下面那条为假");
    check(ModalRoute::isActiveOf(*p1.context) == std::optional<bool>(true), "isActiveOf 在场为真");
    check(ModalRoute::isFirstOf(*p1.context) == std::optional<bool>(true), "isFirstOf 栈底为真");
    check(ModalRoute::isFirstOf(*p2.context) == std::optional<bool>(false), "isFirstOf 栈顶为假");
    check(ModalRoute::opaqueOf(*p2.context) == std::optional<bool>(true), "opaqueOf 整屏路由为真");
    check(ModalRoute::canPopOf(*p2.context) == std::optional<bool>(true), "canPopOf 栈顶为真");
    check(ModalRoute::popDispositionOf(*p2.context) ==
                  std::optional<RoutePopDisposition>(RoutePopDisposition::pop),
          "popDispositionOf 返回 pop");
    const RouteSettings* settings = ModalRoute::settingsOf(*p2.context);
    check(settings != nullptr && settings->name() == "second", "settingsOf 拿到路由名");

    // withName：popUntil 的谓词。
    auto predicate = ModalRoute::withName("first");
    check(predicate(second.get()) == false, "withName 不匹配别的路由");
    check(predicate(first.get()) == true, "withName 匹配同名路由");
    PageProbe dialogProbe;
    auto dialog = dialogRoute(&dialogProbe, 150.0);
    check(predicate(dialog.get()) == false, "无名路由不匹配 withName");
    h.state->push(dialog);
    h.pump();
    check(dialogProbe.context != nullptr, "弹窗页面也拿到了 BuildContext");
    if (dialogProbe.context)
        check(ModalRoute::maybeOf(*dialogProbe.context) == dialog.get(),
              "弹窗路由的 maybeOf 找到自己");
    check(ModalRoute::opaqueOf(*dialogProbe.context) == std::optional<bool>(false),
          "opaqueOf 对浮层路由为假");

    h.state->popUntil(predicate);
    check(h.until([&] { return h.state->history().size() == 1; }), "popUntil 退到 first");
    check(first->isCurrent(), "first 重新成为栈顶");
    // pop 只是把路由移出「在场」历史；dispose 要等退场动画走到下界（同 §5）。
    check(h.until([&] { return second->routeDisposed(); }), "second 已随退场动画 dispose");
}

// ── 5. 退场转场与完成回调 ──────────────────────────────────────────────────

void testExitTransition() {
    std::printf("[test] 5. 退场转场：finalize 等到动画下界 + whenTransitionCompleted\n");

    PageProbe p1;
    PageProbe p2;
    Harness h;
    h.attach(cui::make_shared<Navigator>());

    auto first = pageRoute(&p1, 300.0, "first");
    auto second = pageRoute(&p2, 300.0, "second");
    h.state->push(first);
    h.state->push(second);
    check(h.until([&] { return second->controller()->isCompleted(); }), "第二条入场完成");

    int completedCalls = 0;
    SharedPtr<void> completedResult;
    second->whenTransitionCompleted([&](SharedPtr<void> result) {
        ++completedCalls;
        completedResult = result;
    });
    int poppedCalls = 0;
    second->whenPopped([&](SharedPtr<void>) { ++poppedCalls; });

    int payload = 7;
    auto result = cui::make_shared<int>(payload);
    h.state->pop(result);

    check(second->controller()->status() == AnimationStatus::Reverse, "didPop 起了退场动画");
    check(!h.state->isRoutePresent(second.get()), "popping 期间已算「不在场」");
    check(!second->routeDisposed(), "退场动画未完就先不 dispose");
    check(second->overlayEntries().size() == 2, "退场中两条 Overlay 条目仍在");
    check(completedCalls == 0, "转场未结束 ⇒ whenTransitionCompleted 未派发");
    check(poppedCalls == 1, "whenPopped 在弹出那一刻派发（上游 同）");
    check(second->controller()->isAnimating(), "退场中 controller 仍在运行");
    check(!second->finishedWhenPopped(), "动画未到下界 ⇒ finishedWhenPopped 为假（不当场 finalize）");

    check(h.until([&] { return second->routeDisposed(); }, 120), "退场动画走到下界后 finalize 并 dispose");
    check(completedCalls == 1, "whenTransitionCompleted 派发一次");
    check(completedResult.get() == static_cast<void*>(result.get()), "完成回调拿到弹出结果");
    check(second->transitionCompleted(), "transitionCompleted 为真");
    check(second->overlayEntries().empty(), "dispose 后 Overlay 条目清空");
    check(second->navigator() == nullptr, "dispose 后脱离 Navigator");
    check(!second->installed(), "installed 转假");
    check(h.state->history().size() == 1, "历史只剩第一条");
    check(!Ticker::hasActiveTickers(), "路由的 Ticker 随退场停表");

    // 栈底那条没起退场：它还在场，且没有活动 Ticker。
    check(first->isActive() && first->isCurrent(), "栈底路由不受影响");
    check(!first->routeDisposed(), "栈底路由未被 dispose");
}

// ── 6. TickerMode 冻结 ─────────────────────────────────────────────────────

void testTickerModeFreeze() {
    std::printf("[test] 6. TickerMode 静音：转场时间线不吃 tick\n");

    PageProbe p1;
    auto config = cui::make_shared<GateConfig>();
    config->enabled = false;
    auto navigator = cui::make_shared<Navigator>();
    auto gate = cui::make_shared<Gate>(navigator, config);
    Harness h;
    h.attach(gate);
    check(h.state != nullptr, "隔着 TickerMode 也定位到 NavigatorState");

    auto route = pageRoute(&p1, 300.0, "muted");
    h.state->push(route);
    h.ticks(3, 30);
    check(route->controller()->value() == 0.0, "静音期间动画不动（路由的 Ticker 被 mute）");
    check(route->controller()->isAnimating(), "controller 仍在「运行中」，只是没被喂帧");
    check(!Ticker::hasActiveTickers(), "全静音 ⇒ 不再拉帧");

    // 解除静音：本仓喂给 controller 的是路由自己累加的帧时间线，
    // 而 Ticker 的 elapsed 用真实时钟 ⇒ 恢复后一次补上静音那段（与上游 同形）。
    config->enabled = true;
    Element* gateElement = childElementOf(h.root, [](Element* e) {
        return dynamic_cast<GateState*>(
                       e && dynamic_cast<StatefulElement*>(e)
                               ? static_cast<StatefulElement*>(e)->state()
                               : nullptr) != nullptr;
    });
    check(gateElement != nullptr, "找到 Gate 的 Element");
    if (gateElement) {
        gateElement->markNeedsBuild();
        h.pump();
    }
    h.ticks(2);
    check(route->controller()->value() > 0.0, "解除静音后动画继续推进");
    check(h.until([&] { return route->controller()->isCompleted(); }), "静音过的转场最终仍能走完");
    check(route->settlesWhenPushed(), "完成后条目落位 idle");
}

// ── 7. offstage 与零时长转场 ───────────────────────────────────────────────

void testOffstageAndZeroDuration() {
    std::printf("[test] 7. offstage 常量代理 + 零时长转场\n");

    PageProbe p1;
    Harness h;
    h.attach(cui::make_shared<Navigator>());

    auto first = pageRoute(&p1, 300.0, "first");
    h.state->push(first);
    check(h.until([&] { return first->controller()->isCompleted(); }), "第一条入场完成");
    check(!first->offstage(), "默认不在 offstage");

    const int callsBefore = p1.buildCalls;
    first->setOffstage(true);
    h.pump();
    check(first->offstage(), "setOffstage(true) 生效");
    check(first->animation()->value() == 1.0, "offstage 时对外动画恒为「已完成」");
    check(first->secondaryAnimation()->value() == 0.0, "offstage 时次级动画恒为 0");
    check(p1.buildCalls == callsBefore, "setOffstage 只重建，不重跑 buildPage");
    check(findWidget<Offstage>(h.root) != nullptr, "子树里挂着 Offstage 包装");

    first->setOffstage(false);
    h.pump();
    check(!first->offstage(), "setOffstage(false) 复位");
    check(first->animation()->value() == first->controller()->value(),
          "回到真实动画（本仓没有「动画中途的脏值」）");

    // 零时长：forward() 走到上界只需一帧。
    PageProbe p2;
    auto quick = pageRoute(&p2, 0.0, "quick");
    h.state->push(quick);
    h.tick();
    check(quick->controller()->isCompleted(), "零时长入场一帧即完成");
    check(quick->settlesWhenPushed(), "零时长也走 didFinishPushTransition 落位");
    h.state->pop();
    h.tick();
    check(h.until([&] { return quick->routeDisposed(); }), "零时长退场同样 dispose");

    // 直接「在场出现」的路由（didAdd，页面式 Navigator 的首屏路径）：
    // 动画当场钉在上界，没有入场转场。
    PageProbe pHome;
    auto home = cui::make_shared<HolderPage>(&pHome, 400.0);
    auto pageNavigator = cui::make_shared<Navigator>(std::vector<SharedPtr<Page>>{home});
    Harness h2;
    h2.attach(pageNavigator);
    check(h2.state != nullptr, "页面式 Navigator 定位到 NavigatorState");
    check(h2.state && h2.state->history().size() == 1, "首屏页有一条历史");
    check(home->created() != nullptr, "createRoute 产出了 PageRouteBuilder");
    if (home->created()) {
        const SharedPtr<PageRouteBuilder>& added = home->created();
        check(added->controller()->value() == 1.0, "didAdd 把值直接钉到上界");
        check(added->controller()->isCompleted(), "didAdd 之后动画状态为 Completed");
        check(added->settlesWhenPushed(), "常量化的入场被视为已完成（条目落 idle）");
        check(added->overlayEntries()[0]->opaque() == true, "直现路由的 barrier 条目立刻 opaque");
        check(added->isPageBased(), "settings 带 Page ⇒ 该路由是 page-based");
    }
    check(pHome.buildCalls == 1, "首屏页只建一次");
}

// ── 8. PopupRoute / RawDialogRoute ─────────────────────────────────────────

void testPopupRoute() {
    std::printf("[test] 8. PopupRoute / RawDialogRoute\n");

    PageProbe p1;
    PageProbe p2;
    Harness h;
    h.attach(cui::make_shared<Navigator>());

    auto first = pageRoute(&p1, 300.0, "first");
    h.state->push(first);
    check(h.until([&] { return first->controller()->isCompleted(); }), "页面入场完成");
    check(findWidget<AnimatedModalBarrier>(h.root) == nullptr,
          "整屏路由无 barrierColor ⇒ 未建 AnimatedModalBarrier");

    auto dialog = dialogRoute(&p2, 200.0, /*barrierDismissible=*/true);
    h.state->push(dialog);
    check(dialog->opaque() == false, "PopupRoute 不透明为假");
    check(dialog->maintainState() == true, "PopupRoute maintainState 为真");
    check(dialog->barrierColor().has_value(), "RawDialogRoute 有遮罩色");
    check(dialog->barrierDismissible(), "barrierDismissible 透传");
    check(dialog->overlayEntries().size() == 2, "弹窗也是 barrier + scope 两条条目");
    check(dialog->overlayEntries()[1]->maintainState() == true, "scope 条目按 maintainState 登记");

    h.ticks(2);
    check(findWidget<AnimatedModalBarrier>(h.root) != nullptr, "遮罩用 AnimatedModalBarrier（颜色随动画）");
    check(dialog->animation()->isForwardOrCompleted(), "入场段的目标是上界 ⇒ 遮罩外层 IgnorePointer 不忽略");

    check(h.until([&] { return dialog->controller()->isCompleted(); }), "弹窗淡入完成");
    check(dialog->overlayEntries()[0]->opaque() == false, "浮层路由转场结束后仍不 opaque");
    check(dialog->animation()->value() == 1.0, "淡入到位");

    // 页面在弹窗之下仍保持构建（maintainState + 下层不 opaque）。
    check(p1.buildCalls >= 1, "下层页面仍在场");
    check(h.state->history().size() == 2, "历史两条");

    h.state->pop();
    check(h.until([&] { return dialog->routeDisposed(); }), "弹窗退场并 dispose");
    check(h.state->history().size() == 1, "只剩页面");
    check(findWidget<AnimatedModalBarrier>(h.root) == nullptr, "弹窗摘掉后遮罩随之消失");
}

// ── 主流程 ────────────────────────────────────────────────────────────────

}  // namespace

int main() {
    std::printf("=== 路由转场层（OverlayRoute / TransitionRoute / ModalRoute / PageRoute）测试 ===\n");
    testEntranceTransition();
    testSecondaryAnimation();
    testLocalHistory();
    testRouteQueries();
    testExitTransition();
    testTickerModeFreeze();
    testOffstageAndZeroDuration();
    testPopupRoute();
    std::printf("\n%s：失败 %d 项\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
