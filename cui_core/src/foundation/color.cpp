/// @file color.cpp — Color utility implementations (moved from color.h)
#include "cui/foundation/color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cui {

// ── RGB → HSL ──────────────────────────────────────────────────────────────

HSL rgbToHSL(double r, double g, double b) {
    double mn = std::min({r, g, b});
    double mx = std::max({r, g, b});
    double delta = mx - mn;
    double l = (mx + mn) * 0.5;

    if (delta < 1e-9)
        return {0.0, 0.0, l};

    double s = l > 0.5 ? delta / (2.0 - mx - mn) : delta / (mx + mn);

    double h;
    if (mx == r)
        h = (g - b) / delta + (g < b ? 6.0 : 0.0);
    else if (mx == g)
        h = (b - r) / delta + 2.0;
    else
        h = (r - g) / delta + 4.0;
    h *= 60.0;

    return {h, s, l};
}

// ── HSL → RGB ──────────────────────────────────────────────────────────────

void hslToRGB(double h, double s, double l, double& r, double& g, double& b) {
    if (s < 1e-9) {
        r = g = b = l;
        return;
    }
    double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    double p = 2.0 * l - q;
    h = std::fmod(h, 360.0);
    if (h < 0) h += 360.0;
    h /= 360.0;

    auto hue2rgb = [](double p, double q, double t) -> double {
        if (t < 0.0) t += 1.0;
        if (t > 1.0) t -= 1.0;
        if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
        return p;
    };

    r = hue2rgb(p, q, h + 1.0 / 3.0);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.0 / 3.0);
}

// ── RGB → HSV ──────────────────────────────────────────────────────────────

HSV rgbToHSV(double r, double g, double b) {
    double mn = std::min({r, g, b});
    double mx = std::max({r, g, b});
    double delta = mx - mn;

    double v = mx;
    double s = mx > 1e-9 ? delta / mx : 0.0;

    if (delta < 1e-9)
        return {0.0, s, v};

    double h;
    if (mx == r)
        h = (g - b) / delta + (g < b ? 6.0 : 0.0);
    else if (mx == g)
        h = (b - r) / delta + 2.0;
    else
        h = (r - g) / delta + 4.0;
    h *= 60.0;

    return {h, s, v};
}

// ── HSV → RGB ──────────────────────────────────────────────────────────────

void hsvToRGB(double h, double s, double v, double& r, double& g, double& b) {
    if (s < 1e-9) {
        r = g = b = v;
        return;
    }
    h = std::fmod(h, 360.0);
    if (h < 0) h += 360.0;
    h /= 60.0;
    int i = static_cast<int>(h);
    double f = h - i;
    double p = v * (1.0 - s);
    double q = v * (1.0 - s * f);
    double t = v * (1.0 - s * (1.0 - f));
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

// ── Hex parsing ────────────────────────────────────────────────────────────

Color colorFromHex(const char* hex) {
    if (!hex || *hex == '\0')
        return {};
    if (*hex == '#') ++hex;
    int len = 0;
    for (const char* p = hex; *p; ++p) ++len;

    auto hexDigit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return 0;
    };

    if (len == 3) {
        int ri = hexDigit(hex[0]);
        int gi = hexDigit(hex[1]);
        int bi = hexDigit(hex[2]);
        return Color::fromRGBA((ri * 17) / 255.0, (gi * 17) / 255.0,
                               (bi * 17) / 255.0, 1.0);
    }
    if (len == 4) {
        int ri = hexDigit(hex[0]);
        int gi = hexDigit(hex[1]);
        int bi = hexDigit(hex[2]);
        int ai = hexDigit(hex[3]);
        return Color::fromRGBA((ri * 17) / 255.0, (gi * 17) / 255.0,
                               (bi * 17) / 255.0, (ai * 17) / 255.0);
    }
    if (len >= 6) {
        int ri = hexDigit(hex[0]) * 16 + hexDigit(hex[1]);
        int gi = hexDigit(hex[2]) * 16 + hexDigit(hex[3]);
        int bi = hexDigit(hex[4]) * 16 + hexDigit(hex[5]);
        int ai = len >= 8 ? hexDigit(hex[6]) * 16 + hexDigit(hex[7]) : 255;
        return Color::fromRGBA(ri / 255.0, gi / 255.0, bi / 255.0, ai / 255.0);
    }
    return {};
}

std::string colorToHex(const Color& c) {
    auto toByte = [](double v) -> int {
        return std::clamp(static_cast<int>(std::round(v * 255.0)), 0, 255);
    };
    char buf[10];
    int a = toByte(c.a);
    std::snprintf(buf, sizeof(buf), a == 255 ? "#%02X%02X%02X" : "#%02X%02X%02X%02X",
                  toByte(c.r), toByte(c.g), toByte(c.b), a);
    return buf;
}

// ── Manipulation ───────────────────────────────────────────────────────────

Color colorLighten(const Color& c, double amount) {
    HSL hsl = colorToHSL(c);
    hsl.l = std::clamp(hsl.l + amount, 0.0, 1.0);
    double r, g, b;
    hslToRGB(hsl.h, hsl.s, hsl.l, r, g, b);
    return Color::fromRGBA(r, g, b, c.a);
}

Color colorSaturate(const Color& c, double amount) {
    HSL hsl = colorToHSL(c);
    hsl.s = std::clamp(hsl.s + amount, 0.0, 1.0);
    double r, g, b;
    hslToRGB(hsl.h, hsl.s, hsl.l, r, g, b);
    return Color::fromRGBA(r, g, b, c.a);
}

// ── Blending ───────────────────────────────────────────────────────────────

Color colorLerpLinear(const Color& a, const Color& b, double t) {
    double lr = srgbToLinear(a.r) + (srgbToLinear(b.r) - srgbToLinear(a.r)) * t;
    double lg = srgbToLinear(a.g) + (srgbToLinear(b.g) - srgbToLinear(a.g)) * t;
    double lb = srgbToLinear(a.b) + (srgbToLinear(b.b) - srgbToLinear(a.b)) * t;
    double la = a.a + (b.a - a.a) * t;
    return Color::fromRGBA(linearToSrgb(lr), linearToSrgb(lg), linearToSrgb(lb), la);
}

}  // namespace cui_core
