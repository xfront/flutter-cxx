#pragma once
/// @file event.h
/// @brief Platform-agnostic event types.
///
/// The platform layer (X11, Win32, Cocoa) translates native events
/// into these uniform types. The Application dispatches them to widgets.

#include <cstdint>
#include <string>

#include "../foundation/geometry.h"

namespace cui {

enum class EventType {
    None,
    Close,        // Window close requested
    Resize,       // Window resized
    Expose,       // Window needs repaint
    MouseDown,    // Mouse button pressed
    MouseUp,      // Mouse button released
    MouseMove,    // Mouse moved
    MouseWheel,   // Mouse wheel scrolled
    KeyDown,      // Key pressed
    KeyUp,        // Key released
    Preedit,      // Input-method preedit changed (composition display)
    FocusIn,      // Window gained focus
    FocusOut,     // Window lost focus
    TouchBegin,   // Touch started
    TouchUpdate,  // Touch moved
    TouchEnd,     // Touch ended
};

/// Semantic key codes for KeyDown events. Printable characters do NOT get
/// a Key code — they arrive via PlatformEvent::text instead.
enum class KeyCode : int {
    None = 0,
    Return,
    Escape,
    Backspace,
    Tab,
    Delete,
    Left,
    Right,
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
};

/// Keyboard modifier bit flags (PlatformEvent::modifiers).
enum KeyModifier : int {
    ModShift = 1,
    ModCtrl = 2,
    ModAlt = 4,
    ModMeta = 8,
};

/// 滚轮每格（notch）对应的默认像素步长。
///
/// 平台层报告滚轮增量时保留原始单位（见 PlatformEvent::wheelInNotches），
/// 由 Application 按此步长换算成像素，使各平台手感一致。
inline constexpr double kDefaultWheelPixelsPerNotch = 56.0;

struct PlatformEvent {
    EventType type = EventType::None;
    Point pos{};        // Mouse position (window-local)
    int button = 0;     // Mouse button (1=left, 2=middle, 3=right)
    int key = 0;        // Key code (Key enum value for special keys)
    std::string text;   // UTF-8 text produced by the key (printable keys)
    int modifiers = 0;  // Bitmask of Modifier flags
    int width = 0;      // For Resize
    int height = 0;     // For Resize
    double deltaX = 0;  // Mouse wheel horizontal
    double deltaY = 0;  // Mouse wheel vertical（正值向下）
    /// 滚轮增量的单位：true = deltaX/deltaY 是「格数」（notch），
    /// false = 已是像素。X11 / Win32 / Wayland 报格数，Web 报浏览器原生像素。
    /// Application 据此把增量换算成像素（见 kDefaultWheelPixelsPerNotch）。
    bool wheelInNotches = true;
    int touchId = 0;    // Touch point identifier

    // Internal: used by Win32 backend to track pending KeyDown waiting for WM_CHAR
    bool hasPendingChar = false;
};

}  // namespace cui_core
