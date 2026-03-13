/// @file rendering/render_box_widgets.cpp
/// @brief RenderPadding / RenderConstrainedBox / RenderSizedBox / RenderAlign /
///        RenderStack / RenderDecoratedBox / RenderOpacity / RenderClipRRect /
///        RenderIntrinsicHeight / RenderIntrinsicWidth / RenderParagraph 实现。

#include "cui/rendering/render_box_widgets.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSamplingOptions.h>

#include "cui/foundation/painter.h"
#include "cui/foundation/theme.h"

namespace cui {

namespace {

/// Matrix4 → SkMatrix（取仿射部分，行主序填充）。
SkMatrix toSkMatrix(const Matrix4& m) {
    SkMatrix sk;
    sk.setAll(SkScalar(m.at(0, 0)), SkScalar(m.at(0, 1)), SkScalar(m.at(0, 3)),
              SkScalar(m.at(1, 0)), SkScalar(m.at(1, 1)), SkScalar(m.at(1, 3)),
              SkScalar(m.at(3, 0)), SkScalar(m.at(3, 1)), SkScalar(m.at(3, 3)));
    return sk;
}

/// 上游的 `RenderAligningShiftedBox` 摆放：把子节点按 alignment 放进自身盒子。
Offset alignChildOffset(const Alignment& alignment, const Size& own, const Size& child) {
    return alignment.alongOffset(Offset(own.width - child.width, own.height - child.height));
}

/// WrapAlignment 的自由空间分配：返回首个元素偏移与元素间距。
/// 对齐 上游的 `MainAxisAlignment` 公式。
void distributeFreeSpace(WrapAlignment a, double freeSpace, size_t count, double& offset,
                         double& gap) {
    offset = 0.0;
    gap = 0.0;
    switch (a) {
        case WrapAlignment::Start:
            break;
        case WrapAlignment::End:
            offset = freeSpace;
            break;
        case WrapAlignment::Center:
            offset = freeSpace / 2.0;
            break;
        case WrapAlignment::SpaceBetween:
            gap = count > 1 ? freeSpace / static_cast<double>(count - 1) : 0.0;
            break;
        case WrapAlignment::SpaceAround: {
            const double g = count > 0 ? freeSpace / static_cast<double>(count) : 0.0;
            offset = g / 2.0;
            gap = g;
            break;
        }
        case WrapAlignment::SpaceEvenly: {
            const double g = count > 0 ? freeSpace / static_cast<double>(count + 1) : 0.0;
            offset = g;
            gap = g;
            break;
        }
    }
}

}  // namespace

// ── RenderPadding ───────────────────────────────────────────────────────────

void RenderPadding::performLayout() {
    if (!child()) {
        // 上游：无子节点时尺寸取 padding 本身（clamp 进父约束）。
        setSize(constraints_.constrain(Size(padding_.horizontal(), padding_.vertical())));
        return;
    }
    // 上游：内边距从约束中扣除，子节点拿到 deflate 后的约束。
    BoxConstraints childConstraints = constraints_.deflate(padding_);
    child()->layout(childConstraints);
    Size childSize = child()->size();

    setSize({childSize.width + padding_.horizontal(), childSize.height + padding_.vertical()});

    if (auto* pd = child()->parentData())
        pd->offset = Offset(padding_.left, padding_.top);
}

// 上游的 RenderPadding.compute*Intrinsic*：扣掉本轴方向的 padding 后问子节点，
// 再加回另一轴的 padding。入参为 kInfinity 时减法保持 kInfinity（上游依赖
// double.infinity 的吸收律，本仓的哨兵值必须显式保持）。
double RenderPadding::computeMinIntrinsicWidth(double height) {
    if (!childBox())
        return padding_.horizontal();
    const double inner =
            isBoundedExtent(height) ? std::max(0.0, height - padding_.vertical()) : height;
    return childBox()->getMinIntrinsicWidth(inner) + padding_.horizontal();
}

double RenderPadding::computeMaxIntrinsicWidth(double height) {
    if (!childBox())
        return padding_.horizontal();
    const double inner =
            isBoundedExtent(height) ? std::max(0.0, height - padding_.vertical()) : height;
    return childBox()->getMaxIntrinsicWidth(inner) + padding_.horizontal();
}

double RenderPadding::computeMinIntrinsicHeight(double width) {
    if (!childBox())
        return padding_.vertical();
    const double inner =
            isBoundedExtent(width) ? std::max(0.0, width - padding_.horizontal()) : width;
    return childBox()->getMinIntrinsicHeight(inner) + padding_.vertical();
}

double RenderPadding::computeMaxIntrinsicHeight(double width) {
    if (!childBox())
        return padding_.vertical();
    const double inner =
            isBoundedExtent(width) ? std::max(0.0, width - padding_.horizontal()) : width;
    return childBox()->getMaxIntrinsicHeight(inner) + padding_.vertical();
}

// ── RenderConstrainedBox ────────────────────────────────────────────────────

void RenderConstrainedBox::performLayout() {
    // 上游的 RenderConstrainedBox.performLayout（box.dart）：
    //   有子节点：child.layout(additionalConstraints.enforce(constraints))，
    //             size = child.size；
    //   无子节点：size = additionalConstraints.constrain(constraints.biggest)。
    if (!child()) {
        // 上游：_additionalConstraints.enforce(constraints).constrain(Size.zero)。
        // 先把附加约束 clamp 进父约束（父级紧约束优先），再取满足约束的尺寸。
        // 之前漏了 enforce，导致 additional 压过父级紧约束（SizedBox(0,0) 在
        // tight(100,100) 下错误地得到 0×0）。
        setSize(additional_.enforce(constraints_).constrain(Size(0, 0)));
        return;
    }
    // 上游：附加约束与父约束取交集（enforce，clamp 语义）。
    BoxConstraints childConstraints = additional_.enforce(constraints_);
    child()->layout(childConstraints);
    setSize(child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

// 上游的 RenderConstrainedBox.compute*Intrinsic*。附加约束在该轴已经是
// 「有界紧约束」时父约束无从改变它，直接取该值；否则把子节点的 intrinsic
// 过一遍附加约束；该轴完全无界时原样透传。
double RenderConstrainedBox::computeMinIntrinsicWidth(double height) {
    if (additional_.hasBoundedWidth() && additional_.hasTightWidth())
        return additional_.minWidth;
    const double width = RenderProxyBox::computeMinIntrinsicWidth(height);
    if (!additional_.hasInfiniteWidth())
        return additional_.constrainWidth(width);
    return width;
}

double RenderConstrainedBox::computeMaxIntrinsicWidth(double height) {
    if (additional_.hasBoundedWidth() && additional_.hasTightWidth())
        return additional_.minWidth;
    const double width = RenderProxyBox::computeMaxIntrinsicWidth(height);
    if (!additional_.hasInfiniteWidth())
        return additional_.constrainWidth(width);
    return width;
}

double RenderConstrainedBox::computeMinIntrinsicHeight(double width) {
    if (additional_.hasBoundedHeight() && additional_.hasTightHeight())
        return additional_.minHeight;
    const double height = RenderProxyBox::computeMinIntrinsicHeight(width);
    if (!additional_.hasInfiniteHeight())
        return additional_.constrainHeight(height);
    return height;
}

double RenderConstrainedBox::computeMaxIntrinsicHeight(double width) {
    if (additional_.hasBoundedHeight() && additional_.hasTightHeight())
        return additional_.minHeight;
    const double height = RenderProxyBox::computeMaxIntrinsicHeight(width);
    if (!additional_.hasInfiniteHeight())
        return additional_.constrainHeight(height);
    return height;
}

// ── RenderIntrinsicHeight / RenderIntrinsicWidth（上游 proxy_box.dart）──────

BoxConstraints RenderIntrinsicHeight::childConstraints(
        RenderBox* child, const BoxConstraints& constraints) const {
    // 上游：约束已给紧高度时原样下发，否则把高度收紧为子节点在该宽度下的
    // 最大 intrinsic 高。
    if (constraints.hasTightHeight())
        return constraints;
    return constraints.tighten(std::nullopt, child->getMaxIntrinsicHeight(constraints.maxWidth));
}

void RenderIntrinsicHeight::performLayout() {
    if (!childBox()) {
        setSize(constraints_.smallest());
        return;
    }
    childBox()->layout(childConstraints(childBox(), constraints_));
    setSize(childBox()->size());
}

double RenderIntrinsicHeight::computeMinIntrinsicWidth(double height) {
    if (!childBox())
        return 0.0;
    // 上游：请求方没给有限高度时，先用子节点的最大 intrinsic 高代替，
    // 否则「在无界高度下量宽度」是问不出来的。
    double effective = height;
    if (!isBoundedExtent(effective))
        effective = childBox()->getMaxIntrinsicHeight(BoxConstraints::kInfinity);
    return childBox()->getMinIntrinsicWidth(effective);
}

double RenderIntrinsicHeight::computeMaxIntrinsicWidth(double height) {
    if (!childBox())
        return 0.0;
    double effective = height;
    if (!isBoundedExtent(effective))
        effective = childBox()->getMaxIntrinsicHeight(BoxConstraints::kInfinity);
    return childBox()->getMaxIntrinsicWidth(effective);
}

double RenderIntrinsicHeight::computeMinIntrinsicHeight(double width) {
    // 上游：本盒一旦确定了宽度，子节点的高度就固定为最大 intrinsic 高，
    // 故 min == max。
    return getMaxIntrinsicHeight(width);
}

double RenderIntrinsicWidth::applyStep(double input, const std::optional<double>& step) {
    if (!step.has_value() || step.value() <= 0.0)
        return input;
    return std::ceil(input / step.value()) * step.value();
}

BoxConstraints RenderIntrinsicWidth::childConstraints(
        RenderBox* child, const BoxConstraints& constraints) const {
    // 上游的 _childConstraints：宽度轴已有紧约束就尊重它，否则按子节点的
    // 最大 intrinsic 宽（过 stepWidth）收紧；高度轴只在给出 stepHeight 时
    // 收紧（同样按最大 intrinsic 高过 stepHeight）。
    std::optional<double> width;
    if (!constraints.hasTightWidth())
        width = applyStep(child->getMaxIntrinsicWidth(constraints.maxHeight), stepWidth_);
    std::optional<double> height;
    if (stepHeight_.has_value() && stepHeight_.value() > 0.0)
        height = applyStep(child->getMaxIntrinsicHeight(constraints.maxWidth), stepHeight_);
    return constraints.tighten(width, height);
}

void RenderIntrinsicWidth::performLayout() {
    if (!childBox()) {
        setSize(constraints_.smallest());
        return;
    }
    childBox()->layout(childConstraints(childBox(), constraints_));
    setSize(childBox()->size());
}

double RenderIntrinsicWidth::computeMinIntrinsicWidth(double height) {
    return getMaxIntrinsicWidth(height);
}

double RenderIntrinsicWidth::computeMaxIntrinsicWidth(double height) {
    if (!childBox())
        return 0.0;
    return applyStep(childBox()->getMaxIntrinsicWidth(height), stepWidth_);
}

double RenderIntrinsicWidth::computeMinIntrinsicHeight(double width) {
    if (!childBox())
        return 0.0;
    double effective = width;
    if (!isBoundedExtent(effective))
        effective = getMaxIntrinsicWidth(BoxConstraints::kInfinity);
    return applyStep(childBox()->getMinIntrinsicHeight(effective), stepHeight_);
}

double RenderIntrinsicWidth::computeMaxIntrinsicHeight(double width) {
    if (!childBox())
        return 0.0;
    double effective = width;
    if (!isBoundedExtent(effective))
        effective = getMaxIntrinsicWidth(BoxConstraints::kInfinity);
    return applyStep(childBox()->getMaxIntrinsicHeight(effective), stepHeight_);
}

// ── RenderAlign ─────────────────────────────────────────────────────────────

void RenderAlign::performLayout() {
    constexpr double kInf = BoxConstraints::kInfinity;
    // 上游的 `RenderPositionedBox.performLayout`（rendering/shifted_box.dart）：
    //   shrinkWrapWidth  = widthFactor != null || maxWidth == infinity
    //   shrinkWrapHeight = heightFactor != null || maxHeight == infinity
    // 因子非空时该轴按「子尺寸 × 因子」收缩；否则有界方向撑满上限
    // （constrain(∞) → max）、无界方向收缩到子节点尺寸。
    const bool shrinkWrapWidth = widthFactor_.has_value() || !constraints_.hasBoundedWidth();
    const bool shrinkWrapHeight = heightFactor_.has_value() || !constraints_.hasBoundedHeight();

    if (!child()) {
        setSize(constraints_.constrain(
                Size(shrinkWrapWidth ? 0.0 : kInf, shrinkWrapHeight ? 0.0 : kInf)));
        return;
    }
    // 上游：child.layout(constraints.loosen(), parentUsesSize: true)。
    child()->layout(constraints_.loosen());
    Size childSize = child()->size();

    double w = shrinkWrapWidth ? childSize.width * widthFactor_.value_or(1.0) : kInf;
    double h = shrinkWrapHeight ? childSize.height * heightFactor_.value_or(1.0) : kInf;
    setSize(constraints_.constrain(Size(w, h)));

    // 对齐偏移：alignment ∈ [-1, 1]，0 为居中（上游的 alignChild()）。
    double dx = (size_.width - childSize.width) / 2.0 * (1.0 + alignmentX_);
    double dy = (size_.height - childSize.height) / 2.0 * (1.0 + alignmentY_);
    if (auto* pd = child()->parentData())
        pd->offset = Offset(dx, dy);
}

// 上游的 RenderPositionedBox.compute*Intrinsic*：转发给子节点再乘本轴因子。
// 未给因子时因子按 1 计——注意这与 performLayout 的「有界方向撑满上限」并不
// 等价（intrinsic 无法表达「占满父约束」），上游同样如此。
double RenderAlign::computeMinIntrinsicWidth(double height) {
    return RenderProxyBox::computeMinIntrinsicWidth(height) * widthFactor_.value_or(1.0);
}

double RenderAlign::computeMaxIntrinsicWidth(double height) {
    return RenderProxyBox::computeMaxIntrinsicWidth(height) * widthFactor_.value_or(1.0);
}

double RenderAlign::computeMinIntrinsicHeight(double width) {
    return RenderProxyBox::computeMinIntrinsicHeight(width) * heightFactor_.value_or(1.0);
}

double RenderAlign::computeMaxIntrinsicHeight(double width) {
    return RenderProxyBox::computeMaxIntrinsicHeight(width) * heightFactor_.value_or(1.0);
}

// ── RenderStack ─────────────────────────────────────────────────────────────

void RenderStack::setupParentData(RenderObject* child) {
    // 上游的 RenderStack.setupParentData：创建 StackParentData。
    child->setParentData(new StackParentData());
}

void RenderStack::setPositioned(size_t index, bool positioned) {
    const auto& list = children();
    if (index >= list.size() || !list[index])
        return;
    if (auto* pd = dynamic_cast<StackParentData*>(list[index]->parentData()))
        pd->positioned = positioned;
    markNeedsLayout();
}

bool RenderStack::layoutPositionedChild(RenderObject* child, StackParentData* pd,
                                        const Size& stackSize, Alignment alignment) {
    // 上游的 layoutPositionedChild（render_stack.dart:539-576）。
    // 1. 按定位信息算出子节点的紧约束。
    std::optional<double> w;
    if (pd->hasLeft && pd->hasRight)
        w = stackSize.width - pd->right - pd->left;
    else if (pd->hasWidth)
        w = pd->width;

    std::optional<double> h;
    if (pd->hasTop && pd->hasBottom)
        h = stackSize.height - pd->bottom - pd->top;
    else if (pd->hasHeight)
        h = pd->height;

    BoxConstraints childConstraints = BoxConstraints::tightFor(
            w ? std::optional<double>(std::max(0.0, *w)) : std::nullopt,
            h ? std::optional<double>(std::max(0.0, *h)) : std::nullopt);
    child->layout(childConstraints);

    // 2. 按 left/right（top/bottom）算出偏移；都没有则按 alignment 摆放。
    double dx = 0.0;
    if (pd->hasLeft)
        dx = pd->left;
    else if (pd->hasRight)
        dx = stackSize.width - pd->right - child->size().width;
    else {
        double alignX = 0.0;
        switch (alignment) {
            case Alignment::TopStart:      alignX = 0.0; break;
            case Alignment::TopCenter:     alignX = 0.5; break;
            case Alignment::TopEnd:        alignX = 1.0; break;
            case Alignment::MiddleStart:   alignX = 0.0; break;
            case Alignment::MiddleCenter:  alignX = 0.5; break;
            case Alignment::MiddleEnd:     alignX = 1.0; break;
            case Alignment::BottomStart:   alignX = 0.0; break;
            case Alignment::BottomCenter:  alignX = 0.5; break;
            case Alignment::BottomEnd:     alignX = 1.0; break;
        }
        dx = (stackSize.width - child->size().width) * alignX;
    }

    double dy = 0.0;
    if (pd->hasTop)
        dy = pd->top;
    else if (pd->hasBottom)
        dy = stackSize.height - pd->bottom - child->size().height;
    else {
        double alignY = 0.0;
        switch (alignment) {
            case Alignment::TopStart:      alignY = 0.0; break;
            case Alignment::TopCenter:     alignY = 0.0; break;
            case Alignment::TopEnd:        alignY = 0.0; break;
            case Alignment::MiddleStart:   alignY = 0.5; break;
            case Alignment::MiddleCenter:  alignY = 0.5; break;
            case Alignment::MiddleEnd:     alignY = 0.5; break;
            case Alignment::BottomStart:   alignY = 1.0; break;
            case Alignment::BottomCenter:  alignY = 1.0; break;
            case Alignment::BottomEnd:     alignY = 1.0; break;
        }
        dy = (stackSize.height - child->size().height) * alignY;
    }

    pd->offset = Offset(dx, dy);

    // 越界标记（上游的 _hasVisualOverflow）。
    return dx < 0.0 || dx + child->size().width > stackSize.width ||
           dy < 0.0 || dy + child->size().height > stackSize.height;
}

void RenderStack::performLayout() {
    const auto& list = children();
    if (list.empty()) {
        // 上游：无子节点时取 biggest（有限）或 smallest。
        setSize(constraints_.hasBoundedWidth() && constraints_.hasBoundedHeight()
                        ? constraints_.biggest()
                        : constraints_.smallest());
        return;
    }

    // 非 positioned 子节点的约束（上游的 _computeSize）。
    BoxConstraints nonPositionedConstraints;
    switch (fit_) {
        case Fit::Loose:      nonPositionedConstraints = constraints_.loosen(); break;
        case Fit::Expand:     nonPositionedConstraints = BoxConstraints::tight(constraints_.biggest()); break;
        case Fit::Passthrough: nonPositionedConstraints = constraints_; break;
    }

    // 1. 先测量所有非 positioned 子节点，算出 Stack 自身尺寸。
    double width = constraints_.minWidth;
    double height = constraints_.minHeight;
    bool hasNonPositionedChildren = false;
    for (RenderObject* child : list) {
        if (!child)
            continue;
        auto* pd = dynamic_cast<StackParentData*>(child->parentData());
        if (pd && pd->isPositioned())
            continue;
        hasNonPositionedChildren = true;
        child->layout(nonPositionedConstraints);
        width = std::max(width, child->size().width);
        height = std::max(height, child->size().height);
    }

    Size stackSize;
    if (hasNonPositionedChildren) {
        stackSize = Size(constraints_.constrainWidth(width), constraints_.constrainHeight(height));
    } else {
        // 全部是 positioned 子节点时，Stack 取父约束的 biggest。
        stackSize = constraints_.biggest();
    }
    setSize(stackSize);

    // 2. 再摆放所有子节点。
    double alignX = 0.0;
    double alignY = 0.0;
    switch (alignment_) {
        case Alignment::TopStart:      alignX = 0.0;   alignY = 0.0;   break;
        case Alignment::TopCenter:     alignX = 0.5;   alignY = 0.0;   break;
        case Alignment::TopEnd:        alignX = 1.0;   alignY = 0.0;   break;
        case Alignment::MiddleStart:   alignX = 0.0;   alignY = 0.5;   break;
        case Alignment::MiddleCenter:  alignX = 0.5;   alignY = 0.5;   break;
        case Alignment::MiddleEnd:     alignX = 1.0;   alignY = 0.5;   break;
        case Alignment::BottomStart:   alignX = 0.0;   alignY = 1.0;   break;
        case Alignment::BottomCenter:  alignX = 0.5;   alignY = 1.0;   break;
        case Alignment::BottomEnd:     alignX = 1.0;   alignY = 1.0;   break;
    }

    for (RenderObject* child : list) {
        if (!child)
            continue;
        auto* pd = dynamic_cast<StackParentData*>(child->parentData());
        if (pd && pd->isPositioned()) {
            layoutPositionedChild(child, pd, stackSize, alignment_);
            continue;
        }
        Size cs = child->size();
        if (pd)
            pd->offset = Offset((stackSize.width - cs.width) * alignX,
                                (stackSize.height - cs.height) * alignY);
    }
}

// 上游的 RenderStack.getIntrinsicDimension：跳过 positioned 子节点（它们的
// 尺寸由 Stack 自身尺寸反推，无法参与 intrinsic 计算），取其余子节点的最大值。
double RenderStack::intrinsicDimension(const std::vector<RenderObject*>& children,
                                      const std::function<double(RenderBox*)>& getter) {
    double extent = 0.0;
    for (RenderObject* child : children) {
        if (!child)
            continue;
        auto* pd = dynamic_cast<StackParentData*>(child->parentData());
        if (pd && pd->isPositioned())
            continue;
        extent = std::max(extent, getter(static_cast<RenderBox*>(child)));
    }
    return extent;
}

double RenderStack::computeMinIntrinsicWidth(double height) {
    return intrinsicDimension(children(),
                              [height](RenderBox* c) { return c->getMinIntrinsicWidth(height); });
}

double RenderStack::computeMaxIntrinsicWidth(double height) {
    return intrinsicDimension(children(),
                              [height](RenderBox* c) { return c->getMaxIntrinsicWidth(height); });
}

double RenderStack::computeMinIntrinsicHeight(double width) {
    return intrinsicDimension(children(),
                              [width](RenderBox* c) { return c->getMinIntrinsicHeight(width); });
}

double RenderStack::computeMaxIntrinsicHeight(double width) {
    return intrinsicDimension(children(),
                              [width](RenderBox* c) { return c->getMaxIntrinsicHeight(width); });
}

// ── RenderDecoratedBox ──────────────────────────────────────────────────────

void RenderDecoratedBox::performLayout() {
    // 无子节点时是叶子盒：尺寸取约束的最小值（上游的
    // RenderBox.performResize：size = constraints.smallest()）。
    // 注意：若取 biggest()，作为 Expanded 子节点时会把交叉轴撑到上限，
    // 导致父 Row/Column 的交叉轴尺寸被错误放大。
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    // 有子节点（Container 组合出的 DecoratedBox）：尺寸 = 子节点尺寸。
    child()->layout(constraints_);
    setSize(child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderDecoratedBox::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c || size_.width <= 0 || size_.height <= 0)
        return;

    SkRect rect = SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                                   SkScalar(size_.width), SkScalar(size_.height));
    SkRRect rr = SkRRect::MakeRectXY(rect, SkScalar(radius_), SkScalar(radius_));

    // 第二层阴影（ambient）：更柔和、偏移更小，先于 spot 阴影绘制（在更底层）。
    if (elevation2_ > 0.0 && shadowColor2_.a > 0.0) {
        SkPaint shadow;
        shadow.setAntiAlias(true);
        float alpha = static_cast<float>(
            shadowColor2_.a * std::min(1.0, elevation2_ / 6.0));
        shadow.setColor4f(SkColor4f{static_cast<float>(shadowColor2_.r),
                                    static_cast<float>(shadowColor2_.g),
                                    static_cast<float>(shadowColor2_.b), alpha},
                          nullptr);
        shadow.setStyle(SkPaint::kFill_Style);
        shadow.setMaskFilter(SkMaskFilter::MakeBlur(
                kNormal_SkBlurStyle, SkScalar(elevation2_ * 2.0)));
        SkRRect srr = SkRRect::MakeRectXY(
                rect.makeOffset(0, SkScalar(std::max(elevation2_ * 0.5, 1.0))),
                SkScalar(radius_), SkScalar(radius_));
        c->drawRRect(srr, shadow);
    }

    // elevation 阴影（spot）：模糊 RRect，向下偏移（Material 阴影近似）。
    if (elevation_ > 0.0 && shadowColor_.a > 0.0) {
        SkPaint shadow;
        shadow.setAntiAlias(true);
        float alpha = static_cast<float>(
            shadowColor_.a * std::min(1.0, elevation_ / 6.0));
        shadow.setColor4f(SkColor4f{static_cast<float>(shadowColor_.r),
                                    static_cast<float>(shadowColor_.g),
                                    static_cast<float>(shadowColor_.b), alpha},
                          nullptr);
        shadow.setStyle(SkPaint::kFill_Style);
        shadow.setMaskFilter(SkMaskFilter::MakeBlur(
                kNormal_SkBlurStyle, SkScalar(elevation_ * 2.0)));
        SkRRect srr = SkRRect::MakeRectXY(
                rect.makeOffset(0, SkScalar(elevation_ * 0.8)), SkScalar(radius_),
                SkScalar(radius_));
        c->drawRRect(srr, shadow);
    }

    if (color_.a > 0.0) {
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor4f(SkColor4f{static_cast<float>(color_.r), static_cast<float>(color_.g),
                                   static_cast<float>(color_.b), static_cast<float>(color_.a)},
                         nullptr);
        paint.setStyle(SkPaint::kFill_Style);
        c->drawRRect(rr, paint);
    }
    if (borderWidth_ > 0.0 && borderColor_.a > 0.0) {
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor4f(
                SkColor4f{static_cast<float>(borderColor_.r), static_cast<float>(borderColor_.g),
                          static_cast<float>(borderColor_.b), static_cast<float>(borderColor_.a)},
                nullptr);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(SkScalar(borderWidth_));
        c->drawRRect(rr, paint);
    }

    // 装饰是背景：画完再画子节点（代理盒默认透传）。
    if (child())
        RenderProxyBox::performPaint(context, offset);
}

// ── RenderAspectRatio ───────────────────────────────────────────────────────

void RenderAspectRatio::performLayout() {
    // 上游的 RenderAspectRatio.performLayout（render_box 的 layout helper）。
    Size size;
    if (constraints_.isTight()) {
        size = constraints_.smallest();
    } else if (constraints_.hasBoundedWidth()) {
        double w = constraints_.constrainWidth(constraints_.maxWidth);
        double h = w / ratio_;
        if (constraints_.hasBoundedHeight() && h > constraints_.maxHeight) {
            h = constraints_.maxHeight;
            w = h * ratio_;
        }
        size = Size(w, h);
    } else if (constraints_.hasBoundedHeight()) {
        double h = constraints_.constrainHeight(constraints_.maxHeight);
        size = Size(h * ratio_, h);
    } else {
        size = constraints_.smallest();
    }
    // 最终再 clamp 回约束（上游的 constraints.constrain）。
    size = constraints_.constrain(size);
    setSize(size);

    if (child()) {
        child()->layout(BoxConstraints::tight(size));
        if (auto* pd = child()->parentData())
            pd->offset = Offset(0, 0);
    }
}

// ── RenderFractionallySizedBox ──────────────────────────────────────────────

void RenderFractionallySizedBox::performLayout() {
    // 上游：把父约束按比例缩放后交给子节点（其余轴沿用父约束）。
    double minW = widthFactor_ ? constraints_.minWidth * *widthFactor_ : constraints_.minWidth;
    double maxW = widthFactor_ ? constraints_.maxWidth * *widthFactor_ : constraints_.maxWidth;
    double minH = heightFactor_ ? constraints_.minHeight * *heightFactor_ : constraints_.minHeight;
    double maxH = heightFactor_ ? constraints_.maxHeight * *heightFactor_ : constraints_.maxHeight;

    BoxConstraints childConstraints(minW, maxW, minH, maxH);
    if (child()) {
        child()->layout(childConstraints);
        setSize(child()->size());
        if (auto* pd = child()->parentData())
            pd->offset = Offset(0, 0);
    } else {
        setSize(constraints_.constrain(Size(maxW, maxH)));
    }
}

// ── RenderIgnorePointer ─────────────────────────────────────────────────────

bool RenderIgnorePointer::hitTestSelf(const Offset& position) const {
    (void)position;
    // 上游 `RenderIgnorePointer.hitTestSelf => absorbing`：只有 AbsorbPointer
    // 吃自身命中。非 absorbing 时它必须是**完全透明**的代理盒，命中与否全看
    // 子树——否则链会在它这里截断（ModalScope 用它包路由页，页面收不到点击）。
    if (ignoring_)
        return false;
    return absorbing_;
}

bool RenderIgnorePointer::hitTest(const Offset& position, std::vector<RenderObject*>& result) {
    if (ignoring_)
        return false;
    const bool inBounds = position.dx >= 0 && position.dy >= 0 &&
                          position.dx < size_.width && position.dy < size_.height;
    if (!inBounds)
        return false;
    if (absorbing_) {
        result.push_back(this);
        return true;
    }
    // 上游走 `super.hitTest`：子树命中才算命中，并把自己作为注解记进链
    // （本仓链序是「根 → … → target」，故插在子树条目之前）。
    const size_t start = result.size();
    if (!hitTestChildren(position, result))
        return false;
    result.insert(result.begin() + static_cast<std::ptrdiff_t>(start), this);
    return true;
}

// ── RenderMouseRegion ───────────────────────────────────────────────────────

void RenderMouseRegion::handleEvent(const PointerEvent& event,
                                    const HitTestEntry& entry) {
    (void)entry;
    // 上游只有 onHover 走这条路径（PointerHoverEvent）；onEnter/onExit 由
    // MouseTracker 直调。本仓的 Enter/Exit 是 PipelineOwner 差分后派发到链上
    // 的事件，形态与上游一致（本地坐标已按节点变换），故一并在此回应。
    switch (event.type) {
        case PointerEventType::Enter:
            if (onEnter)
                onEnter(event);
            break;
        case PointerEventType::Exit:
            if (onExit)
                onExit(event);
            break;
        case PointerEventType::Hover:
            if (onHover)
                onHover(event);
            break;
        default:
            break;
    }
}

// ── RenderOpacity ───────────────────────────────────────────────────────────

void RenderOpacity::performLayout() {
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    child()->layout(constraints_);
    setSize(child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderOpacity::performPaint(PaintingContext& context, const Offset& offset) {
    if (!child())
        return;
    // 上游的 RenderOpacity.paint：alpha==0 时 offstage（不绘制），
    // 否则 pushOpacity 把子树包进 OpacityLayer。
    if (opacity_ <= 0.0)
        return;
    // 关键：本地绘制走 paintDirect —— 它在**已平移**的画布上以 offset={} 调用
    // performPaint，而 pushOpacity 会把子树录制到**全新**的画布并挂到
    // containerLayer（未累计父级平移）。因此必须把累加偏移 currentOffset() 补上，
    // 换算到 containerLayer 空间，否则子树会被画到窗口原点 (0,0)。
    context.pushOpacity(opacity_, child(), context.currentOffset() + offset);
}

// ── RenderClipRRect ─────────────────────────────────────────────────────────

void RenderClipRRect::performLayout() {
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    child()->layout(constraints_);
    setSize(child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderClipRRect::performPaint(PaintingContext& context, const Offset& offset) {
    if (!child())
        return;
    PaintingContext::CanvasScope scope(context);
    if (!scope.active()) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    SkRRect rr = SkRRect::MakeRectXY(
            SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                             SkScalar(size_.width), SkScalar(size_.height)),
            SkScalar(radius_), SkScalar(radius_));
    scope.clipRRect(rr, true);
    context.paintChild(child(), offset);
}

// ── RenderClipRect / RenderClipOval ──────────────────────────────────────────

void RenderClipRect::performLayout() {
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    child()->layout(constraints_);
    setSize(child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderClipRect::performPaint(PaintingContext& context, const Offset& offset) {
    if (!child())
        return;
    PaintingContext::CanvasScope scope(context);
    if (!scope.active()) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    scope.clipRect(SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                                    SkScalar(size_.width), SkScalar(size_.height)),
                   antiAlias_);
    context.paintChild(child(), offset);
}

void RenderClipOval::performLayout() {
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    child()->layout(constraints_);
    setSize(child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderClipOval::performPaint(PaintingContext& context, const Offset& offset) {
    if (!child())
        return;
    PaintingContext::CanvasScope scope(context);
    if (!scope.active()) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    scope.clipRRect(SkRRect::MakeOval(
                        SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                                         SkScalar(size_.width), SkScalar(size_.height))),
                    antiAlias_);
    context.paintChild(child(), offset);
}

// ── RenderTransform ─────────────────────────────────────────────────────────

Matrix4 RenderTransform::effectiveTransform() const {
    // 上游的 `_effectiveTransform`（rendering/proxy_box.dart）：先按 origin
    // 平移，再叠加 alignment.alongSize(size)，然后乘变换矩阵、按同样顺序回移。
    // 两段都是纯平移，合并成「绕中心点 o」的 T(o)·M·T(-o)，其中
    //   o = origin + alignment.alongSize(size)。
    const Offset center =
            origin_ + (alignment_ ? alignment_->alongSize(size_) : Offset());
    Matrix4 result = Matrix4::identity();
    if (center != Offset()) {
        result.translate(center.dx, center.dy);
        result.multiply(transform_);
        result.translate(-center.dx, -center.dy);
    } else {
        result.multiply(transform_);
    }
    return result;
}

void RenderTransform::performLayout() {
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    child()->layout(constraints_);
    setSize(child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderTransform::performPaint(PaintingContext& context, const Offset& offset) {
    if (!child() || transform_.isIdentity()) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    PaintingContext::CanvasScope scope(context);
    if (!scope.active()) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    // T(offset)·M·T(-offset)，与 pushBoxTransform 同序。
    scope.translate(offset.dx, offset.dy);
    scope.concat(toSkMatrix(effectiveTransform()));
    scope.translate(-offset.dx, -offset.dy);
    RenderProxyBox::performPaint(context, offset);
}

bool RenderTransform::hitTest(const Offset& position, std::vector<RenderObject*>& result) {
    if (!transformHitTests_ || transform_.isIdentity())
        return RenderBox::hitTest(position, result);
    // 把父坐标系的点逆变换回子节点坐标系（上游的
    // `MatrixUtils.transformPoint(_effectiveTransformInverse, position)`）。
    const Offset local = effectiveTransform().inverse().transformOffset(position);
    return RenderBox::hitTest(local, result);
}

// ── RenderFractionalTranslation ────────────────────────────────────────────

void RenderFractionalTranslation::performLayout() {
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    child()->layout(constraints_);
    setSize(child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderFractionalTranslation::performPaint(PaintingContext& context, const Offset& offset) {
    if (!child()) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    // 平移量按子节点自身尺寸的比例计算（上游语义）。
    context.paintChild(child(), offset + translationOffset());
}

bool RenderFractionalTranslation::hitTest(const Offset& position,
                                          std::vector<RenderObject*>& result) {
    // 上游的 `RenderFractionalTranslation.hitTestChildren`：
    // transformHitTests 为 true 时先用 addWithPaintOffset 把点回移绘制偏移，
    // 再测子节点；为 false 时按未平移的位置测。
    const Offset local = transformHitTests_ ? position - translationOffset() : position;
    return RenderBox::hitTest(local, result);
}

// ── RenderRotatedBox ────────────────────────────────────────────────────────

void RenderRotatedBox::performLayout() {
    paintTransform_ = Matrix4::identity();
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    // 上游的 `_rotateConstraints`：奇数个 90° 时约束翻轴。
    const BoxConstraints childConstraints =
            (quarterTurns_ % 2 == 1) ? constraints_.flipped() : constraints_;
    child()->layout(childConstraints);
    const Size childSize = child()->size();
    // 上游的 `_rotateSize`：奇数个 90° 时尺寸翻轴。
    setSize((quarterTurns_ % 2 == 1) ? Size(childSize.height, childSize.width) : childSize);
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);

    // 上游的 `_setUpPaintTransform`：先平移到旋转后的落点，再旋转。
    switch (quarterTurns_ % 4) {
        case 1:
            paintTransform_.translate(size_.height, 0.0);
            break;
        case 2:
            paintTransform_.translate(size_.width, size_.height);
            break;
        case 3:
            paintTransform_.translate(0.0, size_.width);
            break;
        default:
            break;
    }
    if (quarterTurns_ % 4 != 0)
        paintTransform_.rotateZ(quarterTurns_ * M_PI / 2.0);
}

void RenderRotatedBox::performPaint(PaintingContext& context, const Offset& offset) {
    if (!child() || quarterTurns_ % 4 == 0) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    PaintingContext::CanvasScope scope(context);
    if (!scope.active()) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    scope.translate(offset.dx, offset.dy);
    scope.concat(toSkMatrix(paintTransform_));
    scope.translate(-offset.dx, -offset.dy);
    RenderProxyBox::performPaint(context, offset);
}

// ── RenderFittedBox ─────────────────────────────────────────────────────────

void RenderFittedBox::performLayout() {
    transform_ = Matrix4::identity();
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    // 上游：子节点在**完全无约束**下测量自然尺寸。
    child()->layout(BoxConstraints::unbounded());
    const Size childSize = child()->size();

    // scaleDown 走 loosen 后的约束，其余走原约束。
    setSize(fit_ == BoxFit::ScaleDown
                ? constraints_.loosen().constrainSizeAndAttemptToPreserveAspectRatio(childSize)
                : constraints_.constrainSizeAndAttemptToPreserveAspectRatio(childSize));

    const FittedSizes sizes = applyBoxFit(fit_, childSize, size_);
    const double scaleX = sizes.source.width > 0.0 ? sizes.destination.width / sizes.source.width
                                                   : 0.0;
    const double scaleY = sizes.source.height > 0.0
                              ? sizes.destination.height / sizes.source.height
                              : 0.0;
    // 上游：sourceRect / destinationRect 都由 alignment.inscribe 决定。
    const Rect sourceRect =
            alignment_.inscribe(sizes.source, Rect(0, 0, childSize.width, childSize.height));
    const Rect destinationRect =
            alignment_.inscribe(sizes.destination, Rect(0, 0, size_.width, size_.height));

    transform_ = Matrix4::identity();
    transform_.translate(destinationRect.x, destinationRect.y);
    transform_.scaleBy(scaleX, scaleY);
    transform_.translate(-sourceRect.x, -sourceRect.y);

    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderFittedBox::performPaint(PaintingContext& context, const Offset& offset) {
    if (!child())
        return;
    if (!context.canvas() || transform_.isIdentity()) {
        RenderProxyBox::performPaint(context, offset);
        return;
    }
    PaintingContext::CanvasScope scope(context);
    // 子节点是在无约束下测量的，可能超出自身盒子（BoxFit.cover 必然如此），
    // 与 上游的 `clipBehavior` 对应：默认裁到自身矩形。
    if (clipContent_)
        scope.clipRect(SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                                        SkScalar(size_.width), SkScalar(size_.height)),
                       true);
    scope.translate(offset.dx, offset.dy);
    scope.concat(toSkMatrix(transform_));
    scope.translate(-offset.dx, -offset.dy);
    RenderProxyBox::performPaint(context, offset);
}

// ── RenderBaseline ──────────────────────────────────────────────────────────

void RenderBaseline::performLayout() {
    if (!child()) {
        setSize(Size(constraints_.constrainWidth(0.0), constraints_.constrainHeight(baseline_)));
        return;
    }
    // 上游：子节点在松约束下布局。
    child()->layout(constraints_.loosen());
    const Size childSize = child()->size();

    double childBaseline = -1.0;
    if (auto* box = dynamic_cast<RenderBox*>(child()))
        childBaseline = box->getDistanceToBaseline(baselineType_);
    // 没有基线信息时退化为「底边即基线」（上游 会 assert，这里容错）。
    if (childBaseline < 0.0)
        childBaseline = childSize.height;

    const double top = baseline_ - childBaseline;
    setSize(Size(constraints_.constrainWidth(childSize.width),
                 constraints_.constrainHeight(std::max(0.0, top) + childSize.height)));
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0.0, top);
}

// ── RenderLimitedBox ────────────────────────────────────────────────────────

void RenderLimitedBox::performLayout() {
    if (!child()) {
        setSize(constraints_.constrain(Size()));
        return;
    }
    // 只在父约束**无界**时用自身上限顶住；有界时原样透传。
    const double maxW = constraints_.hasBoundedWidth() ? constraints_.maxWidth : maxWidth_;
    const double maxH = constraints_.hasBoundedHeight() ? constraints_.maxHeight : maxHeight_;
    child()->layout(BoxConstraints(0.0, maxW, 0.0, maxH));
    setSize(constraints_.constrain(child()->size()));
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

// ── RenderOverflowBox ───────────────────────────────────────────────────────

void RenderOverflowBox::performLayout() {
    // 自身尺寸 = 父约束的 biggest（在父节点看来占满可用空间）。
    setSize(constraints_.biggest());
    if (!child())
        return;
    BoxConstraints inner(minWidth_.value_or(constraints_.minWidth),
                         maxWidth_.value_or(constraints_.maxWidth),
                         minHeight_.value_or(constraints_.minHeight),
                         maxHeight_.value_or(constraints_.maxHeight));
    child()->layout(inner);
    if (auto* pd = child()->parentData())
        pd->offset = alignChildOffset(alignment_, size_, child()->size());
}

// ── RenderSizedOverflowBox ──────────────────────────────────────────────────

void RenderSizedOverflowBox::performLayout() {
    setSize(constraints_.constrain(requestedSize_));
    if (!child())
        return;
    // 上游：子节点拿到无约束，可以溢出自身。
    child()->layout(BoxConstraints::unbounded());
    if (auto* pd = child()->parentData())
        pd->offset = alignChildOffset(alignment_, size_, child()->size());
}

// ── RenderUnconstrainedBox ──────────────────────────────────────────────────

void RenderUnconstrainedBox::performLayout() {
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    // 默认解除全部约束；constrainedAxis 指定轴仍沿用父约束。
    BoxConstraints childConstraints = BoxConstraints::unbounded();
    if (constrainedAxis_) {
        childConstraints = *constrainedAxis_ == Axis::Horizontal
                               ? BoxConstraints(constraints_.minWidth, constraints_.maxWidth, 0.0,
                                                BoxConstraints::kInfinity)
                               : BoxConstraints(0.0, BoxConstraints::kInfinity,
                                                constraints_.minHeight, constraints_.maxHeight);
    }
    child()->layout(childConstraints);
    setSize(constraints_.constrain(child()->size()));
    if (auto* pd = child()->parentData())
        pd->offset = alignChildOffset(alignment_, size_, child()->size());
}

// ── RenderOffstage ──────────────────────────────────────────────────────────

void RenderOffstage::performLayout() {
    if (!child()) {
        setSize(constraints_.smallest());
        return;
    }
    // offstage 时子节点仍参与布局（便于测量），但自身不占用父节点空间。
    child()->layout(constraints_);
    setSize(offstage_ ? Size(0.0, 0.0) : child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderOffstage::performPaint(PaintingContext& context, const Offset& offset) {
    if (offstage_)
        return;
    RenderProxyBox::performPaint(context, offset);
}

bool RenderOffstage::hitTest(const Offset& position, std::vector<RenderObject*>& result) {
    if (offstage_)
        return false;
    return RenderBox::hitTest(position, result);
}

// ── RenderImage ─────────────────────────────────────────────────────────────

Size RenderImage::imageSize() const {
    if (!image_)
        return Size();
    return Size(static_cast<double>(image_->width()) / scale_,
                static_cast<double>(image_->height()) / scale_);
}

Size RenderImage::sizeForConstraints(const BoxConstraints& c) const {
    // 上游的 `_sizeForConstraints`：显式 width/height 先与父约束取交集。
    const BoxConstraints constraints = BoxConstraints::tightFor(width_, height_).enforce(c);
    if (!image_)
        return constraints.smallest();
    return constraints.constrainSizeAndAttemptToPreserveAspectRatio(imageSize());
}

void RenderImage::performLayout() {
    setSize(sizeForConstraints(constraints_));
}

void RenderImage::performPaint(PaintingContext& context, const Offset& offset) {
    if (!image_ || size_.width <= 0.0 || size_.height <= 0.0)
        return;
    SkCanvas* c = context.canvas();
    if (!c)
        return;

    const Size src = imageSize();
    const FittedSizes sizes = applyBoxFit(fit_, src, size_);
    if (sizes.source.width <= 0.0 || sizes.source.height <= 0.0 ||
        sizes.destination.width <= 0.0 || sizes.destination.height <= 0.0)
        return;

    // 上游的 paintImage：source / destination 都由 alignment.inscribe 定位。
    const Rect sourceRect =
            alignment_.inscribe(sizes.source, Rect(0, 0, src.width, src.height));
    const Rect destinationRect =
            alignment_.inscribe(sizes.destination, Rect(0, 0, size_.width, size_.height));

    const SkRect skSrc = SkRect::MakeXYWH(SkScalar(sourceRect.x), SkScalar(sourceRect.y),
                                          SkScalar(sourceRect.width), SkScalar(sourceRect.height));
    const SkRect skDst =
            SkRect::MakeXYWH(SkScalar(offset.dx + destinationRect.x),
                             SkScalar(offset.dy + destinationRect.y),
                             SkScalar(destinationRect.width), SkScalar(destinationRect.height));

    SkPaint paint;
    paint.setAntiAlias(true);
    const SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    c->drawImageRect(image_.get(), skSrc, skDst, sampling, &paint,
                     SkCanvas::kStrict_SrcRectConstraint);
}

// ── RenderCustomPaint ───────────────────────────────────────────────────────

void RenderCustomPaint::performLayout() {
    if (!child()) {
        setSize(constraints_.constrain(preferredSize_));
        return;
    }
    child()->layout(constraints_);
    setSize(child()->size());
    if (auto* pd = child()->parentData())
        pd->offset = Offset(0, 0);
}

void RenderCustomPaint::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    // 上游：painter 在子节点之前、foregroundPainter 在子节点之后，
    // 画布已平移到盒子原点，回调拿到的是 (canvas, size)。
    if (c && painter_) {
        c->save();
        c->translate(SkScalar(offset.dx), SkScalar(offset.dy));
        Painter p(c);
        painter_(p, size_);
        c->restore();
    }
    if (child())
        RenderProxyBox::performPaint(context, offset);
    // 子节点里推图层会换录制画布：前景层必须取当前画布，否则会画进已结算的
    // 旧段（内容被压在推入的图层之下）。
    SkCanvas* fg = context.canvas();
    if (fg && foregroundPainter_) {
        fg->save();
        fg->translate(SkScalar(offset.dx), SkScalar(offset.dy));
        Painter p(fg);
        foregroundPainter_(p, size_);
        fg->restore();
    }
}

// ── RenderWrap ──────────────────────────────────────────────────────────────

void RenderWrap::setupParentData(RenderObject* child) {
    child->setParentData(new BoxParentData());
}

void RenderWrap::performLayout() {
    runCount_ = 0;
    if (children().empty()) {
        setSize(constraints_.smallest());
        return;
    }

    const bool horizontal = direction_ == Axis::Horizontal;
    // 子节点：主轴沿用父上限（可能无界），交叉轴无界。
    const BoxConstraints childConstraints =
            horizontal ? BoxConstraints(0.0, constraints_.maxWidth, 0.0,
                                        BoxConstraints::kInfinity)
                       : BoxConstraints(0.0, BoxConstraints::kInfinity, 0.0,
                                        constraints_.maxHeight);
    const double mainLimit = horizontal ? constraints_.maxWidth : constraints_.maxHeight;

    struct RunInfo {
        size_t begin = 0;
        size_t end = 0;
        double mainExtent = 0.0;
        double crossExtent = 0.0;
    };

    // 第一遍：测量并按主轴上限切分 run。
    std::vector<RunInfo> runs;
    std::vector<double> childMainStart(children().size(), 0.0);
    double runMain = 0.0;
    double runCross = 0.0;
    size_t runBegin = 0;
    for (size_t i = 0; i < children().size(); ++i) {
        RenderObject* c = children()[i];
        if (!c)
            continue;
        c->layout(childConstraints);
        const double m = mainExtentOf(c->size());
        const double x = crossExtentOf(c->size());
        if (i > runBegin && runMain + spacing_ + m > mainLimit) {
            runs.push_back({runBegin, i, runMain, runCross});
            runBegin = i;
            runMain = 0.0;
            runCross = 0.0;
        } else if (i > runBegin) {
            runMain += spacing_;
        }
        childMainStart[i] = runMain;
        runMain += m;
        runCross = std::max(runCross, x);
    }
    runs.push_back({runBegin, children().size(), runMain, runCross});
    runCount_ = runs.size();

    double contentMain = 0.0;
    double contentCross = 0.0;
    for (const RunInfo& r : runs)
        contentMain = std::max(contentMain, r.mainExtent);
    for (size_t k = 0; k < runs.size(); ++k) {
        contentCross += runs[k].crossExtent;
        if (k > 0)
            contentCross += runSpacing_;
    }

    setSize(constraints_.constrain(
            horizontal ? Size(contentMain, contentCross) : Size(contentCross, contentMain)));

    // 第二遍：按 alignment / runAlignment / crossAxisAlignment 摆放。
    double runOffset = 0.0;
    double runGap = 0.0;
    distributeFreeSpace(runAlignment_,
                        std::max(0.0, crossExtentOf(size_) - contentCross), runs.size(),
                        runOffset, runGap);

    double crossPos = runOffset;
    for (const RunInfo& r : runs) {
        const size_t count = r.end - r.begin;
        double childOffset = 0.0;
        double childGap = 0.0;
        distributeFreeSpace(alignment_, std::max(0.0, mainExtentOf(size_) - r.mainExtent), count,
                            childOffset, childGap);

        double mainPos = childOffset;
        for (size_t i = r.begin; i < r.end; ++i) {
            RenderObject* c = children()[i];
            if (!c)
                continue;
            const double cross = crossExtentOf(c->size());
            double crossOff = 0.0;
            switch (crossAxisAlignment_) {
                case WrapCrossAlignment::Start:
                    crossOff = 0.0;
                    break;
                case WrapCrossAlignment::End:
                    crossOff = r.crossExtent - cross;
                    break;
                case WrapCrossAlignment::Center:
                    crossOff = (r.crossExtent - cross) / 2.0;
                    break;
            }
            mainPos = childOffset + childMainStart[i] + childGap * (i - r.begin);
            if (auto* pd = c->parentData())
                pd->offset = horizontal ? Offset(mainPos, crossPos + crossOff)
                                        : Offset(crossPos + crossOff, mainPos);
        }
        crossPos += r.crossExtent + runSpacing_ + runGap;
    }
}

// ── RenderParagraph ─────────────────────────────────────────────────────────

namespace {
/// 遍历 TextSpan 树收集叶 run（单行，children 内部节点不自带文字）。
struct ParagraphRun {
    const TextSpan* span;
};
void collectRuns(const TextSpan* root, std::vector<const TextSpan*>& out) {
    if (!root)
        return;
    if (root->children.empty() && !root->text.empty()) {
        out.push_back(root);
    } else {
        for (const auto& c : root->children)
            collectRuns(c.get(), out);
    }
}
}  // namespace

RenderParagraph::NaturalSize RenderParagraph::measureNatural() const {
    std::vector<const TextSpan*> runs;
    collectRuns(text_.get(), runs);

    NaturalSize out;
    if (runs.empty()) {
        // 空段落仍占一行的高度（对齐 上游的 strut 行为：空文本宽度 0、高度非 0）。
        const TextStyle rootStyle = text_ ? text_->style : TextStyle{};
        const char* fam = rootStyle.fontFamily.empty() ? nullptr : rootStyle.fontFamily.c_str();
        double fs = rootStyle.fontSize > 0 ? rootStyle.fontSize : 14.0;
        double fw = rootStyle.fontWeight > 0 ? rootStyle.fontWeight : 400.0;
        out.height = Painter::textHeight(fam, fs, fw, rootStyle.italic);
        out.baseline = Painter::baselineOffset(fam, fs, fw, rootStyle.italic);
        return out;
    }
    for (const auto* span : runs) {
        const TextStyle& s = span->style;
        const char* fam = s.fontFamily.empty() ? nullptr : s.fontFamily.c_str();
        double fs = s.fontSize > 0 ? s.fontSize : 14.0;
        double fw = s.fontWeight > 0 ? s.fontWeight : 400.0;
        out.width += Painter::measureTextWidth(fam, span->text, fs, fw, s.italic);
        out.height = std::max(out.height, Painter::textHeight(fam, fs, fw, s.italic));
        out.baseline = std::max(out.baseline, Painter::baselineOffset(fam, fs, fw, s.italic));
    }
    return out;
}

void RenderParagraph::performLayout() {
    const NaturalSize natural = measureNatural();

    contentWidth_ = natural.width;
    baselineOffset_ = natural.baseline;

    double width = natural.width;
    if (constraints_.hasBoundedWidth())
        width = std::min(width, constraints_.maxWidth);
    width = std::max(width, constraints_.minWidth);

    double height = natural.height;
    if (constraints_.hasBoundedHeight())
        height = std::min(height, constraints_.maxHeight);
    height = std::max(height, constraints_.minHeight);

    setSize(Size(width, height));
}

double RenderParagraph::computeMinIntrinsicWidth(double height) {
    (void)height;
    return measureNatural().width;
}

double RenderParagraph::computeMaxIntrinsicWidth(double height) {
    (void)height;
    return measureNatural().width;
}

double RenderParagraph::computeMinIntrinsicHeight(double width) {
    (void)width;
    return measureNatural().height;
}

double RenderParagraph::computeMaxIntrinsicHeight(double width) {
    (void)width;
    return measureNatural().height;
}

void RenderParagraph::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    const double baseY = offset.dy + baselineOffset_;
    double x = offset.dx;

    std::function<void(const TextSpan*)> draw = [&](const TextSpan* span) {
        if (!span)
            return;
        if (span->children.empty() && !span->text.empty()) {
            const TextStyle& s = span->style;
            const char* fam = s.fontFamily.empty() ? nullptr : s.fontFamily.c_str();
            double fs = s.fontSize > 0 ? s.fontSize : 14.0;
            double fw = s.fontWeight > 0 ? s.fontWeight : 400.0;
            const double w = Painter::measureTextWidth(fam, span->text, fs, fw, s.italic);
            double ascent = 0.0, descent = 0.0;
            Painter::fontBand(fam, fs, fw, s.italic, ascent, descent);
            Painter p(c, fam);
            p.setFontFamily(fam);
            p.setFontSize(fs);
            p.setFontWeight(fw);
            p.setFontItalic(s.italic);
            // 颜色哨兵 a < 0 表示未指定，取当前主题的前景文字色（浅色近黑、
            // 深色近白），这样未显式指定颜色的文本随主题切换而反色。
            Color ec = s.color.a < 0 ? theme().textPrimary : s.color;
            // 所有 run 共享同一行基线 baseY；showText 把基线画在局部
            // y = fontSize 处，故平移到 (x, baseY - fontSize)。
            if (s.backgroundColor.a > 0) {
                p.setSourceColor(s.backgroundColor);
                p.rectangle(Rect(x, baseY - ascent, w, ascent + descent));
                p.fill();
            }
            p.setSourceColor(ec);
            c->save();
            c->translate(SkScalar(x), SkScalar(baseY - fs));
            p.showText(span->text.c_str());
            c->restore();
            if (s.underline || s.strikethrough) {
                // 装饰线跟着字形走：粗细随字号，位置按基线上下取（浏览器同款观感）。
                const double thickness = std::max(1.0, std::round(fs * 0.06));
                p.setLineWidth(thickness);
                if (s.underline) {
                    const double uy = baseY + std::max(2.0, descent * 0.35);
                    p.moveTo(x, uy);
                    p.lineTo(x + w, uy);
                    p.stroke();
                }
                if (s.strikethrough) {
                    const double sy = baseY - ascent * 0.45;
                    p.moveTo(x, sy);
                    p.lineTo(x + w, sy);
                    p.stroke();
                }
            }
            x += w;
        } else {
            for (const auto& ch : span->children)
                draw(ch.get());
        }
    };
    draw(text_.get());
}

}  // namespace cui_core
