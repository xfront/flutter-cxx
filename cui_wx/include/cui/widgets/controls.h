#include <cmath>
#pragma once
/// @file widgets/controls.h
/// @brief 通用交互/导航/反馈控件（上游移植补充批）。
///
/// 把 legacy 的 SegmentedControl / Kbd / Link / SearchBar / DropdownButton /
/// RatingBar / AvatarGroup / NavigationRail / Drawer / PageView / Breadcrumb /
/// Pagination / Skeleton / EmptyState / ColorPicker / SnackBar / Notification /
/// Tabs 以 `cui_core` 三层重新实现，全部由已有原语（Container / Row /
/// Column / Stack / Text / GestureDetector / IndexedStack / Card）组合而成。
///
/// 对应 上游源码（material/*）：
///   - SegmentedControl（cupertino/segmented_control.dart）
///   - SearchBar（material/search_bar.dart）
///   - DropdownButton（material/dropdown.dart）
///   - RatingBar（material/rating_bar.dart，社区常用）
///   - NavigationRail（material/navigation_rail.dart）
///   - Drawer（material/drawer.dart）
///   - PageView（widgets/page_view.dart）
///   - Breadcrumb（material 无官方，breadcrumbs 包）
///   - Pagination（material 无官方，paginated_data_table 等）
///   - Skeleton（material 无官方）
///   - SnackBar（material/snack_bar.dart）

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/ticker.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/data.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/icons.h"
#include "cui/widgets/interactive.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/material.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/paint.h"
// scroll.h 的 ScrollableState 要 BuildOwner 的完整定义（tabs.h 同款前置）。
#include "cui/framework/build_owner.h"
#include "cui/widgets/scroll.h"  // CodeEditor 的横向/纵向滚动
#include "cui/widgets/tabs.h"
#include "cui/widgets/text.h"
#include "cui/widgets/text_input.h"
// RichTextEditor（对齐 Quill）独立成头：它不反向依赖本文件。
#include "cui/widgets/rich_text_editor.h"
// CodeEditor（对齐 code_forge）已拆成独立包（cui_core/code_editor/code_editor.h）：
// 引擎本体不依赖任何 package，用它的调用方自己 include。
#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box.h"

/// 同 data.h：为避免与 legacy 层同名类（cui_core::Splitter / cui_core::Form /
/// cui_core::MenuBar / cui_core::Link / cui_core::Drawer …）在同一二进制中重名（ODR），
/// 本文件的控件放在 `cui_core` 子命名空间。
namespace cui {

// ── SegmentedControl ───────────────────────────────────────────────────────────
/// 分段控件。上游的 `CupertinoSegmentedControl`（cupertino/segmented_control.dart）。
class SegmentedControl : public StatefulWidget {
public:
    explicit SegmentedControl(std::vector<std::string> options, int selected = 0,
                              std::function<void(int)> onChanged = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), options_(std::move(options)), selected_(selected),
          onChanged_(std::move(onChanged)) {}

    const std::vector<std::string>& options() const { return options_; }
    int selected() const { return selected_; }
    void setSelected(int s) { selected_ = s; }
    const std::function<void(int)>& onChanged() const { return onChanged_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SegmentedControl"; }

private:
    std::vector<std::string> options_;
    int selected_ = 0;
    std::function<void(int)> onChanged_;
};

class SegmentedControlState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    SegmentedControl* w() { return static_cast<SegmentedControl*>(widget()); }
    int hovered_ = -1;
};

// ── Kbd ───────────────────────────────────────────────────────────────────────
/// 键盘按键样式。上游 社区常用 `Kbd`（无官方）。
class Kbd : public StatelessWidget {
public:
    explicit Kbd(std::string label, KeyPtr key = nullptr) : StatelessWidget(std::move(key)), label_(std::move(label)) {}
    SharedPtr<Widget> build(BuildContext&) override {
        auto t = cui::make_shared<Text>(label_, TextStyle().size(11.0).bold());
        auto c = cui::make_shared<Container>(cui::make_shared<Center>(t));
        c->padding(EdgeInsets::symmetric(3.0, 7.0))
                .decoration(BoxDecoration{theme().surface, 4.0, 1.0, Color::fromRGBA(0, 0, 0, 0.25)});
        return c;
    }
    std::string toStringShort() const override { return "Kbd"; }

private:
    std::string label_;
};

// ── Link ──────────────────────────────────────────────────────────────────────
/// 链接文字。上游的 `Link`（widgets/link.dart）。
class Link : public StatelessWidget {
public:
    explicit Link(std::string text, std::function<void()> onTap = {}, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), text_(std::move(text)), onTap_(std::move(onTap)) {}
    SharedPtr<Widget> build(BuildContext&) override {
        auto t = cui::make_shared<Text>(text_, TextStyle().size(13.0).fgColor(theme().primary));
        auto c = cui::make_shared<Container>(t);
        c->padding(EdgeInsets::symmetric(2.0, 0.0));
        if (onTap_)
            return cui::make_shared<GestureDetector>(c, onTap_);
        return c;
    }
    std::string toStringShort() const override { return "Link"; }

private:
    std::string text_;
    std::function<void()> onTap_;
};

// ── SearchBar ─────────────────────────────────────────────────────────────────
/// 搜索栏。上游的 `SearchBar`（material/search_bar.dart）简化版。
///
/// 输入走真实键盘路由：状态持 `TextEditingController` + `FocusNode`，文本行是
/// `EditableText`（点它即上焦打字），文本非空时右侧出现清除按钮。
/// 与上游的偏离：上游是一整块 M3 表面（hint 动画、viewLeading/viewTrailing…），
/// 这里只保留「图标 + 输入 + 清除」这条最小形状。
class SearchBar : public StatefulWidget {
public:
    explicit SearchBar(std::string placeholder = "Search", std::function<void(const std::string&)> onChanged = {},
                       KeyPtr key = nullptr, std::function<void(const std::string&)> onSubmitted = nullptr)
        : StatefulWidget(std::move(key)), placeholder_(std::move(placeholder)), onChanged_(std::move(onChanged)),
          onSubmitted_(std::move(onSubmitted)) {}
    const std::string& placeholder() const { return placeholder_; }
    /// 当前文本。挂载后状态在每次变化时写回 value_，所以从 widget 读也是真值。
    const std::string& value() const { return value_; }
    void setValue(const std::string& v) { value_ = v; }
    const std::function<void(const std::string&)>& onChanged() const { return onChanged_; }
    const std::function<void(const std::string&)>& onSubmitted() const { return onSubmitted_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SearchBar"; }

private:
    std::string placeholder_;
    std::string value_;
    std::function<void(const std::string&)> onChanged_;
    std::function<void(const std::string&)> onSubmitted_;
};

class SearchBarState : public State {
public:
    void initState() override;
    SharedPtr<Widget> build(BuildContext&) override;
    ///@{ 状态实际使用的控制器/焦点节点（测试与 gallery 用）。
    const SharedPtr<TextEditingController>& controller() const { return controller_; }
    const SharedPtr<FocusNode>& focusNode() const { return focusNode_; }
    ///@}

private:
    SearchBar* w() { return static_cast<SearchBar*>(widget()); }
    void onTextChanged(const std::string& text);

    SharedPtr<TextEditingController> controller_;
    SharedPtr<FocusNode> focusNode_;
};

// ── DropdownButton ────────────────────────────────────────────────────────────
/// 下拉选择。上游的 `DropdownButton`（material/dropdown.dart）简化版。
class DropdownButton : public StatefulWidget {
public:
    explicit DropdownButton(std::vector<std::string> items, int selected = 0, std::function<void(int)> onChanged = {},
                            KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), items_(std::move(items)), selected_(selected),
          onChanged_(std::move(onChanged)) {}
    const std::vector<std::string>& items() const { return items_; }
    int selected() const { return selected_; }
    void setSelected(int s) { selected_ = s; }
    const std::function<void(int)>& onChanged() const { return onChanged_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "DropdownButton"; }

private:
    std::vector<std::string> items_;
    int selected_ = 0;
    std::function<void(int)> onChanged_;
};

class DropdownButtonState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    DropdownButton* w() { return static_cast<DropdownButton*>(widget()); }
    bool open_ = false;

    /// 按钮的 GlobalKey：用来量出按钮的实际宽度，菜单宽度跟着它走。
    SharedPtr<GlobalKey> buttonKey_ = cui::make_shared<GlobalKey>("DropdownButton.button");

    /// 按钮当前的实际宽度（还没布局出来时为 0）。
    double buttonWidth() const {
        if (!buttonKey_)
            return 0.0;
        auto* e = buttonKey_->currentElement<Element>();
        if (!e)
            return 0.0;
        auto* box = dynamic_cast<RenderBox*>(e->findRenderObject());
        return box ? box->size().width : 0.0;
    }

    /// 菜单宽度：默认与按钮同宽；只有最宽的选项本身比按钮还宽时才更宽
    /// （否则文字会顶出卡片）。文本宽度按 `RenderParagraph` 的同一口径量。
    double menuWidth(BuildContext& context, const TextStyle& itemStyle) {
        double width = buttonWidth();
        TextStyle merged = DefaultTextStyle::of(context);
        merged.mergeFrom(itemStyle);
        const char* family = merged.fontFamily.empty() ? nullptr : merged.fontFamily.c_str();
        for (const auto& item : w()->items()) {
            const double textWidth =
                    Painter::measureTextWidth(family, item, merged.fontSize, merged.fontWeight, merged.italic);
            width = std::max(width, textWidth + kItemPaddingH);
        }
        return width;
    }

    /// 选项行的水平内边距（左右各 6）。
    static constexpr double kItemPaddingH = 12.0;
};

// ── RatingBar ─────────────────────────────────────────────────────────────────
/// 评分条。上游 社区 `RatingBar`（无官方）。
class RatingBar : public StatefulWidget {
public:
    explicit RatingBar(int max = 5, int value = 0, std::function<void(int)> onChanged = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), max_(max), value_(value), onChanged_(std::move(onChanged)) {}
    int max() const { return max_; }
    int value() const { return value_; }
    void setValue(int v) { value_ = v; }
    const std::function<void(int)>& onChanged() const { return onChanged_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "RatingBar"; }

private:
    int max_ = 5;
    int value_ = 0;
    std::function<void(int)> onChanged_;
};

class RatingBarState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    RatingBar* w() { return static_cast<RatingBar*>(widget()); }
};

// ── AvatarGroup ───────────────────────────────────────────────────────────────
/// 头像组（重叠）。上游 社区 `AvatarGroup`（无官方），复用 CircleAvatar。
class AvatarGroup : public StatelessWidget {
public:
    explicit AvatarGroup(std::vector<std::string> initials, std::vector<Color> colors = {}, int maxDisplay = 4,
                         KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), initials_(std::move(initials)), colors_(std::move(colors)),
          maxDisplay_(maxDisplay) {}
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> kids;
        int shown = std::min(maxDisplay_, static_cast<int>(initials_.size()));
        for (int i = 0; i < shown; ++i) {
            Color col = (i < static_cast<int>(colors_.size())) ? colors_[i] : Color::fromRGB(0.13 + 0.1 * i, 0.5, 0.7);
            auto av = cui::make_shared<CircleAvatar>(initials_[i], 18.0, col);
            auto padded = cui::make_shared<Container>(av);
            padded->margin(EdgeInsets(0.0, 0.0, 0.0, -6.0));
            kids.push_back(padded);
        }
        int extra = static_cast<int>(initials_.size()) - shown;
        if (extra > 0) {
            auto more = cui::make_shared<CircleAvatar>("+" + std::to_string(extra), 18.0, theme().textDisabled);
            auto padded = cui::make_shared<Container>(more);
            padded->margin(EdgeInsets(0.0, 0.0, 0.0, -6.0));
            kids.push_back(padded);
        }
        auto row = cui::make_shared<Row>(std::move(kids));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        return row;
    }
    std::string toStringShort() const override { return "AvatarGroup"; }

private:
    std::vector<std::string> initials_;
    std::vector<Color> colors_;
    int maxDisplay_;
};

// ── NavigationRail ────────────────────────────────────────────────────────────
/// 导航栏（竖向）。上游的 `NavigationRail`（material/navigation_rail.dart）。
struct RailDestination {
    std::string label;
    std::string icon;                    ///< 单字占位图标（字体缺字形时的退路）
    IconCode iconCode = IconCode::None;  ///< 矢量图标（优先：emoji 多数字体没有字形）
};

class NavigationRail : public StatefulWidget {
public:
    explicit NavigationRail(std::vector<RailDestination> destinations, int selected = 0,
                            std::function<void(int)> onDestination = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), destinations_(std::move(destinations)), selected_(selected),
          onDestination_(std::move(onDestination)) {}
    const std::vector<RailDestination>& destinations() const { return destinations_; }
    int selected() const { return selected_; }
    void setSelected(int s) { selected_ = s; }
    const std::function<void(int)>& onDestination() const { return onDestination_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "NavigationRail"; }

private:
    std::vector<RailDestination> destinations_;
    int selected_ = 0;
    std::function<void(int)> onDestination_;
};

class NavigationRailState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    NavigationRail* w() { return static_cast<NavigationRail*>(widget()); }
};

// ── Drawer ────────────────────────────────────────────────────────────────────
/// 抽屉面板。上游的 `Drawer`（material/drawer.dart）简化（仅展示面板样式）。
class Drawer : public StatelessWidget {
public:
    explicit Drawer(SharedPtr<Widget> child, double width = 260.0, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)), width_(width) {}
    SharedPtr<Widget> build(BuildContext&) override {
        auto box = cui::make_shared<Container>(child_);
        box->width(width_).decoration(BoxDecoration{theme().surface, 0.0, 0.0, Color::fromRGBA(0, 0, 0, 0), 8.0,
                                                    Color::fromRGBA(0, 0, 0, 0.18)});
        return box;
    }
    std::string toStringShort() const override { return "Drawer"; }

