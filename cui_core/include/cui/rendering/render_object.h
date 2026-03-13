#pragma once
/// @file rendering/render_object.h
/// @brief The render tree base class and the PipelineOwner dirty scheduler.
///
/// RenderObject is the workhorse of the three-layer architecture, mirroring
/// the upstream's rendering/object.dart + box.dart protocol:
///   - It owns the layout result (size, constraints) and the parent-written
///     ParentData.
///   - It participates in the dirty-list scheduling via PipelineOwner
///     (flushLayout → flushCompositingBits → flushPaint).
///   - It exposes the 上游 child-management protocol (insertChild /
///     removeChild / moveChild / reorderChildren) that the Element layer
///     drives during mount / unmount / reconcile.
///   - Subclasses implement `performLayout` and either `performPaint` (the
///     classic Layer-tree recording path) or `paint` (the direct-painting
///     entry, which defaults to performPaint).
///
/// PipelineOwner holds intrusive doubly-linked dirty lists (no heap alloc per
/// mark) and flushes them in three phases:
///   1. flushLayout        — re-measure / re-arrange dirty nodes
///   2. flushCompositingBits — propagate needsCompositing up the tree
///   3. flushPaint         — re-record dirty subtrees into the Layer tree

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "../foundation/geometry.h"
#include "cui/foundation/abstract_node.h"
#include "cui/foundation/events.h"
#include "cui/foundation/layer.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/parent_data.h"

namespace cui {

class PipelineOwner;
class PaintingContext;

/// 渲染树拆除模式开关。
///
/// 元素卸载时（BuildOwner 析构、页面切换后 finalizeTree 卸载 inactive 元素），
/// RenderProxyBox::removeChild → markNeedsLayout 会沿 parent 链向上找 relayout
/// boundary；但此时祖先 RenderObject 可能已经被销毁（元素先卸载、渲染对象随后
/// 释放），访问野指针会崩（典型症状："vtable for __cxxabiv1::__class_type_info"）。
/// 拆除期间一律跳过父链遍历与 owner 访问即可安全拆除。
///
/// 用法：成对 push/pop（可嵌套）。
void pushRenderTeardown();
void popRenderTeardown();
bool renderTeardown();

// ── RenderObject ────────────────────────────────────────────────────────────

/// Base of the render tree.  Subclasses implement `performLayout` (compute
/// size from constraints) and a paint hook (`performPaint` for the Layer-tree
/// pipeline, or override `paint` directly — the upstream's entry point).
class RenderObject : public AbstractNode {
public:
    RenderObject();
    ~RenderObject() override;

    RenderObject(const RenderObject&) = delete;
    RenderObject& operator=(const RenderObject&) = delete;

    // ── Layout bookkeeping ────────────────────────────────────────────────

    /// True when this node's layout is stale (a constraint or child size
    /// changed since the last performLayout).
    bool needsLayout() const { return needsLayout_; }

    /// True when this node's paint output is stale.
    bool needsPaint() const { return needsPaint_; }

    /// True when this node (or any descendant) requires a compositing layer
    /// (opacity, clip, transform that can't be flattened).  The compositing
    /// phase uses this to decide whether to wrap the subtree in a dedicated
    /// Layer node.
    bool needsCompositing() const { return needsCompositing_; }

    /// Mark this node as needing layout.  Propagates the dirty bit upward
    /// to the relayout boundary and pushes this node onto the PipelineOwner's
    /// layout dirty list (if attached).
    ///
    /// 虚函数：上游的 `RenderBox.markNeedsLayout` 覆写它，在自身被父节点
    /// 以 intrinsic 方式依赖时把脏标越过 relayout boundary 传给父节点
    /// （见 `markParentNeedsLayout`）。
    virtual void markNeedsLayout();

    /// Clear the stale-layout flag.  Called by the layout cascade when a
    /// parent actually visits this node (RenderObject::layout) — without it the
    /// flag stays set forever and later markNeedsLayout() calls on this node
    /// early-return, so relayouts are silently dropped.
    void clearNeedsLayout() { needsLayout_ = false; }

    /// Mark this node as needing repaint.  Pushes onto the paint dirty list.
    void markNeedsPaint();

