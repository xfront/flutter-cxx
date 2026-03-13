#pragma once
/// @file rendering/sliver.h
/// @brief Sliver 渲染层：sliver 协议（约束 / 几何 / RenderSliver 基类）与视口。
///
/// 对应 上游源码：
///   - rendering/sliver.dart（AxisDirection 辅助函数、SliverConstraints、
///     SliverGeometry、Sliver*ParentData、RenderSliver、
///     RenderSliverSingleBoxAdapter、RenderSliverToBoxAdapter）
///   - rendering/sliver_padding.dart（RenderSliverEdgeInsetsPadding /
///     RenderSliverPadding）
///   - rendering/sliver_multi_box_adaptor.dart（RenderSliverMultiBoxAdaptor）
///   - rendering/sliver_fixed_extent_list.dart（RenderSliverFixedExtentBoxAdaptor /
///     RenderSliverFixedExtentList）
///   - rendering/viewport.dart（RenderViewportBase.layoutChildSequence +
///     RenderViewport._attemptLayout）
///
/// ── 四处有意的偏离（都在实现文件里逐条落地，勿当 bug 排查）────────────────
///   1. 本仓 `RenderObject::layout` 的签名固定为 `const BoxConstraints&`
///      （Constraints 在 上游是多态基类，这里不是）。故 RenderSliver 不复用该
///      虚入口，另提供一个 layout(SliverConstraints, parentUsesSize) 重载，把
///      上游 layout() 的协议（存约束 → 清脏 → sizedByParent → performLayout）
///      原样复刻一份；performLayout 读 sliverConstraints_。
///      副作用：基类那个 BoxConstraints constraints_ 成员对 sliver 无意义，
///      且 RenderObject* 上调 layout(BoxConstraints) 仍会编译通过 —— 只应在
///      确定类型是 RenderBox 时那样调用。
///   2. 上游用 applyPaintTransform(child, Matrix4) 把绘制偏移传给命中测试与
///      localToGlobal；本仓的坐标换算统一读 parentData->offset。所以所有
///      sliver 容器在写 paintOffset / 计算绘制偏移时，同步把它写进
///      parentData->offset，于是基类的默认 hitTestChildren（倒序 + 减去子节点
///      offset）与 localToGlobal 直接可用，不必为 sliver 另写一套。
///   3. 懒加载：多盒子 sliver 已接 上游 的 `RenderSliverBoxChildManager`
///      （layout 期间 createChild / removeChild，窗口外的子节点被回收）。
///      两点简化：① manager 为空时退化成「子节点全部预置」的 eager 走法
///      （纯渲染层单测用，indexOf 回落槽位号）；② `_keepAliveBucket`
///      （keepAlive 子节点滚出窗口只隐藏不销毁）未移植，
///      `SliverMultiBoxAdaptorParentData::keepAlive` 恒 false。
///   4. 反向轴（Up / Left）只部分支持：本视口没有 center/anchor，正向序列的
///      绘制偏移反算用 geometry.paintExtent（被窗口裁剪后的长度），而 上游
///      靠 ViewportElementItemPaintOffset 的 start/end 双槽 + 子节点自身长度
///      来做轴对称。后果：axisDirection 为 Up/Left 时首段贴底正确，第二段起
///      与首段重叠。Down / Right（本仓所有既有用法）精确。

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

#include "cui/foundation/geometry.h"
#include "cui/foundation/parent_data.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_flex.h"  // Axis
#include "cui/rendering/render_object.h"