private:
    SharedPtr<Widget> child_;
    double width_;
};

// ── PageView ──────────────────────────────────────────────────────────────────
/// 分页视图（横向）。上游的 `PageView`（widgets/page_view.dart）简化版。
class PageView : public StatefulWidget {
public:
    explicit PageView(std::vector<SharedPtr<Widget>> pages, int initial = 0, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), pages_(std::move(pages)), index_(initial) {}
    const std::vector<SharedPtr<Widget>>& pages() const { return pages_; }
    int index() const { return index_; }
    void setIndex(int i) { index_ = std::clamp(i, 0, static_cast<int>(pages_.size()) - 1); }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "PageView"; }

private:
    std::vector<SharedPtr<Widget>> pages_;
    int index_ = 0;
};

class PageViewState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    PageView* w() { return static_cast<PageView*>(widget()); }
};

// ── Breadcrumb ────────────────────────────────────────────────────────────────
/// 面包屑。上游 社区 `Breadcrumb`（无官方）。
class Breadcrumb : public StatelessWidget {
public:
    explicit Breadcrumb(std::vector<std::string> items, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), items_(std::move(items)) {}
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> kids;
        for (size_t i = 0; i < items_.size(); ++i) {
            bool last = i + 1 == items_.size();
            std::function<void()> onTap = last ? std::function<void()>{} : std::function<void()>([] {});
            kids.push_back(cui::make_shared<Link>(items_[i], onTap));
            if (!last) {
                auto sep = cui::make_shared<Text>("/", TextStyle().size(12.0).fgColor(theme().textDisabled));
                auto pad = cui::make_shared<Container>(sep);
                pad->padding(EdgeInsets::symmetric(0.0, 6.0));
                kids.push_back(pad);
            }
        }
        auto row = cui::make_shared<Row>(std::move(kids));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        return row;
    }
    std::string toStringShort() const override { return "Breadcrumb"; }

private:
    std::vector<std::string> items_;
};

// ── Pagination ────────────────────────────────────────────────────────────────
/// 分页器。上游 社区 `Pagination`（无官方）。
class Pagination : public StatefulWidget {
public:
    explicit Pagination(int count, int current = 1, std::function<void(int)> onPage = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), count_(count), current_(current), onPage_(std::move(onPage)) {}
    int count() const { return count_; }
    int current() const { return current_; }
    void setCurrent(int c) { current_ = std::clamp(c, 1, count_); }
    const std::function<void(int)>& onPage() const { return onPage_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Pagination"; }

private:
    int count_ = 1;
    int current_ = 1;
    std::function<void(int)> onPage_;
};

class PaginationState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Pagination* w() { return static_cast<Pagination*>(widget()); }
};

// ── Skeleton ──────────────────────────────────────────────────────────────────
/// 骨架屏占位。上游 社区 `Skeleton`（无官方）。
class Skeleton : public StatelessWidget {
public:
    explicit Skeleton(std::vector<double> widths = {200.0, 160.0, 180.0}, double height = 14.0, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), widths_(std::move(widths)), height_(height) {}
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> kids;
        for (double w : widths_) {
            auto bar = cui::make_shared<Container>(cui::make_shared<SizedBox>(w, height_));
            bar->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.08), height_ / 2.0});
            kids.push_back(bar);
            kids.push_back(cui::make_shared<SizedBox>(0.0, 8.0));
        }
        auto col = cui::make_shared<Column>(std::move(kids));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto box = cui::make_shared<Container>(col);
        box->padding(EdgeInsets::all(12.0)).decoration(BoxDecoration{theme().surface, 8.0});
        return box;
    }
    std::string toStringShort() const override { return "Skeleton"; }

private:
    std::vector<double> widths_;
    double height_;
};

// ── EmptyState ────────────────────────────────────────────────────────────────
/// 空状态。上游 社区 `EmptyState`（无官方）。
class EmptyState : public StatelessWidget {
public:
    explicit EmptyState(std::string emoji, std::string title, std::string desc = "", KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), emoji_(std::move(emoji)), title_(std::move(title)), desc_(std::move(desc)) {}
    SharedPtr<Widget> build(BuildContext&) override {
        auto e = cui::make_shared<Text>(emoji_, TextStyle().size(40.0));
        auto t = cui::make_shared<Text>(title_, TextStyle().size(15.0).bold());
        auto d = cui::make_shared<Text>(desc_, TextStyle().size(12.5).fgColor(theme().textSecondary));
        auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
                cui::make_shared<Center>(e), cui::make_shared<Center>(t), cui::make_shared<Center>(d)});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto box = cui::make_shared<Container>(col);
        box->padding(EdgeInsets::all(20.0)).decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.03), 10.0});
        return box;
    }
    std::string toStringShort() const override { return "EmptyState"; }

private:
    std::string emoji_;
    std::string title_;
    std::string desc_;
};

// ── ColorPicker ───────────────────────────────────────────────────────────────
/// 颜色选择器。上游的 `ColorPicker`（material/color_picker.dart，社区常用）。
class ColorPicker : public StatefulWidget {
public:
    explicit ColorPicker(std::vector<Color> swatches, int selected = -1, std::function<void(Color)> onChanged = {},
                         KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), swatches_(std::move(swatches)), selected_(selected),
          onChanged_(std::move(onChanged)) {}
    const std::vector<Color>& swatches() const { return swatches_; }
    int selected() const { return selected_; }
    void setSelected(int s) { selected_ = s; }
    const std::function<void(Color)>& onChanged() const { return onChanged_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ColorPicker"; }

private:
    std::vector<Color> swatches_;
    int selected_ = -1;
    std::function<void(Color)> onChanged_;
};

class ColorPickerState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    ColorPicker* w() { return static_cast<ColorPicker*>(widget()); }
};

// ── SnackBar ──────────────────────────────────────────────────────────────────
/// 轻提示条。上游的 `SnackBar`（material/snack_bar.dart）简化（直接展示条）。
class SnackBar : public StatelessWidget {
public:
    /// 对齐 Material 3 SnackBar：inverseSurface 深底、onInverseSurface 白字 14px、
    /// 操作键用 inversePrimary、圆角 6、最小高度 48（用 14px 上下内边距保证）、
    /// 6dp 投影。
    explicit SnackBar(std::string message, std::string action = "", Color bg = Color::fromRGB(0.18, 0.18, 0.20),
                      KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), message_(std::move(message)), action_(std::move(action)), bg_(bg) {}
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(
                cui::make_shared<Text>(message_, TextStyle().size(14.0).fgColor(Color::fromRGB(0.96, 0.96, 0.98))));
        if (!action_.empty()) {
            kids.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
            kids.push_back(cui::make_shared<Text>(
                    action_, TextStyle().size(14.0).bold().fgColor(Color::fromRGB(0.55, 0.82, 1.0))));
        }
        auto row = cui::make_shared<Row>(std::move(kids));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto box = cui::make_shared<Container>(row);
        box->padding(EdgeInsets::symmetric(14.0, 16.0))
                .decoration(
                        BoxDecoration{bg_, 6.0, 0.0, Color::fromRGBA(0, 0, 0, 0), 6.0, Color::fromRGBA(0, 0, 0, 0.18)});
        return box;
    }
    std::string toStringShort() const override { return "SnackBar"; }

private:
    std::string message_;
    std::string action_;
    Color bg_;
};

// ── NotificationCard ──────────────────────────────────────────────────────────
/// 通知卡片。上游 社区 `Notification`（无官方，material/notifications）。
class NotificationCard : public StatelessWidget {
public:
    explicit NotificationCard(std::string title, std::string body, std::string time = "", KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), title_(std::move(title)), body_(std::move(body)), time_(std::move(time)) {}
    SharedPtr<Widget> build(BuildContext&) override {
        auto t = cui::make_shared<Text>(title_, TextStyle().size(13.5).bold());
        auto b = cui::make_shared<Text>(body_, TextStyle().size(12.0).fgColor(theme().textSecondary));
        auto time = cui::make_shared<Text>(time_, TextStyle().size(11.0).fgColor(theme().textDisabled));
        auto col = cui::make_shared<Column>(
                std::vector<SharedPtr<Widget>>{cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{t, time}), b});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto box = cui::make_shared<Container>(col);
        box->padding(EdgeInsets::all(12.0))
                .decoration(BoxDecoration{theme().surface, 8.0, 1.0, Color::fromRGBA(0, 0, 0, 0.12), 3.0,
                                          Color::fromRGBA(0, 0, 0, 0.15)});
        return box;
    }
    std::string toStringShort() const override { return "NotificationCard"; }

private:
    std::string title_;
    std::string body_;
    std::string time_;
};

// ── Tabs ──────────────────────────────────────────────────────────────────────
/// 标签页容器：持有 TabController，组合 TabBar + TabBarView（方便页面直接复用）。
class Tabs : public StatefulWidget {
public:
    explicit Tabs(std::vector<std::string> labels, std::vector<SharedPtr<Widget>> pages, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), labels_(std::move(labels)), pages_(std::move(pages)) {}
    const std::vector<std::string>& labels() const { return labels_; }
    const std::vector<SharedPtr<Widget>>& pages() const { return pages_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Tabs"; }

private:
    std::vector<std::string> labels_;
    std::vector<SharedPtr<Widget>> pages_;
};

class TabsState : public State {
public:
    /// TabController 放在 State 里（上游 标准做法）：Widget 是不可变配置，
    /// 若把控制器作为 Widget 成员，Widget 被替换时控制器与已注册监听会一并
    /// 失效/悬空。State 的生命周期与 Element 一致，是持有控制器的正确位置。
    void initState() override {
        auto* t = static_cast<Tabs*>(widget());
        controller_ = TabController(static_cast<int>(t->pages().size()));
        unsub_ = controller_.addListener([this] { setState(); });
    }
    void dispose() override {
        if (unsub_) {
            unsub_();
            unsub_ = {};
        }
    }
    SharedPtr<Widget> build(BuildContext&) override {
        auto* t = static_cast<Tabs*>(widget());
        std::vector<SharedPtr<Widget>> tabWidgets;
        for (const auto& l : t->labels())
            tabWidgets.push_back(cui::make_shared<Text>(l, TextStyle().size(13.0).bold()));
        auto bar = cui::make_shared<TabBar>(&controller_, tabWidgets);
        auto view = cui::make_shared<TabBarView>(&controller_, t->pages());

        // TabBar 的溢出下拉是浮层，必须画在页面内容之上。若按 Column 顺序排布，
        // 图表在 TabBar 之后绘制会盖住菜单，因此把 TabBar 放到 Stack 顶层，
        // 底层用等高占位留出标签栏空间。该高度与 TabBarState 里 cell 的固定
        // 高度（40）保持一致。
        const double kBarH = 40.0;
        // 占位需要一个能撑满宽度的子节点，否则 Column/Stack 会缩到图表宽度。
        auto spacer = cui::make_shared<Container>(cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{}));
        spacer->height(kBarH);
        auto col =
                cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{spacer, cui::make_shared<Expanded>(1, view)});
        col->setMainAxisSize(MainAxisSize::Max);  // 占满父给的高度
        col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);

        auto pos = cui::make_shared<Positioned>(bar);
        pos->hasTop = true;
        pos->top = 0.0;
        pos->hasLeft = true;
        pos->left = 0.0;
        pos->hasRight = true;
        pos->right = 0.0;

        auto stack = cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{col, pos});
        stack->setFit(RenderStack::Fit::Loose);
        return stack;
    }

private:
    TabController controller_;
    std::function<void()> unsub_;
};

// ── ContextMenu ────────────────────────────────────────────────────────────────
/// 右键菜单：鼠标右键在**按下点**弹出（左上角落在点击处，超出窗口时向内收）。
/// 上游 无官方 ContextMenu 控件（通常用 showMenu / PlatformMenuBar）。
///
/// 菜单挂在**上层 Overlay** 里（同 `PopupMenuButton` / `ComboBox`），不是触发区
/// 自己那层 Stack 的越界子树：画在页面里时，宿主 Column 的**后继兄弟**既在它之后
/// 绘制、又在反向命中里排在它前面 ⇒ 下方文字压在菜单上（看着像「菜单太透明」）、
/// 菜单项的点击还会被兄弟控件抢走。
class ContextMenu : public StatefulWidget {
public:
    explicit ContextMenu(SharedPtr<Widget> child, std::vector<std::string> items,
                         std::function<void(int)> onSelect = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)), items_(std::move(items)),
          onSelect_(std::move(onSelect)) {}

    const SharedPtr<Widget>& child() const { return child_; }
    const std::vector<std::string>& items() const { return items_; }
    const std::function<void(int)>& onSelect() const { return onSelect_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ContextMenu"; }

private:
    SharedPtr<Widget> child_;
    std::vector<std::string> items_;
    std::function<void(int)> onSelect_;
};

class ContextMenuState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        auto* self = w();
        return cui::make_shared<GestureDetector>(self->child(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 [this](const PointerEvent& e) { openAt(e.globalPosition); });
    }

    void dispose() override {
        close();
        State::dispose();
    }

    bool isOpen() const { return entry_ && entry_->mounted(); }

