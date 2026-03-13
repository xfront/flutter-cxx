#pragma once
/// @file ios_bridge.h
/// @brief Private transport entry points for the iOS (C/Swift) bridge.
///
/// Mirrors android_bridge.h / ohos_bridge.h: method_channel.cpp falls back to
/// this module when no in-process backend is installed. On non-iOS builds every
/// function is a stub, so the shared channel layer links everywhere.

#include <optional>
#include <string>

namespace cui {
namespace services {
namespace ios {

/// Swift/ObjC host installed via cui_ios_register_host_dispatch().
bool available();

/// Call the host dispatcher `(channel, method, args) => char*`.
/// std::nullopt means "no host" (not installed).
std::optional<std::string> invoke(const std::string& channel,
                                  const std::string& method,
                                  const std::string& arguments);

}  // namespace ios
}  // namespace services
}  // namespace cui_core
