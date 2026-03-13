/// @file rendering/sliver_grid.cpp
/// @brief 网格 sliver 的布局度量与 performLayout。
///
/// 对应 上游源码：rendering/sliver_grid.dart（SliverGridRegularTileLayout 的
/// 六个查询 + RenderSliverGrid.performLayout）。三条结构性偏离写在头文件，
/// 逐段注释里不再重复。

#include "cui/rendering/sliver_grid.h"
#include "cui/foundation/shared_ptr.h"

#include <algorithm>
#include <cmath>
#include <typeinfo>

namespace cui {

namespace {

/// 上游的 main/cross 轴换算：把 (主轴, 交叉轴) 折成 BoxConstraints 的 (宽, 高)。
BoxConstraints tightTileConstraints(const SliverConstraints& c, double mainExtent,
                                    double crossExtent) {
    const double w = std::max(0.0, c.axis() == Axis::Horizontal ? mainExtent : crossExtent);
    const double h = std::max(0.0, c.axis() == Axis::Horizontal ? crossExtent : mainExtent);
    return BoxConstraints::tight(w, h);
}

}  // namespace

// ── SliverGridLayout ───────────────────────────────────────────────────────

double SliverGridLayout::getStartScrollOffsetForIndex(int index) const {
    const int rows = std::max(0, index) / std::max(1, crossAxisCount);
    return static_cast<double>(rows) * mainAxisStride;
}

double SliverGridLayout::getEndScrollOffsetForIndex(int index) const {
    return getStartScrollOffsetForIndex(index) + childMainAxisExtent;
}

double SliverGridLayout::getCrossAxisOffsetForIndex(int index) const {
    const int count = std::max(1, crossAxisCount);
    int col = std::max(0, index) % count;
    if (reverseCrossAxis)
        col = count - 1 - col;
    return static_cast<double>(col) * crossAxisStride;
}

SliverGridGeometry SliverGridLayout::getGeometryForIndex(int index) const {
    SliverGridGeometry g;
    g.scrollOffset = getStartScrollOffsetForIndex(index);
    g.crossAxisOffset = getCrossAxisOffsetForIndex(index);
    g.mainAxisExtent = childMainAxisExtent;
    g.crossAxisExtent = childCrossAxisExtent;
    return g;
}

int SliverGridLayout::getMinIndexForScrollOffset(double scrollOffset) const {
    if (mainAxisStride <= 0.0 || !std::isfinite(scrollOffset))
        return 0;
    const double rows = std::floor(scrollOffset / mainAxisStride);
    return static_cast<int>(std::max(0.0, rows)) * std::max(1, crossAxisCount);
}

int SliverGridLayout::getMaxIndexForScrollOffset(double scrollOffset, int childCount) const {
    // 上游 `getMaxIndexForScrollOffset`：ceil(o/stride) - 1 再补上本行最后一条。
    // 与 minIndex 的 floor 只在「o 正好落在行首」时分岔：此时上一行恰好在 o 处
    // 结束，左闭右开的窗口不该把下一行算进来，所以不能复用 minIndex 的行号。
    const int last = std::max(0, childCount - 1);
    if (mainAxisStride <= 0.0 || !std::isfinite(scrollOffset))
        return last;
    if (scrollOffset < 0.0)
        return 0;
    const int count = std::max(1, crossAxisCount);
    const int turns = static_cast<int>(std::ceil(scrollOffset / mainAxisStride)) - 1;
    return std::max(0, std::min(turns * count + (count - 1), last));
}

double SliverGridLayout::getMaxScrollOffset(int childCount) const {
    if (childCount <= 0)
        return 0.0;
    return getEndScrollOffsetForIndex(childCount - 1);
}

// ── RenderSliverGrid ───────────────────────────────────────────────────────

void RenderSliverGrid::setGridDelegate(SharedPtr<SliverGridDelegate> delegate) {
    if (delegate == delegate_)
        return;
    // 上游：runtimeType 变了 或 shouldRelayout(旧) 为真才重排；同族同参可省。
    const bool relayout = !delegate_ || !delegate ||
                          typeid(*delegate) != typeid(*delegate_) ||
                          delegate->shouldRelayout(*delegate_);
    delegate_ = std::move(delegate);
    if (relayout)
        markNeedsLayout();
}

void RenderSliverGrid::setupParentData(RenderObject* child) {
    if (child && !child->parentData())
        child->setParentData(new SliverGridParentData());
    else
        RenderSliverMultiBoxAdaptor::setupParentData(child);
}

double RenderSliverGrid::paintExtentOf(RenderObject* child) const {
    const SliverGridParentData* pd =
        child && child->parentData()
            ? dynamic_cast<const SliverGridParentData*>(child->parentData())
            : nullptr;
    if (pd && pd->mainAxisExtent)
        return *pd->mainAxisExtent;
    // 本轮还没写回度量（刚入列）时退化到子节点自身的主轴尺寸。
    auto* box = child ? dynamic_cast<RenderBox*>(child) : nullptr;
    if (!box)
        return 0.0;
    return sliverConstraints_.axis() == Axis::Vertical ? box->size().height : box->size().width;
}

double RenderSliverGrid::childCrossAxisPosition(RenderObject* child) const {
    const SliverGridParentData* pd =
        child && child->parentData()
            ? dynamic_cast<const SliverGridParentData*>(child->parentData())
            : nullptr;
    return pd && pd->crossAxisOffset ? *pd->crossAxisOffset : 0.0;
}

void RenderSliverGrid::performLayout() {
    const SliverConstraints c = sliverConstraints();
    if (childManager_) {
        childManager_->didStartLayout();
        childManager_->setDidUnderflow(false);
    }
    if (!delegate_) {
        setGeometry(SliverGeometry::zero());
        if (childManager_)
            childManager_->didFinishLayout();
        return;
    }

    const SliverGridLayout layout = delegate_->getLayout(c);
    const int total = totalChildren();

    const double scrollOffset = c.scrollOffset + c.cacheOrigin;
    const double targetEndScrollOffset = scrollOffset + c.remainingCacheExtent;
    const bool hasTargetLastIndex = std::isfinite(targetEndScrollOffset);
    const int firstIndex = layout.getMinIndexForScrollOffset(scrollOffset);
    const int targetLastIndex =
        hasTargetLastIndex ? layout.getMaxIndexForScrollOffset(targetEndScrollOffset, total) : 0;

    // 上游先掐掉窗口外的存量子节点，再补造缺的。
    if (firstChild()) {
        const int leadingGarbage = calculateLeadingGarbage(firstIndex);
        const int trailingGarbage =
            hasTargetLastIndex ? calculateTrailingGarbage(targetLastIndex) : 0;
        collectGarbage(leadingGarbage, trailingGarbage);
    } else {
        collectGarbage(0, 0);
    }

    if (!firstChild()) {
        if (!addInitialChild(firstIndex, layout.getStartScrollOffsetForIndex(firstIndex))) {
            // 一个都造不出：空列表。已知总数时仍报出精确内容长度（上游同款：
            // 滚过末尾后要靠这个长度把偏移夹回来）。
            const double max = total > 0 ? layout.getMaxScrollOffset(total) : 0.0;
            SliverGeometry g;
            g.scrollExtent = max;
            g.maxPaintExtent = max;
            setGeometry(g);
            if (childManager_) {
                if (firstIndex > 0)
                    childManager_->setDidUnderflow(true);
                childManager_->didFinishLayout();
            }
            return;
        }
    }

    // 首子之前还缺的行：往回补造。造不出来说明 delegate 报的总数比 builder 能
    // 给的多，与 fixed-extent 一样把 scrollOffsetCorrection 交回视口重来。
    for (int index = indexOf(firstChild()) - 1; index >= firstIndex; --index) {
        const SliverGridGeometry geo = layout.getGeometryForIndex(index);
        RenderBox* child =
            insertAndLayoutLeadingChild(tightTileConstraints(c, geo.mainAxisExtent,
                                                             geo.crossAxisExtent));
        if (!child) {
            SliverGeometry g;
            g.scrollOffsetCorrection = layout.getStartScrollOffsetForIndex(index);
            setGeometry(g);
            if (childManager_)
                childManager_->didFinishLayout();
            return;
        }
    }

    // 尾部补造：一次拿不到就认定到了末尾。
    bool truncated = false;
    for (int index = indexOf(lastChild()) + 1; !hasTargetLastIndex || index <= targetLastIndex;
         ++index) {
        const SliverGridGeometry geo = layout.getGeometryForIndex(index);
        RenderBox* child =
            insertAndLayoutChild(tightTileConstraints(c, geo.mainAxisExtent, geo.crossAxisExtent),
                                 static_cast<RenderBox*>(lastChild()));
        if (!child) {
            truncated = true;
            break;
        }
    }

    // 窗口就位：逐个写回瓦片度量并布局（尺寸来自 delegate，见头文件偏离 3）。
    for (RenderObject* child = firstChild(); child; child = childAfter(child)) {
        auto* box = dynamic_cast<RenderBox*>(child);
        if (!box)
            continue;
        auto* pd = dynamic_cast<SliverGridParentData*>(child->parentData());
        const SliverGridGeometry geo = layout.getGeometryForIndex(indexOf(child));
        box->layout(tightTileConstraints(c, geo.mainAxisExtent, geo.crossAxisExtent), false);
        if (pd) {
            pd->layoutOffset = geo.scrollOffset;
            pd->mainAxisExtent = geo.mainAxisExtent;
            pd->crossAxisOffset = geo.crossAxisOffset;
        }
    }

    const int firstIndexNow = firstChild() ? indexOf(firstChild()) : 0;
    const int lastIndexNow = lastChild() ? indexOf(lastChild()) : -1;
    const double leadingScrollOffset = layout.getStartScrollOffsetForIndex(firstIndexNow);
    const double trailingScrollOffset =
        lastIndexNow >= 0 ? layout.getEndScrollOffsetForIndex(lastIndexNow) : leadingScrollOffset;

    double scrollExtent = layout.getMaxScrollOffset(total);
    if (truncated)
        scrollExtent = std::min(scrollExtent, trailingScrollOffset);

    const double paintExtent = calculatePaintOffset(c, leadingScrollOffset, trailingScrollOffset);
    const double cacheExtentValue = calculateCacheOffset(c, leadingScrollOffset, trailingScrollOffset);

    const double targetEndForPaint = c.scrollOffset + c.remainingPaintExtent;
    const bool hasTargetLastForPaint = std::isfinite(targetEndForPaint);
    const int targetLastForPaint =
        hasTargetLastForPaint ? layout.getMaxIndexForScrollOffset(targetEndForPaint, total) : 0;

    SliverGeometry g;
    g.scrollExtent = scrollExtent;
    g.maxPaintExtent = scrollExtent;
    g.paintExtent = paintExtent;
    g.cacheExtent = cacheExtentValue;
    // 上游注释：宁可保守也不要滚动时把裁剪抖没。
    g.hasVisualOverflow =
        (hasTargetLastForPaint && lastIndexNow >= targetLastForPaint) || c.scrollOffset > 0.0;
    setGeometry(g);
    updateChildrenPaintOffsets();

    // 本轮没能覆盖到窗口末尾：告诉托管人，好让它下一帧多看一项找边界。
    if ((truncated || (hasTargetLastIndex && lastIndexNow < targetLastIndex)) && childManager_)
        childManager_->setDidUnderflow(true);
    if (childManager_)
        childManager_->didFinishLayout();
}

}  // namespace cui_core
