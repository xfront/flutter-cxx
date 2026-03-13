/// @file permission.cpp
/// @brief Permission service (see permission.h).

#include "cui/platform/services/permission.h"

#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "cui/platform/services/method_channel.h"

#if defined(__ANDROID__) || defined(__APPLE__) || defined(__EMSCRIPTEN__) || \
    defined(__OHOS__)
#define CUI_HAS_RUNTIME_PERMISSIONS 1
#else
#define CUI_HAS_RUNTIME_PERMISSIONS 0
#endif

namespace cui {
namespace services {

namespace {

constexpr const char* kChannel = "cui_core/permission";

std::mutex g_pendingMutex;
std::map<std::string, PermissionCallback> g_pending;

void installResultHandler() {
    static std::once_flag once;
    std::call_once(once, [] {
        setMethodCallHandler(kChannel, [](const std::string& method,
                                         const std::string& arguments) {
            if (method != "result")
                return std::string{};
            const auto tab = arguments.find('\t');
            if (tab == std::string::npos)
                return std::string{};
            const std::string code = arguments.substr(0, tab);
            const std::string status = arguments.substr(tab + 1);
            PermissionCallback callback;
            {
                std::lock_guard<std::mutex> lock(g_pendingMutex);
                auto it = g_pending.find(code);
                if (it != g_pending.end()) {
                    callback = std::move(it->second);
                    g_pending.erase(it);
                }
            }
            if (callback)
                callback(parsePermissionStatus(status));
            return std::string{};
        });
    });
}

}  // namespace

const char* permissionCode(Permission permission) {
    switch (permission) {
        case Permission::Camera: return "camera";
        case Permission::Microphone: return "microphone";
        case Permission::Storage: return "storage";
        case Permission::Photos: return "photos";
        case Permission::Location: return "location";
        case Permission::Bluetooth: return "bluetooth";
        case Permission::BluetoothScan: return "bluetoothScan";
        case Permission::Notifications: return "notifications";
        case Permission::Contacts: return "contacts";
    }
    return "unknown";
}

const char* permissionStatusName(PermissionStatus status) {
    switch (status) {
        case PermissionStatus::Granted: return "granted";
        case PermissionStatus::Limited: return "limited";
        case PermissionStatus::Denied: return "denied";
        case PermissionStatus::Restricted: return "restricted";
        case PermissionStatus::PermanentlyDenied: return "permanentlyDenied";
        case PermissionStatus::Unavailable: return "unavailable";
        case PermissionStatus::Unknown: break;
    }
    return "unknown";
}

PermissionStatus parsePermissionStatus(const std::string& value) {
    if (value == "granted")
        return PermissionStatus::Granted;
    if (value == "limited")
        return PermissionStatus::Limited;
    if (value == "denied")
        return PermissionStatus::Denied;
    if (value == "restricted")
        return PermissionStatus::Restricted;
    if (value == "permanentlyDenied" || value == "permanently_denied")
        return PermissionStatus::PermanentlyDenied;
    if (value == "unavailable")
        return PermissionStatus::Unavailable;
    return PermissionStatus::Unknown;
}

PermissionStatus checkPermission(Permission permission) {
    installResultHandler();
    auto reply = invokeMethod(kChannel, "check", permissionCode(permission));
    if (!reply) {
#if CUI_HAS_RUNTIME_PERMISSIONS
        return PermissionStatus::Unavailable;
#else
        return PermissionStatus::Granted;
#endif
    }
    return parsePermissionStatus(*reply);
}

void requestPermission(Permission permission, PermissionCallback callback) {
    installResultHandler();
    const std::string code = permissionCode(permission);
    auto reply = invokeMethod(kChannel, "request", code);
    if (!reply) {
#if CUI_HAS_RUNTIME_PERMISSIONS
        if (callback)
            callback(PermissionStatus::Unavailable);
#else
        if (callback)
            callback(PermissionStatus::Granted);
#endif
        return;
    }
    if (*reply == "pending" || reply->empty()) {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        if (callback)
            g_pending[code] = std::move(callback);
        else
            g_pending.erase(code);
        return;
    }
    if (callback)
        callback(parsePermissionStatus(*reply));
}

bool openAppSettings() {
    auto reply = invokeMethod(kChannel, "openSettings");
    return reply && (*reply == "ok" || *reply == "1" || *reply == "true");
}

}  // namespace services
}  // namespace cui_core
