#pragma once
/// @file animation.h
/// @brief 上游-style animation system.
///
/// Core architecture (mirrors the upstream animation framework):
///   - Animation<T>: 上游 animation/animation.dart 的只读动画值接口
///     （`extends Listenable` + `value` + `status` + `addStatusListener`）
///   - AnimationController: 上游 animation/controller.dart（本文件里曾叫
///     `Animation`，2026-11-03 改名让位给上面的抽象）
///   - Tween<T>: separates value interpolation from timing
///   - SpringSimulation: real spring physics (mass/stiffness/damping)
///   - CurvedAnimation / ReverseAnimation / ProxyAnimation:
///     上游 curved_animation.dart / animations.dart
///   - AnimatedValue<T>: implicit animation (auto-transitions on setValue)
///
/// Usage:
///   // Tween + AnimationController (one controller, multiple tweens)
///   auto ctrl = cui_core::make_shared<AnimationController>(300);
///   ctrl->setEasing(Easing::EaseOutCubic);
///   Tween<double> opacityTween(0.0, 1.0);
///   Tween<Color> colorTween(Color::fromRGB(1,0,0), Color::fromRGB(0,0,1));
///   // In paint: opacity = opacityTween.evaluate(*ctrl);
///
///   // Spring physics
///   SpringSimulation spring(1.0, 100.0, 10.0);  // mass, stiffness, damping
///   spring.start(0.0);  // spring from 0 to end (default 1)
///
///   // Implicit animation
///   AnimatedValue<double> opacity(1.0);
///   opacity.setValue(0.5);  // auto-transitions from 1.0 to 0.5
///
/// ── 与 上游 的有意偏离（勿当 bug 排查）────────────────────────────────────
///   1. **AnimationController 自带 Easing**。上游 controller.value 恒为线性，
///      曲线一律由 CurvedAnimation 施加；本类的 `progress()` 会把内部
///      `easing_` 折进去，故 `value() == progress()`。默认 `Easing::Linear`，
///      所以「controller + CurvedAnimation」这条 上游 路线结果一致。
///      `CurvedAnimation` 施加在 `parent->value()` 上（= 上游 口径）。
///   2. **无 vsync / TickerProvider**。控制器不自带 Ticker，由持有者推进
///      （`tick(Duration)` 或 `update()`）；`SharedAnimation` 是本仓那条
///      「自动挂 Ticker」的包装，两者分工不变。
///   3. **监听器走句柄**。`std::function` 无法比较捕获 `this` 的 lambda，
///      故 `addStatusListener` 返回 int 句柄、`removeStatusListener(int)`
///      精确摘除（同 foundation/change_notifier.h 的既定口径）。
///   4. **`Tween<T>::animate()` 返回 `SharedPtr<Animation<T>>`**，而 上游 返回
///      一个轻量 `_AnimatableEvaluation`；本仓的动画一律按 SharedPtr 流动。
///   5. **转发件保留自己的监听表**。上游 的 `CurvedAnimation` /
///      `ReverseAnimation` 混入 `AnimationWithParentMixin`，`addListener` 与
///      `addStatusListener` 直接注册到 parent 上；本仓改为「自己订阅 parent +
///      自己派发」，于是 `statusListenerCount()` 的口径是「挂在这个动画上的
///      数量」，且摘除一律走句柄（见偏离 3）。行为等价，只是多一跳。

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "cui/foundation/geometry.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/ticker.h"
#include "cui/reactive/observable.h"
#include "cui/reactive/signal.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {

/// Easing functions for animation curves
enum class Easing {
    Linear,
    EaseInQuad,
    EaseOutQuad,
    EaseInOutQuad,
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
    EaseInQuart,
    EaseOutQuart,
    EaseInOutQuart,
    EaseInQuint,
    EaseOutQuint,
    EaseInOutQuint,
    EaseInSine,
    EaseOutSine,
    EaseInOutSine,
    EaseInExpo,
    EaseOutExpo,
    EaseInOutExpo,
    EaseInCirc,
    EaseOutCirc,
    EaseInOutCirc,
    EaseInBack,
    EaseOutBack,
    EaseInOutBack,
    Spring  // spring-like bounce with overshoot
};

/// Apply easing function to a progress value [0, 1]
double applyEasing(double progress, Easing easing);

/// 上游 `Curve.flipped`：`transform(t) == 1.0 - curve.transform(1.0 - t)`。
/// 对任意曲线都成立，因此不需要逐条维护 In/Out 映射表。
inline double applyFlippedEasing(double t, Easing easing) {
    return 1.0 - applyEasing(1.0 - t, easing);
}

