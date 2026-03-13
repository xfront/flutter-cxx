#pragma once
/// @file widgets/basic.h
/// @brief 上游 基础 Widget 移植。
///
/// 对应 上游源码：
///   上游 widgets/basic.dart
///
/// 覆盖最常用的一组：
///   Padding / SizedBox / ConstrainedBox / Align / Center
///   IntrinsicHeight / IntrinsicWidth
///   Flex / Column / Row / Expanded / Flexible
///   ColoredBox
///
/// 层级映射严格对齐上游：
///   Padding / SizedBox / ConstrainedBox / Align / Center
///       → SingleChildRenderObjectWidget（单子节点）
///   ColoredBox
///       → LeafRenderObjectWidget（叶子）
///   Flex / Column / Row
///       → MultiChildRenderObjectWidget（多子节点）
///   Expanded / Flexible
///       → ParentDataWidget（不产生 RenderObject，只写 parentData）

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_flex.h"
#include "cui/framework/render_object_widget.h"

namespace cui {

// ── Padding ─────────────────────────────────────────────────────────────────

/// 内边距。上游的 `Padding`（basic.dart）。
///
/// 上游 中 `Padding` 继承 `SingleChildRenderObjectWidget`。
class Padding : public SingleChildRenderObjectWidget {
public:
    explicit Padding(EdgeInsets padding = {}, SharedPtr<Widget> child = nullptr,
                     KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), padding_(padding) {}

    const EdgeInsets& padding() const { return padding_; }
    void setPadding(const EdgeInsets& p) { padding_ = p; }

    RenderObject* createRenderObject() override { return new RenderPadding(padding_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* p = dynamic_cast<RenderPadding*>(ro))
            p->setPadding(padding_);
    }

private:
    EdgeInsets padding_;
};

// ── SizedBox ────────────────────────────────────────────────────────────────

/// 固定尺寸。上游的 `SizedBox`（basic.dart）。
///
/// 上游 用 `RenderConstrainedBox` + 紧约束实现 SizedBox；这里保留独立的
/// `RenderConstrainedBox` 分支，通过 tight 约束达到同样效果。
class SizedBox : public SingleChildRenderObjectWidget {
public:
    SizedBox(double width, double height, SharedPtr<Widget> child = nullptr,
             KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          width_(width), height_(height) {}

    double width() const { return width_; }
    double height() const { return height_; }
    void setSize(double w, double h) { width_ = w; height_ = h; }

    RenderObject* createRenderObject() override {
        return new RenderConstrainedBox(BoxConstraints::tight(width_, height_));
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* c = dynamic_cast<RenderConstrainedBox*>(ro))
            c->setAdditionalConstraints(BoxConstraints::tight(width_, height_));
    }

private:
    double width_ = 0.0;
    double height_ = 0.0;
};

// ── ConstrainedBox ──────────────────────────────────────────────────────────

/// 附加约束。上游的 `ConstrainedBox`（basic.dart）。
class ConstrainedBox : public SingleChildRenderObjectWidget {
public:
    explicit ConstrainedBox(BoxConstraints constraints = BoxConstraints::unbounded(),
                             SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          constraints_(constraints) {}

    const BoxConstraints& constraints() const { return constraints_; }
    void setConstraints(const BoxConstraints& c) { constraints_ = c; }

    RenderObject* createRenderObject() override { return new RenderConstrainedBox(constraints_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* c = dynamic_cast<RenderConstrainedBox*>(ro))
            c->setAdditionalConstraints(constraints_);
    }

private:
    BoxConstraints constraints_;
};

// ── IntrinsicHeight / IntrinsicWidth ───────────────────────────────────────

/// 让子节点按自身最大 intrinsic 高度取高。上游的 `IntrinsicHeight`。
///
/// 上游注释原话：这类盒相对昂贵，能不用就不用。
class IntrinsicHeight : public SingleChildRenderObjectWidget {
public:
    explicit IntrinsicHeight(SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)) {}

