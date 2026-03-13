/// @file widgets/implicit_animation.cpp
/// @brief 隐式动画控件族实现。

#include "cui/widgets/implicit_animation.h"

#include <algorithm>
#include <cmath>

#include "cui/widgets/basic.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/paint.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {

// ── 插值工具 ────────────────────────────────────────────────────────────────

double lerpDouble(double a, double b, double t) {
    return a + (b - a) * t;
}

Color lerpColor(const Color& a, const Color& b, double t) {
    return Color::fromRGBA(lerpDouble(a.r, b.r, t), lerpDouble(a.g, b.g, t),
                           lerpDouble(a.b, b.b, t), lerpDouble(a.a, b.a, t));
}

EdgeInsets lerpEdgeInsets(const EdgeInsets& a, const EdgeInsets& b, double t) {
    return EdgeInsets::only(lerpDouble(a.left, b.left, t),
                             lerpDouble(a.top, b.top, t),
                             lerpDouble(a.right, b.right, t),
                             lerpDouble(a.bottom, b.bottom, t));
}

Size lerpSize(const Size& a, const Size& b, double t) {
    return Size{lerpDouble(a.width, b.width, t), lerpDouble(a.height, b.height, t)};
}

Offset lerpOffset(const Offset& a, const Offset& b, double t) {
    return Offset{lerpDouble(a.dx, b.dx, t), lerpDouble(a.dy, b.dy, t)};
}

Alignment lerpAlignment(const Alignment& a, const Alignment& b, double t) {
    return Alignment{lerpDouble(a.x, b.x, t), lerpDouble(a.y, b.y, t)};
}

BoxDecoration lerpBoxDecoration(const BoxDecoration& a, const BoxDecoration& b, double t) {
    BoxDecoration out;
    out.color = lerpColor(a.color, b.color, t);
    out.borderRadius = lerpDouble(a.borderRadius, b.borderRadius, t);
    out.borderWidth = lerpDouble(a.borderWidth, b.borderWidth, t);
    out.borderColor = lerpColor(a.borderColor, b.borderColor, t);
    out.elevation = lerpDouble(a.elevation, b.elevation, t);
    out.shadowColor = lerpColor(a.shadowColor, b.shadowColor, t);
    out.elevation2 = lerpDouble(a.elevation2, b.elevation2, t);
    out.shadowColor2 = lerpColor(a.shadowColor2, b.shadowColor2, t);
    return out;
}

namespace {

/// 哨兵值：表示「该属性无约束」（区别于数值 0）。
constexpr double kNone = -1e9;

/// 可选值的插值：任一端缺省时直接取另一端（不做过渡），与上游
/// `Tween<double?>` 对 null 的处理一致。
double lerpOptional(double a, bool hasA, double b, bool hasB, double t) {
    if (!hasA)
        return b;
    if (!hasB)
        return a;
    return lerpDouble(a, b, t);
}

}  // namespace

// ── ImplicitTweenDriver ─────────────────────────────────────────────────────

ImplicitTweenDriver::ImplicitTweenDriver(double initial, double durationMs, Easing curve)
    : durationMs_(durationMs), curve_(curve),
      begin_(initial), target_(initial), current_(initial) {}

bool ImplicitTweenDriver::setTarget(double target) {
    if (target == target_)
        return false;
    // 从当前插值结果开始新的一段动画 —— 这是隐式动画「打断即重定向」的关键。
    begin_ = current_;
    target_ = target;
    elapsed_ = 0.0;
    running_ = true;
    return true;
}

bool ImplicitTweenDriver::advance(double dtMs) {
    if (!running_)
        return false;
    elapsed_ += dtMs;
    double t = durationMs_ <= 0.0 ? 1.0
                                  : std::clamp(elapsed_ / durationMs_, 0.0, 1.0);
    current_ = lerpDouble(begin_, target_, applyEasing(t, curve_));
    if (t >= 1.0) {
        current_ = target_;
        running_ = false;
    }
    return running_;
}

void ImplicitTweenDriver::jumpToTarget() {
    current_ = target_;
    begin_ = target_;
    elapsed_ = 0.0;
    running_ = false;
}

void ImplicitTweenDriver::resetTo(double value) {
    begin_ = value;
    target_ = value;
    current_ = value;
    elapsed_ = 0.0;
    running_ = false;
}

// ── ImplicitlyAnimatedWidget ────────────────────────────────────────────────