/// Animation playback status (mirrors the upstream's AnimationStatus).
/// 上游 animation/animation.dart:224。
enum class AnimationStatus {
    Dismissed,   ///< Not yet started or reset.
    Forward,     ///< Playing forward (0 → 1).
    Reverse,     ///< Playing in reverse (1 → 0).
    Completed,   ///< Finished playing forward.
};

// ── Animation<T> ────────────────────────────────────────────────────────────

/// 只读、可订阅的动画值。上游的 `Animation<T>`（animation/animation.dart）。
///
/// 上游 `abstract class Animation<T> extends Listenable`；本仓用
/// `ChangeNotifier` 兑现 Listenable 通道（于是同时拿到句柄式摘除能力）。
/// 控件层接收 `SharedPtr<Animation<double>>`，因此 controller、
/// CurvedAnimation、ReverseAnimation、ProxyAnimation 可互换喂给同一个控件
/// —— 这正是 Hero / 路由转场需要的那层抽象。
template <typename T>
class Animation : public ChangeNotifier {
public:
    /// 上游的 `Animation.value`。
    virtual T value() const = 0;

    /// 上游的 `Animation.status`。
    virtual AnimationStatus status() const { return status_; }

    /// 上游的 `Animation.isAnimating`。
    virtual bool isAnimating() const = 0;

    bool isCompleted() const { return status() == AnimationStatus::Completed; }
    bool isDismissed() const { return status() == AnimationStatus::Dismissed; }

    /// 上游的 `Animation.isForwardOrCompleted`（animation/animation.dart:346）。
    ///
    /// `ModalRoute._buildModalBarrier` 用它决定「入场这一段的目标是不是 1」，
    /// `AnimatedCrossFade` 的 top/bottom 选择也是同一条（见「AnimatedCrossFade」
    /// 节）：单值 0→1 控制器上它等价于「这一段要走向上界」。
    bool isForwardOrCompleted() const {
        return status() == AnimationStatus::Forward ||
               status() == AnimationStatus::Completed;
    }

    /// 上游的 `addStatusListener`；返回句柄供 `removeStatusListener` 使用。
    int addStatusListener(std::function<void(AnimationStatus)> listener) {
        int handle = nextStatusHandle_++;
        statusListeners_.emplace_back(handle, std::move(listener));
        return handle;
    }

    /// 按句柄摘除状态监听器。句柄无效时忽略。
    void removeStatusListener(int handle) {
        for (auto it = statusListeners_.begin(); it != statusListeners_.end(); ++it) {
            if (it->first == handle) {
                statusListeners_.erase(it);
                return;
            }
        }
    }

    /// 状态监听器数量（测试用，口径同 `listenerCount()`）。
    size_t statusListenerCount() const { return statusListeners_.size(); }

protected:
    /// 上游 `Animation` 的默认实现把 `toString()` 收在子类；这里只留存储。
    AnimationStatus status_ = AnimationStatus::Dismissed;

    /// 状态监听器（句柄 + 回调），口径同 `ChangeNotifier` 的 `ListenerEntry`。
    std::vector<std::pair<int, std::function<void(AnimationStatus)>>> statusListeners_;
    int nextStatusHandle_ = 1;

    /// 子类在状态迁移时调用：先派发状态监听（上游 `_notifyStatusListeners`
    /// 早于 `_notifyListeners`），快照迭代以便回调里安全摘除。
    void dispatchStatus(AnimationStatus next) {
        if (next == status_)
            return;
        status_ = next;
        auto snapshot = statusListeners_;
        for (auto& entry : snapshot) {
            if (entry.second)
                entry.second(next);
        }
    }

    /// 上游的 `dispose()`：清掉所有监听器，防止后续 tick 打到悬挂回调。
    void clearStatusListeners() { statusListeners_.clear(); }

    Animation() = default;
    ~Animation() override { dispose(); }

    Animation(const Animation&) = delete;
    Animation& operator=(const Animation&) = delete;
};

// ── AnimationController ─────────────────────────────────────────────────────

/// Core animation controller with timing and progress tracking.
/// Drives progress from 0→1 over a duration. Supports looping, auto-reverse,
/// delay, pause/resume, and physics-based fling.
///
/// 上游的 `AnimationController`（animation/controller.dart）。本仓曾把它命名
/// 为 `Animation`；2026-11-03 让位给上面的抽象接口。
class AnimationController : public Animation<double> {
public:
    explicit AnimationController(double durationMs = 300.0);
    ~AnimationController() override = default;

    // Configuration
    void setDuration(double ms) { durationMs_ = ms; }
    double duration() const { return durationMs_; }

