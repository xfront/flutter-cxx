#pragma once
/// @file widgets/scroll.h
/// @brief 滚动系列 Widget（上游的 scrolling 框架子集）。
///
/// 对应 上游源码：
///   - ScrollController（widgets/scroll_controller.dart）
///   - SingleChildViewport（widgets/single_child_scroll_view.dart 的私有
///     `_SingleChildViewport`；上游的 sliver 版 `Viewport` 在 widgets/sliver.h）
///   - Scrollable（widgets/scrollable.dart：pan/wheel → 位置变化）
///   - SingleChildScrollView（widgets/single_child_scroll_view.dart）
///   - ListView（widgets/list_view.dart，sliver 版：CustomScrollView →
///     SliverList / SliverFixedExtentList；`children:` 用 SliverChildListDelegate，
///     `ListView.builder` / `ListView.separated` 用 SliverChildBuilderDelegate）；
///     旧名 ListViewLazy 已删（它就是 ListView.builder + itemExtent）
///   - GridView.count / GridView.builder（widgets/grid_view.dart，sliver 版：
///     CustomScrollView → SliverGrid）；旧名 GridViewLazy 已删（它就是
///     GridView.builder + Fixed delegate）
///   - CustomScrollView（widgets/scroll_view.dart，sliver 序列版）
///   - Scrollbar（material/scrollbar.dart）
///
/// 简化说明（与 上游的差异）：
///   - 未引入 ScrollPosition：位置直接存在 ScrollController 里，滚动行为由
///     ScrollPhysics（Clamping / Bouncing / NeverScrollable）决定。
///   - 盒子模式（content）用本仓的简化 SingleChildViewport（rendering/render_scroll.h，
///     单子节点、不裁剪到 sliver 粒度）；sliver 模式（slivers）走真 sliver
///     协议：Viewport → RenderViewport（rendering/sliver.h）。
///     **ListView 与 GridView 都已迁到 sliver 模式**（2026-11-02）；
///     **SingleChildScrollView 保持盒子模式是有意的**：上游它就不走 sliver，
///     用的是私有的 `_RenderSingleChildViewport`（见 render_scroll.h）。
///   - 列表/网格的懒加载都是真懒加载（SliverChildBuilderDelegate + 窗口内
///     物化 + 子节点回收）；旧「固定行高 + 等高占位 + Ticker 补帧」近似已删。
///   - Scrollbar 用「代理盒内容之上叠加轨道 + thumb」实现（上游 是
///     ScrollNotification 驱动的独立 overlay，这里由 controller 的绘制观察者
///     直接 markNeedsPaint）；几何口径见 rendering/render_scroll.h。

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/platform/event.h"  // KeyCode
#include "cui/rendering/render_scroll.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/gestures.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>  // TARGET_OS_IPHONE（iOS 判定）
#endif
#include "cui/framework/framework.h"
#include "cui/framework/build_owner.h"  // BuildOwner::focusManager() 键盘滚动
#include "cui/framework/focus.h"        // FocusNode / KeyEvent
#include "cui/foundation/shared_ptr.h"
#include "cui/widgets/grid.h"  // SliverGrid + 两族 SliverGridDelegate（GridView）
#include "cui/framework/listener.h"
#include "cui/widgets/notification.h"         // Notification 冒泡
#include "cui/widgets/scroll_notification.h"  // ScrollNotification 家族
#include "cui/widgets/scroll_physics.h"
#include "cui/widgets/sliver.h"  // Viewport（Scrollable 的 sliver 模式）

namespace cui {

// ScrollController 定义在 rendering/render_scroll.h（渲染层自洽，
// RenderScrollbar 直接作为其绘制观察者）。

// ── SingleChildViewport ─────────────────────────────────────────────────────
/// 视口 Widget：把内容裁剪平移到滚动偏移处。上游的 `_SingleChildViewport`
/// （widgets/single_child_scroll_view.dart）—— 上游的 `SingleChildScrollView` 用的就是
/// 它而不是 sliver `Viewport`，所以本仓这条盒子模式保留、不算待迁项。
class SingleChildViewport : public SingleChildRenderObjectWidget {
public:
    using ExtentCallback = RenderSingleChildViewport::ExtentCallback;

