/// @file x11_window.cpp
/// @brief X11/XCB implementation of PlatformWindow.

#include "cui/platform/x11_window.h"

#include <clocale>
#include <poll.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <xcb/xcb.h>

#include "cui/platform/event.h"

// Xlib is only needed for the XIM input-method bridge (xcb has no XIM
// client implementation in most distro builds). CMake decides whether to
// link Xlib (CUI_XLIB_AVAILABLE); we still confirm the header resolves
// before including it, so a mismatch degrades to raw keysym input instead
// of a link error.
#if defined(CUI_XLIB_AVAILABLE) && __has_include(<X11/Xlib.h>)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#define CUI_HAVE_XLIB 1
// X.h's `#define None 0L` / `#define Expose 12` / `#define FocusIn 9` /
// `#define FocusOut 10` collide with EventType::None / EventType::Expose /
// EventType::FocusIn / EventType::FocusOut / KeyCode::None.
#undef None
#undef Expose
#undef FocusIn
#undef FocusOut
#endif

namespace cui {

namespace {

// X11 wheel reports as button press 4 (up) / 5 (down) / 6 (left) / 7 (right).
constexpr unsigned kWheelUp = 4, kWheelDown = 5, kWheelLeft = 6, kWheelRight = 7;

int keyFromKeysym(xcb_keysym_t sym) {
    switch (sym) {
        case 0xff0d:
        case 0xff8d:
            return static_cast<int>(KeyCode::Return);  // Return / KP Enter
        case 0xff1b:
            return static_cast<int>(KeyCode::Escape);
        case 0xff08:
            return static_cast<int>(KeyCode::Backspace);
        case 0xff09:
            return static_cast<int>(KeyCode::Tab);
        case 0xffff:
            return static_cast<int>(KeyCode::Delete);
        case 0xff50:
            return static_cast<int>(KeyCode::Home);
        case 0xff57:
            return static_cast<int>(KeyCode::End);
        case 0xff55:
            return static_cast<int>(KeyCode::PageUp);  // XK_Prior / XK_Page_Up
        case 0xff56:
            return static_cast<int>(KeyCode::PageDown);  // XK_Next / XK_Page_Down
        case 0xff51:
            return static_cast<int>(KeyCode::Left);
        case 0xff52:
            return static_cast<int>(KeyCode::Up);
        case 0xff53:
            return static_cast<int>(KeyCode::Right);
        case 0xff54:
            return static_cast<int>(KeyCode::Down);
        default:
            return static_cast<int>(KeyCode::None);
    }
}

// Printable characters: the ASCII range maps 1:1, keypad digits have their
// own keysym block (0xffb0–0xffb9).
std::string textFromKeysym(xcb_keysym_t sym) {
    if (sym >= 0x20 && sym <= 0x7e)
        return std::string(1, static_cast<char>(sym));
    if (sym >= 0xffb0 && sym <= 0xffb9)
        return std::string(1, static_cast<char>('0' + (sym - 0xffb0)));
    return {};
}

}  // namespace

X11Window::X11Window() = default;
X11Window::~X11Window() {
    destroy();
}

void X11Window::create(const std::string& title, int width, int height) {
    width_ = width;
    height_ = height;

    // XIM runs its protocol in the process locale: XOpenIM in the "C" locale
    // connects fine and even creates an IC, but fcitx/ibus never convert any
    // forwarded key — every keystroke passes back untranslated and Chinese
    // input is impossible (probe-verified: identical client with/without
    // setlocale composes vs passes straight through). setlocale(LC_CTYPE, "")
    // adopts the environment's locale; LC_NUMERIC and friends stay "C".
    std::setlocale(LC_CTYPE, "");

    connection_ = XcbConnectionPtr(xcb_connect(nullptr, nullptr));
    if (xcb_connection_has_error(connection_.get())) {
        std::fprintf(stderr, "X11Window: Failed to connect to X server\n");
        return;
    }

    // Get screen
    const xcb_setup_t* setup = xcb_get_setup(connection_.get());
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    screen_ = iter.data;

    // Find visual
    xcb_depth_iterator_t depthIter = xcb_screen_allowed_depths_iterator(screen_);
    visual_ = nullptr;
    for (; depthIter.rem; xcb_depth_next(&depthIter)) {
        xcb_visualtype_iterator_t vtIter = xcb_depth_visuals_iterator(depthIter.data);
        for (; vtIter.rem; xcb_visualtype_next(&vtIter)) {
            if (screen_->root_visual == vtIter.data->visual_id) {
                visual_ = vtIter.data;
                break;
            }
        }
        if (visual_)
            break;
    }

    // Create window
    window_ = xcb_generate_id(connection_.get());
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    // FOCUS_CHANGE drives the XIM input-context focus (see setupIM /
    // dispatchNativeEvent): an unfocused IC never gets keystroke conversion
    // from fcitx/ibus, so Chinese input depends on this mask.
    uint32_t values[] = {screen_->white_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
                                                       XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
                                                       XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS |
                                                       XCB_EVENT_MASK_FOCUS_CHANGE};
    xcb_create_window(connection_.get(), XCB_COPY_FROM_PARENT, window_, screen_->root, 0, 0, width, height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual, mask, values);

    // Graphics context used for blitting Skia frames via PutImage.
    // xcb_generate_id() only allocates an XID locally — it must be backed
    // by a real xcb_create_gc() call or the server rejects PutImage (BadGC).
    gc_ = xcb_generate_id(connection_.get());
    {
        uint32_t gcMask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
        uint32_t gcValues[] = {screen_->black_pixel, screen_->white_pixel};
        xcb_create_gc(connection_.get(), gc_, window_, gcMask, gcValues);
    }

    // Title — set through setTitle() so _NET_WM_NAME carries UTF8_STRING.
    // XCB_ATOM_STRING alone is Latin-1 and mangles em dashes / CJK titles.
    setTitle(title);

    // WM_DELETE_WINDOW
    {
        const char* name = "WM_PROTOCOLS";
        auto cookie = xcb_intern_atom(connection_.get(), 0, static_cast<uint16_t>(strlen(name)), name);
        auto* reply = xcb_intern_atom_reply(connection_.get(), cookie, nullptr);
        wmProtocols_ = reply->atom;
        free(reply);
    }
    {
        const char* name = "WM_DELETE_WINDOW";
        auto cookie = xcb_intern_atom(connection_.get(), 0, static_cast<uint16_t>(strlen(name)), name);
        auto* reply = xcb_intern_atom_reply(connection_.get(), cookie, nullptr);
        wmDeleteWin_ = reply->atom;
        free(reply);
    }
    xcb_change_property(connection_.get(), XCB_PROP_MODE_REPLACE, window_, wmProtocols_, 4, 32, 1, &wmDeleteWin_);

    // Wake-up atom
    {
        const char* name = "CUI_WAKE";
        auto cookie = xcb_intern_atom(connection_.get(), 0, static_cast<uint16_t>(strlen(name)), name);
        auto* reply = xcb_intern_atom_reply(connection_.get(), cookie, nullptr);
        wakeAtom_ = reply->atom;
        free(reply);
    }

    // Cache the keyboard mapping so KeyDown events can be translated to
    // keysyms (characters + semantic keys) without an extra round trip later.
    {
        minKeycode_ = setup->min_keycode;
        const int count = setup->max_keycode - setup->min_keycode + 1;
        auto cookie = xcb_get_keyboard_mapping(connection_.get(), setup->min_keycode, count);
        auto* reply = xcb_get_keyboard_mapping_reply(connection_.get(), cookie, nullptr);
        if (reply) {
            keysymsPerKeycode_ = reply->keysyms_per_keycode;
            auto* syms = xcb_get_keyboard_mapping_keysyms(reply);
            keySyms_.assign(syms, syms + xcb_get_keyboard_mapping_keysyms_length(reply));
            free(reply);
        }
    }

    setupIM();
    initClipboardAtoms();

    xcb_flush(connection_.get());
}

void X11Window::destroy() {
#ifdef CUI_HAVE_XLIB
    if (imContext_) {
        XDestroyIC(static_cast<XIC>(imContext_));
        imContext_ = nullptr;
    }
    // The XIM object is torn down by the server when imDisplay_ closes;
    // XCloseIM must not be used because Xlib would also XCloseIM's display.
    if (imDisplay_) {
        XCloseDisplay(imDisplay_);
        imDisplay_ = nullptr;
    }
    imMethod_ = nullptr;
#endif
    if (window_ && connection_) {
        if (gc_) {
            xcb_free_gc(connection_.get(), gc_);
            gc_ = 0;
        }
        xcb_destroy_window(connection_.get(), window_);
        window_ = 0;
    }
    connection_.reset();
}

void X11Window::show() {
    xcb_map_window(connection_.get(), window_);
    xcb_flush(connection_.get());
}

void X11Window::invalidate() {
    // Send an Expose event to ourselves
    xcb_expose_event_t expose;
    std::memset(&expose, 0, sizeof(expose));
    expose.response_type = XCB_EXPOSE;
    expose.window = window_;
    expose.x = 0;
    expose.y = 0;
    expose.width = static_cast<uint16_t>(width_);
    expose.height = static_cast<uint16_t>(height_);
    // We just trigger a render in the event loop by waking it
    wakeUp();
}

bool X11Window::pollEvent(PlatformEvent& event) {
    xcb_generic_event_t* native = xcb_poll_for_event(connection_.get());
    if (!native) {
        // Give the XIM connection a chance even without an XCB wake-up —
        // the IM's ClientMessages arrive on the second connection.
        if (pumpIMEvent(event))
            return true;
        return false;
    }
    dispatchNativeEvent(native, event);
    free(native);
    // Only fall back to the IM connection when the XCB event carried nothing
    // actionable; otherwise a real mouse event would be clobbered by an
    // unrelated IM keystroke sitting in the second queue.
    if (event.type == EventType::None)
        pumpIMEvent(event);
    return event.type != EventType::None;
}

bool X11Window::waitForEvent(PlatformEvent& event, int timeoutMs) {
    if (timeoutMs == 0)
        return pollEvent(event);

    // Block on BOTH X connections. XIM commits (candidate selection) and the
    // IM's forwarded keystrokes surface only on the Xlib connection, so
    // waiting on xcb_wait_for_event alone would sleep straight past them and
    // leave typed text stranded in the IM queue forever — this is why the
    // text boxes looked dead whenever an input method was active.
    for (;;) {
        // 1. Anything already readable on the XCB connection?
        if (xcb_generic_event_t* native = xcb_poll_for_event(connection_.get())) {
            dispatchNativeEvent(native, event);
            free(native);
            // Wake-up / structural events return false, mirroring the old
            // semantics: the run loop must get a turn to drain its task queue.
            return event.type != EventType::None;
        }
        // 2. Anything the IM connection has already buffered?
        if (pumpIMEvent(event))
            return true;

        // 3. Nothing queued — sleep in poll() until either fd fires.
        struct pollfd pfds[2];
        nfds_t n = 0;
        pfds[n].fd = xcb_get_file_descriptor(connection_.get());
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        ++n;
#ifdef CUI_HAVE_XLIB
        if (imDisplay_) {
            pfds[n].fd = ConnectionNumber(imDisplay_);
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            ++n;
        }
#endif
        if (xcb_connection_has_error(connection_.get()))
            return false;
        if (poll(pfds, n, timeoutMs < 0 ? -1 : timeoutMs) <= 0)
            return false;  // timeout or error
        // Pull the freshly-arrived Xlib events into its queue for pumpIMEvent.
#ifdef CUI_HAVE_XLIB
        if (imDisplay_)
            XFlush(imDisplay_);
#endif
    }
}

void X11Window::wakeUp() {
    xcb_client_message_event_t msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = window_;
    msg.type = wakeAtom_;
    msg.data.data32[0] = WAKEUP_MSG;
    xcb_send_event(connection_.get(), 0, window_, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&msg));
    xcb_flush(connection_.get());
}

