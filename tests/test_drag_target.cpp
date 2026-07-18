/// @file test_drag_target.cpp
/// @brief Draggable / DragTarget / LongPressDraggable 单元测试。对齐上游
///        widgets/drag_target.dart（`_DraggableState` / `_DragAvatar` /
///        `_DragTargetState`）+ rendering/proxy_box.dart 的 `RenderMetaData`。
///
/// 覆盖（口径以上游为准）：
///   1. 按下即起拖（= 上游 ImmediateMultiDrag）：onDragStarted、avatar 挂上
///      Overlay 条目、childWhenDragging 生效（盒子尺寸换掉）。
///   2. 进入目标：`didEnter` 调 onWillAcceptWithDetails，builder 拿到
///      candidateData=[7]（= 上游 `List<T?>`）。
///   3. 目标内继续移动：走 listsMatch 短路 ⇒ 只报 didMove，不重复 willAccept。
///   4. 离开目标：onLeave(data) + candidateData 清空。
///   5. 抬手在目标上：onAcceptWithDetails（data + 「avatar 位置 − dragStartPoint」
///      的 offset，上游同款 lastOffset）、wasAccepted ⇒ onDragCompleted、条目
///      摘掉、child 恢复。
///   6. 抬手在空白：!wasAccepted ⇒ onDraggableCanceled，不发 onDragCompleted。
///   7. 类型过滤：`std::any` 的 typeid ≠ T ⇒ 目标根本不认领（willAccept 不调、
///      候选为空）⇒ 抬手取消。
///   8. willAccept 返回 false ⇒ 进 rejectedAvatars（builder 的第二列）。
///   9. maxSimultaneousDrags=1 ⇒ 第二根指针被忽略。
///  10. axis=Horizontal ⇒ avatar 自己夹位移（每次 onDragUpdate 的 delta.dy=0）。
///  11. LongPressDraggable：延时未到不起拖；到点（Ticker）才起拖。
///  12. 目标在拖拽中被摘掉 ⇒ avatar 忘掉它（liveAvatars 反向通知），抬手仍能
///      正常收尾。真牙齿在 ASan 树：没有反向通知就是 heap-use-after-free。
///  13. 源在拖拽中被摘掉 ⇒ `~DragAvatar` 兜底摘掉 Overlay 条目，不回调用户。
///
/// headless 帧泵（flushBuildScope + flushLayout），不做 flushPaint。

#include <any>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/framework/listener.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/drag_target.h"
#include "cui/widgets/overlay.h"

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

