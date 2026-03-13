/// @file rendering/sliver.cpp
/// @brief Sliver 渲染层实现（协议 + 常用 sliver + 视口）。
///
/// 逐段对应 上游：rendering/sliver.dart、sliver_padding.dart、
/// sliver_multi_box_adaptor.dart、sliver_fixed_extent_list.dart、
/// viewport.dart。四处有意偏离（layout 重载 / parentData->offset 承载绘制
/// 偏移 / eager 版多盒子 sliver）在 sliver.h 文件头列明。

#include "cui/rendering/sliver.h"

#include <cassert>
#include <cmath>
#include <limits>

#include "cui/rendering/painting_context.h"

namespace cui {

namespace {

/// 上游的 `_maxLayoutCyclesPerChild`（viewport.dart）。
constexpr int kMaxLayoutCyclesPerChild = 3;

/// 上游的 `precisionErrorTolerance`（dart:ui math_utils.dart）：浮点尾差容限。
constexpr double kPrecisionErrorTolerance = 1.0 / (1LL << 46);

double coordAlongMain(const Offset& o, Axis axis) {
    return axis == Axis::Vertical ? o.dy : o.dx;
}
double coordAlongCross(const Offset& o, Axis axis) {
    return axis == Axis::Vertical ? o.dx : o.dy;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// RenderSliver
// ═══════════════════════════════════════════════════════════════════════════

void RenderSliver::layout(const SliverConstraints& constraints, bool parentUsesSize) {
    (void)parentUsesSize;  // 与 上游 一样：视口恒以 parentUsesSize=true 调用
    sliverConstraints_ = constraints;
    // 级联访问到本节点即清脏（基类 RenderObject::layout 的同一条规矩：不清则
    // 后续 markNeedsLayout 会因「已脏」早退，静默丢帧）。
    clearNeedsLayout();
    if (sizedByParent())
        performResize();
    performLayout();
}

Rect RenderSliver::paintBounds() const {
    if (sliverConstraints_.axis() == Axis::Horizontal)
        return {0, 0, geometry_.paintExtent, sliverConstraints_.crossAxisExtent};
    return {0, 0, sliverConstraints_.crossAxisExtent, geometry_.paintExtent};
}

RenderObject* RenderSliver::childAfter(RenderObject* child) const {
    for (size_t i = 0; i + 1 < children_.size(); ++i)
        if (children_[i] == child)
            return children_[i + 1];
    return nullptr;
}

RenderObject* RenderSliver::childBefore(RenderObject* child) const {
    for (size_t i = 1; i < children_.size(); ++i)
        if (children_[i] == child)
            return children_[i - 1];
    return nullptr;
}

void RenderSliver::insertChild(RenderObject* child, RenderObject* after) {
    if (!child)
        return;
    removeChild(child);
    // 上游 `_containerInsertBefore`：after 为空 = 插到队首（懒加载 sliver 的
    // `insertAndLayoutLeadingChild` 靠这条往回补造），非空 = 插在 after 之后。
    auto it = children_.begin();
    if (after) {
        auto pos = std::find(children_.begin(), children_.end(), after);
        if (pos != children_.end())
            it = pos + 1;
    }
    children_.insert(it, child);
    adoptChild(child);
    markNeedsLayout();
}

void RenderSliver::removeChild(RenderObject* child) {
    if (!child)
        return;
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it == children_.end())
        return;
    children_.erase(it);
    dropChild(child);
    markNeedsLayout();
}

void RenderSliver::moveChild(RenderObject* child, RenderObject* after) {
    insertChild(child, after);
}

void RenderSliver::reorderChildren(const std::vector<RenderObject*>& desired) {
    std::vector<RenderObject*> kept;
    kept.reserve(desired.size());
    for (RenderObject* c : desired)
        if (std::find(children_.begin(), children_.end(), c) != children_.end())
            kept.push_back(c);
    children_ = std::move(kept);
    markNeedsLayout();
}

void RenderSliver::visitChildren(const std::function<void(AbstractNode*)>& visitor) {
    for (RenderObject* child : children_) {
        if (child)
            visitor(child);
    }
}

void RenderSliver::setupParentData(RenderObject* child) {
    if (child && !child->parentData())
        child->setParentData(new SliverPhysicalParentData());
}

double RenderSliver::mainAxisOf(const Offset& position) const {
    return coordAlongMain(position, sliverConstraints_.axis());
}

double RenderSliver::crossAxisOf(const Offset& position) const {
    return coordAlongCross(position, sliverConstraints_.axis());
}

bool RenderSliver::hitTest(const Offset& position, std::vector<RenderObject*>& result) {
    // 上游 RenderSliver.hitTest(mainAxisPosition, crossAxisPosition)：主轴量到
    // 「生长起点那条边」，反向生长时从结束边反算（本仓入参是绘制本地坐标，
    // 见 sliver.h 文件头偏离 2）。
    const AxisDirection eff = applyGrowthDirectionToAxisDirection(
        sliverConstraints_.axisDirection, sliverConstraints_.growthDirection);
    double main = mainAxisOf(position);
    if (eff == AxisDirection::Up || eff == AxisDirection::Left)
        main = hitTestExtent() - main;
    const double cross = crossAxisOf(position);
    if (main < 0.0 || main >= hitTestExtent() || cross < 0.0 ||
        cross >= sliverConstraints_.crossAxisExtent)
        return false;
    const bool hitSelf = hitTestSelf(position);
    if (hitSelf)
        result.push_back(this);
    const bool hitChildren = hitTestChildren(position, result);
    return hitSelf || hitChildren;
}

double RenderSliver::childScrollOffset(RenderObject* child) const {
    if (!child || !child->parentData())
        return 0.0;
    auto* pd = dynamic_cast<SliverLogicalParentData*>(child->parentData());
    return pd && pd->layoutOffset ? *pd->layoutOffset : 0.0;
}

double RenderSliver::calculatePaintOffset(const SliverConstraints& c, double from,
                                          double to) const {
    // 上游 viewport.dart 的同名工具：[from,to] 与可见绘制区间求交后的长度。
    const double a = c.scrollOffset;
    const double b = c.scrollOffset + c.remainingPaintExtent;
    return clampDouble(clampDouble(to, a, b) - clampDouble(from, a, b), 0.0,
                       c.remainingPaintExtent);
}

double RenderSliver::calculateCacheOffset(const SliverConstraints& c, double from,
                                          double to) const {
    const double a = c.scrollOffset + c.cacheOrigin;
    const double b = c.scrollOffset + c.remainingCacheExtent;
    return clampDouble(clampDouble(to, a, b) - clampDouble(from, a, b), 0.0,
                       c.remainingCacheExtent);
}

void RenderSliver::paint(PaintingContext& context, const Offset& offset) {
    if (!visible())
        return;
    for (RenderObject* child : children_) {
        if (!child || !paintsChild(child))
            continue;
        Offset childAt = offset;
        if (child->parentData())
            childAt = childAt + child->parentData()->offset;
        context.paintChild(child, childAt);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// RenderSliverSingleBoxAdapter / RenderSliverToBoxAdapter
// ═══════════════════════════════════════════════════════════════════════════

void RenderSliverSingleBoxAdapter::setChild(RenderBox* child) {
    if (child == child_)
        return;
    if (child_)
        removeChild(child_);
    if (child)
        insertChild(child, nullptr);
}

void RenderSliverSingleBoxAdapter::insertChild(RenderObject* child, RenderObject* after) {
    (void)after;  // 单槽位：顺序无意义（上游 RenderObjectWithChildMixin）
    if (!child || child == child_)
        return;
    if (child_)
        dropChild(child_);
    child_ = static_cast<RenderBox*>(child);
    setupParentData(child_);
    adoptChild(child_);
    markNeedsLayout();
}

void RenderSliverSingleBoxAdapter::removeChild(RenderObject* child) {
    if (!child || child != child_)
        return;
    dropChild(child_);
    child_ = nullptr;
    markNeedsLayout();
}

void RenderSliverSingleBoxAdapter::visitChildren(
    const std::function<void(AbstractNode*)>& visitor) {
    if (child_)
        visitor(child_);
}

double RenderSliverSingleBoxAdapter::childMainAxisPosition(RenderObject* child) const {
    (void)child;
    return -sliverConstraints_.scrollOffset;
}

void RenderSliverSingleBoxAdapter::setChildParentData(RenderObject* child,
                                                      const SliverConstraints& c,
                                                      const SliverGeometry& geometry) const {
    if (!child || !child->parentData())
        return;
    auto* pd = dynamic_cast<SliverPhysicalParentData*>(child->parentData());
    if (!pd)
        return;
    const double along = geometry.paintExtent + c.scrollOffset - geometry.scrollExtent;
    Offset paintOffset;
    switch (applyGrowthDirectionToAxisDirection(c.axisDirection, c.growthDirection)) {
        case AxisDirection::Up: paintOffset = {0, along}; break;
        case AxisDirection::Left: paintOffset = {along, 0}; break;
        case AxisDirection::Right: paintOffset = {-c.scrollOffset, 0}; break;
        case AxisDirection::Down: paintOffset = {0, -c.scrollOffset}; break;
    }
    pd->setPaintOffset(paintOffset);
}

void RenderSliverToBoxAdapter::performLayout() {
    if (!child_) {
        setGeometry(SliverGeometry::zero());
        return;
    }
    const SliverConstraints c = sliverConstraints();
    child_->layout(c.asBoxConstraints(), true);
    const double childExtent =
        c.axis() == Axis::Horizontal ? child_->size().width : child_->size().height;
    const double painted = calculatePaintOffset(c, 0.0, childExtent);
    const double cache = calculateCacheOffset(c, 0.0, childExtent);

    SliverGeometry g;
    g.scrollExtent = childExtent;
    g.paintExtent = painted;
    g.cacheExtent = cache;
    g.maxPaintExtent = childExtent;
    g.hitTestExtent = painted;
    g.hasVisualOverflow = childExtent > c.remainingPaintExtent || c.scrollOffset > 0.0;
    setGeometry(g);
    setChildParentData(child_, c, g);
}

// ═══════════════════════════════════════════════════════════════════════════
// RenderSliverPadding
// ═══════════════════════════════════════════════════════════════════════════

void RenderSliverPadding::setChild(RenderSliver* child) {
    if (!child || child == this->child())
        return;
    if (RenderSliver* old = this->child())
        removeChild(old);
    insertChild(child, nullptr);
}

double RenderSliverPadding::beforePadding() const {
    switch (applyGrowthDirectionToAxisDirection(sliverConstraints_.axisDirection,
                                                sliverConstraints_.growthDirection)) {
        case AxisDirection::Up: return padding_.bottom;
        case AxisDirection::Right: return padding_.left;
        case AxisDirection::Down: return padding_.top;
        case AxisDirection::Left: return padding_.right;
    }
    return 0.0;
}

double RenderSliverPadding::afterPadding() const {
    switch (applyGrowthDirectionToAxisDirection(sliverConstraints_.axisDirection,
                                                sliverConstraints_.growthDirection)) {
        case AxisDirection::Up: return padding_.top;
        case AxisDirection::Right: return padding_.right;
        case AxisDirection::Down: return padding_.bottom;
        case AxisDirection::Left: return padding_.left;
    }
    return 0.0;
}

double RenderSliverPadding::mainAxisPadding() const {
    return sliverConstraints_.axis() == Axis::Horizontal ? padding_.horizontal()
                                                         : padding_.vertical();
}

double RenderSliverPadding::crossAxisPadding() const {
    return sliverConstraints_.axis() == Axis::Horizontal ? padding_.vertical()
                                                         : padding_.horizontal();
}

double RenderSliverPadding::childMainAxisPosition(RenderObject* child) const {
    (void)child;
    return calculatePaintOffset(0.0, beforePadding());
}

double RenderSliverPadding::childCrossAxisPosition(RenderObject* child) const {
    (void)child;
    return sliverConstraints_.axis() == Axis::Horizontal ? padding_.top : padding_.left;
}

void RenderSliverPadding::performLayout() {
    const SliverConstraints c = sliverConstraints();
    const double before = beforePadding();
    const double after = afterPadding();
    const double mainPadding = mainAxisPadding();
    const double crossPadding = crossAxisPadding();
    const double beforePaintExtent = calculatePaintOffset(c, 0.0, before);

    if (!child()) {
        SliverGeometry g;
        g.scrollExtent = mainPadding;
        g.paintExtent = std::min(beforePaintExtent, c.remainingPaintExtent);
        g.maxPaintExtent = mainPadding;
        g.cacheExtent = calculateCacheOffset(c, 0.0, mainPadding);
        setGeometry(g);
        return;
    }

    // 上游：把 beforePadding 从滚动偏移里扣掉，再让子 sliver 布局。
    SliverConstraints cc = c;
    double overlap = c.overlap;
    if (overlap > 0.0)
        overlap = std::max(0.0, c.overlap - beforePaintExtent);
    cc.scrollOffset = std::max(0.0, c.scrollOffset - before);
    cc.cacheOrigin = std::min(0.0, c.cacheOrigin + before);
    cc.overlap = overlap;
    cc.remainingPaintExtent = c.remainingPaintExtent - beforePaintExtent;
    cc.remainingCacheExtent =
        c.remainingCacheExtent - calculateCacheOffset(c, 0.0, before);
    cc.crossAxisExtent = std::max(0.0, c.crossAxisExtent - crossPadding);
    cc.precedingScrollExtent = before + c.precedingScrollExtent;

    child()->layout(cc, true);
    const SliverGeometry cg = child()->geometry();
    if (cg.scrollOffsetCorrection != 0.0) {
        SliverGeometry g;
        g.scrollOffsetCorrection = cg.scrollOffsetCorrection;
        setGeometry(g);
        return;
    }

    const double scrollExtent = cg.scrollExtent;
    const double beforeCache = calculateCacheOffset(c, 0.0, before);
    const double afterCache =
        calculateCacheOffset(c, before + scrollExtent, mainPadding + scrollExtent);
    const double afterPaint =
        calculatePaintOffset(c, before + scrollExtent, mainPadding + scrollExtent);
    const double mainPaddingCache = beforeCache + afterCache;
    const double mainPaddingPaint = beforePaintExtent + afterPaint;
    const double paintExtent =
        std::min(beforePaintExtent + std::max(cg.paintExtent, cg.resolvedLayoutExtent() + afterPaint),
                 c.remainingPaintExtent);

    SliverGeometry g;
    g.paintOrigin = cg.paintOrigin;
    g.scrollExtent = mainPadding + scrollExtent;
    g.paintExtent = paintExtent;
    g.layoutExtent = std::min(mainPaddingPaint + cg.resolvedLayoutExtent(), paintExtent);
    g.cacheExtent = std::min(mainPaddingCache + cg.resolvedCacheExtent(), c.remainingCacheExtent);
    g.maxPaintExtent = mainPadding + cg.maxPaintExtent;
    g.hitTestExtent =
        std::max(mainPaddingPaint + cg.paintExtent, beforePaintExtent + cg.resolvedHitTestExtent());
    g.hasVisualOverflow = cg.hasVisualOverflow;
    setGeometry(g);

    // 上游的 paintOffset：Down 用 before 的绘制长度，Up/Left 从尾部量起。
    const double calculatedOffset = [&]() -> double {
        switch (applyGrowthDirectionToAxisDirection(c.axisDirection, c.growthDirection)) {
            case AxisDirection::Up:
                return calculatePaintOffset(c, padding_.bottom + scrollExtent,
                                            padding_.vertical() + scrollExtent);
            case AxisDirection::Left:
                return calculatePaintOffset(c, padding_.right + scrollExtent,
                                            padding_.horizontal() + scrollExtent);
            case AxisDirection::Right:
                return calculatePaintOffset(c, 0.0, padding_.left);
            case AxisDirection::Down:
                return calculatePaintOffset(c, 0.0, padding_.top);
        }
        return 0.0;
    }();
    Offset paintOffset = c.axis() == Axis::Horizontal
                             ? Offset{calculatedOffset, padding_.top}
                             : Offset{padding_.left, calculatedOffset};
    if (auto* pd = dynamic_cast<SliverPhysicalParentData*>(child()->parentData()))
        pd->setPaintOffset(paintOffset);
    (void)after;  // afterPadding 只在 debug 断言里参与一致性校验
}

// ═══════════════════════════════════════════════════════════════════════════
// RenderSliverMultiBoxAdaptor
// ═══════════════════════════════════════════════════════════════════════════

void RenderSliverMultiBoxAdaptor::setupParentData(RenderObject* child) {
    if (child && !child->parentData())
        child->setParentData(new SliverMultiBoxAdaptorParentData());
}

void RenderSliverMultiBoxAdaptor::insertChild(RenderObject* child, RenderObject* after) {
    // 上游：ContainerRenderObjectMixin.insert → adoptChild → childManager
    // .didAdoptChild(child)（move 同理走 moveChild → insertChild）。本仓
    // adoptChild 非虚，故在入列点补一次序号写入。
    RenderSliver::insertChild(child, after);
    if (child && childManager_)
        childManager_->didAdoptChild(static_cast<RenderBox*>(child));
}

SliverMultiBoxAdaptorParentData* RenderSliverMultiBoxAdaptor::parentDataOf(
    RenderObject* child) const {
    if (!child)
        return nullptr;
    return dynamic_cast<SliverMultiBoxAdaptorParentData*>(child->parentData());
}

int RenderSliverMultiBoxAdaptor::indexOf(RenderObject* child) const {
    if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(child))
        if (pd->index)
            return *pd->index;
    // 无托管人的 eager 走法（sliver.h 文件头偏离 3）：按槽位号兜底。
    for (size_t i = 0; i < children_.size(); ++i)
        if (children_[i] == child)
            return static_cast<int>(i);
    return -1;
}

std::optional<double> RenderSliverMultiBoxAdaptor::childLayoutOffset(
    RenderObject* child) const {
    SliverMultiBoxAdaptorParentData* pd = parentDataOf(child);
    return pd ? pd->layoutOffset : std::optional<double>();
}

double RenderSliverMultiBoxAdaptor::paintExtentOf(RenderObject* child) const {
    if (!child)
        return 0.0;
    const Size s = static_cast<RenderBox*>(child)->size();
    return sliverConstraints_.axis() == Axis::Horizontal ? s.width : s.height;
}

double RenderSliverMultiBoxAdaptor::childMainAxisPosition(RenderObject* child) const {
    return childScrollOffset(child) - sliverConstraints_.scrollOffset;
}

bool RenderSliverMultiBoxAdaptor::paintsChild(RenderObject* child) const {
    // 上游 RenderSliverMultiBoxAdaptor.paint 的可见区间判断。
    const double delta = childMainAxisPosition(child);
    return delta < sliverConstraints_.remainingPaintExtent &&
           delta + paintExtentOf(child) > 0.0;
}

void RenderSliverMultiBoxAdaptor::updateChildrenPaintOffsets() {
    // 上游 paint() 里的 mainAxisUnit / crossAxisUnit / originOffset / addExtent
    // 换算；结果写进 parentData（paintOffset 与 offset 同步，见 sliver.h 偏离 2），
    // 于是基类的绘制/命中偏移机制对 sliver 子树同样成立。
    const AxisDirection eff = applyGrowthDirectionToAxisDirection(
        sliverConstraints_.axisDirection, sliverConstraints_.growthDirection);
    Offset mainUnit, crossUnit, origin;
    bool addExtent = false;
    switch (eff) {
        case AxisDirection::Up:
            mainUnit = {0, -1};
            crossUnit = {1, 0};
            origin = {0, geometry_.paintExtent};
            addExtent = true;
            break;
        case AxisDirection::Left:
            mainUnit = {-1, 0};
            crossUnit = {0, 1};
            origin = {geometry_.paintExtent, 0};
            addExtent = true;
            break;
        case AxisDirection::Right:
            mainUnit = {1, 0};
            crossUnit = {0, 1};
            break;
        case AxisDirection::Down:
            mainUnit = {0, 1};
            crossUnit = {1, 0};
            break;
    }
    for (RenderObject* child : children_) {
        SliverPhysicalParentData* pd =
            child && child->parentData()
                ? dynamic_cast<SliverPhysicalParentData*>(child->parentData())
                : nullptr;
        if (!pd)
            continue;
        const double mainDelta = childMainAxisPosition(child);
        const double crossDelta = childCrossAxisPosition(child);
        Offset at{origin.dx + mainUnit.dx * mainDelta + crossUnit.dx * crossDelta,
                  origin.dy + mainUnit.dy * mainDelta + crossUnit.dy * crossDelta};
        if (addExtent) {
            const double extent = paintExtentOf(child);
            at = {at.dx + mainUnit.dx * extent, at.dy + mainUnit.dy * extent};
        }
        pd->setPaintOffset(at);
    }
}

bool RenderSliverMultiBoxAdaptor::addInitialChild(int index, double layoutOffset) {
    if (childManager_)
        childManager_->createChild(index, nullptr);
    if (!children_.empty()) {
        if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(children_.front()))
            pd->layoutOffset = layoutOffset;
        return true;
    }
    if (childManager_)
        childManager_->setDidUnderflow(true);
    return false;
}

RenderBox* RenderSliverMultiBoxAdaptor::insertAndLayoutLeadingChild(
    const BoxConstraints& childConstraints, bool parentUsesSize) {
    // 上游：index = indexOf(firstChild) - 1，造出来排在队首并立即布局。
    if (!firstChild() || !childManager_)
        return nullptr;
    const int index = indexOf(firstChild()) - 1;
    childManager_->createChild(index, nullptr);
    if (firstChild() && indexOf(firstChild()) == index) {
        firstChild()->layout(childConstraints, parentUsesSize);
        return static_cast<RenderBox*>(firstChild());
    }
    childManager_->setDidUnderflow(true);
    return nullptr;
}

RenderBox* RenderSliverMultiBoxAdaptor::insertAndLayoutChild(
    const BoxConstraints& childConstraints, RenderBox* after, bool parentUsesSize) {
    if (!after || !childManager_)
        return nullptr;
    const int index = indexOf(after) + 1;
    childManager_->createChild(index, after);
    RenderObject* child = childAfter(after);
    if (child && indexOf(child) == index) {
        child->layout(childConstraints, parentUsesSize);
        return static_cast<RenderBox*>(child);
    }
    childManager_->setDidUnderflow(true);
    return nullptr;
}

int RenderSliverMultiBoxAdaptor::calculateLeadingGarbage(int firstIndex) const {
    RenderObject* walker = firstChild();
    int leadingGarbage = 0;
    while (walker && indexOf(walker) < firstIndex) {
        ++leadingGarbage;
        walker = childAfter(walker);
    }
    return leadingGarbage;
}

int RenderSliverMultiBoxAdaptor::calculateTrailingGarbage(int lastIndex) const {
    RenderObject* walker = lastChild();
    int trailingGarbage = 0;
    while (walker && indexOf(walker) > lastIndex) {
        ++trailingGarbage;
        walker = childBefore(walker);
    }
    return trailingGarbage;
}

void RenderSliverMultiBoxAdaptor::collectGarbage(int leadingGarbage, int trailingGarbage) {
    // 上游 _destroyOrCacheChild：keepAlive 子节点进桶、其余交托管人摘除。
    // 本仓桶未移植（sliver.h 文件头偏离 3），一律走 removeChild；无托管人时
    // 子节点由调用方持有，不回收。
    if (!childManager_)
        return;
    while (leadingGarbage > 0 && firstChild()) {
        childManager_->removeChild(static_cast<RenderBox*>(firstChild()));
        --leadingGarbage;
    }
    while (trailingGarbage > 0 && lastChild()) {
        childManager_->removeChild(static_cast<RenderBox*>(lastChild()));
        --trailingGarbage;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// RenderSliverFixedExtentBoxAdaptor
// ═══════════════════════════════════════════════════════════════════════════

BoxConstraints RenderSliverFixedExtentBoxAdaptor::childConstraints(int /*index*/) const {
    return sliverConstraints_.asBoxConstraints(itemExtent_, itemExtent_);
}

int RenderSliverFixedExtentBoxAdaptor::getMinChildIndexForScrollOffset(
    double scrollOffset) const {
    if (itemExtent_ > 0.0) {
        const double actual = scrollOffset / itemExtent_;
        const int round = static_cast<int>(std::lround(actual));
        if (std::fabs(actual * itemExtent_ - round * itemExtent_) < 1e-10)
            return round;
        return static_cast<int>(std::floor(actual));
    }
    return 0;
}

int RenderSliverFixedExtentBoxAdaptor::getMaxChildIndexForScrollOffset(
    double scrollOffset) const {
    if (itemExtent_ > 0.0) {
        const double actual = scrollOffset / itemExtent_ - 1.0;
        const int round = static_cast<int>(std::lround(actual));
        if (std::fabs(actual * itemExtent_ - round * itemExtent_) < 1e-10)
            return std::max(0, round);
        return std::max(0, static_cast<int>(std::ceil(actual)));
    }
    return 0;
}

double RenderSliverFixedExtentBoxAdaptor::estimateMaxScrollOffset(
    const SliverConstraints& c, int firstIndex, int lastIndex, double leadingScrollOffset,
    double trailingScrollOffset) const {
    // 上游：defers to childManager.estimateMaxScrollOffset。无托管人（eager）
    // 时总数即在树数量。
    if (childManager_)
        return childManager_->estimateMaxScrollOffset(c, firstIndex, lastIndex,
                                                     leadingScrollOffset,
                                                     trailingScrollOffset);
    return indexToLayoutOffset(childCount());
}

double RenderSliverFixedExtentBoxAdaptor::computeMaxScrollOffset() const {
    // 上游：childManager.childCount * itemExtent（精确值）。
    return totalChildren() * itemExtent_;
}

void RenderSliverFixedExtentBoxAdaptor::performLayout() {
    const SliverConstraints c = sliverConstraints();
    if (childManager_)
        childManager_->didStartLayout();
    if (childManager_)
        childManager_->setDidUnderflow(false);

    const double scrollOffset = c.scrollOffset + c.cacheOrigin;
    const double remainingExtent = c.remainingCacheExtent;
    const double targetEndScrollOffset = scrollOffset + remainingExtent;

    const int firstIndex = getMinChildIndexForScrollOffset(scrollOffset);
    const bool hasTargetLastIndex = std::isfinite(targetEndScrollOffset);
    const int targetLastIndex =
        hasTargetLastIndex ? getMaxChildIndexForScrollOffset(targetEndScrollOffset) : 0;

    // 上游：先把窗口外的存量子节点回收掉，再补造缺的。
    if (firstChild()) {
        const int leadingGarbage = calculateLeadingGarbage(firstIndex);
        const int trailingGarbage =
            hasTargetLastIndex ? calculateTrailingGarbage(targetLastIndex) : 0;
        collectGarbage(leadingGarbage, trailingGarbage);
    } else {
        collectGarbage(0, 0);
    }

    if (!firstChild()) {
        if (!addInitialChild(firstIndex, indexToLayoutOffset(firstIndex))) {
            // 没有子节点，或者已经滚过末尾。
            const double max = (firstIndex <= 0) ? 0.0 : computeMaxScrollOffset();
            SliverGeometry g;
            g.scrollExtent = max;
            g.maxPaintExtent = max;
            setGeometry(g);
            if (childManager_)
                childManager_->didFinishLayout();
            return;
        }
    }

    RenderObject* trailingChildWithLayout = nullptr;

    // 首子之前还缺的项：逐个往上补造（造不出来说明前面的项没了，让上层重排）。
    for (int index = indexOf(firstChild()) - 1; index >= firstIndex; --index) {
        RenderBox* child = insertAndLayoutLeadingChild(childConstraints(index));
        if (!child) {
            SliverGeometry g;
            g.scrollOffsetCorrection = indexToLayoutOffset(index);
            setGeometry(g);
            return;
        }
        if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(child))
            pd->layoutOffset = indexToLayoutOffset(index);
        if (!trailingChildWithLayout)
            trailingChildWithLayout = child;
    }

    if (!trailingChildWithLayout) {
        firstChild()->layout(childConstraints(indexOf(firstChild())), false);
        if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(firstChild()))
            pd->layoutOffset = indexToLayoutOffset(firstIndex);
        trailingChildWithLayout = firstChild();
    }

    double estimatedMaxScrollOffset = std::numeric_limits<double>::infinity();
    for (int index = indexOf(trailingChildWithLayout) + 1;
         !hasTargetLastIndex || index <= targetLastIndex; ++index) {
        RenderObject* child = childAfter(trailingChildWithLayout);
        if (!child || indexOf(child) != index) {
            child = insertAndLayoutChild(childConstraints(index),
                                         static_cast<RenderBox*>(trailingChildWithLayout));
            if (!child) {
                // 子节点用完了。
                estimatedMaxScrollOffset = indexToLayoutOffset(index);
                break;
            }
        } else {
            child->layout(childConstraints(index), false);
        }
        trailingChildWithLayout = child;
        if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(child))
            pd->layoutOffset = indexToLayoutOffset(index);
    }

