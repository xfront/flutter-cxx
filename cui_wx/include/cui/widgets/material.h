#pragma once
/// @file widgets/material.h
/// @brief Material 组件层（无图标/手势依赖的子集）。
///
/// 对应 上游源码：
///   - Material / MaterialType（material/material.dart）
///   - Divider / VerticalDivider（material/divider.dart）
///   - Card（material/card.dart）
///   - Chip（material/chip.dart）
///   - LinearProgressIndicator / CircularProgressIndicator
///     （material/progress_indicator.dart）
///
/// 本批次刻意不引入 Icon / GestureDetector 依赖：
///   - Divider / Card / Chip 由 Container + BoxDecoration 组合而成；
///   - 进度条用自带的 RenderObject（render_material.h）做静态外观。
/// 交互式 Material 控件（Button / Checkbox / Switch / Slider / Radio）依赖
/// GestureDetector，留待手势系统接入后移植。

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"
#include "cui/rendering/animated_size.h"  // Clip 枚举（上游 dart:ui 的 Clip）
#include "cui/rendering/render_inkwell.h"
#include "cui/rendering/render_material.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/icons.h"
#include "cui/widgets/paint.h"
#include "cui/framework/render_object_widget.h"

namespace cui {

// ── Material ────────────────────────────────────────────────────────────────

/// 材质的种类。上游的 `MaterialType`（material/material.dart:40-68）。
///
/// 决定默认底色与默认形状；显式传入的 `color` / `borderRadius` 优先。
enum class MaterialType {
    Canvas,        ///< 矩形，默认用主题 canvas 色。
    Card,          ///< 2px 圆角，默认用主题 card 色。
    Circle,        ///< 圆形（FloatingActionButton），默认无色。
    Button,        ///< 2px 圆角（MaterialButton），默认无色。
    Transparency,  ///< 透明材质：不上色也不投影，只保留形状裁剪。
};

/// 某个 elevation 下的 surface tint 不透明度。
///
/// 上游 `ElevationOverlay._surfaceTintElevationOpacities`
///（material/elevation_overlay.dart:171-178）的查表 + 线性插值。
inline double materialSurfaceTintOpacity(double elevation) {
    static constexpr double kElevations[] = {0.0, 1.0, 3.0, 6.0, 8.0, 12.0};
    static constexpr double kOpacities[] = {0.0, 0.05, 0.08, 0.11, 0.12, 0.14};
    constexpr int kCount = 6;
    if (elevation <= kElevations[0])
        return kOpacities[0];
    for (int i = 1; i < kCount; ++i) {
        if (elevation < kElevations[i]) {
            const double t = (elevation - kElevations[i - 1]) /
                             (kElevations[i] - kElevations[i - 1]);
            return kOpacities[i - 1] + t * (kOpacities[i] - kOpacities[i - 1]);
        }
    }
    return kOpacities[kCount - 1];
}

/// src-over 混合。上游的 `Color.alphaBlend(fg, bg)`。
inline Color materialAlphaBlend(const Color& fg, const Color& bg) {
    const double outA = fg.a + bg.a * (1.0 - fg.a);
    if (outA <= 0.0)
        return Color::fromRGBA(0.0, 0.0, 0.0, 0.0);
    auto channel = [&](double f, double b) {
        return (f * fg.a + b * bg.a * (1.0 - fg.a)) / outA;
    };
    return Color::fromRGBA(channel(fg.r, bg.r), channel(fg.g, bg.g), channel(fg.b, bg.b),
                           outA);
}

/// 材质表面。上游的 `Material`（material/material.dart:183-...）。
///
/// 视觉骨架对齐上游 `_MaterialInterior` → `PhysicalShape`：
///
///   DecoratedBox(阴影 + 形状填充) →（clipBehavior != none 时裁剪形状）→ child
///
/// `Scaffold` 用它做整页底色（上游 scaffold.dart:3232-3238 的
/// `Material(color: backgroundColor ?? scaffoldBackgroundColor)`）。
///
/// 与上游的差异（本仓缺少对应基础设施，均为有意偏离）：
///   1. 无 `ShapeBorder`：形状只支持矩形 / 圆角矩形（单一半径）/ 圆形；
///      `borderRadius` 非空时优先于 `type` 决定的默认形状。
///   2. 无 `MaterialInkController`：本仓 `RenderInkWell` 自己画水波，因此不提供
///      `Material.of`；水波也不会像上游那样被裁剪到材质形状内。
///   3. 无 `AnimatedPhysicalModel`：elevation / color 变化是瞬跳，没有补间。
///   4. 无 `AnimatedDefaultTextStyle`，故不接 `textStyle` / `animationDuration`；
///      也没有 `shape`（见 1）与 `borderOnForeground`。
class Material : public StatelessWidget {
public:
    explicit Material(SharedPtr<Widget> child = nullptr,
                      MaterialType type = MaterialType::Canvas,
                      std::optional<Color> color = std::nullopt, double elevation = 0.0,
                      Color shadowColor = Color::fromRGBA(0.0, 0.0, 0.0, 0.35),
                      std::optional<Color> surfaceTintColor = std::nullopt,
                      std::optional<double> borderRadius = std::nullopt,
                      Clip clipBehavior = Clip::None, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)), type_(type),
          color_(color), elevation_(elevation), shadowColor_(shadowColor),
          surfaceTintColor_(surfaceTintColor), borderRadius_(borderRadius),
          clipBehavior_(clipBehavior) {}