PlatformWindow::NativeHandles X11Window::nativeHandles() {
    NativeHandles h;
    h.xcb_connection = connection_.get();
    h.xcb_window = window_;
    h.xcb_visual = visual_;
    h.xcb_gc = gc_;
    h.xcb_depth = screen_ ? screen_->root_depth : 0;
    return h;
}

void X11Window::setTitle(const std::string& title) {
    if (!connection_ || !window_)
        return;
    // Use _NET_WM_NAME with UTF8_STRING encoding for proper Unicode support.
    // NOTE: "UTF8_STRING" is 11 characters — a wrong length interns a
    // truncated atom (e.g. "UTF8_STRIN"), which strict WMs reject as a
    // non-UTF8 property type and then fall back to Latin-1 WM_NAME mojibake.
    xcb_intern_atom_cookie_t utf8Cookie = xcb_intern_atom(connection_.get(), 0, 11, "UTF8_STRING");
    xcb_intern_atom_cookie_t netWmNameCookie = xcb_intern_atom(connection_.get(), 0, 12, "_NET_WM_NAME");
    xcb_intern_atom_reply_t* utf8Reply = xcb_intern_atom_reply(connection_.get(), utf8Cookie, nullptr);
    xcb_intern_atom_reply_t* netWmNameReply = xcb_intern_atom_reply(connection_.get(), netWmNameCookie, nullptr);
    if (utf8Reply && netWmNameReply) {
        xcb_change_property(connection_.get(), XCB_PROP_MODE_REPLACE, window_, netWmNameReply->atom, utf8Reply->atom, 8,
                            title.size(), title.c_str());
    }
    // Fallback to WM_NAME for legacy WMs.
    xcb_change_property(connection_.get(), XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        title.size(), title.c_str());
    free(utf8Reply);
    free(netWmNameReply);
    xcb_flush(connection_.get());
}

