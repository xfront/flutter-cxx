#pragma once
/// @file system_chrome.h
/// @brief System UI control (Flutter: services/system_chrome.dart).
///
/// Flutter:
///   SystemChrome.setPreferredOrientations([DeviceOrientation.portraitUp]);
///   SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
///   SystemChrome.setSystemUIOverlayStyle(SystemUiOverlayStyle.dark);
///
/// cui_core:
///   services::SystemChrome::setPreferredOrientations(
///       {services::DeviceOrientation::PortraitUp});
///   services::SystemChrome::setEnabledSystemUIMode(
///       services::SystemUiMode::EdgeToEdge);
///   services::SystemChrome::setSystemUIOverlayStyle(
///       services::SystemUiOverlayStyle::dark);
///
/// Channel `cui_core/platform`, method names identical to Flutter's engine channel
/// (`SystemChrome.setPreferredOrientations`, …), so host code can be ported
/// line for line. Arguments are tab/comma separated strings (a Dart map has
/// no equivalent here); the exact encoding is documented next to each method.
///
/// Every call returns false when no native host is connected (desktop / pure
/// C++ builds); the state is still tracked locally so tests and demos can
/// show what would have been sent.

#include <optional>
#include <string>
#include <vector>

#include "cui/foundation/geometry.h"

namespace cui {
namespace services {

/// Upstream `DeviceOrientation` (system_chrome.dart:43).
enum class DeviceOrientation {
    PortraitUp,
    LandscapeLeft,
    PortraitDown,
    LandscapeRight,
};

/// Wire name with the upstream prefix ("DeviceOrientation.portraitUp").
const char* deviceOrientationName(DeviceOrientation orientation);

/// Upstream `SystemUiOverlay` (system_chrome.dart:87).
enum class SystemUiOverlay {
    Top,
    Bottom,
};

const char* systemUiOverlayName(SystemUiOverlay overlay);

/// Upstream `SystemUiMode` (system_chrome.dart:107).
enum class SystemUiMode {
    LeanBack,
    Immersive,
    ImmersiveSticky,
    EdgeToEdge,
    Manual,
};

/// Wire name with the upstream prefix ("SystemUiMode.edgeToEdge").
const char* systemUiModeName(SystemUiMode mode);

/// Upstream `Brightness` (dart:ui). Kept here because the overlay style is
/// its only user in cui_core.
enum class Brightness {
    Dark,
    Light,
};

const char* brightnessName(Brightness brightness);

/// Upstream `SystemUiOverlayStyle` (system_chrome.dart:235).
struct SystemUiOverlayStyle {
    std::optional<Color> systemNavigationBarColor;
    std::optional<Color> systemNavigationBarDividerColor;
    std::optional<Brightness> systemNavigationBarIconBrightness;
    std::optional<bool> systemNavigationBarContrastEnforced;
    std::optional<Color> statusBarColor;
    std::optional<Brightness> statusBarBrightness;
    std::optional<Brightness> statusBarIconBrightness;
    std::optional<bool> systemStatusBarContrastEnforced;

    /// Light system UI, for dark backgrounds (upstream `light`).
    /// Dark system UI, for light backgrounds (upstream `dark`).
    /// Both are defined inline right after the class: MSVC cannot link
    /// cross-DLL references to static data members
    /// (WINDOWS_EXPORT_ALL_SYMBOLS does not cover them), so an inline
    /// definition gives each TU its own identical copy of this value-type
    /// constant.  Lambda initializer because Color::fromRGB is not constexpr.
    static const SystemUiOverlayStyle light;
    static const SystemUiOverlayStyle dark;

    bool operator==(const SystemUiOverlayStyle& o) const;
    bool operator!=(const SystemUiOverlayStyle& o) const { return !(*this == o); }

    /// `key=value` pairs joined by tabs; unset fields are omitted. Colors use
    /// "#RRGGBB" / "#AARRGGBB", brightness "Brightness.dark|light",
    /// booleans "1"/"0".
    std::string encode() const;
};

// Upstream constants: same fields as system_chrome.dart:316-330.  Inline
// definitions (C++17) so every TU gets its own copy and no cross-DLL static
// data member import is needed on MSVC.
inline const SystemUiOverlayStyle SystemUiOverlayStyle::light = [] {
    SystemUiOverlayStyle style;
    style.systemNavigationBarColor = Color::fromRGB(0.0, 0.0, 0.0);
    style.systemNavigationBarIconBrightness = Brightness::Light;
    style.statusBarIconBrightness = Brightness::Light;
    style.statusBarBrightness = Brightness::Dark;
    return style;
}();

inline const SystemUiOverlayStyle SystemUiOverlayStyle::dark = [] {
    SystemUiOverlayStyle style;
    style.systemNavigationBarColor = Color::fromRGB(0.0, 0.0, 0.0);
    style.systemNavigationBarIconBrightness = Brightness::Light;
    style.statusBarIconBrightness = Brightness::Dark;
    style.statusBarBrightness = Brightness::Light;
    return style;
}();

class SystemChrome {
public:
    /// Lock the app to the given orientations; empty restores the OS default.
    /// Args: comma separated `DeviceOrientation.*` names.
    static bool setPreferredOrientations(const std::vector<DeviceOrientation>& orientations);

    /// Update the application-switcher (recent tasks) entry.
    /// Args: `label\t<color hex>` (color may be empty).
    static bool setApplicationSwitcherDescription(
            const std::string& label, std::optional<Color> primaryColor = std::nullopt);

    /// Show/hide the system overlays. Non-manual modes send
    /// `SystemChrome.setEnabledSystemUIMode` with the mode name; `Manual`
    /// sends `SystemChrome.setEnabledSystemUIOverlays` with the given list
    /// (Flutter's split, system_chrome.dart:609-624).
    static bool setEnabledSystemUIMode(
            SystemUiMode mode,
            const std::vector<SystemUiOverlay>& overlays = {SystemUiOverlay::Top,
                                                           SystemUiOverlay::Bottom});

    /// Set the status/navigation bar appearance (system_chrome.dart:746).
    /// The last accepted style is cached and exposed through latestStyle().
    static bool setSystemUIOverlayStyle(const SystemUiOverlayStyle& style);

    /// Re-apply the last setEnabledSystemUIMode() configuration.
    static bool restoreSystemUIOverlays();

    /// Last style handed to the host (upstream `latestStyle`); reset when the
    /// app detaches, mirroring system_chrome.dart:783-791.
    static std::optional<SystemUiOverlayStyle> latestStyle();
};

}  // namespace services
}  // namespace cui_core
