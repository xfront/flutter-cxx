#pragma once
/// @file rendering/render_box_widgets.h
/// @brief 上游的常见 RenderBox 子类移植。
///
/// 对应 上游源码：
///   上游 rendering/shifted_box.dart
///   上游 rendering/proxy_box.dart
///   上游 rendering/stack.dart
///
/// 包含：
///   RenderPadding        — 内边距（上游的 RenderPadding）
///   RenderConstrainedBox — 附加约束（RenderConstrainedBox）
///   RenderSizedBox       — 固定尺寸（RenderConstrainedBox 的紧约束特化）
///   RenderAlign          — 对齐（RenderPositionedBox 的 alignment 分支）
///   RenderStack          — 堆叠
///   RenderDecoratedBox   — 背景装饰（纯色）
///
/// 结构对齐上游：
///   - 单子节点的盒子继承 RenderProxyBox（上游的
///     RenderObjectWithChildMixin）。
///   - 多子节点的容器继承 RenderBoxContainer（上游的
///     ContainerRenderObjectMixin）。

#include <functional>
#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../utils/shared_ptr.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/text_style.h"
#include "cui/rendering/painting_context.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_object.h"

namespace cui {

/// 内边距盒。上游的 `RenderPadding`。
class RenderPadding : public RenderProxyBox {
public:
    explicit RenderPadding(EdgeInsets padding = {}) : padding_(padding) {}

    const EdgeInsets& padding() const { return padding_; }
    void setPadding(const EdgeInsets& p) { if (padding_ == p) return; padding_ = p; markNeedsLayout(); }

protected:
    void performLayout() override;

    ///@{ 上游的 `RenderPadding.compute*Intrinsic*`：子节点的 intrinsic 加上
    /// 另一轴的 padding（入参先扣掉自身占用；kInfinity 参与减法仍是 kInfinity，
    /// 与上游「依赖 double.infinity 吸收律」同理）。无子节点时取 padding 本身。
    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    double computeMinIntrinsicHeight(double width) override;
    double computeMaxIntrinsicHeight(double width) override;
    ///@}

private:
    EdgeInsets padding_;
};

/// 附加约束盒。上游的 `RenderConstrainedBox`。
class RenderConstrainedBox : public RenderProxyBox {
public:
    explicit RenderConstrainedBox(BoxConstraints constraints = BoxConstraints::unbounded())
        : additional_(constraints) {}

    const BoxConstraints& additionalConstraints() const { return additional_; }
    void setAdditionalConstraints(const BoxConstraints& c) { if (additional_ == c) return; additional_ = c; markNeedsLayout(); }

protected:
    void performLayout() override;

    ///@{ 上游的 `RenderConstrainedBox.compute*Intrinsic*`：附加约束在该轴上
    /// 已是**有界紧约束**时直接取该值（父约束再紧也不过是它）；否则把子节点
    /// 的 intrinsic 过一遍附加约束（该轴无界时原样透传）。
    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    double computeMinIntrinsicHeight(double width) override;
    double computeMaxIntrinsicHeight(double width) override;
    ///@}

private:
    BoxConstraints additional_;
};

/// 让子节点按自身**最大 intrinsic 高度**取高的盒。上游的
/// `RenderIntrinsicHeight`（proxy_box.dart）。
///
/// 上游语义：约束已给紧高度时原样下发；否则把高度收紧为
/// `child.getMaxIntrinsicHeight(constraints.maxWidth)`。交叉轴的 intrinsic
/// 直接等于最大 intrinsic（上游 `computeMinIntrinsicHeight => getMax...`），
/// 主轴（宽）的 intrinsic 则在「无界高度先换算成子节点最大高」之后再问子节点。
///
/// 代价较高：会为求 intrinsic 多做一轮布局推算，最坏让整棵树布局变成
/// O(N²)。能不写就不写。
class RenderIntrinsicHeight : public RenderProxyBox {
protected:
    void performLayout() override;

    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    /// 上游只覆写 min（`=> getMaxIntrinsicHeight(width)`）；max 沿用
    /// `RenderProxyBox` 的原样转发。
    double computeMinIntrinsicHeight(double width) override;

private:
    /// 上游的 `_childConstraints`。
    BoxConstraints childConstraints(RenderBox* child, const BoxConstraints& constraints) const;
};

/// 让子节点按自身最大 intrinsic 宽度取宽的盒。上游的 `RenderIntrinsicWidth`。
///
/// 与 RenderIntrinsicHeight 的差别只在多了 stepWidth / stepHeight：给出步长时
/// 把子节点在该轴上的尺寸向上取整到步长的整数倍。
class RenderIntrinsicWidth : public RenderProxyBox {
public:
    explicit RenderIntrinsicWidth(std::optional<double> stepWidth = std::nullopt,
                                 std::optional<double> stepHeight = std::nullopt)
        : stepWidth_(stepWidth), stepHeight_(stepHeight) {}

    const std::optional<double>& stepWidth() const { return stepWidth_; }
    void setStepWidth(std::optional<double> v) {
        if (stepWidth_ == v)
            return;
        stepWidth_ = v;
        markNeedsLayout();
    }

    const std::optional<double>& stepHeight() const { return stepHeight_; }
    void setStepHeight(std::optional<double> v) {
        if (stepHeight_ == v)
            return;
        stepHeight_ = v;
        markNeedsLayout();
    }

protected:
    void performLayout() override;

    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    double computeMinIntrinsicHeight(double width) override;
    double computeMaxIntrinsicHeight(double width) override;

private:
    /// 上游的 `_applyStep`：把 input 向上取整到 step 的整数倍。
    static double applyStep(double input, const std::optional<double>& step);

    /// 上游的 `_childConstraints`。
    BoxConstraints childConstraints(RenderBox* child, const BoxConstraints& constraints) const;

    std::optional<double> stepWidth_;
    std::optional<double> stepHeight_;
};

/// 对齐盒。上游的 `RenderPositionedBox`。
class RenderAlign : public RenderProxyBox {
public:
    /// alignmentX/alignmentY ∈ [-1, 1]，0 表示居中。
    /// widthFactor/heightFactor 非空时，自身在该轴上取「子尺寸 × 因子」，
    /// 与上游一致（`RenderPositionedBox` 的 shrinkWrap 分支）。
    explicit RenderAlign(double alignmentX = 0.0, double alignmentY = 0.0,
                         std::optional<double> widthFactor = std::nullopt,
                         std::optional<double> heightFactor = std::nullopt)
        : alignmentX_(alignmentX), alignmentY_(alignmentY),
          widthFactor_(widthFactor), heightFactor_(heightFactor) {}

