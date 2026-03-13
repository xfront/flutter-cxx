/// @file application.cpp
/// @brief Cross-platform Application — event loop, task queue and window list.

#include "cui/foundation/application.h"

#include "cui/foundation/painter.h"
#include "cui/foundation/window.h"
#include "cui/foundation/ticker.h"
#include "cui/platform/platform_window.h"
#include "cui/platform/services/method_channel.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/utils/task.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace cui {

Application* Application::sInstance_ = nullptr;

Application* Application::instance() { return sInstance_; }

namespace {

/// Backends that can only ever own one native surface (the host owns it):
/// a second createWindow() hands back the primary window instead of failing.
constexpr bool kSingleSurfaceBackend
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(__OHOS__)
        = true;
#elif defined(__APPLE__) && TARGET_OS_IOS
        = true;
#else
        = false;
#endif

/// True when native messages are queued thread-wide instead of per window,
/// so a wait on any one window can wake for a message addressed to another.
#if defined(_WIN32) || (defined(__APPLE__) && !TARGET_OS_IOS)
constexpr bool kThreadWideMessageQueue = true;
#else
constexpr bool kThreadWideMessageQueue = false;
#endif

}  // namespace

// 自适应帧限速：交互态（最近有指针/键盘输入）~60fps 保证跟手，静置
// 持续动画降到 ~30fps 削 CPU。CUI_FPS_INTERVAL_MS 强制固定间隔（调试）。
int Application::frameIntervalMs() const {
    static const int kForced = [] {
        const char* e = std::getenv("CUI_FPS_INTERVAL_MS");
        int n = e ? std::atoi(e) : 0;
        return n > 0 ? n : 0;
    }();
    if (kForced)
        return kForced;
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - lastInputTime_)
                           .count();
    const bool interacting = lastInputTime_ != std::chrono::steady_clock::time_point{} &&
                             since <= kActiveInputWindowMs;
    return interacting ? 16 : 33;
}

void scheduleTaskResume(std::coroutine_handle<> handle) {
    if (auto* app = Application::instance()) {
        app->postTask([h = handle]() mutable {
            if (h && !h.done())
                h.resume();
        });
        return;
    }
    // 没有事件循环（离屏渲染 / 单元测试）：投递出去也没人处理，协程会永远
    // 挂起。这里直接恢复，保证协程在无窗口环境下也能推进。
    if (handle && !handle.done())
        handle.resume();
}

Application::Application() : threadPool_(std::make_unique<ThreadPool>(4)) {
    sInstance_ = this;
#if defined(__linux__)
    // Prefer a CJK-capable font so Latin and Chinese text both render;
    // probe fontconfig and fall back to whatever actually resolves.
    for (const char* fam :
         {"Noto Sans CJK SC", "Noto Sans CJK JP", "WenQuanYi Micro Hei", "Droid Sans Fallback", "DejaVu Sans"}) {
        if (Painter::resolveTypeface(fam)) {
            defaultFontFamily_ = fam;
            break;
        }
    }
#elif defined(_WIN32)
    // Windows ships with Segoe UI (Latin) and several CJK faces.
    // Try a CJK-capable family first so Chinese text renders out of the box.
    for (const char* fam :
         {"Microsoft YaHei", "SimSun", "Segoe UI", "Arial"}) {
        if (Painter::resolveTypeface(fam)) {
            defaultFontFamily_ = fam;
            break;
        }
    }
#endif
}

Application::~Application() {
    if (threadPool_)
        threadPool_->shutdown();
    // 每个 AppWindow 的 ~AppWindow 先 reset 自己的 BuildOwner（元素树必须在
    // 该窗口的 PipelineOwner / RenderView 之前拆完），这里按 vector 顺序逐个
    // 销毁；sInstance_ 保持有效直到全部窗口拆完。
    windows_.clear();
    primaryWindow_ = nullptr;
    focusedWindow_ = nullptr;
    inputWindow_ = nullptr;
    sInstance_ = nullptr;
}

// ── windows ─────────────────────────────────────────────────────────────────

AppWindow* Application::createWindow(const std::string& title, int width, int height, bool borderless) {
    if (kSingleSurfaceBackend && !windows_.empty())
        return windows_.front().get();

    auto win = std::unique_ptr<AppWindow>(new AppWindow(*this, title, width, height));
    if (!win->createNative(borderless))
        return nullptr;

    AppWindow* raw = win.get();
    windows_.push_back(std::move(win));
    if (!primaryWindow_)
        primaryWindow_ = raw;
    if (!focusedWindow_)
        focusedWindow_ = raw;
    return raw;
}