namespace cui {

// ── 方向枚举（上游 rendering/viewport_offset.dart + sliver.dart）──────────────

/// 主轴朝向。上游的 `AxisDirection`。
enum class AxisDirection { Down, Up, Right, Left };

/// sliver 的生长方向。上游的 `GrowthDirection`。
enum class GrowthDirection { Forward, Reverse };

/// 用户滚动方向（供 sliver 决定是否反向排布，如 SliverGrid 的 reverse）。
/// 上游的 `ScrollDirection`。
enum class ScrollDirection { Idle, Forward, Reverse };

inline Axis axisDirectionToAxis(AxisDirection d) {
    return (d == AxisDirection::Down || d == AxisDirection::Up) ? Axis::Vertical
                                                                : Axis::Horizontal;
}
inline bool axisIsHorizontal(AxisDirection d) {
    return axisDirectionToAxis(d) == Axis::Horizontal;
}
inline AxisDirection flipAxisDirection(AxisDirection d) {
    switch (d) {
        case AxisDirection::Down: return AxisDirection::Up;
        case AxisDirection::Up: return AxisDirection::Down;
        case AxisDirection::Left: return AxisDirection::Right;
        case AxisDirection::Right: return AxisDirection::Left;
    }
    return d;
}
/// 上游的 `axisDirectionIsReversed`：Up / Left 为反向。
inline bool axisDirectionIsReversed(AxisDirection d) {
    return d == AxisDirection::Up || d == AxisDirection::Left;
}
/// 上游的 `axisDirectionToGrowthDirection`。
inline GrowthDirection axisDirectionToGrowthDirection(AxisDirection d) {
    return axisDirectionIsReversed(d) ? GrowthDirection::Reverse : GrowthDirection::Forward;
}
/// 上游的 `applyGrowthDirectionToAxisDirection`：forward 原样、reverse 翻转。
inline AxisDirection applyGrowthDirectionToAxisDirection(AxisDirection d, GrowthDirection g) {
    return g == GrowthDirection::Forward ? d : flipAxisDirection(d);
}
/// 上游的 `applyGrowthDirectionToScrollDirection`。
inline ScrollDirection applyGrowthDirectionToScrollDirection(ScrollDirection s, GrowthDirection g) {
    if (g == GrowthDirection::Forward)
        return s;
    switch (s) {
        case ScrollDirection::Idle: return ScrollDirection::Idle;
        case ScrollDirection::Forward: return ScrollDirection::Reverse;
        case ScrollDirection::Reverse: return ScrollDirection::Forward;
    }
    return s;
}
/// 上游的 `defaultCrossAxisDirection`。
///
/// 偏离：上游 还要读 Directionality（垂直滚动时交叉轴随 ltr/rtl 变
/// left/right）；本仓无方向性上下文，恒按 ltr 取 Right。
inline AxisDirection defaultCrossAxisDirection(AxisDirection d) {
    switch (d) {
        case AxisDirection::Down:
        case AxisDirection::Up: return AxisDirection::Right;
        case AxisDirection::Right: return AxisDirection::Down;
        case AxisDirection::Left: return AxisDirection::Up;
    }
    return AxisDirection::Right;
}
/// 上游的 `getAxisDirectionFromAxisReverseAndDirectionality`（同上，无
/// Directionality 版：水平轴恒按 ltr 解释）。
inline AxisDirection axisDirectionFromAxis(Axis a, bool reverse) {
    if (a == Axis::Vertical)
        return reverse ? AxisDirection::Up : AxisDirection::Down;
    return reverse ? AxisDirection::Left : AxisDirection::Right;
}
/// 上游的 `clampDouble`（避免浮点尾差越界）。
inline double clampDouble(double value, double lo, double hi) {
    return BoxConstraints::clampTo(value, lo, hi);
}

// ── SliverConstraints ──────────────────────────────────────────────────────

/// sliver 的布局输入。上游的 `SliverConstraints`（rendering/sliver.dart:197）。
///
/// 与 BoxConstraints 的根本差别：它描述的是「视口把哪一段滚动区间交给你画」，
/// 而不是「你有多大」。字段齐全对应 上游 的 12 项。
struct SliverConstraints {
    AxisDirection axisDirection = AxisDirection::Down;
    GrowthDirection growthDirection = GrowthDirection::Forward;
    ScrollDirection userScrollDirection = ScrollDirection::Idle;
    /// 该 sliver 起始处已被滚掉的距离。
    double scrollOffset = 0.0;
    /// 排在本 sliver 之前的全部内容总长（用于 precedingScrollExtent 语义）。
    double precedingScrollExtent = 0.0;
    /// 前一个 sliver 已经画在本 sliver 之上的距离（重叠部分）。
    double overlap = 0.0;
    /// 本 sliver 之后还能画的距离。
    double remainingPaintExtent = 0.0;
    double crossAxisExtent = 0.0;
    AxisDirection crossAxisDirection = AxisDirection::Right;
    double viewportMainAxisExtent = 0.0;
    double remainingCacheExtent = 0.0;
    /// 缓存窗口相对滚动偏移的起点（<= 0）。
    double cacheOrigin = 0.0;

    bool operator==(const SliverConstraints&) const = default;
    bool operator!=(const SliverConstraints&) const = default;

    /// 上游的 `constraints.axis`。
    Axis axis() const { return axisDirectionToAxis(axisDirection); }
    /// 上游的 `normalizedGrowthDirection`。
    GrowthDirection normalizedGrowthDirection() const { return growthDirection; }
    /// 上游的 `isNormalized`：scrollOffset 非负、cacheOrigin 非正、overlap 非正。
    bool isNormalized() const {
        return scrollOffset >= 0.0 && overlap <= 0.0 && cacheOrigin <= 0.0 &&
               remainingPaintExtent >= 0.0 && precedingScrollExtent >= 0.0;
    }

