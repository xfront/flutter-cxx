#pragma once
/// @file rendering/render_flex.h
/// @brief 上游的 RenderFlex 移植。
///
/// 对应 上游源码：
///   上游 rendering/flex.dart
///
/// 上游的 flex 布局算法（与本移植严格一致）：
///   1. 沿主轴分配「 inflexible」子节点（flex == 0）自然尺寸。
///   2. 剩余空间按 flex 权重分配给 flexible 子节点。
///   3. 按 mainAxisAlignment 摆放子节点。
///   4. 按 crossAxisAlignment 确定交叉轴位置与尺寸。
///
/// flex 系数不保存在 RenderFlex 里，而是写在每个子节点的
/// `FlexParentData` 中——这正是 上游的做法：Expanded / Flexible 是
/// `ParentDataWidget`，它们把 flex 写进子 RenderObject 的 parentData。

#include <memory>
#include <vector>

#include "cui/rendering/render_object.h"
#include "cui/rendering/render_box.h"

namespace cui {

/// 主轴方向。上游的 `Axis`。
enum class Axis { Horizontal, Vertical };

/// 主轴对齐。上游的 `MainAxisAlignment`。
enum class MainAxisAlignment {
    Start,
    End,
    Center,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

/// 交叉轴对齐。上游的 `CrossAxisAlignment`。
enum class CrossAxisAlignment {
    Start,
    End,
    Center,
    Stretch,
    Baseline,
};

/// 主轴尺寸模式。上游的 `MainAxisSize`。
enum class MainAxisSize {
    Min,  ///< 主轴方向取子节点总长（不占满）。
    Max,  ///< 主轴方向占满可用空间。
};

/// Wrap 的 run 内 / run 间对齐。上游的 `WrapAlignment`（wrap.dart）。
enum class WrapAlignment {
    Start,
    End,
    Center,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

/// Wrap 的交叉轴对齐。上游的 `WrapCrossAlignment`（wrap.dart）。
enum class WrapCrossAlignment {
    Start,
    End,
    Center,
};

/// Flex 布局渲染对象。上游的 `RenderFlex`。
class RenderFlex : public RenderBoxContainer {
public:
    RenderFlex(Axis direction, MainAxisAlignment mainAxisAlignment = MainAxisAlignment::Start,
               CrossAxisAlignment crossAxisAlignment = CrossAxisAlignment::Center,
               MainAxisSize mainAxisSize = MainAxisSize::Max)
        : direction_(direction), mainAxisAlignment_(mainAxisAlignment),
          crossAxisAlignment_(crossAxisAlignment), mainAxisSize_(mainAxisSize) {}

    /// 四个参数的 setter 对齐上游 的 `RenderFlex`：等值即早退，否则
    /// markNeedsLayout。此前这里是「静默赋值」，而
    /// Flex::updateRenderObject 每次重建都会调一遍——参数变了却没人标脏，
    /// 只有恰好被别的脏位带上才重排（Column↔Row 改向不生效）。
    Axis direction() const { return direction_; }
    void setDirection(Axis d) {
        if (d == direction_)
            return;
        direction_ = d;
        markNeedsLayout();
    }

    MainAxisAlignment mainAxisAlignment() const { return mainAxisAlignment_; }
    void setMainAxisAlignment(MainAxisAlignment a) {
        if (a == mainAxisAlignment_)
            return;
        mainAxisAlignment_ = a;
        markNeedsLayout();
    }

    CrossAxisAlignment crossAxisAlignment() const { return crossAxisAlignment_; }
    void setCrossAxisAlignment(CrossAxisAlignment a) {
        if (a == crossAxisAlignment_)
            return;
        crossAxisAlignment_ = a;
        markNeedsLayout();
    }

    MainAxisSize mainAxisSize() const { return mainAxisSize_; }
    void setMainAxisSize(MainAxisSize s) {
        if (s == mainAxisSize_)
            return;
        mainAxisSize_ = s;
        markNeedsLayout();
    }

    /// 子节点数量。
    size_t childCount() const { return children().size(); }

    /// 第 index 个子节点的 flex 权重（从其 FlexParentData 读取）。
    int flexOf(size_t index) const;

    /// 第 index 个子节点的适配方式。
    FlexFit fitOf(size_t index) const;

    /// 创建 FlexParentData（上游的 setupParentData）。
    void setupParentData(RenderObject* child) override;

protected:
    void performLayout() override;

    ///@{ 上游的 `RenderFlex.compute*Intrinsic*`（都走 `_getIntrinsicSize`）。
    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    double computeMinIntrinsicHeight(double width) override;
    double computeMaxIntrinsicHeight(double width) override;
    ///@}

private:
    /// 上游的 `_ChildSizingFunction`：在给定主轴长度 `extent` 下，取子节点
    /// 「被询问的那个维度」的 intrinsic（min/max 由调用点决定）。
    using ChildSizingFunction = std::function<double(RenderBox* child, double extent)>;

    /// 上游的 `RenderFlex._getIntrinsicSize`。
    ///
    /// sizingDirection == direction_ 时算**主轴** intrinsic：flex 子按
    /// 「单位 flex 所需长度的最大值」折算，其余子累加；
    /// 否则算**交叉轴** intrinsic：先复算一遍主轴分配（非 flex 子取自身
    /// max intrinsic 主轴长，flex 子按剩余空间分配），再取各子在该长度下
    /// 交叉轴 intrinsic 的最大值。
    ///
    /// 上游交叉轴分支复用 `_computeSizes`（performLayout 的同一份实现）；
    /// 本仓的 performLayout 尚未拆出共享的 sizes 计算，故这里按同一分配
    /// 规则等价复算（本仓 RenderFlex 无 spacing，交叉轴 stretch 也不收紧
    /// 子约束，两点差异对结果无影响）。
    double getIntrinsicSize(Axis sizingDirection, double extent,
                           const ChildSizingFunction& childSize);

    Axis direction_;
    MainAxisAlignment mainAxisAlignment_;
    CrossAxisAlignment crossAxisAlignment_;
    MainAxisSize mainAxisSize_;
};

}  // namespace cui_core
