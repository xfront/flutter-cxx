/// @file rendering/render_scroll.cpp
/// @brief RenderSingleChildViewport / RenderScrollbar 实现。

#include "cui/rendering/render_scroll.h"

#include <algorithm>

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRRect.h>

#include "cui/foundation/theme.h"
#include "cui/rendering/painting_context.h"

namespace cui {

// ── RenderSingleChildViewport ──────────────────────────────────────────────
Offset RenderSingleChildViewport::contentOffset() const {
    // 上游的 `_paintOffsetForPosition`（widgets/single_child_scroll_view.dart）。
    // Up/Left 时内容贴视口的另一端生长，故偏移要在「内容长 - 视口长」上再加减。
    const Size childSize = child_ ? child_->size() : Size{0, 0};
    switch (axisDirection_) {
        case AxisDirection::Up:
            return Offset(0.0, offset_ - childSize.height + size_.height);
        case AxisDirection::Left:
            return Offset(offset_ - childSize.width + size_.width, 0.0);
        case AxisDirection::Right:
            return Offset(-offset_, 0.0);
        case AxisDirection::Down:
            break;
    }
    return Offset(0.0, -offset_);
}

void RenderSingleChildViewport::performLayout() {
    const Axis axis = axisDirectionToAxis(axisDirection_);
    const Size maxSize = constraints_.biggest();
    if (!child_) {
        setSize(maxSize);
        return;
    }

    const bool mainBounded = axis == Axis::Vertical ? constraints_.hasBoundedHeight()
                                                    : constraints_.hasBoundedWidth();
    const bool crossBounded = axis == Axis::Vertical ? constraints_.hasBoundedWidth()
                                                     : constraints_.hasBoundedHeight();
    // 主轴无界让内容自然伸展（滚动的前提）；交叉轴有界时紧锁为 viewport 尺寸
    // （子节点交叉轴撑满视口）。无界的那一轴不能拿 biggest()（= kInfinity 哨兵）
    // 当尺寸 —— 本视口被另一个滚动视口的可滚主轴包着时（CodeEditor 的横滚嵌在
    // 纵滚里），交叉轴随内容收缩，否则视口自称 1e18 长，外层滚动范围全错。
    const double crossMax =
        axis == Axis::Vertical ? maxSize.width : maxSize.height;
    const BoxConstraints c =
        axis == Axis::Vertical
            ? BoxConstraints(crossBounded ? crossMax : 0.0,
                            crossBounded ? crossMax : BoxConstraints::kInfinity, 0.0,
                            BoxConstraints::kInfinity)
            : BoxConstraints(0.0, BoxConstraints::kInfinity,
                             crossBounded ? crossMax : 0.0,
                             crossBounded ? crossMax : BoxConstraints::kInfinity);
    child_->layout(c);

    // 内容原点即 viewport 原点；滚动平移发生在 paint/hitTest 阶段。
    if (child_->parentData())
        child_->parentData()->offset = Offset(0, 0);

    const Size childSize = child_->size();
    const Size resolved =
        axis == Axis::Vertical
            ? Size(crossBounded ? crossMax : childSize.width,
                   mainBounded ? maxSize.height : childSize.height)
            : Size(mainBounded ? maxSize.width : childSize.width,
                   crossBounded ? crossMax : childSize.height);
    setSize(constraints_.constrain(resolved));

    double content = axis == Axis::Vertical ? childSize.height : childSize.width;
    double view = axis == Axis::Vertical ? size_.height : size_.width;
    double maxExtent = std::max(0.0, content - view);
    // 上游 _RenderSingleChildViewport.performLayout 的顺序：先按新的内容长度把
    // 当前位置 correctBy 夹回 [min, max]，再 applyContentDimensions。内容缩短时
    // （典型：侧栏底部的 folder 收起）只夹 controller 是不够的 —— 视口的绘制偏移
    // 由 widget 在 build 期推下来，布局期不回推 ⇒ 仍按旧偏移绘制，底部空出一条
    // 「剩余空间」。就地夹住，同一帧就把上面的内容顶下来填满视口。
    if (offset_ > maxExtent)
        setOffset(maxExtent);
    else if (offset_ < 0.0)
        setOffset(0.0);  // 上游的 _minScrollExtent 恒为 0
    if (onMaxExtentChanged_)
        onMaxExtentChanged_(maxExtent, view);
}

void RenderSingleChildViewport::performPaint(PaintingContext& context, const Offset& offset) {
    if (!child_)
        return;

    const Offset childAt = offset + contentOffset();
    SkCanvas* c = context.canvas();
    if (!c) {
        // headless：无 canvas 时退化为直接平移绘制子树。
        context.paintChild(child_, childAt);
        return;
    }
    // 用 ClipRectLayer 裁剪子树，而非 canvas 级 clipRect。
    // 子树里可能含 RepaintBoundary 后代（如动画页的 CustomPaint），它们
    // 录制时是作为独立 PictureLayer 追加到当前容器下的兄弟图层；canvas
    // 的 clipRect 只约束录进同一张 picture 的绘制指令，管不到这些兄弟层，
    // 于是滚出视口的内容会「逃逸」画到视口之外。把整棵子树包进
    // ClipRectLayer，raster 时的裁剪才对包括嵌套边界层在内的所有子层生效。
    // 注意：pushClip 的矩形与子偏移在容器（根）空间解释，而 performPaint
    // 的 offset 是画布局部坐标（真实定位靠 currentOffset_ 累积），故容器
    // 空间原点是 currentOffset() + offset，不能直接用 offset。
    const Offset origin = context.currentOffset() + offset;
    context.pushClip(Rect{origin.dx, origin.dy, size_.width, size_.height}, child_,
                     origin + contentOffset());
}

bool RenderSingleChildViewport::hitTestChildren(const Offset& position,
                                     std::vector<RenderObject*>& result) {
    if (!child_)
        return false;
    // viewport 本地坐标 → 内容坐标：把滚动偏移加回来。
    Offset local = position - contentOffset();
    return child_->hitTest(local, result);
}

void RenderSingleChildViewport::applyPaintTransform(const RenderObject* child,
                                                    Matrix4& transform) const {
    // 基类先叠加 child 的 parentData->offset（本 viewport 恒置 0）。
    RenderProxyBox::applyPaintTransform(child, transform);
    // 再把只作用于 paint/hitTest 的滚动偏移补进变换，令 localToGlobal 生效。
    if (child == child_) {
        const Offset co = contentOffset();
        transform.translate(co.dx, co.dy);
    }
}

// ── RenderScrollbar ────────────────────────────────────────────────────────
void RenderScrollbar::performLayout() {
    if (!child_) {
        setSize(constraints_.biggest());
        return;
    }
    // 滚动条是覆盖层：自身占满父级给定区域（thumb 绘在最右缘），内容子节点按
    // 原始约束布局、不被收窄，轨道/thumb 压在内容之上（与 Flutter Scrollbar 一致，
    // 也和本仓 CustomScrollView 自带的滚动条行为一致）。
    // 注意：不能在布局期把条带从宽度里扣掉 —— 否则嵌套可滚动容器时，外层垂直
    // 滚动条的条带会把内层水平滚动视图的交叉轴（宽）压窄，内层视口尺寸与滚动
    // 范围随之算错（见 testNestedAxisViewport）。
    child_->layout(constraints_);
    const Size cs = child_->size();
    setSize(constraints_.constrain(cs));
}

RenderScrollbar::~RenderScrollbar() {
    if (controller_)
        controller_->removePaintObserver(this);
}

void RenderScrollbar::setController(ScrollController* c) {
    if (c == controller_)
        return;
    if (controller_)
        controller_->removePaintObserver(this);
    controller_ = c;
    if (controller_)
        controller_->addPaintObserver(this);
    markNeedsPaint();
}

Rect RenderScrollbar::trackRect() const {
    if (size_.width <= 0.0 || size_.height <= 0.0)
        return Rect{};
    // 上游：trackSize = Size(thickness + 2*crossAxisMargin, _trackExtent)，
    // trackOffset = Offset(thumb.x - crossAxisMargin, padding.top)。
    double trackWidth = thickness_ + 2.0 * crossAxisMargin_;
    return Rect{size_.width - trackWidth, 0.0, trackWidth, size_.height};
}

Rect RenderScrollbar::thumbRect() const {
    double off = controller_ ? controller_->offset() : 0.0;
    double maxExtent = controller_ ? controller_->maxScrollExtent() : 0.0;
    double viewportExtent = controller_ ? controller_->viewportExtent() : 0.0;
    if (maxExtent <= 0.0 || viewportExtent <= 0.0 || size_.height <= 0.0)
        return Rect{};
    // 上游 _traversableTrackExtent = 轨道长 - 2*mainAxisMargin；thumb 在这段里走。
    double traversable = size_.height - 2.0 * mainAxisMargin_;
    double fraction =
        std::clamp(viewportExtent / (viewportExtent + maxExtent), 0.0, 1.0);
    // 上游 _setThumbExtent：max(min(traversable, minLength), traversable*fraction)。
    double len = std::max(std::min(traversable, kMinThumbExtent), traversable * fraction);
    double y = mainAxisMargin_ + (off / maxExtent) * (traversable - len);
    // 上游：x = size.width - thickness - crossAxisMargin - padding.right。
    return Rect{size_.width - thickness_ - crossAxisMargin_, y, thickness_, len};
}

bool RenderScrollbar::hitTestSelf(const Offset& position) const {
    // 不可滚动时轨道只是装饰，不接管指针（上游 `_lastMetricsAreScrollable`）。
    if (!controller_ || controller_->maxScrollExtent() <= 0.0)
        return false;
    const Rect track = trackRect();
    return position.dx >= track.x && position.dx < track.right() && position.dy >= track.y &&
           position.dy < track.bottom();
}

bool RenderScrollbar::hitTestChildren(const Offset& position,
                                      std::vector<RenderObject*>& result) {
    if (hitTestSelf(position))
        return false;
    return RenderProxyBox::hitTestChildren(position, result);
}

double RenderScrollbar::trackToScroll(double thumbTop) const {
    // 上游 `ScrollbarPainter.getTrackToScroll`：
    //   scrollableExtent * localPos / (traversableTrackExtent - thumbExtent)
    const double traversable = size_.height - 2.0 * mainAxisMargin_;
    const double movable = traversable - thumbRect().height;
    if (!controller_ || movable <= 0.0)
        return 0.0;
    return (thumbTop - mainAxisMargin_) / movable * controller_->maxScrollExtent();
}

void RenderScrollbar::handleEvent(const PointerEvent& event, const HitTestEntry& entry) {
    (void)entry;
    if (!controller_ || controller_->maxScrollExtent() <= 0.0)
        return;
    const bool primary = event.pointerKind != PointerDeviceKind::mouse ||
                         (event.buttons & kPrimaryMouseButton) != 0;

    switch (event.type) {
        case PointerEventType::Down: {
            if (!primary || !hitTestSelf(event.position))
                return;
            const Rect thumb = thumbRect();
            if (thumb.contains(Point{event.position.dx, event.position.dy})) {
                dragging_ = true;
                dragPointer_ = event.pointerId;
                dragStartThumbTop_ = thumb.y;
                dragStartY_ = event.position.dy;
            } else {
                // 轨道点击：向点击方向翻一页（上游 handleTrackTapDown，
                // 页长与本仓键盘 PgUp/PgDn 同一口径）。
                const double page =
                    std::max(40.0, controller_->viewportExtent() - 16.0);
                const bool below = event.position.dy > thumb.y;
                controller_->jumpTo(controller_->offset() + (below ? page : -page));
            }
            // 轨道上的手势归滚动条，不再冒泡给内容/祖先滚动视图。
            event.stopPropagation();
            break;
        }
        case PointerEventType::Move: {
            if (!dragging_ || event.pointerId != dragPointer_)
                return;
            // 按下点相对 thumb 顶端的位移直接换算成新的 thumb 顶端位置，
            // 再按比例换成滚动偏移 —— 向下拖 ⇒ 偏移增大（内容向下滚）。
            const double thumbTop = dragStartThumbTop_ + (event.position.dy - dragStartY_);
            controller_->jumpTo(trackToScroll(thumbTop));
            event.stopPropagation();
            break;
        }
        case PointerEventType::Scroll: {
            // 轨道盖住了内容区右缘，内容子树不在命中链里：滚轮落在条带上时
            // 在子树里补一次命中并按同一条「target → 根」顺序转发，保持
            // physics 与 ScrollNotification 的原路径不变（子节点未必是那个
            // 挂 onScroll 的 Listener，不能只转给直接子节点）。
            if (!child_)
                return;
            // 内容视口被条带挤窄了 inset（本仓 Scrollbar 的 performLayout 会
            // 扣掉条带宽度），于是落在条带上的滚轮（x∈[W-inset, W]）相对视口
            // 已越界，直接 child_->hitTest 会整条漏掉内容。条带只是覆盖层，
            // 把坐标夹回视口范围内再命中，内容子树照样被命中（穿透语义）。
            Offset hp = event.position;
            if (child_->size().width > 0.0 && hp.dx >= child_->size().width)
                hp.dx = child_->size().width - 1.0;
            if (child_->size().height > 0.0 && hp.dy >= child_->size().height)
                hp.dy = child_->size().height - 1.0;
            std::vector<RenderObject*> chain;
            child_->hitTest(hp, chain);
            PointerEvent forwarded = event;
            forwarded.position = hp;
            bool claimed = false;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                RenderObject* ro = *it;
                if (!ro)
                    continue;
                forwarded.stopPropagation_ = false;
                ro->handleEvent(forwarded, HitTestEntry(ro));
                if (forwarded.propagationStopped()) {
                    claimed = true;
                    break;
                }
            }
            // 子树里没人认领（里层已滚到底/顶，或压根没有滚动视图）⇒ 不拦，
            // 让这份滚轮继续冒泡给外层滚动视图，与 Scrollable 的认领规则同口径。
            if (claimed)
                event.stopPropagation();
            break;
        }
        case PointerEventType::Up:
        case PointerEventType::Cancel:
            if (event.pointerId == dragPointer_) {
                dragging_ = false;
                dragPointer_ = -1;
            }
            break;
        default:
            break;
    }
}

