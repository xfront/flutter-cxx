/// @file ios_channel.cpp
/// @brief C transport for the platform channel on iOS (see ios_bridge.h).
///
/// Unlike Android (JNI) and HarmonyOS (NAPI), iOS has no class/method lookup
/// layer: the host is Swift or Objective-C and talks to the engine through a
/// handful of plain C symbols (declared for the host in
/// src/platform/ios/cui_ios_bridge.h):
///
///   cui_ios_register_host_dispatch(dispatch)   // C++ → host
///   cui_ios_unregister_host_dispatch()
///   cui_ios_method_call(channel, method, args) // host → C++ (sync)
///   cui_ios_notify(channel, method, args)      // host → C++ (next frame)
///   cui_ios_emit_event(channel, event)         // EventChannel push
///
/// String ownership: the host's dispatcher returns a malloc-allocated string
/// (Swift: strdup); this file copies it and frees it. cui_ios_method_call()
/// returns a string this file allocated with strdup — the host must free it.
///
/// Threading: the dispatcher is called on whichever thread invokes
/// invokeMethod(). The engine pumps frames on the UI thread, so that is the
/// normal case; async host work (permission prompts, capture, BLE scans)
/// replies immediately and reports later through notify/event, so no host
/// callback ever blocks the frame pump.

#include "ios_bridge.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(TARGET_OS_IOS) && TARGET_OS_IOS

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "channel_registry.h"
#include "cui/platform/services/event_channel.h"
#include "cui/platform/services/method_channel.h"

extern "C" {

/// Host dispatcher installed by Swift/ObjC. Returns a malloc-allocated reply
/// (or NULL when the host has nothing to say); the engine frees it.
typedef char* (*CuiIosHostDispatch)(const char* channel, const char* method,
                                    const char* args);

void cui_ios_register_host_dispatch(CuiIosHostDispatch dispatch);
void cui_ios_unregister_host_dispatch(void);
char* cui_ios_method_call(const char* channel, const char* method,
                          const char* args);
void cui_ios_notify(const char* channel, const char* method, const char* args);
void cui_ios_emit_event(const char* channel, const char* event);

}  // extern "C"

namespace cui {
namespace services {
namespace ios {

namespace {

std::mutex g_mutex;
CuiIosHostDispatch g_dispatch = nullptr;

const char* orEmpty(const char* value) {
    return value ? value : "";
}

std::string copyAndFree(char* value) {
    if (!value)
        return {};
    std::string out(value);
    std::free(value);
    return out;
}

}  // namespace

bool available() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_dispatch != nullptr;
}

std::optional<std::string> invoke(const std::string& channel,
                                  const std::string& method,
                                  const std::string& arguments) {
    CuiIosHostDispatch dispatch = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        dispatch = g_dispatch;
    }
    if (!dispatch)
        return std::nullopt;
    char* reply = dispatch(channel.c_str(), method.c_str(), arguments.c_str());
    if (!reply)
        return std::nullopt;
    return copyAndFree(reply);
}

}  // namespace ios
}  // namespace services
}  // namespace cui_core

extern "C" void cui_ios_register_host_dispatch(CuiIosHostDispatch dispatch) {
    std::lock_guard<std::mutex> lock(cui::services::ios::g_mutex);
    cui::services::ios::g_dispatch = dispatch;
}

extern "C" void cui_ios_unregister_host_dispatch(void) {
    std::lock_guard<std::mutex> lock(cui::services::ios::g_mutex);
    cui::services::ios::g_dispatch = nullptr;
}

extern "C" char* cui_ios_method_call(const char* channel, const char* method,
                                     const char* args) {
    using namespace cui::services;
    const std::string reply = detail::dispatchHandlerNow(
        ios::orEmpty(channel), ios::orEmpty(method), ios::orEmpty(args));
    return ::strdup(reply.c_str());
}

extern "C" void cui_ios_notify(const char* channel, const char* method,
                               const char* args) {
    using namespace cui::services;
    pushInboundCall(ios::orEmpty(channel), ios::orEmpty(method),
                    ios::orEmpty(args));
}

extern "C" void cui_ios_emit_event(const char* channel, const char* event) {
    using namespace cui::services;
    pushInboundEvent(ios::orEmpty(channel), ios::orEmpty(event));
}

#else  // !iOS

namespace cui {
namespace services {
namespace ios {

bool available() {
    return false;
}

std::optional<std::string> invoke(const std::string&, const std::string&,
                                  const std::string&) {
    return std::nullopt;
}

}  // namespace ios
}  // namespace services
}  // namespace cui_core

#endif  // TARGET_OS_IOS
