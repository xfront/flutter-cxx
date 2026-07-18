#pragma once
/// @file widgets/grid.h
/// @brief 网格控件层：SliverGridDelegate 家族 + SliverGrid 控件。
///
/// 对应 上游源码：
///   widgets/grid.dart（SliverGridDelegateWithFixedCrossAxisCount /
///   SliverGridDelegateWithMaxCrossAxisExtent / debugAssertIsValid）
///   widgets/sliver.dart（SliverGrid）
///
/// 偏离（叠加在 widgets/sliver.h 文件头那几条之上）：
///   1. 上游还有 `SliverGridDelegateWithMinCrossAxisExtent`（3.27 之后新增，
///      交叉轴「至少这么宽」的一族 + `_SliverGridExtentTileLayout`）。本仓
///      未移植：它要的是非规则（tileExtent）布局，见 rendering/sliver_grid.h
///      偏离 1。
///   2. 上游 `debugAssertIsValid` 在 assert() 里跑（release 无校验）。本仓把
///      同样的校验做成 `validateMessage()`，控件层构造时不强制调用，测试里
///      按需断言；数值非法时 getLayout 另有夹取兜底（列数至少 1、间距非负、
///      比例正数）。
///   3. 上游 `SliverGrid` 的父链是 `SliverWithKeepAliveWidget`，带
///      `addAutomaticKeepAlives`/`addRepaintBoundaries`/`addSemanticIndex`
///      三开关；本仓这三件套未移植（见 widgets/sliver.h 文件头）。

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/rendering/sliver.h"
#include "cui/rendering/sliver_grid.h"
#include "cui/widgets/sliver.h"

namespace cui {

namespace grid_detail {

/// 上游 `_getCrossAxisCount`（MaxCrossAxisExtent 那一族）：
/// `((crossAxisExtent + maxCrossAxisExtent + crossAxisSpacing) ~/
///  (maxCrossAxisExtent + crossAxisSpacing))`，并保证至少 1 列。
inline int crossAxisCountForMaxExtent(double crossAxisExtent, double maxCrossAxisExtent,
                                      double crossAxisSpacing) {
    const double stride = maxCrossAxisExtent + crossAxisSpacing;
    if (stride <= 0.0 || crossAxisExtent <= 0.0)
        return 1;
    const int count = static_cast<int>((crossAxisExtent + stride) / stride);
    return std::max(1, count);
}

/// 上游 `SliverGridRegularTileLayout` 的装配：由列数与瓦片尺寸算出两个 stride。
inline SliverGridLayout regularLayout(const SliverConstraints& c, int crossAxisCount,
                                      double mainAxisSpacing, double crossAxisSpacing,
                                      double childAspectRatio,
                                      const std::optional<double>& mainAxisExtent) {
    const int count = std::max(1, crossAxisCount);
    const double spacingMain = std::max(0.0, mainAxisSpacing);
    const double spacingCross = std::max(0.0, crossAxisSpacing);
    const double aspect = childAspectRatio > 0.0 ? childAspectRatio : 1.0;

    const double crossAxisExtent = std::max(0.0, c.crossAxisExtent);
    const double childCross = std::max(0.0, (crossAxisExtent - (count - 1) * spacingCross) / count);
    const double childMain = mainAxisExtent.value_or(childCross / aspect);

    SliverGridLayout layout;
    layout.crossAxisCount = count;
    layout.childCrossAxisExtent = childCross;
    layout.childMainAxisExtent = childMain;
    layout.crossAxisStride = childCross + spacingCross;
    layout.mainAxisStride = childMain + spacingMain;
    // 上游的 axisDirectionIsReversed(constraints.crossAxisDirection)。
    layout.reverseCrossAxis = axisDirectionIsReversed(c.crossAxisDirection);
    return layout;
}

}  // namespace grid_detail

// ── SliverGridDelegateWithFixedCrossAxisCount ───────────────────────────────

/// 固定列数的网格 delegate。上游的 `SliverGridDelegateWithFixedCrossAxisCount`。
class SliverGridDelegateWithFixedCrossAxisCount final : public SliverGridDelegate {
public:
    SliverGridDelegateWithFixedCrossAxisCount(int crossAxisCount, double mainAxisSpacing = 0.0,
                                              double crossAxisSpacing = 0.0,
                                              double childAspectRatio = 1.0,
                                              std::optional<double> mainAxisExtent = std::nullopt)
        : crossAxisCount_(crossAxisCount),
          mainAxisSpacing_(mainAxisSpacing),
          crossAxisSpacing_(crossAxisSpacing),
          childAspectRatio_(childAspectRatio),
          mainAxisExtent_(mainAxisExtent) {}

