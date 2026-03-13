#pragma once
/// @file widgets/animation/shared_animation.h
/// @brief SharedAnimation — auto-Ticker animation controller.
///
/// Wraps an AnimationController + Ticker pair so that calling forward()/reverse()/
/// repeat() automatically starts the Ticker, and animation completion
/// automatically stops it.  Callers never manage Ticker lifecycle manually.
///
/// Mirrors rui's SharedAnimation (Rust Rc<RefCell<>> wrapper) adapted to
/// C++ shared_ptr semantics.
///
/// Usage:
///   auto anim = cui_core::make_shared<SharedAnimation>(300.0, Easing::EaseOutCubic);
///   anim->forward();
///   // In paint: double opacity = anim->value();
///   //           if (anim->isRunning()) repaint();

#include <functional>
#include <map>
#include <memory>

#include "cui/foundation/ticker.h"
#include "cui/widgets/animation/animation.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {


/// A shareable animation controller with automatic Ticker lifecycle.
///
/// Internally holds an AnimationController and a Ticker inside a shared Inner struct.
/// forward()/reverse()/repeat() start the Ticker; completion stops it.
/// Multiple SharedAnimation copies share the same inner state (like rui's
/// Rc<RefCell<SharedAnimationInner>>).
class SharedAnimation {
public:
    /// Create a shared animation controller.
    /// @param durationMs  Total duration in milliseconds.
    /// @param easing      Easing curve applied to progress.
    SharedAnimation(double durationMs = 300.0, Easing easing = Easing::Linear);

    /// Start forward playback (0 → 1).
    void forward();

    /// Start reverse playback (1 → 0).
    void reverse();

    /// Ping-pong loop (0 → 1 → 0 → 1 ...).
    void repeat();

    /// Unidirectional loop (0 → 1 → 0 → 1 ... without reversing direction).
    void repeatForward();

    /// Stop the animation and the Ticker.
    void stop();

    /// Reset to initial state (elapsed = 0, not running).
    void reset();

    /// Current eased progress [0, 1].
    double value() const;

    /// Current raw linear progress [0, 1] (no easing).
    double rawValue() const;

    /// Current animation status.
    AnimationStatus status() const;

    /// True while the animation is running.
    bool isRunning() const;

    /// Add a value-change listener.  Called after each tick with the eased
    /// progress value.  Returns an ID for removeListener().
    int32_t addListener(std::function<void(double)> f);

    /// Remove a previously added listener by ID.
    void removeListener(int32_t id);

    /// Access the underlying AnimationController (for Tween::evaluate etc.).
    AnimationController& animation();
    const AnimationController& animation() const;

private:
    struct Inner {
        AnimationController animation;
        Ticker ticker;
        AnimationStatus status = AnimationStatus::Dismissed;
        bool tickerStarted = false;
        int32_t nextListenerId = 0;
        std::map<int32_t, std::function<void(double)>> listeners;

        /// Stored so the Ticker callback can be wired after Inner is
        /// constructed (the Ticker placeholder in the ctor cannot capture
        /// the outer SharedAnimation's `this` yet).
        std::function<void(Duration)> onTickCallback;

        Inner();
        Inner(double durationMs, Easing easing);
    };

    SharedPtr<Inner> inner_;

    /// Called by the Ticker callback each frame.
    void onTick(Duration elapsed);

    /// Notify all listeners of the current value.
    void notifyListeners();
};

}  // namespace cui_core
