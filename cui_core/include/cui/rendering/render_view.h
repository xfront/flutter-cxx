#pragma once
/// @file rendering/render_view.h
/// @brief RenderView — the root of the render tree.
///
/// RenderView is the single entry point into the render tree from the
/// framework/platform layer.  It:
///   - Holds the LayerTree that the paint phase builds and the composite
///     phase rasterizes.
///   - Stores the current window size (from the PlatformWindow).
///   - Lays out its single child with tight constraints matching the window.
///   - Provides `composite()` to rasterize the LayerTree onto an SkCanvas.

#include <include/core/SkCanvas.h>

#include "cui/foundation/layer.h"
#include "cui/rendering/render_box.h"

namespace cui {

class PipelineOwner;

/// The root of the render tree.  Exactly one RenderView exists per window.
class RenderView : public RenderBox {
public:
    RenderView();
    ~RenderView() override;

    /// The LayerTree this view paints into.  The paint phase populates it;
    /// the composite phase rasterizes it.
    LayerTree& layerTree() { return layerTree_; }
    const LayerTree& layerTree() const { return layerTree_; }

    /// The current window size.  Updated by the platform layer on resize.
    const Size& windowSize() const { return windowSize_; }
    void setWindowSize(const Size& s);

    /// The single child of the render view (the root widget's render object).
    RenderBox* child() const { return child_; }
    void setChild(RenderBox* child);

    // ── RenderObject overrides ────────────────────────────────────────────
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

    void visitChildren(const std::function<void(AbstractNode*)>& visitor) override;
    void adoptChild(AbstractNode* child) override;
    void dropChild(AbstractNode* child) override;

    /// Element 层的子节点协议（上游的 `RenderObjectWithChildMixin`）。根控件
    /// 更换渲染类型时由 `RenderObjectElement` 的挂载/卸载直接调用，保证
    /// `child_` 始终指向当前根，不会悬空。
    void insertChild(RenderObject* child, RenderObject* after) override;
    void removeChild(RenderObject* child) override;

    bool isRelayoutBoundary() const override { return true; }

    /// The root of the render tree is the last-resort repaint boundary: any
    /// markNeedsPaint that finds no dedicated boundary below the root lands
    /// here and re-records the whole frame.
    bool isRepaintBoundary() const override { return true; }

    /// RenderObject overrides: re-record the entire frame into the LayerTree.
    void repaintBoundaryPaint() override;

    /// Re-record the whole widget paint chain into a fresh PictureLayer under
    /// the root layer (the paint phase).  Does NOT rasterize — call
    /// composite() afterwards.
    void paintTree();

    /// Rasterize the LayerTree onto `canvas`.  Called by the composite phase
    /// after flushPaint has populated the layer tree.
    void composite(SkCanvas* canvas);

    /// Full frame: build the LayerTree by painting the child via
    /// PaintingContext, then rasterize onto `canvas`.  This is the entry
    /// point called by the frame driver (SchedulerBinding / Application).
    void drawFrame(SkCanvas* canvas);

private:
    LayerTree layerTree_;
    Size windowSize_;
    RenderBox* child_ = nullptr;
};

}  // namespace cui_core
