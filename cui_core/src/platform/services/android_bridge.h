#pragma once
/// @file android_bridge.h
/// @brief Private Android JNI transport used by method_channel.cpp.
///
/// On non-Android targets the implementation compiles to stubs so the shared
/// channel layer stays platform-independent.

#include <optional>
#include <string>

namespace cui {
namespace services {
namespace android {

/// True once the host activity installed the bridge.
bool available();

/// C++ → Java: activity.onCuiMethodCall(channel, method, args) → reply.
std::optional<std::string> invoke(const std::string& channel,
                                  const std::string& method,
                                  const std::string& arguments);

}  // namespace android
}  // namespace services
}  // namespace cui_core