std::unique_ptr<State> ImplicitlyAnimatedWidget::createState() {
    return std::make_unique<ImplicitlyAnimatedWidgetState>();
}

void ImplicitlyAnimatedWidgetState::initState() {
    ticker_ = std::make_unique<Ticker>([this](Duration elapsed) {
        double now = elapsed.count();
        double dt = now - lastElapsedMs_;
        lastElapsedMs_ = now;
        if (dt < 0.0)
            dt = 0.0;

        bool running = false;
        bool changed = false;
        for (auto& kv : tweens_) {
            const double before = kv.second.driver->value();
            if (kv.second.driver->advance(dt))
                running = true;
            changed = changed || kv.second.driver->value() != before;
        }
        if (!running) {
            // 全部动画结束：停掉 ticker，等下一次目标变化再唤醒。收尾这一帧
            // 携带的是精确终点值，必须仍然重建一次，否则子树停在倒数第二帧的
            // 中间值上（上游由 AnimationController 在 completed 时再 notify 一次
            // 监听者来保证这点）。
            ticker_->stop();
            lastElapsedMs_ = 0.0;
            if (changed && mounted())
                setState();
            // 上游 status 监听里 setState 在前、onEnd 在后（_AnimatedCrossFadeState
            // ::initState 的 addStatusListener）。
            if (mounted())
                onTweenSettled();
            return;
        }
        if (mounted())
            setState();
    });
}

void ImplicitlyAnimatedWidgetState::didChangeDependencies() {
    if (context() && ticker_)
        tickerMode_.bind(*context(), *ticker_);
}

void ImplicitlyAnimatedWidgetState::dispose() {
    tickerMode_.unbind();
    if (ticker_ && ticker_->isActive())
        ticker_->stop();
    ticker_.reset();
}

double ImplicitlyAnimatedWidgetState::tween(const std::string& key, double target) {
    auto it = tweens_.find(key);
    if (it == tweens_.end()) {
        Entry e;
        // 首次出现该属性：直接落在目标值上，不做入场动画。
        e.driver = cui::make_shared<ImplicitTweenDriver>(target, duration(), curve());
        it = tweens_.emplace(key, std::move(e)).first;
        return it->second.driver->value();
    }

    auto& driver = *it->second.driver;
    if (driver.setTarget(target) && ticker_ && !ticker_->isActive()) {
        // 目标变化 → 唤醒 ticker（start() 会重置 elapsed 计数）。
        lastElapsedMs_ = 0.0;
        ticker_->start();
    }
    return driver.value();
}

double ImplicitlyAnimatedWidgetState::tweenValue(const std::string& key) const {
    auto it = tweens_.find(key);
    return it == tweens_.end() ? 0.0 : it->second.driver->value();
}

void ImplicitlyAnimatedWidgetState::restartTween(const std::string& key, double from,
                                                double to) {
    auto it = tweens_.find(key);
    if (it == tweens_.end())
        return;
    auto& driver = *it->second.driver;
    driver.resetTo(from);
    driver.setTarget(to);
    if (ticker_ && !ticker_->isActive()) {
        lastElapsedMs_ = 0.0;
        ticker_->start();
    }
}

bool ImplicitlyAnimatedWidgetState::hasRunningTween() const {
    for (const auto& kv : tweens_) {
        if (kv.second.driver->isRunning())
            return true;
    }
    return false;
}

void ImplicitlyAnimatedWidgetState::jumpToTargets() {
    for (auto& kv : tweens_)
        kv.second.driver->jumpToTarget();
    if (ticker_ && ticker_->isActive())
        ticker_->stop();
    lastElapsedMs_ = 0.0;
}

double ImplicitlyAnimatedWidgetState::duration() const {
    auto* w = static_cast<const ImplicitlyAnimatedWidget*>(widget());
    return w ? w->duration() : 200.0;
}

Easing ImplicitlyAnimatedWidgetState::curve() const {
    auto* w = static_cast<const ImplicitlyAnimatedWidget*>(widget());
    return w ? w->curve() : Easing::EaseInOutCubic;
}

// ── TweenAnimationBuilder ───────────────────────────────────────────────────

std::unique_ptr<State> TweenAnimationBuilder::createState() {
    return std::make_unique<TweenAnimationBuilderState>();
}

SharedPtr<Widget> TweenAnimationBuilderState::build(BuildContext& context) {
    auto* w = static_cast<TweenAnimationBuilder*>(widget());
    if (!w || !w->builder())
        return nullptr;
    double v = tween("value", w->target());
    return w->builder()(context, v);
}

