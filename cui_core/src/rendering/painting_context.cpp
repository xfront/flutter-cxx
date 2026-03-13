/// @file rendering/painting_context.cpp
/// @brief PaintingContext — builds the Layer tree during the paint phase.

#include "cui/rendering/painting_context.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkPicture.h>
#include <include/core/SkRRect.h>

#include "cui/rendering/render_object.h"

namespace cui {

PaintingContext::PaintingContext(ContainerLayer* containerLayer, SkCanvas* canvas,
                                 const Rect& estimatedBounds)
    : containerLayer_(containerLayer)
    , canvas_(canvas)
    , estimatedBounds_(estimatedBounds)
    , externalCanvas_(canvas != nullptr) {}

PaintingContext::~PaintingContext() {
    // 延迟落图：换段时 recorder 一律保活到这里。外层的 performPaint 可能还
    // 持有旧画布裸指针并在子树画完后 restore（见头文件 pendingSegments_），
    // 提前 finish+析构会让那一步踩到已释放的 SkCanvas。
    for (auto& seg : pendingSegments_) {
        if (seg.first && seg.second)
            seg.first->setPicture(seg.second->finishRecordingAsPicture());
    }
    pendingSegments_.clear();
}

// ── Child painting ──────────────────────────────────────────────────────────

void PaintingContext::paintChild(RenderObject* child, const Offset& offset) {
    if (!child)
        return;

    if (child->isRepaintBoundary()) {
        pushRepaintBoundary(child, currentOffset_ + offset);
    } else if (child->needsCompositing()) {
        // The child needs its own compositing layer (opacity, clip, transform).
        // Wrap in a TransformLayer — the compositing-bits flush will refine
        // this to push the correct layer type.
        pushTransform(currentOffset_ + offset, child, {});
    } else {
        paintDirect(child, offset);
    }
}

void PaintingContext::paintDirect(RenderObject* child, const Offset& offset) {
    if (!canvas_)
        return;

    SkCanvas* canvas = canvas_;
    const size_t epoch = directEpoch_;
    canvas->save();
    const SkMatrix entryMatrix = canvas->getTotalMatrix();
    canvas->translate(static_cast<SkScalar>(offset.dx),
                      static_cast<SkScalar>(offset.dy));
    const Offset saved = currentOffset_;
    currentOffset_ = currentOffset_ + offset;
    canvasOrigin_ = canvasOrigin_ + offset;
    child->paint(*this, {});
    currentOffset_ = saved;
    if (directEpoch_ == epoch) {
        canvas->restore();
        canvasOrigin_ = saved;
    } else if (canvas_) {
        // 子树里推过图层 ⇒ 画布已换成新录制段，外层这次 restore 无从执行。
        // 新画布直接恢复成进入本层时的完整 CTM（含旋转/缩放，不只是平移）。
        canvas_->setMatrix(entryMatrix);
        canvasOrigin_ = saved;
    }
}

// ── 画布作用域 ──────────────────────────────────────────────────────────────

PaintingContext::CanvasScope::CanvasScope(PaintingContext& context)
    : context_(&context), canvas_(context.canvas()) {
    if (!canvas_)
        return;
    active_ = true;
    before_ = canvas_->getTotalMatrix();
    canvas_->save();
    context_->scopes_.push_back(this);
}

PaintingContext::CanvasScope::~CanvasScope() {
    if (!active_)
        return;
    auto& scopes = context_->scopes_;
    for (size_t i = scopes.size(); i-- > 0;) {
        if (scopes[i] == this) {
            scopes.erase(scopes.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (context_->canvas() == canvas_) {
        canvas_->restore();
        return;
    }
    // 作用域内部换过画布：旧画布的 restore 落在退役画布上，新画布按剩余
    // 作用域重建（最外层退出时回到进入前的矩阵，裁剪随之消失）。
    SkCanvas* live = context_->canvas();
    if (!live)
        return;
    live->restoreToCount(1);
    if (!scopes.empty()) {
        live->setMatrix(scopes.front()->before_);
        for (auto* scope : scopes)
            scope->replay(live);
    } else {
        live->setMatrix(before_);
    }
}

void PaintingContext::CanvasScope::clipRect(const SkRect& rect, bool doAntiAlias) {
    if (!canvas_)
        return;
    canvas_->clipRect(rect, doAntiAlias);
    ops_.push_back([rect, doAntiAlias](SkCanvas* c) { c->clipRect(rect, doAntiAlias); });
}

void PaintingContext::CanvasScope::clipRRect(const SkRRect& rrect, bool doAntiAlias) {
    if (!canvas_)
        return;
    canvas_->clipRRect(rrect, doAntiAlias);
    ops_.push_back([rrect, doAntiAlias](SkCanvas* c) { c->clipRRect(rrect, doAntiAlias); });
}

void PaintingContext::CanvasScope::concat(const SkMatrix& matrix) {
    if (!canvas_)
        return;
    canvas_->concat(matrix);
    ops_.push_back([matrix](SkCanvas* c) { c->concat(matrix); });
}

void PaintingContext::CanvasScope::translate(double dx, double dy) {
    concat(SkMatrix::Translate(SkScalar(dx), SkScalar(dy)));
}

void PaintingContext::CanvasScope::replay(SkCanvas* canvas) const {
    canvas->save();
    canvas->setMatrix(before_);
    for (const auto& op : ops_)
        op(canvas);
}

// ── 直接绘制的录制分段 ──────────────────────────────────────────────────────

void PaintingContext::startDirectRecording(const Rect& bounds) {
    if (!containerLayer_ || directLayer_ || externalCanvas_)
        return;
    directBounds_ = bounds;

    // 容器首段复用已有的第 0 个 PictureLayer（跨帧保留光栅缓存）；后续段新建，
    // 由 RenderView::paintTree 末尾的 pruneOrphans 回收上一代的残留。
    PictureLayer* pl = nullptr;
    const auto& kids = containerLayer_->children();
    if (directSegments_ == 0 && !kids.empty())
        pl = dynamic_cast<PictureLayer*>(kids[0].get());
    if (!pl) {
        auto layer = std::make_unique<PictureLayer>();
        pl = layer.get();
        containerLayer_->appendChild(std::move(layer));
    }
    // 复用旧段也必须挪到末尾：本帧后续新建的段层一律追加在末尾，
    // 旧层停在前面的旧下标上就会排到它们下面（绘制顺序错乱）。
    containerLayer_->bringToFront(pl);
    pl->markLive();
    pl->setBounds(bounds);

    directRecorder_ = std::make_unique<SkPictureRecorder>();
    canvas_ = directRecorder_->beginRecording(SkRect::MakeXYWH(
        static_cast<SkScalar>(bounds.x), static_cast<SkScalar>(bounds.y),
        static_cast<SkScalar>(std::max(0.0, bounds.width)),
        static_cast<SkScalar>(std::max(0.0, bounds.height))));
    // 接上绘制遍历当前的画布状态：外层 paintDirect / 作用域（Transform 等）
    // 的 save·clip·transform 都落在上一段画布上，这一段必须从同样的状态接着
    // 画，否则换段之后的内容会整体错位（只补平移会丢掉旋转/裁剪，甚至把外层
    // Transform 的位移泄漏到作用域之外）。
    if (!scopes_.empty()) {
        canvas_->setMatrix(scopes_.front()->before_);
        for (auto* scope : scopes_)
            scope->replay(canvas_);
    } else if (hasSegmentMatrix_) {
        canvas_->setMatrix(segmentMatrix_);
    } else {
        canvas_->translate(static_cast<SkScalar>(currentOffset_.dx),
                           static_cast<SkScalar>(currentOffset_.dy));
    }
    canvasOrigin_ = currentOffset_;
    ++directEpoch_;
    directLayer_ = pl;
    isRecording_ = true;
    ++directSegments_;
}

void PaintingContext::stopDirectRecording() {
    if (!directLayer_)
        return;
    // 换段时不能立刻 finish：外层绘制帧可能还拿着这块画布（见
    // pendingSegments_ 注释）。记下当前完整 CTM，新段据此接续。
    segmentMatrix_ = canvas_->getTotalMatrix();
    hasSegmentMatrix_ = true;
    pendingSegments_.emplace_back(directLayer_, std::move(directRecorder_));
    directLayer_ = nullptr;
    canvas_ = nullptr;
    isRecording_ = false;
    ++directEpoch_;
}

// ── Layer push helpers ──────────────────────────────────────────────────────

void PaintingContext::recordChildInto(ContainerLayer* container,
                                      RenderObject* child, const Offset& offset) {
    if (!container || !child)
        return;
    const Rect pb = child->paintBounds();
    const Rect bounds{offset.dx + pb.x, offset.dy + pb.y, pb.width, pb.height};

    // 录制在 `container` 自己的空间里：startDirectRecording 会把画布平移到
    // currentOffset_（= offset），子节点按本地 (0,0) 绘制；cull rect 覆盖
    // offset+pb（与上游 push 层的 estimatedBounds 同口径）。
    PaintingContext childCtx(container, nullptr, estimatedBounds_);
    childCtx.currentOffset_ = offset;
    childCtx.startDirectRecording(bounds);
    child->paint(childCtx, {});
    childCtx.stopDirectRecording();
}

void PaintingContext::pushTransform(const Offset& offset, RenderObject* child,
                                    const Offset& childOffset) {
    if (!containerLayer_ || !child)
        return;
    // 先结算本段直接绘制，新层才会排在它上面（见头文件「直接绘制的录制分段」）。
    stopDirectRecording();
    // TransformLayer translates by `offset` at raster time, so the picture
    // under it is positioned at `childOffset` (NOT offset + childOffset —
    // the layer's own translation covers `offset`).
    TransformLayer* tl = dynamic_cast<TransformLayer*>(child->currentLayer());
    if (!tl) {
        auto layer = std::make_unique<TransformLayer>(offset);
        tl = layer.get();
        containerLayer_->appendChild(std::move(layer));
        child->setLayer(tl, childOffset);
    } else {
        tl->setOffset(offset);
        containerLayer_->bringToFront(tl);
    }
    tl->markLive();
    recordChildInto(tl, child, childOffset);
    startDirectRecording(directBounds_);
}

void PaintingContext::pushClip(const Rect& clipRect, RenderObject* child,
                               const Offset& childOffset) {
    if (!containerLayer_ || !child)
        return;
    stopDirectRecording();
    // The clip rect is interpreted in the container layer's space at raster
    // time; the child's picture is recorded positioned at childOffset and is
    // therefore clipped by it.
    ClipRectLayer* cl = dynamic_cast<ClipRectLayer*>(child->currentLayer());
    if (!cl) {
        auto layer = std::make_unique<ClipRectLayer>(clipRect);
        cl = layer.get();
        containerLayer_->appendChild(std::move(layer));
        child->setLayer(cl, childOffset);
    } else {
        cl->setClipRect(clipRect);
        containerLayer_->bringToFront(cl);
    }
    cl->markLive();
    recordChildInto(cl, child, childOffset);
    startDirectRecording(directBounds_);
}

void PaintingContext::pushOpacity(double alpha, RenderObject* child,
                                  const Offset& childOffset) {
    if (!containerLayer_ || !child)
        return;
    stopDirectRecording();
    OpacityLayer* ol = dynamic_cast<OpacityLayer*>(child->currentLayer());
    if (!ol) {
        auto layer = std::make_unique<OpacityLayer>(alpha);
        ol = layer.get();
        containerLayer_->appendChild(std::move(layer));
        child->setLayer(ol, childOffset);
    } else {
        ol->setAlpha(alpha);
        containerLayer_->bringToFront(ol);
    }
    ol->markLive();
    recordChildInto(ol, child, childOffset);
    startDirectRecording(directBounds_);
}

void PaintingContext::pushRepaintBoundary(RenderObject* child, const Offset& offset) {
    if (!containerLayer_ || !child)
        return;
    // 边界录进独立 picture：它拿到的坐标必须是「父画布当前的完整 CTM + 本地
    // 偏移」，只补平移会把外层 canvas 变换（RenderTransform 的位移/旋转）丢掉
    // ——Tooltip 气泡（Transform → CustomPaint 边界）会画回未变换的位置。
    // 必须在 stopDirectRecording 之前取，那一步会把 canvas_ 置空。
    const bool hasParentCanvas = canvas_ != nullptr;
    const SkMatrix parentMatrix = hasParentCanvas ? canvas_->getTotalMatrix() : SkMatrix::I();
    // 同 pushClip/pushTransform：先把此前的直接绘制结算成一层，边界层才排在它上面。
    stopDirectRecording();
    const Rect pb = child->paintBounds();

    // 复用已有图层（增量重绘）：boundary 首次绘制时把图层存入 child->layer()，
    // 之后 flushPaint 直接复用同一 PictureLayer，仅替换其 SkPicture。
    PictureLayer* pl = dynamic_cast<PictureLayer*>(child->currentLayer());
    const bool hadLayer = pl != nullptr;
    // 这份缓存图当初是用哪个坐标帧录的。本仓把「窗口坐标」直接烘进 picture
    // （不同于上游 OffsetLayer 只在合成时加 offset），所以矩阵一变，旧图就不
    // 再属于当前位置 —— 见下面 moved 的判定。
    const SkMatrix prevRecordMatrix = hadLayer ? pl->recordingMatrix() : SkMatrix::I();
    if (!hadLayer) {
        auto pictureLayer = std::make_unique<PictureLayer>();
        pl = pictureLayer.get();
        containerLayer_->appendChild(std::move(pictureLayer));
    } else {
        containerLayer_->bringToFront(pl);
    }
    pl->markLive();
    pl->setBounds({offset.dx + pb.x, offset.dy + pb.y, pb.width, pb.height});
    // 每次都刷新 layerOffset_（含复用路径）：边界在容器空间的绝对位置会随
    // 滚动/重排变化。脏区矩形（flushPaint）按 layerOffset_ + paintBounds 计算；
    // 录制坐标帧则由下面的 recordingMatrix 一起刷新（增量重录用它复现位置）。
    // 若只在首次创建时写入，滚动后便停留在旧位置。
    child->setLayer(pl, offset);

    // 录制坐标帧：父画布完整 CTM + 本地偏移。存到图层上后，repaintBoundaryPaint
    // 单录该边界时用同一矩阵，增量重绘才不会丢祖先变换（与整帧重录一致）。
    SkMatrix recordMatrix =
        hasParentCanvas ? parentMatrix : SkMatrix::I();
    if (hasParentCanvas) {
        const Offset local = offset - currentOffset_;
        recordMatrix.postTranslate(static_cast<SkScalar>(local.dx),
                                   static_cast<SkScalar>(local.dy));
    } else {
        recordMatrix.postTranslate(static_cast<SkScalar>(offset.dx),
                                   static_cast<SkScalar>(offset.dy));
    }
    pl->setRecordingMatrix(recordMatrix);

    // 干净即复用（上游 pushPaintingBoundary 的核心收益）：本边界子树这一帧
    // 没被标脏、且已有缓存图 ⇒ 只挂层不重录。判定用 `child->needsPaint()`
    // ——markNeedsPaint 的上抛在最近的 repaint boundary 停住并把它入队，所以
    // 「子树脏」必然表现为「边界自身脏」；flushPaint 清标志的时机见那里的
    // 注释（必须在重录之后清，否则祖先整帧重录时会把脏边界当干净复用 ⇒ 旧像素）。
    //
    // 但「干净」只说明内容没变，不说明位置没变：祖先（视口）滚动时边界自身不脏，
    // 而它的窗口坐标整体平移了。此时复用旧图 = 内容钉在滚动前的位置，与移动后的
    // 兄弟内容重叠（Custom Widget 页向下滚动，仪表盘/波形条/胶囊留在原地，说明
    // 文字压上去）。上游靠 OffsetLayer 天然免疫，本仓必须按坐标帧变化重录。
    const bool moved = hadLayer && !(prevRecordMatrix == recordMatrix);
    if (hadLayer && pl->picture() && !child->needsPaint() && !moved) {
        startDirectRecording(directBounds_);
        return;
    }

    // Record the child's subtree; its local (0,0) maps to `offset`.  The
    // cull rect is the recording canvas's initial clip, so it must cover the
    // content's final position (offset + paint bounds) — a pb-only cull
    // would truncate the picture to whatever overlaps it, exactly like
    // recordChildInto's offset-aware cull.
    SkPictureRecorder recorder;
    // Clamp negative dimensions to prevent Skia crash
    const double safeWidth = std::max(0.0, pb.width);
    const double safeHeight = std::max(0.0, pb.height);
    SkCanvas* recordCanvas = recorder.beginRecording(SkRect::MakeXYWH(
        static_cast<SkScalar>(offset.dx + pb.x),
        static_cast<SkScalar>(offset.dy + pb.y),
        static_cast<SkScalar>(safeWidth), static_cast<SkScalar>(safeHeight)));

    PaintingContext childCtx(containerLayer_, recordCanvas, estimatedBounds_);
    recordCanvas->setMatrix(recordMatrix);
    childCtx.currentOffset_ = offset;
    child->paint(childCtx, {});

    pl->setPicture(recorder.finishRecordingAsPicture());
    startDirectRecording(directBounds_);
}

// ── Recording mode ──────────────────────────────────────────────────────────

sk_sp<SkPicture> PaintingContext::finishRecording() {
    if (!recorder_ || !isRecording_)
        return nullptr;
    isRecording_ = false;
    sk_sp<SkPicture> picture = recorder_->finishRecordingAsPicture();
    recorder_.reset();
    return picture;
}

std::unique_ptr<PaintingContext> PaintingContext::createRecordingContext(
    ContainerLayer* childLayer, const Rect& bounds) {
    auto recorder = std::make_unique<SkPictureRecorder>();
    // Clamp negative dimensions to prevent Skia crash
    const double safeWidth = std::max(0.0, bounds.width);
    const double safeHeight = std::max(0.0, bounds.height);
    SkCanvas* recordCanvas = recorder->beginRecording(
        SkRect::MakeXYWH(
            static_cast<SkScalar>(bounds.x),
            static_cast<SkScalar>(bounds.y),
            static_cast<SkScalar>(safeWidth),
            static_cast<SkScalar>(safeHeight)));

    auto ctx = std::make_unique<PaintingContext>(childLayer, recordCanvas, bounds);
    ctx->recorder_ = std::move(recorder);
    ctx->isRecording_ = true;
    return ctx;
}

}  // namespace cui_core
