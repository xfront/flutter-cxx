/// @file widgets/virtual_list.cpp
/// @brief RenderVirtualList / VirtualList 实现（零逐行节点虚拟列表）。

#include "cui/widgets/virtual_list.h"

#include <include/core/SkCanvas.h>

#include "cui/rendering/painting_context.h"

namespace cui {

// ── VirtualListState（需要 RenderVirtualList 完整类型）──────────────────────

void VirtualListState::invalidatePaint() {
    if (owner_)
        owner_->markNeedsPaint();
}

void VirtualListState::invalidateLayout() {
    if (owner_) {
        owner_->markNeedsLayout();
        owner_->markNeedsPaint();
    }
}

// ── RenderVirtualList ────────────────────────────────────────────────────────

RenderVirtualList::RenderVirtualList(SharedPtr<VirtualListState> state,
                                     SharedPtr<ScrollController> controller)
    : state_(std::move(state)), controller_(std::move(controller)) {
    if (!controller_)
        controller_ = cui::make_shared<ScrollController>();
    state_->setOwner(this);
    // ScrollController 的 paint observer 只收 RenderScrollbar；这里用 listener
    // 做同样的 markNeedsPaint，不触发 setState（layout 期调用也安全）。
    listener_ = controller_->addListener([this] { markNeedsPaint(); });
}

RenderVirtualList::~RenderVirtualList() {
    if (controller_ && listener_ != ScrollController::kInvalidListener)
        controller_->removeListener(listener_);
    state_->setOwner(nullptr);
}

std::pair<std::size_t, std::size_t> RenderVirtualList::visibleRange() const {
    return state_->visibleRange(controller_->offset(), size_.height);
}

double RenderVirtualList::maxScroll() const {
    return std::max(0.0, state_->totalHeight() - size_.height);
}

std::optional<Rect> RenderVirtualList::thumbRect() const {
    const double vp = size_.height;
    const double max = maxScroll();
    if (vp <= 0.0 || max <= 0.0)
        return std::nullopt;
    const double thumb = std::min(vp, std::max(kVirtualListMinThumb, vp * vp / (vp + max)));
    const double travel = std::max(0.0, vp - thumb);
    const double top = (controller_->offset() / max) * travel;
    return Rect{size_.width - kVirtualListScrollbarWidth, top,
                kVirtualListScrollbarWidth, thumb};
}

void RenderVirtualList::scrollToTrack(double thumbTop) {
    const double vp = size_.height;
    const double max = maxScroll();
    if (vp <= 0.0 || max <= 0.0)
        return;
    const double thumb = std::min(vp, std::max(kVirtualListMinThumb, vp * vp / (vp + max)));
    const double travel = std::max(0.0, vp - thumb);
    const double top = std::clamp(thumbTop, 0.0, travel);
    const double next = travel > 0.0 ? top / travel * max : 0.0;
    if (std::fabs(next - controller_->offset()) < 1e-9)
        return;
    controller_->forceOffset(next);
    markNeedsPaint();
}

bool RenderVirtualList::scrollBy(double delta) {
    const double old = controller_->offset();
    const double next = std::clamp(old + delta, 0.0, maxScroll());
    if (std::fabs(next - old) < 1e-9)
        return false;
    controller_->forceOffset(next);
    markNeedsPaint();
    return true;
}

std::optional<std::size_t> RenderVirtualList::rowAt(double localY) const {
    if (state_->itemCount() == 0)
        return std::nullopt;
    const double sy = std::clamp(controller_->offset(), 0.0, maxScroll());
    const std::size_t idx = state_->indexAtY(localY + sy);
    const double top = state_->offsetAt(idx) - sy;
    const double h = std::max(1.0, state_->model()->itemHeight(idx));
    if (localY >= top && localY < top + h)
        return idx;
    return std::nullopt;
}

void RenderVirtualList::performLayout() {
    const double total = state_->totalHeight();
    const double w = constraints().hasBoundedWidth() ? constraints().maxWidth : 240.0;
    const double h = constraints().hasBoundedHeight()
                         ? constraints().maxHeight
                         : std::min(total, 320.0);
    setSize(constraints().constrain(Size{w, h}));
    // layout 期静默夹回 offset，不通知 listener（避免重入）。
    controller_->applyExtents(std::max(0.0, total - size_.height), size_.height);
}

void RenderVirtualList::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c || size_.width <= 0.0 || size_.height <= 0.0)
        return;

    c->save();
    c->translate(static_cast<SkScalar>(offset.dx), static_cast<SkScalar>(offset.dy));
    Painter p(c, kVirtualListFontFamily);
    const double max = maxScroll();
    const double sy = std::clamp(controller_->offset(), 0.0, max);
    const bool hasScrollbar = max > 0.0;
    const double contentW = std::max(0.0, size_.width -
        (hasScrollbar ? kVirtualListScrollbarWidth : 0.0));
    const Theme& t = theme();

    p.save();
    p.clipRect(Rect{0.0, 0.0, size_.width, size_.height});
    p.setSourceColor(t.surface);
    p.rectangle(Rect{0.0, 0.0, size_.width, size_.height});
    p.fill();

    const std::size_t n = state_->itemCount();
    if (n > 0) {
        const auto range = state_->visibleRange(sy, size_.height);
        double y = state_->offsetAt(range.first) - sy;
        const int64_t selected = state_->selected();
        const int64_t hovered = state_->hovered();
        for (std::size_t idx = range.first; idx < n && y < size_.height; ++idx) {
            const double ih = std::max(1.0, state_->model()->itemHeight(idx));
            const Rect row{0.0, y, contentW, ih};
            const int64_t rowId = static_cast<int64_t>(idx);
            if (selected == rowId) {
                p.setSourceColor(t.primaryLight);
                p.rectangle(row);
                p.fill();
            } else if (idx % 2 == 1) {
                p.setSourceColor(t.surfaceVariant);
                p.rectangle(row);
                p.fill();
            }
            if (hovered == rowId && selected != rowId) {
                p.setSourceColor(t.hoverOverlay);
                p.rectangle(row);
                p.fill();
            }
            state_->model()->paintItem(p, idx, row, selected == rowId, hovered == rowId);
            y += ih;
        }
    }
    p.restore();

    p.setSourceColor(t.borderLight);
    p.setLineWidth(1.0);
    p.rectangle(Rect{0.5, 0.5, std::max(0.0, size_.width - 1.0),
                     std::max(0.0, size_.height - 1.0)});
    p.stroke();

    if (hasScrollbar) {
        if (auto r = thumbRect()) {
            p.setSourceColor(t.scrollbarThumb);
            p.fillRoundedRect(*r, 3.0);
        }
    }
    c->restore();
}