    const int lastIndex = indexOf(lastChild());
    const double leadingScrollOffset = indexToLayoutOffset(firstIndex);
    const double trailingScrollOffset = indexToLayoutOffset(lastIndex + 1);

    estimatedMaxScrollOffset =
        std::min(estimatedMaxScrollOffset,
                 estimateMaxScrollOffset(c, firstIndex, lastIndex, leadingScrollOffset,
                                         trailingScrollOffset));

    const double paintExtent =
        calculatePaintOffset(c, leadingScrollOffset, trailingScrollOffset);
    const double cacheExtentValue =
        calculateCacheOffset(c, leadingScrollOffset, trailingScrollOffset);

    const double targetEndScrollOffsetForPaint = c.scrollOffset + c.remainingPaintExtent;
    const bool hasTargetLastIndexForPaint = std::isfinite(targetEndScrollOffsetForPaint);
    const int targetLastIndexForPaint =
        hasTargetLastIndexForPaint ? getMaxChildIndexForScrollOffset(targetEndScrollOffsetForPaint)
                                   : 0;

    SliverGeometry g;
    g.scrollExtent = estimatedMaxScrollOffset;
    g.paintExtent = paintExtent;
    g.cacheExtent = cacheExtentValue;
    g.maxPaintExtent = estimatedMaxScrollOffset;
    // 上游注释：宁可保守也不要滚动时把裁剪抖没。
    g.hasVisualOverflow =
        (hasTargetLastIndexForPaint && lastIndex >= targetLastIndexForPaint) ||
        c.scrollOffset > 0.0;
    setGeometry(g);
    updateChildrenPaintOffsets();

