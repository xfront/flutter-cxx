#pragma once
/// @file widgets/layout.h
/// @brief 上游 布局类 Widget 移植（第二批）。
///
/// 对应 上游源码：
///   上游 widgets/basic.dart      — Transform / RotatedBox / FittedBox
///                                                      Baseline / Offstage / LimitedBox
///                                                      OverflowBox / SizedOverflowBox
///                                                      UnconstrainedBox / IndexedStack / Spacer
///   上游 widgets/layout_builder.dart — StatefulBuilder
///   上游 widgets/wrap.dart       — Wrap
///   上游 rendering/proxy_box.dart — RepaintBoundary
///
/// 层级映射严格对齐上游：
///   Transform / RotatedBox / FittedBox / Baseline / LimitedBox / OverflowBox /
///   SizedOverflowBox / UnconstrainedBox / Offstage / RepaintBoundary
///       → SingleChildRenderObjectWidget
///   Wrap
///       → MultiChildRenderObjectWidget
///   IndexedStack / Spacer / Visibility
///       → StatelessWidget（上游 里 IndexedStack / Spacer 同样是组合出来的）

#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/repaint_boundary.h"
#include "cui/widgets/basic.h"
#include "cui/framework/render_object_widget.h"

namespace cui {

// ── FractionalTranslation ──────────────────────────────────────────────────

/// 按子节点自身尺寸的比例平移。上游的 `FractionalTranslation`。
///
/// translation=(0,1) 表示「向下偏移一个自身高度」。命中区域默认随平移一起
/// 移动（`transformHitTests` = true，与上游一致）。
class FractionalTranslation : public SingleChildRenderObjectWidget {
public:
    explicit FractionalTranslation(Offset translation = Offset(),
                                   SharedPtr<Widget> child = nullptr,
                                   KeyPtr key = nullptr, bool transformHitTests = true)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          translation_(translation), transformHitTests_(transformHitTests) {}

    Offset translation() const { return translation_; }
    void setTranslation(Offset t) { translation_ = t; }

    bool transformHitTests() const { return transformHitTests_; }
    void setTransformHitTests(bool v) { transformHitTests_ = v; }

    RenderObject* createRenderObject() override {
        return new RenderFractionalTranslation(translation_, transformHitTests_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderFractionalTranslation*>(ro)) {
            r->setTranslation(translation_);
            r->setTransformHitTests(transformHitTests_);
        }
    }

private:
    Offset translation_;
    bool transformHitTests_ = true;
};

// ── Transform ───────────────────────────────────────────────────────────────

/// 矩阵变换。上游的 `Transform`（basic.dart）。
class Transform : public SingleChildRenderObjectWidget {
public:
    explicit Transform(Matrix4 transform, SharedPtr<Widget> child = nullptr,
                       Offset origin = Offset(), bool transformHitTests = true,
                       KeyPtr key = nullptr,
                       std::optional<Alignment> alignment = std::nullopt)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          transform_(transform), origin_(origin), transformHitTests_(transformHitTests),
          alignment_(alignment) {}

    /// 平移特化构造（上游的 `Transform.translate`）。
    static SharedPtr<Transform> translate(Offset offset,
                                                SharedPtr<Widget> child = nullptr) {
        return cui::make_shared<Transform>(Matrix4::translation(offset.dx, offset.dy),
                                           std::move(child));
    }
    /// 缩放特化构造（上游的 `Transform.scale`）。
    static SharedPtr<Transform> scale(double scale,
                                            SharedPtr<Widget> child = nullptr) {
        return cui::make_shared<Transform>(Matrix4::scale(scale, scale), std::move(child));
    }
    /// 旋转特化构造（弧度，上游的 `Transform.rotate`）。
    /// 上游的 `Transform.rotate` 默认 `alignment: Alignment.center`（绕中心转），
    /// 而非基础构造的 topLeft。
    static SharedPtr<Transform> rotate(double radians,
                                             SharedPtr<Widget> child = nullptr) {
        return cui::make_shared<Transform>(Matrix4::rotationZ(radians), std::move(child),
                                           Offset(), true, nullptr, Alignment::center());
    }

    const Matrix4& transform() const { return transform_; }
    void setTransform(const Matrix4& m) { transform_ = m; }

    const Offset& origin() const { return origin_; }
    void setOrigin(const Offset& o) { origin_ = o; }

    /// 变换中心相对盒子尺寸的对齐（上游的 `Transform.alignment`）。
    const std::optional<Alignment>& alignment() const { return alignment_; }
    void setAlignment(std::optional<Alignment> a) { alignment_ = a; }

