/// @file rendering/render_flex.cpp
/// @brief RenderFlex 布局算法实现（对齐 上游的 flex.dart）。

#include "cui/rendering/render_flex.h"

#include <algorithm>

namespace cui {

void RenderFlex::setupParentData(RenderObject* child) {
    // 上游的 RenderFlex.setupParentData：创建 FlexParentData。
    child->setParentData(new FlexParentData());
}

int RenderFlex::flexOf(size_t index) const {
    const auto& list = children();
    if (index >= list.size() || !list[index])
        return 0;
    auto* pd = dynamic_cast<FlexParentData*>(list[index]->parentData());
    return pd ? pd->flex : 0;
}

FlexFit RenderFlex::fitOf(size_t index) const {
    const auto& list = children();
    if (index >= list.size() || !list[index])
        return FlexFit::Loose;
    auto* pd = dynamic_cast<FlexParentData*>(list[index]->parentData());
    return pd ? pd->fit : FlexFit::Loose;
}

void RenderFlex::performLayout() {
    const bool horizontal = direction_ == Axis::Horizontal;
    const size_t count = children().size();

    // 约束拆分：主轴与交叉轴各自的下限/上限。
    const double minMain = horizontal ? constraints_.minWidth : constraints_.minHeight;
    const double maxMain = horizontal ? constraints_.maxWidth : constraints_.maxHeight;
    const double minCross = horizontal ? constraints_.minHeight : constraints_.minWidth;
    const double maxCross = horizontal ? constraints_.maxHeight : constraints_.maxWidth;

    std::vector<double> mainSizes(count, 0.0);
    std::vector<double> crossSizes(count, 0.0);

    // 上游 `_constraintsForNonFlexChild` / `_constraintsForFlexChild` 里的
    // `fillCrossAxis`：只有 `CrossAxisAlignment.stretch` 会把交叉轴下限抬到
    // `maxCross`（即紧约束），其余对齐方式交叉轴下限都是 0。交叉轴无界时无法
    // 拉满（上游在 debug 里直接断言），保持松约束。
    const bool fillCrossAxis = crossAxisAlignment_ == CrossAxisAlignment::Stretch &&
                               maxCross != BoxConstraints::kInfinity;
    const double minCrossForChild = fillCrossAxis ? maxCross : 0.0;

    // ── 步骤 1：inflexible 子节点（flex == 0）取自然尺寸 ─────────────────
    // 上游 用「主轴无界、交叉轴有界」的约束测量自然尺寸。
    //
    // 上游 `canFlex = maxMainSize.isFinite`（rendering/flex.dart 的 `_computeSizes`）：
    // **主轴无界时 flex 子一律按非 flex 处理**（debug 模式会抛
    // "RenderFlex children have non-zero flex but incoming height constraints are
    // unbounded"，release 模式则让 Expanded 退化成自然尺寸子节点）。本仓以前把
    // 无界剩余空间当成 1e18 直接紧约束塞给 Expanded，于是「滚动视图里的
    // Column + Expanded」这一常见写法得到 1e18 高的子节点，而 Column 自身只按
    // 非 flex 子求和（几十像素）——内容照样画出来，命中测试却被父盒的
    // `size().contains()` 全部挡掉（点击无响应）。intrinsic 侧早已是 canFlex 口径，
    // 这里补齐 performLayout。
    const bool canFlex = maxMain != BoxConstraints::kInfinity;

    double allocatedMain = 0.0;
    double maxCrossOfChildren = 0.0;

    for (size_t i = 0; i < count; ++i) {
        RenderObject* child = children()[i];
        if (!child || (canFlex && flexOf(i) > 0))
            continue;

        BoxConstraints childConstraints =
                horizontal
                    ? BoxConstraints(0.0, BoxConstraints::kInfinity, minCrossForChild, maxCross)
                    : BoxConstraints(minCrossForChild, maxCross, 0.0,
                                     BoxConstraints::kInfinity);
        child->layout(childConstraints);
        Size childSize = child->size();

        double main = horizontal ? childSize.width : childSize.height;
        double cross = horizontal ? childSize.height : childSize.width;

        allocatedMain += main;
        mainSizes[i] = main;
        crossSizes[i] = cross;
        maxCrossOfChildren = std::max(maxCrossOfChildren, cross);
    }

    // ── 步骤 2：剩余空间按 flex 权重分配给 flexible 子节点 ──────────────
    double remaining = std::max(0.0, maxMain - allocatedMain);

    int totalFlex = 0;
    if (canFlex) {
        for (size_t i = 0; i < count; ++i)
            totalFlex += flexOf(i);
    }

    if (totalFlex > 0) {
        for (size_t i = 0; i < count; ++i) {
            RenderObject* child = children()[i];
            int flex = flexOf(i);
            if (flex <= 0 || !child)
                continue;
            double share = remaining * (flex / static_cast<double>(totalFlex));

            // Expanded(tight) 严格等于分配值（主轴紧约束）；
            // Flexible(loose) 主轴上限为分配值、最小为 0。
            BoxConstraints childConstraints;
            if (horizontal) {
                childConstraints = fitOf(i) == FlexFit::Tight
                                      ? BoxConstraints(share, share, minCrossForChild, maxCross)
                                      : BoxConstraints(0.0, share, minCrossForChild, maxCross);
            } else {
                childConstraints = fitOf(i) == FlexFit::Tight
                                      ? BoxConstraints(minCrossForChild, maxCross, share, share)
                                      : BoxConstraints(minCrossForChild, maxCross, 0.0, share);
            }
            child->layout(childConstraints);
            Size cs = child->size();
            // 上游语义：`Expanded`（FlexFit.tight）**占满分配到的空间**，
            // 即使子节点自己是零尺寸（典型例子就是 `Spacer` =
            // `Expanded(child: SizedBox.shrink())`）——分配的份额决定后续
            // 兄弟节点的落点，所以这里用 share 而不是子节点实际尺寸。
            mainSizes[i] = fitOf(i) == FlexFit::Tight
                               ? share
                               : (horizontal ? cs.width : cs.height);
            crossSizes[i] = horizontal ? cs.height : cs.width;
            maxCrossOfChildren = std::max(maxCrossOfChildren, crossSizes[i]);
        }
    }

    // ── 步骤 3：确定自身尺寸 ────────────────────────────────────────────
    double ownMain;
    if (mainAxisSize_ == MainAxisSize::Max) {
        ownMain = (maxMain == BoxConstraints::kInfinity) ? allocatedMain : maxMain;
    } else {
        ownMain = allocatedMain;
    }
    ownMain = std::max(ownMain, minMain);

    double ownCross = std::max(maxCrossOfChildren, minCross);
    if (maxCross != BoxConstraints::kInfinity)
        ownCross = std::min(ownCross, maxCross);

    if (horizontal)
        setSize({ownMain, ownCross});
    else
        setSize({ownCross, ownMain});

    // ── 步骤 4：按 mainAxisAlignment 计算起始位置与间隙 ─────────────────
    const double ownMainExtent = horizontal ? size_.width : size_.height;
    const double ownCrossExtent = horizontal ? size_.height : size_.width;

    double freeSpace = std::max(0.0, ownMainExtent - allocatedMain);
    double leading = 0.0;
    double gap = 0.0;

    switch (mainAxisAlignment_) {
        case MainAxisAlignment::Start:
            break;
        case MainAxisAlignment::End:
            leading = freeSpace;
            break;
        case MainAxisAlignment::Center:
            leading = freeSpace / 2.0;
            break;
        case MainAxisAlignment::SpaceBetween:
            if (count > 1)
                gap = freeSpace / static_cast<double>(count - 1);
            break;
        case MainAxisAlignment::SpaceAround:
            if (count > 0) {
                gap = freeSpace / static_cast<double>(count);
                leading = gap / 2.0;
            }
            break;
        case MainAxisAlignment::SpaceEvenly:
            if (count > 0) {
                gap = freeSpace / static_cast<double>(count + 1);
                leading = gap;
            }
            break;
    }

    // ── 步骤 5：写入子节点偏移（交叉轴对齐） ────────────────────────────
    double cursor = leading;
    for (size_t i = 0; i < count; ++i) {
        RenderObject* child = children()[i];
        if (!child)
            continue;

        double childCross = crossSizes[i];
        double crossOffset = 0.0;
        switch (crossAxisAlignment_) {
            case CrossAxisAlignment::Start:
                crossOffset = 0.0;
                break;
            case CrossAxisAlignment::End:
                crossOffset = ownCrossExtent - childCross;
                break;
            case CrossAxisAlignment::Center:
                crossOffset = (ownCrossExtent - childCross) / 2.0;
                break;
            case CrossAxisAlignment::Stretch:
                crossOffset = 0.0;
                break;
            case CrossAxisAlignment::Baseline:
                crossOffset = 0.0;
                break;
        }

        if (auto* pd = child->parentData())
            pd->offset = horizontal ? Offset(cursor, crossOffset) : Offset(crossOffset, cursor);
        cursor += mainSizes[i] + gap;
    }
}

// ── Intrinsics（上游的 _getIntrinsicSize）───────────────────────────────────

double RenderFlex::getIntrinsicSize(Axis sizingDirection, double extent,
                                   const ChildSizingFunction& childSize) {
    const auto& list = children();
    const bool horizontal = direction_ == Axis::Horizontal;
    // 子节点在「主轴」上的 max intrinsic（无界环境下能长多大）。
    auto maxMainIntrinsic = [&](RenderBox* c) {
        return horizontal ? c->getMaxIntrinsicWidth(BoxConstraints::kInfinity)
                          : c->getMaxIntrinsicHeight(BoxConstraints::kInfinity);
    };

    if (sizingDirection == direction_) {
        // INTRINSIC MAIN SIZE：flex 子折算成「单位 flex 所需长度的最大值」，
        // 其余子直接累加自身 intrinsic。
        double totalFlex = 0.0;
        double inflexibleSpace = 0.0;  // 上游此处是 spacing * (childCount - 1)，本仓无 spacing
        double maxFlexFractionSoFar = 0.0;
        for (size_t i = 0; i < list.size(); ++i) {
            RenderObject* child = list[i];
            if (!child)
                continue;
            const int flex = flexOf(i);
            totalFlex += flex;
            if (flex > 0) {
                maxFlexFractionSoFar = std::max(
                        maxFlexFractionSoFar,
                        childSize(static_cast<RenderBox*>(child), extent) / static_cast<double>(flex));
            } else {
                inflexibleSpace += childSize(static_cast<RenderBox*>(child), extent);
            }
        }
        return maxFlexFractionSoFar * totalFlex + inflexibleSpace;
    }

    // INTRINSIC CROSS SIZE：先复算主轴分配（上游由 _computeSizes 完成），
    // 再取每个子在该主轴长度下交叉轴 intrinsic 的最大值。
    //   extent 即上游那个「主轴上限 = extent、另一轴无界」的合成约束；
    //   extent 无界时（canFlex = false）flex 子也按非 flex 处理——上游同规则。
    const double maxMainSize = extent;
    const bool canFlex = isBoundedExtent(maxMainSize);
    double accumulatedMain = 0.0;
    double totalFlex = 0.0;
    for (size_t i = 0; i < list.size(); ++i) {
        RenderObject* child = list[i];
        if (!child)
            continue;
        const int flex = flexOf(i);
        if (canFlex && flex > 0) {
            totalFlex += flex;
        } else {
            accumulatedMain += maxMainIntrinsic(static_cast<RenderBox*>(child));
        }
    }
    const double spacePerFlex = totalFlex > 0.0
            ? std::max(0.0, maxMainSize - accumulatedMain) / totalFlex
            : 0.0;

    double extentSoFar = 0.0;
    for (size_t i = 0; i < list.size(); ++i) {
        RenderObject* child = list[i];
        if (!child)
            continue;
        const int flex = flexOf(i);
        const double mainExtent =
                (canFlex && flex > 0) ? spacePerFlex * static_cast<double>(flex)
                                      : maxMainIntrinsic(static_cast<RenderBox*>(child));
        extentSoFar = std::max(extentSoFar, childSize(static_cast<RenderBox*>(child), mainExtent));
    }
    // 上游的合成约束里另一轴无界，故 applyConstraints 不会改变交叉轴结果。
    return extentSoFar;
}

double RenderFlex::computeMinIntrinsicWidth(double height) {
    return getIntrinsicSize(Axis::Horizontal, height,
                            [](RenderBox* c, double e) { return c->getMinIntrinsicWidth(e); });
}

double RenderFlex::computeMaxIntrinsicWidth(double height) {
    return getIntrinsicSize(Axis::Horizontal, height,
                            [](RenderBox* c, double e) { return c->getMaxIntrinsicWidth(e); });
}

double RenderFlex::computeMinIntrinsicHeight(double width) {
    return getIntrinsicSize(Axis::Vertical, width,
                            [](RenderBox* c, double e) { return c->getMinIntrinsicHeight(e); });
}

double RenderFlex::computeMaxIntrinsicHeight(double width) {
    return getIntrinsicSize(Axis::Vertical, width,
                            [](RenderBox* c, double e) { return c->getMaxIntrinsicHeight(e); });
}

}  // namespace cui_core