// ── AnimatedOpacity ─────────────────────────────────────────────────────────

std::unique_ptr<State> AnimatedOpacity::createState() {
    return std::make_unique<AnimatedOpacityState>();
}

SharedPtr<Widget> AnimatedOpacityState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedOpacity*>(widget());
    if (!w)
        return nullptr;
    double v = tween("opacity", w->opacity());
    // 透明度动画是纯绘制变化（内容不越界），包 RepaintBoundary 把
    // markNeedsPaint 隔离在本子树：flushPaint 只重录这一小片，脏区随之
    // 收敛，不再每帧重录整屏（上游对动画子树加 RepaintBoundary 的做法）。
    return cui::make_shared<RepaintBoundary>(cui::make_shared<Opacity>(v, w->child()));
}

// ── AnimatedScale ───────────────────────────────────────────────────────────

std::unique_ptr<State> AnimatedScale::createState() {
    return std::make_unique<AnimatedScaleState>();
}

SharedPtr<Widget> AnimatedScaleState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedScale*>(widget());
    if (!w)
        return nullptr;
    double v = tween("scale", w->scale());
    // 上游 AnimatedScale → ScaleTransition，alignment 默认 center：缩放绕盒子
    // 中心而非左上角。
    return cui::make_shared<Transform>(Matrix4::scale(v, v, 1.0), w->child(), Offset(),
                                       true, nullptr, w->alignment());
}

// ── AnimatedRotation ────────────────────────────────────────────────────────

std::unique_ptr<State> AnimatedRotation::createState() {
    return std::make_unique<AnimatedRotationState>();
}

SharedPtr<Widget> AnimatedRotationState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedRotation*>(widget());
    if (!w)
        return nullptr;
    double turns = tween("turns", w->turns());
    // 上游的 AnimatedRotation → RotationTransition → Transform.rotate，
    // alignment 默认 center（绕盒子中心旋转），不是 topLeft。
    return cui::make_shared<Transform>(Matrix4::rotationZ(turns * 2.0 * M_PI), w->child(),
                                       Offset(), true, nullptr, w->alignment());
}

// ── AnimatedSlide ───────────────────────────────────────────────────────────

std::unique_ptr<State> AnimatedSlide::createState() {
    return std::make_unique<AnimatedSlideState>();
}

SharedPtr<Widget> AnimatedSlideState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedSlide*>(widget());
    if (!w)
        return nullptr;
    Offset target = w->offset();
    double dx = tween("dx", target.dx);
    double dy = tween("dy", target.dy);
    // 上游语义：位移按子节点自身尺寸的比例（FractionalTranslation）。
    return cui::make_shared<FractionalTranslation>(Offset(dx, dy), w->child());
}

// ── AnimatedPadding ─────────────────────────────────────────────────────────

std::unique_ptr<State> AnimatedPadding::createState() {
    return std::make_unique<AnimatedPaddingState>();
}

SharedPtr<Widget> AnimatedPaddingState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedPadding*>(widget());
    if (!w)
        return nullptr;
    EdgeInsets target = w->padding();
    EdgeInsets out = EdgeInsets::only(
        tween("pl", target.left), tween("pt", target.top),
        tween("pr", target.right), tween("pb", target.bottom));
    return cui::make_shared<Padding>(out, w->child());
}

// ── AnimatedAlign ───────────────────────────────────────────────────────────

std::unique_ptr<State> AnimatedAlign::createState() {
    return std::make_unique<AnimatedAlignState>();
}

SharedPtr<Widget> AnimatedAlignState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedAlign*>(widget());
    if (!w)
        return nullptr;
    Alignment target = w->alignment();
    double x = tween("ax", target.x);
    double y = tween("ay", target.y);
    return cui::make_shared<Align>(x, y, w->child());
}

// ── AnimatedPositioned ──────────────────────────────────────────────────────

std::unique_ptr<State> AnimatedPositioned::createState() {
    return std::make_unique<AnimatedPositionedState>();
}