    explicit SingleChildViewport(AxisDirection axisDirection = AxisDirection::Down,
                                 double offset = 0.0, ExtentCallback onMaxExtentChanged = {},
                                 SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          axisDirection_(axisDirection),
          offset_(offset),
          onMaxExtentChanged_(std::move(onMaxExtentChanged)) {}

    RenderObject* createRenderObject() override {
        return new RenderSingleChildViewport(axisDirection_);
    }

    void updateRenderObject(RenderObject* ro) override {
        auto* v = dynamic_cast<RenderSingleChildViewport*>(ro);
        if (!v)
            return;
        // 上游的同款顺序依赖：offset 的绘制平移公式要读 axisDirection。
        v->setAxisDirection(axisDirection_);
        v->setOffset(offset_);
        v->setOnMaxExtentChanged(onMaxExtentChanged_);
    }

    /// 上游的 `_SingleChildViewportElement`（同样混了 ViewportElementMixin）：
    /// 盒子模式视口也参与 depth 计数，嵌套滚动才区分得开。
    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override {
        return std::make_unique<SingleChildViewportElement>(std::move(self));
    }

private:
    AxisDirection axisDirection_;
    double offset_;
    ExtentCallback onMaxExtentChanged_;
};

// ── Scrollable ─────────────────────────────────────────────────────────────
/// 可滚动区域：pan 拖拽 / 滚轮驱动 controller。上游的 `Scrollable`。
/// physics 为空时用 ClampingScrollPhysics（越界 clamp）。
///
/// 两种内容模式（对应 上游 Scrollable 的 `viewportBuilder` 分别产出
/// 盒子版 `_SingleChildViewport` / 多 sliver 版 `Viewport`）：
///   - 盒子模式：传 `content`，内部用本仓简化的单子节点 SingleChildViewport；
///   - sliver 模式：`slivers` 非空，内部用 Viewport（真 sliver 协议），
///     主轴由 `axisDirection` 决定，`content` 此时忽略。
/// 因本仓构造函数只有这一个位置参数序列，sliver 参数排在 `key` 之后。
class Scrollable : public StatefulWidget {
public:
    explicit Scrollable(ScrollController* controller, Axis axis = Axis::Vertical,
                        SharedPtr<Widget> content = nullptr,
                        SharedPtr<ScrollPhysics> physics = nullptr,
                        bool keyboardScroll = true, KeyPtr key = nullptr,
                        std::vector<SharedPtr<Widget>> slivers = {},
                        AxisDirection axisDirection = AxisDirection::Down,
                        double cacheExtent = 250.0, bool reverse = false)
        : StatefulWidget(std::move(key)),
          controller_(controller),
          axis_(axis),
          content_(std::move(content)),
          physics_(std::move(physics)),
          keyboardScroll_(keyboardScroll),
          slivers_(std::move(slivers)),
          axisDirection_(axisDirection),
          cacheExtent_(cacheExtent),
          reverse_(reverse) {}

    ScrollController* controller() const { return controller_; }
    Axis axis() const { return axis_; }
    const SharedPtr<Widget>& content() const { return content_; }
    const SharedPtr<ScrollPhysics>& physics() const { return physics_; }
    /// 是否支持键盘方向键/PageUp/Down 滚动（侧栏用它做「选中项」导航，故关闭）。
    bool keyboardScroll() const { return keyboardScroll_; }

    /// sliver 模式：子 sliver 序列非空。上游的 `Scrollable.slivers`。
    bool hasSlivers() const { return !slivers_.empty(); }
    const std::vector<SharedPtr<Widget>>& slivers() const { return slivers_; }
    AxisDirection axisDirection() const { return axisDirection_; }
    double cacheExtent() const { return cacheExtent_; }
    /// 上游的 `reverse`：盒子模式用它推 AxisDirection（Up/Left）。
    bool reverse() const { return reverse_; }
    /// 实际主轴方向（sliver 模式由 axisDirection 推导）。
    Axis mainAxis() const { return hasSlivers() ? axisDirectionToAxis(axisDirection_) : axis_; }
    /// 盒子模式的主轴朝向 = 上游 `getAxisDirectionFromAxisReverseAndDirectionality`
    /// （本仓无 Directionality，等价于 `axisDirectionFromAxis(axis, reverse)`）。
    AxisDirection boxAxisDirection() const { return axisDirectionFromAxis(axis_, reverse_); }

    std::unique_ptr<State> createState() override;

private:
    ScrollController* controller_;
    Axis axis_;
    SharedPtr<Widget> content_;
    SharedPtr<ScrollPhysics> physics_;
    bool keyboardScroll_;
    std::vector<SharedPtr<Widget>> slivers_;
    AxisDirection axisDirection_;
    double cacheExtent_;
    bool reverse_;
};

/// Scrollable 的状态：监听 controller → setState（重建传入视口（SingleChildViewport /
/// Viewport）的
/// offset），pan/wheel 事件 → 经 ScrollPhysics 换算后写偏移；panEnd 越界
/// 回弹（jumpTo 的边界 clamp 一步到位）。
/// 键盘滚动：可滚动区域获得焦点后，方向键/PgUp/PgDn 直接驱动 controller。
class ScrollableState : public State {
public:
    void initState() override {
        auto* self = this;
        controller_ = scrollable()->controller();
        listenerHandle_ = controller_->addListener([self] { self->setState(); });
        // 键盘滚动：建立焦点节点并挂接方向键/PgUp/PgDn 处理（仅在开启时）。
        if (scrollable()->keyboardScroll()) {
            focusNode_ = cui::make_shared<FocusNode>();
            if (BuildContext* ctx = context())
                if (Element* e = ctx->element())
                    if (BuildOwner* bo = e->owner())
                        focusNode_->setManager(&bo->focusManager());
            focusNode_->onKeyEvent = [this](const KeyEvent& ev) {
                return handleKey(ev);
            };
        }
    }

