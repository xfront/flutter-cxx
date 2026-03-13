#pragma once
/// @file system_navigator.h
/// @brief System navigation / back button (Flutter: services/system_navigator.dart
///        and WidgetsBinding.handlePopRoute).
///
/// Flutter:
///   SystemNavigator.pop();              // leave the Flutter instance
///   SystemNavigator.setFrameworkHandlesBack(true);
///
/// cui_core:
///   services::SystemNavigator::pop();
///   // Hosts deliver the platform back gesture as:
///   //   channel "cui_core/system_navigator", method "back"
///   // (Android: notifyCui(...) from onBackPressed; OHOS: onBackPressed).
///
/// The inbound "back" call runs every registered handler from the most
/// recently registered to the oldest; the first one returning true consumes
/// it. With no consumer the engine calls pop() itself — exactly Flutter's
/// "root route exits the app" behaviour, so hosts must NOT finish the
/// activity on their own.
///
/// The inbound handler replies "1" when the framework consumed the event and
/// "0" otherwise, for hosts that want the synchronous answer
/// (`invokeCuiMethod`).
///
/// Handlers are registered by WidgetsAppState (so a Navigator pops its top
/// route) and by apps that need to intercept back themselves.

#include <functional>

namespace cui {
namespace services {

class SystemNavigator {
public:
    /// Returns true if the framework handled the request.
    using BackHandler = std::function<bool()>;

    /// Leave the current Flutter instance (Android finish / iOS dismiss).
    /// `animated` only matters on iOS. False when no host is connected.
    static bool pop(bool animated = true);

    /// Tell Android whether the framework will consume back events
    /// (predictive-back opt-out). Returns false without a host.
    static bool setFrameworkHandlesBack(bool frameworkHandlesBack);

    /// Register a back handler; returns a handle for removeBackHandler().
    /// Handles are queried newest-first.
    static int addBackHandler(BackHandler handler);

    /// Remove a handler. Unknown handles are ignored.
    static void removeBackHandler(int handle);

    /// Number of registered handlers (Flutter: dispatcher.hasCallbacks).
    static int backHandlerCount();

    /// Deliver one back request. Falls back to pop() when nobody consumed it.
    /// Returns true when a handler consumed the event.
    static bool handleBack();
};

}  // namespace services
}  // namespace cui_core
