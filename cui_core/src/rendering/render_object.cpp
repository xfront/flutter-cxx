/// @file rendering/render_object.cpp
/// @brief RenderObject lifecycle + PipelineOwner dirty-list flush.

#include "cui/rendering/render_object.h"

#include <algorithm>
#include <cassert>
#include <include/core/SkCanvas.h>
#include <include/core/SkPictureRecorder.h>

#include "cui/rendering/painting_context.h"
#include "cui/rendering/render_box.h"

namespace cui {

// ── 拆除模式开关 ──────────────────────────────────────────────────────────────
namespace {
int g_renderTeardownDepth = 0;
}

void pushRenderTeardown() { ++g_renderTeardownDepth; }
void popRenderTeardown() {
    if (g_renderTeardownDepth > 0)
        --g_renderTeardownDepth;
}
bool renderTeardown() { return g_renderTeardownDepth > 0; }

// ── RenderObject ────────────────────────────────────────────────────────────

RenderObject::RenderObject() {
    // Every render object starts dirty — it needs its first layout and paint.
    needsLayout_ = true;
    needsPaint_ = true;
}

RenderObject::~RenderObject() {
    // Clean up parent data if we allocated it.
    delete parentData_;
}

void RenderObject::markNeedsLayout() {
    // 上游的不变量是「needsLayout ⇒ 节点(或其 boundary)必在脏链」，
    // 因此 上游 这里可以安全早退。但本工程有多处会产生
    // 「dirty 却不在脏链」的节点（RenderObject 构造即置脏、树手术后
    // 遗留、render 级联未覆盖旧 DSL 子树等），早退会让这些节点永远
    // 无法进入 flushLayout —— 切页后新页面以零约束绘制、布局错乱
    // 正是这么来的。addNeedsLayout 本身幂等，无条件入队 boundary
    // 即可自愈，语义与 上游的幂等入队一致。
    needsLayout_ = true;
    // 拆除期（元素卸载）跳过：父链/owner 可能已失效，遍历会踩野指针。
    if (renderTeardown())
        return;
    if (!attached() || !owner())
        return;
    // 上游 semantics: walk up the parent chain to find the nearest
    // relayout boundary (or use the cached pointer) and enqueue THAT node
    // — not the root.  The boundary's performLayout will cascade down to
    // reach this node.
    RenderObject* boundary = relayoutBoundary_;
    if (!boundary || boundary == this) {
        // Fallback: walk up to find the boundary (cached pointer may be
        // stale after tree surgery).
        boundary = this;
        while (boundary->parent()) {
            auto* p = static_cast<RenderObject*>(boundary->parent());
            if (p->isRelayoutBoundary() || !p->parent()) {
                boundary = p;
                break;
            }
            boundary = p;
        }
    }
    boundary->needsLayout_ = true;
    static_cast<PipelineOwner*>(owner())->addNeedsLayout(boundary);
}

void RenderObject::markParentNeedsLayout() {
    // 上游的 `RenderObject.markParentNeedsLayout`：本节点置脏，然后把脏标
    // 交给父节点处理（父节点再按自己的规则决定入队还是继续上抛）。
    needsLayout_ = true;
    RenderObject* parent = static_cast<RenderObject*>(AbstractNode::parent());
    if (parent)
        parent->markNeedsLayout();
}

void RenderObject::updateRelayoutBoundary() {
    // Mirror the upstream's _updateRelayoutBoundary(): compute and cache the
    // nearest ancestor that is a relayout boundary.
    if (isRelayoutBoundary() || !parent()) {
        relayoutBoundary_ = this;
    } else {
        auto* p = static_cast<RenderObject*>(parent());
        if (p->isRelayoutBoundary() || !p->parent()) {
            relayoutBoundary_ = p;
        } else {
            relayoutBoundary_ = p->relayoutBoundary_;
        }
    }
}

void RenderObject::markNeedsPaint() {
    if (needsPaint_)
        return;
    needsPaint_ = true;
    if (!attached() || !owner())
        return;
    // 上游 semantics: the repaint request propagates up to the nearest
    // repaint boundary (or the root when no intermediate boundary exists) —
    // that boundary is what flushPaint re-records, not the leaf itself.
    // Every node on the way gets its flag set, so flushPaint can tell that
    // the subtree under the boundary is stale (上游 does the same: each
    // markNeedsPaint up the chain sets _needsPaint before recursing).
    // 中间层边界（RepaintBoundary / RenderCustomPaint）脏的时候**不会**再触碰
    // RenderView，所以只有边界自身的矩形进入本帧脏区。
    RenderObject* boundary = this;
    while (!boundary->isRepaintBoundary() && boundary->parent()) {
        boundary = static_cast<RenderObject*>(boundary->parent());
        boundary->needsPaint_ = true;
    }
    boundary->needsPaint_ = true;
    static_cast<PipelineOwner*>(owner())->addNeedsPaint(boundary);
}

void RenderObject::repaintBoundaryPaint() {
    // 增量重绘：只重录本边界子树，原地替换其持久图层的 SkPicture，
    // 不改动图层树结构（其余图层由 composite 直接 replay 缓存图）。
    // 嵌套的 repaint boundary / transform / clip / opacity 在录制过程中
    // 通过 PaintingContext 复用各自已有图层（见 painting_context.cpp）。
    auto* pl = dynamic_cast<PictureLayer*>(currentLayer());
    // 拿不到持久图层的两种情况（从没录过 / 图层已被祖先容器回收）由
    // PipelineOwner::flushPaint 的「逐层外抛到有图层的祖先边界」兜住，
    // 这里只防御直接调用：静默返回会留下「脏区已算、像素不变」的残影。
    if (!pl)
        return;
    ContainerLayer* parent = pl->parent();
    if (!parent)
        return;
    pl->markLive();

    const Rect pb = paintBounds();
    const double safeWidth = std::max(0.0, pb.width);
    const double safeHeight = std::max(0.0, pb.height);

    SkPictureRecorder recorder;
    SkCanvas* recordCanvas = recorder.beginRecording(SkRect::MakeXYWH(
        static_cast<SkScalar>(layerOffset_.dx + pb.x),
        static_cast<SkScalar>(layerOffset_.dy + pb.y),
        static_cast<SkScalar>(safeWidth),
        static_cast<SkScalar>(safeHeight)));
    // 复现整帧重录时的坐标帧：只补平移会丢掉外层 canvas 变换（Tooltip 的
    // Transform、图表的旋转等），边界在增量帧会画回未变换的位置。
    // recordingMatrix 由 pushRepaintBoundary 每次挂层时刷新。
    recordCanvas->setMatrix(pl->recordingMatrix());

    PaintingContext ctx(parent, recordCanvas, Rect{});
    ctx.currentOffset_ = layerOffset_;
    paint(ctx, {});

    pl->setPicture(recorder.finishRecordingAsPicture());
}

void RenderObject::markNeedsCompositingBits() {
    if (attached() && owner()) {
        auto* po = static_cast<PipelineOwner*>(owner());
        po->addNeedsCompositingBits(this);
    }
}

void RenderObject::attachedToRoot() {
    AbstractNode::attachedToRoot();
    // The render-tree root seeds the dirty lists on attach: it is always a
    // relayout boundary, so enqueueing it ensures the first frame lays out
    // the whole tree.  Non-root relayout boundaries (RepaintBoundary) are
    // reached by the root's layout cascade.
    if (owner() && !parent()) {
        auto* po = static_cast<PipelineOwner*>(owner());
        if (needsLayout_)
            po->addNeedsLayout(this);
        if (needsPaint_)
            po->addNeedsPaint(this);
    }
}

void RenderObject::detachedFromRoot() {
    // Remove from dirty lists by unlinking — a node pulled out of the tree
    // must not keep a dangling next pointer or a stale membership flag, or a
    // later markNeedsLayout on it would be silently dropped (flag still set)
    // and an in-list node would be skipped forever.
    if (owner()) {
        auto* po = static_cast<PipelineOwner*>(owner());
        po->unlinkNeedsLayout(this);
        po->unlinkNeedsPaint(this);
        po->unlinkNeedsCompositingBits(this);
        // 命中链缓存（hoverPath_ / downPaths_）里存的是裸指针：节点离开树后
        // 紧接着就会被销毁，而事件是在两帧之间到达的。不摘除的话，下一次
        // Hover 的 Exit 差分会对着已释放的内存调 globalToLocal/handleEvent
        // （heap-use-after-free，段错误落在 Offset::operator-）。
        po->forgetPointerTarget(this);
    }
    AbstractNode::detachedFromRoot();
}

// ── Layout / paint / hit-test protocol (上游的 RenderObject 协议) ───────

void RenderObject::layout(const BoxConstraints& constraints, bool parentUsesSize) {
    (void)parentUsesSize;  // will be used for relayout-boundary computation

    const BoxConstraints prevConstraints = constraints_;
    const Size prevSize = size_;
    constraints_ = constraints;

    // The cascade visited us — clear our dirty flag NOW.  flushLayout only
    // clears the flag of the enqueued boundary node itself; if descendants
    // kept needsLayout_ = true forever, every later markNeedsLayout() on
    // them would early-return ("already dirty") and never re-enqueue —
    // subtree rebuilds (page switches, popups, …) would then paint stale
    // frames forever.
    clearNeedsLayout();

    // Always run performLayout() when called by a parent during the cascade.
    // The early-return optimization (skip when !needsLayout && constraints
    // unchanged) is unsafe here: flushLayout() no longer recursively clears
    // descendant flags, so a child called via parent->performLayout()->
    // child->layout() must always execute to avoid leaving the subtree
    // un-laid-out at {0,0}.

    // Sized-by-parent: compute size from constraints alone, no child visits.
    if (sizedByParent()) {
        performResize();
    }

    // Full layout: position children, finalize size.
    performLayout();

    // 上游 的 `_layoutWithoutResize()` 以 `markNeedsPaint()` 收尾：凡是重新布局
    // 过的节点都要重绘，否则「只标布局、没标绘制」的这一帧拿不到脏区，会被帧
    // 驱动的「no damage」判定整帧跳过（表现为改了参数、布局算对了像素却没变）。
    // 本仓的级联是无条件重跑 performLayout 的（见上面的注释），照抄上游会把整棵
    // 子树都标脏，故这里按「收到的约束或自身尺寸真的变了」判定。
    if (constraints != prevConstraints || size_ != prevSize)
        markNeedsPaint();
}

void RenderObject::paint(PaintingContext& context, const Offset& offset) {
    performPaint(context, offset);
}

void RenderObject::adoptChild(RenderObject* child) {
    if (!child)
        return;
    setupParentData(child);
    AbstractNode::adoptChild(child);
    // 对齐 上游的 RenderObject.adoptChild：插入子节点后，父节点需要重新
    // 测量（新子节点才能拿到约束/尺寸），并需要重新绘制（否则新增子树永远
    // 停留在 size(0,0) 且不会被绘制——例如 Tooltip 显示气泡、菜单弹出等
    // 「setState 新增子节点」的场景会整片不显示）。
    if (!renderTeardown()) {
        markNeedsLayout();
        markNeedsPaint();
    }
}

void RenderObject::dropChild(RenderObject* child) {
    if (!child)
        return;
    AbstractNode::dropChild(child);
    // 对齐上游：移除子节点后，父节点需要重新测量与绘制（例如 Tooltip
    // 隐藏气泡、菜单收起后，残留的尺寸/像素需更新）。
    if (!renderTeardown()) {
        markNeedsLayout();
        markNeedsPaint();
    }
}

Offset RenderObject::localToGlobal(const Offset& local) const {
    return localToGlobal(local, nullptr);
}

Offset RenderObject::localToGlobal(const Offset& local, const RenderObject* ancestor) const {
    // 上游：MatrixUtils.transformPoint(getTransformTo(ancestor), point)。
    return getTransformTo(ancestor).transformOffset(local);
}

Offset RenderObject::globalToLocal(const Offset& global) const {
    return globalToLocal(global, nullptr);
}

Offset RenderObject::globalToLocal(const Offset& global, const RenderObject* ancestor) const {
    return getTransformTo(ancestor).inverse().transformOffset(global);
}

void RenderObject::applyPaintTransform(const RenderObject* child, Matrix4& transform) const {
    // 上游 的基类实现是空操作，由 RenderBox 补上 parentData offset；本仓的
    // ParentData::offset 就在基类上，所以这里直接做平移。
    if (child != nullptr && child->parentData() != nullptr) {
        const Offset& offset = child->parentData()->offset;
        transform.translate(offset.dx, offset.dy);
    }
}

namespace {

const RenderObject* renderObjectParent(const RenderObject* node) {
    return node && node->parent() ? static_cast<const RenderObject*>(node->parent()) : nullptr;
}

const RenderObject* renderTreeRoot(const RenderObject* node) {
    const RenderObject* root = node;
    while (renderObjectParent(root))
        root = renderObjectParent(root);
    return root;
}

}  // namespace

Matrix4 RenderObject::getTransformTo(const RenderObject* target) const {
    // 逐段照抄上游 object.dart:3686 的双侧上溯 + 两段矩阵合成。
    // 偏离一处：上游在 target==null 时把 fromPath 的最后一跳（根的直接子
    // 偏移）跳过（`lastIndex = length - 2`）；本仓保持既有 localToGlobal
    // 「累加到根」的口径，一律取 length - 1。对 RenderView（其子恒在 (0,0)）
    // 两者等价。
    const RenderObject* to = target ? target : renderTreeRoot(this);
    const RenderObject* from = this;

    std::vector<const RenderObject*> fromPath;  // [this, parent, ..., common]
    std::vector<const RenderObject*> toPath;    // [target, parent, ..., common]

    while (from != to) {
        if (!from || !to)
            return Matrix4::identity();  // 不在同一棵渲染树（上游抛异常）
        const int fromDepth = from->depth();
        const int toDepth = to->depth();
        if (fromDepth >= toDepth) {
            if (fromPath.empty())
                fromPath.push_back(this);
            from = renderObjectParent(from);
            if (!from)
                return Matrix4::identity();
            fromPath.push_back(from);
        }
        if (fromDepth <= toDepth) {
            if (toPath.empty())
                toPath.push_back(target);
            to = renderObjectParent(to);
            if (!to)
                return Matrix4::identity();
            toPath.push_back(to);
        }
    }

    Matrix4 fromTransform = Matrix4::identity();
    for (int index = static_cast<int>(fromPath.size()) - 1; index > 0; --index)
        fromPath[index]->applyPaintTransform(fromPath[index - 1], fromTransform);

    if (toPath.empty())
        return fromTransform;

    Matrix4 toTransform = Matrix4::identity();
    for (int index = static_cast<int>(toPath.size()) - 1; index > 0; --index)
        toPath[index]->applyPaintTransform(toPath[index - 1], toTransform);

    // 上游：`if (toTransform.invert() == 0) return Matrix4.zero();`
    // 本仓 Matrix4::inverse() 对奇异矩阵返回单位矩阵，故不做该分支。
    return matrix4Multiply(fromTransform, toTransform.inverse());
}

bool RenderObject::hitTest(const Offset& position, std::vector<RenderObject*>& result) {
    // 上游 协议：先把自己加入命中链（若 hitTestSelf 命中），再递归子节点；
    // 子节点在自身之后入列。最终链序为「根 → … → target（末位）」，
    // 与事件「自最深节点向根冒泡」的遍历顺序一致（分发时反向遍历即 target 先收到）。
    const bool hitSelf = hitTestSelf(position);
    if (hitSelf)
        result.push_back(this);
    const bool hitChildren = hitTestChildren(position, result);
    return hitSelf || hitChildren;
}

bool RenderObject::hitTestChildren(const Offset& position,
                                   std::vector<RenderObject*>& result) {
    // 上游的默认实现：倒序遍历子节点（后画的在上）。
    std::vector<RenderObject*> children;
    visitChildren([&](AbstractNode* child) {
        children.push_back(static_cast<RenderObject*>(child));
    });
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        RenderObject* child = *it;
        if (!child)
            continue;
        // 转换到子节点本地坐标。
        Offset local = position;
        if (child->parentData())
            local = local - child->parentData()->offset;
        if (child->hitTest(local, result))
            return true;
    }
    return false;
}

