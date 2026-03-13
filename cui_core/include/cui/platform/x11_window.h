#pragma once
/// @file x11_window.h
/// @brief X11/XCB implementation of PlatformWindow (Linux).

#include <deque>
#include <string>
#include <vector>
#include <xcb/xcb.h>

#include "cui/utils/raii.h"
#include "platform_window.h"

// Opaque Xlib types for the XIM input-method bridge (implementation lives
// in x11_window.cpp; no Xlib headers leak to consumers).
typedef struct _XDisplay Display;

namespace cui {

class X11Window : public PlatformWindow {
public:
    X11Window();
    ~X11Window() override;

    void create(const std::string& title, int width, int height) override;
    void destroy() override;
    bool pollEvent(PlatformEvent& event) override;
    bool waitForEvent(PlatformEvent& event, int timeoutMs) override;
    void invalidate() override;
    void show() override;
    int width() const override { return width_; }
    int height() const override { return height_; }
    void wakeUp() override;
    void setTitle(const std::string& title) override;
    void setMinimumSize(int width, int height) override;
    void setBorderless(bool borderless) override;
    void grabPointer() override;
    void ungrabPointer() override;
    bool takeIMCommit(std::string& utf8) override;
    std::string imPreedit() const override;
    void setIMPreeditRect(int x, int y) override;
    bool clipboardSetText(const std::string& utf8) override;
    std::string clipboardText() const override;

    /// Feed a committed UTF-8 string from the XIM commit callback.
    void pushIMCommit(std::string utf8);
    /// Entry point for the XIM preedit callbacks (called via trampoline).
    void onIMPreedit(void* cbStruct);
    double dpiScale() const override;
    NativeHandles nativeHandles() override;

private:
    void dispatchNativeEvent(xcb_generic_event_t* native, PlatformEvent& out);

    // ── XIM input-method bridge (Xlib on a second connection) ───────────
    bool setupIM();                        // called once from create()
    bool pumpIMEvent(PlatformEvent& out);  // XNextEvent + XFilterEvent on every event
    Display* imDisplay_ = nullptr;         // separate X connection
    void* imMethod_ = nullptr;             // XIM
    void* imContext_ = nullptr;            // XIC
    std::deque<std::string> imCommits_;    // committed UTF-8 strings
    std::string imPreedit_;                // current composition
    std::string lastPreeditRect_;          // "x,y" dedupe for SetICValues
    unsigned imKeyState_ = 0;              // modifiers of last key event
    bool imPreeditActive_ = false;         // IM negotiated OnTheSpot preedit

    // Cached keyboard mapping (keycode → keysyms), fetched once in create().
    xcb_keysym_t lookupKeysym(uint8_t keycode, bool shift) const;
    std::vector<xcb_keysym_t> keySyms_;
    int minKeycode_ = 0;
    int keysymsPerKeycode_ = 0;

    XcbConnectionPtr connection_;
    xcb_window_t window_ = 0;
    xcb_gcontext_t gc_ = 0;  // For PutImage blits
    xcb_screen_t* screen_ = nullptr;
    xcb_visualtype_t* visual_ = nullptr;

    int width_ = 0;
    int height_ = 0;

    // WM_PROTOCOLS
    xcb_atom_t wmProtocols_ = 0;
    xcb_atom_t wmDeleteWin_ = 0;

    // Wake-up atom
    xcb_atom_t wakeAtom_ = 0;
    static constexpr uint32_t WAKEUP_MSG = 0x5255574B;  // "RUWK"

    // ── Clipboard (X11 selection) ──────────────────────────────────────
    void initClipboardAtoms();
    void handleSelectionRequest(xcb_selection_request_event_t* req);
    void handleSelectionNotify(xcb_selection_notify_event_t* notify);
    xcb_atom_t clipboardAtom_ = 0;      // CLIPBOARD
    xcb_atom_t targetsAtom_ = 0;        // TARGETS
    xcb_atom_t utf8StringAtom_ = 0;     // UTF8_STRING
    xcb_atom_t textAtom_ = 0;           // TEXT
    xcb_atom_t stringAtom_ = 0;         // STRING
    xcb_atom_t incrAtom_ = 0;           // INCR
    std::string clipboardText_;         // cached clipboard text (owner side)
    bool ownsClipboard_ = false;        // whether we own CLIPBOARD selection
    // Pending paste request state
    mutable std::string pendingPaste_;  // received text from SelectionNotify
    mutable bool pasteReceived_ = false;
    mutable xcb_timestamp_t pasteTime_ = 0;
};

}  // namespace cui_core
