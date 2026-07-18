/// @file test_notification.cpp
/// @brief 通知框架单元测试。对齐 上游 widgets/notification_listener.dart +
///        widgets/scroll_notification.dart 的可观察语义。
///
/// 覆盖：
///   1. 冒泡起点：dispatch 从目标的**父链**开始（自己不是监听者），顺序为
///      自下而上；从根 Element dispatch 时无人可收。
///   2. 中断：回调返回 true 终止该条通知的冒泡，false 继续。
///   3. 类型过滤：typed<T> 只回应 T 及其子类（dynamic_cast 复刻 Dart 泛型），
///      不匹配的通知原样放行。
///   4. 视口 depth：上游 ViewportElementMixin 的计数 —— 盒子视口
///      （SingleChildViewport）与 sliver 视口（Viewport）各加一，嵌套两层得 2。
///   5. Scrollable 手势节奏：Start → UserScroll → Update → …（方向变化再发
///      UserScroll）→ End → UserScroll(idle)；监听者在 Scrollable 之下收不到。
///   6. Overscroll：滚轮「从界内冲出边界」的剩余量照发；已经在边界上再往外推、
///      或 physics 不接受用户滚动 ⇒ 上游的认领判据（硬夹后偏移没变）不成立，
///      这一整次滚轮一条通知都不发，增量留给祖先滚动视图（嵌套用例见 test_scroll.cpp §9）。
///   7. metrics 快照：pixels / maxScrollExtent / viewportExtent / axis。
///   8. 键盘滚动：键位落位发 UserScroll → Update → End → idle；同一次手势内
///      （按下后未抬手）不重复 Start，End 之后复位；越界键位补 Overscroll；
///      未映射键位不发通知。

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/focus.h"
#include "cui/framework/framework.h"
#include "cui/platform/event.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/notification.h"
#include "cui/widgets/scroll.h"
#include "cui/widgets/scroll_notification.h"
#include "cui/widgets/scroll_physics.h"
#include "cui/widgets/sliver.h"

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

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

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

    void dispatch(const PointerEvent& ev) { po.dispatchPointerEvent(ev); }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }
};

PointerDownEvent downAt(double x, double y) {
    PointerDownEvent e;
    e.position = {x, y};
    e.buttons = kPrimaryMouseButton;
    return e;
}
PointerMoveEvent moveAt(double x, double y) {
    PointerMoveEvent e;
    e.position = {x, y};
    e.buttons = kPrimaryMouseButton;
    return e;
}
PointerUpEvent upAt(double x, double y) {
    PointerUpEvent e;
    e.position = {x, y};
    return e;
}
PointerScrollEvent wheelAt(double x, double y, double dy) {
    PointerScrollEvent e;
    e.position = {x, y};
    e.scrollDeltaY = dy;
    return e;
}

/// 记下自己的 BuildContext，测试据此从子树内部发起 dispatch。
/// 上游测试用 Builder((context) => ...) 拿 context，这里是同一招。
class ContextProbe : public StatelessWidget {
public:
    explicit ContextProbe(SharedPtr<Widget> child, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)) {}

    BuildContext* captured() const { return captured_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        captured_ = &context;
        return child_;
    }

private:
    SharedPtr<Widget> child_;
    BuildContext* captured_ = nullptr;
};

std::string dirName(ScrollDirection d) {
    switch (d) {
        case ScrollDirection::Idle: return "idle";
        case ScrollDirection::Forward: return "forward";
        case ScrollDirection::Reverse: return "reverse";
    }
    return "?";
}

/// 一条通知的观测结果。
struct Note {
    std::string name;
    int depth = 0;
    double value = 0.0;
    std::string dir = "idle";

    std::string toString() const {
        std::string s = name + "@depth" + std::to_string(depth);
        if (name == "UserScrollNotification") s += "/" + dir;
        if (name == "ScrollUpdateNotification" || name == "OverscrollNotification")
            s += "/" + std::to_string(static_cast<int>(value));
        return s;
    }
};

std::string seqToString(const std::vector<Note>& notes) {
    std::string out;
    for (const auto& n : notes) {
        if (!out.empty()) out += " → ";
        out += n.toString();
    }
    return out;
}