    void dispose() override {
        // 注销 setState 监听器：否则控制器（常与页面/侧栏等长生命周期对象
        // 共享）会一直保留指向本 State 的悬空回调，下次 forceOffset/jumpTo
        // 触发时回落到已释放的 element_ → markNeedsBuild 崩溃。
        if (controller_ && listenerHandle_ != ScrollController::kInvalidListener)
            controller_->removeListener(listenerHandle_);
        controller_ = nullptr;
        listenerHandle_ = ScrollController::kInvalidListener;
        State::dispose();
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        auto* w = scrollable();
        ScrollController* c = w->controller();
        SharedPtr<ScrollPhysics> physics =
            w->physics() ? w->physics() : cui::make_shared<ClampingScrollPhysics>();

        // 内容：视口携带当前 offset；测量回写 controller。
        SharedPtr<Widget> vp;
        if (w->hasSlivers()) {
            const AxisDirection ad = w->axisDirection();
            vp = cui::make_shared<Viewport>(c, ad, defaultCrossAxisDirection(ad),
                                                  w->cacheExtent(), w->slivers());
        } else {
            vp = cui::make_shared<SingleChildViewport>(
                w->boxAxisDirection(), c->offset(),
                [c](double maxExtent, double viewportExtent) {
                    c->applyExtents(maxExtent, viewportExtent);
                },
                w->content());
        }

        // pan：手指拖拽内容（内容跟手 → offset 减 delta）。
        // 通知节奏对齐上游 ScrollPositionWithSingleContext：按下 ⇒ Start，
        // 每次位移 ⇒ Update（+ 方向变化时 UserScroll，+ 被边界吃掉的量 ⇒
        // Overscroll），抬手 ⇒ End + UserScroll(idle)。
        auto* self = this;
        GestureDetector::PointerCallback onPanDown = [self](const PointerEvent& e) {
            self->lastPanPos_ = e.position;
            self->beginScroll();
        };
        GestureDetector::PointerCallback onPanUpdate = [self, physics,
                                                        axis = w->mainAxis()](const PointerEvent& e) {
            double delta = axis == Axis::Vertical ? e.position.dy - self->lastPanPos_.dy
                                                  : e.position.dx - self->lastPanPos_.dx;
            self->lastPanPos_ = e.position;
            self->applyScrollDelta(-delta, *physics);  // 内容跟手 ⇒ 偏移取反
        };
        GestureDetector::PointerCallback onPanEnd = [self, c](const PointerEvent&) {
            c->jumpTo(c->offset());  // 越界则 clamp 回弹（简化：无弹簧动画）
            self->endScroll();
        };
        auto gd = cui::make_shared<GestureDetector>(vp, nullptr, nullptr, nullptr,
                                                    onPanDown, onPanUpdate, onPanEnd);

        // 滚轮：按自身轴向取增量（纵向视口吃 scrollDeltaY、横向吃 scrollDeltaX，
        // 对齐上游 `_pointerSignalEventDelta`）。鼠标按住 Shift/Ctrl 时横向滚动
        // 视图改吃纵向增量（Shift+滚轮横滚）。上游 pointerSignal 的节奏是「起 →
        // 一次位移 → 立即转 idle」，所以每个滚轮事件是一整组三段通知。
        // 嵌套（两层滚动条）归谁：上游 PointerSignalResolver 只让**第一个**
        // （= 命中链里最深的）「真能吸收这份增量」的 Scrollable 处理，其余全部
        // 落空。本仓靠冒泡实现同一语义 ⇒ 能吃下才动、动完就拦住冒泡（否则外层
        // 会跟着滚同一份增量）；已到边界或 physics 不接受用户滚动则原样放行，
        // 里层滚到底/顶之后外层才接着滚。
        Listener::PointerCallback onScroll = [self, physics,
                                              axis = w->mainAxis()](const PointerEvent& e) {
            if (e.type != PointerEventType::Scroll)
                return;
            double delta = axis == Axis::Vertical ? e.scrollDeltaY : e.scrollDeltaX;
            if (axis == Axis::Horizontal &&
                (e.modifiers & (ModShift | ModCtrl)) != 0)
                delta = e.scrollDeltaY;
            if (!physics->shouldAcceptUserOffset() || !self->wouldAbsorb(delta))
                return;
            self->beginScroll();
            self->applyScrollDelta(delta, *physics);
            self->endScroll();
            e.stopPropagation();
        };
        auto inner = cui::make_shared<Listener>(nullptr, nullptr, nullptr, nullptr,
                                                onScroll, gd);
        if (!w->keyboardScroll() || !focusNode_)
            return inner;
        // 点击聚焦（不抢可编辑文本框的焦点），使方向键/PgUp/PgDn 可滚动本区域。
        auto node = focusNode_;
        Element* scrollElem = context.element();
        Listener::PointerCallback onDown = [node, scrollElem](const PointerEvent&) {
            auto* fm = node->manager();
            FocusNode* cur = fm ? fm->primaryFocus() : nullptr;
            if (cur && cur != node.get() && !cur->isEditable()) {
                // 事件自 target 向根冒泡：内层控件（如 OtpInput 的格子）在本次
                // down 到达这里之前刚上了焦，而本区域是它的祖先 ⇒ 抢回来会让
                // 内层高亮和键盘路由一起失效。上游的 Scrollable 不做「点击即夺
                // 焦」，这里按「焦点还落在自己子树里就不动」等价收敛。
                for (Element* e = cur->context(); e; e = e->parent())
                    if (e == scrollElem)
                        return;
            } else if (cur && cur->isEditable()) {
                return;  // 正在文本输入：不抢焦点（原有口径）
            }
            node->requestFocus();
        };
        return cui::make_shared<Listener>(onDown, nullptr, nullptr, nullptr, nullptr,
                                          inner);
    }