    // 可能一开始就停在末尾，这轮没暴露出新子节点。
    if (estimatedMaxScrollOffset == trailingScrollOffset && childManager_)
        childManager_->setDidUnderflow(true);
    if (childManager_)
        childManager_->didFinishLayout();
}

// ═══════════════════════════════════════════════════════════════════════════
// RenderSliverList
// ═══════════════════════════════════════════════════════════════════════════

void RenderSliverList::performLayout() {
    const SliverConstraints c = sliverConstraints();
    if (childManager_)
        childManager_->didStartLayout();
    if (childManager_)
        childManager_->setDidUnderflow(false);

    const double scrollOffset = c.scrollOffset + c.cacheOrigin;
    const double targetEndScrollOffset = scrollOffset + c.remainingCacheExtent;
    const BoxConstraints childConstraints = c.asBoxConstraints();
    int leadingGarbage = 0;
    int trailingGarbage = 0;
    bool reachedEnd = false;

    // 上游算法注释：先找到与 scrollOffset 相交的首个子节点（必要时向上补造），
    // 再顺走逐个布局、必要时向下补造，直到覆盖整个视口；期间任何补造都可能
    // 让尚未布局的子节点被摘掉，故始终从「当前 firstChild」出发向两端扩。
    if (!firstChild()) {
        if (!addInitialChild()) {
            setGeometry(SliverGeometry::zero());
            if (childManager_)
                childManager_->didFinishLayout();
            return;
        }
    }

    // 记录本轮已布局的子节点区间：区间内 index 连续，区间外随时可能被摘除。
    RenderObject* leadingChildWithLayout = nullptr;
    RenderObject* trailingChildWithLayout = nullptr;
    RenderObject* earliestUsefulChild = firstChild();

    // firstChild 的布局偏移为空多半是子节点被重排过；依赖布局偏移正确，
    // 得往前找到第一个有偏移的子节点，把没有偏移的头部回收掉。
    if (!childLayoutOffset(firstChild())) {
        int leadingChildrenWithoutLayoutOffset = 0;
        while (earliestUsefulChild && !childLayoutOffset(earliestUsefulChild)) {
            earliestUsefulChild = childAfter(earliestUsefulChild);
            ++leadingChildrenWithoutLayoutOffset;
        }
        collectGarbage(leadingChildrenWithoutLayoutOffset, 0);
        if (!firstChild()) {
            if (!addInitialChild()) {
                setGeometry(SliverGeometry::zero());
                if (childManager_)
                    childManager_->didFinishLayout();
                return;
            }
        }
    }

    // 找到最后一个布局偏移 <= scrollOffset 的子节点（向上补造）。
    earliestUsefulChild = firstChild();
    for (double earliestScrollOffset = childLayoutOffset(earliestUsefulChild).value_or(0.0);
         earliestScrollOffset > scrollOffset;
         earliestScrollOffset = childLayoutOffset(earliestUsefulChild).value_or(0.0)) {
        earliestUsefulChild = insertAndLayoutLeadingChild(childConstraints, true);
        if (!earliestUsefulChild) {
            if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(firstChild()))
                pd->layoutOffset = 0.0;
            if (scrollOffset == 0.0) {
                // 只补造了 firstChild 之前的节点，这一轮什么都没布局，手动布局首子。
                firstChild()->layout(childConstraints, true);
                earliestUsefulChild = firstChild();
                leadingChildWithLayout = earliestUsefulChild;
                if (!trailingChildWithLayout)
                    trailingChildWithLayout = earliestUsefulChild;
                break;
            }
            // 还没走到滚动位置就没有子节点了：告知上层本 sliver 无法履约，
            // 需要滚动偏移纠偏（整轮布局重来）。
            SliverGeometry g;
            g.scrollOffsetCorrection = -scrollOffset;
            setGeometry(g);
            return;
        }

        const double firstChildScrollOffset = earliestScrollOffset - paintExtentOf(firstChild());
        if (firstChildScrollOffset < -kPrecisionErrorTolerance) {
            // 假定首子之前没有子节点，下一个布局周期若发现并非如此会再纠。
            if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(firstChild()))
                pd->layoutOffset = 0.0;
            SliverGeometry g;
            g.scrollOffsetCorrection = -firstChildScrollOffset;
            setGeometry(g);
            return;
        }