// ── PipelineOwner ───────────────────────────────────────────────────────────

void PipelineOwner::setRootNode(RenderObject* root) {
    rootNode_ = root;
    if (root)
        root->attachOwnerRecursive(this);
}

void PipelineOwner::clearRootNode(RenderObject* root) {
    if (root && rootNode_ == root)
        rootNode_ = nullptr;
    if (root)
        root->detachOwnerRecursive();
}

void PipelineOwner::dispatchPointerEvent(const PointerEvent& globalEvent) {
    if (!rootNode_)
        return;

    // delta = 自该指针上一事件以来的位移。宿主（Application::dispatchInput）
    // 只给绝对坐标，上游则在 PointerEventConverter 里按 `_PointerState` 现算
    // —— 漏了这步，onPanUpdate 收到的 `event.delta` 恒为 0（无按键的 hover 也
    // 一样，鼠标悬停类控件同样依赖它）。
    Offset delta{};
    if (globalEvent.type == PointerEventType::Move ||
        globalEvent.type == PointerEventType::Hover) {
        auto last = lastPointerPositions_.find(globalEvent.pointerId);
        if (last != lastPointerPositions_.end())
            delta = globalEvent.position - last->second;
    }
    if (globalEvent.type == PointerEventType::Up ||
        globalEvent.type == PointerEventType::Cancel) {
        lastPointerPositions_.erase(globalEvent.pointerId);
    } else {
        lastPointerPositions_[globalEvent.pointerId] = globalEvent.position;
    }

    // 按下**不**摘 hover 链：链里的裸指针已由 `forgetPointerTarget`（节点 detach
    // 时）摘除，清空反而会让「点一下再把鼠标移开」这条最常见的路径永远收不到
    // Exit（hovering 悬在 true，AutoHideToolbar 的到期收回因此失效）。上游
    // MouseTracker 同样把 track 记录跨 down/up 保持，只按 annotation 集合做差分。

    // 1. 命中链：Down 事件做全新 hitTest 并按指针缓存（上游的
    //    GestureBinding 在 pointer down 时缓存 hitTestResult，同一指针的
    //    move/up 都路由到这条链——内容滚动后坐标体系变化也不失联）。
    //    无缓存（悬停移动等）则现场 hitTest。
    std::vector<RenderObject*> fresh;
    std::vector<RenderObject*>* path = nullptr;
    if (globalEvent.type == PointerEventType::Down) {
        rootNode_->hitTest(globalEvent.position, fresh);
        path = &downPaths_[globalEvent.pointerId];
        *path = fresh;
    } else {
        auto it = downPaths_.find(globalEvent.pointerId);
        if (it != downPaths_.end()) {
            path = &it->second;
        } else {
            rootNode_->hitTest(globalEvent.position, fresh);
            path = &fresh;
        }
    }

    // 2. hover 进入/离开：当命中链是“现场命中测试”的结果（即非按下缓存的
    //    drag 链）时，hover 目标可能变化——对“离开”的对象派发 Exit、“新进入”
    //    的对象派发 Enter（使 Tooltip / MouseRegion 能在鼠标移开时收到通知）。
    //    这同时覆盖真实的 Hover 与无按键的鼠标 Move 事件。
    //    上游 rendering/mouse_tracker.dart 的 `_handleDeviceUpdateMouseEvents`
    //    把命中链当成 annotation 集合做差分：每个变化的节点恰好收一条事件，
    //    既不沿链冒泡、也不对共享祖先重复派发。顺序上 Exit 走 hit-test 序
    //    （前→后 = 最深→最浅），Enter 走逆 hit-test 序（最浅→最深）；本仓
    //    命中链向量是「根 → … → target」，故 Enter 用正序、Exit 用逆序。
    //    与上游的偏差：同层兄弟的先后取决于 hitTest 的遍历序（逆绘制序），
    //    opaque=false 时与上游的反向序可能不同——现无消费者依赖。
    if (path == &fresh) {
        const std::vector<RenderObject*>& now = *path;
        auto contained = [](RenderObject* ro, const std::vector<RenderObject*>& c) {
            for (auto* x : c)
                if (x == ro)
                    return true;
            return false;
        };
        // Exit：旧链有、新链无（先通知最深的节点）。
        for (size_t i = hoverPath_.size(); i-- > 0;) {
            RenderObject* ro = hoverPath_[i];
            if (contained(ro, now))
                continue;
            PointerEvent ev = globalEvent;
            ev.type = PointerEventType::Exit;
            ev.stopPropagation_ = false;
            ev.delta = delta;
            ev.globalPosition = globalEvent.position;
            ev.position = ro->globalToLocal(globalEvent.position);
            ro->handleEvent(ev, HitTestEntry(ro));
        }
        // Enter：新链有、旧链无（先通知最浅的节点）。
        for (RenderObject* ro : now) {
            if (contained(ro, hoverPath_))
                continue;
            PointerEvent ev = globalEvent;
            ev.type = PointerEventType::Enter;
            ev.stopPropagation_ = false;
            ev.delta = delta;
            ev.globalPosition = globalEvent.position;
            ev.position = ro->globalToLocal(globalEvent.position);
            ro->handleEvent(ev, HitTestEntry(ro));
        }
        hoverPath_ = now;
    }

    if (path->empty()) {
        if (globalEvent.type == PointerEventType::Up ||
            globalEvent.type == PointerEventType::Cancel)
            downPaths_.erase(globalEvent.pointerId);
        return;
    }

    // 3. 自 target（链末）向根冒泡：每个节点收到经本地坐标变换后的事件副本。
    for (auto it = path->rbegin(); it != path->rend(); ++it) {
        RenderObject* ro = *it;
        if (!ro)
            continue;
        PointerEvent ev = globalEvent;
        ev.stopPropagation_ = false;  // 每个节点独立判定冒泡
        ev.delta = delta;
        ev.globalPosition = globalEvent.position;
        ev.position = ro->globalToLocal(globalEvent.position);
        HitTestEntry entry(ro);
        ro->handleEvent(ev, entry);
        if (ev.propagationStopped())
            break;
    }

    // 4. 指针抬起后解除 down 链绑定。
    if (globalEvent.type == PointerEventType::Up ||
        globalEvent.type == PointerEventType::Cancel)
        downPaths_.erase(globalEvent.pointerId);
}