void dump(const std::string& title, const std::vector<Note>& notes) {
    std::printf("    %s: %s\n", title.c_str(), seqToString(notes).c_str());
}

/// 通用 recorder：把收到的通知摊平成 Note 序列。`ret` 是回调返回值。
std::function<bool(Notification&)> recorder(std::vector<Note>& out, bool ret) {
    return [&out, ret](Notification& n) {
        Note note;
        note.name = n.toString();
        auto* vp = dynamic_cast<ViewportNotification*>(&n);
        note.depth = vp ? vp->depth() : 0;
        if (auto* u = dynamic_cast<UserScrollNotification*>(&n)) note.dir = dirName(u->direction);
        if (auto* up = dynamic_cast<ScrollUpdateNotification*>(&n))
            note.value = up->scrollDelta.value_or(0.0);
        if (auto* o = dynamic_cast<OverscrollNotification*>(&n)) note.value = o->overscroll;
        out.push_back(note);
        return ret;
    };
}

SharedPtr<Widget> columnOfRows(double rowHeight, int rows) {
    std::vector<SharedPtr<Widget>> items;
    for (int i = 0; i < rows; ++i)
        items.push_back(cui::make_shared<SizedBox>(0.0, rowHeight));
    return cui::make_shared<Column>(items);
}

// ── 测试 1：冒泡起点与顺序 ──────────────────────────────────────────────────
void testBubbling() {
    std::printf("[test] 冒泡起点与顺序\n");

    std::vector<Note> outer;
    std::vector<Note> inner;
    auto probe = cui::make_shared<ContextProbe>(cui::make_shared<SizedBox>(100.0, 100.0));
    auto innerListener = cui::make_shared<NotificationListener>(
        probe, recorder(inner, false), nullptr);
    auto outerListener = cui::make_shared<NotificationListener>(
        innerListener, recorder(outer, false), nullptr);

    Harness h;
    h.attach(outerListener, 200, 200);

    // probe 的父链：ContextProbeElement → innerListener → outerListener → root。
    LayoutChangedNotification n;
    const bool handled = n.dispatch(probe->captured());
    dump("inner", inner);
    dump("outer", outer);
    check(inner.size() == 1 && inner[0].name == "LayoutChangedNotification",
          "最近的上游监听者收到");
    check(outer.size() == 1, "再上游的监听者也收到");
    check(!handled, "回调都返回 false ⇒ dispatch 返回 false");

    // dispatch 从目标的父链开始：probe 自己不是监听者，从 probe 的父 Element
    // 出发；反过来，从根 Element dispatch 时链上无人（root 没有父）。
    inner.clear();
    outer.clear();
    check(!n.dispatch(h.root), "从根 Element dispatch 无人可收");
    check(inner.empty() && outer.empty(), "根 dispatch 不回调任何监听者");
}

// ── 测试 2：返回 true 中断冒泡 ──────────────────────────────────────────────
void testStopBubbling() {
    std::printf("[test] 返回值中断冒泡\n");

    std::vector<Note> outer;
    std::vector<Note> inner;
    auto probe = cui::make_shared<ContextProbe>(cui::make_shared<SizedBox>(100.0, 100.0));
    auto innerListener = cui::make_shared<NotificationListener>(probe, recorder(inner, true));
    auto outerListener =
        cui::make_shared<NotificationListener>(innerListener, recorder(outer, false));

    Harness h;
    h.attach(outerListener, 200, 200);

    LayoutChangedNotification n;
    const bool handled = n.dispatch(probe->captured());
    check(inner.size() == 1 && outer.empty(), "内层返回 true ⇒ 外层收不到");
    check(handled, "dispatch 透出「已中断」");
}

