/// @file rendering/render_box.cpp
/// @brief RenderBox layout protocol + RenderProxyBox / RenderBoxContainer /
///        RenderShiftedBox containers.

#include "cui/rendering/render_box.h"

#include <algorithm>
#include <cassert>

#include "cui/rendering/painting_context.h"

namespace cui {

// ── RenderBox ───────────────────────────────────────────────────────────────

void RenderBox::performLayout() {
    // 默认盒模型：尺寸取约束允许的最大值（上游的 RenderBox 默认）。
    setSize(constraints_.biggest());
}

double RenderBox::computeIntrinsics(IntrinsicDimension dim, double input) {
    // 上游的 RenderBox._computeIntrinsics：按 (维度, 入参) 记忆化后分派到
    // compute 钩子。缓存的意义在于 intrinsic 计算最坏是 O(N²) 的，父节点会
    // 反复问同一个子节点同一个维度。
    auto& cache = intrinsicCache_.byDimension[static_cast<size_t>(dim)];
    auto it = cache.find(input);
    if (it != cache.end())
        return it->second;
    double value = 0.0;
    switch (dim) {
        case IntrinsicDimension::MinWidth:
            value = computeMinIntrinsicWidth(input);
            break;
        case IntrinsicDimension::MaxWidth:
            value = computeMaxIntrinsicWidth(input);
            break;
        case IntrinsicDimension::MinHeight:
            value = computeMinIntrinsicHeight(input);
            break;
        case IntrinsicDimension::MaxHeight:
            value = computeMaxIntrinsicHeight(input);
            break;
    }
    cache.emplace(input, value);
    return value;
}

double RenderBox::getMinIntrinsicWidth(double height) {
    assert(height >= 0.0);  // 上游：入参不得为负（可以是 kInfinity）
    return computeIntrinsics(IntrinsicDimension::MinWidth, height);
}

double RenderBox::getMaxIntrinsicWidth(double height) {
    assert(height >= 0.0);
    return computeIntrinsics(IntrinsicDimension::MaxWidth, height);
}

double RenderBox::getMinIntrinsicHeight(double width) {
    assert(width >= 0.0);
    return computeIntrinsics(IntrinsicDimension::MinHeight, width);
}

double RenderBox::getMaxIntrinsicHeight(double width) {
    assert(width >= 0.0);
    return computeIntrinsics(IntrinsicDimension::MaxHeight, width);
}

void RenderBox::markNeedsLayout() {
    // 上游的 RenderBox.markNeedsLayout：缓存非空说明父节点的布局算法查过
    // 本盒的 intrinsic（无法区分「只用于绘制」，故保守地当作布局依赖）。
    // 这种依赖可以越过 relayout boundary，所以把脏标直接交给父节点。
    if (intrinsicCache_.clear() && parent() != nullptr && !renderTeardown()) {
        markParentNeedsLayout();
        return;
    }
    RenderObject::markNeedsLayout();
}

bool RenderBox::hitTest(const Offset& position) const {
    // 上游 的第一步：本盒尺寸是否包含该点（溢出盒子外的子节点点不到）。
    if (!size().contains(position))
        return false;
    // Check self first (e.g. a background that fills the box).
    if (hitTestSelf(position))
        return true;
    // Then children (reverse paint order — last child is topmost).
    return hitTestChildren(position);
}

bool RenderBox::hitTest(const Offset& position, std::vector<RenderObject*>& result) {
    // 上游 的 RenderBox.hitTest 以 `if (_size.contains(position))` 开头，于是命中链
    // 被每一层盒子的尺寸截断：**溢出到盒子外的子节点点不到**。这不是裁剪框的语义
    // （RenderClipRect 只裁绘制），但 SizeTransition 那套「Align 用因子收缩自身、
    // 子节点仍按展开尺寸布局」正是靠它让被裁掉的內容同时失去命中区。本仓基类
    // RenderObject::hitTest 没有尺寸判定，所以在盒子层补齐。
    if (!size().contains(position))
        return false;
    // 上游 的 RenderBox 复用基类的命中测试流程：
    // 先 hitTestSelf，再倒序 hitTestChildren。
    return RenderObject::hitTest(position, result);
}

// ── RenderProxyBox ──────────────────────────────────────────────────────────

void RenderProxyBox::insertChild(RenderObject* child, RenderObject* after) {
    (void)after;
    if (!child || child_ == child)
        return;
    if (child_)
        dropChild(child_);
    child_ = child;
    adoptChild(child_);
    markNeedsLayout();
}

void RenderProxyBox::removeChild(RenderObject* child) {
    if (!child || child_ != child)
        return;
    dropChild(child_);
    child_ = nullptr;
    markNeedsLayout();
}

void RenderProxyBox::moveChild(RenderObject* child, RenderObject* after) {
    // 单子节点槽位：位置无意义。
    (void)child;
    (void)after;
}

void RenderProxyBox::performLayout() {
    // 上游的 RenderProxyBox mixin：代理盒不改变约束，尺寸跟随子节点。
    if (child_) {
        child_->layout(constraints_);
        if (child_->parentData())
            child_->parentData()->offset = Offset(0, 0);
        setSize(child_->size());
    } else {
        setSize(constraints_.smallest());
    }
}

void RenderProxyBox::visitChildren(const std::function<void(AbstractNode*)>& visitor) {
    if (child_)
        visitor(child_);
}

void RenderProxyBox::performPaint(PaintingContext& context, const Offset& offset) {
    // 代理盒自身无内容：把子节点画到它布局时写入 parentData 的偏移处。
    if (!child_)
        return;
    Offset childOffset = offset;
    if (child_->parentData())
        childOffset = childOffset + child_->parentData()->offset;
    context.paintChild(child_, childOffset);
}

double RenderProxyBox::computeMinIntrinsicWidth(double height) {
    return childBox() ? childBox()->getMinIntrinsicWidth(height) : 0.0;
}

double RenderProxyBox::computeMaxIntrinsicWidth(double height) {
    return childBox() ? childBox()->getMaxIntrinsicWidth(height) : 0.0;
}

double RenderProxyBox::computeMinIntrinsicHeight(double width) {
    return childBox() ? childBox()->getMinIntrinsicHeight(width) : 0.0;
}

double RenderProxyBox::computeMaxIntrinsicHeight(double width) {
    return childBox() ? childBox()->getMaxIntrinsicHeight(width) : 0.0;
}

// ── RenderBoxContainer ──────────────────────────────────────────────────────

void RenderBoxContainer::insertChild(RenderObject* child, RenderObject* after) {
    if (!child)
        return;
    // 已存在则先摘除，避免重复。
    removeChild(child);
    auto it = children_.end();
    if (after) {
        auto pos = std::find(children_.begin(), children_.end(), after);
        if (pos != children_.end())
            it = pos + 1;
    }
    children_.insert(it, child);
    adoptChild(child);
    markNeedsLayout();
}

void RenderBoxContainer::removeChild(RenderObject* child) {
    if (!child)
        return;
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it == children_.end())
        return;
    children_.erase(it);
    dropChild(child);
    markNeedsLayout();
}

