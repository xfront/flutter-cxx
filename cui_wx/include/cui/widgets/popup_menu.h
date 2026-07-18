#pragma once
/// @file widgets/popup_menu.h
/// @brief 弹出菜单（上游的 material 弹出菜单子集）。
///
/// 对应 上游源码：material/popup_menu.dart 的 `PopupMenuButton` /
/// `PopupMenuItem`（简化：条目为「label + int value」）。
///
/// 移植约定：
///   - 菜单不是自身 Stack 内的越界子树，而是经由**上层 Overlay**
///     （Navigator 自带）插入条目——与 上游的 _PopupMenuRoute 一致；
///   - 定位：对齐 上游 `showButtonMenu`，用**按钮渲染盒**算出
///     `RelativeRect`（菜单左上角 = 按钮左下角）；且偏移必须换算到
///     **浮层坐标系**——Overlay 那层 Stack 未必落在窗口原点；
///   - 打开时同条目内叠全屏透明 barrier：点击菜单外任意处关闭；
///   - 宽度：**默认与按钮同宽**，只有某条目的文字（含行内边距）比按钮更宽时
///     才变宽（同 DropdownButton 的口径）。上游 是 `ConstrainedBox(minWidth:
///     _kMenuMinWidth=192) + IntrinsicWidth(stepWidth: 56)`，即「菜单恒 ≥192、
///     按 56 步进」的 Material 规范，这里刻意不跟——本仓的菜单是给工具栏按钮
///     用的，192 起步会把窄按钮撑出一条大空卡。显式传 `menuWidth` 则固定用它。
///     宽度在打开那一刻按按钮盒现算，所以文本口径必须和条目绘制一致：两者共用
///     `PopupMenuList::itemStyle()` / `kItemPaddingH`。

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/theme.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/framework/listener.h"
#include "cui/rendering/render_box.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/text.h"

namespace cui {

/// 菜单条目。上游的 `PopupMenuItem<T>`（T 简化为 int）。
struct PopupMenuItem {
    std::string label;
    int value = 0;
};

/// 弹出菜单按钮。上游的 `PopupMenuButton`（popup_menu.dart）。
class PopupMenuButton : public StatefulWidget {
public:
    using SelectedCallback = std::function<void(int)>;

    explicit PopupMenuButton(std::vector<PopupMenuItem> items = {},
                             SelectedCallback onSelected = {},
                             SharedPtr<Widget> child = nullptr,
                             std::optional<double> menuWidth = std::nullopt,
                             KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), items_(std::move(items)),
          onSelected_(std::move(onSelected)), child_(std::move(child)),
          menuWidth_(menuWidth) {}

    const std::vector<PopupMenuItem>& items() const { return items_; }
    const SelectedCallback& onSelected() const { return onSelected_; }
    const SharedPtr<Widget>& child() const { return child_; }
    /// 固定菜单宽度；为空 = 自动（与按钮同宽，条目文字更长时才变宽）。
    std::optional<double> menuWidth() const { return menuWidth_; }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "PopupMenuButton"; }

private:
    std::vector<PopupMenuItem> items_;
    SelectedCallback onSelected_;
    SharedPtr<Widget> child_;
    std::optional<double> menuWidth_;
};

/// 菜单条目列表（自管理 hover 高亮）。独立于 PopupMenuButtonState：hover 变化
/// 由本 widget 自己的 State 重建，不依赖外层 OverlayEntry 的重建时机。
/// `menuWidth` 这里是**已定好的宽度**（由 PopupMenuButtonState 按「按钮同宽，
/// 条目文字更长时才变宽」算出来），不是配置项。
class PopupMenuList : public StatefulWidget {
public:
    explicit PopupMenuList(std::vector<PopupMenuItem> items,
                           std::function<void(int)> onSelected, double menuWidth = 160.0,
                           KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), items_(std::move(items)),
          onSelected_(std::move(onSelected)), menuWidth_(menuWidth) {}
    const std::vector<PopupMenuItem>& items() const { return items_; }
    const std::function<void(int)>& onSelected() const { return onSelected_; }
    double menuWidth() const { return menuWidth_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "PopupMenuList"; }

