#pragma once
/// @file rendering/painting_context.h
/// @brief PaintingContext — builds the Layer tree during the paint phase.
///
/// PaintingContext is the bridge between the render tree's performPaint()
/// hooks and the persistent Layer tree that the composite phase rasterizes.
/// It operates in two modes:
///
///   1. **Direct mode**: painting onto a real SkCanvas.  Children that don't
///      need compositing are painted inline (canvas save/translate/restore).
///
///   2. **Recording mode**: painting into an SkPictureRecorder.  Used by
///      RepaintBoundary to bake a subtree into an SkPicture that is wrapped
///      in a PictureLayer and replayed cheaply on subsequent frames.
///
/// The context manages a ContainerLayer* — the "current scope" — and appends
/// child layers (TransformLayer, ClipRectLayer, OpacityLayer, PictureLayer)
/// to it as the paint walk encounters compositing boundaries.

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPictureRecorder.h>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "../foundation/geometry.h"
#include "cui/foundation/layer.h"

namespace cui {

class RenderObject;

class PaintingContext {
public:
    /// Root context: paints onto a real canvas, building into `containerLayer`.
    /// `estimatedBounds` is the cull rect for the recording (used only in
    /// recording mode; direct mode ignores it).
    PaintingContext(ContainerLayer* containerLayer, SkCanvas* canvas,
                    const Rect& estimatedBounds);

    /// 结算所有延迟落图的录制段（见 stopDirectRecording 的保活注释）。
    ~PaintingContext();

    /// The canvas to paint on.  In direct mode this is the real SkCanvas;
    /// in recording mode it is the SkPictureRecorder's canvas.
    SkCanvas* canvas() const { return canvas_; }

    /// The current layer scope — child layers are appended here.
    ContainerLayer* containerLayer() const { return containerLayer_; }

    /// The estimated paint bounds (cull rect for recording).
    const Rect& estimatedBounds() const { return estimatedBounds_; }

    /// Accumulated translation of the recording canvas — i.e. where this
    /// context's local origin sits in the CONTAINER LAYER's coordinate
    /// space.  Helpers that emit layers (pushClip/pushOpacity/...) take
    /// positions in that space; callers convert with `currentOffset() +
    /// localValue`.
    const Offset& currentOffset() const { return currentOffset_; }

    // ── Child painting ────────────────────────────────────────────────────

    /// Paint a child render object at `offset` (in this context's coordinate
    /// space).  If the child needs compositing (RepaintBoundary, opacity,
    /// clip, transform), pushes the appropriate layer; otherwise paints
    /// directly onto the current canvas with a save/translate/restore.
    void paintChild(RenderObject* child, const Offset& offset);

    // ── Layer push helpers ────────────────────────────────────────────────
    // All three wrap the child's output into a fresh PictureLayer recorded
    // under the pushed layer, so the clip/transform/opacity actually applies
    // at raster time (a picture painted directly onto the base canvas would
    // bypass them).

    /// Push a ClipRectLayer at `clipRect` (in the container layer's space),
    /// record `child` into it, and append both to the current container.
    void pushClip(const Rect& clipRect, RenderObject* child,
                  const Offset& childOffset);

    /// Push an OpacityLayer multiplying `child`'s alpha by `alpha`.
    void pushOpacity(double alpha, RenderObject* child,
                     const Offset& childOffset);

    /// Push a TransformLayer translating by `offset`, record `child` into it
    /// (positioned at `childOffset` inside the transform), and append.
    void pushTransform(const Offset& offset, RenderObject* child,
                       const Offset& childOffset);

    /// Push a PictureLayer for a RepaintBoundary.  Records the child's
    /// performPaint into an SkPicture, wraps it in a PictureLayer, and
    /// appends it to the current container.  `offset` is the boundary's
    /// origin in the container layer's space.
    void pushRepaintBoundary(RenderObject* child, const Offset& offset);

    // ── 直接绘制的录制分段 ────────────────────────────────────────────────
    // 上游 PaintingContext 的 `stopRecordingIfNeeded()` + `appendLayer()`：
    // 每推入一个图层（clip / transform / opacity / 重绘边界）之前，先把已经
    // 直接录进当前 picture 的内容**结算成一个 PictureLayer 追加到容器**，新层
    // 排在它之后，随后再开一段新的录制。于是「先画的在下面」在图层树里成立。
    // 少了这一步，直接绘制的内容永远落在容器第 0 个 PictureLayer 里，而视口
    // 的 ClipRectLayer 排在其后 ⇒ 浮层（Overlay）内容被页面内容盖住。

    /// 开始一段直接绘制录制：内容录进一个 PictureLayer（追加到
    /// `containerLayer_` 末尾）。容器首段复用容器已有的第 0 个子层，保住根
    /// picture 的光栅缓存；后续段每次新建（全量重录末尾由 pruneOrphans 清扫）。
    void startDirectRecording(const Rect& bounds);

    /// 结束当前录制段：把录到的内容落到该段的 PictureLayer 上（`canvas()`
    /// 随即为空，直到再次 start）。
    void stopDirectRecording();

    /// 当前录制段所属的 PictureLayer（未在录制时为 null）。
    PictureLayer* directLayer() const { return directLayer_; }