    RenderObject* createRenderObject() override { return new RenderIntrinsicHeight(); }
    void updateRenderObject(RenderObject* ro) override {
        // 上游的 IntrinsicHeight 没有任何可变属性，updateRenderObject 是空实现。
        (void)ro;
    }
};

/// 让子节点按自身最大 intrinsic 宽度取宽。上游的 `IntrinsicWidth`。
///
/// stepWidth / stepHeight 的 0.0 与「不给」等价（上游把 0.0 归一化成 null），
/// 且都不得为负。
class IntrinsicWidth : public SingleChildRenderObjectWidget {
public:
    explicit IntrinsicWidth(std::optional<double> stepWidth = std::nullopt,
                            std::optional<double> stepHeight = std::nullopt,
                            SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          stepWidth_(normalizeStep(stepWidth)),
          stepHeight_(normalizeStep(stepHeight)) {}

    const std::optional<double>& stepWidth() const { return stepWidth_; }
    const std::optional<double>& stepHeight() const { return stepHeight_; }
    void setStepWidth(std::optional<double> v) { stepWidth_ = normalizeStep(v); }
    void setStepHeight(std::optional<double> v) { stepHeight_ = normalizeStep(v); }

    RenderObject* createRenderObject() override {
        return new RenderIntrinsicWidth(stepWidth_, stepHeight_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* w = dynamic_cast<RenderIntrinsicWidth*>(ro)) {
            w->setStepWidth(stepWidth_);
            w->setStepHeight(stepHeight_);
        }
    }

private:
    /// 上游的 `_stepWidth / _stepHeight`：0.0 视为「不设步长」。
    static std::optional<double> normalizeStep(const std::optional<double>& step) {
        if (step.has_value() && step.value() == 0.0)
            return std::nullopt;
        return step;
    }

    std::optional<double> stepWidth_;
    std::optional<double> stepHeight_;
};

// ── Align ───────────────────────────────────────────────────────────────────

/// 对齐。上游的 `Align`（basic.dart）。
class Align : public SingleChildRenderObjectWidget {
public:
    /// widthFactor/heightFactor 对齐上游的 `Align({widthFactor, heightFactor})`：
    /// 非空时自身在该轴取「子尺寸 × 因子」（SizeTransition / ScaleTransition 依赖）。
    explicit Align(double alignmentX = 0.0, double alignmentY = 0.0,
                   SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr,
                   std::optional<double> widthFactor = std::nullopt,
                   std::optional<double> heightFactor = std::nullopt)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          alignmentX_(alignmentX), alignmentY_(alignmentY),
          widthFactor_(widthFactor), heightFactor_(heightFactor) {}

    void setAlignment(double x, double y) { alignmentX_ = x; alignmentY_ = y; }

    const std::optional<double>& widthFactor() const { return widthFactor_; }
    const std::optional<double>& heightFactor() const { return heightFactor_; }
    void setWidthFactor(std::optional<double> f) { widthFactor_ = f; }
    void setHeightFactor(std::optional<double> f) { heightFactor_ = f; }

    RenderObject* createRenderObject() override {
        return new RenderAlign(alignmentX_, alignmentY_, widthFactor_, heightFactor_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* a = dynamic_cast<RenderAlign*>(ro)) {
            a->setAlignment(alignmentX_, alignmentY_);
            a->setWidthFactor(widthFactor_);
            a->setHeightFactor(heightFactor_);
        }
    }

private:
    double alignmentX_ = 0.0;
    double alignmentY_ = 0.0;
    std::optional<double> widthFactor_;
    std::optional<double> heightFactor_;
};

// ── Center ──────────────────────────────────────────────────────────────────

/// 居中。上游的 `Center`（basic.dart）。
///
/// 上游的 `Center` 继承 `Align`（alignment = Alignment.center）。
class Center : public Align {
public:
    explicit Center(SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : Align(0.0, 0.0, std::move(child), std::move(key)) {}
};

// ── Flex / Column / Row ─────────────────────────────────────────────────────

/// 弹性布局。上游的 `Flex`（basic.dart）。
///
/// 上游的 `Flex` 继承 `MultiChildRenderObjectWidget`，只持有 children；
/// flex 系数不保存在 Widget 上，而是由 `Expanded` / `Flexible`
/// （`ParentDataWidget`）写进子 RenderObject 的 `FlexParentData`。
class Flex : public MultiChildRenderObjectWidget {
public:
    explicit Flex(Axis direction, std::vector<SharedPtr<Widget>> children = {},
                  KeyPtr key = nullptr)
        : MultiChildRenderObjectWidget(std::move(key), std::move(children)),
          direction_(direction) {}

