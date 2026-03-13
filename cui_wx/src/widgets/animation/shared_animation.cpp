/// @file shared_animation.cpp — SharedAnimation auto-Ticker controller.
#include "cui/widgets/animation/shared_animation.h"
#include "cui/foundation/shared_ptr.h"

#include <algorithm>

namespace cui {

// ── Inner ────────────────────────────────────────────────────────────────────

SharedAnimation::Inner::Inner()
    : animation(300.0)
    , ticker([this](Duration elapsed) {
          if (onTickCallback) onTickCallback(elapsed);
      }) {
}

SharedAnimation::Inner::Inner(double durationMs, Easing easing)
    : animation(durationMs)
    , ticker([this](Duration elapsed) {
          if (onTickCallback) onTickCallback(elapsed);
      }) {
    animation.setEasing(easing);
}

// ── SharedAnimation ──────────────────────────────────────────────────────────

SharedAnimation::SharedAnimation(double durationMs, Easing easing)
    : inner_(cui::make_shared<Inner>(durationMs, easing)) {
    // Wire the Ticker callback to our onTick().  The Inner's Ticker was
    // constructed with a placeholder that delegates to onTickCallback;
    // we set that here once the SharedAnimation is fully built.
    inner_->onTickCallback = [this](Duration elapsed) {
        onTick(elapsed);
    };
}

void SharedAnimation::onTick(Duration elapsed) {
    // If the animation finished last frame and loop mode is active,
    // restart it now (one frame after completion) so that tick() below
    // computes meaningful progress for the new cycle.
    // 循环模式下来回折算由控制器自己的 computeProgress 完成（正常不会走到
    // 这里），只有被外部 stop() 过才会「没在跑但表还开着」。start() 不清
    // loop_/autoReverse_ 标志，所以重启后节奏不变。
    if (!inner_->animation.isRunning() && inner_->tickerStarted) {
        if (inner_->animation.loop()) {
            inner_->animation.start();
            inner_->status = AnimationStatus::Forward;
        } else {
            // Non-looping animation finished — stop the ticker.
            inner_->ticker.stop();
            inner_->tickerStarted = false;
        }
    }

    if (!inner_->tickerStarted) return;

    bool stillRunning = inner_->animation.tick(elapsed);

    // Detect cycle completion and update status.
    if (!stillRunning && inner_->tickerStarted) {
        if (inner_->status == AnimationStatus::Reverse) {
            inner_->status = AnimationStatus::Dismissed;
        } else {
            inner_->status = AnimationStatus::Completed;
        }
        // For loop modes, the restart-at-next-frame logic at the top of
        // this function will handle the new cycle.  tickerStarted stays
        // true so the ticker keeps firing.
    }

    notifyListeners();
}

void SharedAnimation::notifyListeners() {
    double val = value();
    for (auto& [id, fn] : inner_->listeners) {
        fn(val);
    }
}

// ── Playback control ─────────────────────────────────────────────────────────

void SharedAnimation::forward() {
    inner_->animation.setLoop(false);
    inner_->animation.setAutoReverse(false);
    inner_->animation.start();
    inner_->status = AnimationStatus::Forward;
    if (!inner_->ticker.isActive()) {
        inner_->ticker.start();
    }
    inner_->tickerStarted = true;
}

void SharedAnimation::reverse() {
    inner_->animation.setLoop(false);
    inner_->animation.setAutoReverse(false);
    // 上游 的 reverse()：从「当前值」往 lowerBound 走。控制器现在自己是双向
    // 段模型，直接调即可（旧写法 start()+reverse() 是因为那时 reverse 只改
    // 方向标签、不改取值，必须先归零）。
    inner_->animation.reverse();
    inner_->status = AnimationStatus::Reverse;
    if (!inner_->ticker.isActive()) {
        inner_->ticker.start();
    }
    inner_->tickerStarted = true;
}

void SharedAnimation::repeat() {
    // Ping-pong loop: 0→1→0→1…
    inner_->animation.setLoop(true);
    inner_->animation.setAutoReverse(true);
    inner_->animation.start();
    inner_->status = AnimationStatus::Forward;
    if (!inner_->ticker.isActive()) {
        inner_->ticker.start();
    }
    inner_->tickerStarted = true;
}

void SharedAnimation::repeatForward() {
    // Unidirectional loop: 0→1→0→1… (never reverses direction).
    inner_->animation.setLoop(true);
    inner_->animation.setAutoReverse(false);
    inner_->animation.start();
    inner_->status = AnimationStatus::Forward;
    if (!inner_->ticker.isActive()) {
        inner_->ticker.start();
    }
    inner_->tickerStarted = true;
}

void SharedAnimation::stop() {
    inner_->animation.stop();
    inner_->ticker.stop();
    inner_->tickerStarted = false;
}

void SharedAnimation::reset() {
    inner_->animation.reset();
    inner_->ticker.stop();
    inner_->tickerStarted = false;
    inner_->status = AnimationStatus::Dismissed;
}

// ── Value accessors ──────────────────────────────────────────────────────────

double SharedAnimation::value() const {
    return inner_->animation.progress();
}

double SharedAnimation::rawValue() const {
    return inner_->animation.rawProgress();
}

AnimationStatus SharedAnimation::status() const {
    return inner_->status;
}

bool SharedAnimation::isRunning() const {
    return inner_->tickerStarted;
}

// ── Listener management ──────────────────────────────────────────────────────

int32_t SharedAnimation::addListener(std::function<void(double)> f) {
    int32_t id = inner_->nextListenerId++;
    inner_->listeners[id] = std::move(f);
    return id;
}

void SharedAnimation::removeListener(int32_t id) {
    inner_->listeners.erase(id);
}

// ── AnimationController access ─────────────────────────────────────────────────────────

AnimationController& SharedAnimation::animation() {
    return inner_->animation;
}

const AnimationController& SharedAnimation::animation() const {
    return inner_->animation;
}

}  // namespace cui_core