void PipelineOwner::forgetPointerTarget(RenderObject* node) {
    if (!node)
        return;
    const auto drop = [node](std::vector<RenderObject*>& path) {
        path.erase(std::remove(path.begin(), path.end(), node), path.end());
    };
    drop(hoverPath_);
    for (auto it = downPaths_.begin(); it != downPaths_.end();) {
        drop(it->second);
        if (it->second.empty())
            it = downPaths_.erase(it);
        else
            ++it;
    }
}

void PipelineOwner::addNeedsLayout(RenderObject* node) {
    assert(node && "addNeedsLayout: null node");
    // Truly idempotent via the membership flag (see header).  The intrusive
    // `next` pointer cannot distinguish "tail of list" from "not in list",
    // which previously let a tail node be re-added and formed a cycle.
    if (node->inLayoutList_)
        return;
    node->inLayoutList_ = true;
    node->nextNeedLayout_ = needsLayoutHead_;
    needsLayoutHead_ = node;
}

void PipelineOwner::addNeedsPaint(RenderObject* node) {
    assert(node && "addNeedsPaint: null node");
    if (node->inPaintList_)
        return;
    node->inPaintList_ = true;
    node->nextNeedPaint_ = needsPaintHead_;
    needsPaintHead_ = node;
}

void PipelineOwner::addNeedsCompositingBits(RenderObject* node) {
    assert(node && "addNeedsCompositingBits: null node");
    if (node->inCompositingBitsList_)
        return;
    node->inCompositingBitsList_ = true;
    node->nextNeedCompositingBits_ = needsCompositingBitsHead_;
    needsCompositingBitsHead_ = node;
}

