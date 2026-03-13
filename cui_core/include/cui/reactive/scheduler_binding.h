#pragma once
/// @file scheduler_binding.h
/// @brief SchedulerBinding — the frame driver that orchestrates the 5-phase
///        frame cycle and drives Tickers.
///
/// The SchedulerBinding is the structural replacement for Application's
/// monolithic frame scheduling.  It:
///   1. Coalesces frame requests (scheduleFrame is idempotent).
///   2. Drives the 5-phase frame cycle:
///      - handleBeginFrame: advance all active Tickers
///      - flushBuildScope: BuildOwner.flushBuildScope()
///      - flushLayout: PipelineOwner.flushLayout()
///      - flushCompositingBits: PipelineOwner.flushCompositingBits()
///      - flushPaint: PipelineOwner.flushPaint()
///      - composite: RenderView.drawFrame(canvas)
///   3. Provides vsync-less frame pacing (16ms cap).
///
/// The SchedulerBinding does NOT own the PipelineOwner or RenderView — it
/// references them.  The Application (or a future Binding aggregate) wires
/// these references during initialization.

#include <chrono>
#include <functional>

#include "../foundation/geometry.h"
#include "cui/foundation/ticker.h"

class SkCanvas;

namespace cui {

class PipelineOwner;
class RenderView;
class BuildOwner;

class SchedulerBinding {
public:
    SchedulerBinding();
    ~SchedulerBinding();

    /// The BuildOwner this binding drives.  Must be set before drawFrame.
    BuildOwner* buildOwner() const { return buildOwner_; }
    void setBuildOwner(BuildOwner* bo) { buildOwner_ = bo; }

    /// The PipelineOwner this binding drives.  Must be set before drawFrame.
    PipelineOwner* pipelineOwner() const { return pipelineOwner_; }
    void setPipelineOwner(PipelineOwner* po) { pipelineOwner_ = po; }

    /// The RenderView this binding composites.  Must be set before drawFrame.
    RenderView* renderView() const { return renderView_; }
    void setRenderView(RenderView* rv) { renderView_ = rv; }

    /// Request a frame.  Idempotent: multiple calls before the frame runs
    /// produce exactly one frame.  The frame is executed by drawFrame().
    void scheduleFrame();

    /// True when a frame has been scheduled but not yet executed.
    bool frameScheduled() const { return frameScheduled_; }

    /// Execute one frame.  Runs the 5-phase cycle:
    ///   1. handleBeginFrame (tick Tickers)
    ///   2. handleDrawFrame (flush + composite)
    /// Returns false if no frame was scheduled (caller should not call).
    bool drawFrame(SkCanvas* canvas);

    /// Advance all active Tickers and consume the scheduled-frame flag, but
    /// do NOT flush/composite.  Application's frame runner calls this instead
    /// of drawFrame(): the flush and paint phases run inside renderFrame()
    /// (which also handles popups and the blit), so a full drawFrame() there
    /// would record and rasterize the LayerTree twice per frame.
    void tickTickers();

    /// Phase 1: advance all active Tickers.  Called at the start of each frame.
    void handleBeginFrame(Duration timeStamp);

    /// 一帧的脏区（damage region，上游 Rasterizer 的 frameDamage 语义）。
    /// `all` 为真表示整屏脏（根边界被重录 / 强制全帧）；否则仅 `rect`
    /// 窗口矩形内像素变化，`rect` 为空表示本帧无任何视觉变化（可跳过整帧）。
    struct FrameDamage {
        Rect rect{};
        bool all = false;
        bool any() const { return all || (rect.width > 0.0 && rect.height > 0.0); }
    };

    /// 只跑 flush 管线（build/layout/compositing/flushPaint），把脏边界录制进
    /// LayerTree，并返回本帧脏区——**不碰目标 canvas**。调用方据脏区决定
    /// clear / composite（clip 到脏区）/ blit 的范围。这是 Flutter 帧驱动：
    /// 无脏区即跳过整帧重绘。
    FrameDamage runPipelineAndComputeDamage();

    /// Phase 2-5: flush build/layout/compositing/paint, then composite.
    void handleDrawFrame(SkCanvas* canvas);

    /// Composite 阶段：把 LayerTree 光栅化到 canvas（不做脏区裁剪，全量）。
    /// runPipelineAndComputeDamage() 之后由调用方按需（含 clip）自行调用。
    void compositeTo(SkCanvas* canvas);

    /// Frame-rate cap: ~60 fps.  Matches the existing Application constant.
    static constexpr int kMinFrameIntervalMs = 16;

    /// Time since the last frame was drawn.  Used for vsync-less pacing.
    std::chrono::steady_clock::time_point lastFrameTime() const { return lastFrameTime_; }

    /// The frame callback — invoked after each frame is drawn.  Used by
    /// Application to update its frame pacing state.
    using FrameCallback = std::function<void()>;
    void setOnFrameDrawn(FrameCallback cb) { onFrameDrawn_ = std::move(cb); }

    /// Singleton access (for Ticker integration and global frame scheduling).
    static SchedulerBinding* instance();

private:
    BuildOwner* buildOwner_ = nullptr;
    PipelineOwner* pipelineOwner_ = nullptr;
    RenderView* renderView_ = nullptr;
    bool frameScheduled_ = false;
    std::chrono::steady_clock::time_point lastFrameTime_;
    FrameCallback onFrameDrawn_;

    static SchedulerBinding* sInstance_;
};

}  // namespace cui_core
