#pragma once
/// @file color.h
/// @brief Extended Color utilities — HSL/HSV/Hex construction, linear-light
///        blending, and manipulation helpers (lighten, darken, saturate, …).
///
/// The base Color struct lives in geometry.h (r/g/b/a in sRGB 0–1).  This
/// header adds free functions and static factories that operate on Color
/// without changing the struct layout — existing code keeps compiling.
///
/// Usage:
///   auto red   = Color::fromHex("#FF0000");
///   auto blue  = Color::fromHSL(240, 1.0, 0.5);
///   auto mixed = Color::lerpLinear(red, blue, 0.5);  // physically correct
///   auto dark  = red.darken(0.2);

#include "cui/foundation/geometry.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace cui {

// ── sRGB ↔ Linear-light conversion (IEC 61966-2-1) ────────────────────────

/// Decode an sRGB component to linear light (gamma ≈ 2.2 piecewise).
inline double srgbToLinear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/// Encode a linear-light component back to sRGB.
inline double linearToSrgb(double c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

// ── Color extension methods (free functions operating on Color) ─────────────

/// HSL representation.  h ∈ [0, 360), s/l ∈ [0, 1].
struct HSL {
    double h = 0;
    double s = 0;
    double l = 0;
};

/// HSV representation.  h ∈ [0, 360), s/v ∈ [0, 1].
struct HSV {
    double h = 0;
    double s = 0;
    double v = 0;
};

// ── RGB → HSL ──────────────────────────────────────────────────────────────

HSL rgbToHSL(double r, double g, double b);

// ── HSL → RGB ──────────────────────────────────────────────────────────────

void hslToRGB(double h, double s, double l, double& r, double& g, double& b);

// ── RGB → HSV ──────────────────────────────────────────────────────────────

HSV rgbToHSV(double r, double g, double b);

// ── HSV → RGB ──────────────────────────────────────────────────────────────

void hsvToRGB(double h, double s, double v, double& r, double& g, double& b);

// ── Hex parsing ────────────────────────────────────────────────────────────

Color colorFromHex(const char* hex);

std::string colorToHex(const Color& c);

// ── Color member-like free functions (extend Color without modifying it) ────

inline Color colorFromHSL(double h, double s, double l, double a = 1.0) {
    double r, g, b;
    hslToRGB(h, s, l, r, g, b);
    return Color::fromRGBA(r, g, b, a);
}

inline Color colorFromHSV(double h, double s, double v, double a = 1.0) {
    double r, g, b;
    hsvToRGB(h, s, v, r, g, b);
    return Color::fromRGBA(r, g, b, a);
}

inline HSL colorToHSL(const Color& c) { return rgbToHSL(c.r, c.g, c.b); }
inline HSV colorToHSV(const Color& c) { return rgbToHSV(c.r, c.g, c.b); }

// ── Manipulation ───────────────────────────────────────────────────────────

Color colorLighten(const Color& c, double amount);

inline Color colorDarken(const Color& c, double amount) {
    return colorLighten(c, -amount);
}

Color colorSaturate(const Color& c, double amount);

inline Color colorDesaturate(const Color& c, double amount) {
    return colorSaturate(c, -amount);
}

inline Color colorWithOpacity(const Color& c, double alpha) {
    return Color::fromRGBA(c.r, c.g, c.b, std::clamp(alpha, 0.0, 1.0));
}

inline Color colorOpacify(const Color& c, double amount) {
    return Color::fromRGBA(c.r, c.g, c.b, std::clamp(c.a + amount, 0.0, 1.0));
}

// ── Blending ───────────────────────────────────────────────────────────────

/// sRGB interpolation (fast, perceptually non-uniform).
inline Color colorLerp(const Color& a, const Color& b, double t) {
    return Color::fromRGBA(
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t);
}

/// Linear-light interpolation (physically correct for additive mixing).
Color colorLerpLinear(const Color& a, const Color& b, double t);

// ── Named colors (CSS basic set, subset) ───────────────────────────────────

namespace colors {
    inline Color transparent() { return Color::fromRGBA(0, 0, 0, 0); }
    inline Color black()       { return Color::fromRGB(0, 0, 0); }
    inline Color white()       { return Color::fromRGB(1, 1, 1); }
    inline Color red()         { return Color::fromRGB(1, 0, 0); }
    inline Color green()       { return Color::fromRGB(0, 0.502, 0); }
    inline Color blue()        { return Color::fromRGB(0, 0, 1); }
    inline Color yellow()      { return Color::fromRGB(1, 1, 0); }
    inline Color cyan()        { return Color::fromRGB(0, 1, 1); }
    inline Color magenta()     { return Color::fromRGB(1, 0, 1); }
    inline Color orange()      { return Color::fromRGB(1, 0.647, 0); }
    inline Color purple()      { return Color::fromRGB(0.502, 0, 0.502); }
    inline Color pink()        { return Color::fromRGB(1, 0.753, 0.796); }
    inline Color gray()        { return Color::fromRGB(0.502, 0.502, 0.502); }
    inline Color lightGray()   { return Color::fromRGB(0.827, 0.827, 0.827); }
    inline Color darkGray()    { return Color::fromRGB(0.663, 0.663, 0.663); }
}  // namespace colors

}  // namespace cui_core
