/// @file test_ticker_mode.cpp
/// @brief 批次B 前置件单元测试：Ticker 静音 / TickerMode / 帧末回调 /
///        BuildContext 子代访问 / getTransformTo 坐标换算。
///
/// 覆盖：
///   1. `Ticker::muted`（上游 scheduler/ticker.dart）：静音时**时钟照走**、
///      回调不打、`hasActiveTickers()` 不再拉帧。
///   2. `TickerMode`（上游 widgets/ticker_provider.dart）：生效值 = 祖先链
///      enabled 之与；`TickerModeBinding` 把它接到 State 自持的 Ticker 上。
///   3. `BuildOwner::addPostFrameCallback`：帧末执行、执行期新加的落下一帧、
///      `inPostFrameCallbacks()` / `inBuildPhase()` 两个阶段标志。
///   4. `BuildContext::visitChildElements` / `visitAncestorElements`：
///      build 期间不得访问子代（上游的 debug 断言）。
///   5. `RenderObject::getTransformTo` + 2 参 `localToGlobal/globalToLocal`：
///      与既有 1 参版本同口径，且能换算到指定祖先。
///
/// 全部 headless。帧泵按约定不做 flushPaint。

#include <chrono>
#include <cmath>
#include <cstdio>
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
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
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

    void rebuildAndLayout() {
        owner.flushBuildScope();
        po.flushLayout();
    }
};

/// 先序收集子树里所有 T 类型 RenderObject。
template <typename T>
std::vector<T*> findAllRo(RenderObject* ro) {
    std::vector<T*> out;
    std::vector<RenderObject*> stack{ro};
    // 先序：用「弹出队首」而不是栈，保证外层容器排在前面。
    while (!stack.empty()) {
        RenderObject* n = stack.front();
        stack.erase(stack.begin());
        if (auto* hit = dynamic_cast<T*>(n))
            out.push_back(hit);
        std::vector<RenderObject*> kids;
        n->visitChildren([&](AbstractNode* c) {
            if (c)
                kids.push_back(static_cast<RenderObject*>(c));
        });
        stack.insert(stack.begin(), kids.begin(), kids.end());
    }
    return out;
}

// ── 测试 1：Ticker 静音 ─────────────────────────────────────────────────────
void testTickerMuting() {
    std::printf("[test] Ticker::muted（静音不打回调、时钟照走）\n");

    int ticks = 0;
    Ticker ticker([&](Duration) { ++ticks; });

    check(!ticker.isActive() && !ticker.isTicking(), "初始：既不 active 也不 ticking");
    check(!ticker.muted(), "初始：未静音");

    ticker.start();
    check(ticker.isActive() && ticker.isTicking(), "start 后：active 且 ticking");
    ticker.tick(Duration(16));
    check(ticks == 1, "未静音：回调被调");
    // 本仓 Ticker::tick 忽略传入的帧时间，用 steady_clock 现算 elapsed()
    // （见 foundation/ticker.h 头注释），所以这里只能断言「被写入」而不是精确值。
    const Duration firstElapsed = ticker.lastElapsed();
    check(firstElapsed >= Duration(0), "未静音：lastElapsed 由 tick 写入");

    ticker.setMuted(true);
    check(ticker.isActive(), "静音后：仍然 active（时间在看）");
    check(!ticker.isTicking(), "静音后：不再 ticking");
    const Duration frozen = ticker.lastElapsed();
    ticker.tick(Duration(120));
    check(ticks == 1, "静音后：回调不再被调");
    check(ticker.lastElapsed() == frozen, "静音后：lastElapsed 冻结（上游 的 _tick 根本不被调度）");
    check(!Ticker::hasActiveTickers(), "全静音 ⇒ 不再拉帧（上游 shouldScheduleTick 带 !muted）");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(ticker.elapsed() > frozen, "静音期间：时钟照走（elapsed 现算，不「接着放」）");

    ticker.setMuted(false);
    check(ticker.isTicking(), "解除静音：恢复 ticking");
    ticker.tick(Duration(150));
    check(ticks == 2, "解除静音后：回调恢复");
    check(ticker.lastElapsed() > frozen, "解除静音后：一次就补上静音期间过去的时间");

    ticker.setMuted(true);
    ticker.setMuted(true);
    check(ticker.muted(), "setMuted 等值早退：重复设同一值不变状态");
    ticker.stop();
    check(!ticker.isActive() && !ticker.isTicking(), "stop 后：既不 active 也不 ticking");
    ticker.tick(Duration(200));
    check(ticks == 2, "stop 后：tick 是空操作");
}