    int crossAxisCount() const { return crossAxisCount_; }
    double mainAxisSpacing() const { return mainAxisSpacing_; }
    double crossAxisSpacing() const { return crossAxisSpacing_; }
    double childAspectRatio() const { return childAspectRatio_; }
    const std::optional<double>& mainAxisExtent() const { return mainAxisExtent_; }

    SliverGridLayout getLayout(const SliverConstraints& c) const override {
        return grid_detail::regularLayout(c, crossAxisCount_, mainAxisSpacing_, crossAxisSpacing_,
                                          childAspectRatio_, mainAxisExtent_);
    }

    bool shouldRelayout(const SliverGridDelegate& oldDelegate) const override {
        const auto* o = dynamic_cast<const SliverGridDelegateWithFixedCrossAxisCount*>(&oldDelegate);
        if (!o)
            return true;
        return o->crossAxisCount_ != crossAxisCount_ || o->mainAxisSpacing_ != mainAxisSpacing_ ||
               o->crossAxisSpacing_ != crossAxisSpacing_ ||
               o->childAspectRatio_ != childAspectRatio_ || o->mainAxisExtent_ != mainAxisExtent_;
    }

    /// 上游的 debugAssertIsValid（见文件头偏离 2）。
    std::optional<std::string> validateMessage() const {
        if (crossAxisCount_ <= 0)
            return "crossAxisCount must be greater than 0";
        if (mainAxisSpacing_ < 0.0 || crossAxisSpacing_ < 0.0)
            return "spacing must be non-negative";
        if (childAspectRatio_ <= 0.0)
            return "childAspectRatio must be greater than zero";
        if (mainAxisExtent_ && *mainAxisExtent_ <= 0.0)
            return "mainAxisExtent must be greater than zero";
        return std::nullopt;
    }

private:
    int crossAxisCount_;
    double mainAxisSpacing_;
    double crossAxisSpacing_;
    double childAspectRatio_;
    std::optional<double> mainAxisExtent_;
};

// ── SliverGridDelegateWithMaxCrossAxisExtent ────────────────────────────────

/// 每格不超过给定宽度的网格 delegate。
/// 上游的 `SliverGridDelegateWithMaxCrossAxisExtent`。
class SliverGridDelegateWithMaxCrossAxisExtent final : public SliverGridDelegate {
public:
    SliverGridDelegateWithMaxCrossAxisExtent(double maxCrossAxisExtent,
                                             double mainAxisSpacing = 0.0,
                                             double crossAxisSpacing = 0.0,
                                             double childAspectRatio = 1.0,
                                             std::optional<double> mainAxisExtent = std::nullopt)
        : maxCrossAxisExtent_(maxCrossAxisExtent),
          mainAxisSpacing_(mainAxisSpacing),
          crossAxisSpacing_(crossAxisSpacing),
          childAspectRatio_(childAspectRatio),
          mainAxisExtent_(mainAxisExtent) {}

    double maxCrossAxisExtent() const { return maxCrossAxisExtent_; }

    SliverGridLayout getLayout(const SliverConstraints& c) const override {
        const int count = grid_detail::crossAxisCountForMaxExtent(
            c.crossAxisExtent, maxCrossAxisExtent_, std::max(0.0, crossAxisSpacing_));
        return grid_detail::regularLayout(c, count, mainAxisSpacing_, crossAxisSpacing_,
                                          childAspectRatio_, mainAxisExtent_);
    }