// ── 测试 3：typed<T> 类型过滤 ───────────────────────────────────────────────
void testTypeFilter() {
    std::printf("[test] typed<T> 类型过滤\n");

    std::vector<Note> updates;   // 只听 ScrollUpdateNotification
    std::vector<Note> all;       // 听整个 ScrollNotification 家族
    auto probe = cui::make_shared<ContextProbe>(cui::make_shared<SizedBox>(100.0, 100.0));

    auto onlyUpdate = NotificationListener::typed<ScrollUpdateNotification>(
        probe, [&updates](ScrollUpdateNotification& n) {
            updates.push_back(Note{n.toString(), n.depth(), n.scrollDelta.value_or(0.0), "idle"});
            return false;
        });
    auto allScroll = NotificationListener::typed<ScrollNotification>(
        onlyUpdate, [&all](ScrollNotification& n) {
            all.push_back(Note{n.toString(), n.depth(), 0.0, "idle"});
            return false;
        });

    Harness h;
    h.attach(allScroll, 200, 200);

    ScrollMetrics m;
    m.pixels = 20.0;
    m.maxScrollExtent = 300.0;
    ScrollStartNotification start(m, probe->captured());
    start.dispatch(probe->captured());
    check(updates.empty(), "typed<Update> 不回应 Start");
    check(all.size() == 1 && all[0].name == "ScrollStartNotification",
          "typed<ScrollNotification>（基类）回应派生类型");

    ScrollUpdateNotification update(m, probe->captured(), 15.0);
    update.dispatch(probe->captured());
    check(updates.size() == 1 && near(updates[0].value, 15.0), "typed<Update> 回应 Update");
    check(all.size() == 2, "不匹配的内层监听者原样放行");
}

// ── 测试 4：视口 depth ─────────────────────────────────────────────────────
void testViewportDepth() {
    std::printf("[test] 视口 depth 计数\n");

    // 4a：盒子模式（SingleChildScrollView → SingleChildViewport）。
    {
        std::vector<Note> seen;
        ScrollController c;
        auto probe = cui::make_shared<ContextProbe>(cui::make_shared<SizedBox>(0.0, 50.0));
        auto box = cui::make_shared<SingleChildScrollView>(&c, probe);
        auto listener = cui::make_shared<NotificationListener>(box, recorder(seen, false));
        Harness h;
        h.attach(listener, 200, 200);

        ScrollMetrics m;
        ScrollUpdateNotification n(m, probe->captured());
        n.dispatch(probe->captured());
        dump("4a", seen);
        check(seen.size() == 1 && seen[0].depth == 1, "穿过一个盒子视口 ⇒ depth 1");
    }

    // 4b：sliver 模式（CustomScrollView → Viewport）。
    {
        std::vector<Note> seen;
        ScrollController c;
        auto probe = cui::make_shared<ContextProbe>(cui::make_shared<SizedBox>(0.0, 50.0));
        std::vector<SharedPtr<Widget>> slivers = {
            cui::make_shared<SliverToBoxAdapter>(probe)};
        auto csv = cui::make_shared<CustomScrollView>(slivers, Axis::Vertical, false, &c);
        auto listener = cui::make_shared<NotificationListener>(csv, recorder(seen, false));
        Harness h;
        h.attach(listener, 200, 200);

        ScrollUpdateNotification n(ScrollMetrics{}, probe->captured());
        n.dispatch(probe->captured());
        dump("4b", seen);
        check(seen.size() == 1 && seen[0].depth == 1, "穿过一个 sliver 视口 ⇒ depth 1");
    }

    // 4c：嵌套两层滚动视图 —— 中间的监听者 depth 1，最外的 depth 2。
    // 这正是上游「depth == 0 才算最近的滚动视图」的由来。
    {
        std::vector<Note> middle;
        std::vector<Note> outer;
        ScrollController oc;
        ScrollController ic;
        auto probe = cui::make_shared<ContextProbe>(cui::make_shared<SizedBox>(0.0, 50.0));
        auto inner = cui::make_shared<SingleChildScrollView>(&ic, probe);
        auto midListener = cui::make_shared<NotificationListener>(inner, recorder(middle, false));
        // 外层内容：一个固定高度的内层滚动视图 + 若干 filler 行。
        std::vector<SharedPtr<Widget>> outerItems = {
            cui::make_shared<SizedBox>(0.0, 100.0),
            cui::make_shared<SizedBox>(0.0, 150.0, midListener),
        };
        auto outerScroll = cui::make_shared<SingleChildScrollView>(
            &oc, cui::make_shared<Column>(outerItems));
        auto outerListener =
            cui::make_shared<NotificationListener>(outerScroll, recorder(outer, false));
        Harness h;
        h.attach(outerListener, 200, 400);

        ScrollUpdateNotification n(ScrollMetrics{}, probe->captured());
        n.dispatch(probe->captured());
        dump("4c-middle", middle);
        dump("4c-outer", outer);
        check(middle.size() == 1 && middle[0].depth == 1, "内层视口已计入 ⇒ 中间监听者 depth 1");
        check(outer.size() == 1 && outer[0].depth == 2, "两个视口 ⇒ 外层监听者 depth 2");
        check(n.depth() == 2, "depth 挂在通知自身：上冒途中累加，不回退");

        ScrollUpdateNotification fresh(ScrollMetrics{}, probe->captured());
        check(fresh.depth() == 0, "新通知初始 depth 0");
    }

    // 4d：非视口通知不参与 depth（LayoutChangedNotification 不带 depth mixin）。
    {
        std::vector<Note> seen;
        ScrollController c;
        auto probe = cui::make_shared<ContextProbe>(cui::make_shared<SizedBox>(0.0, 50.0));
        auto box = cui::make_shared<SingleChildScrollView>(&c, probe);
        auto listener = cui::make_shared<NotificationListener>(box, recorder(seen, false));
        Harness h;
        h.attach(listener, 200, 200);

        LayoutChangedNotification n;
        n.dispatch(probe->captured());
        check(seen.size() == 1 && seen[0].depth == 0, "布局通知不受视口影响");
    }
}

