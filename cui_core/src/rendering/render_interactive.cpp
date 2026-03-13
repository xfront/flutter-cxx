/// @file rendering/render_interactive.cpp
/// @brief RenderCheckbox / RenderSwitch / RenderRadio / RenderSlider 实现。

#include "cui/rendering/render_interactive.h"

#include <algorithm>

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkRRect.h>

#include "cui/foundation/painter.h"
#include "cui/rendering/painting_context.h"

namespace cui {

namespace detail {

void RenderTransition::to(double target) {
    target = clamp01(target);
    if (target == to_)
        return;
    from_ = t_;
    to_ = target;
    startMs_ = steadyNow();
    if (!ticker_)
        ticker_ = std::make_unique<Ticker>([this](Duration) { step(); });
    ticker_->start();
    step();
}

void RenderTransition::step() {
    double e = steadyNow() - startMs_;
    double p = clamp01(e / durationMs_);
    t_ = from_ + (to_ - from_) * ease(p);
    if (onProgress_)
        onProgress_();
    if (p >= 1.0)
        stop();
}

}  // namespace detail

namespace {

inline void setPaintColor(SkPaint& p, const Color& c) {
    p.setColor4f(SkColor4f{static_cast<float>(c.r), static_cast<float>(c.g),
                           static_cast<float>(c.b), static_cast<float>(c.a)},
                 nullptr);
}

/// 颜色线性插值（含 alpha）。
inline Color lerpColor(const Color& a, const Color& b, double t) {
    return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                 a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

}  // namespace

// ── RenderCheckbox ─────────────────────────────────────────────────────────
void RenderCheckbox::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c || size_ <= 0.0)
        return;

    double s = size_;
    double r = s * 0.15;  // 圆角
    SkRect rect = SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                                   SkScalar(s), SkScalar(s));
    SkRRect rr = SkRRect::MakeRectXY(rect, SkScalar(r), SkScalar(r));

    SkPaint p;
    p.setAntiAlias(true);

    // 过渡：t=0 纯描边空框 → t=1 活动色填充 + 对勾。
    double t = transition_.t();

    if (t > 0.0) {
        p.setStyle(SkPaint::kFill_Style);
        setPaintColor(p, lerpColor(inactive_, active_, t));
        c->drawRRect(rr, p);
    }
    if (t < 1.0) {
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(2.0f);
        Color oc = inactive_;
        oc.a *= (1.0 - t);
        setPaintColor(p, oc);
        c->drawRRect(rr, p);
    }

    if (t > 0.0) {
        // 对勾：两段折线，随过渡淡入。
        double x0 = offset.dx + s * 0.22, y0 = offset.dy + s * 0.52;
        double x1 = offset.dx + s * 0.42, y1 = offset.dy + s * 0.72;
        double x2 = offset.dx + s * 0.78, y2 = offset.dy + s * 0.30;
        SkPaint line;
        line.setAntiAlias(true);
        line.setStyle(SkPaint::kStroke_Style);
        line.setStrokeWidth(SkScalar(s * 0.12));
        line.setStrokeCap(SkPaint::kRound_Cap);
        Color cc = check_;
        cc.a *= t;
        setPaintColor(line, cc);
        c->drawLine(SkPoint::Make(SkScalar(x0), SkScalar(y0)),
                    SkPoint::Make(SkScalar(x1), SkScalar(y1)), line);
        c->drawLine(SkPoint::Make(SkScalar(x1), SkScalar(y1)),
                    SkPoint::Make(SkScalar(x2), SkScalar(y2)), line);
    }
}

// ── RenderSwitch ───────────────────────────────────────────────────────────
void RenderSwitch::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c || size_.width <= 0 || size_.height <= 0)
        return;

    double w = size_.width, h = size_.height, tr = thumbRadius_;
    double r = h / 2.0;
    SkRect track = SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                                    SkScalar(w), SkScalar(h));
    SkRRect trr = SkRRect::MakeRectXY(track, SkScalar(r), SkScalar(r));

    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kFill_Style);
    // 轨道色随过渡在关/开色间插值。
    setPaintColor(p, lerpColor(inactive_, active_, transition_.t()));
    c->drawRRect(trr, p);

    // 圆钮：随过渡从左侧滑到右侧。
    double minCx = offset.dx + tr + 2.0;
    double maxCx = offset.dx + w - tr - 2.0;
    double cx = minCx + (maxCx - minCx) * transition_.t();
    double cy = offset.dy + h / 2.0;
    SkPaint thumb;
    thumb.setAntiAlias(true);
    thumb.setStyle(SkPaint::kFill_Style);
    setPaintColor(thumb, Color::fromRGB(1, 1, 1));
    c->drawCircle(SkPoint::Make(SkScalar(cx), SkScalar(cy)), SkScalar(tr), thumb);
}