        if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(earliestUsefulChild))
            pd->layoutOffset = firstChildScrollOffset;
        leadingChildWithLayout = earliestUsefulChild;
        if (!trailingChildWithLayout)
            trailingChildWithLayout = earliestUsefulChild;
    }

    // 滚动位置在 0 时应当确认真的在列表开头（往前补造直到首子 index==0）。
    if (scrollOffset < kPrecisionErrorTolerance) {
        while (indexOf(firstChild()) > 0) {
            const double earliestScrollOffset = childLayoutOffset(firstChild()).value_or(0.0);
            earliestUsefulChild = insertAndLayoutLeadingChild(childConstraints, true);
            if (!earliestUsefulChild)
                break;
            const double firstChildScrollOffset = earliestScrollOffset - paintExtentOf(firstChild());
            if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(firstChild()))
                pd->layoutOffset = 0.0;
            if (firstChildScrollOffset < -kPrecisionErrorTolerance) {
                SliverGeometry g;
                g.scrollOffsetCorrection = -firstChildScrollOffset;
                setGeometry(g);
                return;
            }
        }
    }

    // 至此 earliestUsefulChild == firstChild，其布局偏移 <= scrollOffset，
    // 且已布局区间覆盖了它。
    if (!leadingChildWithLayout) {
        earliestUsefulChild->layout(childConstraints, true);
        leadingChildWithLayout = earliestUsefulChild;
        trailingChildWithLayout = earliestUsefulChild;
    }

    bool inLayoutRange = true;
    RenderObject* child = earliestUsefulChild;
    int index = indexOf(child);
    double endScrollOffset = childLayoutOffset(child).value_or(0.0) + paintExtentOf(child);

    // 上游的内联 advance()：前进一个子节点，必要时补造并布局。返回 false 表示
    // 子节点已用完。
    auto advance = [&]() -> bool {
        if (child == trailingChildWithLayout)
            inLayoutRange = false;
        child = childAfter(child);
        if (!child)
            inLayoutRange = false;
        ++index;
        if (!inLayoutRange) {
            if (!child || indexOf(child) != index) {
                child = insertAndLayoutChild(childConstraints,
                                             static_cast<RenderBox*>(trailingChildWithLayout), true);
                if (!child)
                    return false;
            } else {
                child->layout(childConstraints, true);
            }
            trailingChildWithLayout = child;
        }
        if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(child))
            pd->layoutOffset = endScrollOffset;
        endScrollOffset = childLayoutOffset(child).value_or(0.0) + paintExtentOf(child);
        return true;
    };

    // 跳过完全在滚动偏移之前的子节点。
    while (endScrollOffset < scrollOffset) {
        ++leadingGarbage;
        if (!advance()) {
            // 子节点用尽：保留最后一个，好知道内容真正的末尾在哪。
            collectGarbage(leadingGarbage - 1, 0);
            const double extent =
                childLayoutOffset(lastChild()).value_or(0.0) + paintExtentOf(lastChild());
            SliverGeometry g;
            g.scrollExtent = extent;
            g.maxPaintExtent = extent;
            setGeometry(g);
            if (childManager_)
                childManager_->didFinishLayout();
            return;
        }
    }

    // 布局到覆盖整个缓存窗口。
    while (endScrollOffset < targetEndScrollOffset) {
        if (!advance()) {
            reachedEnd = true;
            break;
        }
    }

    // 剩下的都是垃圾。
    if (child) {
        child = childAfter(child);
        while (child) {
            ++trailingGarbage;
            child = childAfter(child);
        }
    }

    collectGarbage(leadingGarbage, trailingGarbage);

    const double firstChildOffset = childLayoutOffset(firstChild()).value_or(0.0);
    const double estimatedMaxScrollOffset =
        reachedEnd || !childManager_
            ? endScrollOffset
            : childManager_->estimateMaxScrollOffset(c, indexOf(firstChild()),
                                                    indexOf(lastChild()), firstChildOffset,
                                                    endScrollOffset);

    SliverGeometry g;
    g.scrollExtent = estimatedMaxScrollOffset;
    g.paintExtent = calculatePaintOffset(c, firstChildOffset, endScrollOffset);
    g.cacheExtent = calculateCacheOffset(c, firstChildOffset, endScrollOffset);
    g.maxPaintExtent = estimatedMaxScrollOffset;
    // 上游注释：宁可保守也不要滚动时把裁剪抖没。
    g.hasVisualOverflow =
        endScrollOffset > c.scrollOffset + c.remainingPaintExtent || c.scrollOffset > 0.0;
    setGeometry(g);
    updateChildrenPaintOffsets();

    // 可能一开始就停在末尾，这轮没暴露出新子节点。
    if (estimatedMaxScrollOffset == endScrollOffset && childManager_)
        childManager_->setDidUnderflow(true);
    if (childManager_)
        childManager_->didFinishLayout();
}