// ── 测试 5：Scrollable 手势通知节奏 ────────────────────────────────────────
void testScrollableRhythm() {
    std::printf("[test] Scrollable 手势通知节奏\n");

    std::vector<Note> seen;
    std::vector<Note> inner;  // 位于 Scrollable **之下**的监听者
    ScrollController c;
    auto probeListener = cui::make_shared<NotificationListener>(
        columnOfRows(50.0, 10), recorder(inner, false));
    auto scrollable = cui::make_shared<Scrollable>(&c, Axis::Vertical, probeListener);
    auto listener = cui::make_shared<NotificationListener>(scrollable, recorder(seen, false));

    Harness h;
    h.attach(listener, 200, 200);  // 内容 500 ⇒ maxScrollExtent 300

    h.dispatch(downAt(100, 100));
    h.dispatch(moveAt(100, 40));   // dy=-60 ⇒ 请求 +60（forward）
    h.dispatch(moveAt(100, 46));   // dy=+6  ⇒ 请求 -6（reverse）
    h.dispatch(upAt(100, 46));
    dump("节奏", seen);

    const std::vector<std::string> expect = {
        "ScrollStartNotification", "UserScrollNotification", "ScrollUpdateNotification",
        "UserScrollNotification",  "ScrollUpdateNotification",
        "ScrollEndNotification",   "UserScrollNotification"};
    bool namesOk = seen.size() == expect.size();
    for (size_t i = 0; i < namesOk ? expect.size() : 0; ++i)
        namesOk = namesOk && seen[i].name == expect[i];
    check(namesOk, "Start → UserScroll → Update → UserScroll → Update → End → UserScroll(idle)");
    check(!seen.empty() && seen[0].name == "ScrollStartNotification" && seen[0].depth == 0,
          "自身的滚动对最近监听者是 depth 0");
    check(seen.size() >= 2 && seen[1].dir == "forward" && seen[3].dir == "reverse" &&
              seen.back().dir == "idle",
          "方向通知：forward → reverse → 收尾 idle");
    check(seen.size() == 7 && near(seen[2].value, 60.0) && near(seen[4].value, -6.0),
          "Update 带本次真实位移");
    check(inner.empty(), "子树里的监听者收不到（dispatch 只向上）");
    check(near(c.offset(), 54.0), "落位 = 60 - 6");
}