bool nearEq(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

/// 一次拖拽实验里所有回调的落点。
struct Events {
    std::vector<DragUpdateDetails> updates;

    int dragStarted = 0;
    int dragCompleted = 0;
    int dragCanceled = 0;
    std::optional<DraggableDetails> lastDetails;
    int willAcceptCalls = 0;
    int acceptCalls = 0;
    int moveCalls = 0;
    int leaveCalls = 0;
    std::optional<int> acceptedData;
    Offset acceptedOffset{};
    std::optional<int> leftData;
};

/// 被测子树的开关与探针（State 在 build 里回填 overlay / refresh / 候选集合）。
struct Fixture {
    bool showTarget = true;
    bool showSource = true;
    bool longPress = false;
    bool stringPayload = false;
    bool axisHorizontal = false;
    bool maxOneDrag = false;
    bool alwaysReject = false;
    Duration delay = Duration(10);

    int builds = 0;
    std::function<void()> refresh;
    OverlayState* overlay = nullptr;

    /// 目标每次 build 看到的候选/拒绝集合。
    std::vector<int> candidateSizes;
    std::vector<int> rejectedSizes;
    std::vector<int> candidateValues;
};

/// 一块可命中、可辨识的着色盒（Container ⇒ RenderDecoratedBox 覆写
/// hitTestSelf，所以它是「能被命中的叶子」）。
SharedPtr<Widget> tintedBox(double w, double h, Color c) {
    auto box = cui::make_shared<Container>();
    box->width(w).height(h).decoration(BoxDecoration{c});
    return box;
}

SharedPtr<Widget> at(double left, double top, SharedPtr<Widget> child) {
    auto pos = cui::make_shared<Positioned>(std::move(child));
    pos->hasLeft = true;
    pos->left = left;
    pos->hasTop = true;
    pos->top = top;
    return pos;
}

/// int 目标的 builder：记录候选/拒绝集合，并按状态换色。
SharedPtr<Widget> makeIntTarget(Fixture& fx, Events& ev) {
    return cui::make_shared<DragTarget<int>>(
        [&fx](BuildContext&, const std::vector<std::optional<int>>& cand,
              const std::vector<std::any>& rejected) {
            fx.candidateSizes.push_back(static_cast<int>(cand.size()));
            fx.rejectedSizes.push_back(static_cast<int>(rejected.size()));
            fx.candidateValues.push_back(
                cand.empty() || !cand[0].has_value() ? -1 : *cand[0]);
            const bool hot = !cand.empty();
            const bool cold = !rejected.empty();
            return tintedBox(120.0, 60.0,
                             hot ? Color::fromRGB(0.2, 0.8, 0.3)
                                 : (cold ? Color::fromRGB(0.9, 0.4, 0.2)
                                         : Color::fromRGB(0.6, 0.6, 0.7)));
        },
        [&fx, &ev](const DragTargetDetails<int>&) {
            ++ev.willAcceptCalls;
            return !fx.alwaysReject;
        },
        [&ev](const DragTargetDetails<int>& d) {
            ++ev.acceptCalls;
            ev.acceptedData = d.data;
            ev.acceptedOffset = d.offset;
        },
        [&ev](const std::optional<int>& data) {
            ++ev.leaveCalls;
            ev.leftData = data;
        },
        [&ev](const DragTargetDetails<int>&) { ++ev.moveCalls; });
}

/// 拖拽源：盒子在 (20,20) 起 120×60；childWhenDragging 用 90×40 的空位。
SharedPtr<Widget> makeSource(Fixture& fx, Events& ev) {
    std::any payload = fx.stringPayload ? std::any(std::string("abc")) : std::any(7);
    auto child = tintedBox(120.0, 60.0, Color::fromRGB(0.2, 0.4, 0.9));
    auto placeholder = tintedBox(90.0, 40.0, Color::fromRGB(0.95, 0.95, 0.95));
    auto feedback = tintedBox(120.0, 60.0, Color::fromRGB(0.9, 0.8, 0.2));

    std::optional<Axis> axis;
    if (fx.axisHorizontal)
        axis = Axis::Horizontal;
    std::optional<int> maxDrags;
    if (fx.maxOneDrag)
        maxDrags = 1;

    SharedPtr<Draggable> src = cui::make_shared<Draggable>(
        child, feedback, std::move(payload), placeholder, axis, Offset(0, 0),
        childDragAnchorStrategy, maxDrags,
        [&ev] { ++ev.dragStarted; }, [&ev](const DragUpdateDetails& details) {
            ev.updates.push_back(details);
        },
        [&ev](const Velocity&, const Offset&) { ++ev.dragCanceled; },
        [&ev](const DraggableDetails& d) { ev.lastDetails = d; },
        [&ev] { ++ev.dragCompleted; });
    return src;
}

/// 拖拽源（长按版）。
SharedPtr<Widget> makeLongPressSource(Fixture& fx, Events& ev) {
    auto src = cui::make_shared<LongPressDraggable>(
        tintedBox(120.0, 60.0, Color::fromRGB(0.2, 0.4, 0.9)),
        tintedBox(120.0, 60.0, Color::fromRGB(0.9, 0.8, 0.2)), std::any(7),
        tintedBox(90.0, 40.0, Color::fromRGB(0.95, 0.95, 0.95)), std::optional<Axis>{},
        Offset(0, 0), childDragAnchorStrategy, std::optional<int>{},
        [&ev] { ++ev.dragStarted; },
        [&ev](const DragUpdateDetails& details) { ev.updates.push_back(details); },
        [&ev](const Velocity&, const Offset&) { ++ev.dragCanceled; },
        [&ev](const DraggableDetails& d) { ev.lastDetails = d; },
        [&ev] { ++ev.dragCompleted; });
    src->setDelay(fx.delay);
    return src;
}

/// 被测子树的宿主：Overlay 的一条初始条目里放一个 Stack（源 + 目标）。
class FixtureWidget : public StatefulWidget {
public:
    FixtureWidget(Fixture& fx, Events& ev, bool longPressSource, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), fx_(&fx), ev_(&ev),
          longPressSource_(longPressSource) {}

    Fixture& fx() const { return *fx_; }
    Events& ev() const { return *ev_; }
    bool longPressSource() const { return longPressSource_; }

    std::unique_ptr<State> createState() override;

private:
    Fixture* fx_;
    Events* ev_;
    bool longPressSource_;
};

class FixtureState : public State {
public:
    void initState() override {
        State::initState();
        auto* self = this;
        fixture().refresh = [self] { self->setState(); };
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        auto& host = static_cast<FixtureWidget&>(*widget());
        fixture().overlay = OverlayState::maybeOf(context);
        ++fixture().builds;

        std::vector<SharedPtr<Widget>> kids;
        if (fixture().showSource) {
            auto src = host.longPressSource() ? makeLongPressSource(fixture(), event())
                                              : makeSource(fixture(), event());
            kids.push_back(at(20.0, 20.0, std::move(src)));
        }
        if (fixture().showTarget)
            kids.push_back(at(300.0, 20.0, makeIntTarget(fixture(), event())));
        return cui::make_shared<Stack>(std::move(kids));
    }

    void dispose() override {
        fixture().refresh = nullptr;
        State::dispose();
    }

private:
    Fixture& fixture() { return static_cast<FixtureWidget*>(widget())->fx(); }
    Events& event() { return static_cast<FixtureWidget*>(widget())->ev(); }
};

inline std::unique_ptr<State> FixtureWidget::createState() {
    return std::make_unique<FixtureState>();
}

// ── 测试 harness ────────────────────────────────────────────────────────────

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        pump();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
        // 上游一帧的收尾：被 setState 摘掉的子树先 deactivate，真正的 dispose 在
        // finalizeTree 里做（Application 每帧都调）。§12/§13 钉的正是
        // 「拖拽中途 dispose」，少了这一步就只是在测 deactivate。
        owner.finalizeTree();
    }

    /// 只跑帧泵里的 tick（LongPress 的延时判定靠真实时钟，见 Ticker::tick）。
    void tick() {
        Ticker::tickAllActive(Duration(16));
        pump();
    }

    void down(double x, double y, int64_t pointerId = 1) {
        PointerDownEvent e;
        e.position = {x, y};
        e.pointerId = pointerId;
        e.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(e);
        pump();
    }
    void drag(double x, double y, int64_t pointerId = 1) {
        PointerMoveEvent e;
        e.position = {x, y};
        e.pointerId = pointerId;
        e.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(e);
        pump();
    }
    void up(double x, double y, int64_t pointerId = 1) {
        PointerUpEvent e;
        e.position = {x, y};
        e.pointerId = pointerId;
        po.dispatchPointerEvent(e);
        pump();
    }
};

