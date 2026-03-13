#pragma once
/// @file widgets/combobox.h
/// @brief 可搜索下拉选择框（上游的 RawAutocomplete / Autocomplete）。
///
/// 对应 上游源码：
///   上游 widgets/autocomplete.dart（`RawAutocomplete`）
///   上游 material/autocomplete.dart（`Autocomplete`：默认选项视图 + 200px 上限）
///
/// 行为契约（与上游一致）：
///   - 输入框文本 = 过滤查询；焦点进入且过滤结果非空 ⇒ 展开选项浮层
///     （上游 `_canShowOptionsView`）；
///   - ↑/↓（+ PgUp/PgDn、Ctrl+↑/↓ = 首/末项）移动高亮，Enter 选中高亮项，
///     Esc 收起；这些快捷键**只在浮层展开时**生效，否则原样放行给祖先
///     （上游 `_AutocompleteCallbackAction.isEnabledCallback`）；
///   - 选中后把输入框文本写成该项（上游 `_textFieldControllerComplete`）并收起；
///   - 点击浮层外任意处（barrier）收起。
///
/// 与上游的偏离（有意，勿当 bug 排查）：
///   1. 上游把导航键搬到 `Shortcuts` + `Actions`（intent 层）。本仓还没有 intent/
///      action 框架，所以导航键直接挂在输入框自己的 FocusNode 上
///      （`FocusNode::addKeyHandler` ≈ 上游 `registerKeyHandler(node, owner)`：同一
///      节点多个处理器按注册序派发，输入框先吃字符键，它返回 Ignored 的
///      ↑/↓/Enter/Esc 才轮到本控件）。
///      因此**不能**再外包一层 `Focus` 控件：本仓的 `Focus` 用单槽 `onKeyEvent`，
///      会把输入框的处理器顶掉。
///   2. 浮层走仓内既有的 `OverlayEntry` + 全屏透明 barrier 路子（popup_menu.h 先例），
///      不是上游的 `OverlayPortal`；定位用输入框渲染盒的全局坐标（上游用
///      `OverlayChildLayoutInfo.childPaintTransform`）。
///   3. 选项视图高度 = `kMaxVisibleRows` 行（上游 `Autocomplete.optionsMaxHeight`
///      = 200px）；高亮项落到窗口外时平移窗口。上游用 `ensureVisible` 的 post-frame
///      回调滚动，本仓此处还没接 post-frame，等价实现为窗口平移。
///   4. 过滤是**大小写不敏感子串**匹配（对齐 rui story 的 “Type to filter”），上游
///      `Autocomplete` 的示例用的是前缀匹配。
///   5. 宽度由 `width` 参数给定（上游靠字段自身的约束）。只有右侧箭头区可点合/开：
///      点输入框本身只会上焦（焦点驱动展开），把整盒做成开关反而会被按下时的
///      requestFocus 抢先展开、抬起时又被关掉。

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"
#include "cui/platform/event.h"
#include "cui/rendering/render_box.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/framework/focus.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/icons.h"
#include "cui/framework/listener.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/text.h"
#include "cui/widgets/text_input.h"

namespace cui {

namespace combobox_detail {
/// ASCII 小写（过滤用；UTF-8 多字节序列原样透传，不参与大小写折叠）。
inline std::string toLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}
}  // namespace combobox_detail

/// 可搜索下拉选择框。上游的 `Autocomplete<String>`（选项即字符串）。
class ComboBox : public StatefulWidget {
public:
    explicit ComboBox(std::vector<std::string> items, int selectedIndex = -1,
                      std::string placeholder = "Select...",
                      std::function<void(int index)> onChanged = {},
                      double width = 240.0, KeyPtr key = nullptr,
                      SharedPtr<TextEditingController> controller = nullptr,
                      SharedPtr<FocusNode> focusNode = nullptr)
        : StatefulWidget(std::move(key)), items_(std::move(items)),
          selectedIndex_(selectedIndex), placeholder_(std::move(placeholder)),
          onChanged_(std::move(onChanged)), width_(width),
          controller_(std::move(controller)), focusNode_(std::move(focusNode)) {}

