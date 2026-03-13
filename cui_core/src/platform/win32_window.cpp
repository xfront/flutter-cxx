/// @file win32_window.cpp
/// @brief Win32 implementation of PlatformWindow.

#include "cui/platform/win32_window.h"

#include <cstdio>
#include <cstring>

#include "cui/platform/event.h"

// imm32 / imm.h — the IME composition API.  Linking imm32 is done in
// CMakeLists.txt; the header ships with every Windows SDK.
#include <imm.h>
// GET_X_LPARAM / GET_Y_LPARAM for mouse-coordinate extraction.
#include <windowsx.h>

namespace cui {

namespace {

// Custom message posted by wakeUp() so the event loop can drain the GUI
// task queue from another thread.  Must live in the WM_APP range to avoid
// colliding with system messages.
constexpr UINT kWakeUpMsg = 0xC000 + 1;

// WM_MOUSEWHEEL delivers the delta in HIWORD(wParam) as a signed 16-bit
// value measured in WHEEL_DELTA (120) units.
constexpr double kWheelDelta = 120.0;

// Map a Win32 virtual-key code to the semantic Key enum.  Printable
// characters return KeyCode::None — their text arrives via WM_CHAR instead.
int keyFromVK(WPARAM vk) {
    switch (vk) {
        case VK_RETURN:  return static_cast<int>(KeyCode::Return);
        case VK_ESCAPE:  return static_cast<int>(KeyCode::Escape);
        case VK_BACK:    return static_cast<int>(KeyCode::Backspace);
        case VK_TAB:     return static_cast<int>(KeyCode::Tab);
        case VK_DELETE:  return static_cast<int>(KeyCode::Delete);
        case VK_LEFT:    return static_cast<int>(KeyCode::Left);
        case VK_RIGHT:   return static_cast<int>(KeyCode::Right);
        case VK_UP:      return static_cast<int>(KeyCode::Up);
        case VK_DOWN:    return static_cast<int>(KeyCode::Down);
        case VK_PRIOR:   return static_cast<int>(KeyCode::PageUp);
        case VK_NEXT:    return static_cast<int>(KeyCode::PageDown);
        case VK_HOME:    return static_cast<int>(KeyCode::Home);
        case VK_END:     return static_cast<int>(KeyCode::End);
        default:         return static_cast<int>(KeyCode::None);
    }
}

int modifierFlags() {
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)   m |= ModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= ModCtrl;
    if (GetKeyState(VK_MENU) & 0x8000)    m |= ModAlt;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= ModMeta;
    return m;
}

// Convert a wide string to UTF-8.
std::string wideToUtf8(const wchar_t* wide, int len) {
    if (len <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, len, out.data(), n, nullptr, nullptr);
    return out;
}

// Window-class name — process-global, so a static wide string suffices.
const wchar_t* windowClassName() {
    static const wchar_t* name = L"CUIWindow";
    return name;
}

// Register the window class once per process.
ATOM registerClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Win32Window::wndProc;  // resolved via the HWND's user-data
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // we paint every frame via Skia
    wc.lpszClassName = windowClassName();
    return RegisterClassExW(&wc);
}

}  // namespace

// ── Construction / destruction ──────────────────────────────────────────────

Win32Window::Win32Window() = default;

Win32Window::~Win32Window() {
    destroy();
}

// ── create / destroy ────────────────────────────────────────────────────────

void Win32Window::create(const std::string& title, int width, int height) {
    width_ = width;
    height_ = height;

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    registerClass(hInstance);

    // Convert the UTF-8 title to wide chars for CreateWindowW.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(),
                                   static_cast<int>(title.size()), nullptr, 0);
    std::wstring wTitle(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(),
                        static_cast<int>(title.size()), wTitle.data(), wlen);

    // Centre on the primary monitor's work area.
    RECT rc = {0, 0, width, height};
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rc, style, FALSE);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - winW) / 2;
    int y = (screenH - winH) / 2;

    hwnd_ = CreateWindowExW(0, windowClassName(), wTitle.c_str(), style,
                            x, y, winW, winH, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd_) {
        std::fprintf(stderr, "Win32Window: CreateWindowExW failed (%lu)\n", GetLastError());
        return;
    }

    // Store `this` in the window's user-data slot so the static wndProc can
    // dispatch to the correct instance.
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    setupIME();
}

void Win32Window::destroy() {
    destroyIME();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ── show / invalidate / wakeUp ──────────────────────────────────────────────

void Win32Window::show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
    }
}

void Win32Window::invalidate() {
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void Win32Window::wakeUp() {
    if (hwnd_)
        PostMessageW(hwnd_, kWakeUpMsg, 0, 0);
}

// ── title / minimum size ────────────────────────────────────────────────────

void Win32Window::setTitle(const std::string& title) {
    if (!hwnd_) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(),
                                   static_cast<int>(title.size()), nullptr, 0);
    std::wstring wTitle(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(),
                        static_cast<int>(title.size()), wTitle.data(), wlen);
    SetWindowTextW(hwnd_, wTitle.c_str());
}