void X11Window::setMinimumSize(int width, int height) {
    // XCB doesn't have a direct min-size API; this would require
    // setting WM_NORMAL_HINTS via xcb_change_property.
    // For now, store and handle in configure events.
}

void X11Window::setBorderless(bool borderless) {
    if (!connection_ || !window_)
        return;

    // Use _MOTIF_WM_HINTS to toggle window decorations.
    // This is the standard way to remove/add title bar and borders on X11.
    // The Motif WM hints structure:
    //   flags: indicates which hints are present (MWM_HINTS_DECORATIONS = 2)
    //   decorations: bit flags for which decorations to show (0 = none)
    const char* motifWmHints = "_MOTIF_WM_HINTS";
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(
        connection_.get(), 0, static_cast<uint16_t>(strlen(motifWmHints)), motifWmHints);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(connection_.get(), cookie, nullptr);
    if (!reply)
        return;

    struct MotifWmHints {
        uint32_t flags;
        uint32_t functions;
        uint32_t decorations;
        int32_t  inputMode;
        uint32_t status;
    };

    MotifWmHints hints{};
    hints.flags = 2;  // MWM_HINTS_DECORATIONS
    hints.decorations = borderless ? 0 : 1;  // 0 = no decorations, 1 = default

    xcb_change_property(connection_.get(), XCB_PROP_MODE_REPLACE, window_,
                        reply->atom, reply->atom, 32, 5, &hints);
    free(reply);
    xcb_flush(connection_.get());
}

