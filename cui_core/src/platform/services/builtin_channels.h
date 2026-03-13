#pragma once
/// @file builtin_channels.h
/// @brief Private glue for the channels the engine listens on by itself.
///
/// Hosts push lifecycle / back-button events without the app registering
/// anything first, so pumpPlatformMessages() installs these handlers on its
/// first run. Kept out of the public headers: apps talk to the services, not
/// to the channels.

namespace cui {
namespace services {
namespace detail {

/// Channel `cui_core/lifecycle` (AppLifecycleState.changed). Idempotent.
void installLifecycleChannel();

/// Channel `cui_core/system_navigator` (method "back"). Idempotent.
void installSystemNavigatorChannel();

}  // namespace detail
}  // namespace services
}  // namespace cui_core
