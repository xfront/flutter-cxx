/// @file painter.cpp — Painter implementation (moved from painter.h to reduce header bloat)
#include "cui/foundation/painter.h"

#include <include/core/SkFontMetrics.h>
#include <include/core/SkSurface.h>

#if defined(__ANDROID__) || defined(__OHOS__)
// 鸿蒙和 Android 一样没有 fontconfig，走同一个目录字体管理器。
#include <include/ports/SkFontMgr_directory.h>
#elif defined(__EMSCRIPTEN__)
#include <include/core/SkData.h>
// The ports/SkFontMgr_*.h factories only forward-declare SkFontMgr, so the
// matchFamilyStyle / legacyMakeTypeface calls below need the full class.
#include <include/core/SkFontMgr.h>
#include <include/ports/SkFontMgr_data.h>
#include <include/ports/SkFontMgr_empty.h>

#include <cstdio>
#include <cstdlib>
#include <vector>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <string>
#include <unordered_map>

namespace cui {

// ── Constructor ──────────────────────────────────────────────────────────────

Painter::Painter(SkCanvas* canvas, const char* defaultFamily) : canvas_(canvas) {
    if (defaultFamily && *defaultFamily)
        fontFamily_ = defaultFamily;
}

// ── State operations ─────────────────────────────────────────────────────────

void Painter::save() { canvas_->save(); }
void Painter::restore() { canvas_->restore(); }

void Painter::translate(double tx, double ty) {
    canvas_->translate(static_cast<SkScalar>(tx), static_cast<SkScalar>(ty));
}

void Painter::scale(double sx, double sy) {
    canvas_->scale(static_cast<SkScalar>(sx), static_cast<SkScalar>(sy));
}

void Painter::rotate(double degrees) {
    canvas_->rotate(static_cast<SkScalar>(degrees));
}

void Painter::clipRect(const Rect& r) {
    if (!canvas_) return;
    const double w = std::max(0.0, r.width);
    const double h = std::max(0.0, r.height);
    canvas_->clipRect(SkRect::MakeXYWH(static_cast<SkScalar>(r.x), static_cast<SkScalar>(r.y),
                                       static_cast<SkScalar>(w), static_cast<SkScalar>(h)));
}

void Painter::clipRoundedRect(const Rect& r, double radius) {
    if (!canvas_) return;
    const double w = std::max(0.0, r.width);
    const double h = std::max(0.0, r.height);
    SkRRect rr;
    rr.setRectXY(SkRect::MakeXYWH(static_cast<SkScalar>(r.x), static_cast<SkScalar>(r.y),
                                  static_cast<SkScalar>(w), static_cast<SkScalar>(h)),
                 static_cast<SkScalar>(radius), static_cast<SkScalar>(radius));
    canvas_->clipRRect(rr, true);
}

bool Painter::intersectsClip(const Rect& r, double margin) const {
    const SkRect wr = SkRect::MakeLTRB(static_cast<SkScalar>(r.x - margin), static_cast<SkScalar>(r.y - margin),
                                       static_cast<SkScalar>(r.x + r.width + margin),
                                       static_cast<SkScalar>(r.y + r.height + margin));
    SkRect mapped;
    if (!windowToDevice_.mapRect(&mapped, wr))
        return true;  // degenerate mapping — err towards painting
    const SkIRect cb = canvas_->getDeviceClipBounds();
    return mapped.left() < cb.right() && mapped.right() > cb.left() && mapped.top() < cb.bottom() &&
           mapped.bottom() > cb.top();
}

void Painter::setWindowToDevice(const SkMatrix& m) { windowToDevice_ = m; }

// ── Color / stroke ───────────────────────────────────────────────────────────

void Painter::setSourceColor(const Color& c) {
    paint_.setColor(SkColorSetARGB(static_cast<uint8_t>(c.a * opacity_ * 255), static_cast<uint8_t>(c.r * 255),
                                   static_cast<uint8_t>(c.g * 255), static_cast<uint8_t>(c.b * 255)));
}

void Painter::setSourceRGB(double r, double g, double b) {
    paint_.setColor(SkColorSetARGB(static_cast<uint8_t>(opacity_ * 255), static_cast<uint8_t>(r * 255),
                                   static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255)));
}

void Painter::setOpacity(double a) {
    opacity_ = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
}

void Painter::setLineWidth(double w) {
    paint_.setStyle(SkPaint::kStroke_Style);
    paint_.setStrokeWidth(static_cast<SkScalar>(w));
}

void Painter::setFillStyle() {
    paint_.setStyle(SkPaint::kFill_Style);
}

// ── Font ─────────────────────────────────────────────────────────────────────

void Painter::setFontSize(double size) {
    fontSize_ = static_cast<SkScalar>(size);
}

void Painter::setFontFamily(const char* family) {
    fontFamily_ = family;
}

void Painter::setFontWeight(double weight) {
    fontWeight_ = static_cast<SkScalar>(weight);
}

void Painter::setFontItalic(bool italic) {
    fontItalic_ = italic;
}