    Offset lastPanPos_;

private:
    Scrollable* scrollable() const { return static_cast<Scrollable*>(widget()); }

    ScrollController* controller_ = nullptr;
    ScrollController::ListenerHandle listenerHandle_ = ScrollController::kInvalidListener;

    /// 一次滚动手势的起止状态（上游的 `isScrollingInProgress`）。
    bool scrolling_ = false;
    ScrollDirection lastUserDirection_ = ScrollDirection::Idle;
    SharedPtr<ScrollPhysics> cachedPhysics_;

    /// 方向键/PgUp/PgDn → 滚动 controller（clamp 到 [0, maxScrollExtent]）。
    KeyEventResult handleKey(const KeyEvent& ev) {
        ScrollController* c = scrollable()->controller();
        double step = 0.0;
        switch (static_cast<KeyCode>(ev.key)) {
            case KeyCode::Down:    step = kKeyboardLine; break;
            case KeyCode::Up:      step = -kKeyboardLine; break;
            case KeyCode::PageDown: {
                double page = std::max(kKeyboardLine, c->viewportExtent() - 16.0);
                step = page;
                break;
            }
            case KeyCode::PageUp: {
                double page = std::max(kKeyboardLine, c->viewportExtent() - 16.0);
                step = -page;
                break;
            }
            default: return KeyEventResult::Ignored;
        }
        beginScroll();
        applyScrollDelta(step, *resolvedPhysics());
        endScroll();
        return KeyEventResult::Handled;
    }

    // ── 通知（上游 ScrollPositionWithSingleContext 的 _dispatchNotification）──

    /// 当前滚动位置快照。上游的 `ScrollPosition.metrics`。
    ScrollMetrics scrollMetrics() const {
        ScrollController* c = scrollable()->controller();
        return ScrollMetrics{c->offset(), 0.0, c->maxScrollExtent(), c->viewportExtent(),
                             scrollable()->mainAxis()};
    }

    /// 通知从本 State 的 context 冒泡 ⇒ 只有 Scrollable 的**祖先**
    /// NotificationListener 收得到（上游同款）。
    void dispatchScrollNotification(Notification& n) const {
        if (BuildContext* ctx = context())
            n.dispatch(ctx);
    }

    /// 上游 `beginActivity`：同一次手势里只发一次 Start。
    void beginScroll() {
        if (scrolling_)
            return;
        scrolling_ = true;
        ScrollStartNotification n(scrollMetrics(), context());
        dispatchScrollNotification(n);
    }

    /// 上游 `endScrolling`：End 之后紧跟一个 direction=idle 的 UserScroll。
    void endScroll() {
        if (!scrolling_)
            return;
        scrolling_ = false;
        ScrollEndNotification n(scrollMetrics(), context());
        dispatchScrollNotification(n);
        UserScrollNotification idle(scrollMetrics(), context(), ScrollDirection::Idle);
        dispatchScrollNotification(idle);
        lastUserDirection_ = ScrollDirection::Idle;
    }

    /// 上游 `_targetScrollOffsetForPointerScroll` / `pointerScroll` 的判据：把增量
    /// **硬夹**到 `[0, maxScrollExtent]` 后偏移是否真的变了 —— 变了才「认领」这次
    /// 滚轮。刻意用硬夹而不是 physics.applyTo：上游裁决归属时不算 BouncingScrollPhysics
    /// 的越界阻尼，所以里层已在边界、physics 还能阻尼位移时，增量照样让给外层。
    bool wouldAbsorb(double delta) const {
        if (delta == 0.0)
            return false;
        ScrollController* c = scrollable()->controller();
        const double before = c->offset();
        return std::clamp(before + delta, 0.0, c->maxScrollExtent()) != before;
    }

    /// 把 delta 推进 controller，并按上游节奏发 Update / Overscroll / UserScroll。
    /// 返回真正滚掉的像素（被 physics/边界吃掉的部分不发 Update）。
    double applyScrollDelta(double delta, const ScrollPhysics& physics) {
        ScrollController* c = scrollable()->controller();
        // 上游 `applyUserOffset` 的节奏：setUserScrollDirection（按**请求量**，
        // 不是实际落位量）→ didUpdateScrollPositionBy（Update）→ 剩余量
        // Overscroll。
        if (delta != 0.0)
            notifyDirection(delta);
        const double before = c->offset();
        const double after =
            physics.applyTo(before + delta, before, 0.0, c->maxScrollExtent());
        c->forceOffset(after);
        const double applied = c->offset() - before;
        if (applied != 0.0) {
            ScrollUpdateNotification n(scrollMetrics(), context(), applied);
            dispatchScrollNotification(n);
        }
        // 上游分两条路径发 Overscroll（完全没动 ⇒ value=delta；动了但没吃满 ⇒
        // value=delta-applied），数值上等价于这里的 remaining。
        if (const double remaining = delta - applied; remaining != 0.0) {
            OverscrollNotification n(scrollMetrics(), context(), remaining);
            dispatchScrollNotification(n);
        }
        return applied;
    }

