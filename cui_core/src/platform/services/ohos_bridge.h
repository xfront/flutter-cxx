#pragma once
/// @file ohos_bridge.h
/// @brief Private transport entry points for the HarmonyOS (OHOS) NAPI bridge.
///
/// Mirrors android_bridge.h: method_channel.cpp falls back to this module when
/// no in-process backend is installed. On non-OHOS builds every function is a
/// stub, so the shared channel layer links everywhere.

#include <optional>
#include <string>

namespace cui {
namespace services {
namespace ohos {

/// ArkTS host installed via `install(dispatch)` on the native NAPI module.
bool available();

/// Call the ArkTS dispatcher `(channel, method, args) => string`.
/// std::nullopt means "no host" (not installed / JS thread unreachable).
std::optional<std::string> invoke(const std::string& channel,
                                  const std::string& method,
                                  const std::string& arguments);

}  // namespace ohos
}  // namespace services
}  // namespace cui_core
