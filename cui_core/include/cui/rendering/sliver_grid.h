#pragma once
/// @file rendering/sliver_grid.h
/// @brief 网格 sliver 渲染层：网格布局度量 + RenderSliverGrid。
///
/// 对应 上游源码：
///   rendering/sliver_grid.dart（SliverGridParentData、SliverGridDelegate、
///   SliverGridLayout、SliverGridRegularTileLayout、SliverGridGeometry、
///   RenderSliverGrid）
///
/// ── 三处有意的偏离（与 rendering/sliver.h 文件头的四条叠加）───────────────
///   1. 上游的 `SliverGridLayout` 是抽象基类，有两个实现（regular tile 与
///      新的 tileExtent 版），自定义 delegate 可以子类化它做瀑布流。本仓把
///      「一个布局实例」压成值类型 `SliverGridLayout`（regular tile 的六项
///      参数 + 同一套查询方法）：delegate 仍是多态扩展点，但只能返回规则网格
///      度量。tileExtent 版未移植。
///   2. 上游的 `SliverGridLayout.getMaxScrollOffset()` 直接读子节点总数；本仓
///      的布局实例不持有它，总长度经 `childCount` 入参给出（同源信息，来源是
///      `RenderSliverGrid::totalChildren()` = 托管人的 `childCount()`）。
///   3. 上游的 `SliverGridParentData` 存 `Size size` + `Offset paintOffset`
///      （paintOffset 是最终绘制位置）；本仓的最终绘制偏移一律由基类
///      `updateChildrenPaintOffsets()` 从「主轴布局偏移 + 交叉轴偏移」合成
///      （见 sliver.h 偏离 2），故 parentData 只存交叉轴偏移与主轴尺寸，
///      避免同一个槽位被两处写。
///
/// 另有两条与懒加载列表一致的口径，别按上游逐字对照：
///   - `RenderSliverGrid` 不走托管人的 `estimateMaxScrollOffset`：tile 的位置
///     与尺寸完全由 delegate + 序号决定，不需要按已物化子节点外推。
///   - 上游 performLayout 里有个 `maxErrorTurns` 收敛循环（子节点尺寸反过来
///     影响窗口时反复重算）。本仓 tile 尺寸与子节点内容无关，一轮即收敛。

#include <memory>
#include <optional>

#include "../utils/shared_ptr.h"
#include "cui/rendering/sliver.h"

namespace cui {

// ── 单个瓦片的度量 ─────────────────────────────────────────────────────────

/// 一个瓦片在 sliver 逻辑坐标系里的位置与尺寸。
/// 上游的 `SliverGridGeometry`。
struct SliverGridGeometry {
    /// 瓦片靠近零滚动点的一侧，距零滚动点的距离。
    double scrollOffset = 0.0;
    /// 瓦片在交叉轴上距 sliver 交叉轴起点的距离。
    double crossAxisOffset = 0.0;
    double mainAxisExtent = 0.0;
    double crossAxisExtent = 0.0;

    bool operator==(const SliverGridGeometry&) const = default;
};

/// 规则网格的布局度量。上游的 `SliverGridRegularTileLayout`（见文件头偏离 1）。
struct SliverGridLayout {
    /// 交叉轴方向的瓦片数（垂直滚动时即「每行几个」）。
    int crossAxisCount = 1;
    /// 相邻两行（列）起点的间距 = 瓦片主轴尺寸 + mainAxisSpacing。
    double mainAxisStride = 0.0;
    /// 相邻两列（行）起点的间距 = 瓦片交叉轴尺寸 + crossAxisSpacing。
    double crossAxisStride = 0.0;
    double childMainAxisExtent = 0.0;
    double childCrossAxisExtent = 0.0;
    /// 交叉轴反向（`Up`/`Left` 滚动的伴生值）：同一行里瓦片从另一头开始排。
    bool reverseCrossAxis = false;

    bool operator==(const SliverGridLayout&) const = default;

    /// 上游的 `getStartScrollOffsetForIndex`：所在行（列）的起点。
    double getStartScrollOffsetForIndex(int index) const;
    /// 上游的 `getEndScrollOffsetForIndex`。
    double getEndScrollOffsetForIndex(int index) const;
    /// 上游的 `getExtentForIndex`。
    double getExtentForIndex(int index) const {
        (void)index;
        return childMainAxisExtent;
    }
    /// 上游的 `getGeometryForIndex`。
    SliverGridGeometry getGeometryForIndex(int index) const;