// ── 用例 ────────────────────────────────────────────────────────────────────

/// 重新装一棵树（每个用例一份干净的开关/日志）。
struct Scene {
    Fixture fx;
    Events ev;
    SharedPtr<OverlayEntry> entry;
    Harness h;

    void build() {
        entry = cui::make_shared<OverlayEntry>([this](BuildContext&) {
            return cui::make_shared<FixtureWidget>(fx, ev, fx.longPress);
        });
        std::vector<SharedPtr<OverlayEntry>> initial;
        initial.push_back(entry);
        h.attach(cui::make_shared<Overlay>(std::move(initial)), 600.0, 400.0);
    }

    /// 源那条子树的最外层盒子（Draggable 的 Listener）。
    RenderBox* sourceBox() {
        auto* root = h.root ? h.root->findRenderObject() : nullptr;
        return root ? findSourceBox(root) : nullptr;
    }

    static RenderBox* findSourceBox(RenderObject* node) {
        if (auto* box = dynamic_cast<RenderPointerListener*>(node))
            return box;
        RenderBox* found = nullptr;
        node->visitChildren([&](AbstractNode* child) {
            if (found)
                return;
            found = findSourceBox(static_cast<RenderObject*>(child));
        });
        return found;
    }

    int overlayEntries() {
        return fx.overlay ? static_cast<int>(fx.overlay->entries().size()) : -1;
    }
};

