/// @file animation.cpp — 上游-style animation system implementation.
#include "cui/widgets/animation/animation.h"
#include "cui/foundation/shared_ptr.h"

#include <algorithm>
#include <cmath>

namespace cui {

// ── Easing Functions ─────────────────────────────────────────────────────────

double applyEasing(double t, Easing easing) {
    switch (easing) {
        case Easing::Linear:
            return t;
        case Easing::EaseInQuad:
            return t * t;
        case Easing::EaseOutQuad:
            return t * (2 - t);
        case Easing::EaseInOutQuad:
            return t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t;
        case Easing::EaseInCubic:
            return t * t * t;
        case Easing::EaseOutCubic: {
            double u = t - 1;
            return u * u * u + 1;
        }
        case Easing::EaseInOutCubic:
            return t < 0.5 ? 4 * t * t * t
                           : (t - 1) * (2 * t - 2) * (2 * t - 2) + 1;
        case Easing::EaseInQuart:
            return t * t * t * t;
        case Easing::EaseOutQuart: {
            double u = t - 1;
            return 1 - u * u * u * u;
        }
        case Easing::EaseInOutQuart:
            t *= 2;
            if (t < 1) return 0.5 * t * t * t * t;
            t -= 2;
            return -0.5 * (t * t * t * t - 2);
        case Easing::EaseInQuint:
            return t * t * t * t * t;
        case Easing::EaseOutQuint: {
            double u = t - 1;
            return u * u * u * u * u + 1;
        }
        case Easing::EaseInOutQuint:
            t *= 2;
            if (t < 1) return 0.5 * t * t * t * t * t;
            t -= 2;
            return 0.5 * (t * t * t * t * t + 2);
        case Easing::EaseInSine:
            return 1 - std::cos(t * M_PI / 2);
        case Easing::EaseOutSine:
            return std::sin(t * M_PI / 2);
        case Easing::EaseInOutSine:
            return -0.5 * (std::cos(M_PI * t) - 1);
        case Easing::EaseInExpo:
            return (t == 0) ? 0 : std::pow(2, 10 * (t - 1));
        case Easing::EaseOutExpo:
            return (t == 1) ? 1 : 1 - std::pow(2, -10 * t);
        case Easing::EaseInOutExpo:
            if (t == 0 || t == 1) return t;
            if (t < 0.5) return 0.5 * std::pow(2, 20 * t - 10);
            return 1 - 0.5 * std::pow(2, -20 * t + 10);
        case Easing::EaseInCirc:
            return 1 - std::sqrt(1 - t * t);
        case Easing::EaseOutCirc: {
            double u = t - 1;
            return std::sqrt(1 - u * u);
        }
        case Easing::EaseInOutCirc:
            t *= 2;
            if (t < 1) return -0.5 * (std::sqrt(1 - t * t) - 1);
            t -= 2;
            return 0.5 * (std::sqrt(1 - t * t) + 1);
        case Easing::EaseInBack: {
            const double s = 1.70158;
            return t * t * ((s + 1) * t - s);
        }
        case Easing::EaseOutBack: {
            const double s = 1.70158;
            double u = t - 1;
            return u * u * ((s + 1) * u + s) + 1;
        }
        case Easing::EaseInOutBack: {
            const double s = 1.70158 * 1.525;
            t *= 2;
            if (t < 1) return 0.5 * (t * t * ((s + 1) * t - s));
            t -= 2;
            return 0.5 * (t * t * ((s + 1) * t + s) + 2);
        }
        case Easing::Spring: {
            const double frequency = 3.0;
            const double decay = 0.5;
            if (t < 0.5) return 2 * t * t;
            double progress = (t - 0.5) * 2;
            double bounce = std::sin(progress * M_PI * frequency) *
                            std::exp(-decay * progress * 10);
            return 0.5 + 0.5 * progress + 0.1 * bounce;
        }
        default:
            return t;
    }
}

// ── AnimationController ─────────────────────────────────────────────────────────────────

AnimationController::AnimationController(double durationMs)
    : durationMs_(durationMs), segmentDurationMs_(durationMs) {
}

void AnimationController::start() {
    startTime_ = std::chrono::steady_clock::now();
    elapsedMs_ = 0;
    running_ = true;
    paused_ = false;
    reversed_ = false;
    progressRaw_ = 0.0;
    fling_.active = false;
    // 0→1 段（上游 的 `start()` 语义在本仓 = 「从头播到尾」）。
    segFrom_ = lowerBound_;
    segTo_ = upperBound_;
    segmentDurationMs_ = durationMs_;
    heldValue_ = lowerBound_;
    // tick() 用的是 Ticker 的绝对时间；记录当前基准，使本动画的播放时长
    // 从此刻开始（动画可能晚于 Ticker 启动才开始）。
    tickBaseMs_ = lastTickElapsedMs_;
    dispatchStatus(AnimationStatus::Forward);
    started_.emit();
}

void AnimationController::stop() {
    // 上游 `AnimationController.stop()` 不改 status：暂停/中断后仍保留
    // 最后一次迁移的方向，供 Hero 这类观察者判断飞行是否仍在进行。
    if (running_ && !fling_.active) {
        // 冻结当前值（progress() 之后依赖 elapsedMs_ ≠ 0 走映射路径，这里
        // 顺手把「静止态取值」同步好，供 reset / setValue 与外部查询使用）。
        heldValue_ = progress();
    }
    running_ = false;
    paused_ = false;
    fling_.active = false;
}

void AnimationController::pause() {
    if (running_ && !paused_) {
        paused_ = true;
    }
}

void AnimationController::resume() {
    if (paused_) {
        auto now = std::chrono::steady_clock::now();
        auto elapsedDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(elapsedMs_));
        startTime_ = now - elapsedDuration;
        if (fling_.active) {
            auto flingElapsed = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(elapsedMs_));
            fling_.startTime = now - flingElapsed;
        }
        paused_ = false;
    }
}

