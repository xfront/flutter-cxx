#pragma once
/// @file painter.h
/// @brief Skia-based 2D painter — the cross-platform self-drawing engine.
///
/// Wraps SkCanvas to provide a simplified drawing API.
/// All coordinates are in widget-local space (translated by the framework).

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkShader.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTextBlob.h>
#include <include/core/SkTypeface.h>
#include <include/effects/SkGradient.h>

#include "cui/foundation/geometry.h"

#if defined(__ANDROID__) || defined(__OHOS__)
// Android 与鸿蒙都没有 fontconfig：字体从 /system/fonts 直接读（见 painter.cpp）。
#include <include/core/SkFontMgr.h>
#elif defined(__linux__)
#include <fontconfig/fontconfig.h>
#include <include/core/SkFontMgr.h>
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#elif defined(_WIN32)
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontStyle.h>

// Skia exports SkFontMgr_New_GDI from the DLL but does not ship a public
// header for it on Windows.  Forward-declare so we can use the GDI font
// manager (which resolves system-installed fonts by family name).
class SkFontMgr;
extern "C++" sk_sp<SkFontMgr> SkFontMgr_New_GDI();
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <string>
#include <unordered_map>

namespace cui {

class Painter {
public:
    /// @param defaultFamily fontconfig family used when setFontFamily() was
    ///                      not called (e.g. "Noto Sans CJK SC" for CJK text).
    explicit Painter(SkCanvas* canvas, const char* defaultFamily = nullptr);

    // ── State ──────────────────────────────────────────────────────────────
    void save();
    void restore();

    void translate(double tx, double ty);
    void scale(double sx, double sy);
    /// Rotate by `degrees` (positive = clockwise on screen — Skia's y axis
    /// points down, so a positive rotation turns +x towards +y). showText()
    /// detects the rotated matrix and falls back to drawing the baked text
    /// image through it, so vertical runs work out of the box.
    void rotate(double degrees);
    void clipRect(const Rect& r);
    /// Rounded clip — Modifier::clipRadius()/Card-style rounded containers.
    void clipRoundedRect(const Rect& r, double radius);

    // ── Clip testing ───────────────────────────────────────────────────────
    /// Overdraw allowance for intersectsClip() callers: covers paint that
    /// legitimately bleeds outside a widget's reported paint bounds (Card's
    /// baked shadow spreads ~8 px, subpixel AA a little more).
    static constexpr double kClipSkipMargin = 24.0;

    /// True when the window-space rect `r` (grown by `margin` pixels) can
    /// contribute any pixel to the active clip. paint() implementations use
    /// this to skip whole subtrees that are entirely clipped away — the
    /// paint-time analogue of 上游 only repainting needsPaint subtrees.
    /// Callers always pass window-space rects (widget frames are absolute);
    /// the rect is mapped through windowToDevice_ before the test — identity
    /// for the on-screen canvas, whose device clip is window coordinates
    /// regardless of the widget-local translations applied while painting.
    /// Offscreen canvases (AnimatedWidget's layer bakes) install their own
    /// mapping so the test stays in one space instead of comparing window
    /// pixels against a layer-local clip.
    bool intersectsClip(const Rect& r, double margin = 0) const;

    /// Window→device mapping consulted by intersectsClip() (default:
    /// identity — device space IS window space). Offscreen canvases whose
    /// transform chain scales/translates installed window pixels must set
    /// this so window-space skip tests remain meaningful.
    void setWindowToDevice(const SkMatrix& m);

    // ── Color / stroke ─────────────────────────────────────────────────────
    void setSourceColor(const Color& c);
    void setSourceRGB(double r, double g, double b);

    // ── Group opacity ──────────────────────────────────────────────────────
    /// Multiplier applied by setSourceColor()/setSourceRGB() to every colour
    /// set while active (reset to 1.0 afterwards). AnimatedWidget drives its
    /// opacity animation with this — the previous approach of poking
    /// skPaint().setAlpha() was clobbered by each child's own setSourceColor()
    /// call, which silently made pulsing labels a visual no-op.
    void setOpacity(double a);
    double opacity() const { return opacity_; }
    void setLineWidth(double w);
    void setFillStyle();

    // ── Font ───────────────────────────────────────────────────────────────
    /// CSS font-weight scale: 400 is the regular cut, 700 is bold.
    static constexpr double kRegularWeight = 400.0;
    static constexpr double kBoldWeight = 700.0;

    void setFontSize(double size);
    void setFontFamily(const char* family);
    /// Requested weight (100–900) and slant.  Rich text is the reason these
    /// exist: a `<strong>` run that silently rendered in the regular cut makes
    /// the whole editor look broken, so the style has to survive all the way
    /// into the rasterised glyph run.
    void setFontWeight(double weight);
    void setFontItalic(bool italic);
    /// Convenience: `bold` picks the 700 cut, anything else the regular one.
    void setFontBold(bool bold);

    struct TextExtents {
        double width;
        double height;
    };

    TextExtents textExtents(const char* text);

    // ── Path operations ────────────────────────────────────────────────────
    void moveTo(double x, double y);
    void lineTo(double x, double y);

    void rectangle(const Rect& r);
    void roundedRectangle(const Rect& r, double radius);
    void circle(double cx, double cy, double radius);

    /// Append an arc to the current path. The arc is a section of an oval
    /// bounded by (cx-radius, cy-radius, cx+radius, cy+radius), sweeping
    /// clockwise from startAngle (degrees, 0 = 3 o'clock) through sweepAngle
    /// degrees. Used by pie/donut charts.
    void arcTo(double cx, double cy, double radius, double startAngle, double sweepAngle);

