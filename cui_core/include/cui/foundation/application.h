#pragma once
/// @file application.h
/// @brief The Application — cross-platform event loop + Skia rendering.
///
/// The Application uses:
/// It uses:
///   - PlatformWindow: abstract native window (X11/Win32/Cocoa)
///   - Skia:           all rendering via SkCanvas/SkSurface
///
/// Architecture (上游-style three-layer pipeline):
///   ┌──────────────────────────────────────────────────────┐
///   │  Application                                         │
///   │  ┌────────────────┐  ┌─────────────────────────────┐ │
///   │  │ PlatformWindow │  │ Skia Rendering               │ │
///   │  │ (X11/Win32/…)  │  │ SkSurface → SkCanvas → paint │ │
///   │  └────────┬───────┘  └──────────────┬──────────────┘ │
///   │           │ events                  │ draw commands   │
///   │  ┌────────▼─────────────────────────▼──────────────┐ │
///   │  │  Three-layer pipeline (one full frame per tick):  │ │
///   │  │  SchedulerBinding → PipelineOwner::flushLayout    │ │
///   │  │  → flushCompositingBits → flushPaint              │ │
///   │  │  → RenderView::drawFrame (LayerTree → composite)  │ │
///   │  └─────────────────────────────────────────────────┘ │
///   │  ┌─────────────────────────────────────────────────┐ │
///   │  │  ThreadPool + postTask() + scheduleTaskResume()  │ │
///   │  └─────────────────────────────────────────────────┘ │
///   └──────────────────────────────────────────────────────┘

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cui/platform/event.h"
#include "cui/utils/lock_free_queue.h"
#include "cui/utils/thread_pool.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {

class Widget;
class Element;
class BuildOwner;
class AppWindow;

class Application {
public:
    Application();
    ~Application();

    // Non-copyable
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Create a top-level window (delegates to PlatformWindow) and register
    /// it with the event loop.
    /// @param borderless  Remove system title bar and decorations, making the
    ///                    entire window area available for self-drawing.
    /// @return the new window, or nullptr when the platform cannot open
    ///         another one (single-surface backends) or creation failed.
    ///
    /// The first window created becomes the *primary* window: the legacy
    /// single-window API (setRoot / renderFrame / dispatchInput / …) keeps
    /// operating on it.  Each window has its own widget tree, focus manager
    /// and render pipeline.
    AppWindow* createWindow(const std::string& title, int width, int height, bool borderless = false);

    /// Create a window that renders into an offscreen Skia surface and has no
    /// native window.  Useful for tests, tooling and screenshots of trees
    /// that are not currently on screen.  Always available, even on
    /// single-window platforms.
    AppWindow* createOffscreenWindow(int width, int height);

    /// The primary window (first created).  When the application was never
    /// given a native window (headless tests / tooling), a zero-size offscreen
    /// window is created lazily so the legacy API keeps working.
    AppWindow* window();

    size_t windowCount() const { return windows_.size(); }
    AppWindow* windowAt(size_t index) const;

    /// Close and destroy a window now (or defer to the event-loop pass when
    /// called from inside event dispatch / the window's own widget tree).
    /// Closing the last native window quits the application.
    ///
    /// @warning When the close happens immediately the pointer is invalidated;
    ///          observe the result through windowCount()/windowAt().  Called
    ///          from an input callback (the usual "close this window" button)
    ///          the destruction is deferred until dispatch returns, so the
    ///          handler may finish safely.
    void closeWindow(AppWindow* window);

    /// Mount a Widget tree directly on the single pipeline.  This is the
    /// native entry point: the widget's
    /// Element is adopted by the SchedulerBinding's BuildOwner, its root
    /// RenderBox becomes the RenderView child, and every frame flushes the
    /// build scope through the shared PipelineOwner — no nested RenderView.
    void setRoot(SharedPtr<Widget> root);

    /// Route a platform event (pointer / keyboard / IME) into the widget
    /// pipeline.  This is the framework's input entry point for the mounted
    /// root; it is also used by offscreen smoke harnesses to inject synthetic
    /// input without a native window.
    void dispatchInput(const PlatformEvent& event);

    /// Run the blocking event loop.
    ///
    /// On WebAssembly the browser cannot block a thread: this installs an
    /// Emscripten main loop and returns after the page is torn down instead.
    /// `runIteration()` is the shared, single-pass body both paths use.
    int run();

    /// Advance the event loop one pass (drain events, platform messages,
    /// tickers, posted tasks). Returns false once quit() was observed.
    bool runIteration();

    /// One-time setup / teardown shared by the native and Emscripten loops.
    ///
    /// `run()` 自己调这两条，所以常规宿主不需要碰。但自带主循环的宿主
    /// （Android：glue 线程上的 `android_main()` 自己轮转 `runIteration()`）必须
    /// 在循环前 begin、退出后 end —— `running_` 只在 begin 里置 true，否则
    /// `runIteration()` 第一拍就返回 false（循环立即退出，一帧都不出）；
    /// begin 还负责定框 GUI 线程 id 与平台通道的 wakeUp 钩子。
    void beginEventLoop();
    void endEventLoop();