    void setAlignment(double x, double y) {
        if (alignmentX_ == x && alignmentY_ == y)
            return;
        alignmentX_ = x;
        alignmentY_ = y;
        markNeedsLayout();
    }

    const std::optional<double>& widthFactor() const { return widthFactor_; }
    void setWidthFactor(std::optional<double> f) {
        if (widthFactor_ == f)
            return;
        widthFactor_ = f;
        markNeedsLayout();
    }

    const std::optional<double>& heightFactor() const { return heightFactor_; }
    void setHeightFactor(std::optional<double> f) {
        if (heightFactor_ == f)
            return;
        heightFactor_ = f;
        markNeedsLayout();
    }

protected:
    void performLayout() override;

    ///@{ 上游的 `RenderPositionedBox.compute*Intrinsic*`：代理转发（子节点的
    /// intrinsic）再乘上本轴的 widthFactor / heightFactor（未给因子即 ×1）。
    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    double computeMinIntrinsicHeight(double width) override;
    double computeMaxIntrinsicHeight(double width) override;
    ///@}

private:
    double alignmentX_;
    double alignmentY_;
    std::optional<double> widthFactor_;
    std::optional<double> heightFactor_;
};

/// 堆叠盒。上游的 `RenderStack`。
class RenderStack : public RenderBoxContainer {
public:
    enum class Alignment {
        TopStart, TopCenter, TopEnd,
        MiddleStart, MiddleCenter, MiddleEnd,
        BottomStart, BottomCenter, BottomEnd,
    };

    /// 上游的 `StackFit`（render_stack.dart:301-320）。
    enum class Fit {
        /// 子节点拿到松约束（自身自然尺寸）。
        Loose,
        /// 子节点拿到紧约束（撑满 Stack）。
        Expand,
        /// 子节点原样收到父约束。
        Passthrough,
    };

    explicit RenderStack(Alignment alignment = Alignment::TopStart, Fit fit = Fit::Loose)
        : alignment_(alignment), fit_(fit) {}

    Alignment alignment() const { return alignment_; }
    void setAlignment(Alignment a) { if (alignment_ == a) return; alignment_ = a; markNeedsLayout(); }

    Fit fit() const { return fit_; }
    void setFit(Fit f) { if (fit_ == f) return; fit_ = f; markNeedsLayout(); }

    /// 标记某个子节点是否参与定位（上游的 PositionedParentData）。
    void setPositioned(size_t index, bool positioned);

    /// 创建 StackParentData（上游的 setupParentData）。
    void setupParentData(RenderObject* child) override;

    /// 上游的 `layoutPositionedChild`（render_stack.dart:539-576）：
    /// 按 top/right/bottom/left/width/height 布局并摆放 positioned 子节点。
    /// 公开成 static：`RenderTheater`（上游 `_RenderTheater` 是 RenderStack
    /// 派生）也要用同一套定位算术。
    static bool layoutPositionedChild(RenderObject* child, StackParentData* pd,
                                      const Size& stackSize, Alignment alignment);

protected:
    void performLayout() override;

    ///@{ 上游的 `RenderStack.compute*Intrinsic*`：**只算非 positioned 子节点**
    /// 的 intrinsic，取最大值（positioned 子的尺寸由 Stack 自身尺寸反推，
    /// 无法参与 intrinsic）。
    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    double computeMinIntrinsicHeight(double width) override;
    double computeMaxIntrinsicHeight(double width) override;
    ///@}

private:
    /// 上游的 `RenderStack.getIntrinsicDimension`：对非 positioned 子节点
    /// 逐个取 `getter` 的最大值。
    static double intrinsicDimension(const std::vector<RenderObject*>& children,
                                    const std::function<double(RenderBox*)>& getter);

    Alignment alignment_;
    Fit fit_;
};

/// 装饰盒。上游的 `RenderDecoratedBox`（proxy_box.dart 的简化版）。
///
/// 支持纯色背景 + 圆角 + 边框（`BoxDecoration` 的最小子集），先画装饰再画
/// 子节点（背景语义）。无子节点时退化为叶子盒：尺寸取约束的 smallest()
/// （与 上游的 RenderBox.performResize 一致，避免在 flex 交叉轴上撑满）。
class RenderDecoratedBox : public RenderProxyBox {
public:
    explicit RenderDecoratedBox(Color color, double borderRadius = 0.0,
                                double borderWidth = 0.0, Color borderColor = Color{},
                                double elevation = 0.0,
                                Color shadowColor = Color::fromRGBA(0, 0, 0, 0.35),
                                double elevation2 = 0.0,
                                Color shadowColor2 = Color{0, 0, 0, 0})
        : color_(color), radius_(borderRadius), borderWidth_(borderWidth),
          borderColor_(borderColor), elevation_(elevation), shadowColor_(shadowColor),
          elevation2_(elevation2), shadowColor2_(shadowColor2) {}

    const Color& color() const { return color_; }
    void setColor(const Color& c) {
        if (color_ == c)
            return;
        color_ = c;
        markNeedsPaint();
    }

    double borderRadius() const { return radius_; }
    void setBorderRadius(double r) {
        if (radius_ == r)
            return;
        radius_ = r;
        markNeedsPaint();
    }

    double borderWidth() const { return borderWidth_; }
    const Color& borderColor() const { return borderColor_; }
    /// 设置边框（宽度 ≤ 0 表示不画边框）。
    void setBorder(Color c, double width) {
        if (borderColor_ == c && borderWidth_ == width)
            return;
        borderColor_ = c;
        borderWidth_ = width;
        markNeedsPaint();
    }

    double elevation() const { return elevation_; }
    void setElevation(double e) {
        if (e != elevation_) {
            elevation_ = e;
            markNeedsPaint();
        }
    }
    Color shadowColor() const { return shadowColor_; }
    void setShadowColor(Color c) {
        if (c != shadowColor_) {
            shadowColor_ = c;
            markNeedsPaint();
        }
    }