void X11Window::grabPointer() {
    if (!connection_ || !window_)
        return;
    // owner_events = 1: events inside the window keep reporting against it
    // (unchanged event_x/event_y); events from outside are translated into
    // window coordinates instead of the root, so motion stays in the space
    // the widget handlers already speak.
    xcb_grab_pointer(connection_.get(), 1, window_, XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, window_, XCB_NONE, XCB_CURRENT_TIME);
    xcb_flush(connection_.get());
}

void X11Window::ungrabPointer() {
    if (!connection_ || !window_)
        return;
    xcb_ungrab_pointer(connection_.get(), XCB_CURRENT_TIME);
    xcb_flush(connection_.get());
}

double X11Window::dpiScale() const {
    // Query Xft.dpi or use default
    // Simplified: assume 96 DPI = 1.0 scale
    return 1.0;
}

xcb_keysym_t X11Window::lookupKeysym(uint8_t keycode, bool shift) const {
    if (keysymsPerKeycode_ == 0)
        return 0;
    const int idx = keycode - minKeycode_;
    if (idx < 0 || (idx + 1) * keysymsPerKeycode_ > static_cast<int>(keySyms_.size()))
        return 0;
    const xcb_keysym_t* row = &keySyms_[idx * keysymsPerKeycode_];
    return (shift && keysymsPerKeycode_ > 1) ? row[1] : row[0];
}

void X11Window::dispatchNativeEvent(xcb_generic_event_t* native, PlatformEvent& out) {
    uint8_t type = native->response_type & 0x7f;
    out = {};

    switch (type) {
        case XCB_EXPOSE:
            out.type = EventType::Expose;
            break;

        case XCB_CONFIGURE_NOTIFY: {
            auto* ce = reinterpret_cast<xcb_configure_notify_event_t*>(native);
            width_ = ce->width;
            height_ = ce->height;
            out.type = EventType::Resize;
            out.width = ce->width;
            out.height = ce->height;
            break;
        }

        case XCB_BUTTON_PRESS: {
            auto* be = reinterpret_cast<xcb_button_press_event_t*>(native);
            out.pos = {static_cast<double>(be->event_x), static_cast<double>(be->event_y)};
            switch (be->detail) {
                case kWheelUp:
                    out.type = EventType::MouseWheel;
                    out.deltaY = -1;  // 格数（notch），由 Application 换算像素
                    break;
                case kWheelDown:
                    out.type = EventType::MouseWheel;
                    out.deltaY = 1;
                    break;
                case kWheelLeft:
                    out.type = EventType::MouseWheel;
                    out.deltaX = -1;
                    break;
                case kWheelRight:
                    out.type = EventType::MouseWheel;
                    out.deltaX = 1;
                    break;
                default:
                    out.type = EventType::MouseDown;
                    out.button = be->detail;
                    break;
            }
            break;
        }

        case XCB_BUTTON_RELEASE: {
            auto* be = reinterpret_cast<xcb_button_release_event_t*>(native);
            // Wheel buttons only generate a press; swallow their release.
            if (be->detail >= kWheelUp && be->detail <= kWheelRight)
                break;
            out.type = EventType::MouseUp;
            out.pos = {static_cast<double>(be->event_x), static_cast<double>(be->event_y)};
            out.button = be->detail;
            break;
        }

        case XCB_MOTION_NOTIFY: {
            auto* me = reinterpret_cast<xcb_motion_notify_event_t*>(native);
            out.type = EventType::MouseMove;
            out.pos = {static_cast<double>(me->event_x), static_cast<double>(me->event_y)};
            break;
        }

        case XCB_KEY_PRESS: {
            auto* ke = reinterpret_cast<xcb_key_press_event_t*>(native);
            // With the XIM bridge active, keys are filtered on the Xlib
            // connection (pumpIMEvent) so the IM can consume them; the raw XCB
            // copy must not also be dispatched or composing would double-insert.
            if (imContext_ != nullptr)
                break;
            const bool shift = ke->state & XCB_MOD_MASK_SHIFT;
            const xcb_keysym_t sym = lookupKeysym(ke->detail, shift);
            out.type = EventType::KeyDown;
            out.key = keyFromKeysym(sym);
            out.text = textFromKeysym(sym);
            out.modifiers = ((ke->state & XCB_MOD_MASK_SHIFT) ? ModShift : 0) |
                            ((ke->state & XCB_MOD_MASK_CONTROL) ? ModCtrl : 0) |
                            ((ke->state & XCB_MOD_MASK_1) ? ModAlt : 0);
            break;
        }

        case XCB_FOCUS_IN: {
            out.type = EventType::FocusIn;
#ifdef CUI_HAVE_XLIB
            // Mirror window focus onto the XIM input context: an unfocused
            // IC makes fcitx/ibus treat every forwarded key as "no input
            // context focused" — keys are passed straight back untranslated
            // and no composition (or commit) can ever start.
            if (imContext_) {
                XSetICFocus(static_cast<XIC>(imContext_));
                XFlush(imDisplay_);
            }
#endif
            break;
        }

        case XCB_FOCUS_OUT: {
            out.type = EventType::FocusOut;
#ifdef CUI_HAVE_XLIB
            if (imContext_) {
                XUnsetICFocus(static_cast<XIC>(imContext_));
                XFlush(imDisplay_);
            }
#endif
            break;
        }

        case XCB_CLIENT_MESSAGE: {
            auto* cm = reinterpret_cast<xcb_client_message_event_t*>(native);
            if (cm->type == wmProtocols_ && cm->data.data32[0] == wmDeleteWin_) {
                out.type = EventType::Close;
            } else if (cm->type == wakeAtom_ && cm->data.data32[0] == WAKEUP_MSG) {
                out.type = EventType::None;  // Wake-up, not a real event
            }
            break;
        }

        case XCB_SELECTION_REQUEST:
            handleSelectionRequest(reinterpret_cast<xcb_selection_request_event_t*>(native));
            out.type = EventType::None;
            break;

        case XCB_SELECTION_NOTIFY:
            handleSelectionNotify(reinterpret_cast<xcb_selection_notify_event_t*>(native));
            out.type = EventType::None;
            break;

        default:
            out.type = EventType::None;
            break;
    }
}