void AnimationController::reset() {
    running_ = false;
    paused_ = false;
    elapsedMs_ = 0;
    reversed_ = false;
    progressRaw_ = 0.0;
    fling_.active = false;
    segFrom_ = lowerBound_;
    segTo_ = upperBound_;
    segmentDurationMs_ = durationMs_;
    heldValue_ = lowerBound_;
    dispatchStatus(AnimationStatus::Dismissed);
}

void AnimationController::repeat() {
    loop_ = true;
    autoReverse_ = true;
    start();
}

void AnimationController::setValue(double v) {
    // 上游 的 `set value(double newValue)`：先 stop（不改 status），再跳到
    // 目标值，只有正好落在端点时才迁移 status（Dismissed / Completed）。
    stop();
    const double clamped = std::clamp(v, lowerBound_, upperBound_);
    elapsedMs_ = 0;
    progressRaw_ = 0.0;
    heldValue_ = clamped;
    segFrom_ = clamped;
    segTo_ = clamped;
    if (clamped == lowerBound_ || clamped == upperBound_) {
        // 上游 `set value`：notifyListeners() 先于 _checkStatusChanged()。
        // 顺序反了的话，监听者在 DualTransitionBuilder 换掉 ProxyAnimation
        // 的 parent 之后才收到「值已到端点」的通知，那个已经摘掉的 proxy
        // 再也不会重建，末帧就停在倒数第二帧。
        notifyListeners();
        dispatchStatus(clamped == lowerBound_ ? AnimationStatus::Dismissed
                                               : AnimationStatus::Completed);
        return;
    }
    notifyListeners();
}