    /// 主轴区间 [minExtent, maxExtent]、交叉轴锁到 crossAxisExtent（可选紧锁）。
    /// 上游的 `asBoxConstraints({minExtent, maxExtent, crossAxisExtent})`。
    BoxConstraints asBoxConstraints(double minExtent = 0.0,
                                    double maxExtent = BoxConstraints::kInfinity,
                                    std::optional<double> crossAxis = std::nullopt) const {
        const double cross = crossAxis.value_or(crossAxisExtent);
        if (axis() == Axis::Horizontal)
            return {minExtent, maxExtent, cross, cross};
        return {cross, cross, minExtent, maxExtent};
    }

};

// ── SliverGeometry ─────────────────────────────────────────────────────────

/// sliver 的布局输出。上游的 `SliverGeometry`（rendering/sliver.dart:641）。
///
/// 可空字段用 std::optional 表达 上游 的 `??` 默认值语义，读取一律走
/// resolved* 三个函数，别直接读 optional。
struct SliverGeometry {
    double scrollExtent = 0.0;
    double paintExtent = 0.0;
    double paintOrigin = 0.0;
    std::optional<double> layoutExtent;      ///< ??paintExtent
    double maxPaintExtent = 0.0;
    double maxScrollObstructionExtent = 0.0;
    std::optional<double> crossAxisExtent;   ///< ??constraints.crossAxisExtent
    std::optional<double> hitTestExtent;     ///< ??paintExtent
    std::optional<bool> visible;             ///< ??paintExtent>0
    bool hasVisualOverflow = false;
    /// 非 0 表示「本次布局作废，视口要用这个修正后的滚动偏移重来」。
    double scrollOffsetCorrection = 0.0;
    std::optional<double> cacheExtent;       ///< ??layoutExtent ?? paintExtent

    double resolvedLayoutExtent() const { return layoutExtent.value_or(paintExtent); }
    double resolvedHitTestExtent() const { return hitTestExtent.value_or(paintExtent); }
    bool resolvedVisible() const { return visible.value_or(paintExtent > 0.0); }
    double resolvedCacheExtent() const {
        return cacheExtent.value_or(layoutExtent.value_or(paintExtent));
    }
    /// 上游的 `SliverGeometry.zero`。
    static SliverGeometry zero() { return {}; }

    bool operator==(const SliverGeometry&) const = default;
};

// ── ParentData ─────────────────────────────────────────────────────────────

/// 用布局偏移（相对零滚动点）定位子节点的 sliver 用。
/// 上游的 `SliverLogicalParentData`。
struct SliverLogicalParentData : ParentData {
    /// 子节点靠近零滚动点那一侧，距零滚动点的距离。
    std::optional<double> layoutOffset;
};

/// 用绝对绘制偏移定位子节点的 sliver 用（视口、padding、toBoxAdapter）。
/// 上游的 `SliverPhysicalParentData`；见文件头偏离 2：paintOffset 与基类
/// offset 同步写。
struct SliverPhysicalParentData : SliverLogicalParentData {
    Offset paintOffset;
    void setPaintOffset(const Offset& o) {
        paintOffset = o;
        offset = o;  ///< 基类槽位：命中测试 / localToGlobal / 容器绘制读它
    }
};

/// 多盒子子节点 sliver 用：额外带一个「子序号」，供懒加载窗口裁剪定位。
/// 上游的 `SliverMultiBoxAdaptorParentData`。
struct SliverMultiBoxAdaptorParentData : SliverPhysicalParentData {
    /// 该子节点在 child manager 里的序号（eager 版 = Element 给的槽位号）。
    std::optional<int> index;
    /// 上游的 `keepAlive`：滚出窗口也不销毁（真懒加载批次使用）。
    bool keepAlive = false;
};

// ── RenderSliverBoxChildManager ────────────────────────────────────────────

/// 多盒子 sliver 的子节点托管人。上游的 `RenderSliverBoxChildManager`。
///
/// 上游由 `SliverMultiBoxAdaptorElement` 实现；本仓同样（widgets/sliver.h）。
/// 契约与 上游 一致：子节点只在 layout 期间被增删——`createChild` 里只准加
/// 对应 index 的那一个，`removeChild` 只准销掉本次布局尚未使用过的子节点。
class RenderSliverBoxChildManager {
public:
    virtual ~RenderSliverBoxChildManager() = default;