// ── 探针：自持 Ticker 并订阅 ambient TickerMode ─────────────────────────────
class ModeProbeState;

class ModeProbe : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
};

class ModeProbeState : public State {
public:
    void initState() override {
        ticker_ = std::make_unique<Ticker>([this](Duration) { ++ticks; });
        ticker_->start();  // 让「静音 ⇒ 不出 tick」可观测
    }

    void didChangeDependencies() override {
        // valuesOf 建依赖：TickerMode 变化会让我们重算（上游 mixin 用
        // getValuesNotifier 不建依赖，这里两种通道都要覆盖到）。
        if (!context())
            return;
        lastEnabled = TickerMode::valuesOf(*context()).enabled;
        lastForceFrames = TickerMode::valuesOf(*context()).forceFrames;
        binding_.bind(*context(), *ticker_);
        notifierEnabled = TickerMode::valuesNotifierOf(*context())->value().enabled;
    }

    void dispose() override {
        binding_.unbind();
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        ticker_.reset();
    }

    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<SizedBox>(10, 10);
    }

    std::unique_ptr<Ticker> ticker_;
    TickerModeBinding binding_;
    int ticks = 0;
    bool lastEnabled = true;
    bool lastForceFrames = false;
    bool notifierEnabled = true;
};

inline std::unique_ptr<State> ModeProbe::createState() {
    return std::make_unique<ModeProbeState>();
}

/// 可变开关的 TickerMode 外壳：改 enabled → setState → 下面的探针重算。
class GateState;

class Gate : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
};

class GateState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<TickerMode>(probe, enabled, forceFrames);
    }

    void setEnabled(bool value) {
        enabled = value;
        setState();
    }
    void setForceFrames(bool value) {
        forceFrames = value;
        setState();
    }

    SharedPtr<ModeProbe> probe = cui::make_shared<ModeProbe>();
    bool enabled = true;
    bool forceFrames = false;
};

inline std::unique_ptr<State> Gate::createState() { return std::make_unique<GateState>(); }

ModeProbeState* probeState(Element* root) {
    // 根 → TickerMode(StatefulElement) → EffectiveTickerMode → probe(StatefulElement)
    ModeProbeState* found = nullptr;
    std::vector<Element*> stack{root};
    while (!stack.empty() && !found) {
        Element* e = stack.front();
        stack.erase(stack.begin());
        if (auto* se = dynamic_cast<StatefulElement*>(e)) {
            if (auto* s = dynamic_cast<ModeProbeState*>(se->state()))
                found = s;
        }
        e->visitChildren([&](Element* c) {
            if (c)
                stack.push_back(c);
        });
    }
    return found;
}