void Win32Window::setMinimumSize(int width, int height) {
    minWidth_ = width;
    minHeight_ = height;
}

void Win32Window::setBorderless(bool borderless) {
    if (!hwnd_) return;

    // Toggle WS_OVERLAPPEDWINDOW (title bar + border + system menu) vs
    // WS_POPUP (borderless, full client area).
    DWORD style = GetWindowLongW(hwnd_, GWL_STYLE);
    if (borderless) {
        style &= ~WS_OVERLAPPEDWINDOW;
        style |= WS_POPUP | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;  // keep min/max buttons functional
    } else {
        style &= ~(WS_POPUP);
        style |= WS_OVERLAPPEDWINDOW;
    }
    SetWindowLongW(hwnd_, GWL_STYLE, style);
    // Force the window manager to notice the style change.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
}

// ── pointer grab ────────────────────────────────────────────────────────────

void Win32Window::grabPointer() {
    if (hwnd_)
        SetCapture(hwnd_);
}

void Win32Window::ungrabPointer() {
    ReleaseCapture();
}

// ── clipboard ───────────────────────────────────────────────────────────────

bool Win32Window::clipboardSetText(const std::string& utf8) {
    // Convert UTF-8 → UTF-16 and hand the global to the clipboard.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                   static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) return false;
    size_t bytes = static_cast<size_t>(wlen + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) return false;
    wchar_t* dst = static_cast<wchar_t*>(GlobalLock(mem));
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), dst, wlen);
    dst[wlen] = L'\0';
    GlobalUnlock(mem);
    if (!OpenClipboard(hwnd_)) { GlobalFree(mem); return false; }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

std::string Win32Window::clipboardText() const {
    if (!OpenClipboard(hwnd_)) return {};
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return {}; }
    const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
    if (!w) { CloseClipboard(); return {}; }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out;
    if (n > 0) {
        out.resize(static_cast<size_t>(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    }
    GlobalUnlock(h);
    CloseClipboard();
    return out;
}

bool Win32Window::hasClipboardText() const {
    if (!OpenClipboard(hwnd_)) return false;
    bool has = IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
    CloseClipboard();
    return has;
}

// ── DPI ─────────────────────────────────────────────────────────────────────

double Win32Window::dpiScale() const {
    if (!hwnd_) return 1.0;
    // GetDpiForWindow (Win10 1607+).  Falls back to the screen DC.
    UINT dpi = GetDpiForWindow(hwnd_);
    if (dpi == 0) {
        HDC dc = GetDC(hwnd_);
        dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
        ReleaseDC(hwnd_, dc);
    }
    return dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;
}

// ── native handles ──────────────────────────────────────────────────────────

PlatformWindow::NativeHandles Win32Window::nativeHandles() {
    NativeHandles h;
    h.hwnd = hwnd_;
    h.hdc = hwnd_ ? GetDC(hwnd_) : nullptr;
    h.hinstance = GetModuleHandleW(nullptr);
    return h;
}

// ── IME ─────────────────────────────────────────────────────────────────────

void Win32Window::setupIME() {
    // Nothing persistent — Win32 delivers IME via WM_IME_* messages.
    // We just associate the default input context on focus.
}

void Win32Window::destroyIME() {
    imCommits_ = {};
    imPreedit_.clear();
}

bool Win32Window::takeIMCommit(std::string& utf8) {
    if (imCommits_.empty()) return false;
    utf8 = std::move(imCommits_.front());
    imCommits_.pop();
    return true;
}

std::string Win32Window::imPreedit() const {
    return imPreedit_;
}

void Win32Window::setIMPreeditRect(int x, int y) {
    imeCaretPos_ = {x, y};
    if (!hwnd_) return;
    HIMC ctx = ImmGetContext(hwnd_);
    if (!ctx) return;
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = imeCaretPos_;
    ImmSetCompositionWindow(ctx, &cf);
    ImmReleaseContext(hwnd_, ctx);
}

// ── event loop ──────────────────────────────────────────────────────────────

bool Win32Window::pollEvent(PlatformEvent& event) {
    if (!pendingEvents_.empty()) {
        event = pendingEvents_.front();
        pendingEvents_.pop();
        return true;
    }
    MSG msg;
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!pendingEvents_.empty()) {
            event = pendingEvents_.front();
            pendingEvents_.pop();
            return true;
        }
    }
    return false;
}

