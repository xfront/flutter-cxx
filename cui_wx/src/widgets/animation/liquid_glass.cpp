/// @file liquid_glass.cpp — LiquidGlass container + glass overlay render object.
#include "cui/widgets/liquid_glass.h"
#include "cui/foundation/shared_ptr.h"

#include <algorithm>

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>
#include <include/core/SkScalar.h>
#include <include/effects/SkGradient.h>
#include <include/effects/SkImageFilters.h>

namespace cui {

// ── LiquidGlass ──────────────────────────────────────────────────────────────

void LiquidGlass::setEnabled(bool on) {
    enabled_ = on;
    if (anim_)
        on ? anim_->forward() : anim_->reverse();
}

std::unique_ptr<State> LiquidGlass::createState() {
    return std::make_unique<LiquidGlassState>();
}

void LiquidGlassState::initState() {
    auto* w = static_cast<LiquidGlass*>(widget());
    w->anim_ = cui::make_shared<SharedAnimation>(w->durationMs_, w->easing_);
    // Subscribe so the subtree rebuilds whenever the animation ticks.
    unsubscribe_ = w->anim_->animation().progressChanged().subscribe(
        [this](double) { setState(); });
}

void LiquidGlassState::dispose() {
    if (unsubscribe_)
        unsubscribe_();
    if (auto* w = static_cast<LiquidGlass*>(widget()))
        if (w->anim_)
            w->anim_->stop();
}

SharedPtr<Widget> LiquidGlassState::build(BuildContext& context) {
    auto* w = static_cast<LiquidGlass*>(widget());
    double t = w->anim_ ? w->anim_->value() : 0.0;
    if (w->onProgress_)
        w->onProgress_(t);
    // Stack the child beneath a glass overlay whose alpha follows `t`.
    auto glass = cui::make_shared<GlassOverlay>(t, w->config_);
    return cui::make_shared<Stack>(
        std::vector<SharedPtr<Widget>>{w->child_, glass});
}

// ── RenderGlassOverlay ───────────────────────────────────────────────────────

void RenderGlassOverlay::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c)
        return;

    const double overall = config_.opacity * t_;
    const double rim = config_.rimStrength * config_.borderOpacity * t_;
    if (overall <= 0.001 && rim <= 0.001)
        return;  // invisible when t ≈ 0 (toggle off)

    const double x = offset.dx, y = offset.dy;
    const double w = size_.width, h = size_.height;
    SkRect r = SkRect::MakeXYWH((SkScalar)x, (SkScalar)y, (SkScalar)w, (SkScalar)h);
    SkRRect rr;
    rr.setRectXY(r, (SkScalar)config_.cornerRadius, (SkScalar)config_.cornerRadius);

    // 真·背景模糊（上游 `BackdropFilter` 的 SkCanvas 等价物）：用带 backdrop
    // 滤镜的 saveLayer 把「已经画在画布上的内容」按 blur sigma 采样进图层，
    // 霜色 / 高光 / 边缘光随后叠在模糊后的背景上。没有这一步，半透明霜色下面
    // 仍是清晰的壁纸，看起来只是「加了层蒙版」而不是磨砂玻璃。
    bool blurred = false;
    if (config_.blur > 0.5) {
        sk_sp<SkImageFilter> blur = SkImageFilters::Blur(
            (SkScalar)(config_.blur * 0.5), (SkScalar)(config_.blur * 0.5),
            SkTileMode::kClamp, nullptr);
        if (blur) {
            c->save();
            c->clipRRect(rr, true);
            SkPaint layerPaint;
            SkCanvas::SaveLayerRec rec(&r, &layerPaint, blur.get(), 0);
            c->saveLayer(rec);
            blurred = true;
        }
    }

    // Frosted tint fill: top-light / bottom-dark vertical gradient (frostiness
    // absorbs toward black at the bottom). Alpha scales with the animation `t`.
    if (overall > 0.001) {
        SkPaint p;
        SkPoint pts[2] = {{ (SkScalar)x, (SkScalar)y },
                          { (SkScalar)x, (SkScalar)(y + h) }};
        SkColor4f top = { (SkScalar)config_.tint.r, (SkScalar)config_.tint.g,
                          (SkScalar)config_.tint.b,
                          (SkScalar)std::min(1.0, config_.opacity + 0.06) * (SkScalar)t_ };
        SkColor4f bot = { (SkScalar)config_.tint.r, (SkScalar)config_.tint.g,
                          (SkScalar)config_.tint.b,
                          (SkScalar)config_.opacity * (SkScalar)t_ };
        SkColor4f colors[2] = { top, bot };
        SkGradient::Colors gradColors(SkSpan<const SkColor4f>(colors, 2),
                                     SkTileMode::kClamp);
        SkGradient grad(gradColors, {});
        p.setShader(SkShaders::LinearGradient(pts, grad));
        c->drawRRect(rr, p);
        p.setShader(nullptr);
    }

    // Directional specular sheen near the lit edge (light from lightAngle).
    if (config_.specularIntensity * t_ > 0.001) {
        double a = config_.specularIntensity * t_ * 0.5;
        SkPaint sh;
        SkPoint pts[2] = {{ (SkScalar)x, (SkScalar)y },
                          { (SkScalar)x, (SkScalar)(y + h * 0.5) }};
        SkColor4f top = { 1.0f, 1.0f, 1.0f, (SkScalar)a };
        SkColor4f bot = { 1.0f, 1.0f, 1.0f, 0.0f };
        SkColor4f colors[2] = { top, bot };
        SkGradient::Colors gradColors(SkSpan<const SkColor4f>(colors, 2),
                                     SkTileMode::kClamp);
        SkGradient grad(gradColors, {});
        sh.setShader(SkShaders::LinearGradient(pts, grad));
        c->drawRRect(rr, sh);
        sh.setShader(nullptr);
    }

    // Fresnel rim stroke — edge definition (the "glass edge" look).
    if (rim > 0.001) {
        SkPaint stroke;
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setStrokeWidth((SkScalar)config_.borderWidth);
        stroke.setColor4f({ (SkScalar)config_.borderColor.r,
                            (SkScalar)config_.borderColor.g,
                            (SkScalar)config_.borderColor.b, (SkScalar)rim });
        c->drawRRect(rr, stroke);
    }

    // 图层合回画布（restore 弹 saveLayer 的层，再弹 clip）。
    if (blurred) {
        c->restore();
        c->restore();
    }
}

}  // namespace cui_core