void sectionDragAndDrop() {
    std::printf("== 1..6 起拖 / 进入 / 移动 / 离开 / 放下 / 取消 ==\n");
    Scene s;
    s.build();
    check(s.overlayEntries() == 1, "初始只有一条 Overlay 条目");

    // 1. 按下即起拖
    s.h.down(80.0, 50.0);
    check(s.ev.dragStarted == 1, "onDragStarted 触发一次（ImmediateMultiDrag）");
    check(s.overlayEntries() == 2, "反馈盒子挂上了第二条 Overlay 条目");

    auto* srcBox = s.sourceBox();
    check(srcBox != nullptr, "渲染树里能找到源的 Listener 盒子");
    if (srcBox)
        check(nearEq(srcBox->size().width, 90.0) && nearEq(srcBox->size().height, 40.0),
              "childWhenDragging 生效：源盒子换成 90×40");

    // 2. 进入目标（目标 = (300,20) 起 120×60）
    s.h.drag(360.0, 50.0);
    check(s.ev.willAcceptCalls == 1, "进入目标时问了一次 onWillAcceptWithDetails");
    check(!s.fx.candidateSizes.empty() && s.fx.candidateSizes.back() == 1,
          "builder 的 candidateData 长度变 1");
    check(!s.fx.candidateValues.empty() && s.fx.candidateValues.back() == 7,
          "candidateData[0] == 7（DragTarget<int> 收到的载荷）");

    // 3. 目标内继续移动 ⇒ 只报 move
    const int movesBefore = s.ev.moveCalls;
    s.h.drag(370.0, 55.0);
    check(s.ev.willAcceptCalls == 1, "同一段目标里继续移动不再重复 willAccept（listsMatch 短路）");
    check(s.ev.moveCalls == movesBefore + 1, "didMove 每次都报（onMove 被调到）");

    // 4. 离开目标
    s.h.drag(500.0, 300.0);
    check(s.ev.leaveCalls == 1, "离开目标时 onLeave 触发");
    check(s.ev.leftData.has_value() && *s.ev.leftData == 7, "onLeave 带上原载荷 7");
    check(!s.fx.candidateSizes.empty() && s.fx.candidateSizes.back() == 0,
          "离开后 candidateData 清空");

    // 5. 回到目标并抬手 ⇒ accept
    s.h.drag(360.0, 50.0);
    check(s.ev.willAcceptCalls == 2, "重新进入再问一次 willAccept");
    s.h.up(360.0, 50.0);
    check(s.ev.acceptCalls == 1, "onAcceptWithDetails 触发");
    check(s.ev.acceptedData.has_value() && *s.ev.acceptedData == 7, "accepted 的 data == 7");
    // offset = 上游的 `_lastOffset` = avatar 位置 − dragStartPoint。
    // childDragAnchorStrategy 给的是按下点在源盒子里的本地坐标
    // = (80-20, 50-20) = (60,30) ⇒ (360,50)-(60,30) = (300,20)。
    check(nearEq(s.ev.acceptedOffset.dx, 300.0) && nearEq(s.ev.acceptedOffset.dy, 20.0),
          "accepted 的 offset == 位置 − dragStartPoint == (300,20)");
    check(s.ev.dragCompleted == 1, "wasAccepted ⇒ onDragCompleted");
    check(s.ev.dragCanceled == 0, "accepted ⇒ 不发 onDraggableCanceled");
    check(s.overlayEntries() == 1, "抬手后反馈条目被摘掉");
    if (auto* b = s.sourceBox())
        check(nearEq(b->size().width, 120.0), "child 恢复成 120 宽");

    // 6. 拖到空白抬手 ⇒ canceled
    s.h.down(80.0, 50.0);
    s.h.drag(500.0, 300.0);
    s.h.up(500.0, 300.0);
    check(s.ev.dragCanceled == 1, "空白处抬手 ⇒ onDraggableCanceled");
    check(s.ev.dragCompleted == 1, "取消时不补发 onDragCompleted");
    check(s.ev.lastDetails.has_value() && !s.ev.lastDetails->wasAccepted,
          "DraggableDetails.wasAccepted == false");
    check(s.ev.lastDetails && nearEq(s.ev.lastDetails->velocity.pixelsPerSecond.dx, 0.0),
          "velocity 恒 0（本仓无速度估计器）");
}