    double elevation2() const { return elevation2_; }
    void setElevation2(double e) {
        if (e != elevation2_) {
            elevation2_ = e;
            markNeedsPaint();
        }
    }
    Color shadowColor2() const { return shadowColor2_; }
    void setShadowColor2(Color c) {
        if (c != shadowColor2_) {
            shadowColor2_ = c;
            markNeedsPaint();
        }
    }

    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 && position.dx < size_.width &&
               position.dy < size_.height;
    }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    Color color_;
    double radius_ = 0.0;
    double borderWidth_ = 0.0;
    Color borderColor_;
    double elevation_ = 0.0;
    Color shadowColor_;
    double elevation2_ = 0.0;
    Color shadowColor2_;
};

// ── RenderOpacity ─────────────────────────────────────────────────────────
/// 不透明度代理盒。上游的 `RenderOpacity`（proxy_box.dart）。
///
/// 自身不改变布局：把子节点的尺寸当作自己的尺寸，绘制时通过
/// `PaintingContext::pushOpacity` 把整棵子树包进一个 `OpacityLayer`，
/// 由合成阶段乘以 alpha。
class RenderOpacity : public RenderProxyBox {
public:
    explicit RenderOpacity(double opacity = 1.0) : opacity_(std::clamp(opacity, 0.0, 1.0)) {}

    double opacity() const { return opacity_; }
    void setOpacity(double o) {
        const double clamped = std::clamp(o, 0.0, 1.0);
        if (opacity_ == clamped)
            return;
        opacity_ = clamped;
        markNeedsPaint();
    }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    double opacity_ = 1.0;
};

// ── RenderClipRRect ────────────────────────────────────────────────────────
/// 圆角裁剪代理盒。上游的 `RenderClipRRect`（clip.dart）。
///
/// 自身不改变布局：裁剪区域即自身盒子（0..size），绘制时对画布做
/// round-rect clip 再绘制子节点。
class RenderClipRRect : public RenderProxyBox {
public:
    explicit RenderClipRRect(double borderRadius = 0.0) : radius_(borderRadius) {}

    double borderRadius() const { return radius_; }
    void setBorderRadius(double r) {
        if (radius_ == r)
            return;
        radius_ = r;
        markNeedsPaint();
    }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    double radius_ = 0.0;
};

// ── RenderAspectRatio ──────────────────────────────────────────────────────
/// 固定宽高比盒。上游的 `RenderAspectRatio`（layout_helper / shifted_box）。
///
/// 布局算法对齐上游 `RenderAspectRatio.performLayout`：
///   - 约束紧（tight）→ 直接取 smallest；
///   - 先按有界轴反推另一轴，再双向 clamp 到约束内；
///   - 子节点拿到紧约束（BoxConstraints::tight(size)）。
class RenderAspectRatio : public RenderProxyBox {
public:
    explicit RenderAspectRatio(double ratio = 1.0) : ratio_(ratio > 0.0 ? ratio : 1.0) {}

    double aspectRatio() const { return ratio_; }
    void setAspectRatio(double r) {
        const double normalized = r > 0.0 ? r : 1.0;
        if (ratio_ == normalized)
            return;
        ratio_ = normalized;
        markNeedsLayout();
    }

protected:
    void performLayout() override;

private:
    double ratio_ = 1.0;
};

// ── RenderFractionallySizedBox ─────────────────────────────────────────────
/// 按父约束比例取尺寸的盒。上游的 `RenderFractionallySizedBox`。
///
/// widthFactor / heightFactor 为 nullopt 时该轴不缩放（沿用父约束）。
class RenderFractionallySizedBox : public RenderProxyBox {
public:
    RenderFractionallySizedBox(std::optional<double> widthFactor = std::nullopt,
                               std::optional<double> heightFactor = std::nullopt)
        : widthFactor_(widthFactor), heightFactor_(heightFactor) {}

    const std::optional<double>& widthFactor() const { return widthFactor_; }
    void setWidthFactor(std::optional<double> f) {
        if (widthFactor_ == f)
            return;
        widthFactor_ = f;
        markNeedsLayout();
    }

    const std::optional<double>& heightFactor() const { return heightFactor_; }
    void setHeightFactor(std::optional<double> f) {
        if (heightFactor_ == f)
            return;
        heightFactor_ = f;
        markNeedsLayout();
    }

protected:
    void performLayout() override;

private:
    std::optional<double> widthFactor_;
    std::optional<double> heightFactor_;
};

// ── RenderProxyBoxWithHitTestBehavior / RenderMetaData ─────────────────────
/// 带命中行为开关的代理盒。上游的 `RenderProxyBoxWithHitTestBehavior`。
///
/// 上游的 `behavior` setter 只做 `markNeedsPaint()`（命中测试每帧现算，没有
/// 缓存要失效）；本仓同理，不需要标脏，但为了将来接入命中缓存仍保留等值早退。
class RenderProxyBoxWithHitTestBehavior : public RenderProxyBox {
public:
    explicit RenderProxyBoxWithHitTestBehavior(
        HitTestBehavior behavior = HitTestBehavior::DeferToChild)
        : behavior_(behavior) {}

    /// 上游的 `RenderProxyBoxWithHitTestBehavior.behavior`。
    HitTestBehavior behavior() const { return behavior_; }
    void setBehavior(HitTestBehavior v) {
        if (v == behavior_)
            return;
        behavior_ = v;
    }

protected:
    /// 上游是两条：`hitTestSelf` = `behavior != deferToChild`，而「点必须在盒子
    /// 内」由 `RenderBox.hitTest` 的 `size.contains(position)` 前置判定负责
    /// （rendering/box.dart）。本仓的 `RenderBox::hitTest` 没有那条判定（各命中
    /// 盒自己夹范围），所以这里一起夹——否则 translucent 的标记盒会出现在
    /// **每一条**命中链上（`DragTarget` 会认领任意坐标的拖拽）。
    ///
    /// opaque 与 translucent 在上游的差别只在 `hitTest` 末尾那句
    /// `if (hitTarget || behavior == translucent) result.add(...)`：两者在这里
    /// 都返回 true，故都留在链上并阻断身后兄弟，与本仓一致。
    bool hitTestSelf(const Offset& position) const override {
        if (behavior_ == HitTestBehavior::DeferToChild)
            return false;
        return position.dx >= 0 && position.dy >= 0 && position.dx < size_.width &&
               position.dy < size_.height;
    }

private:
    HitTestBehavior behavior_ = HitTestBehavior::DeferToChild;
};