    /// Mark this node as needing a compositing-bits update.
    void markNeedsCompositingBits();

    /// The nearest ancestor that is a relayout boundary (or this node itself
    /// if it is one).  Layout dirty marks propagate up to this node and stop.
    RenderObject* relayoutBoundary() const { return relayoutBoundary_; }

    /// Whether this node is a relayout boundary.  A node is a boundary when
    ///   - it is the root, OR
    ///   - it does not depend on its parent's constraints (sizedByParent), OR
    ///   - its parent does not use its size.
    /// Subclasses override `isRelayoutBoundary()` to customize.
    virtual bool isRelayoutBoundary() const { return false; }

    /// Recompute and cache the relayout boundary pointer.  Called by
    /// flushLayout before performLayout, mirroring the upstream
    /// _updateRelayoutBoundary().
    void updateRelayoutBoundary();

    /// Whether this node is a repaint boundary.  When true, PaintingContext
    /// records the subtree into a dedicated PictureLayer instead of painting
    /// inline.  The cached picture is reused on frames where the subtree is
    /// clean — the structural replacement for per-widget layer compositing.
    virtual bool isRepaintBoundary() const { return false; }

    /// Re-record this repaint boundary's subtree into the Layer tree.  Only
    /// called by PipelineOwner::flushPaint, and only on nodes whose
    /// isRepaintBoundary() is true (markNeedsPaint stops its upward walk
    /// there).  The RenderView records the whole frame; a normal boundary
    /// records just its own subtree into its cached PictureLayer (the
    /// structural replacement for per-widget layer compositing — mirrors
    /// 上游's RepaintBoundary, which re-rasterizes only its own layer while
    /// the rest of the scene graph is replayed from cached pictures).
    virtual void repaintBoundaryPaint();

    /// 该 repaint boundary 拥有的持久图层（增量重绘时原地复用，只替换
    /// 其 SkPicture）。非边界节点为 nullptr。PaintPhase 在首次绘制时写入，
    /// 之后 flushPaint 直接复用同一图层对象，避免每帧重建整棵图层树。
    Layer* layer() const { return layer_; }
    const Offset& layerOffset() const { return layerOffset_; }
    void setLayer(Layer* l, Offset off) { layer_ = l; layerOffset_ = off; }

    /// 取图层句柄，已随祖先容器回收（pruneOrphans）的悬空句柄就地清空。
    /// 绘制/重录前必须走这条入口——直接对 `layer()` 做 dynamic_cast 会踩
    /// 已释放内存的 vtable（未定义行为）。
    Layer* currentLayer() {
        if (layer_ && !Layer::isAlive(layer_))
            layer_ = nullptr;
        return layer_;
    }

    /// Whether this node's layout depends on its parent's constraints (true
    /// for most boxes) or is self-determined (sized-by-parent, like a fixed-
    /// size icon).  When true, a constraint change triggers relayout.
    virtual bool sizedByParent() const { return false; }

    /// For sizedByParent nodes: compute size from constraints alone (no child
    /// visits).  Default is a no-op; sizedByParent overrides fill `size`.
    virtual void performResize() {}

    // ── Layout / paint (subclass hooks) ───────────────────────────────────

    /// Compute the layout.  Subclasses MUST override this to position
    /// children and set their own size.  Called by PipelineOwner::flushLayout.
    virtual void performLayout() = 0;

    /// Run the layout protocol (上游的 `RenderObject.layout`): the parent
    /// hands down `constraints`; short-circuits when constraints are
    /// unchanged and the node is clean.  `parentUsesSize` controls whether a
    /// child size change re-dirties the parent.
    virtual void layout(const BoxConstraints& constraints, bool parentUsesSize = true);

    /// Paint this node (上游的 `RenderObject.paint` — the public paint
    /// entry).  Default forwards to performPaint.  Direct-painting subclasses
    /// (the cui_core widget render objects) override this instead.
    virtual void paint(PaintingContext& context, const Offset& offset);

    /// Paint this node into the PaintingContext (Layer-tree recording path).
    /// Called by the default paint() and by PipelineOwner::flushPaint via the
    /// PaintingContext.  The context provides the canvas in local coordinates.
    virtual void performPaint(PaintingContext& context, const Offset& offset) {
        (void)context;
        (void)offset;
    }