    /// 上游的 `getMinIndexForScrollOffset`：与 scrollOffset 相交的那一行里
    /// 的第一个序号（>= 0）。
    int getMinIndexForScrollOffset(double scrollOffset) const;
    /// 上游的 `getMaxIndexForScrollOffset` + 子节点总数夹取（偏离 2）。
    int getMaxIndexForScrollOffset(double scrollOffset, int childCount) const;

    /// 上游的 `getMaxScrollOffset`：最后一个瓦片的尾部（内容总长）。
    double getMaxScrollOffset(int childCount) const;

    /// 某个序号落在交叉轴上的位置（reverseCrossAxis 已折进去）。
    double getCrossAxisOffsetForIndex(int index) const;
};

// ── SliverGridDelegate ─────────────────────────────────────────────────────

/// 把 sliver 约束翻译成布局度量。上游的 `SliverGridDelegate`。
///
/// 偏离：上游的 `shouldRelayout` 用 Dart 的 covariant 参数（子类签名收同族
/// 子类）；C++ 虚函数不能协变参数，故一律收基类引用，实现里 dynamic_cast
/// 到自己那族再逐字段比较（跨族比较退化「要重排」，与上游换 runtimeType
/// 即重排的行为一致）。
class SliverGridDelegate {
public:
    virtual ~SliverGridDelegate() = default;

    virtual SliverGridLayout getLayout(const SliverConstraints& constraints) const = 0;

    /// 上游同名：度量是否随旧 delegate 变过（同参数时返回 false 可省一轮布局）。
    /// 基类照上游给 false，具体族各自逐字段比较。
    virtual bool shouldRelayout(const SliverGridDelegate& oldDelegate) const {
        (void)oldDelegate;
        return false;
    }
};

// ── SliverGridParentData ───────────────────────────────────────────────────

/// 网格子节点的 parentData。上游的 `SliverGridParentData`（见文件头偏离 3）。
struct SliverGridParentData : SliverMultiBoxAdaptorParentData {
    /// 瓦片的主轴尺寸（由 delegate 给定，与子节点自身尺寸无关）。
    std::optional<double> mainAxisExtent;
    /// 瓦片在交叉轴上的位置。
    std::optional<double> crossAxisOffset;
};

// ── RenderSliverGrid ───────────────────────────────────────────────────────

/// 网格 sliver。上游的 `RenderSliverGrid`。
///
/// 子节点同样由 `RenderSliverBoxChildManager` 在布局期间造销；与
/// `RenderSliverFixedExtentList` 的差别只在「序号 → 位置/尺寸」这一步交给
/// `SliverGridDelegate`，且每个子节点的尺寸是紧约束（瓦片大小）。
class RenderSliverGrid : public RenderSliverMultiBoxAdaptor {
public:
    RenderSliverGrid() = default;
    explicit RenderSliverGrid(RenderSliverBoxChildManager* childManager,
                              SharedPtr<SliverGridDelegate> gridDelegate = nullptr)
        : RenderSliverMultiBoxAdaptor(childManager), delegate_(std::move(gridDelegate)) {}

    /// 上游的 `gridDelegate`。本仓用 shared_ptr 持有：上游靠 GC 保证 delegate
    /// 活得比渲染对象久，这里 widget_ 换代时旧的可能当场析构（见
    /// widgets/sliver.cpp::update 的同款教训）。
    const SharedPtr<SliverGridDelegate>& gridDelegate() const { return delegate_; }
    void setGridDelegate(SharedPtr<SliverGridDelegate> delegate);

    void setupParentData(RenderObject* child) override;
    void performLayout() override;

    /// 上游的 `paintExtentOf(child)`：网格读 parentData 里记下的瓦片主轴尺寸。
    /// 与基类同为 public（视口/命中测试/绘制都从这里问子节点度量）。
    double paintExtentOf(RenderObject* child) const override;
    /// 上游的 `childCrossAxisPosition(child)`：瓦片在交叉轴上的位置。
    double childCrossAxisPosition(RenderObject* child) const override;

protected:
    SharedPtr<SliverGridDelegate> delegate_;
};

}  // namespace cui_core