// ── 测试 2：TickerMode 与绑定 ───────────────────────────────────────────────
void testTickerMode() {
    std::printf("[test] TickerMode（与祖先做「与」/ 或做「或」+ 接 Ticker）\n");

    // 无祖先 ⇒ fallback = { enabled: true, forceFrames: false }
    check(TickerModeData::fallback().enabled && !TickerModeData::fallback().forceFrames,
          "fallback 值与上游 一致");

    {
        Harness h;
        auto gate = cui::make_shared<Gate>();
        h.attach(gate, 200, 200);
        auto* g = dynamic_cast<GateState*>(
                static_cast<StatefulElement*>(h.root)->state());
        check(g != nullptr, "Gate 的 State 取到");
        auto* p = probeState(h.root);
        check(p != nullptr, "探针 State 取到");
        check(p->lastEnabled && p->notifierEnabled, "默认：TickerMode(enabled=true) ⇒ 生效值 true");
        check(!p->ticker_->muted(), "默认：Ticker 未被静音");

        p->ticks = 0;
        p->ticker_->tick(Duration(16));
        check(p->ticks == 1, "启用态：Ticker 出 tick");

        // 关掉 ⇒ 静音；时钟仍算「在跑」，只是回调被摘。
        g->setEnabled(false);
        h.rebuildAndLayout();
        check(!p->lastEnabled, "enabled=false ⇒ valuesOf 变 false");
        check(!p->notifierEnabled, "notifier 通道同样收到 false");
        check(p->ticker_->muted(), "Ticker 被静音");
        check(!p->ticker_->isTicking() && p->ticker_->isActive(),
              "静音：active 但不 ticking（上游 口径）");
        const int before = p->ticks;
        p->ticker_->tick(Duration(32));
        check(p->ticks == before, "静音：回调不再打");
        check(!Ticker::hasActiveTickers(), "静音：不再拉帧");

        // 再打开 ⇒ 解除静音。
        g->setEnabled(true);
        h.rebuildAndLayout();
        check(p->lastEnabled && !p->ticker_->muted(), "重新启用 ⇒ 解除静音");
        p->ticks = 0;
        p->ticker_->tick(Duration(48));
        check(p->ticks == 1, "解除后：回调恢复");

        // forceFrames 走「或」通道（值记录，不参与调度）。
        g->setForceFrames(true);
        h.rebuildAndLayout();
        check(p->lastForceFrames, "forceFrames=true ⇒ 生效值里为 true");
        g->setEnabled(false);
        h.rebuildAndLayout();
        check(!p->lastEnabled && p->lastForceFrames,
              "enabled 取与、forceFrames 取或（关 enabled 不影响已开的 force）");

        // unbind 不是永久的：ambient 值再变化时 didChangeDependencies 会重新
        // 绑定（对齐上游 mixin 的 didChangeDependencies 重绑路径）。
        p->binding_.unbind();
        g->setEnabled(true);
        h.rebuildAndLayout();
        check(!p->ticker_->muted(), "ambient 再变 ⇒ 重新绑定，解除静音");
        check(p->lastEnabled && p->lastForceFrames, "重绑后 valuesOf 与 notifier 通道都跟上");
    }
    check(!Ticker::hasActiveTickers(), "树销毁后：全局不再有拉帧的 Ticker");

    // 嵌套：外层 false ⇒ 内层 true 也压不回 true（与祖先取「与」）。
    {
        Harness h;
        auto inner = cui::make_shared<TickerMode>(cui::make_shared<ModeProbe>(), true);
        auto outer = cui::make_shared<TickerMode>(inner, false);
        h.attach(outer, 200, 200);
        auto* p = probeState(h.root);
        check(p && !p->lastEnabled, "TickerMode(false) 包 TickerMode(true) ⇒ 生效值 false");
    }
    {
        Harness h;
        auto inner = cui::make_shared<TickerMode>(cui::make_shared<ModeProbe>(), false);
        auto outer = cui::make_shared<TickerMode>(inner, true);
        h.attach(outer, 200, 200);
        auto* p = probeState(h.root);
        check(p && !p->lastEnabled, "TickerMode(true) 包 TickerMode(false) ⇒ 生效值 false");
    }
    {
        Harness h;
        auto inner = cui::make_shared<TickerMode>(cui::make_shared<ModeProbe>(), true);
        auto outer = cui::make_shared<TickerMode>(inner, true);
        h.attach(outer, 200, 200);
        auto* p = probeState(h.root);
        check(p && p->lastEnabled, "两层都开 ⇒ 生效值 true");
    }

    // valuesNotifierOf 在没有祖先时给常量源（永不通知）。
    {
        Harness h;
        auto probe = cui::make_shared<ModeProbe>();
        h.attach(probe, 200, 200);
        auto* p = probeState(h.root);
        check(p && p->notifierEnabled, "无 TickerMode 祖先 ⇒ 常量源给 true");
        check(p && p->ticker_ && !p->ticker_->muted(), "无祖先 ⇒ Ticker 不静音");
    }
}