void Painter::setFontBold(bool bold) {
    setFontWeight(bold ? kBoldWeight : kRegularWeight);
}

// ── Path operations ──────────────────────────────────────────────────────────

void Painter::moveTo(double x, double y) {
    path_.moveTo(static_cast<SkScalar>(x), static_cast<SkScalar>(y));
}

void Painter::lineTo(double x, double y) {
    path_.lineTo(static_cast<SkScalar>(x), static_cast<SkScalar>(y));
}

void Painter::rectangle(const Rect& r) {
    const double w = std::max(0.0, r.width);
    const double h = std::max(0.0, r.height);
    path_.addRect(SkRect::MakeXYWH(static_cast<SkScalar>(r.x), static_cast<SkScalar>(r.y),
                                   static_cast<SkScalar>(w), static_cast<SkScalar>(h)));
}

void Painter::roundedRectangle(const Rect& r, double radius) {
    if (!canvas_) return;
    const double w = std::max(0.0, r.width);
    const double h = std::max(0.0, r.height);
    if (radius < 0) radius = 0;
    SkRRect rr;
    rr.setRectXY(SkRect::MakeXYWH(static_cast<SkScalar>(r.x), static_cast<SkScalar>(r.y),
                                  static_cast<SkScalar>(w), static_cast<SkScalar>(h)),
                 static_cast<SkScalar>(radius), static_cast<SkScalar>(radius));
    path_.addRRect(rr);
}

void Painter::circle(double cx, double cy, double radius) {
    // Debug log for negative radius
    if (radius < 0) {
        fprintf(stderr, "[Painter::circle] WARNING: negative radius %.2f at (%.2f, %.2f)\n",
                radius, cx, cy);
    }
    path_.addCircle(static_cast<SkScalar>(cx), static_cast<SkScalar>(cy), static_cast<SkScalar>(std::max(0.0, radius)));
}

void Painter::arcTo(double cx, double cy, double radius, double startAngle, double sweepAngle) {
    SkRect oval = SkRect::MakeXYWH(static_cast<SkScalar>(cx - radius), static_cast<SkScalar>(cy - radius),
                                   static_cast<SkScalar>(radius * 2), static_cast<SkScalar>(radius * 2));
    path_.arcTo(oval, static_cast<SkScalar>(startAngle), static_cast<SkScalar>(sweepAngle), false);
}

void Painter::drawArc(double cx, double cy, double radius, double startAngle, double sweepAngle, double lineWidth) {
    // Debug: check canvas state
    if (!canvas_) {
        fprintf(stderr, "[Painter::drawArc] ERROR: canvas is null!\n");
        return;
    }
    SkRect oval = SkRect::MakeXYWH(static_cast<SkScalar>(cx - radius), static_cast<SkScalar>(cy - radius),
                                   static_cast<SkScalar>(radius * 2), static_cast<SkScalar>(radius * 2));
    SkPaint arcPaint = paint_;
    arcPaint.setStyle(SkPaint::kStroke_Style);
    arcPaint.setStrokeWidth(static_cast<SkScalar>(lineWidth));
    arcPaint.setAntiAlias(true);
    canvas_->drawArc(oval, static_cast<SkScalar>(startAngle), static_cast<SkScalar>(sweepAngle), false, arcPaint);
}

// ── Fill / Stroke ────────────────────────────────────────────────────────────

void Painter::fill() {
    paint_.setStyle(SkPaint::kFill_Style);
    // Debug: check if path is empty before drawing
    SkPath detached = path_.detach();
    if (detached.isEmpty()) {
        // Empty path is OK, just skip drawing
        return;
    }
    SkRect bounds = detached.getBounds();
    if (bounds.width() < 0 || bounds.height() < 0 || 
        bounds.width() > 10000 || bounds.height() > 10000 ||
        std::isnan(bounds.width()) || std::isnan(bounds.height()) ||
        std::isinf(bounds.width()) || std::isinf(bounds.height())) {
        fprintf(stderr, "[Painter::fill] WARNING: suspicious path bounds: %.2fx%.2f at (%.2f, %.2f)\n",
                bounds.width(), bounds.height(), bounds.x(), bounds.y());
    }
    // Debug: check canvas state
    if (!canvas_) {
        fprintf(stderr, "[Painter::fill] ERROR: canvas is null!\n");
        return;
    }
    canvas_->drawPath(detached, paint_);
}

void Painter::stroke() {
    paint_.setStyle(SkPaint::kStroke_Style);
    // Debug: check if path is empty before drawing
    SkPath detached = path_.detach();
    if (detached.isEmpty()) {
        // Empty path is OK, just skip drawing
        return;
    }
    SkRect bounds = detached.getBounds();
    if (bounds.width() < 0 || bounds.height() < 0 || 
        bounds.width() > 10000 || bounds.height() > 10000 ||
        std::isnan(bounds.width()) || std::isnan(bounds.height()) ||
        std::isinf(bounds.width()) || std::isinf(bounds.height())) {
        fprintf(stderr, "[Painter::stroke] WARNING: suspicious path bounds: %.2fx%.2f at (%.2f, %.2f)\n",
                bounds.width(), bounds.height(), bounds.x(), bounds.y());
    }
    // Debug: check canvas state
    if (!canvas_) {
        fprintf(stderr, "[Painter::stroke] ERROR: canvas is null!\n");
        return;
    }
    canvas_->drawPath(detached, paint_);
}