    const SharedPtr<Widget>& child() const { return child_; }
    MaterialType type() const { return type_; }

    /// 显式底色；空表示按 `type` 取主题默认色。
    const std::optional<Color>& color() const { return color_; }

    double elevation() const { return elevation_; }
    Color shadowColor() const { return shadowColor_; }
    const std::optional<Color>& surfaceTintColor() const { return surfaceTintColor_; }

    /// 显式圆角；空表示按 `type` 取默认形状。
    const std::optional<double>& borderRadius() const { return borderRadius_; }

    Clip clipBehavior() const { return clipBehavior_; }

    /// 解析后的底色（上游 `_MaterialState.build` 的 `backgroundColor` 推导）。
    ///
    /// 上游 canvas/card 取 `Theme.canvasColor`/`cardColor`，二者在 M3 都是
    /// `colorScheme.surface`；本仓统一映射为 `theme().surface`。
    std::optional<Color> resolvedColor() const {
        if (color_)
            return color_;
        switch (type_) {
            case MaterialType::Canvas:
            case MaterialType::Card:
                return theme().surface;
            case MaterialType::Circle:
            case MaterialType::Button:
            case MaterialType::Transparency:
                return std::nullopt;
        }
        return std::nullopt;
    }

    /// 解析后的圆角。上游 `kMaterialEdges`（material.dart:72-78）只给
    /// card / button 2px 圆角；circle 由 ClipOval / 胶囊近似表达。
    double resolvedRadius() const {
        if (borderRadius_)
            return *borderRadius_;
        switch (type_) {
            case MaterialType::Card:
            case MaterialType::Button:
                return 2.0;
            default:
                return 0.0;
        }
    }

    /// 上游 `ElevationOverlay.applySurfaceTint`。
    Color tintedColor(const Color& base) const {
        if (!surfaceTintColor_ || *surfaceTintColor_ == colors::transparent() ||
            elevation_ <= 0.0)
            return base;
        Color tint = *surfaceTintColor_;
        tint.a = materialSurfaceTintOpacity(elevation_);
        return materialAlphaBlend(tint, base);
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        const bool circle = type_ == MaterialType::Circle && !borderRadius_;
        const double radius = circle ? Theme::radiusPill : resolvedRadius();

        SharedPtr<Widget> content = child_;
        if (content && clipBehavior_ != Clip::None) {
            const bool aa = clipBehavior_ == Clip::AntiAlias ||
                            clipBehavior_ == Clip::AntiAliasWithSaveLayer;
            if (circle)
                content = cui::make_shared<ClipOval>(content, aa);
            else if (radius > 0.0)
                content = cui::make_shared<ClipRRect>(radius, content);
            else
                content = cui::make_shared<ClipRect>(content, aa);
        }

        BoxDecoration deco;
        if (auto base = resolvedColor())
            deco.color = tintedColor(*base);
        if (type_ != MaterialType::Transparency) {
            deco.elevation = elevation_;
            deco.shadowColor = shadowColor_;
        }
        deco.borderRadius = radius;
        return cui::make_shared<DecoratedBox>(deco, content);
    }

    std::string toStringShort() const override { return "Material"; }

private:
    SharedPtr<Widget> child_;
    MaterialType type_;
    std::optional<Color> color_;
    double elevation_;
    Color shadowColor_;
    std::optional<Color> surfaceTintColor_;
    std::optional<double> borderRadius_;
    Clip clipBehavior_;
};

// ── Divider ─────────────────────────────────────────────────────────────────

/// 水平分隔线。上游的 `Divider`（divider.dart）。
///
/// 占 height 的竖直空间，内部居中一条 thickness 高的着色线。
class Divider : public StatelessWidget {
public:
    explicit Divider(double height = 16.0, double thickness = 1.0,
                     Color color = Color::fromRGBA(0, 0, 0, 0.12), KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), height_(height), thickness_(thickness),
          color_(color) {}

