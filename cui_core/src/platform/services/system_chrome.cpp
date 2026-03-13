/// @file system_chrome.cpp
/// @brief SystemChrome channel calls (see system_chrome.h).

#include "cui/platform/services/system_chrome.h"

#include <mutex>
#include <utility>

#include "cui/foundation/color.h"
#include "cui/platform/services/app_lifecycle.h"
#include "cui/platform/services/method_channel.h"

namespace cui {
namespace services {

namespace {

constexpr const char* kChannel = "cui_core/platform";

std::optional<SystemUiOverlayStyle> g_latestStyle;
std::once_flag g_lifecycleHooked;

/// Upstream resets the cached style when the app detaches
/// (system_chrome.dart:783-791) so the next attach sends it again.
void ensureLifecycleHook() {
    std::call_once(g_lifecycleHooked, [] {
        addAppLifecycleObserver([](AppLifecycleState state) {
            if (state == AppLifecycleState::Detached)
                g_latestStyle.reset();
        });
    });
}

void appendField(std::string& out, const char* key, const std::string& value) {
    if (value.empty())
        return;
    if (!out.empty())
        out += '\t';
    out += key;
    out += '=';
    out += value;
}

std::string colorField(const std::optional<Color>& color) {
    return color ? colorToHex(*color) : std::string{};
}

std::string brightnessField(const std::optional<Brightness>& brightness) {
    return brightness ? brightnessName(*brightness) : std::string{};
}

std::string boolField(const std::optional<bool>& value) {
    if (!value)
        return {};
    return *value ? "1" : "0";
}

std::string joinNames(const std::vector<const char*>& names) {
    std::string out;
    for (const char* name : names) {
        if (!out.empty())
            out += ',';
        out += name;
    }
    return out;
}

bool invoke(const char* method, const std::string& arguments = {}) {
    return invokeMethod(kChannel, method, arguments).has_value();
}

}  // namespace

const char* deviceOrientationName(DeviceOrientation orientation) {
    switch (orientation) {
        case DeviceOrientation::PortraitUp: return "DeviceOrientation.portraitUp";
        case DeviceOrientation::LandscapeLeft: return "DeviceOrientation.landscapeLeft";
        case DeviceOrientation::PortraitDown: return "DeviceOrientation.portraitDown";
        case DeviceOrientation::LandscapeRight: return "DeviceOrientation.landscapeRight";
    }
    return "DeviceOrientation.portraitUp";
}

const char* systemUiOverlayName(SystemUiOverlay overlay) {
    return overlay == SystemUiOverlay::Top ? "SystemUiOverlay.top"
                                           : "SystemUiOverlay.bottom";
}

const char* systemUiModeName(SystemUiMode mode) {
    switch (mode) {
        case SystemUiMode::LeanBack: return "SystemUiMode.leanBack";
        case SystemUiMode::Immersive: return "SystemUiMode.immersive";
        case SystemUiMode::ImmersiveSticky: return "SystemUiMode.immersiveSticky";
        case SystemUiMode::EdgeToEdge: return "SystemUiMode.edgeToEdge";
        case SystemUiMode::Manual: return "SystemUiMode.manual";
    }
    return "SystemUiMode.edgeToEdge";
}

const char* brightnessName(Brightness brightness) {
    return brightness == Brightness::Dark ? "Brightness.dark" : "Brightness.light";
}

bool SystemUiOverlayStyle::operator==(const SystemUiOverlayStyle& o) const {
    auto sameColor = [](const std::optional<Color>& a, const std::optional<Color>& b) {
        return a.has_value() == b.has_value() && (!a || *a == *b);
    };
    return sameColor(systemNavigationBarColor, o.systemNavigationBarColor) &&
           sameColor(systemNavigationBarDividerColor, o.systemNavigationBarDividerColor) &&
           systemNavigationBarIconBrightness == o.systemNavigationBarIconBrightness &&
           systemNavigationBarContrastEnforced == o.systemNavigationBarContrastEnforced &&
           sameColor(statusBarColor, o.statusBarColor) &&
           statusBarBrightness == o.statusBarBrightness &&
           statusBarIconBrightness == o.statusBarIconBrightness &&
           systemStatusBarContrastEnforced == o.systemStatusBarContrastEnforced;
}

std::string SystemUiOverlayStyle::encode() const {
    std::string out;
    appendField(out, "systemNavigationBarColor",
                colorField(systemNavigationBarColor));
    appendField(out, "systemNavigationBarDividerColor",
                colorField(systemNavigationBarDividerColor));
    appendField(out, "systemNavigationBarIconBrightness",
                brightnessField(systemNavigationBarIconBrightness));
    appendField(out, "systemNavigationBarContrastEnforced",
                boolField(systemNavigationBarContrastEnforced));
    appendField(out, "statusBarColor", colorField(statusBarColor));
    appendField(out, "statusBarBrightness", brightnessField(statusBarBrightness));
    appendField(out, "statusBarIconBrightness",
                brightnessField(statusBarIconBrightness));
    appendField(out, "systemStatusBarContrastEnforced",
                boolField(systemStatusBarContrastEnforced));
    return out;
}

// Upstream constants: same fields as system_chrome.dart:316-330. Now defined
// inline in the header (lambda initializer, see comment there) so package
// DLLs / executables don't need cross-DLL static data member imports.

bool SystemChrome::setPreferredOrientations(
        const std::vector<DeviceOrientation>& orientations) {
    std::vector<const char*> names;
    names.reserve(orientations.size());
    for (DeviceOrientation orientation : orientations)
        names.push_back(deviceOrientationName(orientation));
    return invoke("SystemChrome.setPreferredOrientations", joinNames(names));
}

bool SystemChrome::setApplicationSwitcherDescription(
        const std::string& label, std::optional<Color> primaryColor) {
    std::string args = label;
    args += '\t';
    args += colorField(primaryColor);
    return invoke("SystemChrome.setApplicationSwitcherDescription", args);
}

bool SystemChrome::setEnabledSystemUIMode(
        SystemUiMode mode, const std::vector<SystemUiOverlay>& overlays) {
    if (mode != SystemUiMode::Manual)
        return invoke("SystemChrome.setEnabledSystemUIMode", systemUiModeName(mode));
    std::vector<const char*> names;
    names.reserve(overlays.size());
    for (SystemUiOverlay overlay : overlays)
        names.push_back(systemUiOverlayName(overlay));
    return invoke("SystemChrome.setEnabledSystemUIOverlays", joinNames(names));
}

bool SystemChrome::setSystemUIOverlayStyle(const SystemUiOverlayStyle& style) {
    ensureLifecycleHook();
    if (g_latestStyle && *g_latestStyle == style)
        return true;  // Flutter's trivial success (no host round-trip)
    const bool ok = invoke("SystemChrome.setSystemUIOverlayStyle", style.encode());
    if (ok)
        g_latestStyle = style;
    return ok;
}

bool SystemChrome::restoreSystemUIOverlays() {
    return invoke("SystemChrome.restoreSystemUIOverlays");
}

std::optional<SystemUiOverlayStyle> SystemChrome::latestStyle() {
    return g_latestStyle;
}

}  // namespace services
}  // namespace cui_core