private:
    ContextMenu* w() { return static_cast<ContextMenu*>(widget()); }

    /// 右键按下点（窗口全局坐标）：面板左上角落在这里。
    void openAt(const Offset& globalPos) {
        if (isOpen())
            return;
        anchorGlobal_ = globalPos;
        auto* self = this;
        entry_ = cui::make_shared<OverlayEntry>([self](BuildContext& ctx) { return self->buildMenu(ctx); },
                                                /*opaque=*/false, /*maintainState=*/false);
        OverlayState::of(*context())->insert(entry_);
    }

    void close() {
        if (entry_ && entry_->mounted())
            entry_->remove();
        entry_.reset();
    }

    void select(int index) {
        close();
        if (w()->onSelect())
            w()->onSelect()(index);
    }

    SharedPtr<Widget> buildMenu(BuildContext& entryContext) {
        // 全局坐标 → 浮层坐标（换算口径同 `PopupMenuButton`：`Positioned` 的偏移
        // 相对 Overlay 那层 Stack，而它未必落在窗口原点）。
        RenderBox* overlay = nullptr;
        if (Element* host = entryContext.element()) {
            if (auto* roHost = host->ancestorRenderObjectElement())
                overlay = dynamic_cast<RenderBox*>(roHost->findRenderObject());
        }
        Offset anchor = overlay ? overlay->globalToLocal(anchorGlobal_) : anchorGlobal_;
        double left = anchor.dx;
        double top = anchor.dy;
        if (overlay) {
            // 贴边时向内收，避免面板整块落到窗口外。高度要等布局才知道，按
            // 「文字 12.5 + 行上下各 12 内边距」的行高估算（与下面的行容器一致）。
            const double menuH = kMenuRowExtent * static_cast<double>(w()->items().size()) + 2.0;
            left = std::min(left, std::max(0.0, overlay->size().width - kMenuWidth));
            top = std::min(top, std::max(0.0, overlay->size().height - menuH));
        }
        std::vector<SharedPtr<Widget>> stack;

        // 全屏透明 barrier：左右键点菜单外任意处都收起（也覆盖触发区 ⇒ 再点一次
        // 即关闭）。菜单打开后触发区收不到事件，右键关闭由这里接住。
        auto barrierBox = cui::make_shared<Container>();
        barrierBox->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0)});
        auto barrier = cui::make_shared<GestureDetector>(
                barrierBox, [this] { close(); }, nullptr, nullptr, nullptr, nullptr, nullptr,
                [this](const PointerEvent&) { close(); });
        auto fill = cui::make_shared<Positioned>(barrier);
        fill->hasLeft = fill->hasTop = fill->hasRight = fill->hasBottom = true;
        stack.push_back(fill);

        std::vector<SharedPtr<Widget>> rows;
        const std::vector<std::string>& items = w()->items();
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            auto t = cui::make_shared<Text>(items[i], TextStyle().size(12.5));
            auto c = cui::make_shared<Container>(t);
            c->padding(EdgeInsets::symmetric(6.0, 12.0));
            int idx = i;
            rows.push_back(cui::make_shared<GestureDetector>(c, [this, idx] { select(idx); }));
        }
        auto col = cui::make_shared<Column>(std::move(rows));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
        auto panel = cui::make_shared<Container>(col);
        panel->width(kMenuWidth)
                .decoration(BoxDecoration{theme().surface, 6.0, 1.0, Color::fromRGBA(0, 0, 0, 0.2), 8.0,
                                          Color::fromRGBA(0, 0, 0, 0.25)});
        auto pos = cui::make_shared<Positioned>(panel);
        pos->top = top;
        pos->left = left;
        pos->hasTop = true;
        pos->hasLeft = true;
        stack.push_back(pos);

        return cui::make_shared<Stack>(std::move(stack));
    }

    static constexpr double kMenuWidth = 140.0;
    /// 单行高度估算（只用于贴窗口底边时把面板整体上收）。
    static constexpr double kMenuRowExtent = 41.0;

    /// 右键按下点（窗口全局坐标），buildMenu 时换算到浮层坐标。
    Offset anchorGlobal_{};
    SharedPtr<OverlayEntry> entry_;
};

// ── MenuBar ───────────────────────────────────────────────────────────────────
/// 顶层菜单项。
struct MenuEntry {
    std::string label;
    std::vector<std::string> items;
};

/// 菜单栏。上游的 `PlatformMenuBar`（widgets/platform_menu_bar.dart）简化版。
/// 注意：本控件只负责绘制「菜单条」本身（带高亮）。下拉面板由上层
/// GalleryShellState 以浮层（Stack 顶层）方式渲染，从而「漂浮在内容之上、
/// 不挤占布局空间」且命中位于所有兄弟之上。
class MenuBar : public StatefulWidget {
public:
    explicit MenuBar(std::vector<MenuEntry> menus, std::function<void(int, int)> onSelect = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), menus_(std::move(menus)), onSelect_(std::move(onSelect)) {}

    const std::vector<MenuEntry>& menus() const { return menus_; }
    const std::function<void(int, int)>& onSelect() const { return onSelect_; }

    /// 当前展开项的索引（-1 表示未展开）。由上层维护，用于高亮。
    int openIndex() const { return openIndex_; }
    MenuBar* setOpenIndex(int i) {
        openIndex_ = i;
        return this;
    }
    /// 点击某个标签时回调（由上层负责展开/收起/切换）。
    const std::function<void(int)>& onLabelTap() const { return onLabelTap_; }
    MenuBar* setOnLabelTap(std::function<void(int)> f) {
        onLabelTap_ = std::move(f);
        return this;
    }
    /// 展开态下把鼠标移到另一个标签 ⇒ 切到那个标签的下拉（桌面菜单条惯例）。
    /// 与 onLabelTap 一样，展开状态由上层维护；本控件只在 openIndex() >= 0（已展开）
    /// 时才转发 hover，收起态下扫过菜单条不会弹出任何东西。
    const std::function<void(int)>& onLabelHover() const { return onLabelHover_; }
    MenuBar* setOnLabelHover(std::function<void(int)> f) {
        onLabelHover_ = std::move(f);
        return this;
    }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "MenuBar"; }

private:
    std::vector<MenuEntry> menus_;
    std::function<void(int, int)> onSelect_;
    std::function<void(int)> onLabelTap_;
    std::function<void(int)> onLabelHover_;
    int openIndex_ = -1;
};

class MenuBarState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;
    void dispose() override { State::dispose(); }

private:
    MenuBar* w() { return static_cast<MenuBar*>(widget()); }
};

// ── OtpInput ──────────────────────────────────────────────────────────────────
/// 一次性验证码输入。上游 社区 `OtpInput`（无官方）。
///
/// 每格一个 `FocusNode`（状态持有，跨重建持久）：点格子（`Focus` 自带按下即上焦）
/// 或 Tab 选中格子，数字键写入并自动跳到下一格，Backspace 先清当前格、当前格已空
/// 则退到上一格清它，Delete 清当前格，左右方向键移动焦点。
/// 非数字的可打印键与上/下方向键一律 Ignored ⇒ 沿焦点祖先链继续上溯，页面照常滚。
class OtpInput : public StatefulWidget {
public:
    explicit OtpInput(int length = 6, KeyPtr key = nullptr) : StatefulWidget(std::move(key)), length_(length) {}
    int length() const { return length_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "OtpInput"; }

private:
    int length_ = 6;
};

class OtpInputState : public State {
public:
    void initState() override;
    SharedPtr<Widget> build(BuildContext&) override;
    ///@{ 测试与 gallery 用：各位数字（-1 = 空格）、每格的焦点节点、拼出来的码串。
    const std::vector<int>& digits() const { return digits_; }
    const std::vector<SharedPtr<FocusNode>>& cells() const { return nodes_; }
    std::string code() const;
    ///@}

private:
    OtpInput* otp() { return static_cast<OtpInput*>(widget()); }
    KeyEventResult handleKey(int index, const KeyEvent& ev);
    void focusCell(int index);

    std::vector<int> digits_;
    std::vector<SharedPtr<FocusNode>> nodes_;
};

// ── TokenInput ────────────────────────────────────────────────────────────────
/// 标签/词条输入。上游 社区 `TokenInput`（无官方）。
/// 说明：无键盘路由，点击候选词添加、点击 × 删除。
class TokenInput : public StatefulWidget {
public:
    explicit TokenInput(std::vector<std::string> tokens, std::vector<std::string> suggestions, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), tokens_(std::move(tokens)), suggestions_(std::move(suggestions)) {}
    const std::vector<std::string>& tokens() const { return tokens_; }
    const std::vector<std::string>& suggestions() const { return suggestions_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "TokenInput"; }

private:
    std::vector<std::string> tokens_;
    std::vector<std::string> suggestions_;
};

class TokenInputState : public State {
public:
    void initState() override { tokens_ = ti()->tokens(); }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    TokenInput* ti() { return static_cast<TokenInput*>(widget()); }
    std::vector<std::string> tokens_;
};

// ── Form ──────────────────────────────────────────────────────────────────────
/// 表单字段（label + field）。
struct FormFieldSpec {
    std::string label;
    SharedPtr<Widget> field;
};

/// 表单。上游 社区 `Form`（官方 Form 主要做校验状态，这里取布局语义）。
class Form : public StatelessWidget {
public:
    explicit Form(std::vector<FormFieldSpec> fields, double labelWidth = 110.0, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), fields_(std::move(fields)), labelWidth_(labelWidth) {}

    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> kids;
        for (const auto& f : fields_) {
            auto label = cui::make_shared<Text>(f.label,
                                                TextStyle().size(12.5).bold().fgColor(Color::fromRGB(0.3, 0.3, 0.35)));
            auto labelBox = cui::make_shared<Container>(label);
            labelBox->width(labelWidth_);
            auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{labelBox, f.field});
            row->setMainAxisSize(MainAxisSize::Min);
            row->setCrossAxisAlignment(CrossAxisAlignment::Center);
            auto box = cui::make_shared<Container>(row);
            box->padding(EdgeInsets::symmetric(0.0, 8.0));
            kids.push_back(box);
        }
        auto col = cui::make_shared<Column>(std::move(kids));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto outer = cui::make_shared<Container>(col);
        outer->padding(EdgeInsets::all(12.0))
                .decoration(BoxDecoration{theme().surface, 8.0, 1.0, Color::fromRGBA(0, 0, 0, 0.12)});
        return outer;
    }
    std::string toStringShort() const override { return "Form"; }

private:
    std::vector<FormFieldSpec> fields_;
    double labelWidth_;
};

// ── Splitter ──────────────────────────────────────────────────────────────────
/// 可拖拽分隔条。上游 社区 `Splitter`（无官方）。
///
/// 用 LayoutBuilder 取得可用尺寸，按 ratio 分配两栏；拖动手柄时按全局坐标
/// 位移换算新比例（onPanDown 记录起点，onPanUpdate 更新比例并重建）。
class Splitter : public StatefulWidget {
public:
    explicit Splitter(SharedPtr<Widget> first, SharedPtr<Widget> second, Axis axis = Axis::Horizontal,
                      double initialRatio = 0.5, double thickness = 8.0, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), first_(std::move(first)), second_(std::move(second)), axis_(axis),
          initialRatio_(initialRatio), thickness_(thickness) {}

    const SharedPtr<Widget>& first() const { return first_; }
    const SharedPtr<Widget>& second() const { return second_; }
    Axis axis() const { return axis_; }
    double initialRatio() const { return initialRatio_; }
    double thickness() const { return thickness_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Splitter"; }

private:
    SharedPtr<Widget> first_, second_;
    Axis axis_ = Axis::Horizontal;
    double initialRatio_ = 0.5;
    double thickness_ = 8.0;
};

class SplitterState : public State {
public:
    void initState() override { ratio_ = sp()->initialRatio(); }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Splitter* sp() { return static_cast<Splitter*>(widget()); }
    double ratio_ = 0.5;
    double dragStart_ = 0.0;
    double startRatio_ = 0.5;
};

// ── Zoomable ──────────────────────────────────────────────────────────────────
/// 可平移/缩放的内容区。上游的 `InteractiveViewer` 简化版。
///
/// 无滚轮事件，故用底部按钮控制缩放；拖拽（onPanUpdate）控制平移。
/// 变换用 Transform::translate + Transform::scale 组合。
class Zoomable : public StatefulWidget {
public:
    explicit Zoomable(SharedPtr<Widget> child, double initialScale = 1.0, double width = 320.0, double height = 200.0,
                      KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)), initialScale_(initialScale), width_(width),
          height_(height) {}