/// 命中链上的不透明标记。上游的 `RenderMetaData`（rendering/proxy_box.dart）。
///
/// 上游用 `Object? metaData` 携带任意数据，`DragTarget` 靠它在命中链里被认出
/// （见 widgets/drag_target.dart 的 `_getDragTargets`）。本仓的 `void*` 只服务
/// 于这一个消费者，控件层负责把它还原成自己的 State 类型。
class RenderMetaData : public RenderProxyBoxWithHitTestBehavior {
public:
    explicit RenderMetaData(
        void* metaData = nullptr,
        HitTestBehavior behavior = HitTestBehavior::DeferToChild)
        : RenderProxyBoxWithHitTestBehavior(behavior), metaData_(metaData) {}

    /// 上游的 `RenderMetaData.metaData`。
    void* metaData() const { return metaData_; }
    void setMetaData(void* v) {
        if (v == metaData_)
            return;
        metaData_ = v;
    }

private:
    void* metaData_ = nullptr;
};

// ── RenderIgnorePointer ────────────────────────────────────────────────────
/// 命中测试屏蔽盒。上游的 `RenderIgnorePointer` / `RenderAbsorbPointer`。
///
///   ignoring = true  → 自身与整棵子树都不参与命中（事件穿透到下层的兄弟/祖先）；
///   absorbing = true → 自身吃掉命中（留在命中链），子节点不再参与。
class RenderIgnorePointer : public RenderProxyBox {
public:
    explicit RenderIgnorePointer(bool ignoring = true, bool absorbing = false)
        : ignoring_(ignoring), absorbing_(absorbing) {}

    bool ignoring() const { return ignoring_; }
    void setIgnoring(bool v) { ignoring_ = v; }

    bool absorbing() const { return absorbing_; }
    void setAbsorbing(bool v) { absorbing_ = v; }

    /// 命中测试：ignoring 时整棵子树被跳过；absorbing 时只留自身。
    bool hitTest(const Offset& position, std::vector<RenderObject*>& result) override;

protected:
    bool hitTestSelf(const Offset& position) const override;

private:
    bool ignoring_ = true;
    bool absorbing_ = false;
};

// ── RenderMouseRegion ───────────────────────────────────────────────────────
/// 鼠标悬停区域。上游的 `RenderMouseRegion`（rendering/proxy_box.dart，
/// `RenderProxyBoxWithHitTestBehavior` implements `MouseTrackerAnnotation`）。
///
/// 上游的 enter/exit 不是自己 handleEvent 出来的，而是 `MouseTracker` 按命中链
/// 上的 `MouseTrackerAnnotation` 算出「进入/离开集合」后**直接调回调**（只有
/// `onHover` 走 `handleEvent` 收 PointerHoverEvent）。本仓没有 MouseTracker，
/// 同一份差分在 `PipelineOwner::dispatchPointerEvent` 里做（新旧命中链的差集
/// 合成 Enter/Exit 事件沿链派发），所以这里按上游 handleEvent 的形状回应三种
/// 事件。行为等价，差别只在「谁来调回调」。
///
/// 未接：`cursor`（上游 `MouseCursor`/`SystemMouseCursor`）——本仓平台层还没有
/// 光标形状抽象；`hitTestBehavior`（上游 3.47 默认 `HitTestBehavior.opaque`，
/// 即本仓 `hitTestSelf` 写死的「尺寸内即命中」，另两种 `translucent`/`transparent`
/// 本仓没有 `HitTestBehavior` 枚举）；`validForMouseTracker` 是 MouseTracker 的
/// 失效判据，没有 MouseTracker 就没有对应物。
class RenderMouseRegion : public RenderProxyBox {
public:
    using PointerCallback = std::function<void(const PointerEvent&)>;

    explicit RenderMouseRegion(PointerCallback onEnter = {}, PointerCallback onExit = {},
                               PointerCallback onHover = {}, bool opaque = true)
        : onEnter(std::move(onEnter)),
          onExit(std::move(onExit)),
          onHover(std::move(onHover)),
          opaque_(opaque) {}

    /// 上游的 `MouseTrackerAnnotation.onEnter/onExit` + `onHover`。公开字段
    /// 与 `RenderPointerListener` 同风格（Widget 层 updateRenderObject 直接写）。
    PointerCallback onEnter;
    PointerCallback onExit;
    PointerCallback onHover;

    /// 上游的 `opaque`：true ⇒ 本区域吃掉鼠标指针，身后的**兄弟**区域收不到；
    /// false ⇒ 双方都算命中。变更时上游 `markNeedsPaint()` 触发 MouseTracker
    /// 重算，本仓的差分每帧现算，无需额外标记。
    bool opaque() const { return opaque_; }
    void setOpaque(bool v) { opaque_ = v; }

    /// 上游同款：`super.hitTest(result, position) && _opaque`。本仓命中链是
    /// 「边遍历边 push」的向量，返回值只决定父层要不要继续问下一个兄弟 ⇒
    /// opaque=false 时自身与子节点仍留在链上，但身后的兄弟也能被命中。
    bool hitTest(const Offset& position, std::vector<RenderObject*>& result) override {
        const bool hit = RenderProxyBox::hitTest(position, result);
        return hit && opaque_;
    }

    /// 上游的 `computeSizeForNoChild`：无子节点时撑满约束（本仓
    /// `RenderProxyBox` 默认取 `smallest()`，会让无内容的悬停区域塌成 0×0 而
    /// 永远命中不到）。
    void performLayout() override {
        if (child_) {
            child_->layout(constraints(), false);
            if (child_->parentData())
                child_->parentData()->offset = Offset(0, 0);
            setSize(child_->size());
        } else {
            setSize(constraints().biggest());
        }
    }

    void handleEvent(const PointerEvent& event, const HitTestEntry& entry) override;

protected:
    /// 上游 `RenderProxyBoxWithHitTestBehavior.hitTestSelf`，behavior 恒为
    /// MouseRegion 默认的 `HitTestBehavior.opaque` ⇒ 尺寸内即命中（否则一个
    /// 没有可命中子节点的区域永远进不了命中链，也就拿不到 Enter/Exit）。
    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 && position.dx < size_.width &&
               position.dy < size_.height;
    }

