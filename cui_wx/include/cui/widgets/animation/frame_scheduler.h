#pragma once
/// @file widgets/animation/frame_scheduler.h
/// @brief FrameScheduler — drives the three-layer frame cycle.
///
/// The existing `SchedulerBinding` (`reactive/scheduler_binding.h`) only
/// flushes the build scope. This class pumps the
/// `BuildOwner` + `PipelineOwner`, ticking every active `Ticker` (which
/// advance `Animation`s) *before* the build/layout/paint flush. It is the
/// missing "animation driver" for the new layer: call `pump(dt)` once per
/// frame and all running animations advance and rebuild their subtrees.
///
/// Usage:
///   FrameScheduler fs(&buildOwner, &pipelineOwner);
///   fs.pump(std::chrono::milliseconds(16));   // one frame

#include <chrono>

#include "cui/framework/build_owner.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_object.h"

namespace cui {

class FrameScheduler {
public:
    FrameScheduler(BuildOwner* buildOwner, PipelineOwner* pipelineOwner)
        : buildOwner_(buildOwner), pipelineOwner_(pipelineOwner) {}

    /// Advance one frame by `dt`:
    ///   1. tick every active Ticker (animations emit progressChanged)
    ///   2. rebuild dirty Elements (animation listeners call setState)
    ///   3. flush layout / compositing bits / paint.
    ///   4. run post-frame callbacks (upstream SchedulerPhase.postFrameCallbacks).
    void pump(Duration dt) {
        // Phase 1: advance animations.  Tickers fire their callbacks, which
        // drive Animations forward and emit progressChanged — that is what
        // AnimatedBuilder listens to in order to mark itself dirty.
        Ticker::tickAllActive(dt);
        // Phase 2: rebuild any Element marked dirty by the animation listeners.
        if (buildOwner_)
            buildOwner_->flushBuildScope();
        // Phase 3: layout + composite + paint.
        if (pipelineOwner_) {
            pipelineOwner_->flushLayout();
            pipelineOwner_->flushCompositingBits();
            pipelineOwner_->flushPaint();
        }
        // Phase 4: post-frame callbacks (upstream SchedulerPhase
        // .postFrameCallbacks).  Anything scheduled during a build/layout runs
        // here, after the frame is on the canvas.
        if (buildOwner_)
            buildOwner_->flushPostFrameCallbacks(dt);
    }

    BuildOwner* buildOwner() const { return buildOwner_; }
    PipelineOwner* pipelineOwner() const { return pipelineOwner_; }

private:
    BuildOwner* buildOwner_;
    PipelineOwner* pipelineOwner_;
};

}  // namespace cui_core
