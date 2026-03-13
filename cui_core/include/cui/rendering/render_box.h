#pragma once
/// @file rendering/render_box.h
/// @brief RenderBox — the 2-D box-model RenderObject.
///
/// RenderBox adds the box layout protocol on top of RenderObject:
///   - `layout(constraints)` / `size` / `constraints` are inherited from
///     RenderObject (上游 把它们放在 RenderObject / RenderBox 两级)。
///   - `performLayout()` — subclass hook to compute size and position children.
///   - `performPaint(context, offset)` / `paint(context, offset)` — draw in
///     local coordinates.
///   - `hitTest(position)` / `hitTest(position, result)` — hit-test in local
///     coordinates.
///   - `getMin/MaxIntrinsicWidth/Height(extent)` — intrinsic 尺寸协议（上游
///     rendering/box.dart 的 `_IntrinsicDimension` 记忆化 + 子类钩子
///     `compute*Intrinsic*`）；被父节点查询过 intrinsic 的盒，标脏时会把
///     脏标越过 relayout boundary 交给父节点。
///   - `localToGlobal(offset)` — inherited; walk the parent chain to derive
///     window space.
///
/// Helper bases:
///   - `RenderProxyBox` — single-child slot (上游的
///     RenderObjectWithChildMixin).
///   - `RenderBoxContainer` — ordered child list (上游的
///     ContainerRenderObjectMixin).
///   - `RenderShiftedBox` — single-child box that offsets the child by
///     BoxParentData.offset (the workhorse for the classic widget pipeline).
///   - `RenderBoxContainerDefaults` — default container behavior helpers.

#include <unordered_map>
#include <vector>

#include "cui/foundation/layer.h"
#include "cui/rendering/render_object.h"

namespace cui {

class PaintingContext;

/// 基线类型。上游的 `TextBaseline`（dart:ui）。
enum class BaselineType {
    Alphabetic,   ///< 字母基线（拉丁文字）。
    Ideographic,  ///< 表意文字基线（CJK）。
};

/// 命中测试行为。上游的 `HitTestBehavior`（rendering/hit_test.dart）。
///
/// 本仓的命中链是「边遍历边 push」的 `std::vector<RenderObject*>`，所以三种取值
/// 落在 `hitTestSelf` 一个点上（= 上游 `RenderBoxWithHitTestBehavior.hitTestSelf`
/// 的定义）：deferToChild ⇒ 只有子节点命中时自身才入链；opaque / translucent ⇒
/// 自身总是入链。二者的差别在上层的 `hitTest` **返回值**（要不要继续问身后的
/// 兄弟），由各自的渲染对象覆写决定，见 `RenderMouseRegion::hitTest` 的 opaque。
enum class HitTestBehavior {
    DeferToChild,  ///< 上游的 `deferToChild`（默认）。
    Opaque,        ///< 上游的 `opaque`。
    Translucent,   ///< 上游的 `translucent`。
};

/// intrinsic / 布局里「这一轴无约束」的判据。上游写 `double.infinity` 与
/// `extent.isFinite`，本仓的无界哨兵是 `BoxConstraints::kInfinity`，故
/// 上游的 `x.isFinite` 一律对应本函数。
inline bool isBoundedExtent(double extent) {
    return extent < BoxConstraints::kInfinity;
}

// ── RenderBox ───────────────────────────────────────────────────────────────

/// A RenderObject that uses BoxConstraints and produces a Size.
class RenderBox : public RenderObject {
public:
    RenderBox() = default;
    ~RenderBox() override = default;

    // ── RenderObject overrides ────────────────────────────────────────────
    Rect paintBounds() const override {
        return {0, 0, size_.width, size_.height};
    }

    /// 上游的 `RenderBox.getDistanceToBaseline`。
    ///
    /// 返回本盒**顶边**到指定基线的距离；< 0 表示没有基线信息（上游
    /// 返回 null）。需要参与基线对齐的渲染对象（如 RenderParagraph）
    /// 覆写本方法。
    virtual double getDistanceToBaseline(BaselineType type) const {
        (void)type;
        return -1.0;
    }

    /// Hit-test: does `position` (in this box's local coordinates) hit?
    /// If so, returns true (the HitTestResult path records the entry via
    /// hitTestSelf / hitTestChildren hooks).
    bool hitTest(const Offset& position) const;