AppWindow* Application::createOffscreenWindow(int width, int height) {
    auto win = std::unique_ptr<AppWindow>(new AppWindow(*this, std::string(), width, height));
    if (!win->createOffscreen())
        return nullptr;

    AppWindow* raw = win.get();
    windows_.push_back(std::move(win));
    if (!primaryWindow_)
        primaryWindow_ = raw;
    return raw;
}

AppWindow* Application::window() {
    if (!windows_.empty()) {
        if (!primaryWindow_)
            primaryWindow_ = windows_.front().get();
        return primaryWindow_;
    }
    // Headless host (tests / tooling): keep the legacy single-window API
    // working by lazily creating a zero-size offscreen window.
    return createOffscreenWindow(0, 0);
}

AppWindow* Application::windowAt(size_t index) const {
    return index < windows_.size() ? windows_[index].get() : nullptr;
}

void Application::closeWindow(AppWindow* window) {
    if (!window)
        return;
    window->close();
    if (openNativeWindowCount() == 0 && running_)
        quit();
    reapClosedWindows();
}

void Application::reapClosedWindows() {
    // Never destroy a window while its own input/tree callback may still be
    // running (a button that closes its window returns into dispatchInput).
    if (dispatchingEvents_ || inputWindow_)
        return;

    bool changed = false;
    for (auto it = windows_.begin(); it != windows_.end();) {
        AppWindow* w = it->get();
        if (w->isOpen() || w->rendering_) {
            ++it;
            continue;
        }
        if (primaryWindow_ == w)
            primaryWindow_ = nullptr;
        if (focusedWindow_ == w)
            focusedWindow_ = nullptr;
        it = windows_.erase(it);
        changed = true;
    }
    if (changed) {
        for (auto& w : windows_) {
            if (w->isOpen()) {
                primaryWindow_ = w.get();
                break;
            }
        }
    }
}

AppWindow* Application::firstOpenNativeWindow() const {
    for (auto& w : windows_) {
        if (w && w->isOpen() && w->platformWindow())
            return w.get();
    }
    return nullptr;
}

size_t Application::openNativeWindowCount() const {
    size_t n = 0;
    for (auto& w : windows_) {
        if (w && w->isOpen() && w->platformWindow())
            ++n;
    }
    return n;
}

// ── legacy single-window API (delegates to the primary window) ─────────────

void Application::setRoot(SharedPtr<Widget> root) {
    window()->setRoot(std::move(root));
}

void Application::dispatchInput(const PlatformEvent& event) {
    AppWindow* w = window();
    if (!w)
        return;
    AppWindow* prev = inputWindow_;
    inputWindow_ = w;
    w->dispatchInput(event);
    inputWindow_ = prev;
}

void Application::renderFrame(bool forceFull) {
    reapClosedWindows();
    // Render every open window: each owns its surface and damage region, and
    // a frame pass is the only place a window's pipeline is flushed.
    for (size_t i = 0; i < windows_.size(); ++i) {
        AppWindow* w = windows_[i].get();
        if (w && w->isOpen() && w->hasRoot())
            w->renderFrame(forceFull);
    }
}

bool Application::saveScreenshot(const std::string& pngPath) {
    return window()->saveScreenshot(pngPath);
}

void Application::setWindowTitle(const std::string& title) {
    AppWindow* w = primaryWindow_ ? primaryWindow_
                                  : (windows_.empty() ? nullptr : windows_.front().get());
    if (w)
        w->setTitle(title);
}

int Application::windowWidth() const {
    const AppWindow* w = primaryWindow_ ? primaryWindow_
                                        : (windows_.empty() ? nullptr : windows_.front().get());
    return w ? w->width() : 0;
}

int Application::windowHeight() const {
    const AppWindow* w = primaryWindow_ ? primaryWindow_
                                        : (windows_.empty() ? nullptr : windows_.front().get());
    return w ? w->height() : 0;
}

bool Application::clipboardSetText(const std::string& text) {
    AppWindow* w = focusedWindow_ && focusedWindow_->isOpen()
                       ? focusedWindow_
                       : (windows_.empty() ? nullptr : windows_.front().get());
    return w && w->platformWindow() && w->platformWindow()->clipboardSetText(text);
}