void AnimationController::forward(std::optional<double> from) {
    // 上游：`from` 缺省时 begin = 当前 value（animateTo(upperBound) 的口径）。
    const double begin = std::clamp(from.value_or(progress()), lowerBound_, upperBound_);
    // 上游 的 forward() 会取消循环；已在终点时直接完成（不挂 Ticker）。
    loop_ = false;
    autoReverse_ = false;
    if (begin >= upperBound_) {
        setValue(upperBound_);
        return;
    }
    start();
    segFrom_ = begin;
    segTo_ = upperBound_;
    segmentDurationMs_ = durationMs_;
    heldValue_ = begin;
    progressRaw_ = 0.0;
}

void AnimationController::reverse(std::optional<double> from) {
    const double begin = std::clamp(from.value_or(progress()), lowerBound_, upperBound_);
    loop_ = false;
    autoReverse_ = false;
    if (begin <= lowerBound_) {
        setValue(lowerBound_);
        return;
    }
    stop();
    startTime_ = std::chrono::steady_clock::now();
    elapsedMs_ = 0;
    running_ = true;
    paused_ = false;
    reversed_ = true;
    fling_.active = false;
    progressRaw_ = 0.0;
    segFrom_ = begin;
    segTo_ = lowerBound_;
    segmentDurationMs_ = reverseDuration();
    heldValue_ = begin;
    tickBaseMs_ = lastTickElapsedMs_;
    dispatchStatus(AnimationStatus::Reverse);
    started_.emit();
}

void AnimationController::fling(double velocity, double friction) {
    fling_.active = true;
    fling_.velocity = velocity;
    fling_.friction = friction;
    fling_.startValue = progress();
    fling_.startTime = std::chrono::steady_clock::now();
    running_ = true;
    paused_ = false;
    reversed_ = velocity < 0.0;
    dispatchStatus(reversed_ ? AnimationStatus::Reverse : AnimationStatus::Forward);
    started_.emit();
}

double AnimationController::rawProgress() const {
    if (fling_.active) return progress();
    if (!running_ && elapsedMs_ == 0) return 0;
    // 有效进度已由 computeProgress() 按 loop / autoReverse 折算好。
    return progressRaw_;
}

double AnimationController::progress() const {
    // Fling mode: physics-based progress from spring simulation
    if (fling_.active) {
        auto now = std::chrono::steady_clock::now();
        double t = std::chrono::duration<double>(now - fling_.startTime).count();
        // Simple deceleration: value = startValue + velocity * t * e^(-friction * t)
        double decay = std::exp(-fling_.friction * t);
        return fling_.startValue + fling_.velocity * t * decay;
    }

    if (!running_ && elapsedMs_ == 0)
        return heldValue_;

    const double eased = applyEasing(progressRaw_, easing_);
    // loop / autoReverse 两条路径里 progressRaw_ 本身就是「值」（来回折算过），
    // 只有单向行程段才需要 segFrom_ → segTo_ 的映射。
    if (loop_ || autoReverse_)
        return eased;
    return segFrom_ + (segTo_ - segFrom_) * eased;
}

void AnimationController::emitFrame(std::optional<double> value) {
    AnimationStatus next;
    if (running_)
        next = reversed_ ? AnimationStatus::Reverse : AnimationStatus::Forward;
    else
        next = reversed_ ? AnimationStatus::Dismissed : AnimationStatus::Completed;
    // 上游 `_tick`：先 notifyListeners()，再 _checkStatusChanged()。末帧的
    // 状态切换（Forward→Completed）会换掉 DualTransitionBuilder 的 proxy
    // parent，值通知必须发生在换之前，否则旧 proxy 子树丢掉末帧。
    progressChanged_.next(value.value_or(progress()));
    notifyListeners();
    dispatchStatus(next);
}