    /// 造出 index 号子节点并插到 after 之后（after 为空即插到队首）。
    /// index 越界时什么都不做。插入后本节点会经由 `didAdoptChild` 拿到序号。
    ///
    /// 偏离：上游 的 `after` 是 `{required RenderBox? after}` 具名参，本仓用位置参。
    virtual void createChild(int index, RenderBox* after) = 0;

    /// 摘除子节点（上游 同名；实现里同时把对应 Element 卸载）。
    virtual void removeChild(RenderBox* child) = 0;

    /// 估算全部子节点的总滚动长度（从 firstIndex 子节点的头到最后一个子节点的尾）。
    ///
    /// 这些方法都不设 const：本仓 Element 侧的实现要用非 const 的
    /// `BuildContext&`（= Element 自身）去调 delegate 的 builder。
    virtual double estimateMaxScrollOffset(const SliverConstraints& constraints, int firstIndex,
                                           int lastIndex, double leadingScrollOffset,
                                           double trailingScrollOffset) = 0;

    /// 精确子节点总数：比最后一个能造出子节点的 index 大 1。
    /// 上游 的 `childCount`——只在 createChild 无法为目标 index 造出节点时调用。
    virtual int childCount() = 0;

    /// 子节点入列时写入其 `SliverMultiBoxAdaptorParentData::index`。
    virtual void didAdoptChild(RenderBox* child) = 0;

    /// 布局期间告知托管人：剩余子节点不足以填满绘制窗口。
    virtual void setDidUnderflow(bool value) = 0;

    /// 上游 的 didStartLayout / didFinishLayout：布局首尾通知。
    virtual void didStartLayout() {}
    virtual void didFinishLayout() {}
};

// ── RenderSliver ───────────────────────────────────────────────────────────

/// sliver 渲染对象基类。上游的 `RenderSliver`（rendering/sliver.dart:1310）。
class RenderSliver : public RenderObject {
public:
    RenderSliver() = default;
    ~RenderSliver() override = default;

    /// 上游的 `RenderSliver.layout`（见文件头偏离 1）：存约束 → 清脏 →
    /// performResize（sizedByParent）→ performLayout。
    void layout(const SliverConstraints& constraints, bool parentUsesSize = true);

    const SliverConstraints& sliverConstraints() const { return sliverConstraints_; }
    /// 上游的 `geometry`；布局前是 SliverGeometry::zero。
    const SliverGeometry& geometry() const { return geometry_; }

    // 上游 的一组 geometry 便捷读取。
    bool visible() const { return geometry_.resolvedVisible(); }
    double paintExtent() const { return geometry_.paintExtent; }
    double paintOrigin() const { return geometry_.paintOrigin; }
    double layoutExtent() const { return geometry_.resolvedLayoutExtent(); }
    double scrollExtent() const { return geometry_.scrollExtent; }
    double hitTestExtent() const { return geometry_.resolvedHitTestExtent(); }
    double maxPaintExtent() const { return geometry_.maxPaintExtent; }
    double cacheExtent() const { return geometry_.resolvedCacheExtent(); }
    double crossAxisExtent() const {
        return geometry_.crossAxisExtent.value_or(sliverConstraints_.crossAxisExtent);
    }

    /// 上游的 `RenderSliver.paintBounds`：主轴取 paintExtent，交叉轴取约束值。
    Rect paintBounds() const override;

    /// 上游的 `RenderSliver.paint`：可见才画子节点，逐个用其绘制偏移
    /// （parentData->offset，见文件头偏离 2），窗口外的子节点由
    /// paintsChild 剔除。
    void paint(PaintingContext& context, const Offset& offset) override;

    // ── 子节点管理（sliver 的子既可能是 box 也可能是 sliver）──────────────
    RenderObject* firstChild() const { return children_.empty() ? nullptr : children_.front(); }
    RenderObject* lastChild() const { return children_.empty() ? nullptr : children_.back(); }
    int childCount() const { return static_cast<int>(children_.size()); }
    RenderObject* childAfter(RenderObject* child) const;
    RenderObject* childBefore(RenderObject* child) const;

    void insertChild(RenderObject* child, RenderObject* after) override;
    void removeChild(RenderObject* child) override;
    void moveChild(RenderObject* child, RenderObject* after) override;
    void reorderChildren(const std::vector<RenderObject*>& desired) override;
    void visitChildren(const std::function<void(AbstractNode*)>& visitor) override;

    /// 上游的 `RenderSliver.setupParentData`：物理定位型父节点给
    /// SliverPhysicalParentData（多盒子类子类改为 Adaptor 型）。
    void setupParentData(RenderObject* child) override;

