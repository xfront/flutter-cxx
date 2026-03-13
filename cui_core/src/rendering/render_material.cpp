/// @file rendering/render_material.cpp
/// @brief RenderLinearProgressIndicator / RenderCircularProgressIndicator 实现。

#include "cui/rendering/render_material.h"

#include <algorithm>

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkRRect.h>

#include "cui/rendering/painting_context.h"

namespace cui {

namespace {

inline void setPaintColor(SkPaint& p, const Color& c) {
    p.setColor4f(SkColor4f{static_cast<float>(c.r), static_cast<float>(c.g),
                           static_cast<float>(c.b), static_cast<float>(c.a)},
                 nullptr);
}

}  // namespace

// ── RenderLinearProgressIndicator ──────────────────────────────────────────

void RenderLinearProgressIndicator::performLayout() {
    double h = std::max(minHeight_, constraints_.minHeight);
    if (constraints_.hasBoundedHeight())
        h = std::min(h, constraints_.maxHeight);

    double w = constraints_.maxWidth;
    if (w < constraints_.minWidth)
        w = constraints_.minWidth;

    setSize(Size(w, h));
}

void RenderLinearProgressIndicator::performPaint(PaintingContext& context,
                                                 const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c || size_.width <= 0 || size_.height <= 0)
        return;

    double r = size_.height / 2.0;
    SkRect rect =
        SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                         SkScalar(size_.width), SkScalar(size_.height));
    SkRRect rr = SkRRect::MakeRectXY(rect, SkScalar(r), SkScalar(r));

    if (backgroundColor_.a > 0.0) {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);
        setPaintColor(p, backgroundColor_);
        c->drawRRect(rr, p);
    }

    if (valueColor_.a > 0.0) {
        double frac = value_ < 0 ? 1.0 : std::clamp(value_, 0.0, 1.0);
        double fw = size_.width * frac;
        if (fw > 0) {
            SkPaint p;
            p.setAntiAlias(true);
            p.setStyle(SkPaint::kFill_Style);
            setPaintColor(p, valueColor_);
            SkRect fr =
                SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                                 SkScalar(fw), SkScalar(size_.height));
            SkRRect frr = SkRRect::MakeRectXY(fr, SkScalar(r), SkScalar(r));
            c->drawRRect(frr, p);
        }
    }
}

// ── RenderCircularProgressIndicator ────────────────────────────────────────

void RenderCircularProgressIndicator::performLayout() {
    double d = 36.0;  // 上游的 _kMinCircularProgressIndicatorSize。
    double w = constraints_.hasBoundedWidth() ? constraints_.maxWidth : d;
    double h = constraints_.hasBoundedHeight() ? constraints_.maxHeight : d;
    w = std::max(w, constraints_.minWidth);
    h = std::max(h, constraints_.minHeight);
    setSize(Size(w, h));
}

void RenderCircularProgressIndicator::performPaint(PaintingContext& context,
                                                   const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c || size_.width <= 0 || size_.height <= 0)
        return;

    double cx = offset.dx + size_.width / 2.0;
    double cy = offset.dy + size_.height / 2.0;
    double radius = std::min(size_.width, size_.height) / 2.0 - strokeWidth_ / 2.0;
    if (radius <= 0)
        return;

    SkRect oval =
        SkRect::MakeXYWH(SkScalar(cx - radius), SkScalar(cy - radius),
                         SkScalar(2.0 * radius), SkScalar(2.0 * radius));

    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(SkScalar(strokeWidth_));

    if (backgroundColor_.a > 0.0) {
        setPaintColor(p, backgroundColor_);
        c->drawOval(oval, p);
    }

    if (valueColor_.a > 0.0) {
        setPaintColor(p, valueColor_);
        double sweep = value_ < 0 ? 360.0 : std::clamp(value_, 0.0, 1.0) * 360.0;
        // 从顶部（-90°）顺时针扫过 sweep。
        c->drawArc(oval, -90.0f, SkScalar(sweep), false, p);
    }
}

}  // namespace cui_core