    bool transformHitTests() const { return transformHitTests_; }
    void setTransformHitTests(bool v) { transformHitTests_ = v; }

    RenderObject* createRenderObject() override {
        return new RenderTransform(transform_, origin_, alignment_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderTransform*>(ro)) {
            r->setTransform(transform_);
            r->setOrigin(origin_);
            r->setAlignment(alignment_);
            r->setTransformHitTests(transformHitTests_);
        }
    }

private:
    Matrix4 transform_;
    Offset origin_;
    bool transformHitTests_ = true;
    std::optional<Alignment> alignment_;
};

// ── RotatedBox ──────────────────────────────────────────────────────────────

/// 按 90° 整数倍旋转子节点。上游的 `RotatedBox`（basic.dart）。
///
/// 与 `Transform` 的差别：RotatedBox **参与布局**（约束与尺寸都翻轴），
/// Transform 只影响绘制。
class RotatedBox : public SingleChildRenderObjectWidget {
public:
    explicit RotatedBox(int quarterTurns = 1, SharedPtr<Widget> child = nullptr,
                        KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          quarterTurns_(quarterTurns) {}

    int quarterTurns() const { return quarterTurns_; }
    void setQuarterTurns(int t) { quarterTurns_ = t; }

    RenderObject* createRenderObject() override { return new RenderRotatedBox(quarterTurns_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderRotatedBox*>(ro))
            r->setQuarterTurns(quarterTurns_);
    }

private:
    int quarterTurns_ = 1;
};

// ── FittedBox ───────────────────────────────────────────────────────────────

/// 按 BoxFit 缩放子节点。上游的 `FittedBox`（basic.dart）。
class FittedBox : public SingleChildRenderObjectWidget {
public:
    explicit FittedBox(BoxFit fit = BoxFit::Contain,
                       Alignment alignment = Alignment::center(),
                       SharedPtr<Widget> child = nullptr, bool clipContent = true,
                       KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), fit_(fit),
          alignment_(alignment), clipContent_(clipContent) {}

    BoxFit fit() const { return fit_; }
    void setFit(BoxFit f) { fit_ = f; }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { alignment_ = a; }

    bool clipContent() const { return clipContent_; }
    void setClipContent(bool v) { clipContent_ = v; }

    RenderObject* createRenderObject() override {
        return new RenderFittedBox(fit_, alignment_, clipContent_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderFittedBox*>(ro)) {
            r->setFit(fit_);
            r->setAlignment(alignment_);
        }
    }

private:
    BoxFit fit_ = BoxFit::Contain;
    Alignment alignment_ = Alignment::center();
    bool clipContent_ = true;
};

// ── Baseline ────────────────────────────────────────────────────────────────

/// 基线对齐。上游的 `Baseline`（basic.dart）。
class Baseline : public SingleChildRenderObjectWidget {
public:
    explicit Baseline(double baseline = 0.0,
                      BaselineType baselineType = BaselineType::Alphabetic,
                      SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), baseline_(baseline),
          baselineType_(baselineType) {}

    double baseline() const { return baseline_; }
    void setBaseline(double b) { baseline_ = b; }

    BaselineType baselineType() const { return baselineType_; }
    void setBaselineType(BaselineType t) { baselineType_ = t; }

    RenderObject* createRenderObject() override {
        return new RenderBaseline(baseline_, baselineType_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderBaseline*>(ro)) {
            r->setBaseline(baseline_);
            r->setBaselineType(baselineType_);
        }
    }

private:
    double baseline_ = 0.0;
    BaselineType baselineType_ = BaselineType::Alphabetic;
};

// ── LimitedBox ──────────────────────────────────────────────────────────────

/// 仅在父约束无界时限制尺寸。上游的 `LimitedBox`（basic.dart）。
class LimitedBox : public SingleChildRenderObjectWidget {
public:
    explicit LimitedBox(double maxWidth = BoxConstraints::kInfinity,
                        double maxHeight = BoxConstraints::kInfinity,
                        SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), maxWidth_(maxWidth),
          maxHeight_(maxHeight) {}

    double maxWidth() const { return maxWidth_; }
    void setMaxWidth(double v) { maxWidth_ = v; }

    double maxHeight() const { return maxHeight_; }
    void setMaxHeight(double v) { maxHeight_ = v; }