    Axis direction() const { return direction_; }
    void setDirection(Axis d) { direction_ = d; }

    MainAxisAlignment mainAxisAlignment() const { return mainAxisAlignment_; }
    void setMainAxisAlignment(MainAxisAlignment a) { mainAxisAlignment_ = a; }

    CrossAxisAlignment crossAxisAlignment() const { return crossAxisAlignment_; }
    void setCrossAxisAlignment(CrossAxisAlignment a) { crossAxisAlignment_ = a; }

    MainAxisSize mainAxisSize() const { return mainAxisSize_; }
    void setMainAxisSize(MainAxisSize s) { mainAxisSize_ = s; }

    RenderObject* createRenderObject() override {
        return new RenderFlex(direction_, mainAxisAlignment_, crossAxisAlignment_,
                              mainAxisSize_);
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* flex = dynamic_cast<RenderFlex*>(ro);
        if (!flex)
            return;
        flex->setDirection(direction_);
        flex->setMainAxisAlignment(mainAxisAlignment_);
        flex->setCrossAxisAlignment(crossAxisAlignment_);
        flex->setMainAxisSize(mainAxisSize_);
    }

private:
    Axis direction_;
    MainAxisAlignment mainAxisAlignment_ = MainAxisAlignment::Start;
    CrossAxisAlignment crossAxisAlignment_ = CrossAxisAlignment::Center;
    MainAxisSize mainAxisSize_ = MainAxisSize::Max;
};

/// 垂直布局。上游的 `Column`（basic.dart）。
class Column : public Flex {
public:
    explicit Column(std::vector<SharedPtr<Widget>> children = {}, KeyPtr key = nullptr)
        : Flex(Axis::Vertical, std::move(children), std::move(key)) {}
};

/// 水平布局。上游的 `Row`（basic.dart）。
class Row : public Flex {
public:
    explicit Row(std::vector<SharedPtr<Widget>> children = {}, KeyPtr key = nullptr)
        : Flex(Axis::Horizontal, std::move(children), std::move(key)) {}
};

// ── Expanded / Flexible ─────────────────────────────────────────────────────

/// 弹性子节点。上游的 `Flexible`（basic.dart）。
///
/// 上游的 `Flexible` 继承 `ParentDataWidget<FlexParentData>`：
/// 它不产生 RenderObject，只把 `flex` 与 `fit` 写进子 RenderObject 的
/// `FlexParentData`。
class Flexible : public ParentDataWidget {
public:
    explicit Flexible(int flex = 1, FlexFit fit = FlexFit::Loose,
                      SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : ParentDataWidget(std::move(key), std::move(child)), flex_(flex), fit_(fit) {}

    int flex() const { return flex_; }
    void setFlex(int f) { flex_ = f; }

    FlexFit fit() const { return fit_; }
    void setFit(FlexFit f) { fit_ = f; }

    void applyParentData(RenderObject* renderObject) override {
        auto* pd = dynamic_cast<FlexParentData*>(renderObject ? renderObject->parentData() : nullptr);
        if (!pd)
            return;
        pd->flex = flex_;
        pd->fit = fit_;
    }

private:
    int flex_ = 1;
    FlexFit fit_ = FlexFit::Loose;
};

/// 强制填满分配空间的弹性子节点。上游的 `Expanded`（basic.dart）。
///
/// 上游的 `Expanded` 继承 `Flexible`，差别仅在 `fit = FlexFit.tight`。
class Expanded : public Flexible {
public:
    explicit Expanded(int flex = 1, SharedPtr<Widget> child = nullptr,
                      KeyPtr key = nullptr)
        : Flexible(flex, FlexFit::Tight, std::move(child), std::move(key)) {}
};

// ── ColoredBox ──────────────────────────────────────────────────────────────

/// 纯色背景。上游的 `ColoredBox`（basic.dart）。
class ColoredBox : public LeafRenderObjectWidget {
public:
    explicit ColoredBox(Color color, KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)), color_(color) {}

    const Color& color() const { return color_; }
    void setColor(Color c) { color_ = c; }