    /// 上游 协议的命中测试入口（收集命中节点到 result）。
    bool hitTest(const Offset& position, std::vector<RenderObject*>& result) override;

    // ── Intrinsics（上游的 RenderBox intrinsic 协议）─────────────────────
    //
    ///@{ 上游的 `getMinIntrinsicWidth` 等四个入口。**不要覆写它们**，覆写
    /// 下方对应的 `compute*Intrinsic*`。入口负责校验入参（不得为负）并按
    /// 「维度 + 入参」记忆化结果；入参可以是 kInfinity（= 上游的
    /// double.infinity，表示该轴无约束）。
    ///
    /// 上游的约定：这四个入口**只应被子节点调用**（父节点的 performLayout
    /// 查询子节点 intrinsic 的那一刻，父子之间就建立了「布局依赖」），
    /// 因此调用会耦合重排——见 `markNeedsLayout` 覆写。
    /// 计算代价高（最坏 O(N²)），只在必须时使用。
    double getMinIntrinsicWidth(double height);
    double getMaxIntrinsicWidth(double height);
    double getMinIntrinsicHeight(double width);
    double getMaxIntrinsicHeight(double width);
    ///@}

    /// 覆写上游的 `RenderBox.markNeedsLayout`：本节点一旦被父节点查过
    /// intrinsic（缓存非空即说明这一点），即便父节点在 relayout boundary
    /// 之外也必须重排，所以把脏标直接交给父节点。
    void markNeedsLayout() override;

protected:
    /// Default box layout: take the biggest size the constraints allow
    /// (上游的 `RenderBox.performLayout` 默认).  Containers override.
    void performLayout() override;

    ///@{ 上游的 `compute*Intrinsic*` 钩子。基类一律返回 0.0（上游同值），
    /// 即「本盒在这一维度上不占空间」；需要参与 intrinsic 计算的盒覆写。
    ///
    /// 语义与上游一致：
    ///  - `compute*IntrinsicWidth(height)` 忽略 height ⇒ 宽度不依赖高度
    ///    （width-in-height-out）；若依赖，则按给定的 height 计算，height
    ///    可能是 kInfinity。
    ///  - 依赖子节点时要用**子节点的 `get*Intrinsic*`**（带缓存与耦合），
    ///    不是 `compute*`。
    ///  - 不得返回负值或 kInfinity。
    virtual double computeMinIntrinsicWidth(double height) {
        (void)height;
        return 0.0;
    }
    virtual double computeMaxIntrinsicWidth(double height) {
        (void)height;
        return 0.0;
    }
    virtual double computeMinIntrinsicHeight(double width) {
        (void)width;
        return 0.0;
    }
    virtual double computeMaxIntrinsicHeight(double width) {
        (void)width;
        return 0.0;
    }
    ///@}

private:
    /// 上游的 `_IntrinsicDimension`：四个入口共用的缓存键。
    enum class IntrinsicDimension { MinWidth, MaxWidth, MinHeight, MaxHeight };

    /// 上游的 `_computeIntrinsics`：校验 + 记忆化 + 分派到 compute 钩子。
    double computeIntrinsics(IntrinsicDimension dim, double input);

    /// 上游的 `_LayoutCacheStorage`（本仓只缓存 intrinsic；dry layout /
    /// dry baseline 尚未移植，故 clear() 只看这一张表）。
    struct IntrinsicCache {
        std::unordered_map<double, double> byDimension[4];

        /// 清空并回答「此前是否有内容」——调用方据此决定是否把脏标上抛给父节点。
        bool clear() {
            bool had = false;
            for (auto& m : byDimension) {
                if (!m.empty())
                    had = true;
                m.clear();
            }
            return had;
        }
    };

    IntrinsicCache intrinsicCache_;
};

// ── RenderProxyBox ──────────────────────────────────────────────────────────

/// 单子节点渲染对象的基类。上游的 `RenderObjectWithChildMixin`。
///
/// 子节点被放在唯一的 child 槽里；insertChild 替换旧子节点。
///
/// 默认绘制行为与 上游 一致：代理盒自身不画任何东西，只把子节点
/// 画到 parentData.offset 处（`RenderPadding` / `RenderAlign` 等靠这条
/// 默认路径透传绘制；需要额外绘制的子类——如 `RenderDecoratedBox`——
/// 覆写 performPaint 后自行调用 `context.paintChild`）。
class RenderProxyBox : public RenderBox {
public:
    /// 当前子节点。
    RenderObject* child() const { return child_; }