    /// 上游 `didUpdateScrollDirection`：只在方向真的变化时发通知。
    /// 偏离：上游还要按 AxisDirection/Directionality 折算，本仓按
    /// 「偏移增大 = forward，反向轴取反」处理。
    void notifyDirection(double delta) {
        const bool reversed = axisDirectionIsReversed(scrollable()->hasSlivers()
                                                          ? scrollable()->axisDirection()
                                                          : scrollable()->boxAxisDirection());
        // 上游 `_deltaToScrollDirection`：delta==0 ⇒ idle，其余按符号折算。
        if (delta == 0.0)
            return;
        const bool forward = (delta > 0.0) != reversed;
        const ScrollDirection dir =
            forward ? ScrollDirection::Forward : ScrollDirection::Reverse;
        if (dir == lastUserDirection_)
            return;
        lastUserDirection_ = dir;
        UserScrollNotification n(scrollMetrics(), context(), dir);
        dispatchScrollNotification(n);
    }

    /// physics 为空时的默认值（和 build 里给视口的那份一致）。
    const SharedPtr<ScrollPhysics>& resolvedPhysics() {
        if (!cachedPhysics_)
            cachedPhysics_ = cui::make_shared<ClampingScrollPhysics>();
        return cachedPhysics_;
    }

    static constexpr double kKeyboardLine = 40.0;  // 单方向键滚动像素

    SharedPtr<FocusNode> focusNode_;

    friend class Scrollable;
};

inline std::unique_ptr<State> Scrollable::createState() {
    return std::make_unique<ScrollableState>();
}

// ── SingleChildScrollView ──────────────────────────────────────────────────
/// 单子节点滚动视图。上游的 `SingleChildScrollView`
/// （widgets/single_child_scroll_view.dart）。
///
/// 上游它**不走 sliver**：`Scrollable` 的 `viewportBuilder` 产出私有的
/// `_SingleChildViewport` → `_RenderSingleChildViewport`（本仓的
/// SingleChildViewport / RenderSingleChildViewport），所以这条盒子模式是
/// 对齐结果而不是待迁项。
///
/// 与 上游 的差别：`primary`/`PrimaryScrollController`、`clipBehavior`、
/// `hitTestBehavior`、`dragStartBehavior`、`restorationId`、
/// `keyboardDismissBehavior` 依赖本仓尚未移植的机制（继承式控制器、图层裁剪
/// 策略、语义/恢复），故不接；`cacheExtent` 对单子节点视口无意义（内容一次
/// 性布局），上游同样没有。位置参数序列是 上游 命名参数的简化，新增参数一律
/// 追加在 `keyboardScroll` 之后、`key` 之前，避免旧调用点串位。
class SingleChildScrollView : public StatelessWidget {
public:
    explicit SingleChildScrollView(ScrollController* controller,
                                  SharedPtr<Widget> child,
                                  Axis scrollDirection = Axis::Vertical,
                                  bool keyboardScroll = true, bool reverse = false,
                                  EdgeInsets padding = {},
                                  SharedPtr<ScrollPhysics> physics = nullptr,
                                  KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)),
          controller_(controller),
          child_(std::move(child)),
          scrollDirection_(scrollDirection),
          keyboardScroll_(keyboardScroll),
          reverse_(reverse),
          padding_(padding),
          physics_(std::move(physics)) {}

    SharedPtr<Widget> build(BuildContext& context) override;

    ScrollController* controller() const { return controller_; }
    const SharedPtr<Widget>& child() const { return child_; }
    Axis scrollDirection() const { return scrollDirection_; }
    bool reverse() const { return reverse_; }
    const EdgeInsets& padding() const { return padding_; }
    const SharedPtr<ScrollPhysics>& physics() const { return physics_; }

    std::string toStringShort() const override { return "SingleChildScrollView"; }

private:
    ScrollController* controller_;
    SharedPtr<Widget> child_;
    Axis scrollDirection_;
    bool keyboardScroll_;
    bool reverse_;
    EdgeInsets padding_;
    SharedPtr<ScrollPhysics> physics_;
};

// ── ListView ───────────────────────────────────────────────────────────────
/// 列表：`ListView(children:)` / `ListView::builder` / `ListView::separated`。
/// 上游的 `ListView`（widgets/list_view.dart）。
///
/// 与 上游 的差别：上游 ListView 沿 BoxScrollView→Scrollable+Viewport 自己搭
/// 视口，本仓复用 `CustomScrollView`（同一条 Scrollable+Viewport 路径，
/// 语义等价，和 GridView 一样）；`padding`（→SliverPadding）、`shrinkWrap`、
/// `primary`/`PrimaryScrollController`、`addAutomaticKeepAlives` 三件套未接。
/// `itemExtent` 有值时走 `SliverFixedExtentList`（上游同款：定高可以省掉逐个
/// 量子节点），无值时走 `SliverList`（变高，真按需布局）。
///
/// 交叉轴：`SliverConstraints::asBoxConstraints()` 把交叉轴锁到视口宽度，所以
/// 子节点横向撑满（= 上游 ListView 的 stretch 效果，不需要再包 Column）。
class ListView : public StatelessWidget {
public:
    using ItemBuilder = NullableIndexedWidgetBuilder;

    /// 上游的 `ListView(children: ...)`：非懒加载子节点列表。
    explicit ListView(ScrollController* controller,
                      std::vector<SharedPtr<Widget>> children = {},
                      Axis scrollDirection = Axis::Vertical, bool reverse = false,
                      std::optional<double> itemExtent = std::nullopt,
                      double cacheExtent = 250.0,
                      SharedPtr<ScrollPhysics> physics = nullptr, KeyPtr key = nullptr)
        : ListView(controller, std::move(children), nullptr, std::nullopt, itemExtent,
                   scrollDirection, reverse, cacheExtent, std::move(physics), std::move(key)) {}