private:
    bool opaque_ = true;
};

// ── RenderClipRect / RenderClipOval ─────────────────────────────────────────
/// 矩形裁剪代理盒。上游的 `RenderClipRect`（clip.dart）。
///
/// 自身不改变布局：裁剪区域即自身盒子（0..size）。
class RenderClipRect : public RenderProxyBox {
public:
    explicit RenderClipRect(bool antiAlias = true) : antiAlias_(antiAlias) {}

    bool antiAlias() const { return antiAlias_; }
    void setAntiAlias(bool v) { antiAlias_ = v; }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    bool antiAlias_ = true;
};

/// 椭圆裁剪代理盒。上游的 `RenderClipOval`（clip.dart）。
class RenderClipOval : public RenderProxyBox {
public:
    explicit RenderClipOval(bool antiAlias = true) : antiAlias_(antiAlias) {}

    bool antiAlias() const { return antiAlias_; }
    void setAntiAlias(bool v) { antiAlias_ = v; }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    bool antiAlias_ = true;
};

// ── RenderTransform ─────────────────────────────────────────────────────────
/// 变换代理盒。上游的 `RenderTransform`（proxy_box.dart）。
///
/// 布局与 上游 一致：子节点按原约束布局，自身尺寸 = 子节点尺寸。
/// 绘制时把 `_effectiveTransform` 作用在自身盒子原点周围
/// （`T(offset) · M · T(-offset)`，与 `PaintingContext.pushTransform` 一致），
/// 命中测试则对点做逆变换。
class RenderTransform : public RenderProxyBox {
public:
    explicit RenderTransform(Matrix4 transform = Matrix4::identity(), Offset origin = Offset(),
                             std::optional<Alignment> alignment = std::nullopt)
        : transform_(transform), origin_(origin), alignment_(alignment) {}

    const Matrix4& transform() const { return transform_; }
    void setTransform(const Matrix4& m) {
        if (transform_ == m)
            return;
        transform_ = m;
        markNeedsPaint();
    }

    /// 变换中心（上游的 `RenderTransform.origin`，局部坐标）。
    const Offset& origin() const { return origin_; }
    void setOrigin(const Offset& o) {
        if (origin_ == o)
            return;
        origin_ = o;
        markNeedsPaint();
    }

    /// 变换中心相对盒子尺寸的对齐（上游的 `RenderTransform.alignment`）。
    /// 与 origin 同时给出时两者叠加，与上游一致。
    const std::optional<Alignment>& alignment() const { return alignment_; }
    void setAlignment(std::optional<Alignment> a) {
        if (alignment_ == a)
            return;
        alignment_ = a;
        markNeedsPaint();
    }

    /// 变换是否作用于命中测试（上游的 `transformHitTests`）。
    bool transformHitTests() const { return transformHitTests_; }
    void setTransformHitTests(bool v) { transformHitTests_ = v; }

    /// 上游的 `_effectiveTransform`：绕 origin 施加变换。
    Matrix4 effectiveTransform() const;

    /// 命中测试：把点逆变换回子节点坐标系（上游的
    /// `MatrixUtils.transformPoint(_effectiveTransformInverse, position)`）。
    bool hitTest(const Offset& position, std::vector<RenderObject*>& result) override;

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    Matrix4 transform_;
    Offset origin_;
    std::optional<Alignment> alignment_;
    bool transformHitTests_ = true;
};

// ── RenderFractionalTranslation ────────────────────────────────────────────
/// 按子节点自身尺寸的比例平移（上游的 `RenderFractionalTranslation`）。
///
/// 上游语义：平移量 = translation × child.size，因此 translation=(0,1) 表示
/// 「向下偏移一个自身高度」。命中区域默认随平移一起移动（`transformHitTests`
/// = true，与上游一致）；置 false 时命中仍按未平移的位置计算。
class RenderFractionalTranslation : public RenderProxyBox {
public:
    explicit RenderFractionalTranslation(Offset translation = Offset(),
                                         bool transformHitTests = true)
        : translation_(translation), transformHitTests_(transformHitTests) {}

    Offset translation() const { return translation_; }
    void setTranslation(Offset t) {
        if (translation_ == t)
            return;
        translation_ = t;
        markNeedsPaint();
    }

    /// 上游的 `RenderFractionalTranslation.transformHitTests`。
    bool transformHitTests() const { return transformHitTests_; }
    void setTransformHitTests(bool v) {
        if (transformHitTests_ == v)
            return;
        transformHitTests_ = v;
        markNeedsPaint();
    }

    /// 上游的绘制平移量：translation × size（与 performPaint 用同一公式）。
    Offset translationOffset() const {
        return Offset(translation_.dx * size_.width, translation_.dy * size_.height);
    }

    bool hitTest(const Offset& position, std::vector<RenderObject*>& result) override;

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    Offset translation_;
    bool transformHitTests_ = true;
};

// ── RenderRotatedBox ────────────────────────────────────────────────────────
/// 按 90° 的整数倍旋转子节点。上游的 `RenderRotatedBox`（rotated_box.dart）。
///
/// 布局：子节点拿到**翻转**后的约束（flipped），自身尺寸 = 子节点尺寸翻转；
/// 绘制：绕盒子原点把子节点转回来（translate + rotateZ）。
class RenderRotatedBox : public RenderProxyBox {
public:
    explicit RenderRotatedBox(int quarterTurns = 0) : quarterTurns_(quarterTurns) {}

    int quarterTurns() const { return quarterTurns_; }
    void setQuarterTurns(int t) {
        const int n = ((t % 4) + 4) % 4;
        if (quarterTurns_ == n)
            return;
        quarterTurns_ = n;
        markNeedsLayout();
    }

    /// 本次布局得到的绘制变换（供测试与自定义绘制复用）。
    const Matrix4& paintTransform() const { return paintTransform_; }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    int quarterTurns_ = 0;
    Matrix4 paintTransform_;
};

