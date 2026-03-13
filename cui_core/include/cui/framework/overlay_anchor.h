#pragma once
/// @file framework/overlay_anchor.h
/// @brief Overlay positioning helper (mirrors rui's OverlayAnchor).
///
/// ComboBox / DropdownButton / PopupMenuButton cache their absolute window-space
/// origin during paint(), then translate back to that point when painting the
/// overlay popup.  This avoids the "double offset" bug where the popup is
/// drawn at the widget's local origin AND the Application translates it to
/// the absolute origin again.
///
/// Usage:
///   // In paint():
///   anchor_.cache(painter);     // captures absolute origin
///   ... draw content ...
///
///   // In paintPopup():
///   anchor_.translate(painter); // translates to cached point
///   ... draw popup content ...

#include "cui/foundation/geometry.h"

namespace cui {

class Painter;

/// Caches an absolute (window-space) anchor point during the widget paint
/// phase and replays it during the overlay paint phase.
class OverlayAnchor {
public:
    /// Cache the painter's current translation as the anchor point.
    /// Call this at the beginning of paint() before any local transforms.
    /// The painter's transform at this point represents the widget's
    /// absolute origin (set up by the parent chain's translate calls).
    void cache(Painter& painter);

    /// Apply the cached anchor as a translation on the painter.
    /// Call this at the beginning of paintPopup() to position the overlay
    /// at the same location as the widget.
    void translate(Painter& painter) const;

    /// The cached point (window-space).
    Point point() const { return {x_, y_}; }

    /// True if cache() has been called at least once.
    bool hasCached() const { return cached_; }

private:
    double x_ = 0;
    double y_ = 0;
    bool cached_ = false;
};

}  // namespace cui_core
