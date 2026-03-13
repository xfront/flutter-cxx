/// @file interactive.cpp
/// @brief interactive.h 中 >6 行函数体的实现（受控控件 + 按钮）。

#include "cui/widgets/interactive.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/widgets/text.h"

namespace cui {

namespace detail {

void CheckboxBox::updateRenderObject(RenderObject* ro) {
    auto* r = dynamic_cast<RenderCheckbox*>(ro);
    if (!r)
        return;
    r->setValue(value_);
    r->setActiveColor(active_);
    r->setCheckColor(check_);
    r->setInactiveColor(inactive_);
    r->setSizePx(size_);
}

void SwitchBox::updateRenderObject(RenderObject* ro) {
    auto* r = dynamic_cast<RenderSwitch*>(ro);
    if (!r)
        return;
    r->setValue(value_);
    r->setActiveColor(active_);
    r->setInactiveColor(inactive_);
}

void RadioBox::updateRenderObject(RenderObject* ro) {
    auto* r = dynamic_cast<RenderRadio*>(ro);
    if (!r)
        return;
    r->setSelected(selected_);
    r->setActiveColor(active_);
    r->setInactiveColor(inactive_);
    r->setSizePx(size_);
}

RenderObject* SliderBox::createRenderObject() {
    auto* r = new RenderSlider(value_, min_, max_, active_, inactive_, thumb_,
                               height_, thumbR_);
    r->setOnChanged(onChanged_);
    r->setLabel(label_);
    return r;
}

void SliderBox::updateRenderObject(RenderObject* ro) {
    auto* r = dynamic_cast<RenderSlider*>(ro);
    if (!r)
        return;
    r->setValue(value_);
    r->setActiveColor(active_);
    r->setInactiveColor(inactive_);
    r->setThumbColor(thumb_);
    r->setOnChanged(onChanged_);
    r->setLabel(label_);
}

}  // namespace detail

// ── Checkbox ───────────────────────────────────────────────────────────────
SharedPtr<Widget> Checkbox::build(BuildContext& context) {
    (void)context;
    auto ro = cui::make_shared<detail::CheckboxBox>(value_, activeColor_,
                                                    checkColor_, inactiveColor_,
                                                    size_);
    bool v = value_;
    auto oc = onChanged_;
    // 受控：点击翻转 value 并回写父级；禁用时不响应。
    return cui::make_shared<GestureDetector>(
        ro, [v, oc]() {
            if (oc)
                oc(!v);
        });
}

// ── Switch ─────────────────────────────────────────────────────────────────
SharedPtr<Widget> Switch::build(BuildContext& context) {
    (void)context;
    auto ro = cui::make_shared<detail::SwitchBox>(value_, activeColor_,
                                                  inactiveColor_, width_, height_,
                                                  thumbRadius_);
    bool v = value_;
    auto oc = onChanged_;
    return cui::make_shared<GestureDetector>(
        ro, [v, oc]() {
            if (oc)
                oc(!v);
        });
}

// ── Radio ──────────────────────────────────────────────────────────────────
SharedPtr<Widget> Radio::build(BuildContext& context) {
    (void)context;
    auto ro = cui::make_shared<detail::RadioBox>(selected_, activeColor_,
                                                 inactiveColor_, size_);
    bool sel = selected_;
    auto oc = onChanged_;
    return cui::make_shared<GestureDetector>(
        ro, [sel, oc]() {
            // 单选：仅在未选中时被点选时回写为 true（上游 由父级
            // 维护 group value；这里仅上报「被选中」）。
            if (oc && !sel)
                oc(true);
        });
}

// ── Slider ─────────────────────────────────────────────────────────────────
SharedPtr<Widget> Slider::build(BuildContext& context) {
    (void)context;
    // 拖拽由 RenderSlider 自处理（见 render_interactive.cpp）。
    return cui::make_shared<detail::SliderBox>(value_, min_, max_, activeColor_,
                                               inactiveColor_, thumbColor_, height_,
                                               thumbRadius_, label_, onChanged_);
}

// ── Buttons ────────────────────────────────────────────────────────────────
SharedPtr<Widget> ElevatedButton::build(BuildContext& context) {
    (void)context;
    const bool enabled = static_cast<bool>(onPressed_);
    const Color fill =
        enabled ? theme().primary
                : Color(theme().textPrimary.r, theme().textPrimary.g,
                        theme().textPrimary.b, 0.12);
    BoxDecoration deco{fill, 4.0};
    auto label = cui::make_shared<DefaultTextStyle>(
        TextStyle().fgColor(enabled ? theme().textOnPrimary
                                    : theme().textDisabled),
        child_);
    auto chrome = makeButtonChrome(label, deco);
    auto op = onPressed_;
    return cui::make_shared<GestureDetector>(chrome, [op]() {
        if (op)
            op();
    });
}

SharedPtr<Widget> TextButton::build(BuildContext& context) {
    (void)context;
    BoxDecoration deco{Color::fromRGBA(0, 0, 0, 0), 0.0};
    auto label =
        onPressed_
            ? child_
            : cui::make_shared<DefaultTextStyle>(
                  TextStyle().fgColor(theme().textDisabled), child_);
    auto chrome = makeButtonChrome(label, deco);
    auto op = onPressed_;
    return cui::make_shared<GestureDetector>(chrome, [op]() {
        if (op)
            op();
    });
}

SharedPtr<Widget> OutlinedButton::build(BuildContext& context) {
    (void)context;
    BoxDecoration deco{Color::fromRGBA(0, 0, 0, 0), 4.0, 1.0, borderColor_};
    auto label =
        onPressed_
            ? child_
            : cui::make_shared<DefaultTextStyle>(
                  TextStyle().fgColor(theme().textDisabled), child_);
    auto chrome = makeButtonChrome(label, deco);
    auto op = onPressed_;
    return cui::make_shared<GestureDetector>(chrome, [op]() {
        if (op)
            op();
    });
}

}  // namespace cui_core