void PipelineOwner::unlinkNeedsLayout(RenderObject* node) {
    if (!node->inLayoutList_)
        return;
    if (needsLayoutHead_ == node) {
        needsLayoutHead_ = node->nextNeedLayout_;
    } else {
        for (RenderObject* p = needsLayoutHead_; p; p = p->nextNeedLayout_)
            if (p->nextNeedLayout_ == node) { p->nextNeedLayout_ = node->nextNeedLayout_; break; }
    }
    node->nextNeedLayout_ = nullptr;
    node->inLayoutList_ = false;
}

void PipelineOwner::unlinkNeedsPaint(RenderObject* node) {
    if (!node->inPaintList_)
        return;
    if (needsPaintHead_ == node) {
        needsPaintHead_ = node->nextNeedPaint_;
    } else {
        for (RenderObject* p = needsPaintHead_; p; p = p->nextNeedPaint_)
            if (p->nextNeedPaint_ == node) { p->nextNeedPaint_ = node->nextNeedPaint_; break; }
    }
    node->nextNeedPaint_ = nullptr;
    node->inPaintList_ = false;
}

void PipelineOwner::unlinkNeedsCompositingBits(RenderObject* node) {
    if (!node->inCompositingBitsList_)
        return;
    if (needsCompositingBitsHead_ == node) {
        needsCompositingBitsHead_ = node->nextNeedCompositingBits_;
    } else {
        for (RenderObject* p = needsCompositingBitsHead_; p; p = p->nextNeedCompositingBits_)
            if (p->nextNeedCompositingBits_ == node) { p->nextNeedCompositingBits_ = node->nextNeedCompositingBits_; break; }
    }
    node->nextNeedCompositingBits_ = nullptr;
    node->inCompositingBitsList_ = false;
}