SharedPtr<Widget> AnimatedPositionedState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedPositioned*>(widget());
    if (!w)
        return nullptr;

    // 每个定位边各自插值；未给出的一端不参与（不设该边）。
    auto animated = [&](const std::string& key, const std::optional<double>& target) {
        if (!target.has_value())
            return kNone;
        return tween(key, *target);
    };

    auto pos = cui::make_shared<Positioned>(w->child());
    double l = animated("left", w->left);
    double t = animated("top", w->top);
    double r = animated("right", w->right);
    double b = animated("bottom", w->bottom);
    double wd = animated("width", w->width);
    double hgt = animated("height", w->height);

    if (l != kNone) { pos->left = l; pos->hasLeft = true; }
    if (t != kNone) { pos->top = t; pos->hasTop = true; }
    if (r != kNone) { pos->right = r; pos->hasRight = true; }
    if (b != kNone) { pos->bottom = b; pos->hasBottom = true; }
    if (wd != kNone) { pos->width = wd; pos->hasWidth = true; }
    if (hgt != kNone) { pos->height = hgt; pos->hasHeight = true; }
    return pos;
}

// ── AnimatedContainer ───────────────────────────────────────────────────────

std::unique_ptr<State> AnimatedContainer::createState() {
    return std::make_unique<AnimatedContainerState>();
}

SharedPtr<Widget> AnimatedContainerState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedContainer*>(widget());
    if (!w)
        return nullptr;

    // 首次创建进度插值器（无动画，直接到位）。
    tween("progress", 1.0);

    bool changed = false;
    if (!initialized_) {
        prevWidth_ = w->width;
        prevHeight_ = w->height;
        prevPadding_ = w->padding;
        prevMargin_ = w->margin;
        prevAlignment_ = w->alignment;
        prevDecoration_ = w->decoration;
        initialized_ = true;
    } else {
        // 记录变化：旧值成为补间起点，新值成为目标。
        if (!(prevWidth_ == w->width)) {
            beginWidth_ = prevWidth_;
            prevWidth_ = w->width;
            changed = true;
        }
        if (!(prevHeight_ == w->height)) {
            beginHeight_ = prevHeight_;
            prevHeight_ = w->height;
            changed = true;
        }
        if (!(prevPadding_.left == w->padding.left && prevPadding_.top == w->padding.top &&
              prevPadding_.right == w->padding.right &&
              prevPadding_.bottom == w->padding.bottom)) {
            beginPadding_ = prevPadding_;
            prevPadding_ = w->padding;
            changed = true;
        }
        if (!(prevMargin_.left == w->margin.left && prevMargin_.top == w->margin.top &&
              prevMargin_.right == w->margin.right &&
              prevMargin_.bottom == w->margin.bottom)) {
            beginMargin_ = prevMargin_;
            prevMargin_ = w->margin;
            changed = true;
        }
        if (prevAlignment_ != w->alignment) {
            beginAlignment_ = prevAlignment_;
            prevAlignment_ = w->alignment;
            changed = true;
        }
        if (!(prevDecoration_.color == w->decoration.color &&
              prevDecoration_.borderRadius == w->decoration.borderRadius &&
              prevDecoration_.borderWidth == w->decoration.borderWidth &&
              prevDecoration_.borderColor == w->decoration.borderColor &&
              prevDecoration_.elevation == w->decoration.elevation)) {
            beginDecoration_ = prevDecoration_;
            prevDecoration_ = w->decoration;
            changed = true;
        }
    }

    if (changed)
        restartTween("progress", 0.0, 1.0);
    double p = tween("progress", 1.0);

    // 尺寸：缺省的一端直接取另一端（不做过渡，与上游 null 语义一致）。
    double width = kNone;
    if (w->width.has_value())
        width = lerpOptional(beginWidth_.value_or(*w->width), beginWidth_.has_value(),
                             *w->width, true, p);
    double height = kNone;
    if (w->height.has_value())
        height = lerpOptional(beginHeight_.value_or(*w->height), beginHeight_.has_value(),
                              *w->height, true, p);

    EdgeInsets padding = lerpEdgeInsets(beginPadding_, prevPadding_, p);
    EdgeInsets margin = lerpEdgeInsets(beginMargin_, prevMargin_, p);

    SharedPtr<Container> box = cui::make_shared<Container>(w->child());
    if (width != kNone)
        box->width(width);
    if (height != kNone)
        box->height(height);
    box->padding(padding);
    box->margin(margin);
    box->decoration(lerpBoxDecoration(beginDecoration_, prevDecoration_, p));

    if (w->alignment.has_value()) {
        Alignment begin = beginAlignment_.value_or(*w->alignment);
        Alignment cur = lerpAlignment(begin, *w->alignment, p);
        box->alignment(cur.x, cur.y);
    }
    return box;
}

