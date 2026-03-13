/// @file haptic_feedback.cpp
/// @brief Haptic / system-sound channel calls (see haptic_feedback.h).

#include "cui/platform/services/haptic_feedback.h"

#include <string>

#include "cui/platform/services/method_channel.h"

namespace cui {
namespace services {

namespace {
constexpr const char* kChannel = "cui_core/platform";
}  // namespace

const char* hapticFeedbackTypeName(HapticFeedbackType type) {
    switch (type) {
        case HapticFeedbackType::Vibrate: return "";
        case HapticFeedbackType::LightImpact: return "HapticFeedbackType.lightImpact";
        case HapticFeedbackType::MediumImpact: return "HapticFeedbackType.mediumImpact";
        case HapticFeedbackType::HeavyImpact: return "HapticFeedbackType.heavyImpact";
        case HapticFeedbackType::SelectionClick: return "HapticFeedbackType.selectionClick";
        case HapticFeedbackType::SuccessNotification:
            return "HapticFeedbackType.successNotification";
        case HapticFeedbackType::WarningNotification:
            return "HapticFeedbackType.warningNotification";
        case HapticFeedbackType::ErrorNotification:
            return "HapticFeedbackType.errorNotification";
    }
    return "";
}

bool HapticFeedback::perform(HapticFeedbackType type) {
    return invokeMethod(kChannel, "HapticFeedback.vibrate", hapticFeedbackTypeName(type))
            .has_value();
}

bool HapticFeedback::vibrate() { return perform(HapticFeedbackType::Vibrate); }
bool HapticFeedback::lightImpact() { return perform(HapticFeedbackType::LightImpact); }
bool HapticFeedback::mediumImpact() { return perform(HapticFeedbackType::MediumImpact); }
bool HapticFeedback::heavyImpact() { return perform(HapticFeedbackType::HeavyImpact); }
bool HapticFeedback::selectionClick() {
    return perform(HapticFeedbackType::SelectionClick);
}
bool HapticFeedback::successNotification() {
    return perform(HapticFeedbackType::SuccessNotification);
}
bool HapticFeedback::warningNotification() {
    return perform(HapticFeedbackType::WarningNotification);
}
bool HapticFeedback::errorNotification() {
    return perform(HapticFeedbackType::ErrorNotification);
}

bool SystemSound::playClick() {
    return invokeMethod(kChannel, "SystemSound.play", "SystemSoundType.click")
            .has_value();
}

}  // namespace services
}  // namespace cui_core