std::string Application::clipboardText() const {
    const AppWindow* w = focusedWindow_ && focusedWindow_->isOpen()
                             ? focusedWindow_
                             : (windows_.empty() ? nullptr : windows_.front().get());
    return w && w->platformWindow() ? w->platformWindow()->clipboardText() : std::string{};
}

// ── event loop ──────────────────────────────────────────────────────────────

void Application::beginEventLoop() {
    running_ = true;
    guiThreadId_ = std::this_thread::get_id();

    // 原生侧（Android JNI）从任意线程推来的通道消息先入队，再唤醒事件循环；
    // 实际派发在 Phase 1 的 pumpPlatformMessages()，与输入事件同一线程。
    services::setWakeUpHandler([this] {
        for (auto& w : windows_) {
            if (w && w->isOpen() && w->platformWindow())
                w->platformWindow()->wakeUp();
        }
        scheduleRender();
    });

    // Draw the first frame of every window up front — don't rely on the WM
    // sending an Expose event before the widget tree is attached.
    for (auto& w : windows_) {
        if (w && w->isOpen() && w->hasRoot())
            w->renderFrame(/*forceFull=*/true);
    }
}

void Application::endEventLoop() {
    services::setWakeUpHandler(nullptr);
    running_ = false;
}

int Application::run() {
    beginEventLoop();

#ifdef __EMSCRIPTEN__
    // Browsers cannot block the main thread, so the blocking loop below is
    // replaced by an Emscripten main loop (requestAnimationFrame, fps = 0).
    // runIteration() is non-blocking there: WebWindow::waitForEvent() only
    // polls, which is exactly one pass per animation frame.
    emscripten_set_main_loop_arg(&Application::emscriptenMainLoop, this, 0, 1);
    return 0;
#else
    while (running_) {
        if (!runIteration())
            break;
    }
    endEventLoop();
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
void Application::emscriptenMainLoop(void* arg) {
    auto* self = static_cast<Application*>(arg);
    if (!self->runIteration()) {
        emscripten_cancel_main_loop();
        self->endEventLoop();
    }
}
#endif

bool Application::runIteration() {
    if (!running_)
        return false;

    {
        // ── Phase 1: drain ALL pending events (every window) before rendering ──
        // Processing every queued motion event before painting avoids the
        // "render one frame per event" trap: when rendering is slower than
        // the mouse event rate (125 Hz+), the old one-at-a-time loop caused
        // the display to chase the cursor with visible lag.
        {
            // Round-robin over the windows: a thread-wide backend (Win32)
            // hands out messages for any window from the first poll, so the
            // sweep repeats until no window yields anything.
            bool hadEvents = drainWindowEvents();

            // 平台服务消息（权限结果、相机/蓝牙事件）与输入事件同线程派发。
            // 处理器里的 setState 会标脏并请求下一帧，本帧渲染自然带上。
            services::pumpPlatformMessages();

            // ── Phase 1.5: flush deferred render tasks ──
            // scheduleRender() defers postTask() while dispatchingEvents_ is
            // true (see scheduleRender).  Now that event dispatch is done,
            // flush the deferred render from a safe context.
            // Only post when no task is already queued (renderTaskQueued_):
            // the defer path in runScheduledFrame keeps renderScheduled_ true
            // for the next cycle but already re-posts its own task — without
            // this guard Phase 1.5 would post a duplicate every iteration,
            // causing a cascade of thousands of no-op calls per second.
            if (renderScheduled_.load() && !renderTaskQueued_.exchange(true))
                postTask([this]() { runScheduledFrame(); });

            // 动画驱动：有活动 Ticker 时请求下一帧。Ticker 自己不会请求帧，
            // 于是动画会停在原地 —— 只有输入事件才会让它再走一步
            // （「要点一下/滚一下才刷新」）。这里显式续帧，让动画连续推进。
            if (Ticker::hasActiveTickers())
                scheduleRender();

            // ── Phase 2: run all posted tasks (includes render) ──
            auto tasks = guiTaskQueue_.drain();
            renderDeferred_ = false;
            for (auto& task : tasks) {
                task();
            }
            // ── Phase 3: idle wait ──
            // We enter the wait whenever no events arrived this iteration —
            // even if a render task sits in guiTaskQueue_.  This is the key to
            // frame pacing: instead of busy-looping, we block in
            // waitForAnyWindowEvent(remaining frame period) and wake to paint
            // the next frame at ~60 fps.  Same-thread postTask deliberately
            // skips wakeUp(), so the block is not defeated by the render task
            // sitting in the queue.
            if (!hadEvents) {
                // A frame is pending while runScheduledFrame deferred (its
                // re-posted task is in the queue) or while a fresh frame was
                // scheduled.
                //
                // renderScheduled_ alone means "a frame was marked as still
                // needed but not yet rendered" — the queue may legitimately
                // be EMPTY here: runScheduledFrame clears both flags (below),
                // then tickTickers-driven scheduleRender() re-raises
                // renderScheduled_ and posts its follow-up task — which the
                // LAST queued run of this frame will consume and clear again
                // on its own defer path.  Gating on !queue.empty() here left
                // that state unattended: the loop fell into waitForEvent(-1)
                // and an active Ticker starved until some unrelated external
                // event woke the loop (~carousel autoplay) — the
                // "animation freezes intermittently" symptom (toast sliding
                // in snapped from p=0.001 straight to p=1.000).
                bool renderQueued = renderDeferred_ || renderScheduled_.load();
                // A bare postTask in the queue must also wake the loop:
                // task-driven chains (PageView's tick self-continuation,
                // runAsync onDone callbacks) starve whenever no animation
                // happens to be scheduling frames — same-thread postTask
                // deliberately skips wakeUp(), so this check is the only
                // thing that keeps the loop draining the queue while idle.
                const bool tasksPending = !guiTaskQueue_.empty();
                // 动画进行中（有活动 Ticker）也必须以帧周期限速等待，而非
                // 阻塞到 timeout=-1：否则循环会在最后一帧后卡死。这同时保证
                // 每轮迭代恰好渲染一次（不会因重投任务而渲染两次）。
                const bool animating = Ticker::hasActiveTickers();
                // Sleep only the REMAINDER of the frame period — the
                // vsync-less approximation of the upstream's scheduler tick —
                // instead of a fresh full interval on every retry.
                int timeout = -1;
                if (renderQueued || tasksPending || animating) {
                    const auto since =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - lastFrameStart_)
                            .count();
                    timeout = static_cast<int>(std::clamp<int64_t>(
                        frameIntervalMs() - since, 1, frameIntervalMs()));
                }

                PlatformEvent idleEvent;
                AppWindow* source = nullptr;
                if (waitForAnyWindowEvent(idleEvent, &source, timeout)) {
                    if (source && idleEvent.type != EventType::None) {
                        dispatchEvent(*source, idleEvent);
                        // Flush deferred render (see Phase 1.5 comment).
                        if (renderScheduled_.load() && !renderTaskQueued_.exchange(true))
                            postTask([this]() { runScheduledFrame(); });
                    }
                    return running_;  // drain follow-up events + tasks next pass
                }
                // Timed wait expired: fire the deferred render now so the
                // frame is not delayed another cycle behind the queue, and
                // drain any bare tasks that posted while we slept.
                if (renderQueued || tasksPending) {
                    auto deferred = guiTaskQueue_.drain();
                    if (!deferred.empty()) {
                        for (auto& t : deferred) {
                            t();
                        }
                    } else if (renderQueued) {
                        runScheduledFrame();  // fallback: scheduled but not queued
                        // Clear the flag so Phase 1.5 doesn't post a duplicate
                        // task in the next cycle (the direct call already ran).
                        renderScheduled_ = false;
                        renderTaskQueued_ = false;
                    }
                }
            }
        }
    }

    return running_;
}