    // ── 命中测试（见文件头偏离 2：position 为该 sliver 的绘制本地坐标）────
    bool hitTest(const Offset& position, std::vector<RenderObject*>& result) override;
    /// 上游的 `hitTestSelf(mainAxisPosition, crossAxisPosition)`。本仓沿用
    /// 「只有覆写 hitTestSelf 的节点可命中」的约定，入参就是绘制本地坐标。
    bool hitTestSelf(const Offset& position) const override { (void)position; return false; }

    // ── 子节点坐标（上游同名）────────────────────────────────────────────
    virtual double childMainAxisPosition(RenderObject* child) const {
        (void)child;
        return 0.0;
    }
    virtual double childCrossAxisPosition(RenderObject* child) const {
        (void)child;
        return 0.0;
    }
    /// 上游的 `childScrollOffset`：读 SliverLogicalParentData.layoutOffset。
    virtual double childScrollOffset(RenderObject* child) const;

    /// 上游的 `calculatePaintOffset`：区间 [from,to] 里落在可见绘制窗口的长度。
    double calculatePaintOffset(double from, double to) const {
        return calculatePaintOffset(sliverConstraints_, from, to);
    }
    double calculatePaintOffset(const SliverConstraints& c, double from, double to) const;
    /// 上游的 `calculateCacheOffset`：同上，窗口换成缓存区间。
    double calculateCacheOffset(double from, double to) const {
        return calculateCacheOffset(sliverConstraints_, from, to);
    }
    double calculateCacheOffset(const SliverConstraints& c, double from, double to) const;

    /// 上游的 `paintsChild`：该子节点本帧是否参与绘制（列表按窗口裁剪）。
    virtual bool paintsChild(RenderObject* child) const { (void)child; return true; }

protected:
    /// 上游的 `geometry` setter；只在 performLayout / performResize 里调用。
    void setGeometry(SliverGeometry geometry) { geometry_ = std::move(geometry); }

    /// 主轴方向上的「子节点本地坐标 → 主轴坐标」。
    double mainAxisOf(const Offset& position) const;
    double crossAxisOf(const Offset& position) const;

    SliverConstraints sliverConstraints_{};
    SliverGeometry geometry_{};
    std::vector<RenderObject*> children_;
};

// ── RenderSliverSingleBoxAdapter / RenderSliverToBoxAdapter ─────────────────

/// 包一个盒子子节点的 sliver 基类。上游的 `RenderSliverSingleBoxAdapter`。
class RenderSliverSingleBoxAdapter : public RenderSliver {
public:
    RenderBox* child() const { return child_; }
    void setChild(RenderBox* child);

    void insertChild(RenderObject* child, RenderObject* after) override;
    void removeChild(RenderObject* child) override;
    void visitChildren(const std::function<void(AbstractNode*)>& visitor) override;

    /// 上游的 `childMainAxisPosition`：单子节点型 sliver 恒为 -scrollOffset。
    double childMainAxisPosition(RenderObject* child) const override;

protected:
    /// 上游的 `setChildParentData`：按有效轴朝向把绘制偏移写进子节点。
    void setChildParentData(RenderObject* child, const SliverConstraints& c,
                            const SliverGeometry& geometry) const;
    RenderBox* child_ = nullptr;
};

/// 把一个普通控件塞进 sliver 序列。上游的 `RenderSliverToBoxAdapter`。
class RenderSliverToBoxAdapter : public RenderSliverSingleBoxAdapter {
public:
    void performLayout() override;
};

// ── RenderSliverPadding ────────────────────────────────────────────────────

/// 给子 sliver 加内边距。上游的 `RenderSliverEdgeInsetsPadding` +
/// `RenderSliverPadding`。
///
/// 偏离：上游的 padding 是 EdgeInsetsGeometry + TextDirection（方向性内缩
/// start/end）；本仓 EdgeInsets 只有 left/top/right/bottom，无方向性变体，
/// 故不做 textDirection 解析。
class RenderSliverPadding : public RenderSliver {
public:
    RenderSliverPadding() = default;
    explicit RenderSliverPadding(EdgeInsets padding, RenderSliver* child = nullptr)
        : padding_(padding) {
        if (child) setChild(child);
    }

    const EdgeInsets& padding() const { return padding_; }
    void setPadding(const EdgeInsets& p) {
        if (p == padding_)
            return;
        padding_ = p;
        markNeedsLayout();
    }

    RenderSliver* child() const {
        return static_cast<RenderSliver*>(firstChild());
    }
    void setChild(RenderSliver* child);

    void performLayout() override;