// ── 测试 3：帧末回调 ────────────────────────────────────────────────────────
void testPostFrameCallbacks() {
    std::printf("[test] BuildOwner::addPostFrameCallback\n");

    Harness h;
    h.attach(cui::make_shared<SizedBox>(50, 50), 200, 200);

    std::vector<std::string> log;
    Duration stampSeen = Duration(-1);
    bool flagDuring = false;

    h.owner.addPostFrameCallback([&](Duration stamp) {
        stampSeen = stamp;
        flagDuring = h.owner.inPostFrameCallbacks();
        log.push_back("a");
        // 上游 语义：执行期新加的回调排在**下一帧**末尾。
        h.owner.addPostFrameCallback([&](Duration) { log.push_back("b"); });
    });
    h.owner.addPostFrameCallback([&](Duration) { log.push_back("c"); });

    check(h.owner.postFrameCallbackCount() == 2, "两条回调入队");
    check(!h.owner.inPostFrameCallbacks(), "冲刷前：不在 postFrameCallbacks 阶段");

    h.owner.flushPostFrameCallbacks(Duration(1234.0));
    check(log.size() == 2, "本帧执行了两条（快照迭代）");
    check(log[0] == "a" && log[1] == "c", "按入队顺序执行");
    check(flagDuring, "执行中：inPostFrameCallbacks() 为 true");
    check(!h.owner.inPostFrameCallbacks(), "执行后：阶段标志复位");
    check(stampSeen == Duration(1234.0), "帧时间戳透传给回调");
    check(h.owner.postFrameCallbackCount() == 1, "执行期新加的回调留到下一帧");

    h.owner.flushPostFrameCallbacks();  // 不给时间戳：BuildOwner 自己现算
    check(log.size() == 3 && log[2] == "b", "下一帧末尾执行 b");
    check(h.owner.postFrameCallbackCount() == 0, "队列已空");

    h.owner.flushPostFrameCallbacks();  // 空队列：幂等
    check(log.size() == 3, "空队列冲刷不再执行任何东西");

    // 空回调被忽略（上游 的 addPostFrameCallback 要求非空，这里静默丢弃）。
    h.owner.addPostFrameCallback(nullptr);
    check(h.owner.postFrameCallbackCount() == 0, "空回调不入队");
}

// ── 探针：在 build 期间访问子代 ─────────────────────────────────────────────
class BuildVisitorState;

class BuildVisitor : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
};

class BuildVisitorState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        duringBuildCount = 0;
        context.visitChildElements([&](Element*) { ++duringBuildCount; });
        return cui::make_shared<SizedBox>(20, 20);
    }
    int duringBuildCount = -1;
};

inline std::unique_ptr<State> BuildVisitor::createState() {
    return std::make_unique<BuildVisitorState>();
}

// ── 测试 4：visitChildElements / visitAncestorElements ──────────────────────
void testElementVisitors() {
    std::printf("[test] BuildContext::visitChildElements / visitAncestorElements\n");

    // 多子节点：根的 visitChildElements 收到 3 个直接子 Element。
    {
        Harness h;
        auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
                cui::make_shared<SizedBox>(10, 10), cui::make_shared<SizedBox>(10, 10),
                cui::make_shared<SizedBox>(10, 10)});
        h.attach(row, 200, 200);
        int count = 0;
        h.root->visitChildElements([&](Element*) { ++count; });
        check(count == 3, "RenderObjectWidget 根：访问到 3 个直接子");

        // 子 → 父链：Row 的子是 LeafRenderObjectElement，往上 1 级是 Row。
        Element* child = nullptr;
        h.root->visitChildElements([&](Element* e) {
            if (!child)
                child = e;
        });
        check(child && child->parent() == h.root, "子的 parent 是根");
        int ancestors = 0;
        bool reachedTop = child->visitAncestorElements([&](Element*) {
            ++ancestors;
            return true;
        });
        check(ancestors == 1 && reachedTop, "visitAncestorElements：一路走到根 ⇒ 返回 true");
        int seen = 0;
        bool stopped = child->visitAncestorElements([&](Element*) {
            ++seen;
            return false;
        });
        check(seen == 1 && !stopped, "回调返回 false ⇒ 停在中途且返回 false");
    }

    // 组件节点：StatefulWidget 的 visitChildElements 是 O(1)（只有一个子）。
    {
        Harness h;
        auto visitor = cui::make_shared<BuildVisitor>();
        h.attach(visitor, 200, 200);
        auto* s = static_cast<BuildVisitorState*>(
                static_cast<StatefulElement*>(h.root)->state());
        check(s && s->duringBuildCount == 0,
              "build 期间访问子代：上游 断言的场景，本仓直接不执行");
        int count = 0;
        s->context()->visitChildElements([&](Element*) { ++count; });
        check(count == 1, "build 之后：ComponentElement 恰好 1 个子");
    }
}