    void setEasing(Easing easing) { easing_ = easing; }
    Easing easing() const { return easing_; }

    void setDelay(double ms) { delayMs_ = ms; }
    double delay() const { return delayMs_; }

    void setLoop(bool loop) { loop_ = loop; }
    bool loop() const { return loop_; }

    void setAutoReverse(bool reverse) { autoReverse_ = reverse; }
    bool autoReverse() const { return autoReverse_; }

    // ── 上游 AnimationController 的「双向段」语义（批次B2 补）──────────────
    //
    // 上游 controller 是一段「从当前 value 走向某个界」的行程：`forward()` 走向
    // `upperBound`、`reverse()` 走向 `lowerBound`、`value = v` 直接跳。本仓此前
    // 只有「0→1 的时间线 + reversed_ 仅作方向标签」，所以 `reverse()` 实际上
    // 仍在往上走（`SharedAnimation::reverse` / chart / liquid_glass 要的正是
    // 往下走），路由转场必须要真反向，故补齐成上游口径。

    /// 上游的 `AnimationController.upperBound` / `lowerBound`（默认 1.0 / 0.0）。
    double upperBound() const { return upperBound_; }
    double lowerBound() const { return lowerBound_; }
    void setBounds(double lower, double upper) {
        lowerBound_ = lower;
        upperBound_ = upper;
    }

    /// 上游的 `AnimationController.reverseDuration`。
    /// 未显式设置（< 0）时取 `duration()`，与上游的默认口径一致。
    void setReverseDuration(double ms) { reverseDurationMs_ = ms; }
    double reverseDuration() const { return reverseDurationMs_ >= 0 ? reverseDurationMs_ : durationMs_; }

    /// 上游的 `AnimationController.value = v`：立即跳到 v，并按端点派发状态
    /// （到下界 ⇒ Dismissed，到上界 ⇒ Completed，中间不改状态）。
    void setValue(double v);

    /// 上游的 `AnimationController.forward({double? from})`：从 `from`
    /// （默认「当前值」，与上游 `animateTo(upperBound)` 的 begin 口径一致）
    /// 走向上界，时长取 `duration()`。
    void forward(std::optional<double> from = std::nullopt);

    /// 上游的 `AnimationController.reverse({double? from})`：从 `from`
    /// （默认当前值）走向下界，时长取 `reverseDuration()`。
    void reverse(std::optional<double> from = std::nullopt);

    // Control
    void start();
    void stop();
    void pause();
    void resume();
    void reset();

    /// Repeat continuously between 0 and 1.
    void repeat();

    /// Physics-based fling with initial velocity and friction.
    /// Uses an internal SpringSimulation; not bound by duration.
    void fling(double velocity, double friction = 1.0);

    bool isRunning() const { return running_; }
    bool isPaused() const { return paused_; }

    /// 上游的 `AnimationController.isAnimating`。
    bool isAnimating() const override { return running_ && !paused_; }

    /// Progress [0, 1] with easing applied. During fling, returns the
    /// spring simulation's raw value (may exceed [0,1] during overshoot).
    double progress() const;

    /// 上游的 `AnimationController.value`（见文件头偏离 1：本类的 value 含
    /// 内部 easing，故等于 `progress()`）。
    double value() const override { return progress(); }

    /// Raw linear progress [0, 1] without easing. For use by CurvedAnimation.
    double rawProgress() const;

    /// Raw elapsed time in ms.
    double elapsed() const { return elapsedMs_; }

    /// 当前状态对应的原始进度方向（测试与调试用）。
    bool reversed() const { return reversed_; }

    // Observable streams
    Observable<double>& progressChanged() { return progressChanged_; }
    Signal<>& finished() { return finished_; }
    Signal<>& started() { return started_; }

    /// Update must be called each frame (typically in paint).
    /// Returns true if animation is still active.
    bool update();

    /// Tick the animation with an explicit elapsed time (from Ticker).
    /// This is the preferred way to drive animations in the three-layer
    /// architecture — called by Ticker in handleBeginFrame, BEFORE layout/paint.
    /// Returns true if animation is still active.
    /// @note finished() is emitted BEFORE the last progressChanged() of the
    /// same call. A handler that drops the final shared_ptr reference must
    /// not let the object die here — callers owning the Animation should hold
    /// a local shared_ptr across tick() (e.g. `if (auto a = anim_) a->tick(e);`).
    bool tick(Duration elapsed);