    RenderObject* createRenderObject() override { return new RenderDecoratedBox(color_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* d = dynamic_cast<RenderDecoratedBox*>(ro))
            d->setColor(color_);
    }

private:
    Color color_;
};

// ── Stack ───────────────────────────────────────────────────────────────────

/// 堆叠布局。上游的 `Stack`（basic.dart）。
///
/// 上游的 `Stack` 继承 `MultiChildRenderObjectWidget`；子节点是否
/// 「参与定位」由 `Positioned` 决定——`Positioned` 通过 ParentData 把
/// top/right/bottom/left/width/height 写进子 RenderObject 的
/// `StackParentData`。
class Stack : public MultiChildRenderObjectWidget {
public:
    using MultiChildRenderObjectWidget::MultiChildRenderObjectWidget;

    explicit Stack(std::vector<SharedPtr<Widget>> children = {}, KeyPtr key = nullptr,
                   RenderStack::Alignment alignment = RenderStack::Alignment::TopStart,
                   RenderStack::Fit fit = RenderStack::Fit::Loose)
        : MultiChildRenderObjectWidget(std::move(key), std::move(children)),
          alignment_(alignment), fit_(fit) {}

    RenderStack::Alignment alignment() const { return alignment_; }
    void setAlignment(RenderStack::Alignment a) { alignment_ = a; }

    RenderStack::Fit fit() const { return fit_; }
    void setFit(RenderStack::Fit f) { fit_ = f; }

    RenderObject* createRenderObject() override {
        return new RenderStack(alignment_, fit_);
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* s = dynamic_cast<RenderStack*>(ro);
        if (!s)
            return;
        s->setAlignment(alignment_);
        s->setFit(fit_);
    }

private:
    RenderStack::Alignment alignment_;
    RenderStack::Fit fit_;
};

// ── Positioned ──────────────────────────────────────────────────────────────

/// 定位子节点。上游的 `Positioned`（basic.dart）。
///
/// 上游的 `Positioned` 继承 `ParentDataWidget<StackParentData>`，
/// 自身不产生 RenderObject，只把定位信息写进子 RenderObject 的
/// `StackParentData`。
///
/// **上游 约束**：至少要给出 left/right/top/bottom/width/height 中的
/// 两个，且水平方向与竖直方向各至少一个，否则 `Positioned` 的父节点
/// 定位信息不完整。这里用 `hasPosition` 标记表达该约束是否满足。
class Positioned : public ParentDataWidget {
public:
    Positioned(SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : ParentDataWidget(std::move(key), std::move(child)) {}

    /// 左边界外缩距离。
    double left = 0.0;
    /// 上边界内缩距离。
    double top = 0.0;
    /// 右边界内缩距离。
    double right = 0.0;
    /// 下边界内缩距离。
    double bottom = 0.0;
    /// 宽（left+right 同时给出时被忽略）。
    double width = 0.0;
    /// 高（top+bottom 同时给出时被忽略）。
    double height = 0.0;

    bool hasLeft = false;
    bool hasTop = false;
    bool hasRight = false;
    bool hasBottom = false;
    bool hasWidth = false;
    bool hasHeight = false;

    /// 水平与竖直方向各至少给出一个定位值。
    bool hasPosition() const {
        int horizontal = (hasLeft ? 1 : 0) + (hasRight ? 1 : 0) + (hasWidth ? 1 : 0);
        int vertical = (hasTop ? 1 : 0) + (hasBottom ? 1 : 0) + (hasHeight ? 1 : 0);
        return horizontal >= 1 && vertical >= 1;
    }

    void applyParentData(RenderObject* renderObject) override {
        auto* pd = dynamic_cast<StackParentData*>(renderObject ? renderObject->parentData() : nullptr);
        if (!pd)
            return;
        pd->left = left;
        pd->top = top;
        pd->right = right;
        pd->bottom = bottom;
        pd->width = width;
        pd->height = height;
        pd->hasLeft = hasLeft;
        pd->hasTop = hasTop;
        pd->hasRight = hasRight;
        pd->hasBottom = hasBottom;
        pd->hasWidth = hasWidth;
        pd->hasHeight = hasHeight;
        pd->positioned = hasPosition();
    }
};

// ── AspectRatio ────────────────────────────────────────────────────────────

/// 固定宽高比。上游的 `AspectRatio`（basic.dart）。
class AspectRatio : public SingleChildRenderObjectWidget {
public:
    explicit AspectRatio(double ratio = 1.0, SharedPtr<Widget> child = nullptr,
                         KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), ratio_(ratio) {}