// ── Rounded rectangles ───────────────────────────────────────────────────────

void Painter::fillRoundedRect(const Rect& r, double radius) {
    if (!canvas_) return;
    const double w = std::max(0.0, r.width);
    const double h = std::max(0.0, r.height);
    SkRRect rr;
    rr.setRectXY(SkRect::MakeXYWH(static_cast<SkScalar>(r.x), static_cast<SkScalar>(r.y),
                                  static_cast<SkScalar>(w), static_cast<SkScalar>(h)),
                 static_cast<SkScalar>(radius), static_cast<SkScalar>(radius));
    SkPaint p = paint_;
    p.setStyle(SkPaint::kFill_Style);
    p.setAntiAlias(true);
    canvas_->drawRRect(rr, p);
}

void Painter::strokeRoundedRect(const Rect& r, double radius) {
    if (!canvas_) return;
    const double w = std::max(0.0, r.width);
    const double h = std::max(0.0, r.height);
    SkRRect rr;
    rr.setRectXY(SkRect::MakeXYWH(static_cast<SkScalar>(r.x), static_cast<SkScalar>(r.y),
                                  static_cast<SkScalar>(w), static_cast<SkScalar>(h)),
                 static_cast<SkScalar>(radius), static_cast<SkScalar>(radius));
    SkPaint p = paint_;
    p.setStyle(SkPaint::kStroke_Style);
    p.setAntiAlias(true);
    canvas_->drawRRect(rr, p);
}

// ── Gradients ────────────────────────────────────────────────────────────────

void Painter::setLinearGradient(const Point& start, const Point& end,
                                const Color& startColor, const Color& endColor) {
    SkPoint pts[2] = {
        {static_cast<SkScalar>(start.x), static_cast<SkScalar>(start.y)},
        {static_cast<SkScalar>(end.x),   static_cast<SkScalar>(end.y)}
    };
    SkColor4f colors[2] = {
        SkColor4f{static_cast<float>(startColor.r), static_cast<float>(startColor.g),
                  static_cast<float>(startColor.b), static_cast<float>(startColor.a)},
        SkColor4f{static_cast<float>(endColor.r), static_cast<float>(endColor.g),
                  static_cast<float>(endColor.b), static_cast<float>(endColor.a)}
    };
    SkGradient::Colors gradColors(SkSpan<const SkColor4f>(colors, 2), SkTileMode::kClamp);
    SkGradient grad(gradColors, {});
    paint_.setShader(SkShaders::LinearGradient(pts, grad));
}

void Painter::setRadialGradient(const Point& center, double radius,
                                const Color& innerColor, const Color& outerColor) {
    SkColor4f colors[2] = {
        SkColor4f{static_cast<float>(innerColor.r), static_cast<float>(innerColor.g),
                  static_cast<float>(innerColor.b), static_cast<float>(innerColor.a)},
        SkColor4f{static_cast<float>(outerColor.r), static_cast<float>(outerColor.g),
                  static_cast<float>(outerColor.b), static_cast<float>(outerColor.a)}
    };
    SkGradient::Colors gradColors(SkSpan<const SkColor4f>(colors, 2), SkTileMode::kClamp);
    SkGradient grad(gradColors, {});
    paint_.setShader(SkShaders::RadialGradient(
        {static_cast<SkScalar>(center.x), static_cast<SkScalar>(center.y)},
        static_cast<SkScalar>(radius), grad));
}

void Painter::resetGradient() {
    paint_.setShader(nullptr);
}

// ── Blend mode / layers ──────────────────────────────────────────────────────

void Painter::setBlendMode(BlendMode mode) {
    paint_.setBlendMode(toSkBlendMode(mode));
}

void Painter::saveLayer(const Rect& bounds, BlendMode mode) {
    if (!canvas_) return;
    const double w = std::max(0.0, bounds.width);
    const double h = std::max(0.0, bounds.height);
    SkPaint layerPaint;
    layerPaint.setBlendMode(toSkBlendMode(mode));
    canvas_->saveLayer(SkRect::MakeXYWH(static_cast<SkScalar>(bounds.x), static_cast<SkScalar>(bounds.y),
                                         static_cast<SkScalar>(w), static_cast<SkScalar>(h)),
                       &layerPaint);
}

void Painter::saveLayerAlpha(const Rect& bounds, double alpha) {
    if (!canvas_) return;
    const double w = std::max(0.0, bounds.width);
    const double h = std::max(0.0, bounds.height);
    SkPaint layerPaint;
    layerPaint.setAlpha(static_cast<uint8_t>(std::clamp(alpha, 0.0, 1.0) * 255));
    canvas_->saveLayer(SkRect::MakeXYWH(static_cast<SkScalar>(bounds.x), static_cast<SkScalar>(bounds.y),
                                         static_cast<SkScalar>(w), static_cast<SkScalar>(h)),
                       &layerPaint);
}

