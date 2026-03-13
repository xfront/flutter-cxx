#pragma once
/// @file permission.h
/// @brief Runtime permission requests (aligned with Flutter's permission_handler).
///
/// Flutter:
///   final status = await Permission.camera.request();
///
/// cui_core (asynchronous — Android's request dialog is inherently async):
///   services::requestPermission(services::Permission::Camera,
///                               [](services::PermissionStatus s) { ... });
///
/// Desktop platforms have no runtime permission framework: with no channel
/// backend installed requestPermission() completes immediately with Granted.
/// Android goes through the host activity (CuiPlatformServices) which shows
/// the system dialog and pushes the result back on `cui_core/permission`.

#include <functional>
#include <string>

namespace cui {
namespace services {

enum class Permission {
    Camera,          ///< CAMERA
    Microphone,      ///< RECORD_AUDIO
    Storage,         ///< Legacy READ/WRITE_EXTERNAL_STORAGE
    Photos,          ///< READ_MEDIA_IMAGES (Android 13+)
    Location,        ///< ACCESS_FINE_LOCATION
    Bluetooth,       ///< BLUETOOTH_CONNECT (Android 12+)
    BluetoothScan,   ///< BLUETOOTH_SCAN (Android 12+)
    Notifications,   ///< POST_NOTIFICATIONS (Android 13+)
    Contacts,        ///< READ_CONTACTS
};

enum class PermissionStatus {
    Granted,
    Limited,           ///< iOS "limited" photo access
    Denied,
    Restricted,        ///< Policy/parental control
    PermanentlyDenied, ///< User chose "don't ask again" / denied twice
    Unavailable,       ///< Platform has no permission framework / host missing
    Unknown,
};

/// Channel argument code ("camera", "bluetoothScan", …).
const char* permissionCode(Permission permission);

/// Human-readable name for a status (also the wire format).
const char* permissionStatusName(PermissionStatus status);

/// Parse a host reply ("granted", "denied", "permanentlyDenied", …).
PermissionStatus parsePermissionStatus(const std::string& value);

/// Synchronous status query (no dialog). Desktop returns Granted.
PermissionStatus checkPermission(Permission permission);

using PermissionCallback = std::function<void(PermissionStatus)>;

/// Ask the user. The callback always runs on the UI thread: immediately when
/// the platform answers synchronously, otherwise from the next
/// pumpPlatformMessages() after the dialog closes. Only the most recent
/// callback per permission is kept.
void requestPermission(Permission permission, PermissionCallback callback);

/// Open the app's system settings page (for permanently denied permissions).
bool openAppSettings();

}  // namespace services
}  // namespace cui_core