    double aspectRatio() const { return ratio_; }
    void setAspectRatio(double r) { ratio_ = r; }

    RenderObject* createRenderObject() override { return new RenderAspectRatio(ratio_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderAspectRatio*>(ro))
            r->setAspectRatio(ratio_);
    }

private:
    double ratio_ = 1.0;
};

// ── FractionallySizedBox ───────────────────────────────────────────────────

/// 按比例尺寸。上游的 `FractionallySizedBox`（basic.dart）。
///
/// 因子的语义与 上游 一致：可用最大尺寸 × 因子（nullopt 表示该轴不缩放）。
class FractionallySizedBox : public SingleChildRenderObjectWidget {
public:
    explicit FractionallySizedBox(std::optional<double> widthFactor = std::nullopt,
                                  std::optional<double> heightFactor = std::nullopt,
                                  SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          widthFactor_(widthFactor), heightFactor_(heightFactor) {}

    const std::optional<double>& widthFactor() const { return widthFactor_; }
    void setWidthFactor(std::optional<double> f) { widthFactor_ = f; }

    const std::optional<double>& heightFactor() const { return heightFactor_; }
    void setHeightFactor(std::optional<double> f) { heightFactor_ = f; }

    RenderObject* createRenderObject() override {
        return new RenderFractionallySizedBox(widthFactor_, heightFactor_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderFractionallySizedBox*>(ro)) {
            r->setWidthFactor(widthFactor_);
            r->setHeightFactor(heightFactor_);
        }
    }

private:
    std::optional<double> widthFactor_;
    std::optional<double> heightFactor_;
};

// ── MetaData ───────────────────────────────────────────────────────────────

/// 往命中链上打一个标记。上游的 `MetaData`（basic.dart）。
///
/// 上游的 `metaData` 是 `Object?`（Dart 动态类型），本仓用 `void*`：它是
/// `DragTarget` 用来在命中链里认出自己的唯一消费者（drag_target.h 把
/// `_DragTargetState` 塞进去、再原样取回），没有第二处写入，所以不做类型标签。
class MetaData : public SingleChildRenderObjectWidget {
public:
    explicit MetaData(void* metaData = nullptr,
                      HitTestBehavior behavior = HitTestBehavior::DeferToChild,
                      SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          metaData_(metaData), behavior_(behavior) {}

    /// 上游的 `MetaData.metaData` / `MetaData.behavior`。
    void* metaData() const { return metaData_; }
    void setMetaData(void* v) { metaData_ = v; }
    HitTestBehavior behavior() const { return behavior_; }

    RenderObject* createRenderObject() override {
        return new RenderMetaData(metaData_, behavior_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderMetaData*>(ro)) {
            r->setMetaData(metaData_);
            r->setBehavior(behavior_);
        }
    }

    std::string toStringShort() const override { return "MetaData"; }

private:
    void* metaData_ = nullptr;
    HitTestBehavior behavior_ = HitTestBehavior::DeferToChild;
};

// ── IgnorePointer / AbsorbPointer ──────────────────────────────────────────

/// 屏蔽命中测试。上游的 `IgnorePointer`（basic.dart）。
///
/// ignoring 时自身与整棵子树都不进入命中链，指针事件穿透到下层节点。
class IgnorePointer : public SingleChildRenderObjectWidget {
public:
    explicit IgnorePointer(bool ignoring = true, SharedPtr<Widget> child = nullptr,
                           KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), ignoring_(ignoring) {}

    bool ignoring() const { return ignoring_; }
    void setIgnoring(bool v) { ignoring_ = v; }

    RenderObject* createRenderObject() override {
        return new RenderIgnorePointer(ignoring_, false);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderIgnorePointer*>(ro))
            r->setIgnoring(ignoring_);
    }

private:
    bool ignoring_ = true;
};

/// 吃掉命中。上游的 `AbsorbPointer`（basic.dart）。
///
/// 与 IgnorePointer 的差别：自身**留在**命中链（事件到此为止），
/// 子节点不再参与命中测试。
class AbsorbPointer : public SingleChildRenderObjectWidget {
public:
    explicit AbsorbPointer(bool absorbing = true, SharedPtr<Widget> child = nullptr,
                           KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), absorbing_(absorbing) {}

