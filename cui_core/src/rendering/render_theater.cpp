/// @file rendering/render_theater.cpp
/// @brief RenderTheater 实现 —— Overlay 的 onstage/offstage 渲染语义。

#include "cui/rendering/render_theater.h"

#include <algorithm>

#include "cui/rendering/painting_context.h"
#include "cui/rendering/render_box_widgets.h"

namespace cui {

void RenderTheater::setupParentData(RenderObject* child) {
    // 上游 `_RenderTheater` 复用 `StackParentData`（也是 Positioned 的目标）。
    if (child && !dynamic_cast<StackParentData*>(child->parentData()))
        child->setParentData(new StackParentData());
}

void RenderTheater::refreshOnstage() {
    onstage_.clear();
    const auto& all = children();
    size_t start = std::min(skipCount_, all.size());
    for (size_t i = start; i < all.size(); ++i)
        if (all[i])
            onstage_.push_back(all[i]);
}

void RenderTheater::insertChild(RenderObject* child, RenderObject* after) {
    RenderBoxContainer::insertChild(child, after);
    refreshOnstage();
}

void RenderTheater::removeChild(RenderObject* child) {
    RenderBoxContainer::removeChild(child);
    refreshOnstage();
}

void RenderTheater::moveChild(RenderObject* child, RenderObject* after) {
    RenderBoxContainer::moveChild(child, after);
    refreshOnstage();
}

void RenderTheater::reorderChildren(const std::vector<RenderObject*>& desired) {
    RenderBoxContainer::reorderChildren(desired);
    refreshOnstage();
}

void RenderTheater::performLayout() {
    // 上游的 _RenderTheater.performLayout（overlay.dart:1473-1489）：
    //   size = constraints.biggest（有限时），否则由尺寸决定子节点决定；
    //   每个 onstage 子节点用 BoxConstraints.tight(size) 布局。
    //
    // offstage 子节点完全不参与布局。
    refreshOnstage();

    if (constraints_.hasBoundedWidth() && constraints_.hasBoundedHeight())
        setSize(constraints_.biggest());
    else
        setSize(constraints_.smallest());

    for (RenderObject* child : onstage_) {
        auto* pd = dynamic_cast<StackParentData*>(child->parentData());
        if (pd && pd->isPositioned()) {
            // 上游 `layoutChild`：定位条目按 Stack 算术摆放（Hero 的 shuttle
            // 条目），不再被拉到整屏。
            RenderStack::layoutPositionedChild(child, pd, size_,
                                               RenderStack::Alignment::TopStart);
            continue;
        }
        child->layout(BoxConstraints::tight(size_));
        if (pd)
            pd->offset = Offset(0, 0);
    }
}

void RenderTheater::paint(PaintingContext& context, const Offset& offset) {
    // 上游的 _RenderTheater.paint（overlay.dart:1539-1553）：
    // 只绘制 onstage 子节点，按 children 顺序（从底到顶）依次绘制；
    // 定位条目的偏移在父节点坐标系里累加（上游 `childParentData.offset + offset`）。
    for (RenderObject* child : onstage_) {
        const ParentData* pd = child->parentData();
        context.paintChild(child, pd ? offset + pd->offset : offset);
    }
}

bool RenderTheater::hitTestChildren(const Offset& position, std::vector<RenderObject*>& result) {
    // 上游的命中测试顺序：从最上层（列表末尾）往下测。
    for (auto it = onstage_.rbegin(); it != onstage_.rend(); ++it) {
        RenderObject* child = *it;
        if (!child)
            continue;
        Offset local = position;
        if (child->parentData())
            local = local - child->parentData()->offset;
        if (child->hitTest(local, result))
            return true;
    }
    return false;
}

}  // namespace cui_core