// ── XIM input-method bridge ──────────────────────────────────────────────
//
// fcitx/ibus expose an XIM server (XMODIFIERS=@im=...); Xlib implements the
// client side. We keep a second X connection solely for XIM: key events are
// forwarded through XFilterEvent, consumed while composing, and re-surface
// as commit / preedit callbacks. Without the bridge only ASCII keysyms
// reach widgets, which is why Chinese input silently produced nothing.
#ifdef CUI_HAVE_XLIB

// The preedit-callback structs were dropped from modern Xlib headers, but
// Xlib's XIM implementation still passes exactly this ABI to the client.
typedef enum {
    XIM_PREEDIT_START,
    XIM_PREEDIT_DRAW,
    XIM_PREEDIT_CARET,
    XIM_PREEDIT_COPY,
    XIM_PREEDIT_END
} XIMPreeditCallbackReason;
typedef struct {
    XIMPreeditCallbackReason reason;
    XPointer structure;
} XIMPreeditCallbackStruct;

namespace {

// XNPreeditStart/Draw/Copy/Done trampoline. Xlib invokes preedit callbacks
// with (client_data, XIMPreeditCallbackStruct*).
void preeditCb(XPointer ptr, XPointer data) {
    static_cast<X11Window*>(reinterpret_cast<void*>(ptr))->onIMPreedit(data);
}

}  // namespace