bool AnimationController::update() {
    if (!running_ || paused_) {
        return running_;
    }

    auto now = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(now - startTime_).count();

    // ── Fling mode ──────────────────────────────────────────────────────
    if (fling_.active) {
        double t = std::chrono::duration<double>(now - fling_.startTime).count();
        double decay = std::exp(-fling_.friction * t);
        double speed = std::abs(fling_.velocity * decay * (1.0 - fling_.friction * t));

        if (speed < 0.001) {
            // Fling settled
            double last = fling_.startValue + fling_.velocity * t * decay;
            running_ = false;
            fling_.active = false;
            dispatchStatus(last <= 0.0 ? AnimationStatus::Dismissed : AnimationStatus::Completed);
            finished_.emit();
            return false;
        }

        double p = fling_.startValue + fling_.velocity * t * decay;
        emitFrame(p);
        return true;
    }

    // 进度按「已播放总时长」统一计算（loop / autoReverse 都在里面处理）。
    elapsedMs_ = totalMs;
    if (!computeProgress(totalMs)) {
        running_ = false;
        elapsedMs_ = segmentDurationMs_ + delayMs_;
        finished_.emit();
        // 结束帧同样要通知一次（finished 先于最后一次 progressChanged）。
        emitFrame();
        return false;
    }

    emitFrame();
    return true;
}

bool AnimationController::tick(Duration elapsed) {
    if (!running_ || paused_) {
        return running_;
    }

    // Ticker 给的是「自 ticker 启动起的绝对时间」，减去 start() 时记录的基准
    // 才是本动画自己的播放时长（动画可能晚于 ticker 启动才开始）。
    lastTickElapsedMs_ = elapsed.count();
    double totalMs = lastTickElapsedMs_ - tickBaseMs_;
    if (totalMs < 0.0)
        totalMs = 0.0;

    // ── Fling mode ──────────────────────────────────────────────────────
    if (fling_.active) {
        double t = totalMs / 1000.0;  // convert ms to seconds
        double decay = std::exp(-fling_.friction * t);
        double speed = std::abs(fling_.velocity * decay * (1.0 - fling_.friction * t));

        if (speed < 0.001) {
            // Fling settled
            double last = fling_.startValue + fling_.velocity * t * decay;
            running_ = false;
            fling_.active = false;
            dispatchStatus(last <= 0.0 ? AnimationStatus::Dismissed : AnimationStatus::Completed);
            finished_.emit();
            return false;
        }

        double p = fling_.startValue + fling_.velocity * t * decay;
        emitFrame(p);
        return true;
    }

    // 进度按「已播放总时长」统一计算（loop / autoReverse 都在里面处理）。
    elapsedMs_ = totalMs;
    if (!computeProgress(totalMs)) {
        running_ = false;
        elapsedMs_ = segmentDurationMs_ + delayMs_;
        finished_.emit();
        // 结束帧同样要通知一次（finished 先于最后一次 progressChanged）。
        emitFrame();
        return false;
    }

    emitFrame();
    return true;
}

// ── computeProgress ─────────────────────────────────────────────────────────

bool AnimationController::computeProgress(double totalMs) {
    if (totalMs < delayMs_)
        return true;  // 延迟中

    double total = totalMs - delayMs_;
    if (segmentDurationMs_ <= 0.0) {
        progressRaw_ = 1.0;
        return loop_;  // 零时长行程一帧就走完（只有循环模式才继续挂着）
    }

    if (loop_) {
        if (autoReverse_) {
            // 来回摆动：正向一个周期 + 反向一个周期。
            double period = durationMs_ * 2.0;
            double phase = std::fmod(total, period);
            if (phase <= durationMs_) {
                reversed_ = false;
                progressRaw_ = phase / durationMs_;
            } else {
                reversed_ = true;
                progressRaw_ = (period - phase) / durationMs_;
            }
        } else {
            // 单向循环：每周期结束回到起点继续。
            reversed_ = false;
            progressRaw_ = std::fmod(total, durationMs_) / durationMs_;
        }
        return true;
    }

    if (autoReverse_) {
        if (!reversed_) {
            // 正向阶段。
            if (total >= durationMs_) {
                reversed_ = true;
                progressRaw_ = 1.0;
            } else {
                progressRaw_ = total / durationMs_;
            }
            return true;
        }
        // 反向阶段：1 → 0。
        double back = total - durationMs_;
        if (back >= durationMs_) {
            progressRaw_ = 0.0;
            return false;  // 来回结束
        }
        progressRaw_ = 1.0 - back / durationMs_;
        return true;
    }

    if (total >= segmentDurationMs_) {
        progressRaw_ = 1.0;
        return false;  // 播完（segTo_ 由 progress() 的映射给出）
    }
    progressRaw_ = segmentDurationMs_ > 0.0 ? total / segmentDurationMs_ : 1.0;
    return true;
}