void PipelineOwner::flushLayout() {
    // Take the dirty list and clear the head so new marks during flush go
    // into a fresh list (they'll be processed in the next frame).
    RenderObject* dirty = needsLayoutHead_;
    needsLayoutHead_ = nullptr;

    // Process each dirty node.  The RenderView (root) is always in the list
    // on the first frame; its performLayout() cascades down via
    // child_->layout(), which calls performLayout() on each child RenderBox.
    //
    // IMPORTANT: do NOT recursively clear needsLayout_ flags for descendants
    // before the cascade.  RenderBox::layout() checks needsLayout() as a
    // short-circuit — if we clear it here, the cascade from the parent's
    // performLayout() will see clean flags and skip every child, leaving the
    // entire subtree un-laid-out (all widgets at {0,0}).
    //
    // Instead, clear each node's flag when it is actually visited during the
    // cascade (inside RenderBox::layout()).  Nodes that are re-marked dirty
    // during layout will be picked up on the next frame.
    while (dirty) {
        RenderObject* node = dirty;
        dirty = dirty->nextNeedLayout_;
        // Drop from the dirty list (membership flag + link) so any later
        // markNeedsLayout during this flush re-enqueues it onto the fresh
        // list for the next frame instead of corrupting this one.
        node->inLayoutList_ = false;
        node->nextNeedLayout_ = nullptr;

        if (!node->attached()) {
            // Not part of the live tree — leave it fully unlinked.
            continue;
        }

        // Update relayout boundary cache (上游: _updateRelayoutBoundary).
        node->updateRelayoutBoundary();

        // Clear the flag for THIS node only.  Descendants will be cleared
        // when the cascade reaches them.
        node->needsLayout_ = false;

        // Perform layout.
        if (node->sizedByParent()) {
            node->performResize();
        }
        node->performLayout();
        // 上游 的 `_layoutWithoutResize()` 收尾必发 `markNeedsPaint()`：这一批
        // 节点是被显式标脏的，无论尺寸有没有变化都得重录（子节点可能只是挪了
        // 位置，父节点自身尺寸/约束不变，靠上面的约束判定抓不到）。
        node->markNeedsPaint();
        ++layoutFlushCount_;
    }
}

