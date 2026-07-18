#pragma once
/// @file widgets/interactive.h
/// @brief 交互式 Material 控件（依赖 GestureDetector / 自处理指针事件）。
///
/// 对应 上游源码：
///   - Checkbox / Switch / Radio（material/ 下对应文件）
///   - Slider（material/slider.dart）
///   - ElevatedButton / TextButton / OutlinedButton（material/button.dart）
///
/// 设计要点：
///   - Checkbox / Switch / Radio 是「受控」控件：value 由父级持有，
///     onChanged 回调把新值交回父级（与 上游 一致）；onChanged 为 null
///     时视为禁用（视觉置灰、不响应点击）。
///   - Slider 的 RenderObject 自处理指针事件并直接回调 onChanged，因此无需
///     GestureDetector 包裹（避免与 tap 识别器抢事件）。
///   - 三个 Button 由 Container(装饰) + GestureDetector 组合，无 Icon 依赖；
///     文本标签直接传 child（通常是 Text）。

#include <functional>
#include <memory>
#include <string>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/theme.h"
#include "cui/framework/framework.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_interactive.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"

namespace cui {

/// value 变化回调（上游的 ValueChanged<T>）。
template <typename T>
using ValueChanged = std::function<void(T)>;

namespace detail {

// 各控件的叶子渲染 Widget（携带 value/颜色，供 create/update render object）。
class CheckboxBox : public LeafRenderObjectWidget {
public:
    CheckboxBox(bool v, Color active, Color check, Color inactive, double size)
        : value_(v), active_(active), check_(check), inactive_(inactive),
          size_(size) {}
    RenderObject* createRenderObject() override {
        return new RenderCheckbox(value_, active_, check_, inactive_, size_);
    }
    void updateRenderObject(RenderObject* ro) override;

private:
    bool value_;
    Color active_, check_, inactive_;
    double size_;
};

class SwitchBox : public LeafRenderObjectWidget {
public:
    SwitchBox(bool v, Color active, Color inactive, double w, double h, double tr)
        : value_(v), active_(active), inactive_(inactive), width_(w), height_(h),
          thumbRadius_(tr) {}
    RenderObject* createRenderObject() override {
        return new RenderSwitch(value_, active_, inactive_, width_, height_,
                                thumbRadius_);
    }
    void updateRenderObject(RenderObject* ro) override;

private:
    bool value_;
    Color active_, inactive_;
    double width_, height_, thumbRadius_;
};

class RadioBox : public LeafRenderObjectWidget {
public:
    RadioBox(bool sel, Color active, Color inactive, double size)
        : selected_(sel), active_(active), inactive_(inactive), size_(size) {}
    RenderObject* createRenderObject() override {
        return new RenderRadio(selected_, active_, inactive_, size_);
    }
    void updateRenderObject(RenderObject* ro) override;

private:
    bool selected_;
    Color active_, inactive_;
    double size_;
};

class SliderBox : public LeafRenderObjectWidget {
public:
    SliderBox(double value, double min, double max, Color active, Color inactive,
              Color thumb, double height, double thumbR, std::string label,
              ValueChanged<double> oc)
        : value_(value), min_(min), max_(max), active_(active), inactive_(inactive),
          thumb_(thumb), height_(height), thumbR_(thumbR), label_(std::move(label)),
          onChanged_(std::move(oc)) {}
    RenderObject* createRenderObject() override;
    void updateRenderObject(RenderObject* ro) override;

private:
    double value_, min_, max_, height_, thumbR_;
    Color active_, inactive_, thumb_;
    std::string label_;
    ValueChanged<double> onChanged_;
};

}  // namespace detail

// ── Checkbox ─────────────────────────────────────────────────────────────────
class Checkbox : public StatelessWidget {
public:
    explicit Checkbox(bool value, ValueChanged<bool> onChanged = {},
                      Color activeColor = theme().primary,
                      Color checkColor = theme().textOnPrimary,
                      Color inactiveColor = theme().textDisabled,
                      double size = 18.0, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)),
          value_(value), onChanged_(std::move(onChanged)),
          activeColor_(activeColor), checkColor_(checkColor),
          inactiveColor_(inactiveColor), size_(size) {}

    bool value() const { return value_; }
    const ValueChanged<bool>& onChanged() const { return onChanged_; }

    SharedPtr<Widget> build(BuildContext& context) override;

    std::string toStringShort() const override { return "Checkbox"; }

private:
    bool value_;
    ValueChanged<bool> onChanged_;
    Color activeColor_, checkColor_, inactiveColor_;
    double size_;
};