    /// 条目文字样式与行的水平内边距：宽度计算（PopupMenuButtonState）和这里
    /// 的绘制共用同一口径 ⇒ 改一处两边一起变，不会出现「算窄了文字被裁」或
    /// 「算宽了右边留白」。
    static TextStyle itemStyle() {
        return TextStyle().size(12.0).fgColor(theme().textPrimary);
    }
    static constexpr double kItemPaddingH = 8.0;   // 行左右各 8
    static constexpr double kItemPaddingV = 16.0;  // 行上下各 16

private:
    std::vector<PopupMenuItem> items_;
    std::function<void(int)> onSelected_;
    double menuWidth_ = 160.0;
};

class PopupMenuListState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        auto* self = this;
        auto* w = static_cast<PopupMenuList*>(widget());
        const Color kBg = theme().surface;
        const Color kSurfaceVariant = theme().surfaceVariant;  // hover 高亮

        std::vector<SharedPtr<Widget>> col;
        for (size_t i = 0; i < w->items().size(); ++i) {
            int value = w->items()[i].value;
            auto row = cui::make_shared<Container>(
                cui::make_shared<Text>(w->items()[i].label, PopupMenuList::itemStyle()));
            row->padding(EdgeInsets::symmetric(PopupMenuList::kItemPaddingH,
                                               PopupMenuList::kItemPaddingV))
                .decoration(BoxDecoration{hovered_ == static_cast<int>(i)
                                             ? kSurfaceVariant
                                             : Color::fromRGBA(0, 0, 0, 0)});
            int idx = static_cast<int>(i);
            auto g = cui::make_shared<GestureDetector>(row, [self, value, w] {
                if (w->onSelected())
                    w->onSelected()(value);
            });
            // Listener 跟踪 hover（无 exit 事件：移动到其它项会覆盖 hovered_）。
            auto listener = cui::make_shared<Listener>(
                nullptr, nullptr, nullptr, [self, idx](const PointerEvent&) {
                    if (self->hovered_ != idx) {
                        self->hovered_ = idx;
                        self->setState();
                    }
                }, nullptr, g);
            col.push_back(listener);
        }
        auto menuCol = cui::make_shared<Column>(std::move(col));
        menuCol->setMainAxisSize(MainAxisSize::Min);
        menuCol->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto card = cui::make_shared<Container>(menuCol);
        // 对齐 legacy PopupMenuButton::paintPopup：surface 底 + borderLight 边框 +
        //   radiusMedium(6) + 0.12 黑色阴影（偏移 2,2）。
        card->width(w->menuWidth())
            .decoration(BoxDecoration{kBg, 6.0, 1.0, theme().border,
                                     12.0, Color::fromRGBA(0, 0, 0, 0.18)});
        return card;
    }
    int hovered_ = -1;
};

inline std::unique_ptr<State> PopupMenuList::createState() {
    return std::make_unique<PopupMenuListState>();
}

/// PopupMenuButton 的状态：持有打开的 OverlayEntry（跨重建存活）。
class PopupMenuButtonState : public State {
public:
    bool isOpen() const { return entry_ && entry_->mounted(); }

    void initState() override {}

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        auto* self = this;
        auto* w = popupButton();
        // 上游的 onTapDown 只负责「记录按下点并弹菜单」；本移植定位改用按钮
        // 渲染盒（见 buttonRectInOverlay），按下点只作为拿不到盒子时的兜底。
        GestureDetector::PointerCallback onTapDown = [self](const PointerEvent& e) {
            self->openMenu(e.globalPosition);
        };
        return cui::make_shared<GestureDetector>(w->child(), nullptr, onTapDown);
    }

    void dispose() override {
        closeMenu();
        State::dispose();
    }