void PipelineOwner::flushCompositingBits() {
    // Process the compositing-bits dirty list: recompute needsCompositing
    // for each node.  This runs AFTER flushLayout (sizes are fresh) and
    // BEFORE flushPaint (PaintingContext reads needsCompositing).
    RenderObject* dirty = needsCompositingBitsHead_;
    needsCompositingBitsHead_ = nullptr;

    while (dirty) {
        RenderObject* node = dirty;
        dirty = dirty->nextNeedCompositingBits_;
        node->inCompositingBitsList_ = false;
        node->nextNeedCompositingBits_ = nullptr;

        if (!node->attached())
            continue;

        // Let the node recompute its compositing requirement.
        bool newValue = node->updateNeedsCompositing();
        node->needsCompositing_ = newValue;
    }
}

void PipelineOwner::flushPaint() {
    // 本帧脏区复位（上游 Rasterizer 的 frameDamage：每帧从空开始累加）。
    frameDamage_ = {};
    damageAll_ = false;

    // Take the dirty list and clear the head so new marks during paint go
    // into a fresh list (they'll be processed in the next frame).
    RenderObject* dirty = needsPaintHead_;
    needsPaintHead_ = nullptr;

    while (dirty) {
        RenderObject* node = dirty;
        dirty = dirty->nextNeedPaint_;
        node->inPaintList_ = false;
        node->nextNeedPaint_ = nullptr;

        if (!node->attached() || !node->owner())
            continue;

        // Resolve to the enclosing repaint boundary (markNeedsPaint normally
        // stops its walk there already; this also covers nodes that reached
        // the list by other paths).
        RenderObject* boundary = node;
        while (!boundary->isRepaintBoundary() && boundary->parent())
            boundary = static_cast<RenderObject*>(boundary->parent());

        // 中间边界还没建过持久图层（从没录制过，例如 setState 新插入的
        // RepaintBoundary）⇒ 单录它会静默无果：repaintBoundaryPaint 拿不到
        // PictureLayer 就直接返回，可脏区已经按它的矩形算进去了，于是「脏区
        // 有、像素不变」= 残影。改成逐层外抛，重录**再外一层**的有图层边界
        // （或直接到根）：祖先的级联会经 PaintingContext::paintChild →
        // pushRepaintBoundary 为它建层。
        while (boundary != rootNode_ && boundary->parent() &&
               !dynamic_cast<PictureLayer*>(boundary->currentLayer())) {
            RenderObject* outer = static_cast<RenderObject*>(boundary->parent());
            while (outer->parent() && !outer->isRepaintBoundary())
                outer = static_cast<RenderObject*>(outer->parent());
            boundary = outer;
        }

        const bool repaint = node->needsPaint();
        if (!repaint)
            continue;

        // Re-record the boundary's subtree into the Layer tree.  The
        // composite phase (RenderView::composite) then rasterizes whatever
        // the paint phase recorded — this is the real painting step that
        // used to be a stub.

        // 脏区累积（damage region）：本帧真正变化的像素只来自被重录的边界。
        // 根边界（RenderView）重录 = 整屏，置 damageAll_；其余边界取其窗口
        // 坐标矩形（与 repaintBoundaryPaint 录制 cull rect 同一算法：
        // layerOffset + paintBounds），并 union 其持久图层上一次记录的 bounds
        //（覆盖边界缩小/位移后残留的旧像素区）。
        if (boundary == rootNode_) {
            damageAll_ = true;
        } else {
            const Rect pb = boundary->paintBounds();
            const Offset lo = boundary->layerOffset();
            Rect cur{lo.dx + pb.x, lo.dy + pb.y, pb.width, pb.height};
            Rect old;
            auto* pl = dynamic_cast<PictureLayer*>(boundary->currentLayer());
            if (pl)
                old = pl->bounds();
            // 把脏区裁剪到祖先 ClipRectLayer（通常是视口）的裁剪矩形：滚出
            // 视口的边界，其不可见部分不应进入脏区。否则整页所有 demo 的窗口
            // 矩形并集会撑满内容高度（含屏幕外与工具栏区），既让部分合成 /
            // GPU 回读区域膨胀（CPU 高），又在持久 surface 上因 clear 覆盖到
            // 不该动的相邻区而产生拖影。裁剪矩形与脏区同为窗口空间，直接求交。
            auto clipToAncestors = [](Layer* layer, Rect r) {
                for (Layer* p = layer ? static_cast<Layer*>(layer->parent()) : nullptr; p;
                     p = static_cast<Layer*>(p->parent()))
                    if (auto* crl = dynamic_cast<ClipRectLayer*>(p))
                        r = r.intersect(crl->clipRect());
                return r;
            };
            if (pl) {
                cur = clipToAncestors(pl, cur);
                old = clipToAncestors(pl, old);
            }
            frameDamage_ = frameDamage_.unite(cur);
            frameDamage_ = frameDamage_.unite(old);
        }

        boundary->repaintBoundaryPaint();

        // 清标志必须在重录**之后**：pushRepaintBoundary 用「边界自身
        // needsPaint」决定重录还是复用缓存图，而祖先整帧重录时它的级联会走到
        // 每一个中间边界。提前清 ⇒ 祖先级联把刚入队的脏边界当成干净边界复用
        // ⇒ 旧像素（本批第一次接线就是这么测出来的）。录制后清既保留原意
        // （只有进过脏链的节点会被清；标志留着会让下一次 markNeedsPaint 早退
        // 吞掉请求），也把「录制期自标脏」（自驱动动画在 paint 里重绘自己）
        // 一并落到干净标志上，下一次 flush 重新入队。
        std::function<void(RenderObject*)> clearFlags = [&](RenderObject* n) {
            n->needsPaint_ = false;
            n->visitChildren([&](AbstractNode* child) {
                clearFlags(static_cast<RenderObject*>(child));
            });
        };
        clearFlags(boundary);
    }
}

}  // namespace cui_core