    /// Request the event loop to exit.
    void quit();

    // ── Thread-safe task posting ───────────────────────────────────────────
    void postTask(std::function<void()> task);

    /// 执行并清空 GUI 任务队列（= run() 里的 Phase 2）。
    ///
    /// 正常情况下只有事件循环会排水；`runAsync()` 的 onDone、`scheduleRender()`
    /// 的续帧任务都排在 `guiTaskQueue_` 里。离屏冒烟脚本（examples/gallery.cpp
    /// 的 CUI_SEQ）不进 run()，于是跨线程投递的回调永远收不到 —— 这个入口就是给
    /// 它用的，让「点一下 → 后台干活 → 结果回 GUI 线程」能在无事件循环下验证。
    ///
    /// 只能在 GUI 线程调用（与事件循环的 Phase 2 同一约定）。
    void runPendingGuiTasks();

    /// True when the caller runs on the GUI (event loop) thread.
    ///
    /// 事件循环线程在 run() 启动后确定；在此之前（离屏 / 测试）一律视为
    /// GUI 线程。协程从后台线程恢复到 GUI 线程时用它做判定。
    bool isGuiThread() const {
        return guiThreadId_ == std::thread::id{} ||
               std::this_thread::get_id() == guiThreadId_;
    }

    /// True while the event loop is running.
    ///
    /// 事件循环未运行时（离屏渲染 / 单元测试），协程的恢复没有投递目标，
    /// 调用方据此决定是直接恢复还是投递给事件循环。
    bool isRunning() const { return running_; }

    /// Submit work to thread pool; onDone runs on GUI thread.
    template <typename Work, typename Callback>
    void runAsync(Work&& work, Callback&& onDone);

    ThreadPool& threadPool() { return *threadPool_; }

    // ── Rendering ──────────────────────────────────────────────────────────

    /// Full layout pass + paint. Called on expose / after state changes.
    ///
    /// `forceFull = true` 跳过脏区优化，无条件整屏 clear/composite/blit
    ///（首帧 / Expose / Resize 等窗口内容可能丢失的场景）。否则按本帧
    /// 实际脏区收敛：无视觉变化直接不出帧，脏区外的像素保留上一帧。
    void renderFrame(bool forceFull = false);

    /// Queue a coalesced renderFrame(): any number of repaint requests
    /// arriving before the next loop tick produce exactly ONE frame.
    void scheduleRender();

    /// Force immediate execution of a scheduled frame, bypassing the
    /// dispatchingEvents_ check. Used when a widget's size change must
    /// be reflected immediately (e.g., Sidebar collapse/expand).
    void runScheduledFrame();

    /// Encode the last rendered frame as a PNG file. Reads straight from the
    /// offscreen Skia surface, so it works even when the native window is
    /// occluded or headless — useful for tests and render verification.
    bool saveScreenshot(const std::string& pngPath);

    /// Get the singleton instance (for scheduleTaskResume).
    /// Out-of-line on purpose: an inline body would odr-use the private
    /// static member sInstance_ in every client TU, and MSVC cannot link
    /// cross-DLL references to static data members (WINDOWS_EXPORT_ALL_SYMBOLS
    /// does not cover them).  Callers just import this exported function.
    static Application* instance();

    /// Font family for all text (fontconfig name, e.g. "Noto Sans CJK SC").
    /// Must be set before the first frame is rendered.
    void setDefaultFontFamily(const std::string& family) { defaultFontFamily_ = family; }
    const std::string& defaultFontFamily() const { return defaultFontFamily_; }

    // ── Mouse wheel ──────────────────────────────────────────────────────
    /// 滚轮每格（notch）对应的滚动像素数。默认 56px/格。
    ///
    /// 平台层报告的滚轮增量单位不同（X11/Win32/Wayland 是格数，Web 是原生
    /// 像素），由这里统一换算成像素后再派发，Scrollable 等控件把滚轮增量
    /// 直接当位移使用。觉得滚得太慢/太快时调这个值即可。
    void setWheelPixelsPerNotch(double px) { wheelPixelsPerNotch_ = px; }
    double wheelPixelsPerNotch() const { return wheelPixelsPerNotch_; }

    /// Change the native window title of the primary window (e.g. after an
    /// i18n language switch).  For another window use
    /// `windowAt(i)->setTitle(...)`.
    void setWindowTitle(const std::string& title);

    int windowWidth() const;
    int windowHeight() const;

    // ── Clipboard (delegates to the platform window) ──────────────────────
    /// Copy UTF-8 text to the system clipboard. Returns false when no
    /// platform window exists or the backend cannot serve the clipboard.
    bool clipboardSetText(const std::string& text);
    /// Read UTF-8 text from the system clipboard (empty when unavailable).
    std::string clipboardText() const;

private:
    /// Route one platform event to the window that produced it.
    void dispatchEvent(AppWindow& window, const PlatformEvent& event);