// ── RenderFittedBox ─────────────────────────────────────────────────────────
/// 按 BoxFit 缩放子节点的代理盒。上游的 `RenderFittedBox`（proxy_box.dart）。
///
/// 布局算法对齐上游：
///   - 子节点在**无约束**下测量自然尺寸；
///   - 自身尺寸 = `constraints.constrainSizeAndAttemptToPreserveAspectRatio(childSize)`
///     （scaleDown 用 loosen 后的约束）；
///   - 记录 `applyBoxFit` 得到的变换矩阵，绘制时施加在子节点上。
class RenderFittedBox : public RenderProxyBox {
public:
    explicit RenderFittedBox(BoxFit fit = BoxFit::Contain, Alignment alignment = Alignment::center(),
                             bool clipContent = true)
        : fit_(fit), alignment_(alignment), clipContent_(clipContent) {}

    BoxFit fit() const { return fit_; }
    void setFit(BoxFit f) {
        if (fit_ == f)
            return;
        fit_ = f;
        markNeedsLayout();
    }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) {
        if (alignment_ == a)
            return;
        alignment_ = a;
        markNeedsLayout();
    }

    /// 本次布局得到的子节点变换矩阵（diag(scale) + 平移）。
    const Matrix4& childTransform() const { return transform_; }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    BoxFit fit_ = BoxFit::Contain;
    Alignment alignment_ = Alignment::center();
    bool clipContent_ = true;
    Matrix4 transform_;
};

// ── RenderBaseline ──────────────────────────────────────────────────────────
/// 基线对齐盒。上游的 `RenderBaseline`（shifted_box.dart）。
///
/// 子节点按松约束布局，再把它的基线顶到 `baseline` 指定的位置。
/// 子节点没有基线信息（`getDistanceToBaseline < 0`）时退化为「底边对齐基线」。
class RenderBaseline : public RenderProxyBox {
public:
    explicit RenderBaseline(double baseline = 0.0,
                            BaselineType baselineType = BaselineType::Alphabetic)
        : baseline_(baseline), baselineType_(baselineType) {}

    double baseline() const { return baseline_; }
    void setBaseline(double b) {
        if (baseline_ == b)
            return;
        baseline_ = b;
        markNeedsLayout();
    }

    BaselineType baselineType() const { return baselineType_; }
    void setBaselineType(BaselineType t) {
        if (baselineType_ == t)
            return;
        baselineType_ = t;
        markNeedsLayout();
    }

protected:
    void performLayout() override;

private:
    double baseline_ = 0.0;
    BaselineType baselineType_ = BaselineType::Alphabetic;
};

// ── RenderLimitedBox ────────────────────────────────────────────────────────
/// 只有在**父约束无界**时才限制尺寸的盒。上游的 `RenderLimitedBox`
///（shifted_box.dart）。
class RenderLimitedBox : public RenderProxyBox {
public:
    explicit RenderLimitedBox(double maxWidth = BoxConstraints::kInfinity,
                              double maxHeight = BoxConstraints::kInfinity)
        : maxWidth_(maxWidth), maxHeight_(maxHeight) {}

    double maxWidth() const { return maxWidth_; }
    void setMaxWidth(double v) {
        if (maxWidth_ == v)
            return;
        maxWidth_ = v;
        markNeedsLayout();
    }

    double maxHeight() const { return maxHeight_; }
    void setMaxHeight(double v) {
        if (maxHeight_ == v)
            return;
        maxHeight_ = v;
        markNeedsLayout();
    }

protected:
    void performLayout() override;

private:
    double maxWidth_ = BoxConstraints::kInfinity;
    double maxHeight_ = BoxConstraints::kInfinity;
};

// ── RenderOverflowBox ───────────────────────────────────────────────────────
/// 允许子节点溢出自身的盒。上游的 `RenderOverflowBox`（shifted_box.dart）。
///
/// 自身尺寸 = 父约束的 biggest（在父节点看来「占满可用空间」），
/// 子节点按 min/max 覆盖后的约束布局，再按 alignment 摆放。
class RenderOverflowBox : public RenderProxyBox {
public:
    explicit RenderOverflowBox(std::optional<double> minWidth = std::nullopt,
                               std::optional<double> maxWidth = std::nullopt,
                               std::optional<double> minHeight = std::nullopt,
                               std::optional<double> maxHeight = std::nullopt,
                               Alignment alignment = Alignment::center())
        : minWidth_(minWidth), maxWidth_(maxWidth), minHeight_(minHeight), maxHeight_(maxHeight),
          alignment_(alignment) {}

    const std::optional<double>& minWidth() const { return minWidth_; }
    void setMinWidth(std::optional<double> v) { if (minWidth_ == v) return; minWidth_ = v; markNeedsLayout(); }
    const std::optional<double>& maxWidth() const { return maxWidth_; }
    void setMaxWidth(std::optional<double> v) { if (maxWidth_ == v) return; maxWidth_ = v; markNeedsLayout(); }
    const std::optional<double>& minHeight() const { return minHeight_; }
    void setMinHeight(std::optional<double> v) { if (minHeight_ == v) return; minHeight_ = v; markNeedsLayout(); }
    const std::optional<double>& maxHeight() const { return maxHeight_; }
    void setMaxHeight(std::optional<double> v) { if (maxHeight_ == v) return; maxHeight_ = v; markNeedsLayout(); }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { if (alignment_ == a) return; alignment_ = a; markNeedsLayout(); }

protected:
    void performLayout() override;

private:
    std::optional<double> minWidth_, maxWidth_, minHeight_, maxHeight_;
    Alignment alignment_ = Alignment::center();
};

// ── RenderSizedOverflowBox ──────────────────────────────────────────────────
/// 固定自身尺寸、允许子节点溢出的盒。上游的 `RenderSizedOverflowBox`。
class RenderSizedOverflowBox : public RenderProxyBox {
public:
    explicit RenderSizedOverflowBox(Size requestedSize = Size(),
                                    Alignment alignment = Alignment::center())
        : requestedSize_(requestedSize), alignment_(alignment) {}

    const Size& requestedSize() const { return requestedSize_; }
    void setRequestedSize(const Size& s) {
        if (requestedSize_ == s)
            return;
        requestedSize_ = s;
        markNeedsLayout();
    }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { if (alignment_ == a) return; alignment_ = a; markNeedsLayout(); }

protected:
    void performLayout() override;

private:
    Size requestedSize_;
    Alignment alignment_ = Alignment::center();
};

