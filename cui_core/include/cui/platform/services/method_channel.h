#pragma once
/// @file method_channel.h
/// @brief Flutter-style method channels (ServicesBinding / MethodChannel).
///
/// Flutter:
///   static const channel = MethodChannel('cui_core/camera');
///   final result = await channel.invokeMethod('listCameras');
///   channel.setMethodCallHandler((call) async => 'reply');
///
/// cui_core:
///   services::MethodChannel channel("cui_core/camera");
///   auto result = channel.invokeMethod("listCameras");     // → native
///   channel.setMethodCallHandler([](auto& m, auto& a) { return "reply"; });
///
/// Backends:
///   - Android: JNI through the host activity (`dev.cui_core.framework.CuiActivity`
///     in src/platform/android/java/).  Installed when the activity calls
///     `nativeInstallCuiPlatform`, or from native code via
///     services::android::install() in android_bridge.h.
///   - HarmonyOS: NAPI module "cui_core" registered by services/ohos_channel.cpp;
///     the ArkTS host calls `install(dispatch)` from src/platform/ohos/.
///   - iOS: C bridge in services/ios_channel.cpp; the Swift/ObjC host calls
///     `cui_ios_register_host_dispatch` (see src/platform/ios/CuiBridge.swift).
///   - Desktop / tests: an in-process backend installed with
///     setChannelBackend(); without one every invokeMethod() returns nullopt.
///
/// Threading: invokeMethod() and setMethodCallHandler() are UI-thread APIs.
/// Native-originated calls are queued by pushInboundCall() and dispatched by
/// pumpPlatformMessages(), which Application::run() calls once per frame.
/// pushInboundCall() also fires the wake-up hook so a blocked event loop
/// (waitForEvent with no timeout) notices the new work immediately.

#include <functional>
#include <optional>
#include <string>

namespace cui {
namespace services {

/// Native → C++ call handler (Flutter: MethodCallHandler). Returns the reply
/// string sent back to the native side; return an empty string for none.
using MethodCallHandler = std::function<std::string(const std::string& method,
                                                    const std::string& arguments)>;

/// C++ → native transport (Flutter: BinaryMessenger). `std::nullopt` means
/// the channel is not connected (no backend / host not installed).
using ChannelBackend = std::function<std::optional<std::string>(
    const std::string& channel, const std::string& method,
    const std::string& arguments)>;

/// Install the in-process backend (desktop host bridge, tests). Passing an
/// empty function restores the platform transport (JNI / NAPI / iOS C bridge).
void setChannelBackend(ChannelBackend backend);
bool hasChannelBackend();

/// Invoke a method on `channel` (Flutter: invokeMethod).
std::optional<std::string> invokeMethod(const std::string& channel,
                                        const std::string& method,
                                        const std::string& arguments = {});

/// Register the handler for calls arriving on `channel` from the native side
/// (Flutter: setMethodCallHandler). An empty handler unregisters.
void setMethodCallHandler(const std::string& channel, MethodCallHandler handler);
bool hasMethodCallHandler(const std::string& channel);

/// Queue a native-originated call. Safe from any thread; wakes the app loop.
void pushInboundCall(std::string channel, std::string method,
                     std::string arguments);

/// Dispatch queued native calls and event-channel events on the calling
/// (UI) thread. Called by Application::run() once per frame; tests may call
/// it directly after simulating native pushes.
void pumpPlatformMessages();

/// Wake-up hook used when native work arrives from another thread
/// (AndroidWindow::wakeUp in Application). Pass an empty function to clear.
void setWakeUpHandler(std::function<void()> handler);

/// Per-channel convenience wrapper around the free functions.
class MethodChannel {
public:
    explicit MethodChannel(std::string name);

    const std::string& name() const { return name_; }

    /// See services::invokeMethod().
    std::optional<std::string> invokeMethod(const std::string& method,
                                            const std::string& arguments = {}) const;

    /// See services::setMethodCallHandler().
    void setMethodCallHandler(MethodCallHandler handler) const;
    bool hasHandler() const;

private:
    std::string name_;
};

}  // namespace services
}  // namespace cui_core
