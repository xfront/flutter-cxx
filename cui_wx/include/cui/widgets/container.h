#pragma once
/// @file widgets/container.h
/// @brief 上游的 `DecoratedBox` / `Container` 移植。
///
/// 对应 上游源码：
///   上游 widgets/container.dart
///
/// 关键约定（与 上游 一致）：
///   - `Container` 本身**不是** RenderObjectWidget，而是 StatelessWidget：
///     它在 build() 里按「由内向外」的顺序套上 Align → Padding →
///     DecoratedBox → ConstrainedBox → Padding(margin)，因此装饰绘制在
///     padding 之外、margin 之内；
///   - 只有 `decoration` 非空时才插入 DecoratedBox，`padding` / `margin`
///     全零时不插入多余的 Padding；
///   - `width` / `height` 是糖：等价于 `constraints` 在该轴取紧约束。

#include <memory>
#include <optional>

#include "cui/framework/framework.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/widgets/basic.h"
#include "cui/framework/render_object_widget.h"

namespace cui {

// ── BoxDecoration ──────────────────────────────────────────────────────────

/// 盒子装饰。上游的 `BoxDecoration`（painting/box_decoration.dart）子集：
/// 纯色底 + 圆角 + 边框 + elevation 阴影。
struct BoxDecoration {
    /// 背景色（a == 0 表示不绘制背景）。
    Color color{0, 0, 0, 0};
    /// 四角统一的圆角半径。
    double borderRadius = 0.0;
    /// 边框宽度（≤ 0 表示无边框）。
    double borderWidth = 0.0;
    /// 边框颜色。
    Color borderColor{0, 0, 0, 0};
    /// 投影强度（0 = 无阴影）。上游的 `elevation`。
    double elevation = 0.0;
    /// 阴影颜色（alpha 决定浓度）。上游的 `shadowColor`。
    Color shadowColor{0, 0, 0, 0.35};
    /// 第二层阴影（环境光 ambient）。上游/FAB 的双层投影：
    /// 一层 spot（elevation）+ 一层更柔和的 ambient（elevation2）。
    double elevation2 = 0.0;
    Color shadowColor2{0, 0, 0, 0};

    bool isEmpty() const { return color.a <= 0.0 && borderWidth <= 0.0; }
};

// ── DecoratedBox ───────────────────────────────────────────────────────────

/// 装饰盒。上游的 `DecoratedBox`（container.dart）。
///
/// 绘制在子节点**之前**（背景语义），自身不改变布局（尺寸 = 子节点尺寸）。
class DecoratedBox : public SingleChildRenderObjectWidget {
public:
    explicit DecoratedBox(BoxDecoration decoration = {},
                          SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          decoration_(decoration) {}

    const BoxDecoration& decoration() const { return decoration_; }
    void setDecoration(const BoxDecoration& d) { decoration_ = d; }

    RenderObject* createRenderObject() override {
        return new RenderDecoratedBox(decoration_.color, decoration_.borderRadius,
                                      decoration_.borderWidth, decoration_.borderColor,
                                      decoration_.elevation, decoration_.shadowColor,
                                      decoration_.elevation2, decoration_.shadowColor2);
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* d = dynamic_cast<RenderDecoratedBox*>(ro);
        if (!d)
            return;
        d->setColor(decoration_.color);
        d->setBorderRadius(decoration_.borderRadius);
        d->setBorder(decoration_.borderColor, decoration_.borderWidth);
        d->setElevation(decoration_.elevation);
        d->setShadowColor(decoration_.shadowColor);
        d->setElevation2(decoration_.elevation2);
        d->setShadowColor2(decoration_.shadowColor2);
    }

private:
    BoxDecoration decoration_;
};

// ── Container ──────────────────────────────────────────────────────────────

/// 组合容器。上游的 `Container`（container.dart）。
///
/// 本身不产生 RenderObject：build() 里按 上游的顺序组合出等价子树——
///
///   Padding(margin) → ConstrainedBox → DecoratedBox → Padding(padding)
///       → Align → child
///
/// 无子节点时 上游 会让 Container 尽量撑满（LimitedBox + expand 语义）；
/// 本移植保持简单：无子节点时仅保留约束/装饰产生的尺寸。
class Container : public StatelessWidget {
public:
    explicit Container(SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)) {}

    // ── 配置（返回引用以便链式调用）─────────────────────────────────────────

    /// 固定宽（等价于该轴的紧约束）。
    Container& width(double w) {
        width_ = w;
        return *this;
    }
    /// 固定高。
    Container& height(double h) {
        height_ = h;
        return *this;
    }
    /// 附加约束（上游的 `constraints`）。
    Container& constraints(BoxConstraints c) {
        constraints_ = c;
        return *this;
    }
    /// 内边距（在装饰之内）。
    Container& padding(EdgeInsets p) {
        padding_ = p;
        return *this;
    }
    /// 外边距（在装饰之外）。
    Container& margin(EdgeInsets m) {
        margin_ = m;
        return *this;
    }
    /// 子节点对齐（alignment ∈ [-1,1]，0 居中）；nullopt 表示不插入 Align。
    Container& alignment(double x, double y) {
        alignment_ = std::make_pair(x, y);
        return *this;
    }
    /// 装饰（背景/圆角/边框）。
    Container& decoration(BoxDecoration d) {
        decoration_ = d;
        return *this;
    }

    // ── 读取 ───────────────────────────────────────────────────────────────

    const SharedPtr<Widget>& child() const { return child_; }
    const std::optional<double>& widthValue() const { return width_; }
    const std::optional<double>& heightValue() const { return height_; }
    const std::optional<EdgeInsets>& paddingValue() const { return padding_; }
    const std::optional<EdgeInsets>& marginValue() const { return margin_; }
    const std::optional<BoxDecoration>& decorationValue() const { return decoration_; }

    /// 组合出等价子树。
    SharedPtr<Widget> build(BuildContext& context) override;

    std::string toStringShort() const override { return "Container"; }

private:
    /// width/height + constraints 合并后的附加约束（nullopt = 不约束）。
    std::optional<BoxConstraints> mergedConstraints() const;

    SharedPtr<Widget> child_;
    std::optional<double> width_;
    std::optional<double> height_;
    std::optional<BoxConstraints> constraints_;
    std::optional<EdgeInsets> padding_;
    std::optional<EdgeInsets> margin_;
    std::optional<std::pair<double, double>> alignment_;
    std::optional<BoxDecoration> decoration_;
};

}  // namespace cui_core