// ── Tween<T> Specializations ─────────────────────────────────────────────────

template<>
double Tween<double>::lerp(double t) const {
    return begin_ + (end_ - begin_) * t;
}

template<>
Color Tween<Color>::lerp(double t) const {
    return Color::fromRGBA(
        begin_.r + (end_.r - begin_.r) * t,
        begin_.g + (end_.g - begin_.g) * t,
        begin_.b + (end_.b - begin_.b) * t,
        begin_.a + (end_.a - begin_.a) * t
    );
}

template<>
Point Tween<Point>::lerp(double t) const {
    return Point{
        begin_.x + (end_.x - begin_.x) * t,
        begin_.y + (end_.y - begin_.y) * t
    };
}

template<>
Size Tween<Size>::lerp(double t) const {
    return Size{
        begin_.width + (end_.width - begin_.width) * t,
        begin_.height + (end_.height - begin_.height) * t
    };
}

template<>
Rect Tween<Rect>::lerp(double t) const {
    return Rect{
        begin_.x + (end_.x - begin_.x) * t,
        begin_.y + (end_.y - begin_.y) * t,
        begin_.width + (end_.width - begin_.width) * t,
        begin_.height + (end_.height - begin_.height) * t
    };
}

// ── SpringSimulation ─────────────────────────────────────────────────────────

SpringSimulation::SpringSimulation(double mass, double stiffness,
                                   double damping, double initialVelocity)
    : mass_(mass)
    , stiffness_(stiffness)
    , damping_(damping)
    , initialVelocity_(initialVelocity) {
}

void SpringSimulation::start(double fromValue) {
    start_ = fromValue;
}

double SpringSimulation::naturalFrequency() const {
    return std::sqrt(stiffness_ / mass_);
}

double SpringSimulation::dampingRatio() const {
    return damping_ / (2.0 * std::sqrt(stiffness_ * mass_));
}

double SpringSimulation::value(double t) const {
    double x0 = start_ - end_;  // initial displacement from equilibrium
    double v0 = initialVelocity_;
    double w0 = naturalFrequency();
    double zeta = dampingRatio();

    double displacement;

    if (zeta < 0.9999) {
        // Underdamped — oscillates with exponential decay
        double wd = w0 * std::sqrt(1.0 - zeta * zeta);
        double envelope = std::exp(-zeta * w0 * t);
        displacement = envelope * (x0 * std::cos(wd * t) +
                                   (v0 + zeta * w0 * x0) / wd * std::sin(wd * t));
    } else if (zeta <= 1.0001) {
        // Critically damped — fastest return without oscillation
        double envelope = std::exp(-w0 * t);
        displacement = (x0 + (v0 + w0 * x0) * t) * envelope;
    } else {
        // Overdamped — slow return, no oscillation
        double sqrtTerm = std::sqrt(zeta * zeta - 1.0);
        double r1 = (-zeta + sqrtTerm) * w0;
        double r2 = (-zeta - sqrtTerm) * w0;
        double denom = r1 - r2;
        if (std::abs(denom) < 1e-12) {
            // Degenerate case — treat as critically damped
            double envelope = std::exp(-w0 * t);
            displacement = (x0 + (v0 + w0 * x0) * t) * envelope;
        } else {
            double c2 = (v0 - r1 * x0) / denom;
            double c1 = x0 - c2;
            displacement = c1 * std::exp(r1 * t) + c2 * std::exp(r2 * t);
        }
    }

    return end_ + displacement;
}

