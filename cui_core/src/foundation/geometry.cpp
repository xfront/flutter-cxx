/// @file geometry.cpp — Geometry implementations (moved from geometry.h)
#include "cui/foundation/geometry.h"

namespace cui {

Rect Rect::unite(const Rect& o) const {
    if (width <= 0 && height <= 0)
        return o;
    if (o.width <= 0 && o.height <= 0)
        return *this;
    const double l = x < o.x ? x : o.x;
    const double t = y < o.y ? y : o.y;
    const double r = right() > o.right() ? right() : o.right();
    const double b = bottom() > o.bottom() ? bottom() : o.bottom();
    return {l, t, r - l, b - t};
}

Rect Rect::intersect(const Rect& o) const {
    if (!intersects(o))
        return {};
    const double l = x > o.x ? x : o.x;
    const double t = y > o.y ? y : o.y;
    const double r = right() < o.right() ? right() : o.right();
    const double b = bottom() < o.bottom() ? bottom() : o.bottom();
    return {l, t, r - l, b - t};
}

}  // namespace cui_core