    const std::vector<std::string>& items() const { return items_; }
    int selectedIndex() const { return selectedIndex_; }
    const std::string& placeholder() const { return placeholder_; }
    /// 上游 `Autocomplete.onSelected`（这里的下标是 items_ 的原始下标）。
    const std::function<void(int)>& onChanged() const { return onChanged_; }
    double width() const { return width_; }
    /// 上游 `Autocomplete.textEditingController` / `focusNode`：外部注入则跨重建持久化。
    const SharedPtr<TextEditingController>& controller() const { return controller_; }
    const SharedPtr<FocusNode>& focusNode() const { return focusNode_; }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "ComboBox"; }

private:
    std::vector<std::string> items_;
    int selectedIndex_;
    std::string placeholder_;
    std::function<void(int)> onChanged_;
    double width_;
    SharedPtr<TextEditingController> controller_;
    SharedPtr<FocusNode> focusNode_;
};

/// ComboBox 状态：持有控制器/焦点节点/浮层条目，并托管键盘导航。
class ComboBoxState : public State {
public:
    static constexpr int kMaxVisibleRows = 5;  ///< 浮层最多可见行数
    static constexpr int kPageSize = 4;        ///< 上游 `_pageSize`
    static constexpr double kRowHeight = 28.0;
    static constexpr double kBoxHeight = 34.0;
    static constexpr double kGap = 4.0;

    void initState() override {
        auto* w = combobox();
        controller_ = w->controller() ? w->controller()
                                      : cui::make_shared<TextEditingController>();
        focusNode_ = w->focusNode() ? w->focusNode() : cui::make_shared<FocusNode>();

        selectedIndex_ = w->selectedIndex();
        if (selectedIndex_ >= 0 &&
            selectedIndex_ < static_cast<int>(w->items().size()))
            controller_->setText(w->items()[static_cast<size_t>(selectedIndex_)]);
        lastText_ = controller_->text();
        refreshOptions();

        // 上游 `_actionMap` 那一层：导航键挂在字段自己的节点上，且注册在
        // EditableText 之后 ⇒ 字符键先被输入框吃掉，它 Ignored 的键才轮到我们。
        keyHandlerId_ = focusNode_->addKeyHandler(
            [this](const KeyEvent& ev) { return handleKey(ev); });
        // 上游同时监听 `_focusNode`（可见性）和 `_textEditingController`（选项）。
        // 本仓的焦点态由 EditableText 镜像进控制器，所以一个监听覆盖两者。
        controllerListenerId_ = controller_->addListener([this]() { onFieldChanged(); });
    }

    void dispose() override {
        // 监听和键处理器都捕获 this，而控制器/焦点节点可能被外部持有得更久：
        // 不摘就会在节点后续通知上回调已销毁的 State。
        if (keyHandlerId_ > 0) {
            focusNode_->removeKeyHandler(keyHandlerId_);
            keyHandlerId_ = 0;
        }
        if (controllerListenerId_ > 0) {
            controller_->removeListener(controllerListenerId_);
            controllerListenerId_ = 0;
        }
        setExpanded(false);
        State::dispose();
    }

    // ── 对外读取（测试与 gallery 用）──────────────────────────────────────
    bool isExpanded() const { return expanded_; }
    int highlightedIndex() const { return highlight_; }
    /// 过滤后的选项在 `items()` 里的原始下标（上游 `_options`）。
    const std::vector<int>& options() const { return options_; }
    int selectedIndex() const { return selectedIndex_; }
    ///@{ 状态实际使用的控制器/焦点节点（widget 未注入时是状态自己建的）。
    const SharedPtr<TextEditingController>& controller() const { return controller_; }
    const SharedPtr<FocusNode>& focusNode() const { return focusNode_; }
    ///@}
    /// 编程选中（等价于用户点选：写回文本、发回调、收起浮层）。
    void setSelectedIndex(int index) { selectItem(index); }

