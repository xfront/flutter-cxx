/// @file app_lifecycle.cpp
/// @brief AppLifecycleListener implementation (see framework/app_lifecycle.h).

#include "cui/framework/app_lifecycle.h"

#include <utility>

namespace cui {

AppLifecycleListener::AppLifecycleListener(
        AppLifecycleCallbacks callbacks,
        std::optional<services::AppLifecycleState> initialState)
    : callbacks_(std::move(callbacks)),
      state_(initialState ? initialState : services::appLifecycleState()) {
    observerHandle_ = services::addAppLifecycleObserver(
            [this](services::AppLifecycleState state) {
                didChangeAppLifecycleState(state);
            });
}

AppLifecycleListener::~AppLifecycleListener() { dispose(); }

void AppLifecycleListener::dispose() {
    if (observerHandle_ == 0)
        return;
    services::removeAppLifecycleObserver(observerHandle_);
    observerHandle_ = 0;
}

void AppLifecycleListener::invoke(const std::function<void()>& callback) const {
    if (callback) {
        try {
            callback();
        } catch (...) {
            // One listener callback must not break the pump / other listeners.
        }
    }
}

void AppLifecycleListener::didChangeAppLifecycleState(
        services::AppLifecycleState state) {
    const std::optional<services::AppLifecycleState> previous = state_;
    if (previous == state)
        return;  // upstream: a repeated state produces no notifications
    state_ = state;

    // Transition table copied from app_lifecycle_listener.dart:216-275.
    switch (state) {
        case services::AppLifecycleState::Resumed:
            invoke(callbacks_.onResume);
            break;
        case services::AppLifecycleState::Inactive:
            if (previous == services::AppLifecycleState::Hidden)
                invoke(callbacks_.onShow);
            else if (!previous || previous == services::AppLifecycleState::Resumed)
                invoke(callbacks_.onInactive);
            break;
        case services::AppLifecycleState::Hidden:
            if (previous == services::AppLifecycleState::Paused)
                invoke(callbacks_.onRestart);
            else if (!previous || previous == services::AppLifecycleState::Inactive)
                invoke(callbacks_.onHide);
            break;
        case services::AppLifecycleState::Paused:
            if (!previous || previous == services::AppLifecycleState::Hidden)
                invoke(callbacks_.onPause);
            break;
        case services::AppLifecycleState::Detached:
            invoke(callbacks_.onDetach);
            break;
    }

    if (callbacks_.onStateChange) {
        try {
            callbacks_.onStateChange(state);
        } catch (...) {
        }
    }
}

}  // namespace cui_core