// ═══════════════════════════════════════════════════════════════════════════
// RenderViewport
// ═══════════════════════════════════════════════════════════════════════════

void RenderViewport::performResize() {
    setSize(constraints_.biggest());
}

RenderObject* RenderViewport::childAfter(RenderObject* child) const {
    for (size_t i = 0; i + 1 < children_.size(); ++i)
        if (children_[i] == child)
            return children_[i + 1];
    return nullptr;
}

RenderObject* RenderViewport::childBefore(RenderObject* child) const {
    for (size_t i = 1; i < children_.size(); ++i)
        if (children_[i] == child)
            return children_[i - 1];
    return nullptr;
}

void RenderViewport::insertChild(RenderObject* child, RenderObject* after) {
    if (!child)
        return;
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

void RenderViewport::removeChild(RenderObject* child) {
    if (!child)
        return;
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it == children_.end())
        return;
    children_.erase(it);
    dropChild(child);
    markNeedsLayout();
}

void RenderViewport::moveChild(RenderObject* child, RenderObject* after) {
    insertChild(child, after);
}

void RenderViewport::reorderChildren(const std::vector<RenderObject*>& desired) {
    std::vector<RenderObject*> kept;
    kept.reserve(desired.size());
    for (RenderObject* c : desired)
        if (std::find(children_.begin(), children_.end(), c) != children_.end())
            kept.push_back(c);
    children_ = std::move(kept);
    markNeedsLayout();
}