// ── AnimatedSwitcher ────────────────────────────────────────────────────────

SharedPtr<Widget> AnimatedSwitcher::defaultTransitionBuilder(SharedPtr<Widget> child,
                                                                  double animationValue) {
    // 上游：FadeTransition(key: ValueKey<Key?>(child.key), opacity: animation, child: child)。
    // 本仓 builder 收到的是「当帧数值」而不是 Animation 对象，且外面一定会套
    // KeyedSubtree（见 transitionFor），所以这里不再给 Opacity 补 key。
    return cui::make_shared<Opacity>(animationValue, std::move(child));
}

SharedPtr<Widget> AnimatedSwitcher::defaultLayoutBuilder(
    SharedPtr<Widget> currentChild, std::vector<SharedPtr<Widget>> previousChildren) {
    // 上游：Stack(alignment: Alignment.center, children: [...previous, ?current])
    // —— 旧的先铺、新的叠在最上面，Stack 取所有子中最大者。
    std::vector<SharedPtr<Widget>> kids = std::move(previousChildren);
    if (currentChild)
        kids.push_back(std::move(currentChild));
    return cui::make_shared<Stack>(std::move(kids), nullptr,
                                   RenderStack::Alignment::MiddleCenter, RenderStack::Fit::Loose);
}

std::unique_ptr<State> AnimatedSwitcher::createState() {
    return std::make_unique<AnimatedSwitcherState>();
}

void AnimatedSwitcherState::initState() {
    // 一个 State 只有一条时间线，逐帧推进 current + 所有 outgoing（上游是每个
    // entry 一个 AnimationController，由 TickerProvider 各自挂 ticker）。
    ticker_ = std::make_unique<Ticker>([this](Duration elapsed) {
        double dt = elapsed.count() - lastElapsedMs_;
        lastElapsedMs_ = elapsed.count();
        if (dt < 0.0)
            dt = 0.0;

        auto advance = [dt](const SharedPtr<Entry>& e) {
            const double before = e->driver->value();
            e->driver->advance(dt);
            return e->driver->value() != before;
        };
        bool changed = current_ ? advance(current_) : false;
        for (const auto& e : outgoing_)
            changed = advance(e) || changed;

        // 上游 status 监听（dismissed）在 setState 之后，摘除要和推进同帧完成。
        dropSettledOutgoing();

        bool running = current_ && current_->driver->isRunning();
        for (const auto& e : outgoing_)
            running = running || e->driver->isRunning();
        if (!running) {
            // 收尾这一帧携带精确终点值，仍要重建一次，否则子树停在倒数第二帧。
            ticker_->stop();
            lastElapsedMs_ = 0.0;
        }
        if (changed && mounted())
            setState();
    });

    // 上游 initState → _addEntryForNewChild(animate: false)：首个子直接到位，不入场。
    addEntryForNewChild(false);
}

void AnimatedSwitcherState::didChangeDependencies() {
    if (context() && ticker_)
        tickerMode_.bind(*context(), *ticker_);
}

void AnimatedSwitcherState::dispose() {
    tickerMode_.unbind();
    if (ticker_ && ticker_->isActive())
        ticker_->stop();
    ticker_.reset();
    // 上游逐个 controller.dispose()；本仓 driver 无外部资源，交给 shared_ptr。
    outgoing_.clear();
    current_.reset();
}

