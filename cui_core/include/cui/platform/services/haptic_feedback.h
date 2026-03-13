#pragma once
/// @file haptic_feedback.h
/// @brief Haptic / acoustic feedback (Flutter: services/haptic_feedback.dart
///        and services/system_sound.dart).
///
/// Flutter:
///   HapticFeedback.lightImpact();
///   SystemSound.play(SystemSoundType.click);
///
/// cui_core:
///   services::HapticFeedback::lightImpact();
///   services::SystemSound::playClick();
///
/// Both ride channel `cui_core/platform` with Flutter's engine method names
/// (`HapticFeedback.vibrate` + a `HapticFeedbackType.*` argument,
/// `SystemSound.play` + `SystemSoundType.click`), so hosts can mirror the
/// embedder code they already know.
///
/// Calls return false when no native host is connected (desktop / tests) and
/// are otherwise fire-and-forget — flutter's API is async for the same
/// reason, the host may answer after the frame.

namespace cui {
namespace services {

/// Upstream `HapticFeedbackType` values (haptic_feedback.dart:22-151).
enum class HapticFeedbackType {
    Vibrate,
    LightImpact,
    MediumImpact,
    HeavyImpact,
    SelectionClick,
    SuccessNotification,
    WarningNotification,
    ErrorNotification,
};

/// Wire name ("HapticFeedbackType.lightImpact"); empty for plain vibrate,
/// matching Flutter's `vibrate()` which passes no argument.
const char* hapticFeedbackTypeName(HapticFeedbackType type);

class HapticFeedback {
public:
    /// Short vibration (iOS kSystemSoundID_Vibrate / Android LONG_PRESS).
    static bool vibrate();
    /// Collision impact with a light mass (Android VIRTUAL_KEY).
    static bool lightImpact();
    /// Collision impact with a medium mass (Android KEYBOARD_TAP).
    static bool mediumImpact();
    /// Collision impact with a heavy mass (Android CONTEXT_CLICK, API 23+).
    static bool heavyImpact();
    /// Discrete value selection changed (Android CLOCK_TICK).
    static bool selectionClick();
    /// Task completed successfully (Android CONFIRM, API 30+).
    static bool successNotification();
    /// Task produced a warning (Android KEYBOARD_TAP, API 30+).
    static bool warningNotification();
    /// Task failed (Android REJECT, API 30+).
    static bool errorNotification();

    /// Generic entry point used by the helpers above.
    static bool perform(HapticFeedbackType type);
};

class SystemSound {
public:
    /// Short system click (Android FX_KEY_CLICK, iOS system sound 1104).
    static bool playClick();
};

}  // namespace services
}  // namespace cui_core