bool RenderVirtualList::hitTestSelf(const Offset& position) const {
    return position.dx >= 0.0 && position.dy >= 0.0 &&
           position.dx < size_.width && position.dy < size_.height;
}

void RenderVirtualList::handleEvent(const PointerEvent& event, const HitTestEntry&) {
    const Point point{event.position.dx, event.position.dy};
    switch (event.type) {
        case PointerEventType::Scroll:
            if (!scrollBy(event.scrollDeltaY))
                return;
            event.stopPropagation();
            return;

        case PointerEventType::Down: {
            if (!hitTestSelf(event.position))
                return;
            if (auto thumb = thumbRect()) {
                if (thumb->contains(point)) {
                    state_->drag().active = true;
                    state_->drag().pointerId = event.pointerId;
                    state_->drag().startY = event.position.dy;
                    state_->drag().startThumbTop = thumb->y;
                    event.stopPropagation();
                    return;
                }
                // 点击轨道空白处：按页翻动。
                if (event.position.dx >= size_.width - kVirtualListScrollbarWidth) {
                    const double page = std::max(40.0, size_.height - 16.0);
                    const double dir = event.position.dy >= thumb->y + thumb->height / 2.0
                                           ? 1.0 : -1.0;
                    scrollBy(dir * page);
                    event.stopPropagation();
                    return;
                }
            }
            if (auto idx = rowAt(event.position.dy))
                state_->setSelected(static_cast<int64_t>(*idx));
            event.stopPropagation();
            return;
        }

        case PointerEventType::Move:
        case PointerEventType::Hover: {
            auto& drag = state_->drag();
            if (drag.active && drag.pointerId == event.pointerId) {
                scrollToTrack(drag.startThumbTop + (event.position.dy - drag.startY));
                event.stopPropagation();
                return;
            }
            if (hitTestSelf(event.position)) {
                if (auto idx = rowAt(event.position.dy))
                    state_->setHovered(static_cast<int64_t>(*idx));
                else
                    state_->setHovered(-1);
            } else {
                state_->setHovered(-1);
            }
            event.stopPropagation();
            return;
        }

        case PointerEventType::Up:
        case PointerEventType::Cancel: {
            auto& drag = state_->drag();
            if (drag.active && drag.pointerId == event.pointerId) {
                drag.active = false;
                drag.pointerId = -1;
                event.stopPropagation();
            }
            return;
        }

        case PointerEventType::Exit:
            state_->setHovered(-1);
            return;
        case PointerEventType::Enter:
        default:
            return;
    }
}

// ── VirtualList ──────────────────────────────────────────────────────────────

VirtualList::VirtualList(SharedPtr<VirtualListModel> model,
                         SharedPtr<ScrollController> controller, KeyPtr key)
    : LeafRenderObjectWidget(std::move(key)),
      state_(cui::make_shared<VirtualListState>(std::move(model))),
      controller_(controller ? std::move(controller) : cui::make_shared<ScrollController>()) {}

VirtualList::VirtualList(SharedPtr<VirtualListState> state,
                         SharedPtr<ScrollController> controller, KeyPtr key)
    : LeafRenderObjectWidget(std::move(key)),
      state_(std::move(state)),
      controller_(controller ? std::move(controller) : cui::make_shared<ScrollController>()) {}

void VirtualList::scrollToIndex(std::size_t index) {
    const double target = state_->offsetAt(index);
    if (controller_->hasClients())
        controller_->jumpTo(target);
    else
        controller_->forceOffset(target);
}

void VirtualList::scrollToOffset(double offset) { controller_->jumpTo(offset); }

RenderObject* VirtualList::createRenderObject() {
    return new RenderVirtualList(state_, controller_);
}

void VirtualList::updateRenderObject(RenderObject* ro) {
    if (auto* r = dynamic_cast<RenderVirtualList*>(ro)) {
        // state 是共享的，模型/选择态的更新已经被 VirtualListState 自己标脏；
        // 这里只需确保 owner 指回当前 RenderObject（重建后可能曾短暂为空）。
        r->state()->setOwner(r);
    }
}

}  // namespace cui
