#pragma once
/// @file rendering/render_theater.h
/// @brief Overlay 的渲染对象。上游的 `_RenderTheater`。
///
/// 对应 上游源码：
///   上游 widgets/overlay.dart（_RenderTheater 段落，
///   约 1195-1600 行）
///
/// 上游的 `_RenderTheater` 是一个特殊的堆叠渲染对象：
///   - **自身尺寸**由父约束决定（`constraints.biggest`），而不是由子节点决定。
///   - **onstage 子节点**（列表中 `skipCount` 之后的部分）默认被拉伸到整个
///     theater 尺寸（`BoxConstraints.tight(size)`），并按列表顺序绘制；
///     其中用 `Positioned` 标记过的条目按 Stack 定位算术摆放（上游
///     `_RenderTheater` 复用 `StackParentData`）。
///   - **offstage 子节点**（列表前 `skipCount` 个，位于底部、被上方
///     opaque 条目遮住的部分）既不参与布局，也不绘制，也不参与命中测试，
///     但它们仍在 Element 树中（因此其 State 得以保留）。
///
/// 本移植的 `RenderTheater` 沿用同样的语义。

#include <cstddef>
#include <vector>

#include "cui/rendering/render_object.h"
#include "cui/rendering/render_box.h"

namespace cui {

/// Overlay 的渲染对象。上游的 `_RenderTheater`。
class RenderTheater : public RenderBoxContainer {
public:
    explicit RenderTheater(size_t skipCount = 0) : skipCount_(skipCount) {}

    /// 位于列表前端的 offstage 子节点个数。上游的 `_Theater.skipCount`。
    size_t skipCount() const { return skipCount_; }
    void setSkipCount(size_t count) {
        if (skipCount_ == count)
            return;
        skipCount_ = count;
        markNeedsLayout();
        markNeedsPaint();
    }

    /// onstage 子节点（`children[skipCount..]`）。
    const std::vector<RenderObject*>& onstageChildren() const {
        return onstage_;
    }

    void insertChild(RenderObject* child, RenderObject* after) override;
    void removeChild(RenderObject* child) override;
    void moveChild(RenderObject* child, RenderObject* after) override;
    void reorderChildren(const std::vector<RenderObject*>& desired) override;

    /// 上游 `_RenderTheater` 的父类是 `RenderBox + ContainerRenderObjectMixin<
    /// RenderBox, StackParentData>`：Overlay 条目可以用 `Positioned` 定位
    /// （Hero 飞行用的 shuttle 条目就是 `Positioned`），非定位条目仍撑满。
    void setupParentData(RenderObject* child) override;

    void paint(PaintingContext& context, const Offset& offset) override;

protected:
    void performLayout() override;
    bool hitTestChildren(const Offset& position, std::vector<RenderObject*>& result) override;

private:
    /// 重新计算 onstage 视图（跳过前 skipCount_ 个子节点）。
    void refreshOnstage();

    size_t skipCount_ = 0;
    std::vector<RenderObject*> onstage_;
};

}  // namespace cui_core
