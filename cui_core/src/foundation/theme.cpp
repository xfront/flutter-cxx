/// @file theme.cpp — Built-in theme presets.
#include "cui/foundation/theme.h"

#include <algorithm>
#include <cctype>

namespace cui {

// ── Global theme singleton ──────────────────────────────────────────────────

Theme& theme() {
    static Theme current = Theme::light();
    return current;
}

// ── Light (Material 3 default) ──────────────────────────────────────────────

Theme Theme::light() {
    Theme t;
    t.primary       = Color::fromRGB(0.13, 0.59, 0.95);   // #2196F5
    t.primaryDark   = Color::fromRGB(0.10, 0.45, 0.75);
    t.primaryLight  = Color::fromRGB(0.73, 0.87, 1.00);

    t.success       = Color::fromRGB(0.30, 0.69, 0.31);
    t.warning       = Color::fromRGB(1.00, 0.60, 0.00);
    t.error         = Color::fromRGB(0.96, 0.26, 0.21);

    t.surface       = Color::fromRGB(1.00, 1.00, 1.00);
    t.background    = Color::fromRGB(0.96, 0.97, 0.98);
    t.surfaceVariant = Color::fromRGB(0.94, 0.95, 0.97);

    t.textPrimary   = Color::fromRGB(0.13, 0.13, 0.15);
    t.textSecondary = Color::fromRGB(0.45, 0.45, 0.50);
    t.textDisabled  = Color::fromRGB(0.65, 0.65, 0.68);
    t.textOnPrimary = Color::fromRGB(1.00, 1.00, 1.00);

    t.border        = Color::fromRGB(0.82, 0.84, 0.87);
    t.borderLight   = Color::fromRGB(0.88, 0.90, 0.92);
    t.divider       = Color::fromRGB(0.90, 0.91, 0.93);

    t.focusRing     = t.primary;
    t.ripple        = Color::fromRGBA(0.13, 0.59, 0.95, 0.15);  // primary with alpha
    t.hoverOverlay  = Color::fromRGBA(0.0, 0.0, 0.0, 0.04);
    t.pressedOverlay = Color::fromRGBA(0.0, 0.0, 0.0, 0.08);

    // Extended color scheme
    t.primaryContainer  = Color::fromRGB(0.73, 0.87, 1.00);
    t.onPrimaryContainer = Color::fromRGB(0.05, 0.20, 0.40);
    t.secondary         = Color::fromRGB(0.60, 0.50, 0.80);   // Purple accent
    t.secondaryContainer = Color::fromRGB(0.88, 0.84, 0.96);
    t.tertiary          = Color::fromRGB(0.30, 0.69, 0.60);   // Teal accent
    t.tertiaryContainer = Color::fromRGB(0.78, 0.94, 0.90);
    t.outline           = Color::fromRGB(0.70, 0.72, 0.75);
    t.outlineVariant    = Color::fromRGB(0.85, 0.87, 0.89);
    t.scrim             = Color::fromRGBA(0.0, 0.0, 0.0, 0.4);

    t.titleBarStart = Color::fromRGB(0.10, 0.46, 0.82);   // Blue 700
    t.titleBarEnd   = Color::fromRGB(0.08, 0.40, 0.75);   // Blue 800

    t.ribbonStart   = Color::fromRGB(0.973, 0.976, 0.988);
    t.ribbonEnd     = Color::fromRGB(0.953, 0.961, 0.980);

    // 参考截图取的滚动条 thumb（#858188）；轨道沿用 outlineVariant。
    t.scrollbarThumb = Color::fromRGB(133.0 / 255.0, 129.0 / 255.0, 136.0 / 255.0);
    return t;
}

// ── Dark (Material 3 Dark) ──────────────────────────────────────────────────

Theme Theme::dark() {
    Theme t;
    t.primary       = Color::fromRGB(0.54, 0.76, 0.98);   // #8AC2FA
    t.primaryDark   = Color::fromRGB(0.40, 0.60, 0.80);
    t.primaryLight  = Color::fromRGB(0.20, 0.30, 0.42);

    t.success       = Color::fromRGB(0.50, 0.80, 0.50);
    t.warning       = Color::fromRGB(1.00, 0.73, 0.25);
    t.error         = Color::fromRGB(0.96, 0.44, 0.40);

    t.surface       = Color::fromRGB(0.16, 0.16, 0.18);   // #29292E
    t.background    = Color::fromRGB(0.12, 0.12, 0.14);   // #1E1E24
    t.surfaceVariant = Color::fromRGB(0.20, 0.20, 0.23);

    t.textPrimary   = Color::fromRGB(0.92, 0.92, 0.93);
    t.textSecondary = Color::fromRGB(0.65, 0.65, 0.68);
    t.textDisabled  = Color::fromRGB(0.42, 0.42, 0.45);
    t.textOnPrimary = Color::fromRGB(0.10, 0.10, 0.12);

    t.border        = Color::fromRGB(0.30, 0.30, 0.33);
    t.borderLight   = Color::fromRGB(0.25, 0.25, 0.28);
    t.divider       = Color::fromRGB(0.22, 0.22, 0.25);

    t.focusRing     = t.primary;
    t.ripple        = Color::fromRGBA(0.54, 0.76, 0.98, 0.15);
    t.hoverOverlay  = Color::fromRGBA(1.0, 1.0, 1.0, 0.06);
    t.pressedOverlay = Color::fromRGBA(1.0, 1.0, 1.0, 0.10);

    // Extended color scheme
    t.primaryContainer  = Color::fromRGB(0.20, 0.30, 0.42);
    t.onPrimaryContainer = Color::fromRGB(0.80, 0.90, 1.00);
    t.secondary         = Color::fromRGB(0.70, 0.60, 0.90);
    t.secondaryContainer = Color::fromRGB(0.30, 0.25, 0.45);
    t.tertiary          = Color::fromRGB(0.50, 0.80, 0.75);
    t.tertiaryContainer = Color::fromRGB(0.20, 0.40, 0.38);
    t.outline           = Color::fromRGB(0.40, 0.40, 0.44);
    t.outlineVariant    = Color::fromRGB(0.30, 0.30, 0.34);
    t.scrim             = Color::fromRGBA(0.0, 0.0, 0.0, 0.6);

    t.titleBarStart = Color::fromRGB(0.14, 0.14, 0.16);
    t.titleBarEnd   = Color::fromRGB(0.11, 0.11, 0.13);

    t.ribbonStart   = Color::fromRGB(0.15, 0.15, 0.17);
    t.ribbonEnd     = Color::fromRGB(0.13, 0.13, 0.15);

    t.scrollbarThumb = Color::fromRGB(0.40, 0.40, 0.44);
    return t;
}

// ── Ocean (cool blue-teal) ──────────────────────────────────────────────────

Theme Theme::ocean() {
    Theme t;
    t.primary       = Color::fromRGB(0.00, 0.60, 0.65);   // Teal 600
    t.primaryDark   = Color::fromRGB(0.00, 0.47, 0.52);
    t.primaryLight  = Color::fromRGB(0.70, 0.90, 0.90);

    t.success       = Color::fromRGB(0.30, 0.69, 0.31);
    t.warning       = Color::fromRGB(1.00, 0.60, 0.00);
    t.error         = Color::fromRGB(0.96, 0.26, 0.21);

    t.surface       = Color::fromRGB(0.97, 0.99, 1.00);
    t.background    = Color::fromRGB(0.92, 0.96, 0.98);
    t.surfaceVariant = Color::fromRGB(0.90, 0.94, 0.96);

    t.textPrimary   = Color::fromRGB(0.10, 0.15, 0.20);
    t.textSecondary = Color::fromRGB(0.40, 0.48, 0.52);
    t.textDisabled  = Color::fromRGB(0.62, 0.68, 0.70);
    t.textOnPrimary = Color::fromRGB(1.00, 1.00, 1.00);

    t.border        = Color::fromRGB(0.78, 0.86, 0.88);
    t.borderLight   = Color::fromRGB(0.84, 0.90, 0.92);
    t.divider       = Color::fromRGB(0.88, 0.93, 0.95);

    t.focusRing     = t.primary;
    t.ripple        = Color::fromRGBA(0.00, 0.60, 0.65, 0.15);
    t.hoverOverlay  = Color::fromRGBA(0.0, 0.0, 0.0, 0.04);
    t.pressedOverlay = Color::fromRGBA(0.0, 0.0, 0.0, 0.08);

    // Extended color scheme
    t.primaryContainer  = Color::fromRGB(0.70, 0.90, 0.90);
    t.onPrimaryContainer = Color::fromRGB(0.00, 0.25, 0.28);
    t.secondary         = Color::fromRGB(0.40, 0.60, 0.80);
    t.secondaryContainer = Color::fromRGB(0.82, 0.90, 0.96);
    t.tertiary          = Color::fromRGB(0.50, 0.70, 0.40);
    t.tertiaryContainer = Color::fromRGB(0.84, 0.92, 0.80);
    t.outline           = Color::fromRGB(0.68, 0.78, 0.80);
    t.outlineVariant    = Color::fromRGB(0.80, 0.88, 0.90);
    t.scrim             = Color::fromRGBA(0.0, 0.0, 0.0, 0.4);

    t.titleBarStart = Color::fromRGB(0.00, 0.52, 0.58);
    t.titleBarEnd   = Color::fromRGB(0.00, 0.42, 0.48);

    t.ribbonStart   = Color::fromRGB(0.93, 0.97, 0.98);
    t.ribbonEnd     = Color::fromRGB(0.90, 0.95, 0.96);

    t.scrollbarThumb = Color::fromRGB(0.60, 0.74, 0.76);
    return t;
}

// ── Forest (warm green-brown) ───────────────────────────────────────────────

Theme Theme::forest() {
    Theme t;
    t.primary       = Color::fromRGB(0.30, 0.55, 0.25);   // Forest green
    t.primaryDark   = Color::fromRGB(0.22, 0.42, 0.18);
    t.primaryLight  = Color::fromRGB(0.78, 0.88, 0.74);

    t.success       = Color::fromRGB(0.35, 0.65, 0.30);
    t.warning       = Color::fromRGB(0.85, 0.55, 0.10);
    t.error         = Color::fromRGB(0.80, 0.25, 0.20);

    t.surface       = Color::fromRGB(0.98, 0.97, 0.95);
    t.background    = Color::fromRGB(0.94, 0.93, 0.90);
    t.surfaceVariant = Color::fromRGB(0.91, 0.90, 0.87);

    t.textPrimary   = Color::fromRGB(0.15, 0.13, 0.10);
    t.textSecondary = Color::fromRGB(0.45, 0.42, 0.35);
    t.textDisabled  = Color::fromRGB(0.65, 0.62, 0.56);
    t.textOnPrimary = Color::fromRGB(1.00, 1.00, 0.98);

    t.border        = Color::fromRGB(0.80, 0.78, 0.72);
    t.borderLight   = Color::fromRGB(0.86, 0.84, 0.80);
    t.divider       = Color::fromRGB(0.89, 0.87, 0.83);

    t.focusRing     = t.primary;
    t.ripple        = Color::fromRGBA(0.30, 0.55, 0.25, 0.15);
    t.hoverOverlay  = Color::fromRGBA(0.0, 0.0, 0.0, 0.04);
    t.pressedOverlay = Color::fromRGBA(0.0, 0.0, 0.0, 0.08);

    // Extended color scheme
    t.primaryContainer  = Color::fromRGB(0.78, 0.88, 0.74);
    t.onPrimaryContainer = Color::fromRGB(0.12, 0.25, 0.10);
    t.secondary         = Color::fromRGB(0.70, 0.55, 0.35);
    t.secondaryContainer = Color::fromRGB(0.92, 0.86, 0.76);
    t.tertiary          = Color::fromRGB(0.55, 0.65, 0.40);
    t.tertiaryContainer = Color::fromRGB(0.86, 0.90, 0.78);
    t.outline           = Color::fromRGB(0.70, 0.68, 0.62);
    t.outlineVariant    = Color::fromRGB(0.82, 0.80, 0.74);
    t.scrim             = Color::fromRGBA(0.0, 0.0, 0.0, 0.4);

    t.titleBarStart = Color::fromRGB(0.25, 0.45, 0.20);
    t.titleBarEnd   = Color::fromRGB(0.20, 0.38, 0.15);

    t.ribbonStart   = Color::fromRGB(0.95, 0.94, 0.91);
    t.ribbonEnd     = Color::fromRGB(0.92, 0.91, 0.87);

    t.scrollbarThumb = Color::fromRGB(0.68, 0.66, 0.58);
    return t;
}

// ── Sky Blue (bright sky blue) ──────────────────────────────────────────────

Theme Theme::skyBlue() {
    Theme t;
    t.primary       = Color::fromRGB(0.20, 0.65, 0.95);   // Sky blue
    t.primaryDark   = Color::fromRGB(0.15, 0.50, 0.78);
    t.primaryLight  = Color::fromRGB(0.75, 0.90, 1.00);

    t.success       = Color::fromRGB(0.30, 0.69, 0.31);
    t.warning       = Color::fromRGB(1.00, 0.60, 0.00);
    t.error         = Color::fromRGB(0.96, 0.26, 0.21);

    t.surface       = Color::fromRGB(0.99, 0.99, 1.00);
    t.background    = Color::fromRGB(0.94, 0.97, 1.00);
    t.surfaceVariant = Color::fromRGB(0.91, 0.95, 0.99);

    t.textPrimary   = Color::fromRGB(0.10, 0.15, 0.25);
    t.textSecondary = Color::fromRGB(0.42, 0.50, 0.60);
    t.textDisabled  = Color::fromRGB(0.64, 0.70, 0.76);
    t.textOnPrimary = Color::fromRGB(1.00, 1.00, 1.00);

    t.border        = Color::fromRGB(0.78, 0.86, 0.94);
    t.borderLight   = Color::fromRGB(0.84, 0.90, 0.96);
    t.divider       = Color::fromRGB(0.88, 0.93, 0.98);

    t.focusRing     = t.primary;
    t.ripple        = Color::fromRGBA(0.20, 0.65, 0.95, 0.15);
    t.hoverOverlay  = Color::fromRGBA(0.0, 0.0, 0.0, 0.04);
    t.pressedOverlay = Color::fromRGBA(0.0, 0.0, 0.0, 0.08);

    t.primaryContainer  = Color::fromRGB(0.75, 0.90, 1.00);
    t.onPrimaryContainer = Color::fromRGB(0.05, 0.25, 0.45);
    t.secondary         = Color::fromRGB(0.50, 0.70, 0.90);
    t.secondaryContainer = Color::fromRGB(0.84, 0.92, 1.00);
    t.tertiary          = Color::fromRGB(0.40, 0.75, 0.70);
    t.tertiaryContainer = Color::fromRGB(0.80, 0.94, 0.92);
    t.outline           = Color::fromRGB(0.68, 0.78, 0.88);
    t.outlineVariant    = Color::fromRGB(0.80, 0.88, 0.94);
    t.scrim             = Color::fromRGBA(0.0, 0.0, 0.0, 0.4);

    t.titleBarStart = Color::fromRGB(0.15, 0.55, 0.85);
    t.titleBarEnd   = Color::fromRGB(0.10, 0.45, 0.75);

    t.ribbonStart   = Color::fromRGB(0.95, 0.97, 1.00);
    t.ribbonEnd     = Color::fromRGB(0.92, 0.95, 0.99);

    t.scrollbarThumb = Color::fromRGB(0.65, 0.78, 0.90);
    return t;
}

// ── Orange (warm orange) ────────────────────────────────────────────────────

Theme Theme::orange() {
    Theme t;
    t.primary       = Color::fromRGB(0.95, 0.50, 0.15);   // Orange 600
    t.primaryDark   = Color::fromRGB(0.78, 0.40, 0.10);
    t.primaryLight  = Color::fromRGB(1.00, 0.82, 0.65);

    t.success       = Color::fromRGB(0.30, 0.69, 0.31);
    t.warning       = Color::fromRGB(1.00, 0.60, 0.00);
    t.error         = Color::fromRGB(0.96, 0.26, 0.21);

    t.surface       = Color::fromRGB(1.00, 0.99, 0.98);
    t.background    = Color::fromRGB(1.00, 0.97, 0.94);
    t.surfaceVariant = Color::fromRGB(0.99, 0.95, 0.90);

    t.textPrimary   = Color::fromRGB(0.20, 0.13, 0.08);
    t.textSecondary = Color::fromRGB(0.50, 0.42, 0.35);
    t.textDisabled  = Color::fromRGB(0.70, 0.64, 0.58);
    t.textOnPrimary = Color::fromRGB(1.00, 1.00, 1.00);

    t.border        = Color::fromRGB(0.90, 0.84, 0.78);
    t.borderLight   = Color::fromRGB(0.94, 0.90, 0.86);
    t.divider       = Color::fromRGB(0.96, 0.93, 0.90);

    t.focusRing     = t.primary;
    t.ripple        = Color::fromRGBA(0.95, 0.50, 0.15, 0.15);
    t.hoverOverlay  = Color::fromRGBA(0.0, 0.0, 0.0, 0.04);
    t.pressedOverlay = Color::fromRGBA(0.0, 0.0, 0.0, 0.08);

    t.primaryContainer  = Color::fromRGB(1.00, 0.82, 0.65);
    t.onPrimaryContainer = Color::fromRGB(0.40, 0.20, 0.05);
    t.secondary         = Color::fromRGB(0.80, 0.60, 0.30);
    t.secondaryContainer = Color::fromRGB(0.96, 0.90, 0.80);
    t.tertiary          = Color::fromRGB(0.70, 0.55, 0.35);
    t.tertiaryContainer = Color::fromRGB(0.94, 0.88, 0.78);
    t.outline           = Color::fromRGB(0.80, 0.74, 0.68);
    t.outlineVariant    = Color::fromRGB(0.88, 0.84, 0.80);
    t.scrim             = Color::fromRGBA(0.0, 0.0, 0.0, 0.4);

    t.titleBarStart = Color::fromRGB(0.85, 0.42, 0.10);
    t.titleBarEnd   = Color::fromRGB(0.72, 0.35, 0.08);

    t.ribbonStart   = Color::fromRGB(1.00, 0.98, 0.95);
    t.ribbonEnd     = Color::fromRGB(0.99, 0.96, 0.91);

    t.scrollbarThumb = Color::fromRGB(0.82, 0.72, 0.62);
    return t;
}

// ── Lavender (soft purple) ──────────────────────────────────────────────────

Theme Theme::lavender() {
    Theme t;
    t.primary       = Color::fromRGB(0.55, 0.40, 0.80);   // Lavender purple
    t.primaryDark   = Color::fromRGB(0.42, 0.30, 0.65);
    t.primaryLight  = Color::fromRGB(0.84, 0.78, 0.94);

    t.success       = Color::fromRGB(0.30, 0.69, 0.31);
    t.warning       = Color::fromRGB(1.00, 0.60, 0.00);
    t.error         = Color::fromRGB(0.96, 0.26, 0.21);

    t.surface       = Color::fromRGB(0.99, 0.98, 1.00);
    t.background    = Color::fromRGB(0.96, 0.95, 0.99);
    t.surfaceVariant = Color::fromRGB(0.94, 0.92, 0.98);

    t.textPrimary   = Color::fromRGB(0.15, 0.12, 0.20);
    t.textSecondary = Color::fromRGB(0.45, 0.40, 0.55);
    t.textDisabled  = Color::fromRGB(0.66, 0.62, 0.74);
    t.textOnPrimary = Color::fromRGB(1.00, 1.00, 1.00);

    t.border        = Color::fromRGB(0.84, 0.80, 0.90);
    t.borderLight   = Color::fromRGB(0.88, 0.86, 0.94);
    t.divider       = Color::fromRGB(0.92, 0.90, 0.96);

    t.focusRing     = t.primary;
    t.ripple        = Color::fromRGBA(0.55, 0.40, 0.80, 0.15);
    t.hoverOverlay  = Color::fromRGBA(0.0, 0.0, 0.0, 0.04);
    t.pressedOverlay = Color::fromRGBA(0.0, 0.0, 0.0, 0.08);

    t.primaryContainer  = Color::fromRGB(0.84, 0.78, 0.94);
    t.onPrimaryContainer = Color::fromRGB(0.22, 0.15, 0.40);
    t.secondary         = Color::fromRGB(0.70, 0.50, 0.75);
    t.secondaryContainer = Color::fromRGB(0.92, 0.86, 0.94);
    t.tertiary          = Color::fromRGB(0.55, 0.60, 0.70);
    t.tertiaryContainer = Color::fromRGB(0.84, 0.88, 0.94);
    t.outline           = Color::fromRGB(0.74, 0.70, 0.82);
    t.outlineVariant    = Color::fromRGB(0.84, 0.82, 0.90);
    t.scrim             = Color::fromRGBA(0.0, 0.0, 0.0, 0.4);

    t.titleBarStart = Color::fromRGB(0.48, 0.35, 0.72);
    t.titleBarEnd   = Color::fromRGB(0.40, 0.28, 0.62);

    t.ribbonStart   = Color::fromRGB(0.97, 0.96, 0.99);
    t.ribbonEnd     = Color::fromRGB(0.95, 0.94, 0.98);

    t.scrollbarThumb = Color::fromRGB(0.74, 0.68, 0.82);
    return t;
}

// ── IDE (VS 2026-style: dark theme, blue chrome) ───────────────────────────

Theme Theme::ide() {
    Theme t;
    // Primary: VS blue accent
    t.primary       = Color::fromRGB(0.00, 0.48, 0.80);   // #007ACC VS blue
    t.primaryDark   = Color::fromRGB(0.00, 0.36, 0.60);
    t.primaryLight  = Color::fromRGB(0.20, 0.55, 0.85);

    t.success       = Color::fromRGB(0.30, 0.80, 0.35);
    t.warning       = Color::fromRGB(1.00, 0.70, 0.10);
    t.error         = Color::fromRGB(0.95, 0.35, 0.30);

    // Dark surfaces — VS 2026 dark theme
    t.surface       = Color::fromRGB(0.18, 0.18, 0.20);   // #2E2E32 panel bg
    t.background    = Color::fromRGB(0.12, 0.12, 0.14);   // #1E1E24 editor bg
    t.surfaceVariant = Color::fromRGB(0.22, 0.22, 0.25);  // #383840 tab bar

    t.textPrimary   = Color::fromRGB(0.90, 0.90, 0.92);   // Light text on dark
    t.textSecondary = Color::fromRGB(0.60, 0.62, 0.66);
    t.textDisabled  = Color::fromRGB(0.40, 0.40, 0.44);
    t.textOnPrimary = Color::fromRGB(1.00, 1.00, 1.00);

    t.border        = Color::fromRGB(0.28, 0.28, 0.32);
    t.borderLight   = Color::fromRGB(0.22, 0.22, 0.26);
    t.divider       = Color::fromRGB(0.20, 0.20, 0.23);

    t.focusRing     = t.primary;
    t.ripple        = Color::fromRGBA(0.00, 0.48, 0.80, 0.20);
    t.hoverOverlay  = Color::fromRGBA(1.0, 1.0, 1.0, 0.06);
    t.pressedOverlay = Color::fromRGBA(1.0, 1.0, 1.0, 0.10);

    t.primaryContainer  = Color::fromRGB(0.10, 0.20, 0.35);
    t.onPrimaryContainer = Color::fromRGB(0.70, 0.85, 1.00);
    t.secondary         = Color::fromRGB(0.55, 0.45, 0.80);
    t.secondaryContainer = Color::fromRGB(0.22, 0.18, 0.35);
    t.tertiary          = Color::fromRGB(0.30, 0.70, 0.60);
    t.tertiaryContainer = Color::fromRGB(0.12, 0.30, 0.26);
    t.outline           = Color::fromRGB(0.35, 0.35, 0.40);
    t.outlineVariant    = Color::fromRGB(0.25, 0.25, 0.30);
    t.scrim             = Color::fromRGBA(0.0, 0.0, 0.0, 0.6);

    // Panel title bars: dark blue-gray gradient (VS-style)
    t.titleBarStart = Color::fromRGB(0.20, 0.20, 0.24);   // #33333D
    t.titleBarEnd   = Color::fromRGB(0.16, 0.16, 0.19);   // #292930

    t.ribbonStart   = Color::fromRGB(0.18, 0.18, 0.20);
    t.ribbonEnd     = Color::fromRGB(0.15, 0.15, 0.17);

    t.scrollbarThumb = Color::fromRGB(0.40, 0.40, 0.44);
    return t;
}

// ── Name lookup ────────────────────────────────────────────────────────────

Theme Theme::fromName(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower == "dark")      return dark();
    if (lower == "ocean")     return ocean();
    if (lower == "forest")    return forest();
    if (lower == "skyblue")   return skyBlue();
    if (lower == "orange")    return orange();
    if (lower == "lavender")  return lavender();
    if (lower == "ide")       return ide();
    return light();
}

}  // namespace cui_core