    double childMainAxisPosition(RenderObject* child) const override;
    double childCrossAxisPosition(RenderObject* child) const override;
    /// 上游的 `RenderSliverEdgeInsetsPadding.childScrollOffset`：子 sliver 的
    /// 滚动起点即 beforePadding（基类读 layoutOffset 的默认实现不适用）。
    double childScrollOffset(RenderObject* child) const override {
        (void)child;
        return beforePadding();
    }

private:
    double beforePadding() const;
    double afterPadding() const;
    double mainAxisPadding() const;
    double crossAxisPadding() const;
    void setChildPaintOffset();

    EdgeInsets padding_{};
};

// ── RenderSliverMultiBoxAdaptor 家族 ───────────────────────────────────────

/// 多个盒子子节点排成一线的 sliver 基类。
/// 上游的 `RenderSliverMultiBoxAdaptor`（懒加载版，见文件头偏离 3）。
class RenderSliverMultiBoxAdaptor : public RenderSliver {
public:
    RenderSliverMultiBoxAdaptor() = default;
    explicit RenderSliverMultiBoxAdaptor(RenderSliverBoxChildManager* childManager)
        : childManager_(childManager) {}

    /// 上游 的 `childManager`（构造函数注入，此处留出装配口是因为本仓
    /// `createRenderObject()` 拿不到 Element——由 Element 在 didMount 里回填）。
    RenderSliverBoxChildManager* childManager() const { return childManager_; }
    void setChildManager(RenderSliverBoxChildManager* m) { childManager_ = m; }

    void setupParentData(RenderObject* child) override;
    void insertChild(RenderObject* child, RenderObject* after) override;

    /// 上游的 `indexOf(child)`：读 parentData->index；无托管人时回落槽位号。
    int indexOf(RenderObject* child) const;

    /// 上游 可空版 `childScrollOffset` 的真实口径：布局偏移可能还没写入
    /// （子节点刚被挪位 / 刚造出来），懒加载算法要能把「没写过」和「0」分开。
    std::optional<double> childLayoutOffset(RenderObject* child) const;
    /// 上游的 `paintExtentOf(child)`：子节点在主轴上的尺寸。
    virtual double paintExtentOf(RenderObject* child) const;

    /// 上游的 `childMainAxisPosition` = childScrollOffset - scrollOffset。
    double childMainAxisPosition(RenderObject* child) const override;
    double childCrossAxisPosition(RenderObject* child) const override {
        (void)child;
        return 0.0;
    }

    /// 上游 `RenderSliverMultiBoxAdaptor.paint` 的可见区间判断（本仓把 上游
    /// paint 循环里的内联判断落成 paintsChild，见 RenderSliver::paint）。
    bool paintsChild(RenderObject* child) const override;

    /// 上游 `RenderSliverMultiBoxAdaptor.paint` 的偏移算法（本仓把结果同步进
    /// parentData->offset，见文件头偏离 2），布局期调用。
    void updateChildrenPaintOffsets();

    // ── 懒加载窗口 API（上游 同名 protected 方法）─────────────────────────

    /// 上游的 `addInitialChild`：造出 index 号子节点作为唯一的子并写布局偏移。
    /// 造不出来（列表空 / 已过末尾）返回 false 并置 didUnderflow。
    bool addInitialChild(int index = 0, double layoutOffset = 0.0);

    /// 上游的 `insertAndLayoutLeadingChild`：在 firstChild 之前补造并布局一个
    /// 子节点（index = 现有首子 - 1）；返回新首子，造不出来返回 nullptr。
    RenderBox* insertAndLayoutLeadingChild(const BoxConstraints& childConstraints,
                                           bool parentUsesSize = false);

    /// 上游的 `insertAndLayoutChild`：在 after 之后补造并布局一个子节点
    /// （index = after 的 + 1）；返回新子，造不出来返回 nullptr。
    RenderBox* insertAndLayoutChild(const BoxConstraints& childConstraints, RenderBox* after,
                                    bool parentUsesSize = false);

    /// 上游的 `calculateLeadingGarbage` / `calculateTrailingGarbage`。
    int calculateLeadingGarbage(int firstIndex) const;
    int calculateTrailingGarbage(int lastIndex) const;

    /// 上游的 `collectGarbage`：掐头去尾摘除窗口外子节点（经由托管人）。
    /// 每轮布局都要调，即使两个参数都是 0。
    void collectGarbage(int leadingGarbage, int trailingGarbage);

    /// 子节点总数：托管人口径（无托管人时即在树数量）。
    int totalChildren() const {
        return childManager_ ? childManager_->childCount() : childCount();
    }

protected:
    SliverMultiBoxAdaptorParentData* parentDataOf(RenderObject* child) const;

