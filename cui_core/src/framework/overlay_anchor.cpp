/// @file overlay_anchor.cpp — OverlayAnchor implementation.
#include "cui/framework/overlay_anchor.h"

#include <include/core/SkMatrix.h>

#include "cui/foundation/painter.h"

namespace cui {

void OverlayAnchor::cache(Painter& painter) {
    // Extract the current translation from the canvas's total matrix.
    // At the point cache() is called, the canvas transform includes all
    // parent translate() calls — i.e. the widget's absolute origin.
    SkMatrix matrix = painter.canvas()->getTotalMatrix();
    x_ = matrix.getTranslateX();
    y_ = matrix.getTranslateY();
    cached_ = true;
}

void OverlayAnchor::translate(Painter& painter) const {
    if (!cached_) return;
    painter.translate(x_, y_);
}

}  // namespace cui_core
