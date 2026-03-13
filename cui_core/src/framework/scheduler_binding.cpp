/// @file framework/scheduler_binding.cpp
/// @brief SchedulerBinding — frame driver implementation.

#include "cui/reactive/scheduler_binding.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <include/core/SkCanvas.h>

#include "cui/framework/build_owner.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"

namespace cui {

SchedulerBinding* SchedulerBinding::sInstance_ = nullptr;

SchedulerBinding::SchedulerBinding() {
    sInstance_ = this;
    lastFrameTime_ = std::chrono::steady_clock::now();
}

SchedulerBinding::~SchedulerBinding() {
    if (sInstance_ == this)
        sInstance_ = nullptr;
}

SchedulerBinding* SchedulerBinding::instance() {
    return sInstance_;
}

void SchedulerBinding::scheduleFrame() {
    frameScheduled_ = true;
}

bool SchedulerBinding::drawFrame(SkCanvas* canvas) {
    if (!frameScheduled_)
        return false;
    frameScheduled_ = false;

    // Phase 1: advance all active Tickers.
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<Duration>(now - lastFrameTime_);
    handleBeginFrame(elapsed);

    // Phase 2-5: flush + composite.
    handleDrawFrame(canvas);

    lastFrameTime_ = std::chrono::steady_clock::now();

    if (onFrameDrawn_)
        onFrameDrawn_();

    return true;
}

void SchedulerBinding::tickTickers() {
    // Consume the scheduled flag (same as drawFrame), advance the Tickers,
    // and update the elapsed timestamp — but leave the flush/composite
    // phases to renderFrame().
    frameScheduled_ = false;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<Duration>(now - lastFrameTime_);
    handleBeginFrame(elapsed);
    lastFrameTime_ = now;
}

void SchedulerBinding::handleBeginFrame(Duration timeStamp) {
    // Advance all active Tickers — this is the structural replacement for
    // "advance animation in paint()".  Tickers are driven BEFORE any layout
    // or paint, so animation state is fresh when the render tree is laid out
    // and painted.
    Ticker::tickAllActive(timeStamp);
}

void SchedulerBinding::handleDrawFrame(SkCanvas* canvas) {
    // 先跑 flush 管线得脏区，再全量合成（本入口保留给 drawFrame 等
    // 不参与脏区优化的调用方）。
    runPipelineAndComputeDamage();
    compositeTo(canvas);
}

SchedulerBinding::FrameDamage SchedulerBinding::runPipelineAndComputeDamage() {
    // 只跑 flush 管线，不碰目标 canvas。录制后的脏区由 PipelineOwner 累加，
    // 在此取出交给帧驱动决定 clear/composite/blit 范围（Flutter 帧驱动：
    // 无脏区即跳过整帧）。
    //
    // Phase 2: flushBuildScope — rebuild dirty Elements.
    if (buildOwner_)
        buildOwner_->flushBuildScope();

    // Phase 3: flushLayout — re-layout dirty RenderObjects.
    if (pipelineOwner_)
        pipelineOwner_->flushLayout();

    // Phase 4: flushCompositingBits — propagate needsCompositing up the tree.
    if (pipelineOwner_)
        pipelineOwner_->flushCompositingBits();

    // Phase 5: flushPaint — 仅重绘 needsPaint 脏链表里、各自最近
    // RepaintBoundary 的子树（增量重绘），并在 PipelineOwner 里累加本帧脏区。
    // 注意：这里**不再**每帧 renderView_->markNeedsPaint()，否则根边界
    // 每帧都脏 → 整帧重录（动画页 CPU 高的根因）。首次/结构变更时由
    // setWindowSize()/adoptChild 触发根标脏即可。
    if (pipelineOwner_)
        pipelineOwner_->flushPaint();

    FrameDamage d;
    if (pipelineOwner_) {
        d.all = pipelineOwner_->takeDamageAll();
        d.rect = pipelineOwner_->takeFrameDamage();
    } else {
        d.all = true;  // 无管线信息时保守全帧
    }
    return d;
}

void SchedulerBinding::compositeTo(SkCanvas* canvas) {
    // Composite: 把 LayerTree 里的缓存图 rasterize 到画布。调用方已根据
    // 脏区对 canvas 做了 clip（全帧时 clip 到整窗 = 无裁剪）。
    if (renderView_)
        renderView_->composite(canvas);
}

}  // namespace cui_core