    bool shouldRelayout(const SliverGridDelegate& oldDelegate) const override {
        const auto* o = dynamic_cast<const SliverGridDelegateWithMaxCrossAxisExtent*>(&oldDelegate);
        if (!o)
            return true;
        return o->maxCrossAxisExtent_ != maxCrossAxisExtent_ ||
               o->mainAxisSpacing_ != mainAxisSpacing_ ||
               o->crossAxisSpacing_ != crossAxisSpacing_ ||
               o->childAspectRatio_ != childAspectRatio_ || o->mainAxisExtent_ != mainAxisExtent_;
    }

    std::optional<std::string> validateMessage() const {
        if (maxCrossAxisExtent_ <= 0.0)
            return "maxCrossAxisExtent must be greater than zero";
        if (mainAxisSpacing_ < 0.0 || crossAxisSpacing_ < 0.0)
            return "spacing must be non-negative";
        if (childAspectRatio_ <= 0.0)
            return "childAspectRatio must be greater than zero";
        if (mainAxisExtent_ && *mainAxisExtent_ <= 0.0)
            return "mainAxisExtent must be greater than zero";
        return std::nullopt;
    }

private:
    double maxCrossAxisExtent_;
    double mainAxisSpacing_;
    double crossAxisSpacing_;
    double childAspectRatio_;
    std::optional<double> mainAxisExtent_;
};

// ── SliverGrid ──────────────────────────────────────────────────────────────

/// 网格式 sliver。上游的 `SliverGrid`（widgets/sliver.dart）。
///
/// 子节点仍然由 `SliverMultiBoxAdaptorElement` 按序号懒建（本仓 Element 对
/// RenderObject 类型不敏感），只是位置/尺寸交给 gridDelegate。序号决定位置、
/// 与既有子节点的布局偏移无关 ⇒ `replaceMovedChildren()` 取 false（与
/// SliverFixedExtentList 同侧）。
class SliverGrid : public SliverMultiBoxAdaptorWidget {
public:
    SliverGrid(SharedPtr<SliverGridDelegate> gridDelegate,
               SharedPtr<SliverChildDelegate> delegate, KeyPtr key = nullptr)
        : SliverMultiBoxAdaptorWidget(std::move(delegate), std::move(key)),
          gridDelegate_(std::move(gridDelegate)) {}

    /// 上游的 `SliverGrid(children: [...])`。
    SliverGrid(SharedPtr<SliverGridDelegate> gridDelegate,
               std::vector<SharedPtr<Widget>> children, KeyPtr key = nullptr)
        : SliverMultiBoxAdaptorWidget(
              cui::make_shared<SliverChildListDelegate>(std::move(children)), std::move(key)),
          gridDelegate_(std::move(gridDelegate)) {}

    /// 上游的 `SliverGrid.builder`。
    static SharedPtr<SliverGrid> builder(
        SharedPtr<SliverGridDelegate> gridDelegate, NullableIndexedWidgetBuilder itemBuilder,
        std::optional<int> itemCount = std::nullopt,
        ChildIndexGetter findChildIndexCallback = nullptr, KeyPtr key = nullptr) {
        return cui::make_shared<SliverGrid>(
            std::move(gridDelegate),
            cui::make_shared<SliverChildBuilderDelegate>(std::move(itemBuilder),
                                                         std::move(findChildIndexCallback),
                                                         itemCount),
            std::move(key));
    }

    const SharedPtr<SliverGridDelegate>& gridDelegate() const { return gridDelegate_; }

    RenderObject* createRenderObject() override {
        return new RenderSliverGrid(nullptr, gridDelegate_);
    }

    void updateRenderObject(RenderObject* ro) override {
        if (auto* g = dynamic_cast<RenderSliverGrid*>(ro))
            g->setGridDelegate(gridDelegate_);
    }

    std::string toStringShort() const override { return "SliverGrid"; }

private:
    SharedPtr<SliverGridDelegate> gridDelegate_;
};

}  // namespace cui_core
