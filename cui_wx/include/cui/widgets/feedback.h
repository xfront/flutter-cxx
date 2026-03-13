#pragma once
/// @file widgets/feedback.h
/// @brief Platform feedback helpers (Flutter: widgets/feedback.dart).
///
/// Flutter:
///   GestureDetector(
///     onTap: Feedback.wrapForTap(_onTap, context),
///     onLongPress: Feedback.wrapForLongPress(_onLongPress, context),
///   );
///
/// cui_core:
///   detector->setOnTap(Feedback::wrapForTap([this] { ... }));
///   detector->setOnLongPress(Feedback::wrapForLongPress([this] { ... }));
///
/// The platform branches mirror upstream 3.47 exactly:
///   forTap        Android/Fuchsia → click system sound, iOS no-op.
///   forLongPress  Android/Fuchsia → vibrate; iOS → click + heavy impact.
/// Semantics events (TapSemanticEvent / LongPressSemanticEvent) are not sent:
/// cui_core has no semantics tree yet.

#include <functional>
#include <utility>

#include "cui/platform/services/haptic_feedback.h"
#include "cui/platform/services/platform_info.h"

namespace cui {

class Feedback {
public:
    static void forTap() {
        switch (services::defaultTargetPlatform()) {
            case services::TargetPlatform::Android:
            case services::TargetPlatform::Fuchsia:
                services::SystemSound::playClick();
                return;
            default:
                return;
        }
    }

    static void forLongPress() {
        switch (services::defaultTargetPlatform()) {
            case services::TargetPlatform::Android:
            case services::TargetPlatform::Fuchsia:
                services::HapticFeedback::vibrate();
                return;
            case services::TargetPlatform::IOS:
                services::SystemSound::playClick();
                services::HapticFeedback::heavyImpact();
                return;
            default:
                return;
        }
    }

    /// Wrap a tap callback so the platform feedback fires first
    /// (upstream `wrapForTap`). A null callback stays null.
    static std::function<void()> wrapForTap(std::function<void()> callback) {
        if (!callback)
            return nullptr;
        return [callback = std::move(callback)] {
            forTap();
            callback();
        };
    }

    static std::function<void()> wrapForLongPress(std::function<void()> callback) {
        if (!callback)
            return nullptr;
        return [callback = std::move(callback)] {
            forLongPress();
            callback();
        };
    }
};

}  // namespace cui_core