private:
    PopupMenuButton* popupButton() { return static_cast<PopupMenuButton*>(widget()); }

    void openMenu(const Offset& fallbackGlobalPos) {
        if (isOpen())
            return;
        openPos_ = fallbackGlobalPos;
        auto* self = this;
        entry_ = cui::make_shared<OverlayEntry>(
            [self](BuildContext& ctx) { return self->buildMenu(ctx); },
            /*opaque=*/false, /*maintainState=*/false);
        OverlayState::of(*context())->insert(entry_);
    }

    /// 关闭并清除条目。选中与外点关闭共用。
    void closeMenu() {
        if (entry_ && entry_->mounted())
            entry_->remove();
        entry_.reset();
    }

    /// 按钮渲染盒在**浮层坐标系**里的矩形（上游 `RelativeRect.fromRect` 的两个
    /// `localToGlobal(..., ancestor: overlay)`）。拿不到盒子时退回按下点。
    Rect buttonRectInOverlay() const {
        Element* e = context() ? context()->element() : nullptr;
        auto* box = e ? dynamic_cast<RenderBox*>(e->findRenderObject()) : nullptr;
        if (box) {
            const Offset origin = box->localToGlobal(Offset{});
            return Rect{origin.dx, origin.dy, box->size().width, box->size().height};
        }
        return Rect{openPos_.dx, openPos_.dy, 0.0, 0.0};
    }

    /// 菜单宽度：默认与按钮同宽；只有某条目的文字（含行左右内边距）比按钮更宽
    /// 时才变宽。显式传了 `PopupMenuButton::menuWidth` 就固定用它。
    /// 文本按 `Painter::measureTextWidth` 量（与 `RenderParagraph` 同一口径），
    /// 样式取 `PopupMenuList::itemStyle()` ⇒ 与条目绘制完全一致。
    double resolveMenuWidth(double buttonWidth) {
        if (std::optional<double> fixed = popupButton()->menuWidth(); fixed.has_value())
            return *fixed;
        const TextStyle style = PopupMenuList::itemStyle();
        const char* family = style.fontFamily.empty() ? nullptr : style.fontFamily.c_str();
        double width = buttonWidth;
        for (const PopupMenuItem& item : popupButton()->items())
            width = std::max(width, Painter::measureTextWidth(family, item.label, style.fontSize,
                                                              style.fontWeight, style.italic) +
                                        2.0 * PopupMenuList::kItemPaddingH);
        return width;
    }

    SharedPtr<Widget> buildMenu(BuildContext& entryContext) {
        auto* w = popupButton();
        const Rect button = buttonRectInOverlay();
        std::vector<SharedPtr<Widget>> stack;

        // 全屏透明 barrier：点击菜单外任意处关闭。
        auto barrierBox = cui::make_shared<Container>();
        barrierBox->decoration(BoxDecoration{Color{0, 0, 0, 0}});
        auto barrier = cui::make_shared<GestureDetector>(
            barrierBox, [this]() { closeMenu(); });
        auto fill = cui::make_shared<Positioned>(barrier);
        fill->hasLeft = fill->hasTop = fill->hasRight = fill->hasBottom = true;
        stack.push_back(fill);

        // 菜单本体（自管理 hover 高亮的条目列表）。
        auto list = cui::make_shared<PopupMenuList>(
            w->items(), [this](int v) { this->select(v); }, resolveMenuWidth(button.width));

        // 浮层 Stack 的渲染盒要从**条目自己的 context** 往上找：条目 builder 正是在
        // Overlay 的 build 期间被调用，那一刻从 `OverlayState::context()` 找不到子树。
        Element* host = entryContext.element();
        if (auto* roHost = host ? host->ancestorRenderObjectElement() : nullptr) {
            if (auto* overlay = dynamic_cast<RenderBox*>(roHost->findRenderObject())) {
                const Offset topLeft = overlay->globalToLocal(Offset{button.x, button.y});
                auto pos = cui::make_shared<Positioned>(list);
                pos->hasLeft = true;
                pos->left = topLeft.dx;
                pos->hasTop = true;
                pos->top = topLeft.dy + button.height + kMenuGap;
                stack.push_back(pos);
                return cui::make_shared<Stack>(std::move(stack));
            }
        }

        // 找不到浮层盒子（理论上不会发生）：菜单贴着 barrier 左上角摆放。
        stack.push_back(cui::make_shared<Positioned>(list));
        return cui::make_shared<Stack>(std::move(stack));
    }

    void select(int value) {
        closeMenu();
        if (popupButton()->onSelected())
            popupButton()->onSelected()(value);
    }

    /// 上游 `_kMenuScreenPadding` 一类的小间隙；本移植取 4。
    static constexpr double kMenuGap = 4.0;

    Offset openPos_{};
    SharedPtr<OverlayEntry> entry_;
};

inline std::unique_ptr<State> PopupMenuButton::createState() {
    return std::make_unique<PopupMenuButtonState>();
}

}  // namespace cui_core