    const SharedPtr<Widget>& child() const { return child_; }
    double initialScale() const { return initialScale_; }
    double width() const { return width_; }
    double height() const { return height_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Zoomable"; }

private:
    SharedPtr<Widget> child_;
    double initialScale_ = 1.0;
    double width_ = 320.0;
    double height_ = 200.0;
};

class ZoomableState : public State {
public:
    void initState() override { scale_ = zm()->initialScale(); }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Zoomable* zm() { return static_cast<Zoomable*>(widget()); }
    double scale_ = 1.0;
    double dx_ = 0.0, dy_ = 0.0;
    double startX_ = 0.0, startY_ = 0.0;
    double anchorDx_ = 0.0, anchorDy_ = 0.0;
};

// ── Accordion ─────────────────────────────────────────────────────────────────
/// 折叠面板项。
struct AccordionItem {
    std::string title;
    SharedPtr<Widget> content;
};

/// 手风琴（单开）。上游 无官方 Accordion（最接近 ExpansionPanelList）。
/// 与 ExpansionTile 的区别：同一时刻只允许一项展开。
class Accordion : public StatefulWidget {
public:
    explicit Accordion(std::vector<AccordionItem> items, int expandedIndex = -1, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), items_(std::move(items)), expandedIndex_(expandedIndex) {}
    const std::vector<AccordionItem>& items() const { return items_; }
    int expandedIndex() const { return expandedIndex_; }
    void setExpandedIndex(int i) { expandedIndex_ = i; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Accordion"; }

private:
    std::vector<AccordionItem> items_;
    int expandedIndex_ = -1;
};

class AccordionState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Accordion* w() { return static_cast<Accordion*>(widget()); }
};

// ── Carousel ──────────────────────────────────────────────────────────────────
/// 轮播。上游的 `CarouselView`（material/carousel.dart）简化版：
/// 严格 1:1 —— 视口内一次只显示当前页（viewportFraction = 1），
/// 靠左右箭头 / 指示点切换。
class Carousel : public StatefulWidget {
public:
    explicit Carousel(std::vector<SharedPtr<Widget>> items, double itemWidth = 130.0, double itemHeight = 90.0,
                      KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), items_(std::move(items)), itemWidth_(itemWidth), itemHeight_(itemHeight) {}
    const std::vector<SharedPtr<Widget>>& items() const { return items_; }
    double itemWidth() const { return itemWidth_; }
    double itemHeight() const { return itemHeight_; }
    int index() const { return index_; }
    void setIndex(int i) {
        if (items_.empty()) {
            index_ = 0;
            return;
        }
        index_ = std::clamp(i, 0, static_cast<int>(items_.size()) - 1);
    }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Carousel"; }

private:
    std::vector<SharedPtr<Widget>> items_;
    double itemWidth_ = 130.0;
    double itemHeight_ = 90.0;
    int index_ = 0;
};

class CarouselState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Carousel* w() { return static_cast<Carousel*>(widget()); }
};

// ── CommandPalette ────────────────────────────────────────────────────────────
/// 命令项。
struct CommandItem {
    std::string title;
    std::string subtitle;
    std::string shortcut;
};

/// 命令面板（Cmd+Shift+P 风格）。上游 社区 `CommandPalette`（无官方）。
///
/// 查询行走真实键盘路由（与 SearchBar/ComboBox 同一口径）：状态持
/// `TextEditingController` + `FocusNode`，输入框是 `EditableText`，键入即按标题
/// 子串（大小写不敏感）过滤，高亮自动回到第一条匹配。
/// 列表导航挂在**同一个节点**的多槽键处理器上（`FocusNode::addKeyHandler`）：
/// `EditableText` 的单槽对 ↑/↓/回车/Esc 一律 Ignored（单行输入框不吞这些键），
/// 于是才轮到我们 —— 与 ComboBox 的选项导航同形。
///   ↑/↓ 移动高亮（到顶/到底停住；无匹配时放行给祖先，页面照常滚）、
///   回车执行高亮项、Esc 清空查询。
/// 首字母筛选条保留给鼠标用户，点它等价于把那个字母写进查询框。
class CommandPalette : public StatefulWidget {
public:
    explicit CommandPalette(std::vector<CommandItem> commands, std::function<void(int)> onRun = {},
                            KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), commands_(std::move(commands)), onRun_(std::move(onRun)) {}
    const std::vector<CommandItem>& commands() const { return commands_; }
    const std::function<void(int)>& onRun() const { return onRun_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "CommandPalette"; }

private:
    std::vector<CommandItem> commands_;
    std::function<void(int)> onRun_;
};

class CommandPaletteState : public State {
public:
    void initState() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext&) override;

    ///@{ 状态实际使用的控制器/焦点节点与列表状态（测试与 gallery 用）。
    const SharedPtr<TextEditingController>& controller() const { return controller_; }
    const SharedPtr<FocusNode>& focusNode() const { return focusNode_; }
    const std::string& query() const { return controller_->text(); }
    /// 当前匹配项在 `commands()` 里的原始下标（列表顺序）。
    const std::vector<int>& matched() const { return matched_; }
    /// 高亮项在 `matched()` 里的槽位；无匹配时为 -1。
    int highlight() const { return highlight_; }
    ///@}

private:
    CommandPalette* w() { return static_cast<CommandPalette*>(widget()); }
    void rebuildMatched();
    void moveHighlight(int delta);
    void runMatched(int slot);
    KeyEventResult handleKey(const KeyEvent& ev);

    SharedPtr<TextEditingController> controller_;
    SharedPtr<FocusNode> focusNode_;
    int listenerId_ = 0;
    int keyHandlerId_ = 0;
    std::vector<int> matched_;
    int highlight_ = -1;
};

// ── HoverCard ─────────────────────────────────────────────────────────────────
/// 悬停卡片。上游 社区 `HoverCard`（无官方）。
///
/// 用 Listener 的 onHover（PointerEventType::Hover）实现真正的鼠标悬停显示。
/// 关闭策略：
///   - 鼠标移出（PointerEventType::Exit）→ 延时 `closeDelaySec` 秒后自动关闭；
///   - 延时期间鼠标移回（Enter/Hover/Move）→ 取消延时，卡片继续显示；
///   - 鼠标移到卡片本体上同样算「还在」→ 取消延时；
///   - 鼠标抬起（onUp）→ 立即关闭。
class HoverCard : public StatefulWidget {
public:
    explicit HoverCard(SharedPtr<Widget> trigger, SharedPtr<Widget> card, double cardTop = 26.0,
                       double closeDelaySec = 1.0, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), trigger_(std::move(trigger)), card_(std::move(card)), cardTop_(cardTop),
          closeDelaySec_(closeDelaySec) {}
    const SharedPtr<Widget>& trigger() const { return trigger_; }
    const SharedPtr<Widget>& card() const { return card_; }
    double cardTop() const { return cardTop_; }
    /// 鼠标移出后多久自动关闭（秒）。
    double closeDelaySec() const { return closeDelaySec_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "HoverCard"; }

private:
    SharedPtr<Widget> trigger_, card_;
    double cardTop_ = 26.0;
    double closeDelaySec_ = 1.0;
};

class HoverCardState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

    /// 显示（并取消待执行的关闭延时）。
    void showNow() {
        cancelClose();
        if (!visible_) {
            visible_ = true;
            setState();
        }
    }
    /// 立即关闭。
    void hideNow() {
        cancelClose();
        if (visible_) {
            visible_ = false;
            setState();
        }
    }
    /// 鼠标移出：延时 closeDelaySec 秒后自动关闭（期间移回则取消）。
    void scheduleClose() {
        if (!visible_)
            return;
        const double delay = w()->closeDelaySec();
        if (delay <= 0.0) {
            hideNow();
            return;
        }
        cancelClose();
        if (!closeTicker_) {
            closeTicker_ = std::make_unique<Ticker>([this](Duration e) {
                if (!mounted())
                    return;
                if (e.count() >= w()->closeDelaySec() * 1000.0)
                    hideNow();  // 内部会停掉 ticker
            });
        }
        if (!closeTicker_->isActive())
            closeTicker_->start();
    }
    void cancelClose() {
        if (closeTicker_ && closeTicker_->isActive())
            closeTicker_->stop();
    }

    void dispose() override {
        cancelClose();
        State::dispose();
    }

private:
    HoverCard* w() { return static_cast<HoverCard*>(widget()); }
    bool visible_ = false;
    std::unique_ptr<Ticker> closeTicker_;
};

// ── TimePicker ────────────────────────────────────────────────────────────────
/// 时间选择。上游的 `TimePickerDialog`（material/time_picker.dart）简化版。
/// 无键盘输入，用 ▲/▼ 调整小时与分钟。
class TimePicker : public StatefulWidget {
public:
    explicit TimePicker(int hour = 9, int minute = 30, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), hour_(hour), minute_(minute) {}
    int hour() const { return hour_; }
    int minute() const { return minute_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "TimePicker"; }

private:
    int hour_ = 9;
    int minute_ = 30;
};

class TimePickerState : public State {
public:
    void initState() override {
        hour_ = tp()->hour();
        minute_ = tp()->minute();
    }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    TimePicker* tp() { return static_cast<TimePicker*>(widget()); }
    int hour_ = 9;
    int minute_ = 30;
};

// ── DatePicker ────────────────────────────────────────────────────────────────
/// 日期选择：月历（Calendar）+ 选中日期回显。
/// 上游的 `CalendarDatePicker`（material/date_picker.dart）简化版。
class DatePicker : public StatefulWidget {
public:
    explicit DatePicker(KeyPtr key = nullptr) : StatefulWidget(std::move(key)) {}
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "DatePicker"; }
};

class DatePickerState : public State {
public:
    void initState() override {
        std::time_t now = std::time(nullptr);
        std::tm* t = std::localtime(&now);
        selY_ = t->tm_year + 1900;
        selM_ = t->tm_mon;
        selD_ = t->tm_mday;
    }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    int selY_ = 2026, selM_ = 0, selD_ = 1;
};

// ── Gif ───────────────────────────────────────────────────────────────────────
/// 逐帧动画（动图）。上游 无官方 Gif 控件（通常依赖 image 包解码）。
///
/// 这里实现为「帧序列播放器」：由 foundation Ticker 的真实时间驱动，按 fps
/// 计算当前帧并重建；提供暂停/继续。帧内容由调用方给出（可为任意 Widget）。
class Gif : public StatefulWidget {
public:
    explicit Gif(std::vector<SharedPtr<Widget>> frames, double fps = 8.0, double width = 120.0, double height = 90.0,
                 KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), frames_(std::move(frames)), fps_(fps), width_(width), height_(height) {}
    const std::vector<SharedPtr<Widget>>& frames() const { return frames_; }
    double fps() const { return fps_; }
    double width() const { return width_; }
    double height() const { return height_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Gif"; }

private:
    std::vector<SharedPtr<Widget>> frames_;
    double fps_ = 8.0;
    double width_ = 120.0;
    double height_ = 90.0;
};

class GifState : public State {
public:
    void initState() override {
        auto* self = this;
        ticker_ = std::make_unique<Ticker>([self](Duration elapsed) {
            const auto& fs = self->gif()->frames();
            if (fs.empty() || !self->playing_)
                return;
            double msPerFrame = 1000.0 / std::max(0.5, self->gif()->fps());
            int f = static_cast<int>(elapsed.count() / msPerFrame) % static_cast<int>(fs.size());
            if (f != self->frame_) {
                self->frame_ = f;
                self->setState();
            }
        });
        ticker_->start();
    }
    void dispose() override {
        if (ticker_)
            ticker_->stop();
    }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Gif* gif() { return static_cast<Gif*>(widget()); }
    int frame_ = 0;
    bool playing_ = true;
    std::unique_ptr<Ticker> ticker_;
};

// ── Spinner ───────────────────────────────────────────────────────────────────
/// 加载指示器。基于已有的 CircularProgressIndicator（不确定进度）。
class Spinner : public StatelessWidget {
public:
    explicit Spinner(std::string label = "", Color color = theme().primary, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), label_(std::move(label)), color_(color) {}

    SharedPtr<Widget> build(BuildContext&) override {
        auto ind = cui::make_shared<CircularProgressIndicator>(std::nullopt, Color::fromRGBA(0, 0, 0, 0.12), color_);
        auto box = cui::make_shared<Container>(ind);
        box->padding(EdgeInsets::all(4.0));
        if (label_.empty())
            return box;
        auto t = cui::make_shared<Text>(label_, TextStyle().size(12.0).fgColor(theme().textSecondary));
        auto row = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{box, cui::make_shared<SizedBox>(8.0, 0.0), t});
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        return row;
    }
    std::string toStringShort() const override { return "Spinner"; }

private:
    std::string label_;
    Color color_;
};

// ── Settings ──────────────────────────────────────────────────────────────────
/// 设置项。kind：0 = Switch，1 = Checkbox，2 = 纯文本值。
struct SettingItem {
    std::string title;
    std::string subtitle;
    int kind = 0;
    std::string valueText;
};

