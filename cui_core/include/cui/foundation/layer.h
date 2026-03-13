#pragma once
/// @file foundation/layer.h
/// @brief Persistent scene-graph layers, mirroring the upstream Layer hierarchy.
///
/// The paint phase walks the render tree and emits Layer nodes into a tree
/// that the composite phase rasterizes onto the SkSurface.  Layers persist
/// across frames: a RepaintBoundary's PictureLayer is reused until the
/// subtree underneath it actually repaints, so the cost of a complex subtree
/// is paid once and replayed cheaply thereafter.
///
/// The hierarchy:
///   Layer (abstract)
///     ContainerLayer          — ordered child list
///       PictureLayer          — recorded SkPicture (leaf; rasterized by replay)
///       TransformLayer        — applies an Offset / SkMatrix to children
///       ClipRectLayer         — clips children to a Rect
///       OpacityLayer          — multiplies children's alpha
///   LayerTree                 — root ContainerLayer + frame size

#include <include/core/SkImage.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPicture.h>
#include <include/core/SkRect.h>
#include <memory>
#include <vector>

#include "geometry.h"

namespace cui {

// Forward declaration — ContainerLayer is defined below but referenced by Layer.
class ContainerLayer;

// ── Layer base ──────────────────────────────────────────────────────────────

/// Abstract node in the scene graph.  Non-copyable, identity-stable (pointer
/// identity, like 上游).  Each layer knows its parent for upward walks
/// (e.g. finding the nearest RepaintBoundary ancestor).
class Layer {
public:
    virtual ~Layer();

    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    ContainerLayer* parent() const { return parent_; }

    /// 该指针是否仍指向存活图层。
    ///
    /// RenderObject::layer_ 是裸句柄：`pruneOrphans()` 销毁本代未命中的
    /// 孤儿图层时它会被悬空（例如 offstage 子树整帧未绘制，下一帧恢复
    /// 绘制时句柄已释放）。任何对 layer_ 的 dynamic_cast 之前都必须先过
    /// 这一关，否则就是在已释放内存的 vtable 上做类型查询（未定义行为，
    /// 轻则误判、重则段错误/死循环）。
    static bool isAlive(const Layer* layer);

    /// Rasterize this layer (and its children, if any) onto `canvas`.
    virtual void rasterize(SkCanvas* canvas) const = 0;

    // ── 标记-清扫（mark & sweep）───────────────────────────────────────────
    // 全量重录（RenderView::paintTree）会级联重建整棵图层树，但为了复用
    // （PictureLayer 光栅缓存、避免每帧重分配）它不销毁已有子层。代价是：
    // 切页 / 增删子树后被销毁的 render object 其图层会作为「孤儿」残留在容器
    // 里，仍被 composite 光栅化 → 旧页内容重叠到新页之上。为此每次全量重录
    // 推进一个全局「代」计数，级联中每个真正被创建 / 复用的图层盖当前代戳
    // （markLive），级联结束后容器递归删除戳非当前代的子层（pruneOrphans）。

    /// 盖当前代戳：paint 级联中每用到一个图层（新建或复用）即调用。
    void markLive() { liveGen_ = s_gen; }

    /// 推进全局代计数（每次全量重录开始时调用一次）。
    static void advanceGeneration() { ++s_gen; }

protected:
    Layer();
    ContainerLayer* parent_ = nullptr;

    /// 本图层最近一次被级联使用的代；等于 s_gen 表示本帧存活。
    unsigned liveGen_ = 0;
    static unsigned s_gen;

    friend class ContainerLayer;
};

// ── ContainerLayer ──────────────────────────────────────────────────────────

/// A layer that holds an ordered list of child layers.  Rasterization walks
/// children front-to-back (first child drawn first, later children on top).
class ContainerLayer : public Layer {
public:
    ContainerLayer() = default;
    ~ContainerLayer() override;

    const std::vector<std::unique_ptr<Layer>>& children() const { return children_; }

    /// Append `child` to the end of the child list.  Ownership transfers to
    /// this container.  Returns the raw pointer for caller convenience.
    Layer* appendChild(std::unique_ptr<Layer> child);

    /// Remove all children (destroys them).
    void removeAllChildren();

    /// Remove a specific child by raw pointer (does NOT destroy it — caller
    /// retains ownership via the returned unique_ptr).
    std::unique_ptr<Layer> detachChild(Layer* child);

    /// 把已有子层移到列表末尾（子层列表顺序 = 绘制顺序）。
    ///
    /// 子层列表跨帧保留，而复用的旧层不会自动挪位：本帧新录制的直接绘制段
    /// 一律追加到末尾，旧层若停在前面的旧下标上，绘制顺序就会与新一帧的
    /// 绘制遍历顺序不一致（典型症状：玻璃叠层被画到它本该托着的子树之上）。
    /// 因此 paint 级联里每次用到（新建或复用）一个图层，都把它挪到末尾；
    /// 级联严格按绘制顺序推进，遍历结束时列表顺序就等于绘制顺序。
    void bringToFront(Layer* child);