// ── RenderUnconstrainedBox ──────────────────────────────────────────────────
/// 解除子节点约束的盒。上游的 `RenderUnconstrainedBox`（shifted_box.dart）。
///
/// 子节点拿到无约束（`constrainedAxis` 指定轴仍沿用父约束），自身尺寸 = 子节点尺寸。
class RenderUnconstrainedBox : public RenderProxyBox {
public:
    explicit RenderUnconstrainedBox(std::optional<Axis> constrainedAxis = std::nullopt,
                                    Alignment alignment = Alignment::center())
        : constrainedAxis_(constrainedAxis), alignment_(alignment) {}

    const std::optional<Axis>& constrainedAxis() const { return constrainedAxis_; }
    void setConstrainedAxis(std::optional<Axis> a) { if (constrainedAxis_ == a) return; constrainedAxis_ = a; markNeedsLayout(); }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { if (alignment_ == a) return; alignment_ = a; markNeedsLayout(); }

protected:
    void performLayout() override;

private:
    std::optional<Axis> constrainedAxis_;
    Alignment alignment_ = Alignment::center();
};

// ── RenderOffstage ──────────────────────────────────────────────────────────
/// 「在场但不渲染」的盒。上游的 `RenderOffstage`（proxy_box.dart）。
///
/// offstage 时子节点仍参与布局（便于测量），但：
///   - 不绘制；
///   - 不参与命中测试；
///   - **不占用父节点的空间**（自身尺寸归零）。
class RenderOffstage : public RenderProxyBox {
public:
    explicit RenderOffstage(bool offstage = true) : offstage_(offstage) {}

    bool offstage() const { return offstage_; }
    void setOffstage(bool v) {
        if (offstage_ == v)
            return;
        offstage_ = v;
        markNeedsLayout();
    }

    /// 上游的 `RenderOffstage.hitTest` 在 offstage 时直接失败。
    bool hitTest(const Offset& position, std::vector<RenderObject*>& result) override;

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    bool offstage_ = true;
};

// ── RenderImage ─────────────────────────────────────────────────────────────
/// 图像渲染盒。上游的 `RenderImage`（image.dart）。
///
/// 支持 `BoxFit` 适配 + `Alignment` 摆放，布局尺寸 =
/// `constraints.constrainSizeAndAttemptToPreserveAspectRatio(图像尺寸 / scale)`。
class RenderImage : public RenderBox {
public:
    RenderImage() = default;

    const sk_sp<SkImage>& image() const { return image_; }
    void setImage(sk_sp<SkImage> img) {
        if (image_ == img)
            return;
        image_ = std::move(img);
        markNeedsLayout();
    }

    /// 显式宽度（nullopt 表示由图像与约束决定）。
    const std::optional<double>& width() const { return width_; }
    void setWidth(std::optional<double> w) { if (width_ == w) return; width_ = w; markNeedsLayout(); }

    /// 显式高度（nullopt 表示由图像与约束决定）。
    const std::optional<double>& height() const { return height_; }
    void setHeight(std::optional<double> h) { if (height_ == h) return; height_ = h; markNeedsLayout(); }

    /// 图像缩放：逻辑尺寸 = 像素尺寸 / scale。
    double scale() const { return scale_; }
    void setScale(double s) {
        const double normalized = s > 0.0 ? s : 1.0;
        if (scale_ == normalized)
            return;
        scale_ = normalized;
        markNeedsLayout();
    }

    BoxFit fit() const { return fit_; }
    void setFit(BoxFit f) { if (fit_ == f) return; fit_ = f; markNeedsPaint(); }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { if (alignment_ == a) return; alignment_ = a; markNeedsPaint(); }

    /// 图像原始像素尺寸（未加载时为 0）。
    Size imageSize() const;

    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 && position.dx < size_.width &&
               position.dy < size_.height;
    }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    /// 上游的 `RenderImage._sizeForConstraints`。
    Size sizeForConstraints(const BoxConstraints& c) const;

    sk_sp<SkImage> image_;
    std::optional<double> width_;
    std::optional<double> height_;
    double scale_ = 1.0;
    BoxFit fit_ = BoxFit::Contain;
    Alignment alignment_ = Alignment::center();
};

// ── RenderCustomPaint ───────────────────────────────────────────────────────
/// 自绘盒。上游的 `RenderCustomPaint`（custom_paint.dart）。
///
/// `painter` 在子节点**之前**绘制（背景），`foregroundPainter` 在子节点之后。
/// 无子节点时尺寸 = `preferredSize`（受约束 clamp）；有子节点时透传子节点尺寸。
class RenderCustomPaint : public RenderProxyBox {
public:
    using PainterCallback = std::function<void(Painter&, const Size&)>;

    /// 自绘盒是天然的自包含绘制单元：作为 repaint boundary，使其重绘被隔离
    /// 进独立的 PictureLayer（增量重绘，Compose 方式）。动画时只有这个画布
    /// 重光栅化，整页其余内容由 composite 直接 replay 缓存图，CPU 大幅下降。
    bool isRepaintBoundary() const override { return true; }

    explicit RenderCustomPaint(PainterCallback painter = nullptr,
                               PainterCallback foregroundPainter = nullptr,
                               Size preferredSize = Size())
        : painter_(std::move(painter)), foregroundPainter_(std::move(foregroundPainter)),
          preferredSize_(preferredSize) {}

    void setPainter(PainterCallback p) { painter_ = std::move(p); markNeedsPaint(); }
    void setForegroundPainter(PainterCallback p) {
        foregroundPainter_ = std::move(p);
        markNeedsPaint();
    }

    const Size& preferredSize() const { return preferredSize_; }
    void setPreferredSize(const Size& s) {
        // 与上游 RenderCustomPaint.preferredSize setter 一致：尺寸未变时
        // 不标脏布局。AnimatedBuilder 每帧 update 都会回灌同一个固定尺寸，
        // 若无条件 markNeedsLayout 会把脏标冒泡到根（自绘盒非 relayout
        // 边界），导致整页每帧重布局——动画页 CPU 高的根因。
        if (s == preferredSize_)
            return;
        preferredSize_ = s;
        markNeedsLayout();
    }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    PainterCallback painter_;
    PainterCallback foregroundPainter_;
    Size preferredSize_;
};