    /// 上游的 `ListView.builder`：itemBuilder +（可选）itemCount。
    /// `itemCount` 省略 ⇒ 内容长度未知，滚到 builder 返回空为止（上游同款）。
    static SharedPtr<ListView> builder(
        ScrollController* controller, ItemBuilder itemBuilder,
        std::optional<int> itemCount = std::nullopt,
        std::optional<double> itemExtent = std::nullopt, Axis scrollDirection = Axis::Vertical,
        bool reverse = false, double cacheExtent = 250.0,
        SharedPtr<ScrollPhysics> physics = nullptr, KeyPtr key = nullptr) {
        return cui::make_shared<ListView>(controller, std::vector<SharedPtr<Widget>>{},
                                          std::move(itemBuilder), itemCount, itemExtent,
                                          scrollDirection, reverse, cacheExtent,
                                          std::move(physics), std::move(key));
    }

    /// 上游的 `ListView.separated`：item 与 separator 交替，共 `2*n-1` 个子节点。
    static SharedPtr<ListView> separated(
        ScrollController* controller, ItemBuilder itemBuilder, ItemBuilder separatorBuilder,
        int itemCount, std::optional<double> itemExtent = std::nullopt,
        Axis scrollDirection = Axis::Vertical, bool reverse = false, double cacheExtent = 250.0,
        SharedPtr<ScrollPhysics> physics = nullptr, KeyPtr key = nullptr) {
        const int slots = std::max(0, itemCount * 2 - 1);
        return builder(
            controller,
            [itemBuilder = std::move(itemBuilder),
             separatorBuilder = std::move(separatorBuilder)](BuildContext& ctx, int index) {
                const int item = index / 2;  ///< 上游的 `index ~/ 2`
                return index % 2 == 0 ? itemBuilder(ctx, item) : separatorBuilder(ctx, item);
            },
            slots, itemExtent, scrollDirection, reverse, cacheExtent, std::move(physics),
            std::move(key));
    }

    /// 通用入口：children 或 itemBuilder 二选一（builder 非空即懒加载）。
    ListView(ScrollController* controller, std::vector<SharedPtr<Widget>> children,
             ItemBuilder itemBuilder, std::optional<int> itemCount,
             std::optional<double> itemExtent, Axis scrollDirection, bool reverse,
             double cacheExtent, SharedPtr<ScrollPhysics> physics, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)),
          controller_(controller),
          children_(std::move(children)),
          itemBuilder_(std::move(itemBuilder)),
          itemCount_(itemCount),
          itemExtent_(itemExtent),
          scrollDirection_(scrollDirection),
          reverse_(reverse),
          cacheExtent_(cacheExtent),
          physics_(std::move(physics)) {}

    SharedPtr<Widget> build(BuildContext& context) override;

    const std::vector<SharedPtr<Widget>>& children() const { return children_; }
    const ItemBuilder& itemBuilder() const { return itemBuilder_; }
    const std::optional<int>& itemCount() const { return itemCount_; }
    const std::optional<double>& itemExtent() const { return itemExtent_; }
    Axis scrollDirection() const { return scrollDirection_; }
    double cacheExtent() const { return cacheExtent_; }

    std::string toStringShort() const override { return "ListView"; }

private:
    ScrollController* controller_;
    std::vector<SharedPtr<Widget>> children_;
    ItemBuilder itemBuilder_;
    std::optional<int> itemCount_;
    std::optional<double> itemExtent_;
    Axis scrollDirection_;
    bool reverse_;
    double cacheExtent_;
    SharedPtr<ScrollPhysics> physics_;
};

// ── GridView ───────────────────────────────────────────────────────────────
/// 网格：`GridView.count`（列数 + 子节点列表）与 `GridView.builder`（delegate +
/// itemBuilder）。上游的 `GridView`（widgets/scroll_view.dart）。
///
/// 与 上游 的差别：上游 GridView 沿 BoxScrollView→Scrollable+Viewport 自己搭
/// 视口，本仓复用 `CustomScrollView`（同一条 Scrollable+Viewport 路径，
/// 语义等价）；`padding`（→SliverPadding）、`shrinkWrap`、
/// `primary`/`PrimaryScrollController`、`addAutomaticKeepAlives` 三件套未接。
/// `GridView.extent`/`extentBuilder` 两族入口同理：把 delegate 传进来即可，
/// 故未另设便捷构造（`SliverGridDelegateWithMaxCrossAxisExtent` 已在
/// widgets/grid.h）。
///
/// 单元格尺寸全部由 `SliverGridDelegate` 给紧约束（瓦片大小），子节点自己写的
/// 宽高会被顶掉 —— 这是 上游 的真实行为，不是本仓的近似。
class GridView : public StatelessWidget {
public:
    using ItemBuilder = ListView::ItemBuilder;