void RenderViewport::visitChildren(
    const std::function<void(AbstractNode*)>& visitor) {
    for (RenderObject* child : children_) {
        if (child)
            visitor(child);
    }
}

void RenderViewport::setupParentData(RenderObject* child) {
    if (child && !child->parentData())
        child->setParentData(new SliverPhysicalParentData());
}

double RenderViewport::extentAlongAxis(const RenderObject* child) const {
    const RenderSliver* sl = static_cast<const RenderSliver*>(child);
    return sl ? sl->geometry().paintExtent : 0.0;
}

void RenderViewport::updateChildLayoutOffset(RenderObject* child, double layoutOffset,
                                                   GrowthDirection g) const {
    if (!child || !child->parentData())
        return;
    auto* pd = dynamic_cast<SliverPhysicalParentData*>(child->parentData());
    if (!pd)
        return;
    // 上游的 computeAbsolutePaintOffset。
    // 反向分支见 sliver.h 文件头偏离 4：这里用 paintExtent（裁剪后的可见长度），
    // 上游用子节点自身长度 + start/end 双槽，故 Up/Left 第二段起会与首段重叠。
    const double paintExtent = extentAlongAxis(child);
    Offset offset;
    switch (applyGrowthDirectionToAxisDirection(axisDirection_, g)) {
        case AxisDirection::Up:
            offset = {0, size().height - layoutOffset - paintExtent};
            break;
        case AxisDirection::Left:
            offset = {size().width - layoutOffset - paintExtent, 0};
            break;
        case AxisDirection::Right:
            offset = {layoutOffset, 0};
            break;
        case AxisDirection::Down:
            offset = {0, layoutOffset};
            break;
    }
    pd->setPaintOffset(offset);
}