// ── RenderWrap ──────────────────────────────────────────────────────────────
/// 流式换行布局。上游的 `RenderWrap`（wrap.dart）。
///
/// 主轴放不下就换一行（run）；每个 run 内按 `alignment` 分布，
/// run 之间按 `runAlignment` / `runSpacing` 分布，run 内交叉轴按
/// `crossAxisAlignment` 对齐。
class RenderWrap : public RenderBoxContainer {
public:
    explicit RenderWrap(Axis direction = Axis::Horizontal,
                        WrapAlignment alignment = WrapAlignment::Start, double spacing = 0.0,
                        WrapAlignment runAlignment = WrapAlignment::Start, double runSpacing = 0.0,
                        WrapCrossAlignment crossAxisAlignment = WrapCrossAlignment::Start)
        : direction_(direction), alignment_(alignment), spacing_(spacing),
          runAlignment_(runAlignment), runSpacing_(runSpacing),
          crossAxisAlignment_(crossAxisAlignment) {}

    Axis direction() const { return direction_; }
    void setDirection(Axis d) { if (direction_ == d) return; direction_ = d; markNeedsLayout(); }

    WrapAlignment alignment() const { return alignment_; }
    void setAlignment(WrapAlignment a) { if (alignment_ == a) return; alignment_ = a; markNeedsLayout(); }

    double spacing() const { return spacing_; }
    void setSpacing(double s) { if (spacing_ == s) return; spacing_ = s; markNeedsLayout(); }

    WrapAlignment runAlignment() const { return runAlignment_; }
    void setRunAlignment(WrapAlignment a) { if (runAlignment_ == a) return; runAlignment_ = a; markNeedsLayout(); }

    double runSpacing() const { return runSpacing_; }
    void setRunSpacing(double s) { if (runSpacing_ == s) return; runSpacing_ = s; markNeedsLayout(); }

    WrapCrossAlignment crossAxisAlignment() const { return crossAxisAlignment_; }
    void setCrossAxisAlignment(WrapCrossAlignment a) {
        if (crossAxisAlignment_ == a)
            return;
        crossAxisAlignment_ = a;
        markNeedsLayout();
    }

    /// 本次布局产生的 run 数量。
    size_t runCount() const { return runCount_; }

    void setupParentData(RenderObject* child) override;

protected:
    void performLayout() override;

private:
    /// 主轴 / 交叉轴尺寸读取（按 direction 取 Size 的对应分量）。
    double mainExtentOf(const Size& s) const {
        return direction_ == Axis::Horizontal ? s.width : s.height;
    }
    double crossExtentOf(const Size& s) const {
        return direction_ == Axis::Horizontal ? s.height : s.width;
    }

    Axis direction_ = Axis::Horizontal;
    WrapAlignment alignment_ = WrapAlignment::Start;
    double spacing_ = 0.0;
    WrapAlignment runAlignment_ = WrapAlignment::Start;
    double runSpacing_ = 0.0;
    WrapCrossAlignment crossAxisAlignment_ = WrapCrossAlignment::Start;
    size_t runCount_ = 0;
};

// ── RenderParagraph ──────────────────────────────────────────────────────
/// 文本渲染盒。上游的 `RenderParagraph`（paragraph.dart）的单行简化版。
///
/// 持有 `TextSpan` 根：布局时遍历所有叶 span 累加宽度（不换行），高度取
/// 最大行高，基线取最大基线下偏移；绘制时复用 `Painter::showText`，逐 run
/// 用各自的 `TextStyle`（字号 / 字重 / 斜体 / 颜色）栅格化。
class RenderParagraph : public RenderBox {
public:
    explicit RenderParagraph(SharedPtr<TextSpan> text)
        : text_(text ? std::move(text) : cui::make_shared<TextSpan>()) {}

    const SharedPtr<TextSpan>& text() const { return text_; }
    void setText(SharedPtr<TextSpan> t) {
        if (!t)
            t = cui::make_shared<TextSpan>();
        if (t == text_)
            return;
        text_ = std::move(t);
        markNeedsLayout();
    }

    /// 顶边到基线的距离（布局后有效）。
    double getDistanceToBaseline(BaselineType type) const override {
        (void)type;
        return baselineOffset_;
    }

    /// 内容（不换行）总宽，布局后有效。
    double contentWidth() const { return contentWidth_; }

    /// 顶边到基线的距离（布局后有效）。
    double baselineOffset() const { return baselineOffset_; }

protected:
    void performLayout() override;
    void performPaint(PaintingContext& context, const Offset& offset) override;

    ///@{ 单行文本 = 经典的 width-in-height-out：min = max = 不换行的自然宽度
    /// （忽略 height 入参），高 = 最大行高（忽略 width 入参）。本仓
    /// RenderParagraph 不支持换行，故四个值只有两组。
    double computeMinIntrinsicWidth(double height) override;
    double computeMaxIntrinsicWidth(double height) override;
    double computeMinIntrinsicHeight(double width) override;
    double computeMaxIntrinsicHeight(double width) override;
    ///@}

private:
    /// 自然尺寸（不换行的内容宽 / 最大行高 / 最大基线偏移）。
    struct NaturalSize {
        double width = 0.0;
        double height = 0.0;
        double baseline = 0.0;
    };

    /// 遍历所有叶 span 做测量。`performLayout` 与 intrinsic 计算共用这一份，
    /// 保证「布局出来的尺寸」和「对外报告的 intrinsic」不会漂移。
    NaturalSize measureNatural() const;

    SharedPtr<TextSpan> text_;
    double contentWidth_ = 0.0;
    double baselineOffset_ = 0.0;
};

// ── RenderLayoutBuilder ─────────────────────────────────────────────────
/// LayoutBuilder 的渲染对象。上游的 `_RenderLayoutBuilder`。
///
/// 持有唯一的子节点；在 `performLayout` 期间回调 `layoutCallback`（由
/// `LayoutBuilderElement` 注入），把当前约束交给 Element 触发重建；随后
/// 像普通单子盒一样布局子节点并以子节点尺寸作为自身尺寸。
class RenderLayoutBuilder : public RenderProxyBox {
public:
    /// 由 LayoutBuilderElement 在 mount 时注入：约束变化时同步重建子节点。
    std::function<void(const BoxConstraints&)> layoutCallback;

protected:
    void performLayout() override {
        if (layoutCallback)
            layoutCallback(constraints_);
        if (child()) {
            child()->layout(constraints_);
            setSize(child()->size());
        } else {
            setSize(constraints_.biggest());
        }
    }
};

}  // namespace cui_core