    SharedPtr<Widget> build(BuildContext&) override {
        auto* w = combobox();
        const Color kBorder = theme().border;
        const Color kAccent = theme().primary;
        const Color kText = theme().textPrimary;

        auto field = cui::make_shared<EditableText>(
            controller_, TextStyle().size(12.5).fgColor(kText), TextAlign::Left,
            w->placeholder(), /*obscureText=*/false, /*autofocus=*/false, nullptr,
            focusNode_);

        // 箭头区：唯一的点击开关（见文件头偏离 5）。
        auto chevron = cui::make_shared<Icon>(IconCode::ArrowDownward, 14.0,
                                              theme().textSecondary);
        auto arrowBox = cui::make_shared<Container>(chevron);
        arrowBox->width(24.0).height(kBoxHeight)
            .alignment(0.0, 0.0)
            .decoration(BoxDecoration{theme().surfaceVariant});
        auto arrowTap = cui::make_shared<GestureDetector>(arrowBox,
                                                          [this] { toggleExpanded(); });

        auto row = cui::make_shared<Row>(
            std::vector<SharedPtr<Widget>>{cui::make_shared<Expanded>(1, field),
                                           arrowTap});
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);

        const bool active = hasFocus_ || expanded_;
        auto box = cui::make_shared<Container>(row);
        box->width(w->width())
            .height(kBoxHeight)
            .padding(EdgeInsets::symmetric(8.0, 0.0))
            .decoration(BoxDecoration{theme().surface, 6.0,
                                      active ? 1.5 : 1.0,
                                      active ? kAccent : kBorder});
        return box;
    }