// ── Switch ────────────────────────────────────────────────────────────────────
class Switch : public StatelessWidget {
public:
    explicit Switch(bool value, ValueChanged<bool> onChanged = {},
                    Color activeColor = theme().primary,
                    Color inactiveColor = theme().textDisabled,
                    double width = 40.0, double height = 24.0,
                    double thumbRadius = 10.0, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)),
          value_(value), onChanged_(std::move(onChanged)),
          activeColor_(activeColor), inactiveColor_(inactiveColor), width_(width),
          height_(height), thumbRadius_(thumbRadius) {}

    bool value() const { return value_; }
    const ValueChanged<bool>& onChanged() const { return onChanged_; }

    SharedPtr<Widget> build(BuildContext& context) override;

    std::string toStringShort() const override { return "Switch"; }

private:
    bool value_;
    ValueChanged<bool> onChanged_;
    Color activeColor_, inactiveColor_;
    double width_, height_, thumbRadius_;
};

// ── Radio ──────────────────────────────────────────────────────────────────────
class Radio : public StatelessWidget {
public:
    explicit Radio(bool selected, ValueChanged<bool> onChanged = {},
                   Color activeColor = theme().primary,
                   Color inactiveColor = theme().textDisabled,
                   double size = 18.0, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)),
          selected_(selected), onChanged_(std::move(onChanged)),
          activeColor_(activeColor), inactiveColor_(inactiveColor), size_(size) {}

    bool selected() const { return selected_; }
    const ValueChanged<bool>& onChanged() const { return onChanged_; }

    SharedPtr<Widget> build(BuildContext& context) override;

    std::string toStringShort() const override { return "Radio"; }

private:
    bool selected_;
    ValueChanged<bool> onChanged_;
    Color activeColor_, inactiveColor_;
    double size_;
};

// ── Slider ────────────────────────────────────────────────────────────────────
class Slider : public StatelessWidget {
public:
    explicit Slider(double value, ValueChanged<double> onChanged = {}, double min = 0.0,
                    double max = 1.0,
                    Color activeColor = theme().primary,
                    Color inactiveColor = theme().textDisabled,
                    Color thumbColor = theme().primary,
                    double height = 20.0, double thumbRadius = 10.0,
                    std::string label = {}, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)),
          value_(value), onChanged_(std::move(onChanged)), min_(min), max_(max),
          activeColor_(activeColor), inactiveColor_(inactiveColor),
          thumbColor_(thumbColor), height_(height), thumbRadius_(thumbRadius),
          label_(std::move(label)) {}

    double value() const { return value_; }
    const ValueChanged<double>& onChanged() const { return onChanged_; }
    const std::string& label() const { return label_; }

    SharedPtr<Widget> build(BuildContext& context) override;

    std::string toStringShort() const override { return "Slider"; }

private:
    double value_, min_, max_;
    ValueChanged<double> onChanged_;
    Color activeColor_, inactiveColor_, thumbColor_;
    double height_, thumbRadius_;
    std::string label_;
};

// ── Buttons ─────────────────────────────────────────────────────────────────────
namespace {

inline SharedPtr<Widget> makeButtonChrome(SharedPtr<Widget> child,
                                                 const BoxDecoration& deco) {
    auto c = cui::make_shared<Container>(child);
    c->padding(EdgeInsets::symmetric(12.0, 16.0)).decoration(deco);
    return c;
}

}  // namespace

/// 凸起按钮。上游的 `ElevatedButton`（button.dart）。
class ElevatedButton : public StatelessWidget {
public:
    using VoidCallback = std::function<void()>;

    explicit ElevatedButton(SharedPtr<Widget> child, VoidCallback onPressed = {},
                            KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)),
          onPressed_(std::move(onPressed)) {}

    const SharedPtr<Widget>& child() const { return child_; }

    SharedPtr<Widget> build(BuildContext& context) override;

    std::string toStringShort() const override { return "ElevatedButton"; }

private:
    SharedPtr<Widget> child_;
    VoidCallback onPressed_;
};

/// 文字按钮。上游的 `TextButton`（button.dart）。
class TextButton : public StatelessWidget {
public:
    using VoidCallback = std::function<void()>;

    explicit TextButton(SharedPtr<Widget> child, VoidCallback onPressed = {},
                        KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)),
          onPressed_(std::move(onPressed)) {}

    const SharedPtr<Widget>& child() const { return child_; }

    SharedPtr<Widget> build(BuildContext& context) override;

    std::string toStringShort() const override { return "TextButton"; }

private:
    SharedPtr<Widget> child_;
    VoidCallback onPressed_;
};

/// 描边按钮。上游的 `OutlinedButton`（button.dart）。
class OutlinedButton : public StatelessWidget {
public:
    using VoidCallback = std::function<void()>;

    explicit OutlinedButton(SharedPtr<Widget> child, VoidCallback onPressed = {},
                            Color borderColor = theme().textDisabled,
                            KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)),
          onPressed_(std::move(onPressed)), borderColor_(borderColor) {}

    const SharedPtr<Widget>& child() const { return child_; }

    SharedPtr<Widget> build(BuildContext& context) override;

    std::string toStringShort() const override { return "OutlinedButton"; }

private:
    SharedPtr<Widget> child_;
    VoidCallback onPressed_;
    Color borderColor_;
};

}  // namespace cui_core