    /// 上游的 `GridView.count`：固定列数 + 显式子节点列表。
    explicit GridView(ScrollController* controller, int crossAxisCount,
                      std::vector<SharedPtr<Widget>> children = {},
                      double mainAxisSpacing = 0.0, double crossAxisSpacing = 0.0,
                      double childAspectRatio = 1.0, Axis scrollDirection = Axis::Vertical,
                      bool reverse = false, double cacheExtent = 250.0,
                      SharedPtr<ScrollPhysics> physics = nullptr, KeyPtr key = nullptr)
        : GridView(controller,
                   cui::make_shared<SliverGridDelegateWithFixedCrossAxisCount>(
                       crossAxisCount, mainAxisSpacing, crossAxisSpacing, childAspectRatio),
                   std::move(children), nullptr, std::nullopt, scrollDirection, reverse,
                   cacheExtent, std::move(physics), std::move(key)) {}

    /// 上游的 `GridView.builder`：delegate + itemBuilder +（可选）itemCount。
    static SharedPtr<GridView> builder(
        ScrollController* controller, SharedPtr<SliverGridDelegate> gridDelegate,
        ItemBuilder itemBuilder, std::optional<int> itemCount = std::nullopt,
        Axis scrollDirection = Axis::Vertical, bool reverse = false,
        double cacheExtent = 250.0, SharedPtr<ScrollPhysics> physics = nullptr,
        KeyPtr key = nullptr) {
        return cui::make_shared<GridView>(
            controller, std::move(gridDelegate), std::vector<SharedPtr<Widget>>{},
            std::move(itemBuilder), itemCount, scrollDirection, reverse, cacheExtent,
            std::move(physics), std::move(key));
    }

    /// 通用入口：delegate 由调用方给（Fixed / Max 两族皆可）。
    GridView(ScrollController* controller, SharedPtr<SliverGridDelegate> gridDelegate,
             std::vector<SharedPtr<Widget>> children, ItemBuilder itemBuilder,
             std::optional<int> itemCount, Axis scrollDirection, bool reverse, double cacheExtent,
             SharedPtr<ScrollPhysics> physics, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)),
          controller_(controller),
          gridDelegate_(std::move(gridDelegate)),
          children_(std::move(children)),
          itemBuilder_(std::move(itemBuilder)),
          itemCount_(itemCount),
          scrollDirection_(scrollDirection),
          reverse_(reverse),
          cacheExtent_(cacheExtent),
          physics_(std::move(physics)) {}

    SharedPtr<Widget> build(BuildContext& context) override;

    const SharedPtr<SliverGridDelegate>& gridDelegate() const { return gridDelegate_; }
    Axis scrollDirection() const { return scrollDirection_; }
    const std::optional<int>& itemCount() const { return itemCount_; }
    double cacheExtent() const { return cacheExtent_; }

    std::string toStringShort() const override { return "GridView"; }

private:
    ScrollController* controller_;
    SharedPtr<SliverGridDelegate> gridDelegate_;
    std::vector<SharedPtr<Widget>> children_;
    ItemBuilder itemBuilder_;
    std::optional<int> itemCount_;
    Axis scrollDirection_;
    bool reverse_;
    double cacheExtent_;
    SharedPtr<ScrollPhysics> physics_;
};

// ── CustomScrollView ───────────────────────────────────────────────────────
/// 直接以 sliver 序列为内容的滚动视图。上游的 `CustomScrollView`
/// （widgets/scroll_view.dart）。
///
/// 与 上游 的差别：`shrinkWrap` / `center` / `anchor`（居中 sliver 与
/// 自适应用户滚动方向）依赖 RenderViewport 的负向生长序列，本仓视口尚未
/// 支持，故不提供；`primary`/`PrimaryScrollController` 继承亦无。
class CustomScrollView : public StatelessWidget {
public:
    explicit CustomScrollView(std::vector<SharedPtr<Widget>> slivers = {},
                              Axis scrollDirection = Axis::Vertical, bool reverse = false,
                              ScrollController* controller = nullptr,
                              SharedPtr<ScrollPhysics> physics = nullptr,
                              double cacheExtent = 250.0, bool keyboardScroll = true,
                              KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)),
          slivers_(std::move(slivers)),
          scrollDirection_(scrollDirection),
          reverse_(reverse),
          controller_(controller),
          physics_(std::move(physics)),
          cacheExtent_(cacheExtent),
          keyboardScroll_(keyboardScroll) {}

    /// 内容 sliver 序列。上游的 `CustomScrollView.slivers`。
    const std::vector<SharedPtr<Widget>>& slivers() const { return slivers_; }
    /// 上游的 `CustomScrollView.getDirection()`：scrollDirection + reverse。
    AxisDirection axisDirection() const { return axisDirectionFromAxis(scrollDirection_, reverse_); }

    SharedPtr<Widget> build(BuildContext& context) override;

private:
    std::vector<SharedPtr<Widget>> slivers_;
    Axis scrollDirection_;
    bool reverse_;
    ScrollController* controller_;
    SharedPtr<ScrollPhysics> physics_;
    double cacheExtent_;
    bool keyboardScroll_;
};