    /// 释放：摘掉所有监听器（上游 `AnimationController.dispose`）。
    void disposeController() {
        dispose();
        clearStatusListeners();
    }

private:
    /// 每帧推进后统一派发：progressChanged → Listenable → 状态监听。
    /// 上游 `set value`/`_tick` 都是「先 notifyListeners() 再 _checkStatusChanged()」，
    /// 顺序反了会让 DualTransitionBuilder 在换 proxy parent 之后才收到末帧值。
    /// `value` 为 nullopt 时取 `progress()`；fling 路径按 Ticker 的时间自己算，故显式传入。
    void emitFrame(std::optional<double> value = std::nullopt);

    double durationMs_;
    double delayMs_ = 0;
    Easing easing_ = Easing::Linear;
    bool loop_ = false;
    bool autoReverse_ = false;

    /// 上游的 upperBound / lowerBound（本仓的 value 恒夹在这两者之间）。
    double upperBound_ = 1.0;
    double lowerBound_ = 0.0;
    /// `reverse()` 段的时长；负值表示「未设置 ⇒ 取 durationMs_」。
    double reverseDurationMs_ = -1.0;

    /// 当前这一段行程的取值区间：`progress()` =
    /// `segFrom_ + (segTo_ - segFrom_) * easing(t)`。loop / autoReverse 两条
    /// 路径不走这个映射（那两条里 progressRaw_ 本身就是值）。
    double segFrom_ = 0.0;
    double segTo_ = 1.0;
    /// 当前这一段的时长（forward 用 durationMs_，reverse 用 reverseDuration()）。
    double segmentDurationMs_ = 0.0;
    /// 静止（未启动 / reset / setValue 之后）时的值。
    double heldValue_ = 0.0;

    std::chrono::steady_clock::time_point startTime_;
    double elapsedMs_ = 0;

    /// 当前有效原始进度 [0,1]（已按 loop / autoReverse 折算）。
    /// 由 computeProgress() 维护；progress() / rawProgress() 读取它。
    double progressRaw_ = 0.0;

    /// tick() 路径的时间基准：动画开始时对应的 Ticker elapsed。
    /// Ticker 给的是「自 ticker 启动起的绝对时间」，动画可能晚于 ticker
    /// 启动才 start()，因此需要一个基准把两者对齐。
    double tickBaseMs_ = 0.0;
    double lastTickElapsedMs_ = 0.0;

    /// 依据「已播放总时长」计算有效进度（loop / autoReverse / 普通三种情形
    /// 统一在此处理）。返回 false 表示动画已结束。
    bool computeProgress(double totalMs);
    bool running_ = false;
    bool paused_ = false;
    bool reversed_ = false;

    // Fling (spring physics) state
    struct FlingState {
        bool active = false;
        double velocity = 0;
        double friction = 1.0;
        double startValue = 0;
        std::chrono::steady_clock::time_point startTime;
    };
    FlingState fling_;

    Observable<double> progressChanged_;
    Signal<> finished_;
    Signal<> started_;
};

// ── Tween<T> ────────────────────────────────────────────────────────────────

/// 上游 `Animatable<T>` 的最小对应物：一个「按 t 求值」的东西。
/// `Tween<T>` 是它最常见的实现。
template <typename T>
class Animatable {
public:
    virtual ~Animatable() = default;

    /// 上游的 `Animatable.transform(t)`。
    virtual T lerp(double t) const = 0;

    /// 上游的 `Animatable.evaluate(animation)` —— 只读接口收 `Animation<double>`。
    virtual T evaluate(const Animation<double>& animation) const {
        return lerp(animation.value());
    }
};

/// Interpolates between two values. Separates value mapping from timing.
/// One AnimationController can drive multiple Tweens simultaneously.
///
/// 上游的 `Tween<T>`（animation/tween.dart）。
template <typename T>
class Tween : public Animatable<T> {
public:
    Tween(T begin, T end) : begin_(std::move(begin)), end_(std::move(end)) {}

    /// Linear interpolation at parameter t (typically [0,1], but may extrapolate).
    T lerp(double t) const override;

    /// Evaluate this tween at the animation's current value.
    T evaluate(const Animation<double>& anim) const override { return lerp(anim.value()); }

    /// 上游的 `Tween.animate(parent)`：得到随 parent 每帧求值的动画值。
    SharedPtr<Animation<T>> animate(SharedPtr<Animation<double>> parent) const;

    /// 上游的 `Tween.flipped`：交换 begin/end。
    Tween<T> flipped() const { return Tween<T>(end_, begin_); }

    void setBegin(T b) { begin_ = std::move(b); }
    void setEnd(T e) { end_ = std::move(e); }
    const T& begin() const { return begin_; }
    const T& end() const { return end_; }

private:
    T begin_;
    T end_;
};