    /// The rect in local coordinates that this node paints into.  Used by
    /// the paint phase to decide whether a node is visible inside the clip.
    virtual Rect paintBounds() const { return paintBounds_; }

    // ── Layout result (上游语义：size / constraints 属于 RenderObject) ──

    /// The layout result: the size this node chose within its constraints.
    const Size& size() const { return size_; }
    void setSize(const Size& s) { size_ = s; }

    /// The constraints this node was last laid out with.
    const BoxConstraints& constraints() const { return constraints_; }

    // ── Parent data ───────────────────────────────────────────────────────

    /// The parent-written per-child data (offset, flex info, positioning).
    /// Allocated lazily by the parent's `setupParentData`.
    ParentData* parentData() const { return parentData_; }
    void setParentData(ParentData* pd) { parentData_ = pd; }

    /// Called by the parent when adding this child.  Override to allocate a
    /// concrete ParentData subtype (BoxParentData, FlexParentData, ...).
    virtual void setupParentData(RenderObject* child) {
        if (!child->parentData_)
            child->parentData_ = new ParentData();
    }

    /// 接管子节点：建立父子关系、传播 owner、分配 ParentData（上游的
    /// `RenderObject.adoptChild`）。非虚便捷入口；容器子类通常经由
    /// insertChild 间接调用。
    void adoptChild(RenderObject* child);

    /// 移除子节点：在 AbstractNode::dropChild 基础上，让父节点重新测量/绘制
    /// （对齐 上游的 adopt/drop 语义，保证「setState 增删子节点」可见）。
    void dropChild(RenderObject* child);

    // ── Child management protocol (上游的 insert / remove / move) ─────
    //
    // 由 Element 层在挂载、卸载、重排时调用。默认实现什么都不做，
    // 容器型 RenderObject（RenderProxyBox / RenderBoxContainer）覆写。

    /// 把 child 插入到 after 之后（after 为 nullptr 表示插到最前）。
    virtual void insertChild(RenderObject* child, RenderObject* after) {
        (void)child;
        (void)after;
    }
    /// 移除子节点。
    virtual void removeChild(RenderObject* child) {
        (void)child;
    }
    /// 把 child 移动到 after 之后。
    virtual void moveChild(RenderObject* child, RenderObject* after) {
        (void)child;
        (void)after;
    }
    /// 按给定顺序重排子节点（仅重排已存在的子节点）。
    virtual void reorderChildren(const std::vector<RenderObject*>& desired) {
        (void)desired;
    }

    // ── Coordinate conversion ─────────────────────────────────────────────

    /// 本地坐标 → 全局（窗口）坐标。沿父链累加 parentData offset
    /// （上游的 `localToGlobal`）。
    Offset localToGlobal(const Offset& local = {}) const;

    /// 带祖先的 2 参版本。上游的 `localToGlobal(Offset delta, {RenderObject? ancestor})`
    /// = `MatrixUtils.transformPoint(getTransformTo(ancestor), delta)`。
    /// `ancestor == nullptr` 时换算到渲染树根（本仓即 RenderView）。
    Offset localToGlobal(const Offset& local, const RenderObject* ancestor) const;

    /// 全局（窗口）坐标 → 本节点本地坐标。localToGlobal 的逆变换
    /// （上游的 `globalToLocal`）。
    Offset globalToLocal(const Offset& global) const;

    /// 带祖先的 2 参版本（上游的 `globalToLocal(point, ancestor: ...)`）。
    /// 上游 还要做「沿视线反投影到本地 XY 平面」，那只在与透视/3D 变换有关；
    /// 本仓 的 paint 变换目前只含平移（见 applyPaintTransform），仿射逆变换
    /// 即等价结果。
    Offset globalToLocal(const Offset& global, const RenderObject* ancestor) const;