// ── RenderRadio ────────────────────────────────────────────────────────────
void RenderRadio::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c || size_ <= 0.0)
        return;

    double s = size_;
    double radius = s / 2.0;
    SkPoint center =
        SkPoint::Make(SkScalar(offset.dx + radius), SkScalar(offset.dy + radius));

    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(2.0f);
    setPaintColor(p, selected_ ? active_ : inactive_);
    c->drawCircle(center, SkScalar(radius - 1.0), p);

    if (selected_) {
        SkPaint dot;
        dot.setAntiAlias(true);
        dot.setStyle(SkPaint::kFill_Style);
        setPaintColor(dot, active_);
        c->drawCircle(center, SkScalar(radius * 0.5), dot);
    }
}

// ── RenderSlider ───────────────────────────────────────────────────────────
Rect RenderSlider::bubbleRect() const {
    const double fontSize = 12.0;
    double textW = Painter::measureTextWidth(nullptr, label_, fontSize);
    double w = textW + 16.0;
    double h = 24.0;
    double cx = thumbCenterX();
    // 气泡不出左右边界。
    cx = std::clamp(cx, w / 2.0, std::max(w / 2.0, size_.width - w / 2.0));
    double bottom = size_.height / 2.0 - thumbRadius_ - 8.0;
    return Rect(cx - w / 2.0, bottom - h, w, h);
}

void RenderSlider::updateValueFromPosition(double localX) {
    double inset = thumbRadius_;
    double w = size_.width;
    double trackW = std::max(1.0, w - 2.0 * inset);
    double frac = std::clamp((localX - inset) / trackW, 0.0, 1.0);
    double v = min_ + frac * (max_ - min_);
    if (v != value_) {
        value_ = v;
        markNeedsPaint();
        if (onChanged_)
            onChanged_(v);
    }
}

void RenderSlider::handleEvent(const PointerEvent& event, const HitTestEntry&) {
    if (event.type == PointerEventType::Down) {
        bool primary = (event.pointerKind != PointerDeviceKind::mouse) ||
                       ((event.buttons & kPrimaryMouseButton) != 0);
        if (!primary)
            return;
        dragging_ = true;
        downId_ = event.pointerId;
        if (hasLabel())
            labelT_.to(1.0);
        updateValueFromPosition(event.position.dx);
    } else if (event.type == PointerEventType::Move) {
        if (dragging_ && event.pointerId == downId_)
            updateValueFromPosition(event.position.dx);
    } else if (event.type == PointerEventType::Up) {
        if (event.pointerId == downId_) {
            dragging_ = false;
            downId_ = -1;
            if (hasLabel())
                labelT_.to(0.0);
        }
    }
}