/// 上游的 `RectTween`（animation/tween.dart 里 `Tween<Rect>` 的具名版）。
/// Hero 的 `createRectTween` 回调要求返回一个可 `evaluate` 的 `Tween<Rect>`，
/// 这里以子类形式给出同名类型，便于 `std::function<SharedPtr<Tween<Rect>>(...)`
/// 的签名与 上游 对齐。
class RectTween : public Tween<Rect> {
public:
    RectTween(std::optional<Rect> begin, std::optional<Rect> end)
        : Tween<Rect>(begin.value_or(Rect{}), end.value_or(Rect{})),
          begin_(std::move(begin)),
          end_(std::move(end)) {}

    const std::optional<Rect>& beginRect() const { return begin_; }
    const std::optional<Rect>& endRect() const { return end_; }

private:
    std::optional<Rect> begin_;
    std::optional<Rect> end_;
};

/// 上游的 `_TweenAnimation<T, U>`（animation/tween.dart）：把一条 `Tween<U>`
/// 挂在 `Animation<T>` 上逐帧求值。由 `Tween<U>::animate` 产出，
/// 业务代码不直接构造。
///
/// 上游 混 `AnimationWithParentMixin`（监听直接挂到 parent）；本仓按文件头
/// 偏离 5 的口径自己订阅 + 自己派发，析构时自动摘干净 —— 因此 `animate()`
/// 即使写在 build 里也不会留下悬挂监听（对象一死就解绑）。
template <typename T, typename U>
class EvaluatedAnimation : public Animation<U> {
public:
    EvaluatedAnimation(SharedPtr<Animation<T>> parent, Tween<U> tween)
        : parent_(std::move(parent)), tween_(std::move(tween)) {
        if (parent_) {
            listenerHandle_ = parent_->addListenerWithHandle([this] { this->notifyListeners(); });
            statusHandle_ = parent_->addStatusListener([this](AnimationStatus s) {
                this->dispatchStatus(s);
            });
        }
    }
    ~EvaluatedAnimation() override { disposeListener(); }

    U value() const override { return parent_ ? tween_.lerp(parent_->value()) : tween_.lerp(0.0); }

    AnimationStatus status() const override {
        return parent_ ? parent_->status() : AnimationStatus::Dismissed;
    }

    bool isAnimating() const override { return parent_ && parent_->isAnimating(); }

    const SharedPtr<Animation<T>>& parent() const { return parent_; }
    const Tween<U>& tween() const { return tween_; }

    /// 断开与 parent 的订阅（幂等）。
    void disposeListener() {
        if (parent_) {
            if (listenerHandle_ != 0)
                parent_->removeListenerByHandle(listenerHandle_);
            if (statusHandle_ != 0)
                parent_->removeStatusListener(statusHandle_);
        }
        listenerHandle_ = 0;
        statusHandle_ = 0;
    }

private:
    SharedPtr<Animation<T>> parent_;
    Tween<U> tween_;
    int listenerHandle_ = 0;
    int statusHandle_ = 0;
};

/// 上游的 `Tween<T>.animate(parent)`。
template <typename T>
SharedPtr<Animation<T>> Tween<T>::animate(SharedPtr<Animation<double>> parent) const {
    return cui::make_shared<EvaluatedAnimation<double, T>>(std::move(parent), *this);
}

// ── SpringSimulation ────────────────────────────────────────────────────────

/// Real spring physics simulation.
/// Models a damped harmonic oscillator: mass on a spring with friction.
///   - dampingRatio < 1: underdamped (oscillates/overshoots)
///   - dampingRatio = 1: critically damped (fastest non-oscillatory)
///   - dampingRatio > 1: overdamped (slow return, no oscillation)
class SpringSimulation {
public:
    SpringSimulation(double mass = 1.0, double stiffness = 100.0,
                     double damping = 10.0, double initialVelocity = 0.0);

    void setStart(double s) { start_ = s; }
    void setEnd(double e) { end_ = e; }
    void setInitialVelocity(double v) { initialVelocity_ = v; }

    /// Begin simulation from `fromValue` toward end().
    void start(double fromValue);

    /// Displacement at time t seconds after start.
    double value(double timeSeconds) const;

    /// Velocity at time t seconds after start.
    double velocity(double timeSeconds) const;

    /// True when the spring has settled within tolerance of the end value.
    bool isDone(double timeSeconds, double tolerance = 0.001) const;

    double start() const { return start_; }
    double end() const { return end_; }
    double dampingRatio() const;
    double naturalFrequency() const;

private:
    double mass_;
    double stiffness_;
    double damping_;
    double initialVelocity_;
    double start_ = 0;
    double end_ = 1;
};