// ── 测试 6：Overscroll ─────────────────────────────────────────────────────
void testOverscroll() {
    std::printf("[test] Overscroll\n");

    // 6a：滚轮一次越界 ⇒ Update(300) + Overscroll(剩余 9700)。
    {
        std::vector<Note> seen;
        ScrollController c;
        auto scrollable =
            cui::make_shared<Scrollable>(&c, Axis::Vertical, columnOfRows(50.0, 10));
        auto listener = cui::make_shared<NotificationListener>(scrollable, recorder(seen, false));
        Harness h;
        h.attach(listener, 200, 200);

        h.dispatch(wheelAt(100, 100, 10000.0));
        dump("6a", seen);
        check(seen.size() == 6 && seen[1].name == "UserScrollNotification" &&
                  seen[2].name == "ScrollUpdateNotification" &&
                  seen[3].name == "OverscrollNotification",
              "越界滚轮：Start → UserScroll → Update → Overscroll → End → idle");
        check(seen.size() == 6 && near(seen[2].value, 300.0) && near(seen[3].value, 9700.0),
              "Update 只报真实位移，Overscroll 报被吃掉的量");
        check(near(seen[3].value, 10000.0 - c.maxScrollExtent()),
              "overscroll = 请求量 - maxScrollExtent");

        // 6b：已在边界再推 ⇒ 上游 `pointerScroll` 的 `targetPixels == pixels` 短路，
        // 既不滚也不认领 ⇒ 一条通知都不发（旧实现会在这里补发 Overscroll）。
        // 这次滚轮因此会向祖先冒泡，正是嵌套时「里层到底才轮到外层」的依据。
        seen.clear();
        h.dispatch(wheelAt(100, 100, 10.0));
        dump("6b", seen);
        check(seen.empty(), "界外推进：不认领 ⇒ 无 Overscroll 也无 Start/End");
        check(near(c.offset(), 300.0), "界外推进偏移仍夹在 maxScrollExtent");
    }

    // 6c：NeverScrollable ⇒ 上游 `_receivedPointerSignal` 在认领之前就 return，
    // 所以这一次滚轮**一条通知都不发**（旧实现会发 Overscroll）。滚轮增量原样
    // 留给外层滚动视图，嵌套用例见 test_scroll.cpp §9。
    {
        std::vector<Note> seen;
        ScrollController c;
        auto scrollable = cui::make_shared<Scrollable>(
            &c, Axis::Vertical, columnOfRows(50.0, 10),
            cui::make_shared<NeverScrollableScrollPhysics>());
        auto listener = cui::make_shared<NotificationListener>(scrollable, recorder(seen, false));
        Harness h;
        h.attach(listener, 200, 200);

        h.dispatch(wheelAt(100, 100, 60.0));
        dump("6c", seen);
        check(seen.empty(), "NeverScrollable：滚轮不认领 ⇒ 无 Start/Overscroll/End");
        check(near(c.offset(), 0.0), "NeverScrollable 偏移不动");
    }

    // 6d：监听者返回 true 只中断「这一条」，后续通知照发。
    {
        std::vector<Note> outer;
        ScrollController c;
        auto scrollable =
            cui::make_shared<Scrollable>(&c, Axis::Vertical, columnOfRows(50.0, 10));
        auto innerListener = cui::make_shared<NotificationListener>(scrollable,
                                                                   [](Notification&) { return true; });
        auto outerListener =
            cui::make_shared<NotificationListener>(innerListener, recorder(outer, false));
        Harness h;
        h.attach(outerListener, 200, 200);

        h.dispatch(wheelAt(100, 100, 60.0));
        check(outer.empty(), "每条通知都在内层被吃掉 ⇒ 外层一无所获");
        check(near(c.offset(), 60.0), "中断冒泡不影响滚动本身");
    }
}