void RenderViewport::updateOutOfBandData(GrowthDirection g,
                                               const SliverGeometry& geometry) {
    // 上游：累加的是「内容长度」，减视口尺寸发生在 applyContentDimensions 时。
    if (g == GrowthDirection::Forward)
        contentMaxScrollExtent_ += geometry.scrollExtent;
    else
        contentMinScrollExtent_ -= geometry.scrollExtent;
    if (geometry.hasVisualOverflow)
        hasVisualOverflow_ = true;
}

double RenderViewport::layoutChildSequence(
    RenderObject* child, double scrollOffset, double overlap, double layoutOffset,
    double remainingPaintExtent, double mainAxisExtent, double crossAxisExtent,
    GrowthDirection growthDirection, const Advance& advance, double remainingCacheExtent,
    double cacheOrigin) {
    // 上游 RenderViewportBase.layoutChildSequence 的逐行移植。
    const double initialLayoutOffset = layoutOffset;
    const ScrollDirection adjustedUserScrollDirection =
        applyGrowthDirectionToScrollDirection(userScrollDirection_, growthDirection);
    double maxPaintOffset = layoutOffset + overlap;
    double precedingScrollExtent = 0.0;

    while (child) {
        auto* sliver = static_cast<RenderSliver*>(child);
        const double sliverScrollOffset = scrollOffset <= 0.0 ? 0.0 : scrollOffset;
        // 滚动偏移不足以覆盖缓存窗口时，缓存起点要跟着收缩（上游注释同义）。
        const double correctedCacheOrigin = std::max(cacheOrigin, -sliverScrollOffset);
        const double cacheExtentCorrection = cacheOrigin - correctedCacheOrigin;

        SliverConstraints c;
        c.axisDirection = axisDirection_;
        c.growthDirection = growthDirection;
        c.userScrollDirection = adjustedUserScrollDirection;
        c.scrollOffset = sliverScrollOffset;
        c.precedingScrollExtent = precedingScrollExtent;
        c.overlap = maxPaintOffset - layoutOffset;
        c.remainingPaintExtent =
            std::max(0.0, remainingPaintExtent - layoutOffset + initialLayoutOffset);
        c.crossAxisExtent = crossAxisExtent;
        c.crossAxisDirection = crossAxisDirection_;
        c.viewportMainAxisExtent = mainAxisExtent;
        c.remainingCacheExtent = std::max(0.0, remainingCacheExtent + cacheExtentCorrection);
        c.cacheOrigin = correctedCacheOrigin;
        sliver->layout(c, true);

        const SliverGeometry childGeometry = sliver->geometry();
        if (childGeometry.scrollOffsetCorrection != 0.0)
            return childGeometry.scrollOffsetCorrection;

        const double effectiveLayoutOffset = layoutOffset + childGeometry.paintOrigin;
        // layoutExtent 为 0（已滚过末端）时改用 -scrollOffset 维持兄弟次序，
        // 上游注释：此时 layoutOffset 已失去意义。
        if (childGeometry.resolvedVisible() || scrollOffset > 0.0)
            updateChildLayoutOffset(child, effectiveLayoutOffset, growthDirection);
        else
            updateChildLayoutOffset(child, -scrollOffset + initialLayoutOffset,
                                    growthDirection);

        maxPaintOffset =
            std::max(effectiveLayoutOffset + childGeometry.paintExtent, maxPaintOffset);
        scrollOffset -= childGeometry.scrollExtent;
        precedingScrollExtent += childGeometry.scrollExtent;
        layoutOffset += childGeometry.resolvedLayoutExtent();
        if (childGeometry.resolvedCacheExtent() != 0.0) {
            remainingCacheExtent -= childGeometry.resolvedCacheExtent() - cacheExtentCorrection;
            cacheOrigin = std::min(correctedCacheOrigin + childGeometry.resolvedCacheExtent(),
                                   0.0);
        }

        updateOutOfBandData(growthDirection, childGeometry);
        child = advance(child);
    }
    return 0.0;
}