    double height() const { return height_; }
    double thickness() const { return thickness_; }
    Color color() const { return color_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        auto line = cui::make_shared<Container>();
        line->height(thickness_).decoration(BoxDecoration{color_});
        auto outer = cui::make_shared<Container>(cui::make_shared<Center>(line));
        outer->height(height_);
        return outer;
    }

    std::string toStringShort() const override { return "Divider"; }

private:
    double height_;
    double thickness_;
    Color color_;
};

// ── VerticalDivider ─────────────────────────────────────────────────────────

/// 竖直分隔线。上游的 `VerticalDivider`（divider.dart）。
class VerticalDivider : public StatelessWidget {
public:
    explicit VerticalDivider(double width = 16.0, double thickness = 1.0,
                             Color color = Color::fromRGBA(0, 0, 0, 0.12),
                             KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), width_(width), thickness_(thickness),
          color_(color) {}

    double width() const { return width_; }
    double thickness() const { return thickness_; }
    Color color() const { return color_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        auto line = cui::make_shared<Container>();
        line->width(thickness_).decoration(BoxDecoration{color_});
        auto outer = cui::make_shared<Container>(cui::make_shared<Center>(line));
        outer->width(width_);
        return outer;
    }

    std::string toStringShort() const override { return "VerticalDivider"; }

private:
    double width_;
    double thickness_;
    Color color_;
};

// ── InkWell ────────────────────────────────────────────────────────────────

/// Material 水波纹。上游的 `InkWell`（ink_well.dart）。
///
/// 渲染对象 RenderInkWell 自处理指针事件（Down 启动水波扩散、Up 触发
/// onTap、鼠标 Hover 点亮高亮），并在子节点内容之上叠加水波/高亮层。
class InkWell : public SingleChildRenderObjectWidget {
public:
    using VoidCallback = RenderInkWell::VoidCallback;
    using DownCallback = RenderInkWell::DownCallback;

    explicit InkWell(SharedPtr<Widget> child = nullptr, VoidCallback onTap = {},
                     DownCallback onTapDown = {}, VoidCallback onTapCancel = {},
                     KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          onTap_(std::move(onTap)), onTapDown_(std::move(onTapDown)),
          onTapCancel_(std::move(onTapCancel)) {}

    const VoidCallback& onTap() const { return onTap_; }
    const DownCallback& onTapDown() const { return onTapDown_; }
    const VoidCallback& onTapCancel() const { return onTapCancel_; }

    RenderObject* createRenderObject() override { return new RenderInkWell(); }

    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderInkWell*>(ro);
        if (!r)
            return;
        r->setOnTap(onTap_);
        r->setOnTapDown(onTapDown_);
        r->setOnTapCancel(onTapCancel_);
    }

    std::string toStringShort() const override { return "InkWell"; }

private:
    VoidCallback onTap_;
    DownCallback onTapDown_;
    VoidCallback onTapCancel_;
};

// ── Card ────────────────────────────────────────────────────────────────────

/// 卡片。上游的 `Card`（card.dart）。
///
/// 组合为「外 Padding(margin) → 内 InkWell?(onTap 非 null 时) → Container
/// (color + 圆角 + elevation)」；elevation 由 BoxDecoration 的阴影绘制支持
/// （RenderDecoratedBox 模糊 RRect）。
class Card : public StatelessWidget {
public:
    using VoidCallback = std::function<void()>;

    explicit Card(SharedPtr<Widget> child = nullptr,
                  EdgeInsets margin = EdgeInsets::all(4.0),
                  Color color = theme().surface, double borderRadius = 8.0,
                  double elevation = 4.0, VoidCallback onTap = {}, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)), margin_(margin),
          color_(color), borderRadius_(borderRadius), elevation_(elevation),
          onTap_(std::move(onTap)) {}

    const SharedPtr<Widget>& child() const { return child_; }
    EdgeInsets margin() const { return margin_; }
    Color color() const { return color_; }
    double borderRadius() const { return borderRadius_; }
    double elevation() const { return elevation_; }
    const VoidCallback& onTap() const { return onTap_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        SharedPtr<Widget> inner = child_;
        if (onTap_)
            inner = cui::make_shared<InkWell>(inner, onTap_);
        auto c = cui::make_shared<Container>(inner);
        // 对齐 legacy Card（src/widgets/data/card.cpp）：radiusLarge(8) 圆角、
        // 0.5px borderLight 边框、内容内边距 8、M3 双层柔和投影（ambient + spot）。
        BoxDecoration deco;
        deco.color = color_;
        deco.borderRadius = borderRadius_;
        deco.borderWidth = 0.5;
        deco.borderColor = Color::fromRGB(0.88, 0.90, 0.92);  // Theme::borderLight(light)
        deco.elevation = elevation_;
        deco.shadowColor = Color::fromRGBA(0, 0, 0, 0.22);   // spot
        deco.elevation2 = elevation_ * 0.5;                   // ambient（更柔、偏移更小）
        deco.shadowColor2 = Color::fromRGBA(0, 0, 0, 0.10);
        c->margin(margin_).padding(EdgeInsets::all(8.0)).decoration(deco);
        return c;
    }

    std::string toStringShort() const override { return "Card"; }