bool X11Window::setupIM() {
    if (!window_)
        return false;
    imDisplay_ = XOpenDisplay(nullptr);
    if (!imDisplay_)
        return false;
    XSetLocaleModifiers("");
    imMethod_ = XOpenIM(imDisplay_, nullptr, nullptr, nullptr);
    if (!imMethod_) {
        // Symmetric with the "attached" banner below: XOpenIM fails when
        // XMODIFIERS names an input-method server that is not running.
        // Surface that so a "cannot type Chinese" session can tell "no IM
        // bridge attached" apart from "bridge attached but IM broken".
        const char* mods = std::getenv("XMODIFIERS");
        std::fprintf(stderr, "cui_core: XOpenIM failed (XMODIFIERS=%s) — Chinese input disabled\n",
                     mods ? mods : "(unset)");
        XCloseDisplay(imDisplay_);
        imDisplay_ = nullptr;
        return false;
    }
    // Style preference (most → least reliable for actual text delivery):
    //  1. PreeditNothing/StatusNothing — the IM owns its own preedit/candidate
    //     window and the committed characters arrive through the follow-up
    //     KeyPress via Xutf8LookupString. This is the classic, battle-tested
    //     path (fcitx/ibus advertise it) and needs no commit-callback ABI.
    //  2. PreeditCallbacks (OnTheSpot) — inline preedit we render ourselves;
    //     kept as a secondary so imPreedit_ can be populated where available.
    bool hasNothing = false, hasCallbacks = false;
    XIMStyles* styles = nullptr;
    XGetIMValues(static_cast<XIM>(imMethod_), XNQueryInputStyle, &styles, nullptr);
    if (styles) {
        for (int i = 0; i < styles->count_styles; ++i) {
            const XIMStyle s = styles->supported_styles[i];
            if (s == (XIMPreeditNothing | XIMStatusNothing))
                hasNothing = true;
            else if (s == (XIMPreeditCallbacks | XIMStatusNothing))
                hasCallbacks = true;
        }
        XFree(styles);
    }
    XIMStyle style = (hasNothing   ? static_cast<XIMStyle>(XIMPreeditNothing | XIMStatusNothing) :
                      hasCallbacks ? static_cast<XIMStyle>(XIMPreeditCallbacks | XIMStatusNothing) :
                                     static_cast<XIMStyle>(XIMPreeditNothing | XIMStatusNothing));
    imPreeditActive_ = hasCallbacks && !hasNothing;

    XIC ic = XCreateIC(static_cast<XIM>(imMethod_), XNInputStyle, style, XNClientWindow, static_cast<Window>(window_),
                       XNFocusWindow, static_cast<Window>(window_), nullptr);
    if (!ic) {
        XCloseIM(static_cast<XIM>(imMethod_));
        imMethod_ = nullptr;
        XCloseDisplay(imDisplay_);
        imDisplay_ = nullptr;
        return false;
    }
    imContext_ = ic;

    // Preedit (OnTheSpot) callbacks only matter when the IC negotiated
    // XIMPreeditCallbacks; with PreeditNothing the IM draws its own window.
    if (imPreeditActive_) {
        XIMCallback preeditCB;
        preeditCB.callback = reinterpret_cast<XIMProc>(preeditCb);
        preeditCB.client_data = reinterpret_cast<XPointer>(this);
        XSetICValues(ic, XNPreeditStartCallback, &preeditCB, XNPreeditDrawCallback, &preeditCB, XNPreeditDoneCallback,
                     &preeditCB, nullptr);
    }

    // The Xlib connection needs its own key selection: XFilterEvent can only
    // forward events retrieved from this connection's queue. The XCB copy of
    // the same KeyPress is ignored (see dispatchNativeEvent).
    XSelectInput(imDisplay_, static_cast<Window>(window_), KeyPressMask | KeyReleaseMask);
    // An unfocused IC is a dead IC: fcitx AND ibus pass every forwarded key
    // back untranslated until the input context is told it has focus, so no
    // composition can ever start. Focus is mirrored from the window's X
    // focus-change events (XCB_FOCUS_IN/OUT in dispatchNativeEvent); sync
    // the initial state here because the window may already be focused by
    // the time setupIM() runs.
    Window focusW = 0;
    int revertTo = 0;
    XGetInputFocus(imDisplay_, &focusW, &revertTo);
    if (focusW == static_cast<Window>(window_))
        XSetICFocus(ic);
    XSync(imDisplay_, False);
    std::fprintf(stderr, "cui_core: XIM input method attached\n");
    return true;
}

bool X11Window::pumpIMEvent(PlatformEvent& out) {
    if (!imDisplay_ || !imContext_)
        return false;
    // The queue must be drained through XNextEvent, NOT XCheckWindowEvent:
    // Xlib delivers the XIM protocol traffic (the commit packets) as
    // ClientMessages addressed to the transport's own private window, which
    // a window/mask-filtered peek never returns — the packet then sits in
    // the queue forever while XFilterEvent keeps reporting every keystroke
    // as consumed, i.e. the text box looks completely dead.
    bool gotPreedit = false;
    while (XPending(imDisplay_)) {
        XEvent xe;
        XNextEvent(imDisplay_, &xe);
        const XKeyEvent& k = xe.xkey;
        // Ctrl/Alt shortcuts (Ctrl+C/V/X/Z, …) must bypass XFilterEvent:
        // some IM frameworks (fcitx with certain modules, ibus) consume
        // Ctrl+key through XFilterEvent for their own shortcut handling,
        // so the event never reaches Xutf8LookupString and the app sees
        // nothing.  Detect modifier combos up-front, skip the filter, and
        // synthesise the single-char text from the base keysym.
        // 0x20 (Space) is deliberately excluded: Ctrl+Space is the universal
        // IM toggle (fcitx/ibus trigger) — synthesising a shortcut for it
        // would keep the hotkey from the IM server, and the user could then
        // never switch to/from Chinese inside the app.
        if (xe.type == KeyPress && (k.state & (ControlMask | Mod1Mask))) {
            KeySym baseSym = XLookupKeysym(const_cast<XKeyEvent*>(&k), 0);
            if (baseSym >= 0x21 && baseSym <= 0x7e) {
                out = {};
                out.type = EventType::KeyDown;
                out.text.assign(1, static_cast<char>(baseSym));
                out.key = keyFromKeysym(static_cast<xcb_keysym_t>(baseSym));
                out.modifiers = ((k.state & ShiftMask) ? ModShift : 0) |
                                ((k.state & ControlMask) ? ModCtrl : 0) |
                                ((k.state & Mod1Mask) ? ModAlt : 0);
                imKeyState_ = k.state;
                return true;
            }
        }
        // EVERY dequeued event must pass through XFilterEvent BEFORE any
        // type check: this is where libX11 runs the XIM X-transport's event
        // filter (_XRegisterFilterByType, imTrX.c) for the ClientMessage
        // packets the IM server sends to that private window. A commit is
        // turned into a synthetic KeyPress via XPutBackEvent and picked up
        // on a later loop iteration. Filtering only KeyPress — or skipping
        // non-key events before the filter — silently dropped every commit
        // packet: composing worked, Space produced the candidate, and the
        // committed text never landed in the widget.
        if (XFilterEvent(&xe, xe.xany.window)) {
            // Consumed by the IM: a composing keystroke, a commit packet, or
            // transport bookkeeping. Keep draining — the synthetic commit
            // KeyPress put back by the transport is NOT consumed.
            gotPreedit = true;
            continue;
        }
        if (xe.type != KeyPress)
            continue;  // KeyRelease / stray traffic: nothing for widgets
        // Not consumed: either a normal key or the IM's commit keystroke.
        // Translate with the IC's UTF-8 state to get committed Chinese text.
        out = {};
        out.type = EventType::KeyDown;
        imKeyState_ = k.state;
        KeySym sym = NoSymbol;
        char buf[64];
        int n = Xutf8LookupString(static_cast<XIC>(imContext_), const_cast<XKeyEvent*>(&k), buf, sizeof(buf) - 1,
                                  &sym, nullptr);
        if (n > 0)
            out.text.assign(buf, static_cast<size_t>(n));
        out.key = keyFromKeysym(static_cast<xcb_keysym_t>(sym));
        out.modifiers = ((k.state & ShiftMask) ? ModShift : 0) | ((k.state & ControlMask) ? ModCtrl : 0) |
                        ((k.state & Mod1Mask) ? ModAlt : 0);
        return true;
    }
    // Queue exhausted. If we saw preedit events, surface a Preedit beat so
    // the app repaints the inline composition.
    if (gotPreedit) {
        out = {};
        out.type = EventType::Preedit;
        return true;
    }
    return false;
}

