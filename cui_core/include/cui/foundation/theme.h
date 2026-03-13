#pragma once
/// @file theme.h
/// @brief Multi-theme system with runtime switching and external customization.
///
/// Design goals (inspired by 上游 Material 3 / Qt styling):
///   • All color tokens are PUBLIC fields → users can modify individual fields
///   • theme() returns the active Theme by reference → assign a whole new Theme
///   • Built-in presets: Light, Dark, Ocean, Forest
///   • Metrics (radius, spacing, shadow, font sizes) are shared across themes
///
/// Usage:
///   // Read a token:
///   p.setSourceColor(theme().primary);
///
///   // Switch to a built-in preset:
///   theme() = Theme::dark();
///
///   // Create a fully custom theme:
///   Theme myTheme;
///   myTheme.primary = Color::fromRGB(0.8, 0.2, 0.5);
///   myTheme.surface = Color::fromRGB(0.12, 0.12, 0.14);
///   // ... set other fields as needed ...
///   theme() = myTheme;
///
///   // Or tweak just one field on the current theme:
///   theme().primary = Color::fromRGB(1.0, 0.4, 0.0);

#include <cstdint>
#include <string>

#include "geometry.h"

namespace cui {

/// Design tokens for the entire UI. All color fields are public so that
/// users can construct, copy, or modify individual tokens at runtime.
struct Theme {
    // ── Primary accent ────────────────────────────────────────────────────
    Color primary;           // Main accent (buttons, selection, focus)
    Color primaryDark;       // Pressed / darker variant
    Color primaryLight;      // Selection background / light accent

    // ── Semantic colors ───────────────────────────────────────────────────
    Color success;
    Color warning;
    Color error;

    // ── Surface / background ──────────────────────────────────────────────
    Color surface;           // Card / widget background
    Color background;        // Window / app background
    Color surfaceVariant;    // Subtle alternative surface (tab strip, etc.)

    // ── Text hierarchy ────────────────────────────────────────────────────
    Color textPrimary;       // High-emphasis text
    Color textSecondary;     // Medium-emphasis text
    Color textDisabled;      // Disabled text
    Color textOnPrimary;     // Text on primary-colored backgrounds

    // ── Borders / dividers ────────────────────────────────────────────────
    Color border;            // Standard border
    Color borderLight;       // Subtle border
    Color divider;           // Very subtle separator

    // ── Interactive state overlays ────────────────────────────────────────
    Color focusRing;         // Focus indicator (usually = primary)
    Color ripple;            // Ripple effect color (usually = primary with alpha)
    Color hoverOverlay;      // Hover state overlay
    Color pressedOverlay;    // Pressed state overlay

    // ── Extended color scheme (上游 MD3 style) ─────────────────────────
    Color primaryContainer;      // Container for primary elements
    Color onPrimaryContainer;    // Text/icons on primaryContainer
    Color secondary;             // Secondary accent color
    Color secondaryContainer;    // Container for secondary elements
    Color tertiary;              // Tertiary accent color
    Color tertiaryContainer;     // Container for tertiary elements
    Color outline;               // Important outlines and borders
    Color outlineVariant;        // Subtle outlines
    Color scrim;                 // Scrim overlay (dialogs, modals)

    // ── Title bar gradient endpoints (DockArea) ───────────────────────────
    Color titleBarStart;     // Gradient start (top)
    Color titleBarEnd;       // Gradient end (bottom)

    // ── Ribbon body gradient endpoints ────────────────────────────────────
    Color ribbonStart;       // Ribbon body gradient start
    Color ribbonEnd;         // Ribbon body gradient end

    // ── Scrollbar thumb ───────────────────────────────────────────────────
    Color scrollbarThumb;    // Scrollbar handle color

    // ── Static metrics (shared across all themes) ─────────────────────────
    static constexpr double radiusSmall  = 4.0;
    static constexpr double radiusMedium = 6.0;
    static constexpr double radiusLarge  = 8.0;
    static constexpr double radiusPill   = 999.0;

    static constexpr uint8_t shadowLight  = 18;
    static constexpr uint8_t shadowMedium = 30;
    static constexpr uint8_t shadowHeavy  = 45;

    static constexpr double spacingXS = 4.0;
    static constexpr double spacingSM = 8.0;
    static constexpr double spacingMD = 12.0;
    static constexpr double spacingLG = 16.0;
    static constexpr double spacingXL = 24.0;

    static constexpr double fontSizeCaption   = 11.0;
    static constexpr double fontSizeBody      = 13.0;
    static constexpr double fontSizeSubtitle  = 15.0;
    static constexpr double fontSizeTitle     = 18.0;
    static constexpr double fontSizeHeadline  = 22.0;

    // ── Elevation system (上游 MD3 style) ─────────────────────────────
    // Elevation levels with corresponding shadow opacity and offset
    struct Elevation {
        double offset;
        uint8_t shadowAlpha;
        double blurRadius;
    };

    static constexpr Elevation elevation0   = {0.0, 0, 0.0};       // No elevation
    static constexpr Elevation elevation1   = {1.0, 12, 2.0};      // Subtle (cards, buttons)
    static constexpr Elevation elevation2   = {2.0, 18, 4.0};      // Raised (hovered cards)
    static constexpr Elevation elevation3   = {3.0, 24, 6.0};      // Floating (FAB, dialogs)
    static constexpr Elevation elevation6   = {6.0, 30, 10.0};     // High (menus, popovers)
    static constexpr Elevation elevation8   = {8.0, 36, 14.0};     // Very high (nav drawer)
    static constexpr Elevation elevation12  = {12.0, 42, 18.0};    // Modal (bottom sheet)
    static constexpr Elevation elevation16  = {16.0, 48, 22.0};    // Highest (dialog)

    // ── Focus ring configuration ─────────────────────────────────────────
    static constexpr double focusRingWidth    = 2.0;
    static constexpr double focusRingOffset   = 2.0;  // Gap between widget and ring
    static constexpr double focusRingRadius   = 4.0;  // Extra radius for ring

    // ── Ripple effect configuration ──────────────────────────────────────
    static constexpr double rippleDuration    = 300.0;  // ms
    static constexpr double rippleMaxRadius   = 150.0;  // px
    static constexpr uint8_t  rippleAlpha     = 40;     // 0-255

    // ── Built-in presets ──────────────────────────────────────────────────
    static Theme light();
    static Theme dark();
    static Theme ocean();
    static Theme forest();
    static Theme skyBlue();     // 天蓝
    static Theme orange();      // 橙色
    static Theme lavender();    // 熏衣草紫
    static Theme ide();         // VS 2026-style IDE (light surface, blue chrome)

    /// Look up a preset by name (case-insensitive): "light","dark","ocean","forest","skyblue","orange","lavender","ide".
    /// Returns light() if the name is not recognised.
    static Theme fromName(const std::string& name);
};

/// Global accessor for the active theme. Returns a mutable reference so that
/// users can either replace the whole theme or tweak individual fields:
///
///   theme() = Theme::dark();           // replace
///   theme().primary = myAccentColor;   // tweak
///
Theme& theme();

}  // namespace cui_core