/// 一条曲线。上游是 `Curve` 类层次（`Curves.*`）；本仓的曲线口径是
/// `Easing` 枚举 + `applyEasing`，但 `Curve.flipped` 没法用枚举表达，
/// 因此凡是「可能翻转」的曲线一律折成函数传递（同 `RenderAnimatedSize::CurveFn`）。
using CurveFn = std::function<double(double)>;

/// 把 `Easing` 包成 `CurveFn`。
inline CurveFn makeCurveFn(Easing easing) {
    return [easing](double t) { return applyEasing(t, easing); };
}

/// 上游的 `Curve.flipped`。
inline CurveFn flipCurveFn(const CurveFn& curve) {
    // 按值捕获：返回的 lambda 比这个形参活得久。
    return [curve](double t) { return 1.0 - curve(1.0 - t); };
}

// ── CurvedAnimation ─────────────────────────────────────────────────────────

/// Applies an easing curve to a parent animation's value.
/// Decouples curve selection from the controller, so the same controller
/// can drive different values with different curves.
///
/// 上游的 `CurvedAnimation`（animation/animations.dart:387）。
class CurvedAnimation : public Animation<double> {
public:
    CurvedAnimation(SharedPtr<Animation<double>> parent, CurveFn curve,
                    std::optional<CurveFn> reverseCurve = std::nullopt);
    /// 便捷版：直接给 `Easing`（既有调用点用法）。
    CurvedAnimation(SharedPtr<Animation<double>> parent, Easing curve);
    ~CurvedAnimation() override { disposeListener(); }

    /// Curved progress: applies the easing curve to the parent's value.
    /// 上游 `CurvedAnimation.value`：端点 0/1 直接返回（曲线端点保护），
    /// 反向时取 `reverseCurve`（为 null 时正反向同曲线，见 上游 文档）。
    double value() const override;

    /// 既有调用点的别名（本类曾只有 `progress()`）。
    double progress() const { return value(); }

    /// 上游 `AnimationWithParentMixin.status` —— 直接转发 parent。
    AnimationStatus status() const override;

    bool isAnimating() const override;

    const CurveFn& curve() const { return curve_; }
    void setCurve(CurveFn c) { curve_ = std::move(c); }
    const std::optional<CurveFn>& reverseCurve() const { return reverseCurve_; }
    void setReverseCurve(std::optional<CurveFn> c) { reverseCurve_ = std::move(c); }

    const SharedPtr<Animation<double>>& parent() const { return parent_; }

    /// 上游 `CurvedAnimation.dispose`：断开与 parent 的订阅。
    void disposeListener();

private:
    /// 上游 `_updateCurveDirection`：只在真正开始反向播放时才切曲线，
    /// 中途换向不切，避免数值跳变。
    void updateCurveDirection(AnimationStatus status);
    bool useForwardCurve() const;
    void onParentTick();
    void onParentStatus(AnimationStatus status);

    SharedPtr<Animation<double>> parent_;
    CurveFn curve_;
    std::optional<CurveFn> reverseCurve_;
    /// 上游 `_curveDirection`：当前这一段用的是正曲线还是反曲线。
    std::optional<AnimationStatus> curveDirection_;
    int listenerHandle_ = 0;
    int statusHandle_ = 0;
};

// ── Interval ────────────────────────────────────────────────────────────────

/// Maps a sub-range [begin, end] of a parent animation's progress to [0, 1].
/// Outside the interval, the output is clamped to 0 (before) or 1 (after).
/// Optionally applies an easing curve within the interval.
///
/// 上游 equivalent: `Interval(begin, end, curve: ...)`
///
/// Usage:
///   Interval fadeInterval(0.0, 0.3);   // fades in during first 30%
///   Interval slideInterval(0.2, 1.0);  // slides in from 20% to 100%
///   double fadeProgress  = fadeInterval.map(ctrl->rawProgress());
///   double slideProgress = slideInterval.map(ctrl->rawProgress());
class Interval {
public:
    Interval(double begin, double end, Easing curve = Easing::Linear);

    /// Map parent progress value to local [0, 1] progress.
    /// Returns 0 before begin, 1 after end, and interpolated value within.
    double map(double parentProgress) const;

    double begin() const { return begin_; }
    double end() const { return end_; }
    Easing curve() const { return curve_; }

private:
    double begin_;
    double end_;
    Easing curve_;
};

// ── ReverseAnimation ────────────────────────────────────────────────────────

