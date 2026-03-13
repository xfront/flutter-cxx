#pragma once
/// @file app_lifecycle.h
/// @brief Application lifecycle state (Flutter: `dart:ui` AppLifecycleState).
///
/// Flutter:
///   class _Observer extends WidgetsBindingObserver {
///     @override
///     void didChangeAppLifecycleState(AppLifecycleState state) { ... }
///   }
///
/// cui_core (no WidgetsBinding singleton — the platform service is the binding):
///   services::addAppLifecycleObserver([](services::AppLifecycleState s) { ... });
///   services::appLifecycleState();
///
/// Hosts push transitions on channel `cui_core/lifecycle`, method
/// `AppLifecycleState.changed`, arguments `AppLifecycleState.resumed`
/// (same names as Flutter's engine channel). The value is cached even when no
/// observer is registered, so a listener added later still sees the current
/// state. Desktop hosts are optional: with no host the state stays Detached.
///
/// Threading: the observer callbacks run on the UI thread from
/// pumpPlatformMessages(). add/remove may be called from any thread.

#include <functional>
#include <optional>
#include <string>

namespace cui {
namespace services {

/// Upstream `AppLifecycleState` (dart:ui/platform_dispatcher.dart).
enum class AppLifecycleState {
    Detached,
    Resumed,
    Inactive,
    Hidden,
    Paused,
};

/// Wire name without the enum prefix ("resumed", "inactive", "hidden", …).
const char* appLifecycleStateName(AppLifecycleState state);

/// Parse a host value. Accepts both "resumed" and "AppLifecycleState.resumed".
std::optional<AppLifecycleState> parseAppLifecycleState(const std::string& value);

/// Current state; Detached until the host reports otherwise.
AppLifecycleState appLifecycleState();

using AppLifecycleObserver = std::function<void(AppLifecycleState)>;

/// Register an observer and return its handle. Registering the same observer
/// twice yields two handles, each removing its own registration.
int addAppLifecycleObserver(AppLifecycleObserver observer);

/// Remove a previously registered observer. Unknown handles are ignored.
void removeAppLifecycleObserver(int handle);

/// Feed a transition. Notifies observers only when the state actually changes
/// (Flutter's `WidgetsBinding.handleAppLifecycleStateChanged` does the same);
/// the Detached → Detached initial state therefore stays silent.
void setAppLifecycleState(AppLifecycleState state);

}  // namespace services
}  // namespace cui_core