double RenderViewport::attemptLayout(double mainAxisExtent, double crossAxisExtent,
                                           double correctedOffset) {
    // 上游 RenderViewport._attemptLayout 的非居中分支（anchor = 0、无 center）。
    const double anchor = 0.0;
    contentMinScrollExtent_ = 0.0;
    contentMaxScrollExtent_ = 0.0;
    hasVisualOverflow_ = false;

    const double centerOffset = mainAxisExtent * anchor - correctedOffset;
    const double reverseRemainingPaintExtent =
        clampDouble(centerOffset, 0.0, mainAxisExtent);
    const double forwardRemainingPaintExtent =
        clampDouble(mainAxisExtent - centerOffset, 0.0, mainAxisExtent);

    const double fullCacheExtent = mainAxisExtent + 2.0 * cacheExtent_;
    const double centerCacheOffset = centerOffset + cacheExtent_;
    const double forwardRemainingCacheExtent =
        clampDouble(fullCacheExtent - centerCacheOffset, 0.0, fullCacheExtent);

    Advance next = [this](RenderObject* c) { return childAfter(c); };
    // 非居中 ⇒ 无负偏移前缀序列（leadingNegativeChild 恒为 null）。
    return layoutChildSequence(
        firstChild(), std::max(0.0, -centerOffset),
        std::min(0.0, -centerOffset),
        centerOffset >= mainAxisExtent ? centerOffset : reverseRemainingPaintExtent,
        forwardRemainingPaintExtent, mainAxisExtent, crossAxisExtent,
        GrowthDirection::Forward, next, forwardRemainingCacheExtent,
        clampDouble(centerOffset, -cacheExtent_, 0.0));
}

void RenderViewport::performLayout() {
    const double mainAxisExtent = axis() == Axis::Vertical ? size().height : size().width;
    const double crossAxisExtent = axis() == Axis::Vertical ? size().width : size().height;
    // 本仓视口没有 center/anchor（所有子 sliver 都按 Forward 生长），故 anchor 恒为 0。
    const double anchor = 0.0;

    if (children_.empty()) {
        minScrollExtent_ = maxScrollExtent_ = 0.0;
        contentMinScrollExtent_ = contentMaxScrollExtent_ = 0.0;
        hasVisualOverflow_ = false;
        if (onExtentsChanged_)
            onExtentsChanged_(0.0, 0.0, mainAxisExtent);
        return;
    }

    const int maxLayoutCycles = kMaxLayoutCyclesPerChild * std::max(1, childCount());
    int count = 0;
    for (;;) {
        const double correction = attemptLayout(mainAxisExtent, crossAxisExtent, offset_);
        bool restart = false;
        if (correction != 0.0) {
            // 上游：offset.correctBy(correction) 后整帧重来。
            offset_ += correction;
            restart = true;
        } else {
            // 上游的 applyContentDimensions(min(0, _minScrollExtent + main*anchor),
            //                              max(0, _maxScrollExtent - main*(1-anchor)))。
            const double minExtent =
                std::min(0.0, contentMinScrollExtent_ + mainAxisExtent * anchor);
            const double maxExtent = std::max(
                0.0, contentMaxScrollExtent_ - mainAxisExtent * (1.0 - anchor));
            minScrollExtent_ = minExtent;
            maxScrollExtent_ = maxExtent;
            if (onExtentsChanged_)
                onExtentsChanged_(minExtent, maxExtent, mainAxisExtent);
            const double clamped = clampDouble(offset_, minExtent, maxExtent);
            if (clamped != offset_) {
                offset_ = clamped;
                restart = true;
            }
        }
        if (!restart || ++count >= maxLayoutCycles)
            break;
    }
}

void RenderViewport::clampOffsetIntoContent() {
    const double clamped = clampDouble(offset_, minScrollExtent_, maxScrollExtent_);
    if (clamped != offset_)
        setOffset(clamped);
}

bool RenderViewport::hitTestChildren(const Offset& position,
                                           std::vector<RenderObject*>& result) {
    // 倒序（后画的在上）；跳过不可见 sliver —— 上游 hitTestSlivers 的同款前置。
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        RenderObject* child = *it;
        auto* sliver = child ? static_cast<RenderSliver*>(child) : nullptr;
        if (!sliver || !sliver->visible())
            continue;
        Offset local = position;
        if (child->parentData())
            local = local - child->parentData()->offset;
        if (child->hitTest(local, result))
            return true;
    }
    return false;
}

void RenderViewport::performPaint(PaintingContext& context, const Offset& offset) {
    // 每个 sliver 子节点各自包一层裁剪（对齐本仓 RenderViewport 的做法：canvas
    // 级 clip 管不住子树里嵌套的 PictureLayer，滚出视口的内容会画到外面）。
    const Offset origin = context.currentOffset() + offset;
    const Rect clip{origin.dx, origin.dy, size().width, size().height};
    for (RenderObject* child : children_) {
        auto* sliver = child ? static_cast<RenderSliver*>(child) : nullptr;
        if (!sliver || !sliver->visible())
            continue;
        Offset childAt = origin;
        if (child->parentData())
            childAt = childAt + child->parentData()->offset;
        context.pushClip(clip, child, childAt);
    }
}

}  // namespace cui_core