    bool absorbing() const { return absorbing_; }
    void setAbsorbing(bool v) { absorbing_ = v; }

    RenderObject* createRenderObject() override {
        return new RenderIgnorePointer(!absorbing_, absorbing_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderIgnorePointer*>(ro)) {
            r->setAbsorbing(absorbing_);
            r->setIgnoring(!absorbing_);
        }
    }

private:
    bool absorbing_ = true;
};

// ── Opacity ────────────────────────────────────────────────────────────────

/// 不透明度。上游的 `Opacity`（basic.dart）。
///
/// 不改变布局：自身尺寸等于子节点尺寸，绘制时把子树包进 OpacityLayer。
class Opacity : public SingleChildRenderObjectWidget {
public:
    explicit Opacity(double opacity, SharedPtr<Widget> child = nullptr,
                     KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          opacity_(opacity) {}

    double opacity() const { return opacity_; }
    void setOpacity(double o) { opacity_ = o; }

    RenderObject* createRenderObject() override { return new RenderOpacity(opacity_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderOpacity*>(ro))
            r->setOpacity(opacity_);
    }

private:
    double opacity_;
};

// ── ClipRRect ──────────────────────────────────────────────────────────────

/// 圆角裁剪。上游的 `ClipRRect`（clip.dart）。
///
/// 不改变布局：裁剪区域即自身盒子，绘制时对画布做 round-rect clip。
class ClipRRect : public SingleChildRenderObjectWidget {
public:
    explicit ClipRRect(double borderRadius, SharedPtr<Widget> child = nullptr,
                       KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          radius_(borderRadius) {}

    double borderRadius() const { return radius_; }
    void setBorderRadius(double r) { radius_ = r; }

    RenderObject* createRenderObject() override { return new RenderClipRRect(radius_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderClipRRect*>(ro))
            r->setBorderRadius(radius_);
    }

private:
    double radius_;
};

// ── MouseRegion ────────────────────────────────────────────────────────────

/// 鼠标悬停区域。上游的 `MouseRegion`（widgets/basic.dart，
/// `SingleChildRenderObjectWidget` over `RenderMouseRegion`）。
///
/// 上游签名：`MouseRegion({key, onEnter, onExit, onHover, cursor, opaque, hitTestBehavior, child})`。
/// 本仓按仓库惯例把 `child` 提到第一位（其余保持 上游 的相对次序：
/// onEnter → onExit → onHover → opaque）；`cursor`（平台层无光标抽象）与
/// `hitTestBehavior`（无 `HitTestBehavior` 枚举，上游默认值等价于本仓写死的行为）未接。
///
/// 与 `Listener` 的分工和 上游 一致：Listener 收全部指针事件（含按下/抬起/滚轮），
/// MouseRegion 只关心悬停三件套，并参与进入/离开差分与 `opaque` 遮挡判定。
class MouseRegion : public SingleChildRenderObjectWidget {
public:
    using PointerCallback = RenderMouseRegion::PointerCallback;

    explicit MouseRegion(SharedPtr<Widget> child = nullptr,
                         PointerCallback onEnter = {}, PointerCallback onExit = {},
                         PointerCallback onHover = {}, bool opaque = true,
                         KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          onEnter_(std::move(onEnter)),
          onExit_(std::move(onExit)),
          onHover_(std::move(onHover)),
          opaque_(opaque) {}

    const PointerCallback& onEnter() const { return onEnter_; }
    const PointerCallback& onExit() const { return onExit_; }
    const PointerCallback& onHover() const { return onHover_; }
    bool opaque() const { return opaque_; }

    RenderObject* createRenderObject() override {
        return new RenderMouseRegion(onEnter_, onExit_, onHover_, opaque_);
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderMouseRegion*>(ro);
        if (!r)
            return;
        r->onEnter = onEnter_;
        r->onExit = onExit_;
        r->onHover = onHover_;
        r->setOpaque(opaque_);
    }

    std::string toStringShort() const override { return "MouseRegion"; }

private:
    PointerCallback onEnter_, onExit_, onHover_;
    bool opaque_ = true;
};

}