/// 设置面板。上游 社区 `Settings`（无官方）。
class Settings : public StatefulWidget {
public:
    explicit Settings(std::vector<SettingItem> items, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), items_(std::move(items)) {}
    const std::vector<SettingItem>& items() const { return items_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Settings"; }

private:
    std::vector<SettingItem> items_;
};

class SettingsState : public State {
public:
    void initState() override {
        values_.assign(st()->items().size(), false);
        if (!values_.empty())
            values_[0] = true;
    }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Settings* st() { return static_cast<Settings*>(widget()); }
    std::vector<bool> values_;
};

// ── Animated ──────────────────────────────────────────────────────────────────
/// 动画容器（尺寸插值）。上游的 `AnimatedContainer` 简化版。
/// 由 foundation Ticker 的真实时间驱动：点击切换目标态，尺寸按 durationMs 插值过渡。
class Animated : public StatefulWidget {
public:
    explicit Animated(double smallW = 80.0, double bigW = 200.0, double height = 60.0, double durationMs = 400.0,
                      KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), smallW_(smallW), bigW_(bigW), height_(height), durationMs_(durationMs) {}
    double smallW() const { return smallW_; }
    double bigW() const { return bigW_; }
    double height() const { return height_; }
    double durationMs() const { return durationMs_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Animated"; }

private:
    double smallW_ = 80.0, bigW_ = 200.0, height_ = 60.0, durationMs_ = 400.0;
};

class AnimatedState : public State {
public:
    void initState() override {
        auto* self = this;
        ticker_ = std::make_unique<Ticker>([self](Duration elapsed) {
            double ms = static_cast<double>(elapsed.count());
            double dt = ms - self->lastMs_;
            self->lastMs_ = ms;
            if (dt <= 0.0)
                return;
            double step = dt / std::max(1.0, self->an()->durationMs());
            double target = self->big_ ? 1.0 : 0.0;
            if (self->t_ < target)
                self->t_ = std::min(target, self->t_ + step);
            else if (self->t_ > target)
                self->t_ = std::max(target, self->t_ - step);
            else
                return;  // 已到位，无需重建
            self->setState();
        });
        ticker_->start();
    }
    void dispose() override {
        if (ticker_)
            ticker_->stop();
    }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Animated* an() { return static_cast<Animated*>(widget()); }
    double t_ = 0.0;
    bool big_ = false;
    double lastMs_ = 0.0;
    std::unique_ptr<Ticker> ticker_;
};

// ── Popup ─────────────────────────────────────────────────────────────────────
/// 弹出浮层（点击触发器展开、点 ✕ 或再点触发器收起）。
/// 上游 无官方 Popup（通常用 Overlay / PopupMenuButton）。
class Popup : public StatefulWidget {
public:
    explicit Popup(SharedPtr<Widget> trigger, SharedPtr<Widget> content, std::string title = "", KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), trigger_(std::move(trigger)), content_(std::move(content)),
          title_(std::move(title)) {}
    const SharedPtr<Widget>& trigger() const { return trigger_; }
    const SharedPtr<Widget>& content() const { return content_; }
    const std::string& title() const { return title_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Popup"; }

private:
    SharedPtr<Widget> trigger_, content_;
    std::string title_;
};

class PopupState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Popup* w() { return static_cast<Popup*>(widget()); }
    bool open_ = false;
};

// ── GroupBox ──────────────────────────────────────────────────────────────────
/// 带标题的分组框（类似 HTML fieldset / Win32 GroupBox）。
class GroupBox : public StatelessWidget {
public:
    explicit GroupBox(std::string title, SharedPtr<Widget> child, double width = 300.0, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), title_(std::move(title)), child_(std::move(child)), width_(width) {}

    SharedPtr<Widget> build(BuildContext&) override {
        // 对齐 legacy（src/widgets/basic/frame.cpp 的 GroupBox::paint）：
        //   1px 圆角边框、标题骑跨在上边框（fieldset 风格，patch 遮住边框段）、
        //   标题 caption 尺寸 + textSecondary、内边距 12（顶部留出标题高度避免压住内容）。
        const Color kBorder = theme().border;
        const Color kTitle = theme().textSecondary;

        auto box = cui::make_shared<Container>(child_);
        box->width(width_)
                .padding(EdgeInsets(18.0, 12.0, 12.0, 12.0))
                .decoration(BoxDecoration{theme().surface, 6.0, 1.0, kBorder});
        auto label = cui::make_shared<Text>(title_, TextStyle().size(12.5).fgColor(kTitle));
        auto titleBox = cui::make_shared<Container>(label);
        titleBox->padding(EdgeInsets::symmetric(0.0, 5.0)).decoration(BoxDecoration{theme().surface});
        // 标题水平居左骑跨上边框（fieldset 风格）：Positioned 直接贴左边（left=10），
        // titleBox 按自然宽度摆放，surface 补丁遮住其下的边框段。
        auto pos = cui::make_shared<Positioned>(titleBox);
        pos->hasTop = true;
        pos->top = -15.0;
        pos->hasLeft = true;
        pos->left = 10.0;
        auto stack = cui::make_shared<Stack>(std::vector<SharedPtr<Widget>>{box, pos});
        stack->setFit(RenderStack::Fit::Loose);
        auto wrap = cui::make_shared<Container>(stack);
        wrap->padding(EdgeInsets(0.0, 10.0, 0.0, 4.0));
        return wrap;
    }
    std::string toStringShort() const override { return "GroupBox"; }

private:
    std::string title_;
    SharedPtr<Widget> child_;
    double width_;
};

// ── ExpandableFab ─────────────────────────────────────────────────────────────
/// 悬浮动作按钮（合并 FAB 本体 + 展开菜单）。对齐 rui
/// `crates/rui/src/widgets/basic/expandable_fab.rs`（Flutter `_FloatingActionButtonType`
/// + `flutter_expandable_fab`）：
///   · 本体四种尺寸变体 Regular(56,圆角16) / Small(40,12) / Large(96,28) / Extended(高56,
///     图标+文字标签)；M3 双层投影、hover/focus/pressed 覆盖层、焦点环、涟漪、禁用蒙版。
///   · 展开菜单 Fan(扇形)/Up(竖直)/Side(水平) 三种类型，子项动画 None/Rotate/Scale/RotateScale，
///     三种位置 Right/Left/Center 决定扇形朝向，distance/fanAngle/duration 可调。
///   · 菜单挂上层 Overlay（同 ContextMenu/ComboBox 锚点口径）⇒ 扇出的子项越出本体也能命中。
enum class FabSize { Regular, Small, Large, Extended };
enum class ExpandType { Fan, Up, Side };
enum class ChildrenAnimation { None, Rotate, Scale, RotateScale };
enum class FabPosition { Right, Left, Center };

/// 菜单项（一枚迷你圆形 FAB）。`onTap` 为空时仅收起菜单。
struct FabAction {
    IconCode icon = IconCode::None;  ///< 迷你 FAB 的矢量图标
    std::function<void()> onTap;     ///< 点击该项（在收起菜单之前触发）
};

class ExpandableFab;       ///< 前向声明（供 State::w 使用）
class ExpandableFabState;  ///< 前向声明

class ExpandableFab : public StatefulWidget {
public:
    explicit ExpandableFab(IconCode icon = IconCode::Add, std::string label = "", FabSize size = FabSize::Regular,
                           std::vector<FabAction> actions = {}, std::optional<Color> bgColor = std::nullopt,
                           std::optional<Color> fgColor = std::nullopt, ExpandType expandType = ExpandType::Fan,
                           ChildrenAnimation childrenAnim = ChildrenAnimation::Rotate,
                           FabPosition position = FabPosition::Right, double distance = 100.0, double fanAngle = 90.0,
                           double durationMs = 250.0, double elevation = 4.0, bool disabled = false,
                           std::function<void()> onClick = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), icon_(icon), label_(std::move(label)), size_(size),
          actions_(std::move(actions)), bgColor_(bgColor), fgColor_(fgColor), expandType_(expandType),
          childrenAnim_(childrenAnim), position_(position), distance_(distance), fanAngle_(fanAngle),
          durationMs_(durationMs), elevation_(elevation), disabled_(disabled), onClick_(std::move(onClick)) {}

    IconCode icon() const { return icon_; }
    const std::string& label() const { return label_; }
    FabSize size() const { return size_; }
    const std::vector<FabAction>& actions() const { return actions_; }
    const std::optional<Color>& bgColor() const { return bgColor_; }
    const std::optional<Color>& fgColor() const { return fgColor_; }
    ExpandType expandType() const { return expandType_; }
    ChildrenAnimation childrenAnim() const { return childrenAnim_; }
    FabPosition position() const { return position_; }
    double distance() const { return distance_; }
    double fanAngle() const { return fanAngle_; }
    double durationMs() const { return durationMs_; }
    double elevation() const { return elevation_; }
    bool disabled() const { return disabled_; }
    const std::function<void()>& onClick() const { return onClick_; }
    bool hasActions() const { return !actions_.empty(); }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ExpandableFab"; }

private:
    IconCode icon_;
    std::string label_;
    FabSize size_;
    std::vector<FabAction> actions_;
    std::optional<Color> bgColor_;
    std::optional<Color> fgColor_;
    ExpandType expandType_;
    ChildrenAnimation childrenAnim_;
    FabPosition position_;
    double distance_;
    double fanAngle_;
    double durationMs_;
    double elevation_;
    bool disabled_;
    std::function<void()> onClick_;
};

class ExpandableFabState : public State {
public:
    // 本体几何（对齐 rui button_size / border_radius / icon_font_size）。
    double bodyW() const { return w()->size() == FabSize::Extended ? extendedWidth() : bodySize(); }
    double bodyH() const { return w()->size() == FabSize::Extended ? 56.0 : bodySize(); }
    double bodySize() const {
        switch (w()->size()) {
            case FabSize::Regular:
                return 56.0;
            case FabSize::Small:
                return 40.0;
            case FabSize::Large:
                return 96.0;
            default:
                return 56.0;
        }
    }
    double borderRadius() const {
        switch (w()->size()) {
            case FabSize::Regular:
                return 16.0;
            case FabSize::Small:
                return 12.0;
            case FabSize::Large:
                return 28.0;
            default:
                return 16.0;
        }
    }
    double iconSize() const { return w()->size() == FabSize::Large ? 36.0 : 24.0; }
    double extendedWidth() const {
        double labelW = w()->label().empty() ? 0.0 : Painter::measureTextWidth("", w()->label(), 14.0);
        double spacing = w()->label().empty() ? 0.0 : 8.0;
        return 16.0 + iconSize() + spacing + labelW + 20.0;
    }

    Color resolveBg() const {
        if (const auto& c = w()->bgColor())
            return *c;
        return theme().primaryContainer;
    }
    Color resolveFg() const {
        if (const auto& c = w()->fgColor())
            return *c;
        return theme().onPrimaryContainer;
    }

    bool isOpen() const { return open_; }
    double progress() const { return menu_; }

    /// 本体渲染盒中心的全局坐标（供测试与外部定位用）。
    Offset bodyCenterGlobal() const {
        Element* e = context() ? context()->element() : nullptr;
        auto* box = e ? dynamic_cast<RenderBox*>(e->findRenderObject()) : nullptr;
        if (!box)
            return Offset{};
        return box->localToGlobal(Offset{box->size().width / 2.0, box->size().height / 2.0});
    }

    void initState() override { focusNode_ = cui::make_shared<FocusNode>(); }
    void dispose() override {
        if (menuTicker_)
            menuTicker_->stop();
        if (rippleTicker_)
            rippleTicker_->stop();
        if (entry_ && entry_->mounted())
            entry_->remove();
        entry_.reset();
    }

    SharedPtr<Widget> build(BuildContext&) override;

    /// 本体点击：起涟漪；有菜单项则展开/收起，否则回调 onClick。
    void activate() {
        if (w()->disabled())
            return;
        startRipple();
        if (w()->hasActions()) {
            if (menuAnimating_)
                return;  // 动画期间忽略（对齐 rui is_running 守卫）
            if (menu_ >= 1.0)
                collapse();
            else if (menu_ <= 0.0)
                expand();
        } else if (w()->onClick()) {
            w()->onClick()();
        }
    }

    void expand() {
        if (!entry_) {
            auto* self = this;
            entry_ = cui::make_shared<OverlayEntry>([self](BuildContext& ctx) { return self->buildMenu(ctx); },
                                                    /*opaque=*/false, /*maintainState=*/false);
            OverlayState::of(*context())->insert(entry_);
        }
        open_ = true;
        runMenu(1.0);
    }

    void collapse() {
        open_ = false;
        runMenu(0.0);
    }

private:
    ExpandableFab* w() const { return static_cast<ExpandableFab*>(widget()); }

    void runMenu(double to) {
        animFrom_ = menu_;
        animTo_ = to;
        menuAnimating_ = true;
        double dur = w()->durationMs();
        if (dur <= 0.0) {
            dur = 1.0;
        }
        if (menuTicker_)
            menuTicker_->stop();
        auto* self = this;
        menuTicker_ = std::make_unique<Ticker>([self, dur](Duration elapsed) {
            double t = std::min(1.0, elapsed.count() / dur);
            double k = 1.0 - std::pow(1.0 - t, 4.0);  // easeOutQuart
            self->menu_ = self->animFrom_ + (self->animTo_ - self->animFrom_) * k;
            if (t >= 1.0) {
                self->menu_ = self->animTo_;
                self->menuAnimating_ = false;
                self->menuTicker_->stop();
                if (self->animTo_ <= 0.0 && self->entry_ && self->entry_->mounted()) {
                    self->entry_->remove();
                    self->entry_.reset();
                }
            }
            if (self->entry_ && self->entry_->mounted())
                self->entry_->markNeedsBuild();
            self->setState();
        });
        menuTicker_->start();
    }

    void startRipple() {
        rippleCenterSet_ = true;
        if (rippleTicker_)
            rippleTicker_->stop();
        ripple_ = 0.0;
        auto* self = this;
        rippleTicker_ = std::make_unique<Ticker>([self](Duration elapsed) {
            double t = std::min(1.0, elapsed.count() / 400.0);
            double k = 1.0 - std::pow(1.0 - t, 3.0);  // easeOutCubic
            self->ripple_ = k;
            self->setState();
            if (t >= 1.0) {
                self->ripple_ = 0.0;
                self->rippleTicker_->stop();
            }
        });
        rippleTicker_->start();
    }

    // ── 菜单项几何（逐条对齐 rui item_offset / item_local_rect / 动画因子）──
    static constexpr double kActionSize = 48.0;
    static constexpr double kHalf = kActionSize / 2.0;