bool Application::drainWindowEvents() {
    bool any = false;
    PlatformEvent event;
    bool progressed = true;
    while (progressed && running_) {
        progressed = false;
        for (size_t i = 0; i < windows_.size(); ++i) {
            AppWindow* w = windows_[i].get();
            if (!w || !w->isOpen() || !w->platformWindow())
                continue;
            // Re-read platformWindow() every turn: dispatching an event may
            // close this window (or open a new one behind it).
            while (w->platformWindow() && w->platformWindow()->pollEvent(event)) {
                progressed = true;
                any = true;
                if (event.type != EventType::None)
                    dispatchEvent(*w, event);
                if (!running_)
                    return true;
            }
        }
    }
    reapClosedWindows();
    return any;
}

bool Application::waitForAnyWindowEvent(PlatformEvent& event, AppWindow** source, int timeoutMs) {
    *source = nullptr;

    auto pollAll = [&]() -> AppWindow* {
        for (auto& w : windows_) {
            if (!w || !w->isOpen() || !w->platformWindow())
                continue;
            if (w->platformWindow()->pollEvent(event))
                return w.get();
        }
        return nullptr;
    };

    if (AppWindow* hit = pollAll()) {
        *source = hit;
        return true;
    }

    AppWindow* first = firstOpenNativeWindow();
    if (!first) {
        // Offscreen-only application: nothing external can wake the loop, so
        // pace it with a short sleep instead of blocking forever.
        if (timeoutMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return false;
    }

    const bool multiple = openNativeWindowCount() > 1;

    if (!multiple || kThreadWideMessageQueue) {
        // One wait serves every window here: Windows/macOS keep a single
        // thread-wide message queue, and a single window has one connection.
        if (first->platformWindow()->waitForEvent(event, timeoutMs)) {
            *source = first;
            return true;
        }
        // Thread-wide backends queue a message for window B while we waited
        // on window A — pick it up from B's own queue.
        if (multiple) {
            if (AppWindow* hit = pollAll()) {
                *source = hit;
                return true;
            }
        }
        return false;
    }

    // Per-connection backends (X11): waiting on one window's display never
    // wakes for another window, so every connection gets a short slice.
    // Even unbounded waits only make a single pass, so posted tasks keep
    // draining in between.
    constexpr int kSliceMs = 4;
    const int budget = timeoutMs < 0
                           ? kSliceMs * static_cast<int>(windows_.size())
                           : timeoutMs;
    int spent = 0;
    for (size_t i = 0; i < windows_.size() && spent < budget; ++i) {
        AppWindow* w = windows_[i].get();
        if (!w || !w->isOpen() || !w->platformWindow())
            continue;
        const int slice = std::max(1, std::min(kSliceMs, budget - spent));
        if (w->platformWindow()->waitForEvent(event, slice)) {
            *source = w;
            return true;
        }
        spent += slice;
    }
    return false;
}

void Application::quit() {
    running_ = false;
    for (auto& w : windows_) {
        if (w && w->platformWindow())
            w->platformWindow()->wakeUp();
    }
}

void Application::postTask(std::function<void()> task) {
    guiTaskQueue_.push(std::move(task));
    // Only wake the event loop if posting from a different thread.
    // Same-thread posts don't need wakeUp() — the event loop will process
    // the task in its next iteration. Calling wakeUp() from the same thread
    // sends a client message that immediately wakes the wait, preventing it
    // from blocking and causing high CPU usage.
    if (std::this_thread::get_id() != guiThreadId_) {
        for (auto& w : windows_) {
            if (w && w->platformWindow())
                w->platformWindow()->wakeUp();
        }
    }
}

void Application::runPendingGuiTasks() {
    for (auto& task : guiTaskQueue_.drain())
        task();
}

void Application::scheduleRender() {
    if (renderScheduled_.exchange(true))
        return;  // a frame is already queued — coalesce into it
    // When called from inside dispatchEvent() (e.g. a signal handler fired
    // by a widget during mouse-release processing), defer ALL side-effects
    // until event dispatch completes.  During dispatch the widget tree and
    // framework objects may be transiently inconsistent (popup stack, grab
    // state, signal chains in flight) — touching SchedulerBinding or the
    // task queue from here has caused crashes.  Phase 1.5 in run() picks
    // up the deferred frame via the renderScheduled_ flag we just set.
    if (dispatchingEvents_)
        return;
    // Frame bookkeeping lives on each window's SchedulerBinding; only the
    // primary drives the (process-wide) Ticker clock in runScheduledFrame.
    if (primaryWindow_ && primaryWindow_->schedulerBinding())
        primaryWindow_->schedulerBinding()->scheduleFrame();
    renderTaskQueued_ = true;
    postTask([this]() { runScheduledFrame(); });
}

void Application::runScheduledFrame() {
    // Clear THIS frame's scheduling marks BEFORE running it: any
    // scheduleRender() that fires while we run (a Ticker tick advancing an
    // animation, an event handler, …) then lands on a clean slate — its
    // CAS succeeds and the follow-up frame is queued behind this one.
    //
    // Clearing AFTER tickTickers() was a race that starved animations
    // intermittently: the tick's scheduleRender() hit the still-true
    // renderScheduled_ left by the previous frame's defer path, CAS-failed
    // (no follow-up task queued), and the unconditional clear below then
    // erased the pending-frame bookkeeping — the loop ended the iteration
    // with an active Ticker but an empty queue, clean flags, and therefore
    // timeout=-1 in Phase 3: the animation froze until some unrelated
    // external event woke the loop (toast slid in snapping p=0.001 → 1.000).
    renderScheduled_ = false;
    renderTaskQueued_ = false;

    // Drive Tickers (animation callbacks) before the render.  Tickers are a
    // process-wide list — tick them ONCE per frame no matter how many windows
    // are open; each window's pipeline then picks the new state up.
    if (primaryWindow_ && primaryWindow_->schedulerBinding())
        primaryWindow_->schedulerBinding()->tickTickers();
    else
        Ticker::tickAllActive(Duration(0));

    // 注意：本函数**不**在这里再次 scheduleRender()。下一帧由事件循环统一
    // 驱动——run() 的 Phase 244 在有活动 Ticker 时每轮 scheduleRender()，
    // Phase 3 以剩余帧周期（kMinFrameIntervalMs）限速等待，保证每个迭代
    // 恰好渲染一次。若在此（或 Ticker::tick 内）重投渲染任务，Phase 2 已
    // 渲染一次、Phase 3 超时后又把它再渲染一次 → 每轮两次 → CPU 翻倍。

    // 60 fps frame pacing.  Instead of busy-looping via self-re-post (which
    // floods the task queue and pins CPU at 100%), we simply mark the frame
    // as still needed and return.  Phase 3's timed wait naturally blocks the
    // thread until the next frame is due — the same principle as the
    // upstream's vsync-less pacing.
    //
    // The frame is NEVER dropped: Phase 244 (hasActiveTickers) re-raises
    // renderScheduled_ every iteration while an animation runs, so the next
    // iteration's Phase 1.5 posts the next frame.
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastFrameStart_).count();
    if (elapsedMs < frameIntervalMs() && lastFrameStart_ !=
            std::chrono::steady_clock::time_point{}) {
        // Too early — just mark that a frame is still needed.  Do NOT
        // postTask: that creates a busy loop.  Phase 3's timed wait will
        // fire when the frame period elapses.
        renderDeferred_ = true;
        renderScheduled_ = true;
        return;
    }

    lastFrameStart_ = now;

    // 上游-style frame dispatch: ALWAYS run the full frame via the
    // three-layer pipeline (PipelineOwner + RenderView + PaintingContext) —
    // for every window that has a tree mounted.
    reapClosedWindows();
    for (size_t i = 0; i < windows_.size(); ++i) {
        AppWindow* w = windows_[i].get();
        if (w && w->isOpen() && w->hasRoot())
            w->renderFrame();
    }
}