// ── 测试 7：metrics 快照 ───────────────────────────────────────────────────
void testMetrics() {
    std::printf("[test] ScrollMetrics 快照\n");

    std::vector<Note> seen;
    double pixels = -1.0, maxExtent = -1.0, viewport = -1.0;
    bool axisVertical = false;
    ScrollController c;
    auto scrollable =
        cui::make_shared<Scrollable>(&c, Axis::Vertical, columnOfRows(50.0, 10));
    auto listener = NotificationListener::typed<ScrollUpdateNotification>(
        scrollable, [&seen, &pixels, &maxExtent, &viewport, &axisVertical](
                        ScrollUpdateNotification& n) {
            seen.push_back(Note{n.toString(), n.depth(), n.scrollDelta.value_or(0.0), "idle"});
            pixels = n.metrics.pixels;
            maxExtent = n.metrics.maxScrollExtent;
            viewport = n.metrics.viewportExtent;
            axisVertical = n.metrics.axis == Axis::Vertical;
            return false;
        });

    Harness h;
    h.attach(listener, 200, 200);
    h.dispatch(wheelAt(100, 100, 60.0));

    dump("7", seen);
    check(seen.size() == 1 && near(pixels, 60.0), "Update 的 pixels = 落位后的偏移");
    check(near(maxExtent, 300.0), "maxScrollExtent = 内容 500 - 视口 200");
    check(near(viewport, 200.0), "viewportExtent = 窗口高");
    check(axisVertical, "axis 跟随 scrollDirection");

    ScrollMetrics m;
    m.pixels = 0.0;
    m.maxScrollExtent = 300.0;
    m.viewportExtent = 200.0;
    check(m.isScrollable() && !m.outOfRange(), "isScrollable / outOfRange");
    check(near(m.extentAfter(), 200.0) && near(m.extentBefore(), 0.0),
          "extentBefore/After 受视口长度上限");
}

// ── 测试 8：键盘滚动的通知节奏 ─────────────────────────────────────────────
void testKeyboardRhythm() {
    std::printf("[test] 键盘滚动通知\n");

    std::vector<Note> seen;
    ScrollController c;
    auto scrollable =
        cui::make_shared<Scrollable>(&c, Axis::Vertical, columnOfRows(50.0, 10));
    auto listener = cui::make_shared<NotificationListener>(scrollable, recorder(seen, false));
    Harness h;
    h.attach(listener, 200, 200);  // maxScrollExtent 300，页 = 200 - 16

    h.dispatch(downAt(100, 100));  // 点击聚焦：手指按下本身就发了一次 Start
    check(seen.size() == 1 && seen[0].name == "ScrollStartNotification",
          "按下即发 Start（上游 userGesture 起始，抬手才 End）");
    seen.clear();
    auto* node = h.owner.focusManager().primaryFocus();
    check(node != nullptr && node->onKeyEvent != nullptr, "点击后滚动区拿到焦点");
    if (!node)
        return;

    // 同一次手势里 scrolling_ 仍为真 ⇒ 第二组没有新的 Start（上游 beginActivity
    // 的 isScrollingInProgress 判据）。抬手/滚轮那类「独立手势」才自成一组。
    node->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::Down), "", 0, false});
    dump("8-down", seen);
    check(seen.size() == 4 && seen[0].name == "UserScrollNotification" &&
              seen[0].dir == "forward" && near(seen[1].value, 40.0) &&
              seen[2].name == "ScrollEndNotification" && seen[3].dir == "idle",
          "手势未结束 ⇒ 无重复 Start，仍有 Update/End/idle");

    seen.clear();
    node->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::PageDown), "", 0, false});
    dump("8-pagedown", seen);
    check(seen.size() == 5 && seen[0].name == "ScrollStartNotification",
          "End 之后 scrolling_ 复位 ⇒ 下一次按键重新发 Start");
    check(seen.size() == 5 && near(seen[2].value, 184.0), "PageDown 位移 = viewportExtent - 16");

    // 越界：Update 只报到 300，剩余 108 进 Overscroll。
    seen.clear();
    node->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::PageDown), "", 0, false});
    dump("8-overscroll", seen);
    check(seen.size() == 6 && seen[2].name == "ScrollUpdateNotification" &&
              near(seen[2].value, 76.0) && seen[3].name == "OverscrollNotification" &&
              near(seen[3].value, 108.0),
          "越界键位：Update 到界 + Overscroll 剩余量");

    seen.clear();
    node->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::Up), "", 0, false});
    check(seen.size() == 5 && seen[1].dir == "reverse", "向上键 ⇒ UserScroll(reverse)");

    seen.clear();
    node->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::Right), "", 0, false});
    check(seen.empty(), "未映射的键位不发通知（handleKey 返回 Ignored）");
}

}  // namespace

int main() {
    std::printf("=== notification tests ===\n");
    testBubbling();
    testStopBubbling();
    testTypeFilter();
    testViewportDepth();
    testScrollableRhythm();
    testOverscroll();
    testMetrics();
    testKeyboardRhythm();
    std::printf(g_failures ? "FAILED %d\n" : "ALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