// ── Image drawing ────────────────────────────────────────────────────────────

void Painter::drawImageSkia(sk_sp<SkImage> image, double x, double y, double w, double h) {
    if (!image) return;
    // Debug: check canvas state
    if (!canvas_) {
        fprintf(stderr, "[Painter::drawImageSkia] ERROR: canvas is null!\n");
        return;
    }
    if (w > 0 && h > 0) {
        SkRect dst = SkRect::MakeXYWH(static_cast<SkScalar>(x), static_cast<SkScalar>(y),
                                       static_cast<SkScalar>(w), static_cast<SkScalar>(h));
        canvas_->drawImageRect(image.get(), dst, SkSamplingOptions(SkFilterMode::kLinear), &paint_);
    } else {
        canvas_->drawImage(image.get(), static_cast<SkScalar>(x), static_cast<SkScalar>(y),
                           SkSamplingOptions(), &paint_);
    }
}

// ── Surface operations ───────────────────────────────────────────────────────

void Painter::drawSurface(SkSurface* surface, double x, double y) {
    // Debug: check canvas state
    if (!canvas_) {
        fprintf(stderr, "[Painter::drawSurface] ERROR: canvas is null!\n");
        return;
    }
    if (!surface) {
        fprintf(stderr, "[Painter::drawSurface] WARNING: surface is null!\n");
        return;
    }
    canvas_->drawImage(surface->makeImageSnapshot().get(), static_cast<SkScalar>(x), static_cast<SkScalar>(y));
}

// ── Private helpers ──────────────────────────────────────────────────────────

SkBlendMode Painter::toSkBlendMode(BlendMode m) {
    switch (m) {
        case BlendMode::kSrcOver:  return SkBlendMode::kSrcOver;
        case BlendMode::kSrcIn:    return SkBlendMode::kSrcIn;
        case BlendMode::kSrcOut:   return SkBlendMode::kSrcOut;
        case BlendMode::kDstOver:  return SkBlendMode::kDstOver;
        case BlendMode::kMultiply: return SkBlendMode::kMultiply;
        case BlendMode::kScreen:   return SkBlendMode::kScreen;
        case BlendMode::kOverlay:  return SkBlendMode::kOverlay;
    }
    return SkBlendMode::kSrcOver;
}

