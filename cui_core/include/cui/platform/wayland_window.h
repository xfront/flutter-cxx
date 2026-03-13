#pragma once
/// @file wayland_window.h
/// @brief Wayland implementation of PlatformWindow (modern Linux).

#include "platform_window.h"

#if defined(__linux__) && !defined(__ANDROID__) && !defined(__OHOS__)

#include <queue>
#include <string>
#include <vector>
#include <deque>

// Forward declarations for Wayland types
struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_shm;
struct wl_shm_pool;
struct wl_buffer;
struct wl_seat;
struct wl_pointer;
struct wl_keyboard;
struct wl_data_device;
struct wl_data_device_manager;
struct wl_data_source;
struct wl_data_offer;
struct wl_output;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct zwp_text_input_v3;
struct zwp_text_input_manager_v3;

namespace cui {

class WaylandWindow : public PlatformWindow {
public:
    WaylandWindow();
    ~WaylandWindow() override;

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
    double dpiScale() const override;
    NativeHandles nativeHandles() override;

    // Pointer grab
    void grabPointer() override;
    void ungrabPointer() override;

    // Clipboard
    bool clipboardSetText(const std::string& utf8) override;
    std::string clipboardText() const override;
    bool hasClipboardText() const override;

    // IME
    bool takeIMCommit(std::string& utf8) override;
    std::string imPreedit() const override;
    void setIMPreeditRect(int x, int y) override;

    // Internal methods called from static listeners
    void onPointerEnter(uint32_t serial, wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy);
    void onPointerLeave(uint32_t serial, wl_surface* surface);
    void onPointerMotion(uint32_t time, wl_fixed_t sx, wl_fixed_t sy);
    void onPointerButton(uint32_t serial, uint32_t time, uint32_t button, uint32_t state);
    void onPointerAxis(uint32_t time, uint32_t axis, wl_fixed_t value);
    void onPointerFrame();
    void onPointerAxisSource(uint32_t axis_source);
    void onPointerAxisStop(uint32_t time, uint32_t axis);
    void onPointerAxisDiscrete(uint32_t axis, int32_t discrete);

    void onKey(uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
    void onModifiers(uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
                     uint32_t mods_locked, uint32_t group);

    void onKeyboardEnter();
    void onKeyboardLeave();

    void onXdgConfigure(int32_t width, int32_t height);
    void onXdgClose();

    void onOutputScale(int32_t scale);

    void onDataOfferEnter(uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y,
                          struct wl_data_offer* offer);
    void onDataOfferLeave();
    void onDataOfferMotion(uint32_t time, wl_fixed_t x, wl_fixed_t y);
    void onDataOfferDrop();
    void onDataOfferAction(uint32_t dnd_action);

    // Clipboard callbacks
    void onDataOffer(struct wl_data_offer* offer);
    void onDataDeviceSelection(struct wl_data_offer* offer);
    void onDataOfferMime(struct wl_data_offer* offer, const char* mime_type);
    void onDataSourceSend(struct wl_data_source* source, const char* mime_type, int32_t fd);
    void onDataSourceCancelled(struct wl_data_source* source);

    // Text input callbacks
    void onTextInputCommit(const char* text);
    void onTextInputPreedit(const char* text, int cursor_begin, int cursor_end);
    void onTextInputPreeditEnd();

private:
    void initSeat();
    void initClipboard();
    void initTextInput();

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_surface* surface_ = nullptr;
    wl_shm* shm_ = nullptr;
    xdg_wm_base* xdgBase_ = nullptr;
    xdg_surface* xdgSurface_ = nullptr;
    xdg_toplevel* xdgToplevel_ = nullptr;

    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    wl_data_device* dataDevice_ = nullptr;
    wl_data_device_manager* dataDeviceManager_ = nullptr;
    wl_output* output_ = nullptr;

    // Text input (IME)
    zwp_text_input_manager_v3* textInputManager_ = nullptr;
    zwp_text_input_v3* textInput_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int minWidth_ = 0;
    int minHeight_ = 0;
    int scale_ = 1;

    std::string title_;
    std::queue<PlatformEvent> pendingEvents_;

    // Pointer state
    bool pointerInSurface_ = false;
    double pointerX_ = 0;
    double pointerY_ = 0;
    uint32_t pointerSerial_ = 0;
    bool pointerGrabbed_ = false;

    // Keyboard state
    uint32_t keyboardModifiers_ = 0;

    // Clipboard
    std::string clipboardText_;
    bool hasClipboard_ = false;
    struct wl_data_offer* clipboardOffer_ = nullptr;
    std::vector<std::string> clipboardMimes_;
    uint32_t selectionSerial_ = 0;

    // IME state
    std::deque<std::string> imCommits_;
    std::string imPreedit_;
    int imPreeditX_ = 0;
    int imPreeditY_ = 0;
};

}  // namespace cui_core

#endif  // __linux__