    void insertChild(RenderObject* child, RenderObject* after) override;
    void removeChild(RenderObject* child) override;
    void moveChild(RenderObject* child, RenderObject* after) override;

    void visitChildren(const std::function<void(AbstractNode*)>& visitor) override;

protected:
    /// 默认：把子节点绘制到 offset + parentData.offset。
    void performPaint(PaintingContext& context, const Offset& offset) override;

    /// 代理盒默认布局（上游的 RenderProxyBox.mixin）：子节点以相同约束
    /// 布局，自身尺寸取子节点尺寸；无子节点时取 smallest()。
    void performLayout() override;

    ///@{ 上游的 `RenderProxyBoxMixin.compute*Intrinsic*`：整条协议原样转发给
    /// 子节点，无子节点时为 0.0。
    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    double computeMinIntrinsicHeight(double width) override;
    double computeMaxIntrinsicHeight(double width) override;
    ///@}

    /// 子节点（上游的 `RenderObjectWithChildMixin<RenderBox>.child`）。
    RenderBox* childBox() const { return child_ ? static_cast<RenderBox*>(child_) : nullptr; }

    RenderObject* child_ = nullptr;
};

// ── RenderBoxContainer ──────────────────────────────────────────────────────

/// 多子节点渲染对象的基类。上游的 `ContainerRenderObjectMixin`。
///
/// 子节点以有序列表保存；`insertChild` 支持插到指定兄弟之后，
/// 从而在 Element 侧重排（Key 变化导致的位置移动）时保持顺序。
class RenderBoxContainer : public RenderBox {
public:
    /// 当前子节点列表（按绘制顺序）。
    const std::vector<RenderObject*>& children() const { return children_; }

    void insertChild(RenderObject* child, RenderObject* after) override;
    void removeChild(RenderObject* child) override;
    void moveChild(RenderObject* child, RenderObject* after) override;

    void visitChildren(const std::function<void(AbstractNode*)>& visitor) override;

    /// 按期望顺序重排子节点（上游的 slot 顺序语义）。
    void reorderChildren(const std::vector<RenderObject*>& desired) override;

protected:
    /// 默认：按列表顺序把每个子节点绘制到 offset + parentData.offset
    ///（上游的 ContainerRenderObjectMixin.defaultPaint）。
    void performPaint(PaintingContext& context, const Offset& offset) override;

    std::vector<RenderObject*> children_;
};

// ── RenderShiftedBox ────────────────────────────────────────────────────────

/// A RenderBox with exactly one child, positioned at BoxParentData.offset.
/// The child is laid out with constraints derived from the parent's; the
/// parent's size is either the child's size (plus offset) or independently
/// determined.
class RenderShiftedBox : public RenderBox {
public:
    RenderBox* child() const { return child_; }
    void setChild(RenderBox* child);

    void visitChildren(const std::function<void(AbstractNode*)>& visitor) override;

    // ── AbstractNode child management ─────────────────────────────────────
    void adoptChild(AbstractNode* child) override;
    void dropChild(AbstractNode* child) override;

protected:
    /// Layout the single child with the given constraints, writing the
    /// child's offset into its BoxParentData.
    void layoutChild(RenderBox* child, const BoxConstraints& constraints,
                     const Offset& childOffset);

    ///@{ 上游的 `RenderShiftedBox.compute*Intrinsic*`：原样转发给子节点。
    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    double computeMinIntrinsicHeight(double width) override;
    double computeMaxIntrinsicHeight(double width) override;
    ///@}

    RenderBox* child_ = nullptr;
};

// ── RenderBoxContainerDefaults ──────────────────────────────────────────────

/// Mixin-style helpers for containers that manage a list of BoxParentData-
/// carrying children.  Not a RenderObject itself — provides static helpers
/// that concrete container render objects (RenderFlex, RenderStack, ...) call.
struct RenderBoxContainerDefaults {
    /// Default setupParentData: allocate a BoxParentData for the child.
    static void defaultSetupParentData(RenderObject* child) {
        if (!child->parentData())
            child->setParentData(new BoxParentData());
    }

    /// Walk the child list and return the child at `index`, or null.
    static RenderBox* childAt(RenderBox* firstChild, int index) {
        // This is a placeholder — real containers use their own child list.
        (void)firstChild;
        (void)index;
        return nullptr;
    }
};

}  // namespace cui_core