    Offset itemOffset(size_t idx, double progress) const {
        const auto* wdg = w();
        const size_t count = wdg->actions().size();
        const double dist = progress * wdg->distance();
        switch (wdg->expandType()) {
            case ExpandType::Up: {
                double d = progress * wdg->distance() * static_cast<double>(idx + 1);
                return Offset{0.0, -d};
            }
            case ExpandType::Side: {
                double d = progress * wdg->distance() * static_cast<double>(idx + 1);
                double dx = wdg->position() == FabPosition::Right ? -d : d;
                return Offset{dx, 0.0};
            }
            case ExpandType::Fan:
            default: {
                const double angleHalf = (90.0 - wdg->fanAngle()) / 2.0;
                double dir = count > 1 ? wdg->fanAngle() / static_cast<double>(count - 1) * static_cast<double>(idx) +
                                                 angleHalf :
                                         wdg->fanAngle() + angleHalf;
                const double startAngle = wdg->position() == FabPosition::Left ? 270.0 : 180.0;
                if (wdg->position() == FabPosition::Center)
                    dir += 45.0;
                const double rad = (startAngle + dir) * 3.14159265358979323846 / 180.0;
                return Offset{std::cos(rad) * dist, std::sin(rad) * dist};
            }
        }
    }

    double itemRotationRad(double progress) const {
        auto a = w()->childrenAnim();
        if (a == ChildrenAnimation::Rotate || a == ChildrenAnimation::RotateScale)
            return (1.0 - progress) * 90.0 * 3.14159265358979323846 / 180.0;
        return 0.0;
    }
    double itemScale(double progress) const {
        auto a = w()->childrenAnim();
        if (a == ChildrenAnimation::Scale || a == ChildrenAnimation::RotateScale)
            return 0.5 + 0.5 * progress;
        return 1.0;
    }

    /// FAB 本体渲染盒在浮层坐标系里的矩形（换算口径同 ContextMenu::triggerRectInOverlay）。
    Rect fabRectInOverlay(BuildContext& entryContext) const {
        Element* e = context() ? context()->element() : nullptr;
        auto* box = e ? dynamic_cast<RenderBox*>(e->findRenderObject()) : nullptr;
        if (!box)
            return Rect{};
        Offset origin = box->localToGlobal(Offset{});
        Element* host = entryContext.element();
        if (auto* roHost = host ? host->ancestorRenderObjectElement() : nullptr) {
            if (auto* ov = dynamic_cast<RenderBox*>(roHost->findRenderObject()))
                origin = ov->globalToLocal(origin);
        }
        return Rect{origin.dx, origin.dy, box->size().width, box->size().height};
    }

    SharedPtr<Widget> buildMenu(BuildContext& entryContext) {
        const Rect fab = fabRectInOverlay(entryContext);
        const double cx0 = fab.x + fab.width / 2.0;
        const double cy0 = fab.y + fab.height / 2.0;
        const double p = menu_;
        std::vector<SharedPtr<Widget>> layers;

        // 全屏透明 barrier：点菜单外任意处收起（也覆盖本体 ⇒ 再点一次即关闭）。
        auto barrierBox = cui::make_shared<Container>();
        barrierBox->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0)});
        auto barrier = cui::make_shared<GestureDetector>(barrierBox, [this] { collapse(); });
        auto fill = cui::make_shared<Positioned>(barrier);
        fill->hasLeft = fill->hasTop = fill->hasRight = fill->hasBottom = true;
        layers.push_back(fill);

        const auto& actions = w()->actions();
        const Color itemBg = theme().primary;
        for (size_t i = 0; i < actions.size(); ++i) {
            Offset off = itemOffset(i, p);
            auto ic = cui::make_shared<Icon>(actions[i].icon, 22.0, theme().textOnPrimary);
            auto disc = cui::make_shared<Container>(cui::make_shared<Center>(ic));
            disc->width(kActionSize)
                    .height(kActionSize)
                    .decoration(BoxDecoration{itemBg, kHalf, 0.0, Color(), w()->elevation() + 2.0,
                                              Color::fromRGBA(0, 0, 0, 0.25)});
            size_t idx = i;
            SharedPtr<Widget> item = cui::make_shared<GestureDetector>(disc, [this, idx] {
                if (idx < w()->actions().size() && w()->actions()[idx].onTap)
                    w()->actions()[idx].onTap();
                collapse();
            });
            // 子项动画：绕中心缩放 + 旋转，透明度随进度淡入。
            double sc = itemScale(p);
            if (sc != 1.0) {
                item = cui::make_shared<Transform>(Matrix4::scale(sc, sc), item, Offset(), true, nullptr,
                                                   Alignment::center());
            }
            double rot = itemRotationRad(p);
            if (rot != 0.0)
                item = Transform::rotate(rot, item);
            item = cui::make_shared<Opacity>(p, item);

            auto pos = cui::make_shared<Positioned>(item);
            pos->hasLeft = pos->hasTop = pos->hasWidth = pos->hasHeight = true;
            pos->left = cx0 + off.dx - kHalf;
            pos->top = cy0 + off.dy - kHalf;
            pos->width = kActionSize;
            pos->height = kActionSize;
            layers.push_back(pos);
        }
        return cui::make_shared<Stack>(std::move(layers));
    }

    /// 本体状态覆盖层（hover/focus/pressed 染色、涟漪、焦点环、禁用蒙版），裁到圆角矩形内。
    std::function<void(Painter&, const Size&)> makeStatePainter() {
        Color overlay = resolveFg();
        bool hovered = hovered_, focused = focused_, pressed = pressed_, disabled = w()->disabled();
        double rp = ripple_, radius = borderRadius();
        return [overlay, hovered, focused, pressed, disabled, rp, radius](Painter& p, const Size& s) {
            Rect full{0, 0, s.width, s.height};
            auto tint = [&](double a) {
                p.setSourceColor(Color::fromRGBA(overlay.r, overlay.g, overlay.b, a));
                p.fillRoundedRect(full, radius);
            };
            if (hovered && !disabled)
                tint(0.08);
            if (focused && !disabled)
                tint(0.10);
            if (rp > 0.0 && rp < 1.0 && !disabled) {
                p.save();
                p.clipRoundedRect(full, radius);
                double maxR = std::sqrt(s.width * s.width + s.height * s.height);
                p.setSourceColor(Color::fromRGBA(overlay.r, overlay.g, overlay.b, (1.0 - rp) * 0.10));
                p.circle(s.width / 2.0, s.height / 2.0, maxR * rp);
                p.fill();
                p.restore();
            }
            if (pressed && !disabled)
                tint(0.10);
            if (focused && !disabled) {
                Color ring = theme().focusRing;
                p.setSourceColor(Color::fromRGBA(ring.r, ring.g, ring.b, 0.5));
                p.setLineWidth(2.0);
                p.strokeRoundedRect(Rect{2.0, 2.0, s.width - 4.0, s.height - 4.0}, radius - 2.0);
            }
            if (disabled) {
                Color surf = theme().surface;
                p.setSourceColor(Color::fromRGBA(surf.r, surf.g, surf.b, 0.62));
                p.fillRoundedRect(full, radius);
            }
        };
    }

    bool open_ = false;
    bool menuAnimating_ = false;
    double menu_ = 0.0;
    double animFrom_ = 0.0, animTo_ = 0.0;
    double ripple_ = 0.0;
    bool rippleCenterSet_ = false;
    bool hovered_ = false, pressed_ = false, focused_ = false;
    std::unique_ptr<Ticker> menuTicker_;
    std::unique_ptr<Ticker> rippleTicker_;
    SharedPtr<OverlayEntry> entry_;
    SharedPtr<FocusNode> focusNode_;
};

// ── TitleBar ──────────────────────────────────────────────────────────────────
/// 标题栏（窗口顶部）。上游 无官方（通常由 AppBar / 平台窗口提供）。
class TitleBar;  ///< 前向声明（供 TitleBarState::build 使用）

class TitleBarState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    int hovered_ = -1;
};

class TitleBar : public StatefulWidget {
public:
    /// @param actions  右侧窗口按钮的字形（legacy 用 "—" / "□" / "×"）。
    explicit TitleBar(std::string title, std::vector<std::string> actions = {}, double width = 420.0,
                      IconCode icon = IconCode::None, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), title_(std::move(title)), actions_(std::move(actions)), width_(width),
          icon_(icon) {}

    const std::string& title() const { return title_; }
    const std::vector<std::string>& actions() const { return actions_; }
    double width() const { return width_; }
    IconCode icon() const { return icon_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "TitleBar"; }

private:
    std::string title_;
    std::vector<std::string> actions_;
    double width_;
    IconCode icon_ = IconCode::None;
};

// ── Toolbar ───────────────────────────────────────────────────────────────────
/// 工具栏（一行图标按钮 + 分隔线）。对齐 legacy（src/widgets/navigation/toolbar.cpp）：
///   surface 底、底部 1px 分隔线、28px 方形按钮、16px 矢量图标、分隔线为 1px 竖线。
struct ToolbarItem {
    IconCode icon = IconCode::None;  ///< 矢量图标（优先）
    std::string label;               ///< 文字（无图标时显示）
    bool separator = false;          ///< true 表示竖分隔线
};

class Toolbar : public StatelessWidget {
public:
    explicit Toolbar(std::vector<ToolbarItem> items, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), items_(std::move(items)) {}

    SharedPtr<Widget> build(BuildContext&) override {
        const Color kSurface = theme().surface;
        const Color kTextSecondary = theme().textSecondary;
        const Color kBorderLight = theme().borderLight;
        const Color kDivider = theme().divider;
        const Color kHover = theme().hoverOverlay;  // hoverOverlay

        const double kHeight = 36.0, kBtn = 28.0, kPad = 4.0, kGap = 2.0;

        std::vector<SharedPtr<Widget>> kids;
        for (const auto& it : items_) {
            if (it.separator) {
                auto sep = cui::make_shared<Container>(cui::make_shared<SizedBox>(1.0, kHeight - 16.0));
                sep->decoration(BoxDecoration{kDivider});
                kids.push_back(cui::make_shared<SizedBox>(kPad, 0.0));
                kids.push_back(sep);
                kids.push_back(cui::make_shared<SizedBox>(kPad + kGap, 0.0));
                continue;
            }
            SharedPtr<Widget> content;
            if (it.icon != IconCode::None)
                content = cui::make_shared<Icon>(it.icon, 16.0, kTextSecondary);
            else
                content = cui::make_shared<Text>(it.label, TextStyle().size(12.0).fgColor(kTextSecondary));
            auto c = cui::make_shared<Container>(content);
            c->width(kBtn).height(kBtn).alignment(0.0, 0.0).decoration(
                    BoxDecoration{kHover, 4.0});  // 圆角 hover 背景（常态）
            kids.push_back(cui::make_shared<SizedBox>(kPad, 0.0));
            kids.push_back(c);
            kids.push_back(cui::make_shared<SizedBox>(kGap, 0.0));
        }
        if (!kids.empty())
            kids.pop_back();

        auto row = cui::make_shared<Row>(std::move(kids));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto box = cui::make_shared<Container>(row);
        // 底色 + 底部 1px 边框（legacy：kBorderLight 横线）。
        box->height(kHeight)
                .padding(EdgeInsets::symmetric(0.0, kPad))
                .decoration(BoxDecoration{kSurface, 0.0, 1.0, kBorderLight});
        return box;
    }
    std::string toStringShort() const override { return "Toolbar"; }

private:
    std::vector<ToolbarItem> items_;
};

// ── StatusBar ─────────────────────────────────────────────────────────────────
/// 状态栏（窗口底部）。上游 无官方。
class StatusBar : public StatelessWidget {
public:
    /// @param left  左侧状态文本；@param right 右侧状态项（右对齐）。
    /// @param dot   左侧状态点颜色（nullopt = 不画）。
    /// @param surface 底色（nullopt = 不透明白）。液态玻璃外观下传半透明白，
    ///        让底下的玻璃壁纸透出来。
    explicit StatusBar(std::string left, std::vector<std::string> right = {}, std::optional<Color> dot = std::nullopt,
                       double width = 0.0, KeyPtr key = nullptr, std::optional<Color> surface = std::nullopt)
        : StatelessWidget(std::move(key)), left_(std::move(left)), right_(std::move(right)), dot_(dot), width_(width),
          surface_(surface) {}

    const std::string& left() const { return left_; }
    const std::vector<std::string>& right() const { return right_; }
    double width() const { return width_; }

    SharedPtr<Widget> build(BuildContext&) override {
        // 对齐 legacy StatusBar（src/widgets/navigation/status_bar.cpp）：
        //   surface 底、1px 上边框、左侧状态点 + 文案、右侧项右对齐。
        const Color kSurface = surface_.value_or(theme().surface);
        const Color kTextSecondary = theme().textSecondary;
        const Color kTextDisabled = theme().textDisabled;

        std::vector<SharedPtr<Widget>> kids;
        if (dot_.has_value()) {
            auto d = cui::make_shared<Container>(cui::make_shared<SizedBox>(8.0, 8.0));
            d->decoration(BoxDecoration{*dot_, 4.0});
            kids.push_back(d);
            kids.push_back(cui::make_shared<SizedBox>(6.0, 0.0));
        }
        if (!left_.empty())
            kids.push_back(cui::make_shared<Text>(left_, TextStyle().size(11.0).fgColor(kTextSecondary)));

        if (!right_.empty()) {
            std::vector<SharedPtr<Widget>> tail;
            for (auto it = right_.rbegin(); it != right_.rend(); ++it) {
                tail.push_back(cui::make_shared<Text>(*it, TextStyle().size(11.0).fgColor(kTextDisabled)));
                tail.push_back(cui::make_shared<SizedBox>(12.0, 0.0));
            }
            if (!tail.empty())
                tail.pop_back();
            auto tailRow = cui::make_shared<Row>(std::move(tail));
            tailRow->setMainAxisSize(MainAxisSize::Min);
            tailRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
            kids.push_back(cui::make_shared<Expanded>(1, cui::make_shared<SizedBox>(0.0, 0.0)));
            kids.push_back(tailRow);
        }

        auto row = cui::make_shared<Row>(std::move(kids));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto box = cui::make_shared<Container>(row);
        // width <= 0 表示不约束宽度（由父级拉伸），便于作为窗口底部状态条。
        if (width_ > 0.0)
            box->width(width_);
        // 本仓 EdgeInsets::symmetric 的形参是 (horizontal, vertical)，与上游
        // `symmetric(vertical:, horizontal:)` 相反。写成 (0.0, 10.0) 等于「上下各
        // 10」，22 的条高被吃掉 20 只剩 2px，11px 文字就顶到上边去了。
        box->height(22.0)
                .padding(EdgeInsets::symmetric(10.0, 0.0))
                .decoration(BoxDecoration{kSurface, 0.0, 1.0, Color::fromRGBA(0, 0, 0, 0.10)});
        return box;
    }
    std::string toStringShort() const override { return "StatusBar"; }

private:
    std::string left_;
    std::vector<std::string> right_;
    std::optional<Color> dot_;
    std::optional<Color> surface_;
    double width_;
};