    RenderObject* createRenderObject() override {
        return new RenderLimitedBox(maxWidth_, maxHeight_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderLimitedBox*>(ro)) {
            r->setMaxWidth(maxWidth_);
            r->setMaxHeight(maxHeight_);
        }
    }

private:
    double maxWidth_ = BoxConstraints::kInfinity;
    double maxHeight_ = BoxConstraints::kInfinity;
};

// ── OverflowBox ─────────────────────────────────────────────────────────────

/// 允许子节点溢出自身。上游的 `OverflowBox`（basic.dart）。
class OverflowBox : public SingleChildRenderObjectWidget {
public:
    explicit OverflowBox(std::optional<double> minWidth = std::nullopt,
                         std::optional<double> maxWidth = std::nullopt,
                         std::optional<double> minHeight = std::nullopt,
                         std::optional<double> maxHeight = std::nullopt,
                         Alignment alignment = Alignment::center(),
                         SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), minWidth_(minWidth),
          maxWidth_(maxWidth), minHeight_(minHeight), maxHeight_(maxHeight),
          alignment_(alignment) {}

    const std::optional<double>& maxWidth() const { return maxWidth_; }
    void setMaxWidth(std::optional<double> v) { maxWidth_ = v; }
    const std::optional<double>& maxHeight() const { return maxHeight_; }
    void setMaxHeight(std::optional<double> v) { maxHeight_ = v; }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { alignment_ = a; }

    RenderObject* createRenderObject() override {
        return new RenderOverflowBox(minWidth_, maxWidth_, minHeight_, maxHeight_, alignment_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderOverflowBox*>(ro)) {
            r->setMinWidth(minWidth_);
            r->setMaxWidth(maxWidth_);
            r->setMinHeight(minHeight_);
            r->setMaxHeight(maxHeight_);
            r->setAlignment(alignment_);
        }
    }

private:
    std::optional<double> minWidth_, maxWidth_, minHeight_, maxHeight_;
    Alignment alignment_ = Alignment::center();
};

// ── SizedOverflowBox ────────────────────────────────────────────────────────

/// 固定自身尺寸、允许子节点溢出。上游的 `SizedOverflowBox`（basic.dart）。
class SizedOverflowBox : public SingleChildRenderObjectWidget {
public:
    explicit SizedOverflowBox(Size requestedSize = Size(),
                              Alignment alignment = Alignment::center(),
                              SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          requestedSize_(requestedSize), alignment_(alignment) {}

    const Size& requestedSize() const { return requestedSize_; }
    void setRequestedSize(const Size& s) { requestedSize_ = s; }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { alignment_ = a; }

    RenderObject* createRenderObject() override {
        return new RenderSizedOverflowBox(requestedSize_, alignment_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderSizedOverflowBox*>(ro)) {
            r->setRequestedSize(requestedSize_);
            r->setAlignment(alignment_);
        }
    }

private:
    Size requestedSize_;
    Alignment alignment_ = Alignment::center();
};

// ── UnconstrainedBox ────────────────────────────────────────────────────────

/// 解除子节点约束。上游的 `UnconstrainedBox`（basic.dart）。
class UnconstrainedBox : public SingleChildRenderObjectWidget {
public:
    explicit UnconstrainedBox(std::optional<Axis> constrainedAxis = std::nullopt,
                              Alignment alignment = Alignment::center(),
                              SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          constrainedAxis_(constrainedAxis), alignment_(alignment) {}

    const std::optional<Axis>& constrainedAxis() const { return constrainedAxis_; }
    void setConstrainedAxis(std::optional<Axis> a) { constrainedAxis_ = a; }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { alignment_ = a; }

    RenderObject* createRenderObject() override {
        return new RenderUnconstrainedBox(constrainedAxis_, alignment_);
    }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderUnconstrainedBox*>(ro)) {
            r->setConstrainedAxis(constrainedAxis_);
            r->setAlignment(alignment_);
        }
    }

private:
    std::optional<Axis> constrainedAxis_;
    Alignment alignment_ = Alignment::center();
};

// ── Offstage ────────────────────────────────────────────────────────────────

/// 在场但不渲染、不命中、不占空间。上游的 `Offstage`（basic.dart）。
class Offstage : public SingleChildRenderObjectWidget {
public:
    explicit Offstage(bool offstage = true, SharedPtr<Widget> child = nullptr,
                      KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)), offstage_(offstage) {}

    bool offstage() const { return offstage_; }
    void setOffstage(bool v) { offstage_ = v; }