SkFont Painter::makeFont() const {
    SkFont font;
    const int skWeight = static_cast<int>(std::clamp(fontWeight_, 100.0f, 900.0f));
    const SkFontStyle style(skWeight, SkFontStyle::kNormal_Width,
                            fontItalic_ ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    const bool hasFamily = fontFamily_ && *fontFamily_;
    if (sk_sp<SkTypeface> tf = resolveTypeface(hasFamily ? fontFamily_ : "", style))
        font.setTypeface(std::move(tf));
    font.setSize(fontSize_);
    font.setEdging(SkFont::Edging::kAntiAlias);
    if (const SkTypeface* tf = font.getTypeface()) {
        const SkFontStyle got = tf->fontStyle();
        if (skWeight >= static_cast<int>(kBoldWeight) && got.weight() < skWeight)
            font.setEmbolden(true);
        if (fontItalic_ && got.slant() == SkFontStyle::kUpright_Slant)
            font.setSkewX(-0.22f);
    }
    return font;
}

std::string Painter::styleKey() const {
    std::string k;
    k.reserve(16);
    k += std::to_string(static_cast<long>(std::lround(fontWeight_)));
    k += fontItalic_ ? ",i" : ",r";
    return k;
}

double Painter::bakedBaseline(double size) {
    return size * 0.85;
}

double Painter::italicPad(const SkFont& font, double baseline) {
    if (font.getSkewX() == 0.0f)
        return 0.0;
    return std::abs(static_cast<double>(font.getSkewX())) * baseline;
}

double Painter::quantisedScale(double s) {
    return static_cast<double>(std::lround(s * 50.0)) / 50.0;
}

// ── Text measurement cache ───────────────────────────────────────────────────

Painter::TextExtents Painter::textExtents(const char* text) {
    // Early exit for null or empty text
    if (!text || !*text) {
        return {0.0, 0.0};
    }
    
    // Debug log
    size_t textLen = strlen(text);
    if (textLen > 10000) {
        fprintf(stderr, "[Painter::textExtents] WARNING: very long text len=%zu\n", textLen);
    }
    
    static std::list<std::string> order;
    struct Entry {
        TextExtents ext;
        std::list<std::string>::iterator it;
    };
    static std::unordered_map<std::string, Entry> cache;
    constexpr size_t kLimit = 8192;
    std::string key;
    key.reserve(48);
    key += fontFamily_ ? fontFamily_ : "";
    key += '\x1f';
    key += std::to_string(static_cast<long>(std::lround(fontSize_ * 4.0)));
    key += '\x1f';
    key += styleKey();
    key += '\x1f';
    key += text;
    if (auto it = cache.find(key); it != cache.end()) {
        order.splice(order.end(), order, it->second.it);
        return it->second.ext;
    }
    SkFont font = makeFont();
    SkRect bounds;
    font.measureText(text, strlen(text), SkTextEncoding::kUTF8, &bounds);
    TextExtents ext{bounds.width(), bounds.height()};
    if (cache.size() >= kLimit) {
        for (size_t i = 0; i < kLimit / 4 && !order.empty(); ++i) {
            cache.erase(order.front());
            order.pop_front();
        }
    }
    order.push_back(key);
    auto ins = cache.emplace(std::move(key), Entry{ext, std::prev(order.end())}).first;
    return ins->second.ext;
}

// ── Static font utilities ────────────────────────────────────────────────────

#if defined(__ANDROID__) || defined(__OHOS__)
sk_sp<SkTypeface> Painter::resolveTypeface(const char* family, SkFontStyle style) {
    // Android 没有 fontconfig，也不带 fonts.xml 的解析器；这版 Skia 已移除
    // SkTypeface::MakeFromFile（只剩 SkFontMgr::makeFromFile），所以走 FreeType 的
    // 目录字体管理器直接扫 /system/fonts（ports/SkFontMgr_directory.h）。
    // 鸿蒙同一份实现：NDK sysroot 里也没有 fontconfig，系统字体同样在 /system/fonts。
    static const sk_sp<SkFontMgr> mgr = SkFontMgr_New_Custom_Directory("/system/fonts");
    if (!mgr)
        return nullptr;
    static std::unordered_map<std::string, sk_sp<SkTypeface>> cache;
    const std::string key = std::string(family ? family : "") + '\x1f' + std::to_string(style.weight()) +
                            ',' + std::to_string(static_cast<int>(style.slant()));
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    sk_sp<SkTypeface> tf = *family ? mgr->matchFamilyStyle(family, style) : nullptr;
    if (!tf) {
        // 默认字面按 CJK 优先：系统里的 Noto Sans CJK / DroidSansFallback 才能
        // 覆盖中文，Roboto 只作为最后兵底。
        for (const char* fb : {"Noto Sans CJK SC", "Noto Sans CJK JP", "Droid Sans Fallback", "Roboto"}) {
            if ((tf = mgr->matchFamilyStyle(fb, style)))
                break;
        }
    }
    if (!tf)
        tf = mgr->legacyMakeTypeface(nullptr, style);
    cache.emplace(key, tf);
    return tf;
}
#elif defined(__linux__)
sk_sp<SkTypeface> Painter::resolveTypeface(const char* family, SkFontStyle style) {
    static const sk_sp<SkFontMgr> mgr =
            SkFontMgr_New_FontConfig(FcInitLoadConfigAndFonts(), SkFontScanner_Make_FreeType());
    if (!mgr)
        return nullptr;
    static std::unordered_map<std::string, sk_sp<SkTypeface>> cache;
    const std::string key = std::string(family ? family : "") + '\x1f' + std::to_string(style.weight()) +
                            ',' + std::to_string(static_cast<int>(style.slant()));
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    sk_sp<SkTypeface> tf =
            *family ? mgr->matchFamilyStyle(family, style) : mgr->legacyMakeTypeface(nullptr, style);
    cache.emplace(key, tf);
    return tf;
}
#elif defined(_WIN32)
sk_sp<SkTypeface> Painter::resolveTypeface(const char* family, SkFontStyle style) {
    static const sk_sp<SkFontMgr> mgr = SkFontMgr_New_GDI();
    if (!mgr)
        return nullptr;
    static std::unordered_map<std::string, sk_sp<SkTypeface>> cache;
    const std::string key = std::string(family ? family : "") + '\x1f' + std::to_string(style.weight()) +
                            ',' + std::to_string(static_cast<int>(style.slant()));
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    sk_sp<SkTypeface> tf =
            *family ? mgr->matchFamilyStyle(family, style) : mgr->legacyMakeTypeface(nullptr, style);
    if (!tf) {
        for (const char* fb : {"Microsoft YaHei", "SimSun", "Segoe UI", "Arial"}) {
            if ((tf = mgr->matchFamilyStyle(fb, style)))
                break;
        }
    }
    if (!tf)
        tf = mgr->legacyMakeTypeface(nullptr, style);
    cache.emplace(key, tf);
    return tf;
}
#elif defined(__EMSCRIPTEN__)
sk_sp<SkTypeface> Painter::resolveTypeface(const char* family, SkFontStyle style) {
    // Browsers expose no system font manager to wasm. Text therefore needs a
    // font shipped with the app: `src/platform/web/fonts/` is preloaded to
    // /cui_core/fonts (see src/platform/web/README.md), or CUI_WEB_FONT can point
    // at an absolute MEMFS path. Without one, the empty manager makes text
    // invisible.
    static const sk_sp<SkFontMgr> mgr = [] {
        const char* path = std::getenv("CUI_WEB_FONT");
        if (!path || !*path)
            path = "/cui_core/fonts/font.ttf";
        std::vector<sk_sp<SkData>> fonts;
        if (FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            if (auto data = SkData::MakeFromFileName(path))
                fonts.push_back(std::move(data));
        }
        if (fonts.empty()) {
            std::fprintf(stderr,
                         "cui_core: no web font at %s — bundle a TTF/OTF under "
                         "src/platform/web/fonts/\n",
                         path);
            return SkFontMgr_New_Custom_Empty();
        }
        return SkFontMgr_New_Custom_Data(SkSpan<sk_sp<SkData>>(fonts.data(), fonts.size()));
    }();
    if (!mgr)
        return nullptr;
    static std::unordered_map<std::string, sk_sp<SkTypeface>> cache;
    const std::string key = std::string(family ? family : "") + '\x1f' + std::to_string(style.weight()) +
                            ',' + std::to_string(static_cast<int>(style.slant()));
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    sk_sp<SkTypeface> tf =
            *family ? mgr->matchFamilyStyle(family, style) : nullptr;
    if (!tf)
        tf = mgr->legacyMakeTypeface(nullptr, style);
    cache.emplace(key, tf);
    return tf;
}
#else
sk_sp<SkTypeface> Painter::resolveTypeface(const char*, SkFontStyle) { return nullptr; }
#endif

SkFont Painter::fontFor(const char* family, double size, double weight, bool italic) {
    SkFont font;
    const int skWeight = static_cast<int>(std::clamp(weight, 100.0, 900.0));
    const SkFontStyle style(skWeight, SkFontStyle::kNormal_Width,
                            italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    const bool hasFamily = family && *family;
    if (sk_sp<SkTypeface> tf = resolveTypeface(hasFamily ? family : "", style))
        font.setTypeface(std::move(tf));
    font.setSize(static_cast<SkScalar>(size));
    font.setEdging(SkFont::Edging::kAntiAlias);
    if (const SkTypeface* tf = font.getTypeface()) {
        const SkFontStyle got = tf->fontStyle();
        if (skWeight >= static_cast<int>(kBoldWeight) && got.weight() < skWeight)
            font.setEmbolden(true);
        if (italic && got.slant() == SkFontStyle::kUpright_Slant)
            font.setSkewX(-0.22f);
    }
    return font;
}

double Painter::measureTextWidth(const char* family, const std::string& text, double size,
                                 double weight, bool italic) {
    if (text.empty())
        return 0.0;
    
    // Use Skia's precise text measurement instead of rough estimation
    SkFont font = fontFor(family, size, weight, italic);
    if (!font.getTypeface()) {
        // Fallback to rough estimation if font loading fails
        int chars = 0;
        for (unsigned char c : text)
            if ((c & 0xC0) != 0x80)
                ++chars;
        double w = chars * size * 0.62;
        if (weight >= kBoldWeight)
            w *= 1.06;
        return w;
    }
    
    // Use Skia's measureText for accurate width
    const double w = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
    return w;
}

void Painter::fontBand(const char* family, double size, double weight, bool italic,
                       double& ascent, double& descent) {
    SkFont font = fontFor(family, size, weight, italic);
    if (!font.getTypeface()) {
        ascent = size * 0.8;
        descent = size * 0.2;
        return;
    }
    SkFontMetrics m;
    if (font.getMetrics(&m) && m.fAscent < 0) {
        ascent = -m.fAscent;
        descent = m.fDescent > 0 ? m.fDescent : size * 0.25;
    } else {
        ascent = size * 0.8;
        descent = size * 0.2;
    }
}

void Painter::fontBand(const char* family, double size, double& ascent, double& descent) {
    fontBand(family, size, kRegularWeight, false, ascent, descent);
}

double Painter::textHeight(const char* family, double size, double weight, bool italic) {
    double a = 0, d = 0;
    fontBand(family, size, weight, italic, a, d);
    return std::max(size + 4.0, a + d);
}

double Painter::baselineOffset(const char* family, double size, double weight, bool italic) {
    double a = 0, d = 0;
    fontBand(family, size, weight, italic, a, d);
    double th = std::max(size + 4.0, a + d);
    return (th - (a + d)) / 2.0 + a;
}

double Painter::centeredBaselineY(double containerH, const char* family, double size,
                                  double weight, bool italic) {
    double ascent = 0, descent = 0;
    fontBand(family, size, weight, italic, ascent, descent);
    double textHeight = ascent + descent;
    double topPadding = (containerH - textHeight) / 2.0;
    double desiredBaseline = topPadding + ascent;
    return desiredBaseline - size;
}

// ── Text rendering (showText / textImage) ────────────────────────────────────

void Painter::showText(const char* text) {
    if (!text || !*text) return;
    
    // Debug: check canvas state
    if (!canvas_) {
        fprintf(stderr, "[Painter::showText] ERROR: canvas is null!\n");
        return;
    }
    
    double pad = 0.0;
    const SkMatrix& m = canvas_->getTotalMatrix();
    bool aligned = !m.hasPerspective() && m.getSkewX() == 0.0f && m.getSkewY() == 0.0f;
    const double sx = aligned ? static_cast<double>(m.getScaleX()) : 1.0;
    const double sy = aligned ? static_cast<double>(m.getScaleY()) : 1.0;
    aligned = aligned && sx > 0.25 && sx < 3.0 && sy > 0.25 && sy < 3.0;
    const double qx = aligned ? quantisedScale(sx) : 1.0;
    const double qy = aligned ? quantisedScale(sy) : 1.0;

    const sk_sp<SkImage>& img = textImage(text, qx, qy, &pad);
    if (img) {
        SkPaint blit;
        blit.setAlpha(SkColorGetA(paint_.getColor()));
        const auto yoff = static_cast<SkScalar>(fontSize_ - bakedBaseline(fontSize_));
        if (aligned) {
            const SkPoint dp = m.mapPoint(SkPoint{static_cast<SkScalar>(-pad), yoff});
            canvas_->save();
            canvas_->resetMatrix();
            canvas_->drawImage(img.get(), static_cast<SkScalar>(std::lround(dp.x())),
                               static_cast<SkScalar>(std::lround(dp.y())), SkSamplingOptions(), &blit);
            canvas_->restore();
        } else {
            canvas_->drawImage(img.get(), static_cast<SkScalar>(-pad), yoff, SkSamplingOptions(SkFilterMode::kLinear),
                               &blit);
        }
    }
}

const sk_sp<SkImage>& Painter::textImage(const char* text, double qx, double qy, double* padOut) const {
    static const sk_sp<SkImage> kNone;
    static std::list<std::string> order;
    struct Entry {
        sk_sp<SkImage> img;
        std::list<std::string>::iterator it;
    };
    static std::unordered_map<std::string, Entry> cache;
    constexpr size_t kLimit = 512;

    // Early exit for null or empty text
    if (!text || !*text) {
        static const std::string kEmptyKey = "\x1f\x1f\x1f\x1f\x1f\x1f";
        if (auto it = cache.find(kEmptyKey); it != cache.end()) {
            order.splice(order.end(), order, it->second.it);
            return it->second.img;
        }
        order.push_back(kEmptyKey);
        auto ins = cache.emplace(kEmptyKey, Entry{kNone, std::prev(order.end())}).first;
        return ins->second.img;
    }

    std::string key;
    key.reserve(64);
    key += fontFamily_ ? fontFamily_ : "";
    key += '\x1f';
    key += std::to_string(static_cast<long>(std::lround(fontSize_ * 4.0)));
    key += '\x1f';
    key += styleKey();
    key += '\x1f';
    key += std::to_string(static_cast<long>(std::lround(qx * 50.0)));
    key += ',';
    key += std::to_string(static_cast<long>(std::lround(qy * 50.0)));
    key += '\x1f';
    key += std::to_string(static_cast<unsigned>(paint_.getColor()) & 0x00FFFFFFu);
    key += '\x1f';
    key += text;
    if (auto it = cache.find(key); it != cache.end()) {
        order.splice(order.end(), order, it->second.it);
        return it->second.img;
    }
    if (cache.size() >= kLimit) {
        for (size_t i = 0; i < kLimit / 4 && !order.empty(); ++i) {
            cache.erase(order.front());
            order.pop_front();
        }
    }

    SkFont font = makeFont();
    if (!font.getTypeface()) {
        order.push_back(key);
        auto ins = cache.emplace(std::move(key), Entry{kNone, std::prev(order.end())}).first;
        return ins->second.img;
    }
    size_t textLen = strlen(text);
    if (textLen > 100000) {  // sanity check: reject absurdly long strings
        order.push_back(key);
        auto ins = cache.emplace(std::move(key), Entry{kNone, std::prev(order.end())}).first;
        return ins->second.img;
    }
    
    // UTF-8 validation and sanitization: replace invalid sequences with '?'
    // This prevents Skia crashes when measuring text with malformed UTF-8
    std::string sanitizedText;
    bool needsSanitization = false;
    
    for (size_t i = 0; i < textLen; ++i) {
        uint8_t c = static_cast<uint8_t>(text[i]);
        
        if (c < 0x80) {
            // ASCII byte (0x00-0x7F) - always valid
            if (needsSanitization) sanitizedText += static_cast<char>(c);
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8 sequence (0xC0-0xDF)
            if (i + 1 >= textLen) {
                // Incomplete sequence - replace with '?'
                if (!needsSanitization) {
                    sanitizedText = std::string(text, i);
                    needsSanitization = true;
                }
                sanitizedText += '?';
            } else {
                uint8_t next = static_cast<uint8_t>(text[i + 1]);
                if ((next & 0xC0) != 0x80) {
                    // Invalid continuation byte - replace with '?'
                    if (!needsSanitization) {
                        sanitizedText = std::string(text, i);
                        needsSanitization = true;
                    }
                    sanitizedText += '?';
                } else {
                    // Valid 2-byte sequence
                    if (needsSanitization) {
                        sanitizedText += static_cast<char>(c);
                        sanitizedText += static_cast<char>(next);
                    }
                    ++i; // Skip next byte
                }
            }
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8 sequence (0xE0-0xEF)
            if (i + 2 >= textLen) {
                // Incomplete sequence - replace with '?'
                if (!needsSanitization) {
                    sanitizedText = std::string(text, i);
                    needsSanitization = true;
                }
                sanitizedText += '?';
                i = textLen; // Skip to end
            } else {
                uint8_t next1 = static_cast<uint8_t>(text[i + 1]);
                uint8_t next2 = static_cast<uint8_t>(text[i + 2]);
                if ((next1 & 0xC0) != 0x80 || (next2 & 0xC0) != 0x80) {
                    // Invalid continuation bytes - replace with '?'
                    if (!needsSanitization) {
                        sanitizedText = std::string(text, i);
                        needsSanitization = true;
                    }
                    sanitizedText += '?';
                    i += 2; // Skip next 2 bytes
                } else {
                    // Valid 3-byte sequence
                    if (needsSanitization) {
                        sanitizedText += static_cast<char>(c);
                        sanitizedText += static_cast<char>(next1);
                        sanitizedText += static_cast<char>(next2);
                    }
                    i += 2; // Skip next 2 bytes
                }
            }
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8 sequence (0xF0-0xF7)
            if (i + 3 >= textLen) {
                // Incomplete sequence - replace with '?'
                if (!needsSanitization) {
                    sanitizedText = std::string(text, i);
                    needsSanitization = true;
                }
                sanitizedText += '?';
                i = textLen; // Skip to end
            } else {
                uint8_t next1 = static_cast<uint8_t>(text[i + 1]);
                uint8_t next2 = static_cast<uint8_t>(text[i + 2]);
                uint8_t next3 = static_cast<uint8_t>(text[i + 3]);
                if ((next1 & 0xC0) != 0x80 || (next2 & 0xC0) != 0x80 || (next3 & 0xC0) != 0x80) {
                    // Invalid continuation bytes - replace with '?'
                    if (!needsSanitization) {
                        sanitizedText = std::string(text, i);
                        needsSanitization = true;
                    }
                    sanitizedText += '?';
                    i += 3; // Skip next 3 bytes
                } else {
                    // Valid 4-byte sequence
                    if (needsSanitization) {
                        sanitizedText += static_cast<char>(c);
                        sanitizedText += static_cast<char>(next1);
                        sanitizedText += static_cast<char>(next2);
                        sanitizedText += static_cast<char>(next3);
                    }
                    i += 3; // Skip next 3 bytes
                }
            }
        } else {
            // Invalid UTF-8 start byte (0x80-0xBF or 0xF8-0xFF) - replace with '?'
            if (!needsSanitization) {
                sanitizedText = std::string(text, i);
                needsSanitization = true;
            }
            sanitizedText += '?';
        }
    }
    
    // Use sanitized text if needed
    const char* textToMeasure = needsSanitization ? sanitizedText.c_str() : text;
    size_t measureLen = needsSanitization ? sanitizedText.length() : textLen;
    
    const double w = font.measureText(textToMeasure, measureLen, SkTextEncoding::kUTF8);
    
    const double baseAscent = bakedBaseline(fontSize_);
    const double pad = italicPad(font, baseAscent);
    if (padOut)
        *padOut = pad;
    int iw = static_cast<int>(std::ceil(((w > 0 ? w : 1.0) + pad) * qx)) + 4;
    if (iw < 1)
        iw = 1;
    int ih = static_cast<int>(std::ceil((baseAscent + fontSize_ * 0.6) * qy)) + 4;
    if (ih < 1)
        ih = 1;
    
    // Debug log for crash diagnosis
    if (iw <= 0 || ih <= 0 || iw > 10000 || ih > 10000) {
        fprintf(stderr, "[Painter::textImage] WARNING: suspicious dimensions: %dx%d for text len=%zu, w=%.2f, qx=%.2f, qy=%.2f\n",
                iw, ih, textLen, w, qx, qy);
    }
    
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(iw, ih));
    
    if (surface) {
        SkCanvas* bc = surface->getCanvas();
        if (qx != 1.0 || qy != 1.0)
            bc->scale(static_cast<SkScalar>(qx), static_cast<SkScalar>(qy));
        SkPaint tp;
        tp.setColor(SkColorSetA(paint_.getColor(), 0xFF));
        tp.setAntiAlias(true);
        
        bc->drawString(textToMeasure, static_cast<SkScalar>(pad), static_cast<SkScalar>(baseAscent), font, tp);
    }
    sk_sp<SkImage> img = surface ? surface->makeImageSnapshot() : nullptr;
    order.push_back(key);
    auto ins = cache.emplace(std::move(key), Entry{std::move(img), std::prev(order.end())}).first;
    return ins->second.img;
}

}  // namespace cui_core