// ── Scrollbar ──────────────────────────────────────────────────────────────
/// 滚动指示条：包裹可滚动内容，右缘叠加轨道 + thumb（样式对齐参考图的
/// 8px 胶囊：整高浅色轨道贴住控件右边缘，深色 thumb 在其上按比例伸缩）。
/// RenderScrollbar 直接持有 controller，任何滚动变化只 markNeedsPaint
/// （无需 setState 重建）。上游的 `Scrollbar`。
/// 颜色默认取主题（thumb=`theme().scrollbarThumb`，轨道=`theme().outlineVariant`），
/// 传 std::nullopt 即跟随主题；新参一律尾置在 key 之后。
class Scrollbar : public SingleChildRenderObjectWidget {
public:
    explicit Scrollbar(ScrollController* controller, SharedPtr<Widget> child,
                       KeyPtr key = nullptr, double thickness = 8.0,
                       double mainAxisMargin = 0.0, double crossAxisMargin = 0.0,
                       std::optional<Color> thumbColor = std::nullopt,
                       std::optional<Color> trackColor = std::nullopt)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          controller_(controller), thickness_(thickness),
          mainAxisMargin_(mainAxisMargin), crossAxisMargin_(crossAxisMargin),
          thumbColor_(thumbColor), trackColor_(trackColor) {}

    ScrollController* controller() const { return controller_; }

    RenderObject* createRenderObject() override {
        auto* ro = new RenderScrollbar();
        if (controller_)
            controller_->addPaintObserver(ro);
        return ro;
    }

    void updateRenderObject(RenderObject* ro) override {
        auto* s = dynamic_cast<RenderScrollbar*>(ro);
        if (!s)
            return;
        s->setController(controller_);
        s->setThickness(thickness_);
        s->setMainAxisMargin(mainAxisMargin_);
        s->setCrossAxisMargin(crossAxisMargin_);
        s->setThumbColor(thumbColor_);
        s->setTrackColor(trackColor_);
    }

private:
    ScrollController* controller_;
    double thickness_;
    double mainAxisMargin_;
    double crossAxisMargin_;
    std::optional<Color> thumbColor_;
    std::optional<Color> trackColor_;
};

/// 是否移动平台。编译期按平台生成：Android / Web(emscripten) 视为移动端，
/// 触摸滚动不自动显示滚动条；其余（Linux/Windows/macOS）为桌面端，自动显示。
inline bool g_isMobilePlatform =
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || \
    (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
    true;
#else
    false;
#endif

inline SharedPtr<Widget> SingleChildScrollView::build(BuildContext& context) {
    (void)context;
    // 上游同款：padding 在视口**内部**包一层 Padding（padding 随内容一起滚）。
    // child_ 不能 move：build 可能被调多次。
    SharedPtr<Widget> contents =
        padding_ != EdgeInsets{} ? cui::make_shared<Padding>(padding_, child_) : child_;
    auto s = cui::make_shared<Scrollable>(controller_, scrollDirection_, contents, physics_,
                                          keyboardScroll_, nullptr,
                                          std::vector<SharedPtr<Widget>>{},
                                          AxisDirection::Down, 250.0, reverse_);
    if (g_isMobilePlatform)
        return s;  // 移动端触摸滚动，不叠加滚动条
    // 桌面端：自动为可滚动内容显示滚动条（thumb 按内容/视口比例绘制）。
    return cui::make_shared<Scrollbar>(controller_, s);
}

inline SharedPtr<Widget> CustomScrollView::build(BuildContext& context) {
    (void)context;
    const AxisDirection ad = axisDirection();
    auto s = cui::make_shared<Scrollable>(controller_, axisDirectionToAxis(ad), nullptr,
                                          physics_, keyboardScroll_, nullptr, slivers_, ad,
                                          cacheExtent_);
    if (g_isMobilePlatform)
        return s;
    return cui::make_shared<Scrollbar>(controller_, s);
}

// GridView / ListView 的 build 要在 CustomScrollView 之后定义（它们把内容包成
// sliver 序列）。

inline SharedPtr<Widget> ListView::build(BuildContext& context) {
    (void)context;
    SharedPtr<SliverChildDelegate> childDelegate;
    if (itemBuilder_)
        childDelegate =
            cui::make_shared<SliverChildBuilderDelegate>(itemBuilder_, nullptr, itemCount_);
    else
        // 注意不能 move：build 可能被调多次，children_ 得留着。
        childDelegate = cui::make_shared<SliverChildListDelegate>(children_);

    SharedPtr<Widget> sliver =
        itemExtent_
            ? cui::static_pointer_cast<Widget>(
                  cui::make_shared<SliverFixedExtentList>(*itemExtent_, childDelegate))
            : cui::static_pointer_cast<Widget>(cui::make_shared<SliverList>(childDelegate));

    std::vector<SharedPtr<Widget>> slivers = {std::move(sliver)};
    return cui::make_shared<CustomScrollView>(std::move(slivers), scrollDirection_, reverse_,
                                              controller_, physics_, cacheExtent_);
}

inline SharedPtr<Widget> GridView::build(BuildContext& context) {
    (void)context;
    SharedPtr<SliverChildDelegate> childDelegate;
    if (itemBuilder_)
        childDelegate =
            cui::make_shared<SliverChildBuilderDelegate>(itemBuilder_, nullptr, itemCount_);
    else
        // 注意不能 move：build 可能被调多次，children_ 得留着。
        childDelegate = cui::make_shared<SliverChildListDelegate>(children_);

    std::vector<SharedPtr<Widget>> slivers = {
        cui::make_shared<SliverGrid>(gridDelegate_, std::move(childDelegate))};
    return cui::make_shared<CustomScrollView>(std::move(slivers), scrollDirection_, reverse_,
                                              controller_, physics_, cacheExtent_);
}

}  // namespace cui_core