void X11Window::pushIMCommit(std::string utf8) {
    imCommits_.push_back(std::move(utf8));
}

void X11Window::onIMPreedit(void* cbStructRaw) {
    auto* st = reinterpret_cast<XIMPreeditCallbackStruct*>(cbStructRaw);
    if (!st)
        return;
    if (st->reason == XIM_PREEDIT_DRAW && st->structure) {
        auto* draw = reinterpret_cast<XIMPreeditDrawCallbackStruct*>(st->structure);
        // Only the single-line concatenation (combo) text is rendered;
        // the text is NUL-terminated UTF-8 in the fcitx/ibus implementations.
        if (draw->text && draw->text->length > 0 && !draw->text->encoding_is_wchar && draw->text->string.multi_byte)
            imPreedit_ = draw->text->string.multi_byte;
        else
            imPreedit_.clear();
    } else if (st->reason == XIM_PREEDIT_START || st->reason == XIM_PREEDIT_END) {
        imPreedit_.clear();
    }
}

bool X11Window::takeIMCommit(std::string& utf8) {
    if (imCommits_.empty())
        return false;
    utf8 = std::move(imCommits_.front());
    imCommits_.pop_front();
    return true;
}

std::string X11Window::imPreedit() const {
    return imPreedit_;
}

void X11Window::setIMPreeditRect(int x, int y) {
    if (!imDisplay_ || !imContext_)
        return;
    const std::string key = std::to_string(x) + "," + std::to_string(y);
    if (key == lastPreeditRect_)
        return;  // avoid a round trip per keystroke
    lastPreeditRect_ = key;
    XPoint spot;
    spot.x = static_cast<short>(x);
    spot.y = static_cast<short>(y);
    XVaNestedList attrs = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
    XSetICValues(static_cast<XIC>(imContext_), XNPreeditAttributes, attrs, nullptr);
    XFree(attrs);
}

#else  // !CUI_HAVE_XLIB — stubs so the header contracts still link.

bool X11Window::setupIM() {
    return false;
}
bool X11Window::pumpIMEvent(PlatformEvent&) {
    return false;
}
void X11Window::pushIMCommit(std::string utf8) {
    imCommits_.push_back(std::move(utf8));
}
void X11Window::onIMPreedit(void*) {
}
bool X11Window::takeIMCommit(std::string&) {
    return false;
}
std::string X11Window::imPreedit() const {
    return {};
}
void X11Window::setIMPreeditRect(int, int) {
}

#endif

// ── Clipboard (X11 selection) ──────────────────────────────────────────────

void X11Window::initClipboardAtoms() {
    if (!connection_) return;
    auto intern = [this](const char* name) {
        auto cookie = xcb_intern_atom(connection_.get(), 0,
            static_cast<uint16_t>(strlen(name)), name);
        auto* reply = xcb_intern_atom_reply(connection_.get(), cookie, nullptr);
        xcb_atom_t atom = reply ? reply->atom : 0;
        free(reply);
        return atom;
    };
    clipboardAtom_ = intern("CLIPBOARD");
    targetsAtom_ = intern("TARGETS");
    utf8StringAtom_ = intern("UTF8_STRING");
    textAtom_ = intern("TEXT");
    stringAtom_ = intern("STRING");
    incrAtom_ = intern("INCR");
}