// ── BottomSheet ───────────────────────────────────────────────────────────────
/// 底部面板。上游的 `BottomSheet`（material/bottom_sheet.dart）简化版：
/// 仅渲染面板本体（顶部圆角 + 阴影），不实现拖拽关闭手势。
class BottomSheet : public StatelessWidget {
public:
    explicit BottomSheet(SharedPtr<Widget> child, double width = 320.0, double height = 150.0, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)), width_(width), height_(height) {}

    SharedPtr<Widget> build(BuildContext&) override {
        auto grab = cui::make_shared<Container>(cui::make_shared<SizedBox>(40.0, 4.0));
        grab->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.2), 2.0});
        auto grabBox = cui::make_shared<Container>(cui::make_shared<Center>(grab));
        grabBox->padding(EdgeInsets(0.0, 6.0, 0.0, 8.0));
        auto col = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{grabBox, child_});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto box = cui::make_shared<Container>(col);
        box->width(width_)
                .height(height_)
                .padding(EdgeInsets::all(12.0))
                .decoration(BoxDecoration{theme().surface, 12.0, 1.0, Color::fromRGBA(0, 0, 0, 0.14), 14.0,
                                          Color::fromRGBA(0, 0, 0, 0.3)});
        return box;
    }
    std::string toStringShort() const override { return "BottomSheet"; }

private:
    SharedPtr<Widget> child_;
    double width_, height_;
};

// ── ButtonGroup ───────────────────────────────────────────────────────────────
/// 按钮组（互斥选中）。上游的 `SegmentedButton` / `ToggleButtons` 简化版。
/// 对应 legacy 的 ButtonGroup（basic/button_group.h）。
class ButtonGroup : public StatefulWidget {
public:
    explicit ButtonGroup(std::vector<std::string> labels, int selected = 0, std::function<void(int)> onChanged = {},
                         KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), labels_(std::move(labels)), selected_(selected),
          onChanged_(std::move(onChanged)) {}

    const std::vector<std::string>& labels() const { return labels_; }
    int selected() const { return selected_; }
    const std::function<void(int)>& onChanged() const { return onChanged_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "ButtonGroup"; }

private:
    std::vector<std::string> labels_;
    int selected_;
    std::function<void(int)> onChanged_;
};

class ButtonGroupState : public State {
public:
    void initState() override { sel_ = bg()->selected(); }
    SharedPtr<Widget> build(BuildContext&) override {
        auto* self = bg();
        const Color kPrimary = theme().primary;
        const Color kTextSecondary = theme().textSecondary;
        const Color kTrack = Color::fromRGBA(0, 0, 0, 0.06);

        // 对齐 legacy SegmentedControl（src/widgets/input/segmented_control.cpp）：
        //   外层浅色圆角轨道；选中项为主色药丸 + 白字；未选中项之间 1px 细线。
        std::vector<SharedPtr<Widget>> kids;
        for (int i = 0; i < static_cast<int>(self->labels().size()); ++i) {
            const bool on = (i == sel_);
            auto t = cui::make_shared<Text>(self->labels()[i], TextStyle().size(13.0).bold().fgColor(
                                                                       on ? theme().textOnPrimary : kTextSecondary));
            auto c = cui::make_shared<Container>(t);
            c->padding(EdgeInsets::symmetric(6.0, 14.0))
                    .alignment(0.0, 0.0)
                    .decoration(BoxDecoration{on ? kPrimary : Color::fromRGBA(0, 0, 0, 0), 4.0});
            int idx = i;
            kids.push_back(cui::make_shared<GestureDetector>(c, [this, idx] {
                if (sel_ == idx)
                    return;
                sel_ = idx;
                setState();
                if (bg()->onChanged())
                    bg()->onChanged()(idx);
            }));
            if (i + 1 < static_cast<int>(self->labels().size())) {
                auto sep = cui::make_shared<Container>(cui::make_shared<SizedBox>(1.0, 18.0));
                sep->decoration(BoxDecoration{Color::fromRGBA(0, 0, 0, 0.12)});
                kids.push_back(sep);
            }
        }

        auto row = cui::make_shared<Row>(std::move(kids));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto track = cui::make_shared<Container>(row);
        track->padding(EdgeInsets::all(2.0))
                .decoration(BoxDecoration{kTrack, 6.0, 1.0, Color::fromRGBA(0, 0, 0, 0.10)});

        auto hint = cui::make_shared<Text>("selected: " + std::to_string(sel_),
                                           TextStyle().size(11.5).fgColor(theme().textSecondary));
        auto col = cui::make_shared<Column>(
                std::vector<SharedPtr<Widget>>{track, cui::make_shared<SizedBox>(0.0, 6.0), hint});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return col;
    }

private:
    ButtonGroup* bg() { return static_cast<ButtonGroup*>(widget()); }
    int sel_ = 0;
};

// ── SpinBox ───────────────────────────────────────────────────────────────────
/// 数值输入框（数值 + ▲▼）。上游 无官方对应（近似 material/SpinButton）。
/// 对应 legacy 的 SpinBox（input/spin_box.h）。
///
/// 值区是一个真的 `EditableText`（State 持 controller + FocusNode），所以能直接键入：
///   - 逐字符校验「是否还是一个合法数字的前缀」，非法字符整串退回上一次合法文本
///     （键级过滤做不到：可打印字符被 `EditableText` 的单槽 `onKeyEvent` 先消费，
///     `FocusNode::addKeyHandler` 排在其后，见 focus.cpp 的 sendKeyEvent）；
///   - 能解析出数字就实时写进 value 并触发 onChanged（四舍五入到 decimals、夹到
///     [min, max]），中间态如 ""、"-"、"12." 保留当前值不动；
///   - 回车或失焦时把文本规范化成当前值（上游 SpinButton 同口径：非法输入不落地）。
/// 右侧 ▲▼ 按 step 增减，并把新值回写进输入框。
class SpinBox : public StatefulWidget {
public:
    explicit SpinBox(double value = 0.0, double minValue = 0.0, double maxValue = 100.0, double step = 1.0,
                     int decimals = 0, std::function<void(double)> onChanged = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), value_(value), min_(minValue), max_(maxValue), step_(step),
          decimals_(decimals), onChanged_(std::move(onChanged)) {}

    double value() const { return value_; }
    double minValue() const { return min_; }
    double maxValue() const { return max_; }
    double step() const { return step_; }
    int decimals() const { return decimals_; }
    const std::function<void(double)>& onChanged() const { return onChanged_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SpinBox"; }

private:
    double value_, min_, max_, step_;
    int decimals_;
    std::function<void(double)> onChanged_;
};

class SpinBoxState : public State {
public:
    void initState() override {
        auto* self = sb();
        v_ = clamp(self->value());
        controller_ = cui::make_shared<TextEditingController>(format(v_));
        focusNode_ = cui::make_shared<FocusNode>();
        lastText_ = controller_->text();
        // 控制器可能活得比本 State 久（页面切走但控件仍被引用）→ dispose 必须摘掉。
        listenerId_ = controller_->addListener([this] { onTextChanged(); });
    }
    void dispose() override {
        if (listenerId_ > 0) {
            controller_->removeListener(listenerId_);
            listenerId_ = 0;
        }
        State::dispose();
    }

    ///@{ 状态实际使用的控制器/焦点节点与当前值（测试与 gallery 用）。
    const SharedPtr<TextEditingController>& controller() const { return controller_; }
    const SharedPtr<FocusNode>& focusNode() const { return focusNode_; }
    double value() const { return v_; }
    ///@}

    SharedPtr<Widget> build(BuildContext&) override {
        auto* self = sb();

        auto valueField = cui::make_shared<EditableText>(
                controller_, TextStyle().size(14.0).fgColor(Color::fromRGB(0, 0, 0)), TextAlign::Left, std::string(),
                /*obscureText=*/false, /*autofocus=*/false, nullptr, focusNode_, nullptr,
                [this](const std::string&) { normalizeField(); });
        auto valueBox = cui::make_shared<Container>(valueField);
        valueBox->padding(EdgeInsets::symmetric(0.0, 8.0));

        // 右侧上下箭头（legacy：20px 宽两段按钮，浅灰底 + 深灰箭头）。
        auto arrowBtn = [this](const std::string& glyph, double delta) {
            auto g = cui::make_shared<Text>(glyph, TextStyle().size(7.0).fgColor(theme().textPrimary));
            auto c = cui::make_shared<Container>(g);
            c->width(20.0).height(13.0).alignment(0.0, 0.0).decoration(
                    BoxDecoration{theme().surfaceVariant, 0.0, 1.0, theme().border});
            return cui::make_shared<GestureDetector>(c, [this, delta] {
                double nv = clamp(v_ + delta);
                if (nv == v_)
                    return;
                v_ = nv;
                normalizeField();
                setState();
                if (sb()->onChanged())
                    sb()->onChanged()(v_);
            });
        };
        auto arrows = cui::make_shared<Column>(
                std::vector<SharedPtr<Widget>>{arrowBtn("▲", self->step()), arrowBtn("▼", -self->step())});
        arrows->setMainAxisSize(MainAxisSize::Min);
        arrows->setCrossAxisAlignment(CrossAxisAlignment::Center);

        auto row =
                cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{cui::make_shared<Expanded>(1, valueBox), arrows});
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);

        // 外框（legacy：白底 + 圆角 4 + 1px 灰边）。
        auto box = cui::make_shared<Container>(row);
        box->width(96.0).height(28.0).decoration(BoxDecoration{theme().surface, 4.0, 1.0, theme().border});

        std::string range = "range " + std::to_string(static_cast<int>(self->minValue())) + " ~ " +
                            std::to_string(static_cast<int>(self->maxValue())) + "  step " +
                            std::to_string(self->step()).substr(0, 3);
        auto hint = cui::make_shared<Text>(range, TextStyle().size(11.0).fgColor(theme().textSecondary));
        auto col = cui::make_shared<Column>(
                std::vector<SharedPtr<Widget>>{box, cui::make_shared<SizedBox>(0.0, 6.0), hint});
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return col;
    }

private:
    SpinBox* sb() { return static_cast<SpinBox*>(widget()); }

    double clamp(double x) { return std::max(sb()->minValue(), std::min(sb()->maxValue(), x)); }
    std::string format(double x) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", sb()->decimals(), x);
        return buf;
    }

    /// 「还能不能长成一个合法数字」的前缀判定：只允许数字、至多一个小数点、符号只能在
    /// 首位；空串也算合法中间态（全选后删干净，接着再打）。
    static bool looksNumeric(const std::string& s) {
        bool dot = false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (c >= '0' && c <= '9')
                continue;
            if (c == '.') {
                if (dot)
                    return false;
                dot = true;
                continue;
            }
            if ((c == '+' || c == '-') && i == 0)
                continue;
            return false;
        }
        return true;
    }
    /// 整串解析（`"12e"` 这种部分解析一律算失败，留给下一次键入）。
    static bool parseNumber(const std::string& s, double& out) {
        if (s.empty())
            return false;
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || end != s.c_str() + s.size())
            return false;
        out = v;
        return true;
    }

    void setTextProtected(const std::string& t) {
        syncing_ = true;
        controller_->setText(t);
        syncing_ = false;
    }
    /// 把输入框文本规范成当前值（同时更新 lastText_，免得自己的监听再绕一圈）。
    void normalizeField() {
        lastText_ = format(v_);
        setTextProtected(lastText_);
    }

    void onTextChanged() {
        if (syncing_)
            return;
        // 焦点翻转和文本变化共用同一条通知，必须先单独认出「失焦」——否则「打字后失焦」
        // 时文本与 lastText_ 相等，会在下面的等值判定里提前 return，规范化永远不触发。
        const bool focused = controller_->hasFocus();
        if (hadFocus_ != focused) {
            hadFocus_ = focused;
            if (!focused)
                normalizeField();
        }
        if (!focused)
            return;
        const std::string now = controller_->text();
        if (now == lastText_)
            return;  // 只是选区变化
        if (!looksNumeric(now)) {
            setTextProtected(lastText_);  // 非法字符：整串退回上一次合法文本
            return;
        }
        lastText_ = now;
        double parsed = 0.0;
        if (!parseNumber(now, parsed))
            return;  // 中间态（""/"-"/"12."）：值先不动
        double p = std::pow(10.0, static_cast<double>(std::max(0, sb()->decimals())));
        double nv = clamp(std::round(parsed * p) / p);
        if (nv == v_)
            return;
        v_ = nv;
        if (sb()->onChanged())
            sb()->onChanged()(v_);
    }

    double v_ = 0.0;
    SharedPtr<TextEditingController> controller_;
    SharedPtr<FocusNode> focusNode_;
    int listenerId_ = 0;
    std::string lastText_;
    bool syncing_ = false;
    bool hadFocus_ = false;
};