/// Wraps a parent animation and returns the reversed value.
/// When parent is at 0.3, ReverseAnimation returns 0.7; when parent is at 1.0,
/// ReverseAnimation returns 0.0.
///
/// 上游 equivalent: `ReverseAnimation(controller)`（animation/animations.dart）。
/// 与 上游 一致：status 取 parent 的镜像（Forward↔Reverse、Completed↔Dismissed）。
class ReverseAnimation : public Animation<double> {
public:
    explicit ReverseAnimation(SharedPtr<Animation<double>> parent);
    ~ReverseAnimation() override { disposeListener(); }

    /// Reversed value: 1.0 - parent->value().
    double value() const override;

    /// 既有调用点的别名。
    double progress() const { return value(); }

    AnimationStatus status() const override;

    bool isAnimating() const override;

    const SharedPtr<Animation<double>>& parent() const { return parent_; }

    void disposeListener();

private:
    /// 上游 `_reverseStatus`：Forward↔Reverse、Completed↔Dismissed。
    static AnimationStatus reverseStatus(AnimationStatus status);
    void onParentTick();
    void onParentStatus(AnimationStatus status);

    SharedPtr<Animation<double>> parent_;
    int listenerHandle_ = 0;
    int statusHandle_ = 0;
};

// ── ProxyAnimation ──────────────────────────────────────────────────────────

/// 上游的 `ProxyAnimation`（animation/animations.dart）：一个「parent 可换」
/// 的转发动画。Hero 的 `_HeroFlight` 靠它在 push/pop 之间把飞行中的动画
/// 从一条路由动画改挂到另一条（含 `ReverseAnimation` 包裹），而不必重建
/// 订阅关系。
class ProxyAnimation : public Animation<double> {
public:
    ProxyAnimation() = default;
    explicit ProxyAnimation(SharedPtr<Animation<double>> parent);
    ~ProxyAnimation() override { disposeListener(); }

    /// 上游的 `ProxyAnimation.parent` setter：换 parent 时重新订阅。
    void setParent(SharedPtr<Animation<double>> parent);
    const SharedPtr<Animation<double>>& parent() const { return parent_; }

    /// 无 parent 时 上游 恒返回 0.0（`kAlwaysDismissedAnimation`）。
    double value() const override;

    AnimationStatus status() const override;

    bool isAnimating() const override;

    void disposeListener();

private:
    void onParentTick();
    void onParentStatus(AnimationStatus status);

    SharedPtr<Animation<double>> parent_;
    /// 上游 `_status` / `_value`：换 parent 的那一瞬用来判定「值/状态是否真的
    /// 变了」，只有变了才补发一次通知（避免订阅方看到重复帧）。
    std::optional<AnimationStatus> detachedStatus_;
    std::optional<double> detachedValue_;
    int listenerHandle_ = 0;
    int statusHandle_ = 0;
};

// ── AlwaysStoppedAnimation ──────────────────────────────────────────────────

/// 永不运动的常量动画。上游的 `AlwaysStoppedAnimation<T>`（animation.dart）。
///
/// 路由转场用它表达「这条路由根本没有入场/退场」：把 `ProxyAnimation` 的
/// parent 换成 `kAlwaysCompleteAnimation()`，订阅关系不用重建。
class AlwaysStoppedAnimation : public Animation<double> {
public:
    explicit AlwaysStoppedAnimation(double value) : value_(value) {}

    double value() const override { return value_; }

    /// 上游的 `AnimationStatus get status => _status`，而 `_status` 由
    /// `AlwaysStoppedAnimation` 的构造定为「值在哪个端点就在哪个状态」。
    AnimationStatus status() const override {
        return value_ > 0.0 ? AnimationStatus::Completed : AnimationStatus::Dismissed;
    }

    bool isAnimating() const override { return false; }

    void disposeListener() {}

private:
    double value_;
};

/// 上游的 `kAlwaysCompleteAnimation`（= `AlwaysStoppedAnimation<double>(1.0)`）。
inline const SharedPtr<Animation<double>>& kAlwaysCompleteAnimation() {
    static const SharedPtr<Animation<double>> sValue =
            cui::make_shared<AlwaysStoppedAnimation>(1.0);
    return sValue;
}

/// 上游的 `kAlwaysDismissedAnimation`（= `AlwaysStoppedAnimation<double>(0.0)`）。
inline const SharedPtr<Animation<double>>& kAlwaysDismissedAnimation() {
    static const SharedPtr<Animation<double>> sValue =
            cui::make_shared<AlwaysStoppedAnimation>(0.0);
    return sValue;
}

// ── TweenSequence<T> ────────────────────────────────────────────────────────