private:
    SharedPtr<Widget> child_;
    EdgeInsets margin_;
    Color color_;
    double borderRadius_;
    double elevation_;
    VoidCallback onTap_;
};

// ── ListTile ───────────────────────────────────────────────────────────────

/// 列表项。上游的 `ListTile`（list_tile.dart）。
///
/// 布局：Row[ leading?, Expanded(Column[title, subtitle?]), trailing? ]，
/// 外层 padding(vertical: dense?4:8, horizontal:16)；onTap 非 null 时
/// 包裹 GestureDetector。
class ListTile : public StatelessWidget {
public:
    using VoidCallback = std::function<void()>;

    explicit ListTile(SharedPtr<Widget> title = nullptr,
                      SharedPtr<Widget> subtitle = nullptr,
                      SharedPtr<Widget> leading = nullptr,
                      SharedPtr<Widget> trailing = nullptr, bool dense = false,
                      VoidCallback onTap = {}, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), title_(std::move(title)),
          subtitle_(std::move(subtitle)), leading_(std::move(leading)),
          trailing_(std::move(trailing)), dense_(dense), onTap_(std::move(onTap)) {}

    const SharedPtr<Widget>& title() const { return title_; }
    const SharedPtr<Widget>& leading() const { return leading_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        std::vector<SharedPtr<Widget>> row;
        if (leading_)
            row.push_back(leading_);
        std::vector<SharedPtr<Widget>> texts;
        if (title_)
            texts.push_back(title_);
        if (subtitle_)
            texts.push_back(subtitle_);
        auto col = cui::make_shared<Column>(std::move(texts));
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        col->setMainAxisSize(MainAxisSize::Min);
        row.push_back(cui::make_shared<Expanded>(1, col));
        if (trailing_)
            row.push_back(trailing_);

        auto r = cui::make_shared<Row>(std::move(row));
        auto c = cui::make_shared<Container>(r);
        c->padding(EdgeInsets::symmetric(dense_ ? 4.0 : 8.0, 16.0));
        if (onTap_)
            return cui::make_shared<GestureDetector>(c, onTap_);
        return c;
    }

    std::string toStringShort() const override { return "ListTile"; }

private:
    SharedPtr<Widget> title_, subtitle_, leading_, trailing_;
    bool dense_;
    VoidCallback onTap_;
};

// ── Chip ────────────────────────────────────────────────────────────────────

/// 胶囊标签。上游的 `Chip`（chip.dart）。
///
/// 布局：Row[ avatar?, label, (4) deleteButton? ]，外层 Container
/// (padding + 胶囊圆角底色)。onDeleted 非 null 时尾部追加 Close 图标的
/// IconButton；selected 时底色换 selectedColor。
class Chip : public StatelessWidget {
public:
    using VoidCallback = std::function<void()>;

    explicit Chip(SharedPtr<Widget> label,
                  Color backgroundColor = Color::fromRGBA(0, 0, 0, 0.12),
                  double radius = 16.0,
                  EdgeInsets padding = EdgeInsets::symmetric(4.0, 8.0),
                  SharedPtr<Widget> avatar = nullptr,
                  IconCode deleteIcon = IconCode::None, VoidCallback onDeleted = {},
                  bool selected = false,
                  Color selectedColor = Color::fromRGB(0.0, 0.45, 0.95),
                  KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), label_(std::move(label)),
          backgroundColor_(backgroundColor), radius_(radius), padding_(padding),
          avatar_(std::move(avatar)), deleteIcon_(deleteIcon),
          onDeleted_(std::move(onDeleted)), selected_(selected),
          selectedColor_(selectedColor) {}