    RenderSliverBoxChildManager* childManager_ = nullptr;
};

/// 主轴尺寸固定的多盒子 sliver 基类。
/// 上游的 `RenderSliverFixedExtentBoxAdaptor`。
class RenderSliverFixedExtentBoxAdaptor : public RenderSliverMultiBoxAdaptor {
public:
    using RenderSliverMultiBoxAdaptor::RenderSliverMultiBoxAdaptor;

    double itemExtent() const { return itemExtent_; }
    void setItemExtent(double e) {
        if (e == itemExtent_)
            return;
        itemExtent_ = e;
        markNeedsLayout();
    }

    /// 上游的 `indexToLayoutOffset`。
    double indexToLayoutOffset(int index) const { return itemExtent_ * index; }
    /// 上游的 `getMinChildIndexForScrollOffset`。
    int getMinChildIndexForScrollOffset(double scrollOffset) const;
    /// 上游的 `getMaxChildIndexForScrollOffset`。
    int getMaxChildIndexForScrollOffset(double scrollOffset) const;

    void performLayout() override;

protected:
    /// 上游的 `_getChildConstraints`。
    BoxConstraints childConstraints(int index) const;
    double paintExtentOf(RenderObject* /*child*/) const override { return itemExtent_; }

    /// 上游的 `estimateMaxScrollOffset`（转交托管人）与 `computeMaxScrollOffset`
    /// （精确总长：子节点总数 x itemExtent）。
    double estimateMaxScrollOffset(const SliverConstraints& constraints, int firstIndex,
                                   int lastIndex, double leadingScrollOffset,
                                   double trailingScrollOffset) const;
    double computeMaxScrollOffset() const;

    double itemExtent_ = 0.0;
};

/// 固定行高的 sliver 列表。上游的 `RenderSliverFixedExtentList`。
class RenderSliverFixedExtentList : public RenderSliverFixedExtentBoxAdaptor {
public:
    RenderSliverFixedExtentList() = default;
    explicit RenderSliverFixedExtentList(RenderSliverBoxChildManager* childManager,
                                         double itemExtent)
        : RenderSliverFixedExtentBoxAdaptor(childManager) {
        itemExtent_ = itemExtent;
    }
    explicit RenderSliverFixedExtentList(double itemExtent) { itemExtent_ = itemExtent; }
};

/// 变高子节点的 sliver 列表。上游的 `RenderSliverList`。
///
/// 窗口算法（先向上补造、再向下补造、最后 collectGarbage）已按 上游 移植；
/// 子节点由 childManager 在布局期间造销（文件头偏离 3）。
class RenderSliverList : public RenderSliverMultiBoxAdaptor {
public:
    RenderSliverList() = default;
    explicit RenderSliverList(RenderSliverBoxChildManager* childManager)
        : RenderSliverMultiBoxAdaptor(childManager) {}

    void performLayout() override;
};

// ── RenderViewport ──────────────────────────────────────────────────────────

/// 把一串 sliver 装进盒子世界的视口。上游的 `RenderViewport`（rendering/viewport.dart）。
///
/// 命名对齐完成（2026-11-02）：盒子模式的单子节点视口已改名成
/// `RenderSingleChildViewport`（= 上游 `_RenderSingleChildViewport`），`RenderViewport`
/// 让给这里。仍缺 上游的 `center`/`anchor`/`cacheOrigin` 居中序列（见文件头偏离 4）。
class RenderViewport : public RenderBox {
public:
    explicit RenderViewport(AxisDirection axisDirection = AxisDirection::Down)
        : axisDirection_(axisDirection) {}

    /// 上游的 `sizedByParent`：视口尺寸完全由父约束决定。
    bool sizedByParent() const override { return true; }
    void performResize() override;
    void performLayout() override;

    AxisDirection axisDirection() const { return axisDirection_; }
    void setAxisDirection(AxisDirection d) {
        if (d == axisDirection_)
            return;
        axisDirection_ = d;
        markNeedsLayout();
    }
    AxisDirection crossAxisDirection() const { return crossAxisDirection_; }
    void setCrossAxisDirection(AxisDirection d) {
        if (d == crossAxisDirection_)
            return;
        crossAxisDirection_ = d;
        markNeedsLayout();
    }
    Axis axis() const { return axisDirectionToAxis(axisDirection_); }

    /// 滚动偏移（上游的 ViewportOffset.pixels）。clamp 到新算出的内容区间。
    double offset() const { return offset_; }
    void setOffset(double v) {
        if (v == offset_)
            return;
        offset_ = v;
        markNeedsLayout();  // 上游：offset 变化 → markNeedsLayout（不是只重绘）
    }
    ScrollDirection userScrollDirection() const { return userScrollDirection_; }
    void setUserScrollDirection(ScrollDirection s) { userScrollDirection_ = s; }