    /// 上游的 `RenderObject.applyPaintTransform`：把「绘制 `child` 时会施加的
    /// 变换」乘进 `transform`。
    ///
    /// 上游的基类实现是空操作、由 `RenderBox` 补上 parentData offset；本仓的
    /// `ParentData::offset` 就在基类上，所以基类直接做平移，和现有
    /// `localToGlobal` 的父链累加口径一致。
    /// **待接**：`RenderTransform` / `RenderFittedBox` 这类「绘制时自带变换」的
    /// 节点尚未覆写它（它们的变换目前只在 performPaint 与 hitTest 里生效），
    /// 因此经过这些节点的坐标换算暂时只含平移。
    virtual void applyPaintTransform(const RenderObject* child, Matrix4& transform) const;

    /// 上游的 `RenderObject.getTransformTo`：本节点坐标系 → `ancestor` 坐标系
    /// 的矩阵。`ancestor == nullptr` 时目标是渲染树根。
    /// 两节点不在同一棵树时返回单位矩阵（上游抛异常，本仓不打断渲染）。
    Matrix4 getTransformTo(const RenderObject* ancestor) const;

    // ── Hit testing ───────────────────────────────────────────────────────

    /// Does `offset` (in this node's local coordinates) hit this node?
    virtual bool hitTestSelf(const Offset& offset) const { (void)offset; return false; }

    /// Does `offset` hit any child?  Subclasses with children override this
    /// to walk children in reverse paint order and add HitTestEntries.
    virtual bool hitTestChildren(const Offset& offset) const { (void)offset; return false; }

    /// 上游 协议的命中测试（本地坐标）：先 hitTestSelf，再倒序
    /// hitTestChildren；命中的节点依次加入 result。默认实现按
    /// visitChildren 的逆序遍历子节点，用 parentData offset 换算本地坐标。
    virtual bool hitTest(const Offset& position, std::vector<RenderObject*>& result);

    /// 上游 协议的子节点命中测试。默认实现：倒序遍历子节点。
    virtual bool hitTestChildren(const Offset& position,
                                 std::vector<RenderObject*>& result);

    /// 命中事件处理（上游的 `HitTestTarget.handleEvent`）。
    ///
    /// 事件从命中链最深节点（target）起、沿父链向上冒泡时被逐一调用；
    /// position 已变换为该节点的本地坐标。处理者调用
    /// `event.stopPropagation()` 即可终止冒泡。默认空实现（不消费事件）。
    virtual void handleEvent(const PointerEvent& event, const HitTestEntry& entry) {
        (void)event;
        (void)entry;
    }

    // ── Intrusive dirty-list pointers (managed by PipelineOwner) ──────────
    /// @{
    RenderObject* nextNeedLayout() const { return nextNeedLayout_; }
    RenderObject* nextNeedPaint() const { return nextNeedPaint_; }
    RenderObject* nextNeedCompositingBits() const { return nextNeedCompositingBits_; }
    RenderObject*& nextNeedLayoutRef() { return nextNeedLayout_; }
    RenderObject*& nextNeedPaintRef() { return nextNeedPaint_; }
    RenderObject*& nextNeedCompositingBitsRef() { return nextNeedCompositingBits_; }
    /// @}

    // ── AbstractNode overrides ────────────────────────────────────────────
    void attachedToRoot() override;
    void detachedFromRoot() override;

protected:
    /// Cached paint bounds in local coordinates.  Subclasses update this in
    /// performLayout or performPaint.
    Rect paintBounds_{};

    /// 上游的 `RenderObject.markParentNeedsLayout`：把自己的脏标就地置位，
    /// 然后把处理交给父节点（父节点继续按自己的规则向上冒泡）。
    /// 只在 `parent()` 非空时调用。
    void markParentNeedsLayout();

    /// 持久图层（增量重绘用）。isRepaintBoundary() 为 true 的节点，PaintPhase
    /// 首次绘制时会把为它创建的图层存入此处；后续 flushPaint 复用同一对象、
    /// 仅替换其 SkPicture，从而只重光栅化该子树。见 setLayer()。
    Layer* layer_ = nullptr;
    Offset layerOffset_{};

    /// The layout result (size chosen within constraints).
    Size size_{};

    /// The constraints from the last layout call.
    BoxConstraints constraints_{};

    /// Whether this node needs a compositing layer (set by the compositing
    /// bits flush based on opacity/clip/transform presence).
    bool needsCompositing_ = false;