    /// Draw an arc directly to the canvas (no path accumulation). Useful for
    /// stroked arcs (progress indicators, gauge needles) without affecting
    /// the current path builder state.
    void drawArc(double cx, double cy, double radius, double startAngle, double sweepAngle, double lineWidth);

    // ── Fill / Stroke ──────────────────────────────────────────────────────
    void fill();
    void stroke();

    // ── Rounded rectangles ─────────────────────────────────────────────────
    /// Fill a rounded rectangle directly (no path accumulation).
    void fillRoundedRect(const Rect& r, double radius);
    /// Stroke a rounded rectangle directly.
    void strokeRoundedRect(const Rect& r, double radius);

    // ── Gradients ──────────────────────────────────────────────────────────
    /// Set a linear gradient shader on the paint.  Subsequent fill/stroke
    /// calls use the gradient instead of a flat colour.
    void setLinearGradient(const Point& start, const Point& end,
                           const Color& startColor, const Color& endColor);
    /// Set a radial gradient shader.
    void setRadialGradient(const Point& center, double radius,
                           const Color& innerColor, const Color& outerColor);
    /// Clear the shader, reverting to flat-colour mode.
    void resetGradient();

    // ── Blend mode / layers ────────────────────────────────────────────────
    /// Blend mode enum mapping to Skia's SkBlendMode.
    enum class BlendMode {
        kSrcOver, kSrcIn, kSrcOut, kDstOver, kMultiply, kScreen, kOverlay
    };
    /// Set the blend mode for subsequent draw calls.
    void setBlendMode(BlendMode mode);
    /// Save a layer with a blend mode — everything drawn until the matching
    /// restore() is composited with `mode`.
    void saveLayer(const Rect& bounds, BlendMode mode = BlendMode::kSrcOver);
    /// Save a layer with an alpha value — everything drawn until restore()
    /// is composited with the given opacity.
    void saveLayerAlpha(const Rect& bounds, double alpha);

    // ── Image drawing ──────────────────────────────────────────────────────
    /// Draw an SkImage at (x, y), optionally scaled to (w, h).
    void drawImageSkia(sk_sp<SkImage> image, double x, double y, double w = -1, double h = -1);

    // ── Text ───────────────────────────────────────────────────────────────
    void showText(const char* text);

    // ── Surface operations ─────────────────────────────────────────────────
    void drawSurface(SkSurface* surface, double x, double y);

    // ── Raw access ─────────────────────────────────────────────────────────
    SkCanvas* canvas() { return canvas_; }
    SkPaint& skPaint() { return paint_; }

    // ── Font utilities ──────────────────────────────────────────────────
    static sk_sp<SkTypeface> resolveTypeface(const char* family, SkFontStyle style = SkFontStyle::Normal());

    static SkFont fontFor(const char* family, double size, double weight = kRegularWeight,
                          bool italic = false);

    static double measureTextWidth(const char* family, const std::string& text, double size,
                                  double weight = kRegularWeight, bool italic = false);

    static void fontBand(const char* family, double size, double weight, bool italic,
                         double& ascent, double& descent);
    static void fontBand(const char* family, double size, double& ascent, double& descent);

    static double textHeight(const char* family, double size, double weight = kRegularWeight, bool italic = false);

    static double baselineOffset(const char* family, double size, double weight = kRegularWeight, bool italic = false);

    static double centeredBaselineY(double containerH, const char* family, double size,
                                    double weight = kRegularWeight, bool italic = false);

private:
    /// Map our BlendMode enum to Skia's SkBlendMode.
    static SkBlendMode toSkBlendMode(BlendMode m);

    /// Font with the active family and style resolved through the system font
    /// manager so CJK glyphs can be picked up.
    SkFont makeFont() const;

    /// Cache-key fragment for the active font style.  Weight and slant change
    /// both the raster and its advance, so every text cache has to separate
    /// them — otherwise a bold run silently hits the regular image and the
    /// formatting disappears without a single error.
    std::string styleKey() const;

    /// See intersectsClip() / setWindowToDevice().
    SkMatrix windowToDevice_ = SkMatrix::I();

    /// Baseline position inside the textImage() rasters: a little below the
    /// image top so tall glyphs (CJK ascenders reach ~1.16 × size) cannot
    /// clip. showText() blits at (0, fontSize_ - bakedBaseline()) so the run
    /// still lands with its baseline at y = fontSize_ in local space.
    static double bakedBaseline(double size);

    /// Room a synthetic-italic (skewed) run needs inside its baked raster: the
    /// shear slides the whole baseline row left by |skew| × baseline, so the
    /// bake draws the run at +pad and showText() blits at -pad — the ink still
    /// starts at the layout origin.  Real italic faces carry no skew.
    static double italicPad(const SkFont& font, double baseline);

    /// Bake scale used by the device-aligned text path: quantised to 0.02
    /// steps so breathing-style scale animations reuse one baked image per
    /// level instead of re-rasterising the run every frame.
    static double quantisedScale(double s);

    /// Rasterised-run cache behind showText().
    const sk_sp<SkImage>& textImage(const char* text, double qx, double qy, double* padOut = nullptr) const;

    SkCanvas* canvas_;
    SkPaint paint_;
    SkPathBuilder path_;
    SkScalar fontSize_ = 14.0f;
    SkScalar fontWeight_ = kRegularWeight;
    bool fontItalic_ = false;
    const char* fontFamily_ = "";
    double opacity_ = 1.0;
};

}  // namespace cui_core
