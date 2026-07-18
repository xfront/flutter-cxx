#pragma once
/// @file foundation/window.h
/// @brief AppWindow — one top-level window and its own render pipeline.
///
/// Historically Application owned exactly one PlatformWindow plus one
/// BuildOwner/PipelineOwner/RenderView/SkiaRenderer.  AppWindow is that bundle
/// extracted into a per-window object, so several top-level windows can run at
/// the same time:
///
///   Application (single event loop, task queue, thread pool)
///     ├── AppWindow #0 (PlatformWindow + Skia surface + widget tree)
///     ├── AppWindow #1 (…)
///     └── …
///
/// Each window keeps its own widget tree, focus manager, damage region and
/// input state; Application routes every platform event to the window it came
/// from.  The first window is the *primary* one and backs the legacy
/// single-window API (Application::setRoot / renderFrame / dispatchInput /
/// saveScreenshot …).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "geometry.h"

namespace cui {

class Application;
class BuildOwner;
class Element;
class PipelineOwner;
class PlatformWindow;
class RenderView;
class SchedulerBinding;
class SkiaRenderer;
class Widget;
struct PlatformEvent;

class AppWindow {
public:
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    // ── Widget tree ────────────────────────────────────────────────────────

    /// Mount (or replace) the widget tree rendered by this window.  The tree
    /// becomes the child of this window's RenderView; rebuilding and layout
    /// are driven by this window's own pipeline.
    void setRoot(SharedPtr<Widget> root);

    bool hasRoot() const { return hasRoot_; }
    Element* rootElement() const { return rootEl_; }

    // ── Rendering ──────────────────────────────────────────────────────────

    /// Full layout pass + paint for this window.  `forceFull` skips the damage
    /// optimisation (first frame / expose / resize).
    void renderFrame(bool forceFull = false);

    /// Queue a coalesced render; the Application's next frame pass renders
    /// every window that requested one.
    void scheduleRender();

    /// Encode this window's last rendered frame as a PNG file.
    bool saveScreenshot(const std::string& pngPath);

    // ── Native window ──────────────────────────────────────────────────────

    const std::string& title() const { return title_; }

    /// Change the native window title.
    void setTitle(const std::string& title);

    void setMinimumSize(int width, int height);
    void setBorderless(bool borderless);
    void show();

    /// Current drawable size.  Follows the native window when there is one
    /// (so it is authoritative even before a Resize event is dispatched).
    int width() const;
    int height() const;

    /// Close this window: destroys the native window immediately and marks
    /// the AppWindow as closed.  The Application drops it from its window
    /// list at a safe point (it stays owned until then — this may be called
    /// from inside the window's own event dispatch or its widget tree).
    /// The application quits once the last window is closed.
    void close();

    bool isOpen() const { return open_; }

    /// True when this window renders into an offscreen Skia surface (no
    /// native window; see Application::createOffscreenWindow).
    bool isOffscreen() const { return platformWindow_ == nullptr; }

    // ── Pipeline access (tests / tooling) ─────────────────────────────────

    Application& application() { return app_; }
    const Application& application() const { return app_; }
    PlatformWindow* platformWindow() const { return platformWindow_.get(); }
    SkiaRenderer* renderer() const { return renderer_.get(); }
    BuildOwner* buildOwner() const { return buildOwner_.get(); }
    PipelineOwner* pipelineOwner() const { return pipelineOwner_.get(); }
    RenderView* renderView() const { return renderView_.get(); }
    SchedulerBinding* schedulerBinding() const { return schedulerBinding_.get(); }

    /// Route a platform event into this window's pipeline as if it came from
    /// the native window (used by Application and by offscreen harnesses).
    void dispatchInput(const PlatformEvent& event);

private:
    friend class Application;

    AppWindow(Application& app, std::string title, int width, int height);

    /// Create the platform window + Skia surface.  Returns false when the
    /// platform window could not be created (the AppWindow is then not open).
    bool createNative(bool borderless);

    /// Create only the Skia surface (no native window).
    bool createOffscreen();

    void destroyNative();
    void applyResize(int width, int height);
    void recreateSkiaSurface(int width, int height);
    void paintFullFrame(int drawW, int drawH, bool forceFull);
    void blitToNativeWindow();

    Application& app_;
    std::string title_;

    std::unique_ptr<PlatformWindow> platformWindow_;
    int winWidth_ = 0;
    int winHeight_ = 0;
    bool borderless_ = false;

    std::unique_ptr<SkiaRenderer> renderer_;

    // Persistent buffers reused across frames by the blit path.
    std::vector<uint8_t> readbackBuf_;
    std::vector<uint8_t> blitScratch_;
    Rect presentDamage_{};
    bool presentDamageAll_ = true;

    Element* rootEl_ = nullptr;
    bool hasRoot_ = false;

    // buildOwner_ MUST be declared before pipelineOwner_/renderView_ and is
    // reset explicitly in the destructor: ~BuildOwner recursively unmounts the
    // element tree, which still needs the PipelineOwner and RenderView alive.
    std::unique_ptr<BuildOwner> buildOwner_;
    std::unique_ptr<SchedulerBinding> schedulerBinding_;
    std::unique_ptr<PipelineOwner> pipelineOwner_;
    std::unique_ptr<RenderView> renderView_;

    PointerButtons activeButtons_ = 0;
    bool rendering_ = false;
    bool open_ = false;
};

}  // namespace cui_core