Color RenderScrollbar::resolveThumbColor() const {
    return thumbColor_.value_or(theme().scrollbarThumb);
}

Color RenderScrollbar::resolveTrackColor() const {
    return trackColor_.value_or(theme().outlineVariant);
}

void RenderScrollbar::performPaint(PaintingContext& context, const Offset& offset) {
    // 先画被包裹的内容（代理盒默认转发）。
    RenderProxyBox::performPaint(context, offset);

    SkCanvas* c = context.canvas();
    double maxExtent = controller_ ? controller_->maxScrollExtent() : 0.0;
    double viewportExtent = controller_ ? controller_->viewportExtent() : 0.0;
    if (!c || maxExtent <= 0.0 || viewportExtent <= 0.0)
        return;

    // 轨道 + thumb 都画成胶囊（圆角取短边的一半），绘制顺序同 上游：先轨道后 thumb。
    auto drawPill = [c, &offset](Rect r, const Color& color) {
        if (r.width <= 0.0 || r.height <= 0.0)
            return;
        SkRect rect = SkRect::MakeXYWH(SkScalar(offset.dx + r.x), SkScalar(offset.dy + r.y),
                                       SkScalar(r.width), SkScalar(r.height));
        double radius = std::min(r.width, r.height) / 2.0;
        SkRRect rr = SkRRect::MakeRectXY(rect, SkScalar(radius), SkScalar(radius));
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);
        p.setColor4f(SkColor4f{static_cast<float>(color.r), static_cast<float>(color.g),
                               static_cast<float>(color.b), static_cast<float>(color.a)},
                     nullptr);
        c->drawRRect(rr, p);
    };
    drawPill(trackRect(), resolveTrackColor());
    drawPill(thumbRect(), resolveThumbColor());
}

}  // namespace cui_core