void RenderBoxContainer::moveChild(RenderObject* child, RenderObject* after) {
    if (!child)
        return;
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it == children_.end())
        return;
    children_.erase(it);
    auto target = children_.end();
    if (after) {
        auto pos = std::find(children_.begin(), children_.end(), after);
        if (pos != children_.end())
            target = pos + 1;
    }
    children_.insert(target, child);
    markNeedsLayout();
}

void RenderBoxContainer::reorderChildren(const std::vector<RenderObject*>& desired) {
    // 只重排当前已存在的子节点；未在 desired 中出现的保持在末尾。
    if (desired.empty())
        return;
    std::vector<RenderObject*> reordered;
    reordered.reserve(desired.size());
    for (RenderObject* child : desired) {
        if (!child)
            continue;
        if (std::find(children_.begin(), children_.end(), child) == children_.end())
            continue;
        reordered.push_back(child);
    }
    for (RenderObject* child : children_) {
        if (std::find(reordered.begin(), reordered.end(), child) == reordered.end())
            reordered.push_back(child);
    }
    if (reordered == children_)
        return;
    children_ = std::move(reordered);
    markNeedsLayout();
}

void RenderBoxContainer::visitChildren(const std::function<void(AbstractNode*)>& visitor) {
    for (RenderObject* child : children_)
        if (child)
            visitor(child);
}

void RenderBoxContainer::performPaint(PaintingContext& context, const Offset& offset) {
    // 容器自身无内容：按绘制顺序（列表顺序 = 后画的在上）绘制每个子节点。
    for (RenderObject* child : children_) {
        if (!child)
            continue;
        Offset childOffset = offset;
        if (child->parentData())
            childOffset = childOffset + child->parentData()->offset;
        context.paintChild(child, childOffset);
    }
}

// ── RenderShiftedBox ────────────────────────────────────────────────────────

void RenderShiftedBox::setChild(RenderBox* child) {
    if (child_ == child)
        return;
    if (child_)
        dropChild(child_);
    child_ = child;
    if (child_)
        adoptChild(child_);
    markNeedsLayout();
}

void RenderShiftedBox::visitChildren(const std::function<void(AbstractNode*)>& visitor) {
    if (child_)
        visitor(child_);
}

double RenderShiftedBox::computeMinIntrinsicWidth(double height) {
    return child_ ? child_->getMinIntrinsicWidth(height) : 0.0;
}

double RenderShiftedBox::computeMaxIntrinsicWidth(double height) {
    return child_ ? child_->getMaxIntrinsicWidth(height) : 0.0;
}

double RenderShiftedBox::computeMinIntrinsicHeight(double width) {
    return child_ ? child_->getMinIntrinsicHeight(width) : 0.0;
}

double RenderShiftedBox::computeMaxIntrinsicHeight(double width) {
    return child_ ? child_->getMaxIntrinsicHeight(width) : 0.0;
}

void RenderShiftedBox::adoptChild(AbstractNode* child) {
    AbstractNode::adoptChild(child);
    // Allocate BoxParentData if the child doesn't have one.
    auto* ro = static_cast<RenderObject*>(child);
    if (!ro->parentData())
        ro->setParentData(new BoxParentData());
    markNeedsLayout();
}

void RenderShiftedBox::dropChild(AbstractNode* child) {
    AbstractNode::dropChild(child);
    markNeedsLayout();
}

void RenderShiftedBox::layoutChild(RenderBox* child, const BoxConstraints& constraints,
                                   const Offset& childOffset) {
    if (!child)
        return;
    child->layout(constraints);
    // Write the offset into the child's BoxParentData.
    auto* pd = dynamic_cast<BoxParentData*>(child->parentData());
    if (pd)
        pd->offset = childOffset;
}

}  // namespace cui_core