    RenderObject* createRenderObject() override { return new RenderOffstage(offstage_); }
    void updateRenderObject(RenderObject* ro) override {
        if (auto* r = dynamic_cast<RenderOffstage*>(ro))
            r->setOffstage(offstage_);
    }

private:
    bool offstage_ = true;
};

// ── Visibility ──────────────────────────────────────────────────────────────

/// 可见性开关。上游的 `Visibility`（visibility.dart）。
///
/// 与 上游 一致：`visible == false` 且 `maintainSize == false` 时整棵子树
/// 走 Offstage（不绘制、不命中、不占空间）；`maintainSize == true` 时保留
/// 空间，改用 `Opacity(0)` 让子树隐形。
class Visibility : public StatelessWidget {
public:
    explicit Visibility(bool visible = true, SharedPtr<Widget> child = nullptr,
                        SharedPtr<Widget> replacement = nullptr, bool maintainSize = false,
                        KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), visible_(visible), replacement_(std::move(replacement)),
          child_(std::move(child)), maintainSize_(maintainSize) {}

    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }

    bool maintainSize() const { return maintainSize_; }
    void setMaintainSize(bool v) { maintainSize_ = v; }

    SharedPtr<Widget> build(BuildContext&) override {
        if (!visible_ && !maintainSize_)
            return replacement_ ? replacement_ : cui::make_shared<Offstage>(true, child_);
        if (!visible_ && maintainSize_)
            return cui::make_shared<Opacity>(0.0, child_);
        return child_;
    }

private:
    bool visible_ = true;
    SharedPtr<Widget> replacement_;
    SharedPtr<Widget> child_;
    bool maintainSize_ = false;
};

// ── RepaintBoundary ─────────────────────────────────────────────────────────

/// 重绘边界。上游的 `RepaintBoundary`（basic.dart）。
///
/// 子树重绘被隔离在独立图层内，不再把父节点标脏。
class RepaintBoundary : public SingleChildRenderObjectWidget {
public:
    explicit RepaintBoundary(SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)) {}

    RenderObject* createRenderObject() override { return new RenderRepaintBoundary(); }
};

// ── IndexedStack ────────────────────────────────────────────────────────────

/// 只显示第 index 个子节点的 Stack。上游的 `IndexedStack`（basic.dart）。
///
/// 与 上游 一致：非当前 index 的子节点被包进 `Offstage`（仍参与构建，
/// 但不绘制 / 不命中 / 不占空间）。
class IndexedStack : public StatelessWidget {
public:
    explicit IndexedStack(int index = 0,
                          std::vector<SharedPtr<Widget>> children = {},
                          RenderStack::Alignment alignment = RenderStack::Alignment::TopStart,
                          KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), index_(index), children_(std::move(children)),
          alignment_(alignment) {}

    int index() const { return index_; }
    void setIndex(int i) { index_ = i; }

    const std::vector<SharedPtr<Widget>>& children() const { return children_; }
    void setChildren(std::vector<SharedPtr<Widget>> c) { children_ = std::move(c); }

    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> built;
        built.reserve(children_.size());
        for (size_t i = 0; i < children_.size(); ++i) {
            if (static_cast<int>(i) == index_)
                built.push_back(children_[i]);
            else
                built.push_back(cui::make_shared<Offstage>(true, children_[i]));
        }
        return cui::make_shared<Stack>(built, nullptr, alignment_, RenderStack::Fit::Loose);
    }

private:
    int index_ = 0;
    std::vector<SharedPtr<Widget>> children_;
    RenderStack::Alignment alignment_ = RenderStack::Alignment::TopStart;
};

// ── Spacer ──────────────────────────────────────────────────────────────────

/// 占满剩余空间。上游的 `Spacer`（basic.dart）。
///
/// 上游的实现就是 `Expanded(flex: flex, child: SizedBox.shrink())`。
class Spacer : public StatelessWidget {
public:
    explicit Spacer(int flex = 1, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), flex_(flex) {}

    int flex() const { return flex_; }
    void setFlex(int f) { flex_ = f; }

    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<Expanded>(flex_, cui::make_shared<SizedBox>(0.0, 0.0));
    }

private:
    int flex_ = 1;
};

// ── Wrap ────────────────────────────────────────────────────────────────────

/// 流式换行布局。上游的 `Wrap`（wrap.dart）。
class Wrap : public MultiChildRenderObjectWidget {
public:
    explicit Wrap(Axis direction = Axis::Horizontal,
                  std::vector<SharedPtr<Widget>> children = {},
                  WrapAlignment alignment = WrapAlignment::Start, double spacing = 0.0,
                  WrapAlignment runAlignment = WrapAlignment::Start, double runSpacing = 0.0,
                  WrapCrossAlignment crossAxisAlignment = WrapCrossAlignment::Start,
                  KeyPtr key = nullptr)
        : MultiChildRenderObjectWidget(std::move(key), std::move(children)), direction_(direction),
          alignment_(alignment), spacing_(spacing), runAlignment_(runAlignment),
          runSpacing_(runSpacing), crossAxisAlignment_(crossAxisAlignment) {}