void AnimatedSwitcherState::addEntryForNewChild(bool animate) {
    auto* w = static_cast<AnimatedSwitcher*>(widget());
    if (!w)
        return;

    if (current_) {
        // 上游 `controller.reverse()`：当前 entry 转头淡出，用的是 switchOutCurve。
        // driver 从「当前插值结果」起补间，所以中途被打断也是视觉接续的。
        outgoing_.push_back(current_);
        current_->driver->setCurve(w->switchOutCurve());
        current_->driver->setTarget(0.0);
        current_.reset();
    }
    if (w->child()) {
        auto e = cui::make_shared<Entry>();
        e->widgetChild = w->child();
        // 上游 `KeyedSubtree.wrap(builder(child, animation), _childNumber)`：
        // 子自带 key 时按 key 认人（同一个 key 再次进来 ⇒ 与旧子同 key，会被
        // build 里的去重过滤掉），否则用 childNumber 造一份唯一身份。
        if (e->widgetChild->key())
            e->transitionKey = cui::make_shared<const ValueKey<KeyPtr>>(e->widgetChild->key());
        else
            e->transitionKey = cui::make_shared<const ValueKey<int>>(childNumber_);
        // entry 在创建时抓住 duration/曲线 —— 上游注释明说「改 duration 不影响
        // 进行中的过渡」。
        e->driver = cui::make_shared<ImplicitTweenDriver>(animate ? 0.0 : 1.0, w->duration(),
                                                          w->switchInCurve());
        if (animate)
            e->driver->setTarget(1.0);
        current_ = std::move(e);
    }
    // 上游同款：child == null ⇒ 只淡出旧的，不起新 entry。

    // 上游每个 entry 一个 controller（reverse()/forward() 各自 start 一个 ticker）。
    // 本仓一条 ticker 驱动所有 entry，所以只要有任何 driver 在跑就唤醒它 ——
    // child==null 只剩淡出、或旧子被打断重新转头，都靠这里起表。
    bool anyRunning = current_ && current_->driver->isRunning();
    for (const auto& old : outgoing_)
        anyRunning = anyRunning || old->driver->isRunning();
    if (anyRunning && ticker_ && !ticker_->isActive()) {
        lastElapsedMs_ = 0.0;
        ticker_->start();
    }
}

void AnimatedSwitcherState::dropSettledOutgoing() {
    outgoing_.erase(std::remove_if(outgoing_.begin(), outgoing_.end(),
                                   [](const SharedPtr<Entry>& e) {
                                       return !e->driver->isRunning() && e->driver->value() == 0.0;
                                   }),
                    outgoing_.end());
}

SharedPtr<Widget> AnimatedSwitcherState::transitionFor(const SharedPtr<Entry>& entry,
                                                             double value) {
    auto* w = static_cast<AnimatedSwitcher*>(widget());
    const AnimatedSwitcherTransitionBuilder& builder = w->transitionBuilder();
    SharedPtr<Widget> built =
        builder ? builder(entry->widgetChild, value)
                : AnimatedSwitcher::defaultTransitionBuilder(entry->widgetChild, value);
    return cui::make_shared<KeyedSubtree>(entry->transitionKey, std::move(built));
}

SharedPtr<Widget> AnimatedSwitcherState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedSwitcher*>(widget());
    if (!w)
        return nullptr;

    // 上游把这段判定放在 didUpdateWidget；本仓宿主经常原地改同一个 Widget 对象
    // 的 child（gallery 与测试都这么驱动），那条回调走不到，所以放在 build 开头。
    // 语义不变：一次 build 内完成 entry 换班，再按新 entry 出图。
    const bool hasNew = (w->child() != nullptr);
    const bool hasOld = (current_ != nullptr);
    if (hasNew != hasOld || (hasNew && !w->child()->canUpdate(*current_->widgetChild))) {
        // 换子（类型或 key 变了）：旧的转为淡出，新的淡入。
        ++childNumber_;
        addEntryForNewChild(true);
    } else if (hasOld) {
        // 同类型同 key：只换内容，不做过渡 —— 但要把新配置写回 entry，
        // 上游 `_currentEntry.widgetChild = widget.child` 同款。
        current_->widgetChild = w->child();
    }

    SharedPtr<Widget> currentChild;
    const KeyPtr currentKey = current_ ? current_->transitionKey : KeyPtr();
    std::vector<SharedPtr<Widget>> previous;
    for (const auto& e : outgoing_) {
        // 上游 build 里的 where：与 current 同 key 的 outgoing 不重复进列表。
        if (currentKey && e->transitionKey && *e->transitionKey == *currentKey)
            continue;
        previous.push_back(transitionFor(e, e->driver->value()));
    }
    if (current_)
        currentChild = transitionFor(current_, current_->driver->value());

    const AnimatedSwitcherLayoutBuilder& layout = w->layoutBuilder();
    return layout ? layout(std::move(currentChild), std::move(previous))
                  : AnimatedSwitcher::defaultLayoutBuilder(std::move(currentChild),
                                                           std::move(previous));
}

// ── AnimatedCrossFade ───────────────────────────────────────────────────────