    /// Recompute whether this node needs its own compositing layer.
    /// Called by flushCompositingBits.  Subclasses override to report
    /// opacity/clip/transform requirements.  Default: false.
    virtual bool updateNeedsCompositing() { return false; }

private:
    bool needsLayout_ = true;
    bool needsPaint_ = true;

    /// Cached relayout boundary (refreshed on layout).
    RenderObject* relayoutBoundary_ = nullptr;

    /// Parent-written per-child layout data.
    ParentData* parentData_ = nullptr;

    // Intrusive dirty-list pointers (PipelineOwner manages these).
    RenderObject* nextNeedLayout_ = nullptr;
    RenderObject* nextNeedPaint_ = nullptr;
    RenderObject* nextNeedCompositingBits_ = nullptr;

    // Membership flags — the authoritative "is this node already on a dirty
    // list" marker.  A singly-linked head-insertion list cannot tell a node
    // that is the *tail* (next == null) apart from a node that is *not in the
    // list* (next == null), so relying on `next == null` for idempotency lets
    // a tail node be re-added and forms a cycle (RenderView <-> RenderWidget)
    // that makes flushLayout's list walk loop forever.  These flags make
    // addNeedsLayout / addNeedsPaint / addNeedsCompositingBits truly
    // idempotent regardless of position in the list.
    bool inLayoutList_ = false;
    bool inPaintList_ = false;
    bool inCompositingBitsList_ = false;

    friend class PipelineOwner;
};

// ── PipelineOwner ───────────────────────────────────────────────────────────

/// Manages the dirty-node scheduling for the render tree.  Holds intrusive
/// linked lists of nodes needing layout and paint, and flushes them in the
/// correct order each frame.
///
/// Frame integration: the SchedulerBinding calls
///   pipelineOwner.flushLayout();
///   pipelineOwner.flushCompositingBits();
///   pipelineOwner.flushPaint();
/// in that order, then the composite phase rasterizes the LayerTree.
class PipelineOwner : public Owner {
public:
    PipelineOwner() = default;
    ~PipelineOwner() override = default;

    /// Attach a root RenderObject to this owner.  Propagates the owner
    /// down the entire subtree, pushing dirty nodes onto our lists.
    void setRootNode(RenderObject* root);
    /// Detach the current root (reverse of setRootNode).
    void clearRootNode(RenderObject* root);

    /// 当前根 RenderObject（命中测试与事件下发的起点）。
    RenderObject* rootNode() const { return rootNode_; }

    /// Add a node to the layout dirty list (idempotent — already-dirty nodes
    /// are not re-added).
    void addNeedsLayout(RenderObject* node);

    /// Add a node to the paint dirty list.
    void addNeedsPaint(RenderObject* node);

    /// Add a node to the compositing-bits dirty list.
    void addNeedsCompositingBits(RenderObject* node);

    /// Remove a node from a dirty list without processing it (used when a node
    /// is detached from the tree).  Walks the small list to fix the predecessor
    /// link; safe even if the node is the head or not present.  Public so
    /// RenderObject::detachedFromRoot can call them.
    void unlinkNeedsLayout(RenderObject* node);
    void unlinkNeedsPaint(RenderObject* node);
    void unlinkNeedsCompositingBits(RenderObject* node);

    /// 把节点从所有已缓存的命中链（hoverPath_ / 各指针的 downPaths_）里摘掉，
    /// 空的 down 链整条删除。节点离开渲染树即被销毁，而指针事件在两帧之间
    /// 到达 —— 不摘除就是对着已释放内存派发事件。由
    /// RenderObject::detachedFromRoot 调用（Public 理由同 unlink*）。
    ///
    /// 语义后果：被摘掉的节点**不会**收到 Exit / 后续 drag 事件（上游 Dart
    /// 靠 GC 保活 annotation 仍能送达，本仓做不到）。
    void forgetPointerTarget(RenderObject* node);

    /// Phase 1: re-layout all dirty nodes.  Walks the layout dirty list in
    /// depth order (shallowest first) so parent layouts run before children,
    /// matching the upstream's invariant.
    void flushLayout();

    /// Phase 2: propagate needsCompositing bits up the tree.
    void flushCompositingBits();

