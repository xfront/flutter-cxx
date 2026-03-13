#pragma once
/// @file foundation/parent_data.h
/// @brief Per-child layout slots the parent's RenderObject writes into.
///
/// Mirrors the upstream's `ParentData`: a parent container allocates a ParentData
/// sub-object for each child RenderObject and stashes the child-specific
/// layout hints there (offset after layout, flex factor, positioning ...).
/// Children never read their own parentData; the parent's `performLayout`
/// does.
///
/// 上游语义对齐要点：
///   - `ParentData.offset` 是子节点相对父节点的偏移（上游 把它放在
///     BoxParentData；这里提升到基类，让 hit-testing / localToGlobal /
///     容器绘制都能直接读取，语义不变）。
///   - Flex / Stack 的扩展数据由具体容器通过 `setupParentData` 分配。

#include "geometry.h"

namespace cui {

/// Base slot.  `RenderObject::parentData` is a ParentData*; containers
/// downcast to the concrete type they expect and assert the shape.
struct ParentData {
    virtual ~ParentData() = default;

    /// 子节点相对父节点的偏移（上游的 BoxParentData.offset）。
    /// 由父容器在布局时写入，绘制 / 命中测试 / localToGlobal 消费。
    Offset offset;
};

/// The offset of a box's top-left corner relative to its parent box, written
/// by the parent during layout and consumed by painting / hit-testing.  A
/// RenderBox only knows its `size` plus this local `offset`; the window-space
/// rect is derived by walking the ancestor chain (`localToGlobal`).
///
/// 历史兼容：offset 现由基类 ParentData 持有，本类型仅作为「带偏移的
/// 盒子子节点」的语义标记保留。
struct BoxParentData : ParentData {};

/// Flex 适配方式。上游的 `FlexFit`（rendering/flex.dart）。
enum class FlexFit {
    Loose,  ///< Flexible：不超过分配到的空间。
    Tight,  ///< Expanded：严格填满分配到的空间。
};

/// Flex children carry a main-axis flex factor (0 = natural size) and how
/// the allocated space is consumed (tightly = Expanded, loosely = Flexible).
/// 上游的 `FlexParentData`。
struct FlexParentData : BoxParentData {
    /// flex 权重。上游的 `FlexParentData.flex`。
    int flex = 0;
    /// 适配方式。上游的 `FlexParentData.fit`。
    FlexFit fit = FlexFit::Tight;
};

/// Stack children can be positioned; the offset doubles as the placement.
/// 上游的 `StackParentData`（rendering/stack.dart）——除 offset 外还
/// 携带 top/right/bottom/left/width/height，任意一项非空即视为 positioned
/// （render_stack.dart:246-251 的派生语义）。
struct StackParentData : BoxParentData {
    /// 该子节点是否参与定位（显式标记，与 has* 派生标志取或）。
    bool positioned = false;

    /// 距 Stack 顶边的内缩距离（上游的 `StackParentData.top`）。
    double top = 0.0;
    /// 距 Stack 右边界的内缩距离。
    double right = 0.0;
    /// 距 Stack 底边的内缩距离。
    double bottom = 0.0;
    /// 距 Stack 左边界的内缩距离。
    double left = 0.0;
    /// 子节点宽度（left 与 right 同时给出时被忽略）。
    double width = 0.0;
    /// 子节点高度（top 与 bottom 同时给出时被忽略）。
    double height = 0.0;

    /// 各定位值是否已设置。
    bool hasTop = false;
    bool hasRight = false;
    bool hasBottom = false;
    bool hasLeft = false;
    bool hasWidth = false;
    bool hasHeight = false;

    /// 上游的 `StackParentData.isPositioned`（render_stack.dart:246-251）。
    bool isPositioned() const {
        return positioned || hasTop || hasRight || hasBottom || hasLeft || hasWidth ||
               hasHeight;
    }
};

}  // namespace cui_core