    /// Poll every open window once (round-robin) and dispatch all queued
    /// events.  Returns true when at least one event was dispatched.
    bool drainWindowEvents();

    /// Wait for an event from ANY window, blocking up to `timeoutMs`
    /// (< 0 = until one arrives).  Thread-wide backends (Win32) wake on a
    /// message for any window; per-connection backends are polled round-robin
    /// with a short cap so every connection gets a turn.  Returns true with
    /// `source` set on success.
    bool waitForAnyWindowEvent(PlatformEvent& event, AppWindow** source, int timeoutMs);

    /// Drop AppWindows that were closed (AppWindow::close / WM_CLOSE).
    void reapClosedWindows();

    AppWindow* firstOpenNativeWindow() const;
    size_t openNativeWindowCount() const;

#ifdef __EMSCRIPTEN__
    /// emscripten_set_main_loop_arg trampoline (requestAnimationFrame).
    static void emscriptenMainLoop(void* arg);
#endif

    /// 自适应帧限速间隔（ms）。用户最近 kActiveInputWindowMs 内有输入
    /// （指针/键盘）→ 用交互间隔（~60fps，保证拖拽/点击跟手）；否则用
    /// 静置间隔（~30fps，削减持续动画的 CPU）。CUI_FPS_INTERVAL_MS 可强制
    /// 固定间隔（调试用）。无活动 Ticker 时仍为事件驱动（不空转）。
    int frameIntervalMs() const;

    // ── Windows ────────────────────────────────────────────────────────────
    // Each entry owns its native window, Skia surface, RenderView,
    // PipelineOwner and BuildOwner.  The vector is stable except when a
    // closed window is reaped (between event-loop phases, never mid-dispatch
    // of that same window).
    std::vector<std::unique_ptr<AppWindow>> windows_;
    AppWindow* primaryWindow_ = nullptr;   // == windows_.front() when set
    AppWindow* focusedWindow_ = nullptr;   // last window that saw FocusIn
    // Window currently receiving input; closed windows are never reaped (and
    // therefore never destroy their widget tree) while this is set.
    AppWindow* inputWindow_ = nullptr;

    // Default font family handed to every Painter (see setDefaultFontFamily).
    std::string defaultFontFamily_;

    // 滚轮每格对应的像素步长（见 setWheelPixelsPerNotch）。
    double wheelPixelsPerNotch_ = kDefaultWheelPixelsPerNotch;

    // ── Thread pool ────────────────────────────────────────────────────────
    std::unique_ptr<ThreadPool> threadPool_;

    // ── GUI task queue (lock-free — no mutex needed) ────────────────────────
    LockFreeQueue<std::function<void()>> guiTaskQueue_;
    std::thread::id guiThreadId_{};  // ID of the event loop thread

    // ── State ──────────────────────────────────────────────────────────────
    bool running_ = false;

    // True while a frame task sits in the GUI queue; cleared when the task
    // runs. Collapses repaint bursts into a single frame.
    std::atomic<bool> renderScheduled_{false};

    bool dispatchingEvents_ = false;  // true while inside dispatchEvent()

    // Start time of the previous frame, used by runScheduledFrame() to pace
    // frames to the frame-period floor.
    std::chrono::steady_clock::time_point lastFrameStart_{};

    // 最近一次用户输入（指针/键盘）的时刻，供 frameIntervalMs() 判定交互态。
    std::chrono::steady_clock::time_point lastInputTime_{};
    static constexpr int kActiveInputWindowMs = 400;  // 输入后维持高帧率的窗口

    // Frame-rate cap: ~60 fps.  Without pacing the event loop can run
    // thousands of blit + flush cycles per second.  XCB accumulates these in
    // an output buffer; when it fills, xcb_flush() blocks the GUI thread and
    // the window manager stops delivering events ("stall for 7-8 seconds").
    static constexpr int kMinFrameIntervalMs = 16;
    bool renderDeferred_ = false;  // render task fired too early, needs re-fire
    // True while a runScheduledFrame task actually sits in guiTaskQueue_.
    // Separated from renderScheduled_ ("render is needed") so that the
    // defer path can keep renderScheduled_ true for the next cycle without
    // Phase 1.5 posting a duplicate task each iteration.
    std::atomic<bool> renderTaskQueued_{false};

    static Application* sInstance_;
};

// ── Template implementation ────────────────────────────────────────────────

template <typename Work, typename Callback>
void Application::runAsync(Work&& work, Callback&& onDone) {
    using ResultType = std::invoke_result_t<Work>;
    auto* app = this;

    threadPool_->submit([app, w = std::forward<Work>(work), cb = std::forward<Callback>(onDone)]() mutable {
        if constexpr (std::is_void_v<ResultType>) {
            w();
            app->postTask([cb = std::move(cb)]() { cb(); });
        } else {
            auto result = w();
            app->postTask([cb = std::move(cb), result = std::move(result)]() mutable { cb(std::move(result)); });
        }
    });
}

}  // namespace cui_core