    /// Phase 3: re-paint all dirty nodes via the PaintingContext.
    ///
    /// 每次调用都会先把本帧 damage 记录复位，再随每个被重录的 repaint
    /// boundary 累加其窗口坐标矩形（上游 Rasterizer 的 frameDamage 语义）。
    /// flush 结束后可由 takeFrameDamage()/takeDamageAll() 取用。
    void flushPaint();

    /// 本帧真正被重录的边界并集（窗口坐标）。damageAll() 为真时表示整屏。
    /// take* 为读并清（读后复位），供帧驱动决定 clear/composite/blit 范围。
    Rect takeFrameDamage() {
        Rect r = frameDamage_;
        frameDamage_ = {};
        return r;
    }
    bool takeDamageAll() {
        const bool a = damageAll_;
        damageAll_ = false;
        return a;
    }

    /// 命中测试 + 指针事件下发（上游的 RendererBinding 事件分发）。
    ///
    /// `globalEvent.position` 为窗口（根）坐标系。流程：
    ///   1. Down 事件做全新 hitTest 并按指针缓存命中链；同一指针的
    ///      move/up 复用该链（上游的 GestureBinding 行为——内容滚动
    ///      导致坐标体系变化后，事件仍路由到按下时的目标）；
    ///   2. 自 target 向根冒泡：对每个节点把 position 变换为其本地坐标，
    ///      调用 handleEvent（globalPosition 保持窗口坐标）；
    ///   3. move/hover 事件的 `delta` 按该指针的上一位置现算（上游
    ///      PointerEventConverter 的 `_PointerState.deltaTo`；宿主只给绝对
    ///      坐标，不填 delta）；
    ///   4. 任一节点调用 event.stopPropagation() 即终止冒泡；
    ///   5. Up/Cancel 后解除该指针的链缓存与位置记录。
    /// 无 root 或命中链为空时静默返回。
    void dispatchPointerEvent(const PointerEvent& globalEvent);

    /// Number of nodes the last flushLayout actually re-laid-out (read-and-
    /// clear). Application::paintFullFrame uses this to bump the subtree
    /// picture-cache epoch: layout can move/resize subtrees without any
    /// accompanying repaint.
    uint64_t takeLayoutFlushCount() {
        const uint64_t c = layoutFlushCount_;
        layoutFlushCount_ = 0;
        return c;
    }

private:
    // Intrusive singly-linked lists (head pointers).  Nodes link via their
    // nextNeedLayout_ / nextNeedPaint_ pointers.
    RenderObject* needsLayoutHead_ = nullptr;
    RenderObject* needsPaintHead_ = nullptr;
    RenderObject* needsCompositingBitsHead_ = nullptr;

    /// 根 RenderObject（命中测试起点）。
    RenderObject* rootNode_ = nullptr;

    // Nodes actually re-laid-out by the last flushLayout (see takeLayoutFlushCount).
    uint64_t layoutFlushCount_ = 0;

    /// 每指针的 down 命中链缓存（上游的 GestureBinding._hitTests）。
    /// key = pointerId；Up/Cancel 后移除。缓存的是裸指针，有效性由
    /// forgetPointerTarget 在节点 detach 时摘除来保证。
    std::unordered_map<int64_t, std::vector<RenderObject*>> downPaths_;

    /// 每指针的上一事件位置（move/hover 的 `delta` 来源，= 上游
    /// PointerEventConverter 的 `_PointerState`）。Down 起记录，Up/Cancel 移除。
    std::unordered_map<int64_t, Offset> lastPointerPositions_;

    /// hover 命中链（上一帧 Hover 命中的渲染对象链，target→root 顺序），
    /// 用于派发 Enter/Exit：当 hover 目标变化时，对“离开”的对象派发 Exit、
    /// 对“新进入”的对象派发 Enter（对齐 上游的 enter/exit 语义）。
    /// 同样是裸指针，靠 forgetPointerTarget 保命。
    std::vector<RenderObject*> hoverPath_;

    /// 本帧脏区（见 flushPaint / takeFrameDamage）。damageAll_ 为真表示脏区
    /// 覆盖整屏（根边界被重录），此时 frameDamage_ 无意义。
    Rect frameDamage_{};
    bool damageAll_ = false;
};

}  // namespace cui_core