void Application::dispatchEvent(AppWindow& w, const PlatformEvent& event) {
    dispatchingEvents_ = true;
    switch (event.type) {
        case EventType::Close:
            // WM_CLOSE only asks; destroying the native window is ours to do.
            w.close();
            if (openNativeWindowCount() == 0 && running_)
                quit();
            break;

        case EventType::Resize:
            // applyResize() re-creates the surface, updates the RenderView
            // size and renders a full frame when the size actually changed.
            w.applyResize(event.width, event.height);
            break;

        case EventType::Expose:
            // 窗口内容可能被 X 服务器丢弃（首次映射/被遮挡后露出），必须整帧重绘。
            w.renderFrame(/*forceFull=*/true);
            break;

        case EventType::FocusIn:
            focusedWindow_ = &w;
            break;

        case EventType::FocusOut:
            if (focusedWindow_ == &w)
                focusedWindow_ = nullptr;
            break;

        default: {
            // Pointer / keyboard / IME events go to the window they came
            // from; every window has its own focus manager and hit-test
            // state.  记录输入时刻→frameIntervalMs() 在接下来一段窗口内维持
            // 60fps（跟手）。
            lastInputTime_ = std::chrono::steady_clock::now();
            AppWindow* prev = inputWindow_;
            inputWindow_ = &w;
            w.dispatchInput(event);
            inputWindow_ = prev;
            break;
        }
    }

    dispatchingEvents_ = false;
}

}  // namespace cui_core