void sectionTypeFilterAndReject() {
    std::printf("== 7..8 类型过滤 / willAccept 拒绝 ==\n");

    {
        Scene s;
        s.fx.stringPayload = true;
        s.build();
        s.h.down(80.0, 50.0);
        s.h.drag(360.0, 50.0);
        check(s.ev.willAcceptCalls == 0, "std::string 载荷不进 int 目标的类型过滤（不调 willAccept）");
        check(!s.fx.candidateSizes.empty() && s.fx.candidateSizes.back() == 0,
              "int 目标的 candidateData 始终为空");
        s.h.up(360.0, 50.0);
        check(s.ev.acceptCalls == 0, "类型不匹配 ⇒ 不 accept");
        check(s.ev.dragCanceled == 1, "类型不匹配 ⇒ 抬手算取消");
    }

    {
        Scene s;
        s.fx.alwaysReject = true;
        s.build();
        s.h.down(80.0, 50.0);
        s.h.drag(360.0, 50.0);
        check(s.ev.willAcceptCalls == 1, "拒绝路径仍然问 willAccept");
        check(!s.fx.rejectedSizes.empty() && s.fx.rejectedSizes.back() == 1,
              "被拒的 avatar 进 rejectedData（builder 的第二列）");
        check(!s.fx.candidateSizes.empty() && s.fx.candidateSizes.back() == 0,
              "被拒 ⇒ 不进 candidateData");
        s.h.up(360.0, 50.0);
        check(s.ev.acceptCalls == 0 && s.ev.dragCanceled == 1,
              "拒绝后抬手 ⇒ 取消，不 accept");
    }
}

void sectionLimitsAndAxis() {
    std::printf("== 9..10 maxSimultaneousDrags / axis ==\n");

    {
        Scene s;
        s.fx.maxOneDrag = true;
        s.build();
        s.h.down(80.0, 50.0, /*pointerId=*/1);
        check(s.ev.dragStarted == 1, "第一根指针起拖");
        check(s.overlayEntries() == 2, "第一条反馈挂上 Overlay");
        // 第二根指针落在缩成 90×40 的源盒子内（(60,40) → 本地 (40,20)）
        s.h.down(60.0, 40.0, /*pointerId=*/2);
        check(s.ev.dragStarted == 1, "maxSimultaneousDrags=1 ⇒ 第二根指针被忽略");
        check(s.overlayEntries() == 2, "第二根指针没有挂新条目");
        s.h.up(60.0, 40.0, /*pointerId=*/2);
        s.h.up(80.0, 50.0, /*pointerId=*/1);
        check(s.ev.dragCanceled == 1, "第一根指针抬手正常收尾");

        // 结束后可以再拖一次（activeCount 归零）
        s.h.down(80.0, 50.0, /*pointerId=*/3);
        check(s.ev.dragStarted == 2, "上一段结束后第三根指针又能起拖");
        s.h.up(80.0, 50.0, /*pointerId=*/3);
    }

    {
        Scene s;
        s.fx.axisHorizontal = true;
        s.build();
        s.h.down(80.0, 50.0);
        s.h.drag(200.0, 300.0);
        s.h.drag(360.0, 50.0);
        bool allDyZero = !s.ev.updates.empty();
        bool anyMoved = false;
        for (const auto& d : s.ev.updates) {
            if (!nearEq(d.delta.dy, 0.0))
                allDyZero = false;
            if (d.delta.dx != 0.0)
                anyMoved = true;
        }
        check(allDyZero, "axis=Horizontal ⇒ 每次 onDragUpdate 的 delta.dy 被夹成 0");
        check(anyMoved, "axis=Horizontal ⇒ 水平位移照常上报");
        // y 一路被夹住 ⇒ avatar 始终停在 y=50，落点 (360,50) 仍在目标里。
        check(!s.ev.updates.empty() && nearEq(s.ev.updates.back().globalPosition.dy, 50.0),
              "axis=Horizontal ⇒ avatar 的 y 恒等于按下时的 50");
        check(s.ev.willAcceptCalls == 1, "锁横轴后 (360,50) 仍命中目标（y 被夹回 50）");
        s.h.up(360.0, 50.0);
    }
}

