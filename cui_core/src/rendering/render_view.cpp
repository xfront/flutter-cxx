/// @file rendering/render_view.cpp
/// @brief RenderView — root render node implementation.

#include "cui/rendering/render_view.h"

#include <cassert>

#include <include/core/SkPictureRecorder.h>

#include "cui/rendering/painting_context.h"

namespace cui {

RenderView::RenderView() {
    // The root is always a relayout boundary — it doesn't depend on parent
    // constraints (it IS the constraint source).
    size_ = {800, 600};  // default window size
    windowSize_ = size_;
    layerTree_.setFrameSize(windowSize_);
}

RenderView::~RenderView() = default;

void RenderView::setWindowSize(const Size& s) {
    if (windowSize_.width == s.width && windowSize_.height == s.height)
        return;
    windowSize_ = s;
    layerTree_.setFrameSize(windowSize_);
    // Window resize changes our constraints — mark for relayout, and
    // invalidates every picture recorded in the layer tree.
    markNeedsLayout();
    markNeedsPaint();
}

void RenderView::setChild(RenderBox* child) {
    if (child_ == child)
        return;
    if (child_)
        dropChild(child_);
    child_ = child;
    if (child_)
        adoptChild(child_);
    markNeedsLayout();
}

void RenderView::insertChild(RenderObject* child, RenderObject* after) {
    (void)after;
    if (auto* box = dynamic_cast<RenderBox*>(child))
        setChild(box);
}

void RenderView::removeChild(RenderObject* child) {
    if (child && child_ == child)
        setChild(nullptr);
}

void RenderView::performLayout() {
    // The render view lays out its child with tight constraints matching
    // the window size.  Constraint-change detection handles the short-circuit:
    // if the child was already laid out with the same constraints and isn't
    // dirty, layout() returns immediately.
    size_ = windowSize_;
    if (child_) {
        child_->layout(BoxConstraints::tight(windowSize_.width, windowSize_.height),
                       true);
    }
}

void RenderView::performPaint(PaintingContext& context, const Offset& offset) {
    // Paint the child at the given offset.  The PaintingContext handles
    // pushing layers for compositing boundaries.
    if (child_) {
        context.paintChild(child_, offset);
    }
}

void RenderView::visitChildren(const std::function<void(AbstractNode*)>& visitor) {
    if (child_)
        visitor(child_);
}

void RenderView::adoptChild(AbstractNode* child) {
    AbstractNode::adoptChild(child);
    markNeedsLayout();
}

void RenderView::dropChild(AbstractNode* child) {
    AbstractNode::dropChild(child);
    markNeedsLayout();
}

void RenderView::paintTree() {
    // Rebuild the root layer's children through the PaintingContext.
    // Each repaint boundary child gets its own PictureLayer (created by
    // PaintingContext::paintChild), so the layer tree mirrors the render
    // tree's boundary structure instead of being a single monolithic picture.
    //
    // 复用：根 PictureLayer 跨帧保留（不 removeAllChildren），这样子边界
    // 的持久图层不被销毁，pushRepaintBoundary 才能正确复用（否则悬空指针
    // → dynamic_cast 失败 → 每帧新建层 → 图层树暴涨/崩溃）。
    //
    // 标记-清扫：全量重录会级联重建整棵图层树并为每个存活图层盖当前代戳
    // （见 PaintingContext 各 push 的 markLive）。切页 / 增删子树后被销毁 render
    // object 的图层不会被命中，级联后 pruneOrphans 递归删除它们，避免旧页图层
    // 作为孤儿被 composite → 内容重叠。
    Layer::advanceGeneration();
    auto* container = layerTree_.rootLayer();

    const double w = windowSize_.width;
    const double h = windowSize_.height;
    const Rect bounds{0, 0, w, h};

    // 直接绘制由 PaintingContext 分段录制：每推入一个图层（视口裁剪、透明度、
    // 重绘边界）就先结算当前段，新层排在它之后 ⇒ 后画的内容真的在上面
    // （浮层 / Overlay 不再被页面内容盖住）。第 0 段仍复用容器首个
    // PictureLayer，根 picture 的光栅缓存不受影响。
    PaintingContext context(container, nullptr, bounds);
    context.startDirectRecording(bounds);
    this->setLayer(context.directLayer(), {0, 0});  // 记下根图层，供复用
    performPaint(context, {});
    context.stopDirectRecording();

    // 级联已为所有存活图层盖戳，删除本代未命中的孤儿图层。
    container->pruneOrphans();
}

void RenderView::repaintBoundaryPaint() {
    // The root boundary owns the whole frame.
    paintTree();
}

void RenderView::composite(SkCanvas* canvas) {
    if (!canvas)
        return;
    // Rasterize the LayerTree onto the canvas.
    layerTree_.rasterize(canvas);
}

void RenderView::drawFrame(SkCanvas* canvas) {
    if (!canvas)
        return;

    // Full frame = paint phase (record into the LayerTree) followed by the
    // composite phase (rasterize).  The pipeline driver normally runs the
    // two phases separately (flushPaint → composite); this combined entry
    // remains for callers outside the flush cycle.
    paintTree();
    composite(canvas);
}

}  // namespace cui_core