    /// 递归删除本代未被 markLive 的后代子层（孤儿图层），连同其子树一起销毁。
    /// 由 RenderView::paintTree 在全量级联后调用，清理切页 / 增删子树遗留的图层。
    void pruneOrphans();

    void rasterize(SkCanvas* canvas) const override;

private:
    std::vector<std::unique_ptr<Layer>> children_;
};

// ── PictureLayer (leaf) ─────────────────────────────────────────────────────

/// Holds a recorded SkPicture.  Rasterization replays the picture onto the
/// canvas — this is the cheap "bake once, replay many times" path that makes
/// RepaintBoundary effective.
///
/// 光栅缓存：当一个「大」picture 连续多帧未被重录（stable）时，把它一
/// 次性光栅成 GPU SkImage，后续帧直接 blit 该图，避免每帧重放矢量
/// 操作（卡片阴影/文字等）——这是动画帧 composite 开销的大头。每帧都变的
/// 小 picture（动画画布）不缓存（stable 计数达不到阈值），避免反复重建。
class PictureLayer : public Layer {
public:
    PictureLayer() = default;
    explicit PictureLayer(sk_sp<SkPicture> picture) : picture_(std::move(picture)) {}

    const sk_sp<SkPicture>& picture() const { return picture_; }
    void setPicture(sk_sp<SkPicture> p) {
        picture_ = std::move(p);
        cache_.reset();
        stable_ = 0;
    }

    /// The window-space bounds this picture covers (used by the composite
    /// stage to decide whether a cached picture is still valid).
    const Rect& bounds() const { return bounds_; }
    void setBounds(const Rect& r) { bounds_ = r; }

    /// 重绘边界录制该 picture 时录制画布的初始矩阵（父画布完整 CTM + 本地
    /// 偏移）。增量重录只重录单个边界、不经过祖先，必须用它复现同一坐标帧，
    /// 否则外层 canvas 变换（RenderTransform 的位移/旋转等）会丢失 ——
    /// 典型症状是 Tooltip 气泡画回未变换的位置。见 pushRepaintBoundary。
    const SkMatrix& recordingMatrix() const { return recordingMatrix_; }
    void setRecordingMatrix(const SkMatrix& m) { recordingMatrix_ = m; }

    void rasterize(SkCanvas* canvas) const override;

private:
    sk_sp<SkPicture> picture_;
    Rect bounds_;
    SkMatrix recordingMatrix_;
    mutable sk_sp<SkImage> cache_;  // picture_ 的已光栅位图（设备像素）
    mutable int stable_ = 0;        // picture_ 未被重录的连续帧数
};

// ── TransformLayer ──────────────────────────────────────────────────────────

/// Applies a translation (Offset) to all children.  This is the workhorse
/// layer: every RenderBox with a BoxParentData.offset gets a TransformLayer
/// wrapping its subtree so painting happens in local coordinates.
class TransformLayer : public ContainerLayer {
public:
    TransformLayer() = default;
    explicit TransformLayer(const Offset& offset) : offset_(offset) {}

    const Offset& offset() const { return offset_; }
    void setOffset(const Offset& o) { offset_ = o; }

    void rasterize(SkCanvas* canvas) const override;

private:
    Offset offset_;
};

// ── ClipRectLayer ───────────────────────────────────────────────────────────

/// Clips children to a Rect (in the parent's coordinate space).  Used by
/// ScrollView / viewport render objects to clip overflowing content.
class ClipRectLayer : public ContainerLayer {
public:
    ClipRectLayer() = default;
    explicit ClipRectLayer(const Rect& clipRect) : clipRect_(clipRect) {}

    const Rect& clipRect() const { return clipRect_; }
    void setClipRect(const Rect& r) { clipRect_ = r; }

    void rasterize(SkCanvas* canvas) const override;

private:
    Rect clipRect_;
};

// ── OpacityLayer ────────────────────────────────────────────────────────────

/// Multiplies children's alpha by `alpha` (0.0 = fully transparent,
/// 1.0 = fully opaque).  Used by AnimatedWidget's opacity animation and any
/// widget that fades in/out.
class OpacityLayer : public ContainerLayer {
public:
    OpacityLayer() = default;
    explicit OpacityLayer(double alpha) : alpha_(alpha) {}

    double alpha() const { return alpha_; }
    void setAlpha(double a) { alpha_ = a; }

    void rasterize(SkCanvas* canvas) const override;

private:
    double alpha_ = 1.0;
};

// ── LayerTree ───────────────────────────────────────────────────────────────

/// The root of a frame's scene graph.  Holds the root ContainerLayer and the
/// frame size (so the composite stage knows how large the raster surface
/// must be).  RenderView owns one LayerTree and rebuilds it each frame.
class LayerTree {
public:
    LayerTree();

    ContainerLayer* rootLayer() { return root_.get(); }
    const ContainerLayer* rootLayer() const { return root_.get(); }

    const Size& frameSize() const { return frameSize_; }
    void setFrameSize(const Size& s) { frameSize_ = s; }

    /// Rasterize the entire tree onto `canvas`.
    void rasterize(SkCanvas* canvas) const;

private:
    std::unique_ptr<ContainerLayer> root_;
    Size frameSize_;
};

}  // namespace cui_core