void sectionLongPress() {
    std::printf("== 11 LongPressDraggable 延时 ==\n");
    Scene s;
    s.fx.longPress = true;
    s.fx.delay = Duration(30);
    s.build();

    s.h.down(80.0, 50.0);
    check(s.ev.dragStarted == 0, "按下后延时未到 ⇒ 还没起拖");
    check(s.overlayEntries() == 1, "延时期间没有挂反馈条目");

    // 延时期间的移动只更新「按下点」，仍不起拖
    s.h.drag(120.0, 90.0);
    check(s.ev.dragStarted == 0, "延时内移动也不起拖");

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    s.h.tick();
    check(s.ev.dragStarted == 1, "Ticker 到点起拖（DelayedMultiDrag 的定时器）");
    check(s.overlayEntries() == 2, "起拖后挂上反馈条目");

    s.h.up(120.0, 90.0);
    check(s.ev.dragCanceled == 1, "抬手收尾");

    // 抬手在延时之内 ⇒ 根本不产生拖拽
    Scene s2;
    s2.fx.longPress = true;
    s2.fx.delay = Duration(5000);
    s2.build();
    s2.h.down(80.0, 50.0);
    s2.h.up(80.0, 50.0);
    s2.h.tick();
    check(s2.ev.dragStarted == 0, "抬手早于延时 ⇒ 定时器不触发拖拽");
    check(s2.overlayEntries() == 1, "延时中被抬手 ⇒ 没有残留条目");
}

void sectionTeardown() {
    std::printf("== 12..13 拖拽中途拆除 ==\n");

    {
        Scene s;
        s.build();
        s.h.down(80.0, 50.0);
        s.h.drag(360.0, 50.0);
        check(s.ev.willAcceptCalls == 1, "拆之前目标已进入");

        // 摘掉目标：avatar 必须通过 liveAvatars 反向忘掉它（ASan 的牙齿）
        s.fx.showTarget = false;
        s.fx.refresh();
        s.h.pump();
        check(!s.fx.candidateSizes.empty() && s.fx.candidateSizes.back() == 1,
              "目标销毁前的最后一次候选仍是 1");

        s.h.drag(400.0, 60.0);
        check(s.ev.leaveCalls == 0, "目标销毁 ⇒ 不补发 onLeave（widget 已不在）");
        s.h.up(400.0, 60.0);
        check(s.ev.acceptCalls == 0, "目标销毁后抬手 ⇒ 不 accept");
        check(s.ev.dragCanceled == 1, "目标销毁后抬手算取消");
        check(s.overlayEntries() == 1, "反馈条目已摘掉");
    }

    {
        Scene s;
        s.build();
        s.h.down(80.0, 50.0);
        check(s.overlayEntries() == 2, "起拖挂上条目");

        // 摘掉源：~DragAvatar 兜底摘条目，且不回调用户
        s.fx.showSource = false;
        s.fx.refresh();
        s.h.pump();
        s.h.pump();
        check(s.overlayEntries() == 1, "源销毁 ⇒ ~DragAvatar 摘掉 Overlay 条目");
        check(s.ev.dragCanceled == 0 && s.ev.dragCompleted == 0,
              "源销毁 ⇒ 不触发 onDraggableCanceled / onDragCompleted");
        check(s.ev.lastDetails == std::nullopt, "源销毁 ⇒ 不触发 onDragEnd");

        // 之后的事件无处可去，但不能再崩
        s.h.drag(360.0, 50.0);
        s.h.up(360.0, 50.0);
        check(s.overlayEntries() == 1, "收尾后 Overlay 仍只有初始条目");
    }
}

}  // namespace

int main() {
    sectionDragAndDrop();
    sectionTypeFilterAndReject();
    sectionLimitsAndAxis();
    sectionLongPress();
    sectionTeardown();

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
