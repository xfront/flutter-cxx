#pragma once
/// @file platform_info.h
/// @brief Which platform the binary targets (Flutter: `defaultTargetPlatform`).
///
/// Flutter:
///   if (defaultTargetPlatform == TargetPlatform.android) ...
///
/// cui_core:
///   if (services::defaultTargetPlatform() == services::TargetPlatform::Android) ...
///
/// The value is fixed at compile time — the same as Flutter's io/web split.
/// `OHOS` and `Web` are cui_core extensions (Flutter's web target reports the
/// browser's host OS instead); the six upstream names keep their order so
/// ports read the same.

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace cui {
namespace services {

enum class TargetPlatform {
    Android,
    Fuchsia,
    IOS,
    Linux,
    MacOS,
    Windows,
    OHOS,  ///< cui_core extension: HarmonyOS NEXT
    Web,   ///< cui_core extension: Emscripten target
};

inline TargetPlatform defaultTargetPlatform() {
#if defined(__ANDROID__)
    return TargetPlatform::Android;
#elif defined(__OHOS__)
    return TargetPlatform::OHOS;
#elif defined(__EMSCRIPTEN__)
    return TargetPlatform::Web;
#elif defined(__APPLE__) && TARGET_OS_IOS
    return TargetPlatform::IOS;
#elif defined(__APPLE__)
    return TargetPlatform::MacOS;
#elif defined(_WIN32)
    return TargetPlatform::Windows;
#else
    return TargetPlatform::Linux;
#endif
}

/// Convenience for code that only cares about touch-first platforms.
/// (cui_core addition; Flutter has no equivalent predicate.)
inline bool isMobilePlatform() {
    const TargetPlatform platform = defaultTargetPlatform();
    return platform == TargetPlatform::Android || platform == TargetPlatform::IOS ||
           platform == TargetPlatform::OHOS;
}

}  // namespace services
}  // namespace cui_core