    double cacheExtent() const { return cacheExtent_; }
    void setCacheExtent(double e) {
        if (e == cacheExtent_)
            return;
        cacheExtent_ = e;
        markNeedsLayout();
    }

    /// 内容区间（上游的 `offset.constraints.min/max`，即 applyContentDimensions
    /// 之后的值：所有前向 sliver 的 scrollExtent 之和减去视口尺寸）。
    double minScrollExtent() const { return minScrollExtent_; }
    double maxScrollExtent() const { return maxScrollExtent_; }
    /// 内容总长（上游私有字段 `_minScrollExtent/_maxScrollExtent` 的累加值，
    /// 未减视口）。
    double contentScrollExtent() const { return contentMaxScrollExtent_; }
    bool hasVisualOverflow() const { return hasVisualOverflow_; }

    /// 布局完成后的内容尺寸回写（对齐 上游 的 applyContentDimensions）。
    using ExtentsCallback = std::function<void(double minExtent, double maxExtent,
                                               double viewportExtent)>;
    void setOnExtentsChanged(ExtentsCallback cb) { onExtentsChanged_ = std::move(cb); }
    /// 把 offset 夹进内容区间（回调方用；不额外标脏）。
    void clampOffsetIntoContent();

    // ── 子节点（sliver）──────────────────────────────────────────────────
    RenderObject* firstChild() const { return children_.empty() ? nullptr : children_.front(); }
    RenderObject* lastChild() const { return children_.empty() ? nullptr : children_.back(); }
    int childCount() const { return static_cast<int>(children_.size()); }
    RenderObject* childAfter(RenderObject* child) const;
    RenderObject* childBefore(RenderObject* child) const;

    void insertChild(RenderObject* child, RenderObject* after) override;
    void removeChild(RenderObject* child) override;
    void moveChild(RenderObject* child, RenderObject* after) override;
    void reorderChildren(const std::vector<RenderObject*>& desired) override;
    void visitChildren(const std::function<void(AbstractNode*)>& visitor) override;
    void setupParentData(RenderObject* child) override;

    bool hitTestSelf(const Offset& position) const override { (void)position; return false; }
    bool hitTestChildren(const Offset& position, std::vector<RenderObject*>& result) override;

protected:
    /// 逐个子 sliver 裁剪绘制（画布级 clip，见 sliver.cpp 的说明）。
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    /// 上游的 `RenderViewportBase.layoutChildSequence`（非居中分支）。
    /// `advance` = 上游的 childAfter / childBefore。
    using Advance = std::function<RenderObject*(RenderObject*)>;
    double layoutChildSequence(RenderObject* child, double scrollOffset, double overlap,
                               double layoutOffset, double remainingPaintExtent,
                               double mainAxisExtent, double crossAxisExtent,
                               GrowthDirection growthDirection, const Advance& advance,
                               double remainingCacheExtent, double cacheOrigin);
    /// 上游的 `RenderViewport._attemptLayout`（anchor = 0，无居中 sliver）。
    double attemptLayout(double mainAxisExtent, double crossAxisExtent, double correctedOffset);
    /// 上游的 `updateChildLayoutOffset` / `computeAbsolutePaintOffset`。
    void updateChildLayoutOffset(RenderObject* child, double layoutOffset,
                                 GrowthDirection growthDirection) const;
    /// 上游的 `updateOutOfBandData`。
    void updateOutOfBandData(GrowthDirection g, const SliverGeometry& geometry);
    double extentAlongAxis(const RenderObject* child) const;

    AxisDirection axisDirection_ = AxisDirection::Down;
    AxisDirection crossAxisDirection_ = AxisDirection::Right;
    ScrollDirection userScrollDirection_ = ScrollDirection::Idle;
    double offset_ = 0.0;
    double cacheExtent_ = 250.0;
    double minScrollExtent_ = 0.0;
    double maxScrollExtent_ = 0.0;
    /// 上游的 `_minScrollExtent/_maxScrollExtent`：各 sliver scrollExtent 的
    /// 裸累加（正/负向分开），尚未按 anchor 与视口尺寸换算成内容区间。
    double contentMinScrollExtent_ = 0.0;
    double contentMaxScrollExtent_ = 0.0;
    bool hasVisualOverflow_ = false;
    ExtentsCallback onExtentsChanged_;
    std::vector<RenderObject*> children_;
};

}  // namespace cui_core