// ── PullToRefresh ─────────────────────────────────────────────────────────────
/// 下拉刷新容器。上游的 `RefreshIndicator`（material/refresh_indicator.dart）。
/// 下拉超过 triggerExtent 后松手触发 onRefresh；指示条显示约 700ms 后自动收起。
class PullToRefresh : public StatefulWidget {
public:
    explicit PullToRefresh(SharedPtr<Widget> child, std::function<void()> onRefresh = {}, double triggerExtent = 48.0,
                           KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)), onRefresh_(std::move(onRefresh)),
          triggerExtent_(triggerExtent) {}

    const SharedPtr<Widget>& child() const { return child_; }
    const std::function<void()>& onRefresh() const { return onRefresh_; }
    double triggerExtent() const { return triggerExtent_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "PullToRefresh"; }

private:
    SharedPtr<Widget> child_;
    std::function<void()> onRefresh_;
    double triggerExtent_;
};

class PullToRefreshState : public State {
public:
    void initState() override {
        auto* self = this;
        ticker_ = std::make_unique<Ticker>([self](Duration elapsed) {
            double ms = static_cast<double>(elapsed.count());
            if (ms - self->holdStartMs_ < 700.0) {
                self->holdProgress_ = (ms - self->holdStartMs_) / 700.0;
                self->setState();
                return;
            }
            self->refreshing_ = false;
            self->pull_ = 0.0;
            self->holdProgress_ = 0.0;
            self->setState();
            if (self->ticker_)
                self->ticker_->stop();
        });
    }
    void dispose() override {
        if (ticker_)
            ticker_->stop();
    }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    PullToRefresh* pr() { return static_cast<PullToRefresh*>(widget()); }
    double pull_ = 0.0;
    double dragStartY_ = 0.0;
    bool refreshing_ = false;
    double holdStartMs_ = 0.0;
    double holdProgress_ = 0.0;
    std::unique_ptr<Ticker> ticker_;
};

// ── Ribbon ─────────────────────────────────────────────────────────────────────
/// Office 风格 Ribbon：选项卡条 + 分组大/小命令按钮。
/// 上游 无官方对应（社区 ribbon 包）；对应 legacy cui_core::Ribbon
/// （navigation/ribbon.h）。放在 cui_core 以避开与 legacy 同名类的 ODR 冲突。
struct RibbonItem {
    std::string label;
    bool large = true;  // true: 大按钮（独占一列）；false: 小按钮（两枚一行）
    std::function<void()> onTap;
};

struct RibbonGroupData {
    std::string title;
    std::vector<RibbonItem> items;
};

struct RibbonTabData {
    std::string name;
    std::vector<RibbonGroupData> groups;
};

class Ribbon : public StatefulWidget {
public:
    explicit Ribbon(std::vector<RibbonTabData> tabs, std::function<void(int)> onTabChanged = {}, double width = 560.0,
                    KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), tabs_(std::move(tabs)), onTabChanged_(std::move(onTabChanged)),
          width_(width) {}

    const std::vector<RibbonTabData>& tabs() const { return tabs_; }
    const std::function<void(int)>& onTabChanged() const { return onTabChanged_; }
    double width() const { return width_; }
    /// 命令区右侧的尾部挂件（如解决方案配置下拉）；合并工具栏时常用来承载
    /// 原工具栏右端的控件。为 nullptr 时不渲染。
    const SharedPtr<Widget>& trailing() const { return trailing_; }
    Ribbon* setTrailing(SharedPtr<Widget> w) {
        trailing_ = std::move(w);
        return this;
    }
    /// 液态玻璃外观：宿主把本控件叠在壁纸/磨砂层之上时打开，所有面改用半透明白，
    /// 否则 Ribbon 的不透明 surface/surfaceVariant 会把整条玻璃糊成一块白卡。
    bool glass() const { return glass_; }
    Ribbon* setGlass(bool on) {
        glass_ = on;
        return this;
    }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Ribbon"; }

private:
    std::vector<RibbonTabData> tabs_;
    std::function<void(int)> onTabChanged_;
    double width_;
    bool glass_ = false;
    SharedPtr<Widget> trailing_;
};

class RibbonState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    Ribbon* rb() { return static_cast<Ribbon*>(widget()); }
    int active_ = 0;
    bool collapsed_ = false;
    std::string lastAction_;
};

// ── MaterialBanner ─────────────────────────────────────────────────────────────
/// Material 横幅通知。上游的 `MaterialBanner`（material/banner.dart）：
/// 整宽的重要提示，可带 leading 图标、操作按钮与关闭（✕）。
class MaterialBanner : public StatefulWidget {
public:
    using BannerAction = std::pair<std::string, std::function<void()>>;

    explicit MaterialBanner(std::string message, std::string leading = "!", std::vector<BannerAction> actions = {},
                            std::function<void()> onDismiss = {}, Color backgroundColor = theme().surface,
                            Color foregroundColor = theme().textPrimary, double width = 470.0, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), message_(std::move(message)), leading_(std::move(leading)),
          actions_(std::move(actions)), onDismiss_(std::move(onDismiss)), backgroundColor_(backgroundColor),
          foregroundColor_(foregroundColor), width_(width) {}

    const std::string& message() const { return message_; }
    const std::string& leading() const { return leading_; }
    const std::vector<BannerAction>& actions() const { return actions_; }
    const std::function<void()>& onDismiss() const { return onDismiss_; }
    Color backgroundColor() const { return backgroundColor_; }
    Color foregroundColor() const { return foregroundColor_; }
    double width() const { return width_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "MaterialBanner"; }

private:
    std::string message_, leading_;
    std::vector<BannerAction> actions_;
    std::function<void()> onDismiss_;
    Color backgroundColor_, foregroundColor_;
    double width_;
};

class MaterialBannerState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override;

private:
    MaterialBanner* bn() { return static_cast<MaterialBanner*>(widget()); }
    bool dismissed_ = false;
    std::string lastAction_;
};

// ── AppBar ─────────────────────────────────────────────────────────────────────
/// 顶部应用栏。上游的 `AppBar`（material/app_bar.dart）简化版：
/// leading + 标题 + actions，含底部 1px 分隔线与轻微投影。
class AppBar : public StatelessWidget {
public:
    explicit AppBar(std::string title, std::vector<IconCode> actions = {}, IconCode leading = IconCode::None,
                    double width = 470.0, Color backgroundColor = theme().primary, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), title_(std::move(title)), actions_(std::move(actions)), leading_(leading),
          width_(width), backgroundColor_(backgroundColor) {}

    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> kids;
        if (leading_ != IconCode::None) {
            auto lead = cui::make_shared<Icon>(leading_, 18.0, theme().textOnPrimary);
            auto leadBox = cui::make_shared<Container>(lead);
            leadBox->width(28.0).height(28.0).alignment(0.0, 0.0);
            kids.push_back(leadBox);
            kids.push_back(cui::make_shared<SizedBox>(8.0, 0.0));
        }
        auto t = cui::make_shared<Text>(title_, TextStyle().size(14.5).bold().fgColor(theme().textOnPrimary));
        kids.push_back(t);

        if (!actions_.empty()) {
            std::vector<SharedPtr<Widget>> tail;
            for (const auto& a : actions_) {
                auto at = cui::make_shared<Icon>(a, 16.0, theme().textOnPrimary);
                auto c = cui::make_shared<Container>(at);
                c->width(28.0).height(28.0).alignment(0.0, 0.0).decoration(
                        BoxDecoration{Color::fromRGBA(1, 1, 1, 0.16), 14.0});
                tail.push_back(c);
                tail.push_back(cui::make_shared<SizedBox>(6.0, 0.0));
            }
            tail.pop_back();
            auto tailRow = cui::make_shared<Row>(std::move(tail));
            tailRow->setMainAxisSize(MainAxisSize::Min);
            tailRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
            kids.push_back(cui::make_shared<Expanded>(1, cui::make_shared<SizedBox>(0.0, 0.0)));
            kids.push_back(tailRow);
        }

        auto row = cui::make_shared<Row>(std::move(kids));
        row->setMainAxisAlignment(MainAxisAlignment::Start);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto box = cui::make_shared<Container>(row);
        box->width(width_)
                .height(44.0)
                .padding(EdgeInsets::symmetric(0.0, 14.0))
                .decoration(BoxDecoration{backgroundColor_, 0.0, 1.0, Color::fromRGBA(0, 0, 0, 0.20), 8.0,
                                          Color::fromRGBA(0, 0, 0, 0.28)});
        return box;
    }
    std::string toStringShort() const override { return "AppBar"; }

private:
    std::string title_;
    std::vector<IconCode> actions_;
    IconCode leading_ = IconCode::None;
    double width_;
    Color backgroundColor_;
};

// ── WindowFrame ────────────────────────────────────────────────────────────────
/// 窗口外壳：标题栏 + 内容区 + 状态栏，外加 1px 边框（模拟一个独立窗口）。
/// 上游 无官方对应（桌面端通常由窗口管理器提供）。
class WindowFrame : public StatelessWidget {
public:
    explicit WindowFrame(std::string title, SharedPtr<Widget> body, std::vector<std::string> statusSections = {},
                         double width = 470.0, double height = 250.0, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), title_(std::move(title)), body_(std::move(body)),
          statusSections_(std::move(statusSections)), width_(width), height_(height) {}

    SharedPtr<Widget> build(BuildContext&) override {
        auto header = cui::make_shared<TitleBar>(title_, std::vector<std::string>{"—", "□", "×"}, width_);
        // 状态段：第一段在左，其余在右（对齐 legacy StatusBar 的左右分区）。
        std::string leftText;
        std::vector<std::string> rightText;
        if (!statusSections_.empty()) {
            leftText = statusSections_.front();
            rightText.assign(statusSections_.begin() + 1, statusSections_.end());
        }
        auto footer = cui::make_shared<StatusBar>(leftText, rightText, std::nullopt, width_);

        std::vector<SharedPtr<Widget>> kids{header};
        if (body_) {
            auto bodyBox = cui::make_shared<Container>(body_);
            bodyBox->padding(EdgeInsets::all(10.0));
            kids.push_back(cui::make_shared<Expanded>(1, bodyBox));
        } else {
            kids.push_back(cui::make_shared<Expanded>(1, cui::make_shared<SizedBox>(0.0, 0.0)));
        }
        if (!statusSections_.empty())
            kids.push_back(footer);

        auto col = cui::make_shared<Column>(std::move(kids));
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        auto box = cui::make_shared<Container>(col);
        box->width(width_).height(height_).decoration(BoxDecoration{
                theme().surface, 6.0, 1.0, Color::fromRGBA(0, 0, 0, 0.28), 14.0, Color::fromRGBA(0, 0, 0, 0.30)});
        return box;
    }
    std::string toStringShort() const override { return "WindowFrame"; }

private:
    std::string title_;
    SharedPtr<Widget> body_;
    std::vector<std::string> statusSections_;
    double width_, height_;
};

// ── DockPanel ──────────────────────────────────────────────────────────────────
/// 可停靠面板外壳：标题栏（图钉 / 折叠）+ 内容区。
/// 对应 legacy 的 Docking 概念（examples/gallery.cpp 的 page_docking）。
class DockPanel : public StatefulWidget {
public:
    explicit DockPanel(std::string title, SharedPtr<Widget> body, double width = 330.0, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), title_(std::move(title)), body_(std::move(body)), width_(width) {}

    const std::string& title() const { return title_; }
    const SharedPtr<Widget>& body() const { return body_; }
    double width() const { return width_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "DockPanel"; }

private:
    std::string title_;
    SharedPtr<Widget> body_;
    double width_;
};

class DockPanelState : public State {
public:
    void initState() override {
        auto* self = this;
        ticker_ = std::make_unique<Ticker>([self](Duration elapsed) {
            double e = elapsed.count();
            double t = self->animDur_ > 0.0 ? std::min(1.0, e / self->animDur_) : 1.0;
            double k = 1.0 - std::pow(1.0 - t, 3.0);  // easeOutCubic
            self->collapse_ = self->animFrom_ + (self->animTo_ - self->animFrom_) * k;
            self->setState();
            if (t >= 1.0 && self->ticker_)
                self->ticker_->stop();
        });
    }
    void dispose() override {
        if (ticker_)
            ticker_->stop();
    }
    SharedPtr<Widget> build(BuildContext&) override;
    void toggleCollapse() {
        collapsed_ = !collapsed_;
        animFrom_ = collapse_;
        animTo_ = collapsed_ ? 0.0 : 1.0;
        if (ticker_)
            ticker_->stop();
        ticker_ = std::make_unique<Ticker>([this](Duration elapsed) {
            double e = elapsed.count();
            double t = animDur_ > 0.0 ? std::min(1.0, e / animDur_) : 1.0;
            double k = 1.0 - std::pow(1.0 - t, 3.0);
            collapse_ = animFrom_ + (animTo_ - animFrom_) * k;
            setState();
            if (t >= 1.0 && ticker_)
                ticker_->stop();
        });
        ticker_->start();
    }

private:
    DockPanel* dp() { return static_cast<DockPanel*>(widget()); }
    bool pinned_ = true;
    bool collapsed_ = false;
    double collapse_ = 1.0;  // 1=展开 0=收起（动画）
    double animFrom_ = 1.0, animTo_ = 1.0;
    double animDur_ = 180.0;
    std::unique_ptr<Ticker> ticker_;
};

}  // namespace cui_core