namespace {

/// 上游的两个常量 key：`ValueKey<CrossFadeState>(showFirst/showSecond)`。
/// 挂在 `Positioned` 上 ⇒ 交换 top/bottom 时 Element 按 key 复用子树。
const KeyPtr& crossFadeKey(CrossFadeState which) {
    static const KeyPtr kFirst = cui::make_shared<const ValueKey<CrossFadeState>>(
        CrossFadeState::ShowFirst);
    static const KeyPtr kSecond = cui::make_shared<const ValueKey<CrossFadeState>>(
        CrossFadeState::ShowSecond);
    return which == CrossFadeState::ShowFirst ? kFirst : kSecond;
}

}  // namespace

std::unique_ptr<State> AnimatedCrossFade::createState() {
    return std::make_unique<AnimatedCrossFadeState>();
}

SharedPtr<Widget> AnimatedCrossFade::defaultLayoutBuilder(SharedPtr<Widget> topChild,
                                                               KeyPtr topChildKey,
                                                               SharedPtr<Widget> bottomChild,
                                                               KeyPtr bottomChildKey) {
    std::vector<SharedPtr<Widget>> kids;
    if (bottomChild) {
        // Positioned(key: bottomChildKey, left: 0, top: 0, right: 0, child: bottomChild)
        // ⇒ 横向拉满、不锁高度。
        auto pos = cui::make_shared<Positioned>(std::move(bottomChild), std::move(bottomChildKey));
        pos->hasLeft = pos->hasTop = pos->hasRight = true;
        kids.push_back(std::move(pos));
    }
    if (topChild) {
        // Positioned(key: topChildKey, child: topChild) ⇒ 无定位值 ⇒ 非定位子，
        // 按 Stack 的 fit 摆放（本仓 Positioned::applyParentData 写 hasPosition()）。
        kids.push_back(
            cui::make_shared<Positioned>(std::move(topChild), std::move(topChildKey)));
    }
    auto stack = cui::make_shared<Stack>(std::move(kids));
    stack->setFit(RenderStack::Fit::Loose);
    return stack;
}

SharedPtr<Widget> AnimatedCrossFadeState::build(BuildContext& context) {
    auto* w = static_cast<AnimatedCrossFade*>(widget());
    if (!w)
        return nullptr;

    const bool showSecond = w->crossFadeState() == CrossFadeState::ShowSecond;
    // 基类 curve 已钉成 Linear ⇒ p 就是上游的 `controller.value`（对时间线性）。
    // 首次 tween() 直接落在目标上，对应上游 initState 里
    // `if (crossFadeState == showSecond) _controller.value = 1.0`。
    const double p = tween("progress", showSecond ? 1.0 : 0.0);

    // firstCurve 作用后整体取反（上游 `_initAnimation(curve, inverted: true)`）。
    const double firstOpacity = 1.0 - applyEasing(p, w->firstCurve());
    const double secondOpacity = applyEasing(p, w->secondCurve());

    // 上游按 `_controller.isForwardOrCompleted` 选 top/bottom；单值 0→1 控制器
    // 上它等价于「这一段的目标是不是 1」⇒ 就是 crossFadeState。
    SharedPtr<Widget> topChild = showSecond ? w->second() : w->first();
    SharedPtr<Widget> bottomChild = showSecond ? w->first() : w->second();
    const double topOpacity = showSecond ? secondOpacity : firstOpacity;
    const double bottomOpacity = showSecond ? firstOpacity : secondOpacity;

    if (topChild)
        topChild = cui::make_shared<Opacity>(topOpacity, std::move(topChild));
    if (bottomChild) {
        // 淡出者不吃命中（上游 IgnorePointer 包在 FadeTransition 外面）。
        bottomChild = cui::make_shared<IgnorePointer>(
            true, cui::make_shared<Opacity>(bottomOpacity, std::move(bottomChild)));
    }

    auto layout = (w->layoutBuilder() ? w->layoutBuilder()
                                      : &AnimatedCrossFade::defaultLayoutBuilder)(
        std::move(topChild), crossFadeKey(showSecond ? CrossFadeState::ShowSecond
                                                     : CrossFadeState::ShowFirst),
        std::move(bottomChild), crossFadeKey(showSecond ? CrossFadeState::ShowFirst
                                                        : CrossFadeState::ShowSecond));

    return cui::make_shared<ClipRect>(cui::make_shared<AnimatedSize>(
        std::move(layout), w->alignment(), w->duration(), w->sizeCurve(), w->clipBehavior()));
}

void AnimatedCrossFadeState::onTweenSettled() {
    auto* w = static_cast<AnimatedCrossFade*>(widget());
    if (w && w->onEnd())
        w->onEnd()();
}

}  // namespace cui_core