double SpringSimulation::velocity(double t) const {
    // Numerical differentiation — accurate enough for all use cases
    constexpr double dt = 1e-5;
    return (value(t + dt) - value(t - dt)) / (2.0 * dt);
}

bool SpringSimulation::isDone(double t, double tolerance) const {
    double disp = std::abs(value(t) - end_);
    double vel = std::abs(velocity(t));
    return disp < tolerance && vel < tolerance;
}

// ── CurvedAnimation ──────────────────────────────────────────────────────────

CurvedAnimation::CurvedAnimation(SharedPtr<Animation<double>> parent, CurveFn curve,
                                 std::optional<CurveFn> reverseCurve)
    : parent_(std::move(parent))
    , curve_(std::move(curve))
    , reverseCurve_(std::move(reverseCurve)) {
    if (parent_) {
        // 上游 构造时就按 parent 当前 status 锁定方向，再挂监听。
        updateCurveDirection(parent_->status());
        statusHandle_ =
            parent_->addStatusListener([this](AnimationStatus s) { onParentStatus(s); });
        listenerHandle_ = parent_->addListenerWithHandle([this] { onParentTick(); });
    }
}

CurvedAnimation::CurvedAnimation(SharedPtr<Animation<double>> parent, Easing curve)
    : CurvedAnimation(std::move(parent), makeCurveFn(curve), std::nullopt) {
}

void CurvedAnimation::updateCurveDirection(AnimationStatus status) {
    // 上游：`_curveDirection = status.isAnimating ? _curveDirection ?? status : null`
    // —— 一段播放期间方向锁死，中途换向不切曲线，避免数值跳变。
    if (status == AnimationStatus::Forward || status == AnimationStatus::Reverse) {
        if (!curveDirection_.has_value())
            curveDirection_ = status;
    } else {
        curveDirection_ = std::nullopt;
    }
}

bool CurvedAnimation::useForwardCurve() const {
    // 没有 reverseCurve 时，正反向都用正曲线。
    if (!reverseCurve_.has_value())
        return true;
    AnimationStatus direction =
        curveDirection_.value_or(parent_ ? parent_->status() : AnimationStatus::Dismissed);
    return direction != AnimationStatus::Reverse;
}

double CurvedAnimation::value() const {
    if (!parent_)
        return 0.0;
    const CurveFn& active = useForwardCurve() ? curve_ : *reverseCurve_;
    const double t = parent_->value();
    // 上游 端点保护：曲线必须把 0 映到 0、1 映到 1，这里直接短路返回，
    // 免得弹性曲线的 overshoot 在端点留下 0.9999。
    if (t == 0.0 || t == 1.0)
        return t;
    return active(t);
}

AnimationStatus CurvedAnimation::status() const {
    return parent_ ? parent_->status() : AnimationStatus::Dismissed;
}

bool CurvedAnimation::isAnimating() const { return parent_ && parent_->isAnimating(); }

void CurvedAnimation::onParentTick() { notifyListeners(); }

void CurvedAnimation::onParentStatus(AnimationStatus status) {
    updateCurveDirection(status);
    dispatchStatus(status);
}

void CurvedAnimation::disposeListener() {
    if (parent_) {
        if (listenerHandle_ != 0)
            parent_->removeListenerByHandle(listenerHandle_);
        if (statusHandle_ != 0)
            parent_->removeStatusListener(statusHandle_);
    }
    listenerHandle_ = 0;
    statusHandle_ = 0;
}

// ── Interval ─────────────────────────────────────────────────────────────────

Interval::Interval(double begin, double end, Easing curve)
    : begin_(begin), end_(end), curve_(curve) {
}

double Interval::map(double parentProgress) const {
    if (end_ <= begin_) return 0.0;
    if (parentProgress <= begin_) return 0.0;
    if (parentProgress >= end_) return 1.0;
    double localT = (parentProgress - begin_) / (end_ - begin_);
    return applyEasing(localT, curve_);
}