private:
    ComboBox* combobox() { return static_cast<ComboBox*>(widget()); }

    /// 上游 `_onChangedField` + `_onFocusChange`：文本变了才重算选项，
    /// 焦点变了才改可见性；两者都只经由控制器的一个通知进来。
    void onFieldChanged() {
        if (selecting_)
            return;  // 上游 `_selecting`：选中时程序化写文本不该重新过滤
        const bool focused = focusNode_->hasFocus();
        if (focused != hasFocus_) {
            hasFocus_ = focused;
            // 重新获得焦点 ⇒ 撤掉「手动收起」闩锁（上游的可见性纯由焦点推）。
            if (focused)
                dismissedByUser_ = false;
        }
        const std::string text = controller_->text();
        if (text != lastText_) {
            lastText_ = text;
            dismissedByUser_ = false;
            refreshOptions();
        }
        setExpanded(hasFocus_ && !options_.empty() && !dismissedByUser_);
        setState();
    }

    /// 大小写不敏感子串过滤，得到原始下标序列（上游 `_options`）。
    void refreshOptions() {
        const auto& items = combobox()->items();
        const std::string query = combobox_detail::toLower(controller_->text());
        // 文本正好等于选中项时算「已选定」而不是「过滤到只剩它」，否则一进控件
        // 就只能看见那一条，rui 侧同样是这么处理的（query 为空 ⇒ 全量）。
        const bool showingSelection =
            selectedIndex_ >= 0 &&
            selectedIndex_ < static_cast<int>(items.size()) &&
            controller_->text() == items[static_cast<size_t>(selectedIndex_)];
        options_.clear();
        const bool filtering = !query.empty() && !showingSelection;
        for (size_t i = 0; i < items.size(); ++i) {
            if (!filtering ||
                combobox_detail::toLower(items[i]).find(query) != std::string::npos)
                options_.push_back(static_cast<int>(i));
        }
        // 上游 `_updateHighlight(_highlightedOptionIndex.value)`：集合变了先夹住高亮。
        clampHighlight();
    }

    void clampHighlight() {
        highlight_ = options_.empty()
                         ? 0
                         : std::clamp(highlight_, 0,
                                      static_cast<int>(options_.size()) - 1);
    }

    /// 上游 `_highlightOption`：移动高亮（夹在集合内）并把窗口平移到含高亮项。
    void highlightTo(int index) {
        highlight_ = std::clamp(index, 0, static_cast<int>(options_.size()) - 1);
        if (windowStart_ > highlight_)
            windowStart_ = highlight_;
        else if (highlight_ >= windowStart_ + kMaxVisibleRows)
            windowStart_ = highlight_ - kMaxVisibleRows + 1;
        if (entry_ && entry_->mounted())
            entry_->markNeedsBuild();
    }

    /// 展开/收起浮层（唯一写 `expanded_` 的地方）。
    void setExpanded(bool show) {
        if (show && options_.empty())
            show = false;
        if (expanded_ == show)
            return;
        expanded_ = show;
        if (expanded_) {
            highlight_ = 0;
            windowStart_ = 0;
            if (!insertEntry())
                expanded_ = false;  // 树上没有上层 Overlay ⇒ 弹不出来，别留下假状态
        } else {
            removeEntry();
        }
    }

    /// 上游 `_select`：写回文本、发回调、收起浮层。
    void selectItem(int itemIndex) {
        const auto& items = combobox()->items();
        if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size()))
            return;
        const bool changed = itemIndex != selectedIndex_;
        selectedIndex_ = itemIndex;
        selecting_ = true;
        controller_->setText(items[static_cast<size_t>(itemIndex)]);
        lastText_ = controller_->text();
        selecting_ = false;
        dismissedByUser_ = true;
        setExpanded(false);
        if (changed && combobox()->onChanged())
            combobox()->onChanged()(itemIndex);
        setState();
    }

    /// 箭头区点击：展开中 ⇒ 收起并挂手动闩锁；否则先聚焦（焦点驱动展开）。
    void toggleExpanded() {
        if (expanded_) {
            dismissedByUser_ = true;
            setExpanded(false);
        } else {
            dismissedByUser_ = false;
            focusNode_->requestFocus();
            hasFocus_ = focusNode_->hasFocus();
            setExpanded(hasFocus_ && !options_.empty());
        }
        setState();
    }

    KeyEventResult handleKey(const KeyEvent& ev) {
        // 上游 isEnabledCallback：浮层没展开时一个键都不接，交给祖先
        // （滚动视图据此用方向键滚动、页面据此接 Enter）。
        if (!expanded_)
            return KeyEventResult::Ignored;
        const bool toEdge = (ev.modifiers & (ModCtrl | ModMeta)) != 0;
        switch (static_cast<KeyCode>(ev.key)) {
            case KeyCode::Down:
                highlightTo(toEdge ? static_cast<int>(options_.size()) - 1
                                   : highlight_ + 1);
                return KeyEventResult::Handled;
            case KeyCode::Up:
                highlightTo(toEdge ? 0 : highlight_ - 1);
                return KeyEventResult::Handled;
            case KeyCode::PageDown:
                highlightTo(highlight_ + kPageSize);
                return KeyEventResult::Handled;
            case KeyCode::PageUp:
                highlightTo(highlight_ - kPageSize);
                return KeyEventResult::Handled;
            case KeyCode::Return:
                if (!options_.empty())
                    selectItem(options_[static_cast<size_t>(highlight_)]);
                return KeyEventResult::Handled;
            case KeyCode::Escape:
                dismissedByUser_ = true;
                setExpanded(false);
                setState();
                return KeyEventResult::Handled;
            default:
                return KeyEventResult::Ignored;
        }
    }

    // ── 浮层 ────────────────────────────────────────────────────────────────
    /// 成功挂上条目才返回 true（`OverlayState::of` 找不到会抛，这里用 maybeOf）。
    bool insertEntry() {
        if (entry_ && entry_->mounted())
            return true;
        OverlayState* overlay = OverlayState::maybeOf(*context());
        if (!overlay)
            return false;
        auto* self = this;
        entry_ = cui::make_shared<OverlayEntry>(
            [self](BuildContext& ctx) { return self->buildOptionsView(ctx); },
            /*opaque=*/false, /*maintainState=*/false);
        overlay->insert(entry_);
        return true;
    }

    void removeEntry() {
        if (entry_ && entry_->mounted())
            entry_->remove();
        entry_.reset();
    }

    /// 输入框渲染盒在**浮层坐标系**里的位置（上游用 OverlayChildLayoutInfo 的
    /// child 变换）。
    ///
    /// `Positioned` 的偏移是相对 Overlay 那层 Stack 的，而 Stack 未必落在窗口
    /// 原点（gallery 里它在侧栏 + ribbon 之内）⇒ 直接把 `localToGlobal` 的
    /// 结果当偏移用会整体右下平移一个 Stack 原点。
    ///
    /// Stack 的渲染盒从**条目自己的 context** 往上找（上游 `Overlay` 也是这么
    /// 定位子条目的）；从 `OverlayState::context()` 找不行——条目 builder 正是在
    /// Overlay 的 build 期间被调用，那一刻它的子树还没挂上渲染对象。
    Rect fieldRectInOverlay(BuildContext& entryContext) const {
        Element* e = context() ? context()->element() : nullptr;
        auto* box = e ? dynamic_cast<RenderBox*>(e->findRenderObject()) : nullptr;
        if (!box)
            return Rect{};
        Offset origin = box->localToGlobal(Offset{});
        Element* host = entryContext.element();
        if (auto* roHost = host ? host->ancestorRenderObjectElement() : nullptr) {
            if (auto* stack = dynamic_cast<RenderBox*>(roHost->findRenderObject()))
                origin = stack->globalToLocal(origin);
        }
        return Rect{origin.dx, origin.dy, box->size().width, box->size().height};
    }

    SharedPtr<Widget> buildOptionsView(BuildContext& entryContext) {
        const Rect field = fieldRectInOverlay(entryContext);
        const int visible = std::max(
            0, std::min(kMaxVisibleRows,
                        static_cast<int>(options_.size()) - windowStart_));

        std::vector<SharedPtr<Widget>> stack;

        // 全屏透明 barrier：点浮层外任意处收起（上游的 TextFieldTapRegion）。
        auto barrierBox = cui::make_shared<Container>();
        barrierBox->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0)});
        auto barrier = cui::make_shared<GestureDetector>(barrierBox, [this] {
            dismissedByUser_ = true;
            setExpanded(false);
            setState();
        });
        auto fill = cui::make_shared<Positioned>(barrier);
        fill->hasLeft = fill->hasTop = fill->hasRight = fill->hasBottom = true;
        stack.push_back(fill);

        std::vector<SharedPtr<Widget>> rows;
        for (int r = 0; r < visible; ++r) {
            const int optionIndex = windowStart_ + r;
            const int itemIndex = options_[static_cast<size_t>(optionIndex)];
            auto row = cui::make_shared<Container>(
                cui::make_shared<Text>(
                    combobox()->items()[static_cast<size_t>(itemIndex)],
                    TextStyle().size(12.5).fgColor(theme().textPrimary)));
            row->height(kRowHeight).padding(EdgeInsets::symmetric(10.0, 0.0));
            if (optionIndex == highlight_)
                row->decoration(BoxDecoration{theme().surfaceVariant});
            auto tapped = cui::make_shared<GestureDetector>(
                row, [this, itemIndex] { selectItem(itemIndex); });
            // 悬停即高亮（rui 侧 on_mouse_move 的语义）。
            rows.push_back(cui::make_shared<Listener>(
                nullptr, nullptr, nullptr,
                [this, optionIndex](const PointerEvent&) {
                    if (highlight_ != optionIndex)
                        highlightTo(optionIndex);
                },
                nullptr, tapped));
        }

        auto list = cui::make_shared<Column>(std::move(rows));
        list->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto card = cui::make_shared<Container>(list);
        card->width(field.width)
            .decoration(BoxDecoration{theme().surface, 6.0, 1.0,
                                      theme().border, 8.0,
                                      Color::fromRGBA(0, 0, 0, 0.18)});

        auto pos = cui::make_shared<Positioned>(card);
        pos->hasLeft = true;
        pos->left = field.x;
        pos->hasTop = true;
        pos->top = field.y + field.height + kGap;
        stack.push_back(pos);

        return cui::make_shared<Stack>(std::move(stack));
    }

    SharedPtr<TextEditingController> controller_;
    SharedPtr<FocusNode> focusNode_;
    std::vector<int> options_;
    int selectedIndex_ = -1;
    int highlight_ = 0;
    int windowStart_ = 0;
    bool expanded_ = false;
    bool hasFocus_ = false;
    bool dismissedByUser_ = false;
    bool selecting_ = false;
    std::string lastText_;
    int controllerListenerId_ = 0;
    int keyHandlerId_ = 0;
    SharedPtr<OverlayEntry> entry_;
};

inline std::unique_ptr<State> ComboBox::createState() {
    return std::make_unique<ComboBoxState>();
}

}  // namespace cui_core