void RenderSlider::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c || size_.width <= 0 || size_.height <= 0)
        return;

    double w = size_.width, h = size_.height, tr = thumbRadius_;
    double y = offset.dy + h / 2.0;
    double inset = tr;
    double xL = offset.dx + inset;
    double xR = offset.dx + w - inset;
    double trackH = std::max(2.0, h * 0.25);

    // 静态轨道。
    SkPaint track;
    track.setAntiAlias(true);
    track.setStyle(SkPaint::kStroke_Style);
    track.setStrokeWidth(SkScalar(trackH));
    track.setStrokeCap(SkPaint::kRound_Cap);
    setPaintColor(track, inactive_);
    c->drawLine(SkPoint::Make(SkScalar(xL), SkScalar(y)),
                SkPoint::Make(SkScalar(xR), SkScalar(y)), track);

    // 活动段 + thumb。
    double frac = (max_ > min_) ? std::clamp((value_ - min_) / (max_ - min_), 0.0, 1.0)
                                : 0.0;
    double thumbX = xL + frac * (xR - xL);
    thumbX_ = thumbX;

    if (frac > 0.0) {
        SkPaint active;
        active.setAntiAlias(true);
        active.setStyle(SkPaint::kStroke_Style);
        active.setStrokeWidth(SkScalar(trackH));
        active.setStrokeCap(SkPaint::kRound_Cap);
        setPaintColor(active, active_);
        c->drawLine(SkPoint::Make(SkScalar(xL), SkScalar(y)),
                    SkPoint::Make(SkScalar(thumbX), SkScalar(y)), active);
    }

    SkPaint thumb;
    thumb.setAntiAlias(true);
    thumb.setStyle(SkPaint::kFill_Style);
    setPaintColor(thumb, thumb_);
    c->drawCircle(SkPoint::Make(SkScalar(thumbX), SkScalar(y)), SkScalar(tr), thumb);

    // 标签气泡：拖拽期间淡入，显示在 thumb 上方（越界绘制）。
    double lt = labelT_.t();
    if (hasLabel() && lt > 0.01) {
        Rect b = bubbleRect();
        SkRect bubbleSk = SkRect::MakeXYWH(SkScalar(offset.dx + b.left()),
                                           SkScalar(offset.dy + b.top()),
                                           SkScalar(b.width), SkScalar(b.height));
        SkRRect bubble =
            SkRRect::MakeRectXY(bubbleSk, SkScalar(4.0), SkScalar(4.0));
        SkPaint bp;
        bp.setAntiAlias(true);
        bp.setStyle(SkPaint::kFill_Style);
        setPaintColor(bp, Color::fromRGBA(0.15, 0.15, 0.15, 0.9 * lt));
        c->drawRRect(bubble, bp);

        const double fontSize = 12.0;
        const char* fam = nullptr;
        double ascent = 0.0, descent = 0.0;
        Painter::fontBand(fam, fontSize, ascent, descent);
        double textW = Painter::measureTextWidth(fam, label_, fontSize);
        Painter tp(c, fam);
        tp.setFontSize(fontSize);
        tp.setFontWeight(Painter::kRegularWeight);
        tp.setSourceColor(Color{1, 1, 1, lt});
        // showText 把基线画在局部 y = fontSize 处：平移到文本中心基线位置。
        double textX = offset.dx + b.left() + (b.width - textW) / 2.0;
        double baselineY = offset.dy + b.top() + (b.height + ascent - descent) / 2.0;
        c->save();
        c->translate(SkScalar(textX), SkScalar(baselineY - fontSize));
        tp.showText(label_.c_str());
        c->restore();
    }
}

// ── RenderCheckbox ─────────────────────────────────────────────────────────
void RenderCheckbox::setValue(bool v) {
    if (v != value_) {
        value_ = v;
        transition_.to(v ? 1.0 : 0.0);
        markNeedsPaint();
    }
}

void RenderCheckbox::performLayout() {
    double s = size_;
    if (constraints_.hasBoundedWidth())
        s = std::min(s, constraints_.maxWidth);
    if (constraints_.hasBoundedHeight())
        s = std::min(s, constraints_.maxHeight);
    setSize(Size(s, s));
}

// ── RenderSwitch ───────────────────────────────────────────────────────────
void RenderSwitch::setValue(bool v) {
    if (v != value_) {
        value_ = v;
        transition_.to(v ? 1.0 : 0.0);
        markNeedsPaint();
    }
}

void RenderSwitch::performLayout() {
    double w = width_, h = height_;
    if (constraints_.hasBoundedWidth())
        w = std::min(w, constraints_.maxWidth);
    if (constraints_.hasBoundedHeight())
        h = std::min(h, constraints_.maxHeight);
    setSize(Size(w, h));
}

// ── RenderRadio ────────────────────────────────────────────────────────────
void RenderRadio::performLayout() {
    double s = size_;
    if (constraints_.hasBoundedWidth())
        s = std::min(s, constraints_.maxWidth);
    if (constraints_.hasBoundedHeight())
        s = std::min(s, constraints_.maxHeight);
    setSize(Size(s, s));
}

// ── RenderSlider ───────────────────────────────────────────────────────────
void RenderSlider::setValue(double v) {
    v = std::clamp(v, min_, max_);
    if (v != value_) {
        value_ = v;
        markNeedsPaint();
    }
}

double RenderSlider::thumbCenterX() const {
    double inset = thumbRadius_;
    double trackW = std::max(1.0, size_.width - 2.0 * inset);
    double frac = (max_ > min_)
                      ? std::clamp((value_ - min_) / (max_ - min_), 0.0, 1.0)
                      : 0.0;
    return inset + frac * trackW;
}

void RenderSlider::performLayout() {
    double h = height_;
    if (constraints_.hasBoundedHeight())
        h = std::min(h, constraints_.maxHeight);
    double w = constraints_.maxWidth;
    if (w < constraints_.minWidth)
        w = constraints_.minWidth;
    setSize(Size(w, h));
}

}  // namespace cui_core