// ── ReverseAnimation ─────────────────────────────────────────────────────────

ReverseAnimation::ReverseAnimation(SharedPtr<Animation<double>> parent)
    : parent_(std::move(parent)) {
    if (parent_) {
        listenerHandle_ = parent_->addListenerWithHandle([this] { onParentTick(); });
        statusHandle_ = parent_->addStatusListener([this](AnimationStatus s) { onParentStatus(s); });
    }
}

double ReverseAnimation::value() const {
    if (!parent_)
        return 1.0;
    return 1.0 - parent_->value();
}

AnimationStatus ReverseAnimation::status() const {
    if (!parent_)
        return AnimationStatus::Completed;
    return reverseStatus(parent_->status());
}

bool ReverseAnimation::isAnimating() const { return parent_ && parent_->isAnimating(); }

AnimationStatus ReverseAnimation::reverseStatus(AnimationStatus status) {
    // 上游 的 `_reverseStatus`：Forward 与 Reverse 互换、Completed 与 Dismissed 互换。
    switch (status) {
        case AnimationStatus::Forward:
            return AnimationStatus::Reverse;
        case AnimationStatus::Reverse:
            return AnimationStatus::Forward;
        case AnimationStatus::Completed:
            return AnimationStatus::Dismissed;
        case AnimationStatus::Dismissed:
            return AnimationStatus::Completed;
    }
    return status;
}

void ReverseAnimation::onParentTick() { notifyListeners(); }

void ReverseAnimation::onParentStatus(AnimationStatus status) {
    dispatchStatus(reverseStatus(status));
}

void ReverseAnimation::disposeListener() {
    if (parent_) {
        if (listenerHandle_ != 0)
            parent_->removeListenerByHandle(listenerHandle_);
        if (statusHandle_ != 0)
            parent_->removeStatusListener(statusHandle_);
    }
    listenerHandle_ = 0;
    statusHandle_ = 0;
}

// ── ProxyAnimation ─────────────────────────────────────────────────────────

ProxyAnimation::ProxyAnimation(SharedPtr<Animation<double>> parent)
    : parent_(std::move(parent)) {
    if (parent_) {
        // 上游 构造时只赋 parent（监听是 lazy 的），本仓没有 lazy 订阅机制，
        // 于是构造时就订阅 —— 语义等价，只是提前挂了两个句柄。
        listenerHandle_ = parent_->addListenerWithHandle([this] { onParentTick(); });
        statusHandle_ = parent_->addStatusListener([this](AnimationStatus s) { onParentStatus(s); });
    } else {
        // 上游 `ProxyAnimation()`：无 parent 即 status=dismissed、value=0。
        detachedStatus_ = AnimationStatus::Dismissed;
        detachedValue_ = 0.0;
    }
}

void ProxyAnimation::setParent(SharedPtr<Animation<double>> parent) {
    if (parent.get() == parent_.get())
        return;

    // 上游 的 parent setter：先摘掉旧订阅、把旧 parent 的值与状态存成快照，
    // 换上新 parent 之后只有值或状态真的变了才补发一次通知。
    if (parent_) {
        detachedStatus_ = parent_->status();
        detachedValue_ = parent_->value();
        disposeListener();
    }
    parent_ = std::move(parent);
    if (parent_) {
        listenerHandle_ = parent_->addListenerWithHandle([this] { onParentTick(); });
        statusHandle_ = parent_->addStatusListener([this](AnimationStatus s) { onParentStatus(s); });
        if (!detachedValue_ || *detachedValue_ != parent_->value())
            notifyListeners();
        if (detachedStatus_ && *detachedStatus_ != parent_->status())
            dispatchStatus(parent_->status());
        // 快照只在「换 parent 的那一瞬」有意义，接上新 parent 后即失效。
        detachedStatus_ = std::nullopt;
        detachedValue_ = std::nullopt;
    }
    // 换到 null 时保留快照：上游 的 value/status getter 就靠它兜底。
}