    // ── 画布作用域（save + clip / transform 包住子树绘制）───────────────────
    // 子树里推入图层会「换段」：stopDirectRecording 结束当前录制画布，之后在
    // 新画布上继续。包住这段子树的外层作用域仍持有旧画布，它们的 restore()
    // 落在退役画布上 ⇒ 新画布会带着该作用域的矩阵/裁剪跑到作用域结束之后
    // （Tooltip：气泡推层后，外层 Transform 的 -34 平移泄漏到后续所有兄弟
    // 节点，整页内容上移 34px）。CanvasScope 因此由 context 记录：换段时在
    // 新画布上重放「当前活动的作用域」，作用域结束（含换过画布的情况）时再
    // 按剩余作用域重建一次。
    class CanvasScope {
    public:
        explicit CanvasScope(PaintingContext& context);
        ~CanvasScope();
        CanvasScope(const CanvasScope&) = delete;
        CanvasScope& operator=(const CanvasScope&) = delete;

        void clipRect(const SkRect& rect, bool doAntiAlias);
        void clipRRect(const SkRRect& rrect, bool doAntiAlias);
        /// 叠加一个变换（等价于 canvas->concat）。
        void concat(const SkMatrix& matrix);
        void translate(double dx, double dy);

        /// 构造时拿不到画布 ⇒ 作用域无效，调用方退回代理盒默认绘制。
        bool active() const { return active_; }
        SkCanvas* canvas() const { return canvas_; }

    private:
        friend class PaintingContext;
        void replay(SkCanvas* canvas) const;

        PaintingContext* context_;
        SkCanvas* canvas_;
        /// 进入作用域时的完整 CTM（换段重放的基准）。
        SkMatrix before_;
        /// 作用域内施加过的操作，按顺序重放（裁剪 + 变换）。
        std::vector<std::function<void(SkCanvas*)>> ops_;
        bool active_ = false;
    };

    // ── Recording mode (for RepaintBoundary) ──────────────────────────────

    /// True when this context is recording into an SkPictureRecorder.
    bool isRecording() const { return isRecording_; }

    /// Stop recording and return the recorded picture.  Only valid when
    /// isRecording() is true.
    sk_sp<SkPicture> finishRecording();

private:
    friend class RenderObject;
    /// Paint a single render object directly onto the canvas (save/translate/
    /// restore).  Used for non-compositing children.
    void paintDirect(RenderObject* child, const Offset& offset);

    /// Record `child`'s output into a fresh PictureLayer appended to
    /// `container`, positioned at `offset` (in `container`'s space).  The
    /// workhorse behind the push helpers.
    void recordChildInto(ContainerLayer* container, RenderObject* child,
                         const Offset& offset);

    /// Create a child PaintingContext that paints into `childLayer` using
    /// a recording canvas.  Returns the child context.
    std::unique_ptr<PaintingContext> createRecordingContext(
        ContainerLayer* childLayer, const Rect& bounds);

    ContainerLayer* containerLayer_;
    SkCanvas* canvas_;
    Rect estimatedBounds_;

    /// Where this context's local origin sits in the container layer's
    /// coordinate space (see currentOffset()).
    Offset currentOffset_;

    // Recording mode state (for RepaintBoundary).
    std::unique_ptr<SkPictureRecorder> recorder_;
    bool isRecording_ = false;

    // 直接绘制的录制段状态（见上面的「直接绘制的录制分段」）。
    std::unique_ptr<SkPictureRecorder> directRecorder_;
    PictureLayer* directLayer_ = nullptr;
    Rect directBounds_;
    size_t directSegments_ = 0;
    /// `canvas_` 里已经烘进去的平移量。分段录制换了画布之后，绘制遍历外层那些
    /// `save()/translate()` 就落在了旧画布上，所以新画布必须自己接上同样的
    /// CTM；paintDirect 结束时按这个值补偿，保证「画布 CTM == currentOffset_」
    /// 这条不变量在任何时刻成立。
    Offset canvasOrigin_;
    /// 录制画布的「代」：每次换画布（分段录制起停）自增。**不能用画布指针
    /// 判断画布是否被换过** —— 旧画布随 recorder 析构后，新画布常常落在同一
    /// 个堆地址上，指针比较会误判成「没换」，于是补偿不执行，新画布保留着
    /// 上一段残留的 CTM 基准（兄弟节点被整体画偏，浮层跑到页面中间）。
    size_t directEpoch_ = 0;
    /// 换段时保活的旧 recorder：外层 performPaint 可能已经拿了旧画布的裸
    /// 指针（`save()/translate()/restore()`，例如 RenderTransform），而换段
    /// 发生在其子树里（子节点是重绘边界 ⇒ pushRepaintBoundary）。若 stop 时
    /// 立刻 finish+析构 recorder，外层的 restore 就会落到已释放的 SkCanvas 上
    /// （Tooltip 气泡 `Transform → CustomPaint` 悬停即崩）。故延迟到本上下文
    /// 结束再统一 finish 落图。
    std::vector<std::pair<PictureLayer*, std::unique_ptr<SkPictureRecorder>>>
        pendingSegments_;
    /// 换段那一刻旧画布的完整 CTM：新段必须接着同一个矩阵画（旧实现只补
    /// 平移，外层 canvas 变换——如 Tooltip 的位移——会在换段后丢失，被边界
    /// 之后的兄弟节点画到未变换的位置）。
    SkMatrix segmentMatrix_;
    bool hasSegmentMatrix_ = false;
    /// 当前活动的作用域（外→内）。换段 / 作用域结束时据此重建画布状态。
    std::vector<CanvasScope*> scopes_;
    /// 构造时由调用方给了一个真实画布（外部模式：直接往那块画布上画，
    /// 例如增量重录某个边界、或测试里直接光栅到 SkSurface）⇒ 分段录制关闭，
    /// start/stop 都是空操作，画布不会被换掉。
    bool externalCanvas_ = false;
};

}  // namespace cui_core