    const SharedPtr<Widget>& label() const { return label_; }
    const SharedPtr<Widget>& avatar() const { return avatar_; }
    Color backgroundColor() const { return backgroundColor_; }
    Color selectedColor() const { return selectedColor_; }
    double radius() const { return radius_; }
    EdgeInsets padding() const { return padding_; }
    bool selected() const { return selected_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        std::vector<SharedPtr<Widget>> row;
        if (avatar_)
            row.push_back(avatar_);
        row.push_back(label_);
        if (onDeleted_) {
            if (deleteIcon_ == IconCode::None)
                row.push_back(cui::make_shared<SizedBox>(4.0, 0.0));
            row.push_back(cui::make_shared<IconButton>(
                deleteIcon_ == IconCode::None ? IconCode::Close : deleteIcon_,
                onDeleted_, 18.0));
        }
        auto r = cui::make_shared<Row>(std::move(row));
        r->setMainAxisSize(MainAxisSize::Min);
        auto c = cui::make_shared<Container>(r);
        c->padding(padding_)
            .decoration(BoxDecoration{selected_ ? selectedColor_ : backgroundColor_,
                                      radius_});
        return c;
    }

    std::string toStringShort() const override { return "Chip"; }

private:
    SharedPtr<Widget> label_;
    Color backgroundColor_;
    double radius_;
    EdgeInsets padding_;
    SharedPtr<Widget> avatar_;
    IconCode deleteIcon_;
    VoidCallback onDeleted_;
    bool selected_;
    Color selectedColor_;
};

// ── LinearProgressIndicator ─────────────────────────────────────────────────

/// 线性进度条。上游的 `LinearProgressIndicator`（progress_indicator.dart）。
///
/// value 为 nullopt 表示 indeterminate：静态画满整条（上游 无 value 时由
/// 动画驱动，这里用填满近似，保证 headless 可测、可见）。
class LinearProgressIndicator : public LeafRenderObjectWidget {
public:
    explicit LinearProgressIndicator(std::optional<double> value = std::nullopt,
                                     Color backgroundColor = Color::fromRGBA(0, 0, 0, 0.12),
                                     Color valueColor = Color::fromRGB(0, 0, 0),
                                     double minHeight = 4.0, KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)), value_(value),
          backgroundColor_(backgroundColor), valueColor_(valueColor),
          minHeight_(minHeight) {}

    const std::optional<double>& value() const { return value_; }
    Color backgroundColor() const { return backgroundColor_; }
    Color valueColor() const { return valueColor_; }
    double minHeight() const { return minHeight_; }

    RenderObject* createRenderObject() override {
        return new RenderLinearProgressIndicator(value_ ? *value_ : -1.0, minHeight_,
                                                  backgroundColor_, valueColor_);
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderLinearProgressIndicator*>(ro);
        if (!r)
            return;
        r->setValue(value_ ? *value_ : -1.0);
        r->setMinHeight(minHeight_);
        r->setBackgroundColor(backgroundColor_);
        r->setValueColor(valueColor_);
    }

    std::string toStringShort() const override { return "LinearProgressIndicator"; }

private:
    std::optional<double> value_;
    Color backgroundColor_;
    Color valueColor_;
    double minHeight_;
};

// ── CircularProgressIndicator ───────────────────────────────────────────────

/// 环形进度条。上游的 `CircularProgressIndicator`（progress_indicator.dart）。
class CircularProgressIndicator : public LeafRenderObjectWidget {
public:
    explicit CircularProgressIndicator(std::optional<double> value = std::nullopt,
                                       Color backgroundColor = Color::fromRGBA(0, 0, 0, 0.12),
                                       Color valueColor = Color::fromRGB(0, 0, 0),
                                       double strokeWidth = 4.0, KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)), value_(value),
          backgroundColor_(backgroundColor), valueColor_(valueColor),
          strokeWidth_(strokeWidth) {}

    const std::optional<double>& value() const { return value_; }
    Color backgroundColor() const { return backgroundColor_; }
    Color valueColor() const { return valueColor_; }
    double strokeWidth() const { return strokeWidth_; }

    RenderObject* createRenderObject() override {
        return new RenderCircularProgressIndicator(value_ ? *value_ : -1.0, strokeWidth_,
                                                    backgroundColor_, valueColor_);
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderCircularProgressIndicator*>(ro);
        if (!r)
            return;
        r->setValue(value_ ? *value_ : -1.0);
        r->setStrokeWidth(strokeWidth_);
        r->setBackgroundColor(backgroundColor_);
        r->setValueColor(valueColor_);
    }

    std::string toStringShort() const override { return "CircularProgressIndicator"; }

private:
    std::optional<double> value_;
    Color backgroundColor_;
    Color valueColor_;
    double strokeWidth_;
};

}  // namespace cui_core