bool Win32Window::waitForEvent(PlatformEvent& event, int timeoutMs) {
    if (timeoutMs == 0)
        return pollEvent(event);

    // Drain any events already queued before blocking.
    if (!pendingEvents_.empty()) {
        event = pendingEvents_.front();
        pendingEvents_.pop();
        return true;
    }

    if (timeoutMs < 0) {
        // Block until a message arrives.
        MSG msg;
        if (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    } else {
        // Wait with a timeout using MsgWaitForMultipleObjects.
        DWORD wakeMask = QS_ALLINPUT;
        DWORD result = MsgWaitForMultipleObjects(0, nullptr, FALSE,
                                                  static_cast<DWORD>(timeoutMs),
                                                  wakeMask);
        if (result == WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    if (!pendingEvents_.empty()) {
        event = pendingEvents_.front();
        pendingEvents_.pop();
        return true;
    }
    return false;
}

// ── wndProc ─────────────────────────────────────────────────────────────────

LRESULT CALLBACK Win32Window::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // WM_IME_CHAR and WM_CHAR may arrive before create() finishes wiring up
    // the user-data pointer during CreateWindowW's internal message pump.
    // Let DefWindowProc handle those.
    if (!self)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    auto push = [&](PlatformEvent ev) {
        self->pendingEvents_.push(ev);
    };

    switch (msg) {
        case WM_CLOSE: {
            PlatformEvent ev;
            ev.type = EventType::Close;
            push(ev);
            return 0;
        }

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            self->width_ = rc.right;
            self->height_ = rc.bottom;
            PlatformEvent ev;
            ev.type = EventType::Resize;
            ev.width = rc.right;
            ev.height = rc.bottom;
            push(ev);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            PlatformEvent ev;
            ev.type = EventType::Expose;
            push(ev);
            return 0;
        }

        case WM_ERASEBKGND:
            // Skia paints every pixel each frame; suppress the system
            // background erase to avoid flicker.
            return 1;

        // Windows 把双击的第二次按下发成 WM_*BUTTONDBLCLK（消息序为
        // down → up → dblclk → up），不接就等于吞掉一次点击：连着点两下只响应
        // 第一下，而「连点两下」正是试展开/收起的人最自然的节奏。
        case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
        case WM_LBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_RBUTTONDBLCLK: {
            bool middle = msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK;
            bool right = msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK;
            int btn = middle ? 2 : right ? 3 : 1;
            PlatformEvent ev;
            ev.type = EventType::MouseDown;
            ev.button = btn;
            ev.pos = {static_cast<double>(GET_X_LPARAM(lParam)),
                      static_cast<double>(GET_Y_LPARAM(lParam))};
            ev.modifiers = modifierFlags();
            push(ev);
            return 0;
        }

        case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: {
            int btn = (msg == WM_LBUTTONUP) ? 1 : (msg == WM_MBUTTONUP) ? 2 : 3;
            PlatformEvent ev;
            ev.type = EventType::MouseUp;
            ev.button = btn;
            ev.pos = {static_cast<double>(GET_X_LPARAM(lParam)),
                      static_cast<double>(GET_Y_LPARAM(lParam))};
            ev.modifiers = modifierFlags();
            push(ev);
            return 0;
        }

        case WM_MOUSEMOVE: {
            PlatformEvent ev;
            ev.type = EventType::MouseMove;
            ev.pos = {static_cast<double>(GET_X_LPARAM(lParam)),
                      static_cast<double>(GET_Y_LPARAM(lParam))};
            // Track mouse-leave so hover state clears when the cursor exits.
            if (!self->trackingMouseLeave_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                self->trackingMouseLeave_ = true;
            }
            push(ev);
            return 0;
        }

        case WM_MOUSELEAVE:
            self->trackingMouseLeave_ = false;
            return 0;

        case WM_MOUSEWHEEL: {
            PlatformEvent ev;
            ev.type = EventType::MouseWheel;
            // WHEEL_DELTA(120) = 一格；这里报格数，由 Application 换算像素。
            ev.deltaY = -(static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / kWheelDelta);
            // WM_MOUSEWHEEL gives screen coordinates — translate to client.
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            ev.pos = {static_cast<double>(pt.x), static_cast<double>(pt.y)};
            ev.modifiers = modifierFlags();
            push(ev);
            return 0;
        }

        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            // A real (non-IME) key press ends any pending IME echo window.
            if (wParam != VK_PROCESSKEY)
                self->imeEcho_.clear();
            PlatformEvent ev;
            ev.type = EventType::KeyDown;
            ev.key = keyFromVK(wParam);
            ev.modifiers = modifierFlags();
            // Printable keys have no text here — WM_CHAR fills it in.
            // Stash the event so WM_CHAR can attach the text.
            self->pendingKeyDown_ = ev;
            self->pendingKeyDown_.hasPendingChar = true;
            // Non-printable keys (arrows, F-keys…) dispatch immediately.
            if (ev.key != static_cast<int>(KeyCode::None)) {
                push(ev);
                self->pendingKeyDown_.hasPendingChar = false;
            }
            return 0;
        }

        case WM_KEYUP: case WM_SYSKEYUP: {
            PlatformEvent ev;
            ev.type = EventType::KeyUp;
            ev.key = keyFromVK(wParam);
            ev.modifiers = modifierFlags();
            push(ev);
            return 0;
        }

        case WM_CHAR: case WM_IME_CHAR: {
            // IMEs echo the committed composition back as WM_CHAR/WM_IME_CHAR
            // messages; that text already entered through the commit stream
            // (GCS_RESULTSTR above), so swallow the echo to avoid inserting
            // every hanzi twice. A mismatch with the expected echo sequence
            // clears the queue and falls through to normal dispatch.
            if (!self->imeEcho_.empty()) {
                if (static_cast<wchar_t>(wParam) == self->imeEcho_.front()) {
                    self->imeEcho_.erase(self->imeEcho_.begin());
                    self->pendingKeyDown_.hasPendingChar = false;
                    return 0;
                }
                self->imeEcho_.clear();
            }
            // A printable character — attach to the pending KeyDown if one
            // is waiting, otherwise synthesise a KeyDown with text only.
            wchar_t ch = static_cast<wchar_t>(wParam);
            // All control characters are skipped: Enter already dispatched a
            // KeyDown{KeyCode::Return} from WM_KEYDOWN, so letting WM_CHAR('\r')
            // through would deliver the key twice — the second copy landed in
            // the text buffer as a literal CR that renders as a notdef box.
            if (ch < 0x20)
                return 0;
            std::string utf8 = wideToUtf8(&ch, 1);
            if (self->pendingKeyDown_.hasPendingChar) {
                self->pendingKeyDown_.text = utf8;
                self->pendingKeyDown_.hasPendingChar = false;
                push(self->pendingKeyDown_);
            } else {
                PlatformEvent ev;
                ev.type = EventType::KeyDown;
                ev.text = utf8;
                ev.modifiers = modifierFlags();
                push(ev);
            }
            return 0;
        }

        case WM_IME_COMPOSITION: {
            HIMC ctx = ImmGetContext(hwnd);
            if (!ctx) return 0;
            if (lParam & GCS_RESULTSTR) {
                // Composition committed — pull the final string.
                LONG n = ImmGetCompositionStringW(ctx, GCS_RESULTSTR, nullptr, 0);
                if (n > 0) {
                    std::wstring buf(static_cast<size_t>(n) / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(ctx, GCS_RESULTSTR, buf.data(), n);
                    self->imCommits_.push(wideToUtf8(buf.c_str(),
                        static_cast<int>(buf.size())));
                    self->imeEcho_ += buf;  // swallow the WM_CHAR echo below
                    self->imPreedit_.clear();
                    PlatformEvent ev;
                    ev.type = EventType::Preedit;
                    push(ev);
                }
            }
            if (lParam & GCS_COMPSTR) {
                // In-progress composition (preedit).
                LONG n = ImmGetCompositionStringW(ctx, GCS_COMPSTR, nullptr, 0);
                if (n > 0) {
                    std::wstring buf(static_cast<size_t>(n) / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(ctx, GCS_COMPSTR, buf.data(), n);
                    self->imPreedit_ = wideToUtf8(buf.c_str(),
                        static_cast<int>(buf.size()));
                } else {
                    self->imPreedit_.clear();
                }
                PlatformEvent ev;
                ev.type = EventType::Preedit;
                push(ev);
            }
            ImmReleaseContext(hwnd, ctx);
            return 0;
        }

        case WM_SETFOCUS:
        {
            PlatformEvent ev;
            ev.type = EventType::FocusIn;
            push(ev);
            // Associate the IME context with the focused window.
            HIMC ctx = ImmGetContext(hwnd);
            if (ctx) {
                COMPOSITIONFORM cf{};
                cf.dwStyle = CFS_POINT;
                cf.ptCurrentPos = self->imeCaretPos_;
                ImmSetCompositionWindow(ctx, &cf);
                ImmReleaseContext(hwnd, ctx);
            }
            return 0;
        }

        case WM_KILLFOCUS:
        {
            PlatformEvent ev;
            ev.type = EventType::FocusOut;
            push(ev);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            if (self->minWidth_ > 0 || self->minHeight_ > 0) {
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
                RECT rc = {0, 0, self->minWidth_, self->minHeight_};
                AdjustWindowRect(&rc, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), FALSE);
                mmi->ptMinTrackSize.x = rc.right - rc.left;
                mmi->ptMinTrackSize.y = rc.bottom - rc.top;
            }
            return 0;
        }

        default:
            if (msg == kWakeUpMsg)
                return 0;  // consumed — the event loop just wakes up
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace cui_core