double ProxyAnimation::value() const {
    // 无 parent 时用换 parent 时留下的快照值兜底。
    return parent_ ? parent_->value() : detachedValue_.value_or(0.0);
}

AnimationStatus ProxyAnimation::status() const {
    return parent_ ? parent_->status() : detachedStatus_.value_or(AnimationStatus::Dismissed);
}

bool ProxyAnimation::isAnimating() const { return parent_ && parent_->isAnimating(); }

void ProxyAnimation::onParentTick() { notifyListeners(); }

void ProxyAnimation::onParentStatus(AnimationStatus status) { dispatchStatus(status); }

void ProxyAnimation::disposeListener() {
    if (parent_) {
        if (listenerHandle_ != 0)
            parent_->removeListenerByHandle(listenerHandle_);
        if (statusHandle_ != 0)
            parent_->removeStatusListener(statusHandle_);
    }
    listenerHandle_ = 0;
    statusHandle_ = 0;
}

// ── TweenSequence<T> ─────────────────────────────────────────────────────────

template<typename T>
T TweenSequence<T>::valueAt(double t) const {
    if (items_.empty()) return T{};
    if (t <= 0.0) return items_.front().tween.begin();

    // Compute total weight
    double totalWeight = 0.0;
    for (const auto& item : items_) {
        totalWeight += item.weight;
    }
    if (totalWeight <= 0.0) return items_.front().tween.begin();

    // Clamp to [0, 1] and scale to total weight
    double clampedT = std::min(1.0, t);
    double target = clampedT * totalWeight;

    // Find the active segment
    double accumulated = 0.0;
    for (const auto& item : items_) {
        double segEnd = accumulated + item.weight;
        if (target <= segEnd || &item == &items_.back()) {
            // Compute local progress within this segment
            double localT = (item.weight > 0.0)
                ? (target - accumulated) / item.weight
                : 1.0;
            localT = std::min(1.0, std::max(0.0, localT));
            // Apply segment easing
            double easedT = applyEasing(localT, item.easing);
            return item.tween.lerp(easedT);
        }
        accumulated = segEnd;
    }
    // Fallback (should never reach here)
    return items_.back().tween.end();
}

// Explicit instantiations for common types (same reason as AnimatedValue)
template class TweenSequence<double>;
template class TweenSequence<Color>;
template class TweenSequence<Point>;

// ── AnimatedValue<T> ─────────────────────────────────────────────────────────

template<typename T>
AnimatedValue<T>::AnimatedValue(T initialValue, double durationMs, Easing easing)
    : current_(initialValue)
    , target_(initialValue)
    , from_(initialValue)
    , tween_(initialValue, initialValue)
    , anim_(durationMs) {
    anim_.setEasing(easing);
    anim_.finished().connect([this]() {
        completed_.emit();
    });
}

template<typename T>
void AnimatedValue<T>::setValue(T target) {
    from_ = current_;
    target_ = target;
    tween_.setBegin(from_);
    tween_.setEnd(target_);
    anim_.reset();
    anim_.start();
}

template<typename T>
void AnimatedValue<T>::setValue(T target, double durationMs, Easing easing) {
    anim_.setDuration(durationMs);
    anim_.setEasing(easing);
    setValue(target);
}

template<typename T>
void AnimatedValue<T>::update() {
    if (anim_.update()) {
        current_ = tween_.lerp(anim_.progress());
    } else {
        // Animation<double> finished (or never started) — snap to target
        current_ = target_;
    }
}

template<typename T>
void AnimatedValue<T>::tick(Duration elapsed) {
    if (anim_.tick(elapsed)) {
        current_ = tween_.lerp(anim_.progress());
    } else {
        // Animation<double> finished (or never started) — snap to target
        current_ = target_;
    }
}

// Explicit instantiations
template class AnimatedValue<double>;
template class AnimatedValue<Color>;
template class AnimatedValue<Point>;

}  // namespace cui_core
