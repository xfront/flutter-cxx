#pragma once
/// @file framework/app_lifecycle.h
/// @brief Application lifecycle listener (Flutter:
///        widgets/app_lifecycle_listener.dart).
///
/// Flutter:
///   _listener = AppLifecycleListener(
///     onStateChange: (state) => ...,
///     onPause: () => ...,
///     onResume: () => ...,
///   );
///   // dispose(): _listener.dispose();
///
/// cui_core (no WidgetsBinding singleton — services::AppLifecycleState is the
/// binding):
///   AppLifecycleListener listener({
///       .onStateChange = [](services::AppLifecycleState s) { ... },
///       .onPause = [] { ... },
///   });
///
/// The state machine and the individual callbacks follow upstream exactly
/// (app_lifecycle_listener.dart:216-275), including `onShow` firing only on
/// hidden → inactive and `onRestart` only on paused → hidden.
///
/// `onExitRequested` / `AppExitResponse` are not ported: cui_core has no
/// cancelable app-exit pipeline yet (SystemNavigator::pop is fire-and-forget).
///
/// A listener is a plain object, not a widget — keep it as a member of the
/// State that owns it and call dispose() from State::dispose().

#include <functional>
#include <optional>

#include "cui/platform/services/app_lifecycle.h"

namespace cui {

/// Upstream's named constructor arguments, in one aggregate so call sites can
/// use designated initializers.
struct AppLifecycleCallbacks {
    std::function<void(services::AppLifecycleState)> onStateChange;
    std::function<void()> onResume;
    std::function<void()> onInactive;
    std::function<void()> onHide;
    std::function<void()> onShow;
    std::function<void()> onPause;
    std::function<void()> onRestart;
    std::function<void()> onDetach;
};

class AppLifecycleListener {
public:
    /// Subscribes immediately. `initialState` defaults to the state the
    /// platform service currently reports, like upstream reading
    /// `binding.lifecycleState` in its constructor.
    explicit AppLifecycleListener(
            AppLifecycleCallbacks callbacks = {},
            std::optional<services::AppLifecycleState> initialState = std::nullopt);
    ~AppLifecycleListener();

    AppLifecycleListener(const AppLifecycleListener&) = delete;
    AppLifecycleListener& operator=(const AppLifecycleListener&) = delete;

    /// Unsubscribe. Safe to call twice; the destructor does it as well.
    void dispose();

    bool disposed() const { return observerHandle_ == 0; }

    /// Last state seen by this listener (upstream `_lifecycleState`).
    std::optional<services::AppLifecycleState> lifecycleState() const {
        return state_;
    }

    /// Upstream `didChangeAppLifecycleState`; exposed for tests.
    void didChangeAppLifecycleState(services::AppLifecycleState state);

private:
    void invoke(const std::function<void()>& callback) const;

    AppLifecycleCallbacks callbacks_;
    std::optional<services::AppLifecycleState> state_;
    int observerHandle_ = 0;
};

}  // namespace cui_core