bool X11Window::clipboardSetText(const std::string& utf8) {
    if (!connection_ || !window_ || !clipboardAtom_)
        return false;
    clipboardText_ = utf8;
    // Claim ownership of CLIPBOARD selection. Use CurrentTime so the server
    // accepts the request even if our last event timestamp is stale.
    xcb_set_selection_owner(connection_.get(), window_, clipboardAtom_, XCB_CURRENT_TIME);
    // Verify ownership was granted.
    auto cookie = xcb_get_selection_owner(connection_.get(), clipboardAtom_);
    auto* reply = xcb_get_selection_owner_reply(connection_.get(), cookie, nullptr);
    bool ok = reply && reply->owner == window_;
    ownsClipboard_ = ok;
    free(reply);
    xcb_flush(connection_.get());
    return ok;
}

std::string X11Window::clipboardText() const {
    if (!connection_ || !window_ || !clipboardAtom_)
        return {};
    // Check if we own the selection — return our cached text directly.
    if (ownsClipboard_)
        return clipboardText_;

    // Request the clipboard content from the current owner.
    pendingPaste_.clear();
    pasteReceived_ = false;
    pasteTime_ = XCB_CURRENT_TIME;

    xcb_convert_selection(connection_.get(), window_,
        clipboardAtom_, utf8StringAtom_, clipboardAtom_, XCB_CURRENT_TIME);
    xcb_flush(connection_.get());

    // Wait for SelectionNotify (with timeout to avoid hanging).
    for (int i = 0; i < 50; ++i) {
        xcb_generic_event_t* ev = xcb_poll_for_event(connection_.get());
        if (!ev) {
            // Brief sleep to avoid busy-waiting.
            struct timespec ts = {0, 10000000};  // 10ms
            nanosleep(&ts, nullptr);
            continue;
        }
        uint8_t type = ev->response_type & 0x7f;
        if (type == XCB_SELECTION_NOTIFY) {
            const_cast<X11Window*>(this)->handleSelectionNotify(
                reinterpret_cast<xcb_selection_notify_event_t*>(ev));
            free(ev);
            break;
        }
        // Dispatch other events normally so we don't stall the app.
        PlatformEvent dummy;
        const_cast<X11Window*>(this)->dispatchNativeEvent(ev, dummy);
        free(ev);
    }
    return pendingPaste_;
}

void X11Window::handleSelectionRequest(xcb_selection_request_event_t* req) {
    if (!req || req->selection != clipboardAtom_)
        return;

    xcb_atom_t target = req->target;
    xcb_atom_t property = req->property;
    if (property == XCB_ATOM_NONE)
        property = req->target;  // legacy: use target as property

    if (target == targetsAtom_) {
        // Advertise supported targets.
        xcb_atom_t targets[] = {targetsAtom_, utf8StringAtom_, textAtom_, stringAtom_};
        xcb_change_property(connection_.get(), XCB_PROP_MODE_REPLACE, req->requestor,
            property, XCB_ATOM_ATOM, 32, sizeof(targets) / sizeof(targets[0]), targets);
    } else if (target == utf8StringAtom_ || target == textAtom_ || target == stringAtom_) {
        if (!clipboardText_.empty()) {
            xcb_change_property(connection_.get(), XCB_PROP_MODE_REPLACE, req->requestor,
                property, target, 8, clipboardText_.size(), clipboardText_.c_str());
        } else {
            property = XCB_ATOM_NONE;
        }
    } else {
        property = XCB_ATOM_NONE;
    }

    // Send SelectionNotify.
    xcb_selection_notify_event_t notify;
    std::memset(&notify, 0, sizeof(notify));
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.requestor = req->requestor;
    notify.selection = req->selection;
    notify.target = req->target;
    notify.property = property;
    notify.time = req->time;
    xcb_send_event(connection_.get(), 0, req->requestor,
        XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&notify));
    xcb_flush(connection_.get());
}

void X11Window::handleSelectionNotify(xcb_selection_notify_event_t* notify) {
    if (!notify || notify->selection != clipboardAtom_)
        return;
    if (notify->property == XCB_ATOM_NONE) {
        // Conversion failed.
        pasteReceived_ = true;
        return;
    }
    // Read the property data.
    auto cookie = xcb_get_property(connection_.get(), 0, window_,
        notify->property, notify->target, 0, 4096);
    auto* reply = xcb_get_property_reply(connection_.get(), cookie, nullptr);
    if (reply) {
        int len = xcb_get_property_value_length(reply);
        if (len > 0) {
            const char* data = static_cast<const char*>(xcb_get_property_value(reply));
            pendingPaste_.assign(data, len);
        }
        // Delete the property to free server memory.
        xcb_delete_property(connection_.get(), window_, notify->property);
    }
    free(reply);
    pasteReceived_ = true;
}

}  // namespace cui_core