// ── 测试 5：getTransformTo / 带祖先的坐标换算 ───────────────────────────────
void testGetTransformTo() {
    std::printf("[test] RenderObject::getTransformTo / localToGlobal(ancestor)\n");

    Harness h;
    // 同心两层 Padding：外层偏移 (10,20) 内层 (5,7)，最里 100×50 的盒子。
    // 外包 Align(0,0)：RenderView 给的是窗口大小的紧约束，直接外包 SizedBox
    // 会被顶满；Align 松开约束让 300×300 的外框成立，并把它居中到 (50,50)，
    // 于是每一层的全局位置互不相同，祖先换算才测得出来。
    auto leaf = cui::make_shared<SizedBox>(100, 50, cui::make_shared<DecoratedBox>(
                                                     BoxDecoration{}));
    auto inner = cui::make_shared<Padding>(EdgeInsets{5, 7, 0, 0}, leaf);
    auto outer = cui::make_shared<Padding>(EdgeInsets{10, 20, 0, 0}, inner);
    auto frame = cui::make_shared<SizedBox>(300, 300, outer);
    h.attach(cui::make_shared<Align>(0.0, 0.0, frame), 400, 400);

    auto paddings = findAllRo<RenderPadding>(h.root->findRenderObject());
    check(paddings.size() == 2, "找到两层 RenderPadding");
    auto decorated = findAllRo<RenderDecoratedBox>(h.root->findRenderObject());
    check(decorated.size() == 1, "找到最里的 RenderDecoratedBox");
    if (paddings.size() != 2 || decorated.size() != 1)
        return;
    RenderPadding* outerRo = paddings[0];
    RenderPadding* innerRo = paddings[1];
    RenderObject* leafRo = decorated.front();

    // 各层全局位置（居中 50,50 + 两层 padding 偏移）。
    check(outerRo->localToGlobal() == Offset(50, 50), "外层 Padding 在全局 (50, 50)");
    check(innerRo->localToGlobal() == Offset(60, 70), "内层 Padding 在全局 (60, 70)");

    // 1 参版本：既有口径（累加父链偏移）不变。
    Offset global = leafRo->localToGlobal();
    check(global == Offset(65, 77), "localToGlobal()：三层偏移累加 (65, 77)");

    // 2 参版本与 1 参版本对同一目标（根）结果一致。
    Offset viaTransform = leafRo->localToGlobal(Offset(0, 0), nullptr);
    check(viaTransform == global, "localToGlobal(local, nullptr) 与 1 参版本同口径");

    // 指定祖先：换算结果只含「祖先以下」的偏移，与树在窗口里的位置无关。
    Offset inOuter = leafRo->localToGlobal(Offset(0, 0), outerRo);
    check(inOuter == Offset(15, 27), "换算到外层 Padding ⇒ 两层 padding 相加 (15, 27)");
    Offset inInner = leafRo->localToGlobal(Offset(0, 0), innerRo);
    check(inInner == Offset(5, 7), "换算到内层 Padding ⇒ 只剩它的偏移 (5, 7)");

    // 逆变换一致。
    Offset back =
            leafRo->globalToLocal(leafRo->localToGlobal(Offset(3, 4), outerRo), outerRo);
    check(back == Offset(3, 4), "globalToLocal(…, ancestor) 是 2 参 localToGlobal 的逆");

    // 自身到自身：单位矩阵。
    Matrix4 self = leafRo->getTransformTo(leafRo);
    check(self.isIdentity(), "getTransformTo(self) = 单位矩阵");
    Matrix4 toOuter = leafRo->getTransformTo(outerRo);
    check(toOuter.transformOffset(Offset(0, 0)) == Offset(15, 27), "getTransformTo(outer) 平移 (15, 27)");
    Matrix4 toSibling = leafRo->getTransformTo(innerRo->child());
    check(toSibling.transformOffset(Offset(0, 0)) == Offset(0, 0),
          "经由共同祖先合成：SizedBox 与 DecoratedBox 同点 ⇒ 净平移为 0");

    // 非同一棵树：返回单位矩阵而不是崩（上游 抛异常）。
    RenderProxyBox orphan;
    Matrix4 detached = orphan.getTransformTo(leafRo);
    check(detached.isIdentity(), "孤立节点：getTransformTo 给单位矩阵（上游 抛异常）");
}

}  // namespace

int main() {
    std::printf("=== test_ticker_mode ===\n");
    testTickerMuting();
    testTickerMode();
    testPostFrameCallbacks();
    testElementVisitors();
    testGetTransformTo();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