    Axis direction() const { return direction_; }
    void setDirection(Axis d) { direction_ = d; }

    WrapAlignment alignment() const { return alignment_; }
    void setAlignment(WrapAlignment a) { alignment_ = a; }

    double spacing() const { return spacing_; }
    void setSpacing(double s) { spacing_ = s; }

    WrapAlignment runAlignment() const { return runAlignment_; }
    void setRunAlignment(WrapAlignment a) { runAlignment_ = a; }

    double runSpacing() const { return runSpacing_; }
    void setRunSpacing(double s) { runSpacing_ = s; }

    WrapCrossAlignment crossAxisAlignment() const { return crossAxisAlignment_; }
    void setCrossAxisAlignment(WrapCrossAlignment a) { crossAxisAlignment_ = a; }

    RenderObject* createRenderObject() override {
        return new RenderWrap(direction_, alignment_, spacing_, runAlignment_, runSpacing_,
                              crossAxisAlignment_);
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderWrap*>(ro);
        if (!r)
            return;
        r->setDirection(direction_);
        r->setAlignment(alignment_);
        r->setSpacing(spacing_);
        r->setRunAlignment(runAlignment_);
        r->setRunSpacing(runSpacing_);
        r->setCrossAxisAlignment(crossAxisAlignment_);
    }

private:
    Axis direction_ = Axis::Horizontal;
    WrapAlignment alignment_ = WrapAlignment::Start;
    double spacing_ = 0.0;
    WrapAlignment runAlignment_ = WrapAlignment::Start;
    double runSpacing_ = 0.0;
    WrapCrossAlignment crossAxisAlignment_ = WrapCrossAlignment::Start;
};

// ── StatefulBuilder ─────────────────────────────────────────────────────────

/// 自带 `setState` 的 Builder。上游的 `StatefulBuilder`（framework.dart）。
///
/// 回调签名与 上游 一致：`builder(BuildContext, StateSetter)`，
/// 其中 StateSetter 等价于 上游的 `StateSetter`（即 `setState`）。
class StatefulBuilder : public StatefulWidget {
public:
    using StateSetter = std::function<void()>;
    using BuilderCallback = std::function<SharedPtr<Widget>(BuildContext&, StateSetter)>;

    explicit StatefulBuilder(BuilderCallback builder, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), builder_(std::move(builder)) {}

    /// StatefulBuilder 的 State。嵌套类天然可访问外层私有成员。
    class BuilderState : public State {
    public:
        SharedPtr<Widget> build(BuildContext& context) override {
            auto* w = static_cast<StatefulBuilder*>(widget());
            if (!w || !w->builder_)
                return nullptr;
            // 捕获 this 以把 setState 交给回调（上游的 `StateSetter`）。
            State* self = this;
            StateSetter setter = [self]() { self->setState(); };
            return w->builder_(context, setter);
        }
    };

    std::unique_ptr<State> createState() override { return std::make_unique<BuilderState>(); }

    const BuilderCallback& builder() const { return builder_; }

private:
    BuilderCallback builder_;
};

// ── LayoutBuilder ───────────────────────────────────────────────────────────

/// 把「当前约束」暴露给构建回调的 Widget。上游的 `LayoutBuilder`
/// （layout_builder.dart）。
///
/// 典型用途：根据可用空间选择不同布局（如空间不足时换行 / 切换紧凑模式）。
/// `build` 回调签名 `(BuildContext, BoxConstraints) -> Widget` 与 上游 一致。
class LayoutBuilder : public SingleChildRenderObjectWidget {
public:
    using LayoutWidgetBuilder =
        std::function<SharedPtr<Widget>(BuildContext&, const BoxConstraints&)>;

    explicit LayoutBuilder(LayoutWidgetBuilder builder, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(key), builder_(std::move(builder)) {}

    /// 用给定约束构建子节点。由 LayoutBuilderElement 在约束变化时调用。
    SharedPtr<Widget> build(BuildContext& context,
                                  const BoxConstraints& constraints) const {
        return builder_ ? builder_(context, constraints) : nullptr;
    }

    RenderObject* createRenderObject() override { return new RenderLayoutBuilder(); }

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override {
        return std::make_unique<LayoutBuilderElement>(std::move(self));
    }

private:
    LayoutWidgetBuilder builder_;
};

}  // namespace cui_core