/// A weighted sequence of Tweens evaluated as a continuous animation.
/// Each segment occupies a fraction of the total duration proportional to
/// its weight. The output is the value from the active segment at its local
/// progress, with an optional per-segment easing curve.
///
/// 上游 equivalent: `TweenSequence([TweenSequenceItem(...), ...])`
///
/// Usage:
///   TweenSequence<double> seq;
///   seq.add({Tween<double>(0.0, 100.0), 1.0, Easing::EaseOut});   // first half
///   seq.add({Tween<double>(100.0, 50.0), 0.5, Easing::EaseIn});    // second half
///   double val = seq.valueAt(ctrl->rawProgress());
template <typename T>
class TweenSequence {
public:
    struct Item {
        Tween<T> tween;
        double weight;
        Easing easing;

        Item(Tween<T> tw, double w, Easing e = Easing::Linear)
            : tween(std::move(tw)), weight(w), easing(e) {}
    };

    TweenSequence() = default;
    explicit TweenSequence(std::vector<Item> items) : items_(std::move(items)) {}

    void add(Tween<T> tween, double weight, Easing easing = Easing::Linear) {
        items_.emplace_back(std::move(tween), weight, easing);
    }

    /// Evaluate the sequence at the given overall progress [0, 1].
    T valueAt(double t) const;

    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    void clear() { items_.clear(); }

private:
    std::vector<Item> items_;
};

// ── Spring Presets ──────────────────────────────────────────────────────────

/// Pre-defined spring configurations matching the upstream's material springs.
struct SpringPreset {
    double mass;
    double stiffness;
    double damping;

    /// Create a SpringSimulation from this preset with given start/end values.
    SpringSimulation toSimulation(double fromValue = 0.0,
                                  double toValue = 1.0,
                                  double initialVelocity = 0.0) const {
        SpringSimulation sim(mass, stiffness, damping, initialVelocity);
        sim.setEnd(toValue);
        sim.start(fromValue);
        return sim;
    }
};

namespace springs {
    /// Default spring: moderate bounce, settles in ~1s.
    constexpr SpringPreset kDefaultSpring   = {1.0, 100.0, 10.0};
    /// Stiff spring: minimal overshoot, fast settle.
    constexpr SpringPreset kStiffSpring     = {1.0, 180.0, 26.0};
    /// Bouncy spring: noticeable overshoot, playful feel.
    constexpr SpringPreset kBouncySpring    = {1.0, 120.0, 8.0};
    /// Smooth spring: critically damped, no overshoot.
    constexpr SpringPreset kSmoothSpring    = {1.0, 150.0, 24.5};
    /// Gentle spring: slow, soft approach.
    constexpr SpringPreset kGentleSpring    = {1.0, 80.0, 14.0};
}  // namespace springs

// ── AnimatedValue<T> ────────────────────────────────────────────────────────

/// Implicit animation: automatically tweens from current to target value.
/// Call setValue() to change the target; the value smoothly transitions
/// using the configured duration and easing.
template <typename T>
class AnimatedValue {
public:
    explicit AnimatedValue(T initialValue, double durationMs = 300.0,
                           Easing easing = Easing::EaseInOutCubic);

    /// Current interpolated value.
    T value() const { return current_; }

    /// Target value.
    T target() const { return target_; }

    /// Set a new target; auto-transitions from the current value.
    void setValue(T target);

    /// Set a new target with custom transition parameters.
    void setValue(T target, double durationMs, Easing easing);

    bool isAnimating() const { return anim_.isRunning(); }

    /// Advance the animation (call each frame, e.g. in paint()).
    void update();

    /// Tick the animation with an explicit elapsed time (from Ticker).
    /// This is the preferred way to drive animations in the three-layer
    /// architecture — called by Ticker in handleBeginFrame, BEFORE layout/paint.
    void tick(Duration elapsed);

    Signal<>& completed() { return completed_; }

    /// 内部控制器（上游 `AnimatedWidget.controller` 的对应物）。
    AnimationController& animation() { return anim_; }

private:
    T current_;
    T target_;
    T from_;
    Tween<T> tween_;
    AnimationController anim_;
    Signal<> completed_;
};

// AnimatedValue specializations
template<>
double Tween<double>::lerp(double t) const;
template<>
Color Tween<Color>::lerp(double t) const;
template<>
Point Tween<Point>::lerp(double t) const;
template<>
Size Tween<Size>::lerp(double t) const;
template<>
Rect Tween<Rect>::lerp(double t) const;

// Convenience functions
inline SharedPtr<AnimationController> fadeIn(double durationMs = 300) {
    return cui::make_shared<AnimationController>(durationMs);
}

inline SharedPtr<AnimationController> slideIn(double durationMs = 300) {
    auto anim = cui::make_shared<AnimationController>(durationMs);
    anim->setEasing(Easing::EaseOutCubic);
    return anim;
}

}  // namespace cui_core
