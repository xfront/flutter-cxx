#pragma once
/// @file widgets/liquid_glass.h
/// @brief LiquidGlass — a toggleable liquid-glass (frosted glass) overlay.
///
/// Adapted from rui's liquid-glass optical model (`crates/rui/src/glass.rs`):
///   - a translucent tint fill with a top-light / bottom-dark vertical
///     gradient (frostiness absorbs toward black at the bottom);
///   - a directional specular sheen (light from `lightAngle`);
///   - a Fresnel rim stroke for edge definition.
///
/// The effect is driven by a `SharedAnimation`: `setEnabled(true/false)` plays
/// the animation forward/reverse, and the overlay's opacity follows the
/// animated progress `t` (0 = invisible, 1 = full glass). It is wired into the
/// new 上游 three-layer via `AnimatedBuilder` + the `FrameScheduler` Ticker
/// pump, so the same animation infrastructure that drives any other widget
/// drives the glass fade.

#include <functional>
#include <memory>

#include "cui/framework/framework.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/painting_context.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/animation/shared_animation.h"
#include "cui/widgets/basic.h"
#include "cui/framework/render_object_widget.h"

namespace cui {

/// Visual parameters for the glass surface — a focused subset of rui's
/// `GlassConfig` (see glass.rs for the full model and `standard` / `dark` /
/// `ios27_*` presets).
struct GlassConfig {
    double cornerRadius = 18.0;
    Color tint = Color::fromRGB(0.82, 0.86, 0.94);
    double opacity = 0.32;           ///< overall opacity (0..1)
    double specularIntensity = 1.0;   ///< top sheen strength
    double rimStrength = 0.65;        ///< Fresnel edge-light strength
    double borderWidth = 1.0;
    double frostiness = 0.14;         ///< bottom darkening (frost absorption)
    double blur = 18.0;               ///< backdrop blur radius (renderer concern)
    double lightAngle = -90.0;         ///< degrees; -90 = light from top
    Color borderColor = Color::fromRGB(1.0, 1.0, 1.0);
    double borderOpacity = 1.0;

    static GlassConfig standard() { return GlassConfig{}; }
    static GlassConfig dark() {
        GlassConfig c;
        c.tint = Color::fromRGB(0.10, 0.12, 0.18);
        c.opacity = 0.48;
        c.rimStrength = 0.5;
        c.frostiness = 0.14;
        return c;
    }
};

/// Leaf render object that paints the glass at progress `t` (0..1).
class RenderGlassOverlay : public RenderBox {
public:
    RenderGlassOverlay() = default;

    double t() const { return t_; }
    void setT(double t) { if (t_ != t) { t_ = t; markNeedsPaint(); } }
    const GlassConfig& config() const { return config_; }
    void setConfig(const GlassConfig& c) { config_ = c; markNeedsPaint(); }

    /// The glass overlay is purely decorative — it does not capture pointer
    /// events, letting the child beneath receive them.
    bool hitTestSelf(const Offset&) const override { return false; }

protected:
    void performLayout() override { setSize(constraints().biggest()); }
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    double t_ = 0.0;
    GlassConfig config_;
};

/// Widget that paints the glass overlay for a given progress `t`.
class GlassOverlay : public LeafRenderObjectWidget {
public:
    GlassOverlay(double t, GlassConfig config = GlassConfig::standard(), KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)), t_(t), config_(config) {}

    RenderObject* createRenderObject() override { return new RenderGlassOverlay(); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* g = dynamic_cast<RenderGlassOverlay*>(ro)) {
            g->setT(t_);
            g->setConfig(config_);
        }
    }

private:
    double t_;
    GlassConfig config_;
};

/// A toggleable liquid-glass container. Wraps `child` and overlays an animated
/// glass pane. Call `setEnabled(true/false)` to fade the glass in/out; the
/// `FrameScheduler` Ticker pump advances the animation and `AnimatedBuilder`
/// rebuilds the overlay each frame.
///
/// The "toggle function" the caller wants: `glass->setEnabled(on)`.
class LiquidGlass : public StatefulWidget {
public:
    LiquidGlass(SharedPtr<Widget> child, double durationMs = 300.0,
                Easing easing = Easing::EaseOutCubic,
                GlassConfig config = GlassConfig::standard(),
                std::function<void(double)> onProgress = nullptr, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)),
          child_(std::move(child)),
          durationMs_(durationMs),
          easing_(easing),
          config_(config),
          onProgress_(std::move(onProgress)) {}

    SharedPtr<Widget> child() const { return child_; }
    double durationMs() const { return durationMs_; }
    Easing easing() const { return easing_; }
    const GlassConfig& config() const { return config_; }

    /// The toggle: true fades the glass in, false fades it out.
    bool enabled() const { return enabled_; }
    void setEnabled(bool on);

    /// The underlying self-driving `SharedAnimation`. Exposed so callers / tests
    /// can inspect or deterministically drive it.
    SharedPtr<SharedAnimation> sharedAnimation() const { return anim_; }

    std::unique_ptr<State> createState() override;

private:
    friend class LiquidGlassState;
    SharedPtr<Widget> child_;
    double durationMs_;
    Easing easing_;
    GlassConfig config_;
    std::function<void(double)> onProgress_;
    SharedPtr<SharedAnimation> anim_;
    bool enabled_ = false;
};

class LiquidGlassState : public State {
public:
    void initState() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    Observable<double>::Unsubscribe unsubscribe_;
};

}  // namespace cui_core
