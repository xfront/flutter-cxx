#pragma once
/// @file widgets/tabs.h
/// @brief 上游 导航/展开/步骤结构化控件移植（第三批）。
///
/// 对应 上游源码：
///   - TabController / TabBar / TabBarView（material/tabs.dart）
///   - ExpansionTile（material/expansion_tile.dart）
///   - Stepper（material/stepper.dart）
///
/// 这些控件此前只存在于旧  层（navigation/tab_controller.h、
/// dialogs/expansion_tile.h、basic/stepper.h）。本文件把它们 port 成纯
/// cui_core::Widget（Widget/Element/RenderObject 三棵树）等价物，对齐
/// 「不再兼容旧的」目标：全部由已有新层原语（Row/Column/InkWell/Container/
/// IndexedStack/Text）组合而成，无新增渲染对象。
///
/// 布局/命中语义严格对齐上游：
///   - TabController 持有 index + 变化监听（非 Widget，由 TabBar/TabBarView 订阅重建）
///   - TabBar 选中项高亮、点击切页
///   - TabBarView 用 IndexedStack 只显示当前页
///   - ExpansionTile 头部点击切换展开，主体用 AnimatedCrossFade 淡入 + 撑高，
///     右侧箭头（Icons.expand_more 矢量版）转 0.5 圈；收起动画结束后主体不挂载
///   - Stepper 渲染步骤圆点 + 内容

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/theme.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/focus.h"
#include "cui/framework/framework.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/material.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/scroll.h"
#include "cui/widgets/text.h"

namespace cui {

// ── TabController ──────────────────────────────────────────────────────────
/// 标签页索引控制器。上游的 `TabController`（非 Widget：持有 index + 变化
/// 监听，TabBar / TabBarView 订阅它来重建）。setIndex 出界自动 clamp。
class TabController {
public:
    explicit TabController(int length = 0) : length_(std::max(0, length)) {}

    int length() const { return length_; }
    void setLength(int l) { length_ = std::max(0, l); }

    int index() const { return index_; }
    void setIndex(int i) {
        int clamped = std::clamp(i, 0, std::max(0, length_ - 1));
        if (clamped != index_) {
            index_ = clamped;
            notify();
        }
    }
    void next() { setIndex(index_ + 1); }
    void previous() { setIndex(index_ - 1); }

    /// 注册监听，返回注销闭包（State::dispose 时调用）。
    std::function<void()> addListener(std::function<void()> fn) {
        int id = nextId_++;
        listeners_.emplace_back(id, std::move(fn));
        auto self = this;
        return [self, id]() { self->removeListener(id); };
    }

private:
    void notify() {
        for (auto& kv : listeners_)
            kv.second();
    }
    void removeListener(int id) {
        listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                        [id](auto& kv) { return kv.first == id; }),
                         listeners_.end());
    }

    int length_ = 0;
    int index_ = 0;
    int nextId_ = 1;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
};

// ── TabBar ─────────────────────────────────────────────────────────────────
/// 标签栏。上游的 `TabBar`（material/tabs.dart）。
///
/// 把 tabs 渲染成一行可点标签；选中项带浅色背景高亮。订阅 controller 在
/// index 变化时重建高亮。每个标签用 InkWell 包裹以响应点击。
class TabBar : public StatefulWidget {
public:
    TabBar(TabController* controller, std::vector<SharedPtr<Widget>> tabs,
           Color indicatorColor = theme().primary, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), controller_(controller),
          tabs_(std::move(tabs)), indicatorColor_(indicatorColor) {}

    TabController* controller() const { return controller_; }
    const std::vector<SharedPtr<Widget>>& tabs() const { return tabs_; }
    const Color& indicatorColor() const { return indicatorColor_; }

    std::unique_ptr<State> createState() override;

private:
    TabController* controller_;
    std::vector<SharedPtr<Widget>> tabs_;
    Color indicatorColor_;
};

class TabBarState : public State {
public:
    void initState() override {
        if (auto* c = tab()->controller())
            unsub_ = c->addListener([this] { onControllerChanged(); });
    }
    void dispose() override {
        removeMenu();
        if (unsub_) {
            unsub_();
            unsub_ = {};
        }
    }
    SharedPtr<Widget> build(BuildContext&) override;

private:
    TabBar* tab() { return static_cast<TabBar*>(widget()); }

    /// controller 变了：重建标签栏高亮；下拉菜单开着时同步重建（✓ 要跟着走）。
    void onControllerChanged();

    /// 展开/收起溢出菜单（唯一写 `overflowOpen_` 的地方）。
    void setMenuOpen(bool open);
    /// 成功挂上浮层条目才返回 true；树上没有上层 Overlay 时弹不出来。
    bool insertMenu();
    void removeMenu();
    SharedPtr<Widget> buildMenu(BuildContext& entryContext, ScrollController& scroll);

    /// 标签栏渲染盒在**浮层坐标系**里的位置。
    ///
    /// `Positioned` 的偏移相对 Overlay 那层 Stack，而 Stack 未必落在窗口原点
    /// （gallery 里它在侧栏 + ribbon 之内）⇒ 直接拿 `localToGlobal` 的结果当
    /// 偏移会整体右下平移一个 Stack 原点。同 ComboBoxState。
    Rect barRectInOverlay(BuildContext& entryContext) const;

    std::function<void()> unsub_;
    bool overflowOpen_ = false;  ///< 标签过多时右侧「»」下拉列表是否展开
    /// 下拉菜单过长时滚动用。**必须是 shared_ptr 且被条目 builder 捕获一份**：
    /// 菜单挂在 Overlay 条目里，与标签栏是 Overlay 的两个兄弟子树，而
    /// `ComponentElement::unmount` 会先卸载并销毁前一个兄弟（标签栏 ⇒ 本 State
    /// 连同其成员），菜单里的 `RenderScrollbar` 析构时才去 controller 摘观察者
    /// ⇒ 值成员就是悬空指针（拆除期崩溃）。
    SharedPtr<ScrollController> menuScroll_;
    SharedPtr<OverlayEntry> menuEntry_;
    /// 标签窗口之外被折叠的项：右侧溢出（菜单上半）与左侧滚出（菜单下半）。
    std::vector<int> menuRightOverflow_;
    std::vector<int> menuLeftOverflow_;
};

// ── TabBarView ────────────────────────────────────────────────────────────
/// 标签内容区。上游的 `TabBarView`（material/tabs.dart）。
///
/// 用 IndexedStack 承载全部页面，只显示 controller.index() 指向的那一页；
/// 订阅 controller 在切换时重建。
class TabBarView : public StatefulWidget {
public:
    TabBarView(TabController* controller, std::vector<SharedPtr<Widget>> children,
               KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), controller_(controller),
          children_(std::move(children)) {}

    TabController* controller() const { return controller_; }
    const std::vector<SharedPtr<Widget>>& children() const { return children_; }

    std::unique_ptr<State> createState() override;

private:
    TabController* controller_;
    std::vector<SharedPtr<Widget>> children_;
};

class TabBarViewState : public State {
public:
    void initState() override {
        if (auto* c = view()->controller())
            unsub_ = c->addListener([this] { setState(); });
    }
    void dispose() override {
        if (unsub_) {
            unsub_();
            unsub_ = {};
        }
    }
    SharedPtr<Widget> build(BuildContext&) override {
        auto* c = view()->controller();
        int idx = c ? c->index() : 0;
        return cui::make_shared<IndexedStack>(idx, view()->children());
    }

private:
    TabBarView* view() { return static_cast<TabBarView*>(widget()); }
    std::function<void()> unsub_;
};

// ── ExpansionTile ─────────────────────────────────────────────────────────
/// 可展开/折叠的列表项。上游的 `ExpansionTile`（material/expansion_tile.dart）。
///
/// 头部 = 上游 ListTile 的形状（title 用 Expanded 撑开，尾部是 `Icons.expand_more`
/// 箭头，展开时转 0.5 圈）；点击切换展开。主体 = `Align → Padding → Column`
/// （上游 `_buildBody`），用 `AnimatedCrossFade` 做高度+淡入淡出（上游 3.47 的
/// `Expansible` 就是这个形状）。折叠且动画结束后主体**不再挂载**，对应上游
/// `maintainState=false` 的 `shouldRemoveBody`。
///
/// 箭头必须是矢量图标：▸/▾ 这类字符在本机字体里没有字形，早先用 Text 画箭头
/// 等于没有指示器，展开/收起完全看不出来。
class ExpansionTile : public StatefulWidget {
public:
    ExpansionTile(SharedPtr<Widget> title,
                  std::vector<SharedPtr<Widget>> children,
                  bool initiallyExpanded = false, KeyPtr key = nullptr,
                  // 以下三个参数的默认值即上游口径（childrenPadding 为
                  // EdgeInsets.zero、两个对齐都取 center）。
                  EdgeInsets childrenPadding = EdgeInsets{},
                  Alignment expandedAlignment = Alignment::center(),
                  CrossAxisAlignment expandedCrossAxisAlignment =
                          CrossAxisAlignment::Center)
        : StatefulWidget(std::move(key)), title_(std::move(title)),
          children_(std::move(children)), initiallyExpanded_(initiallyExpanded),
          childrenPadding_(childrenPadding), expandedAlignment_(expandedAlignment),
          expandedCrossAxisAlignment_(expandedCrossAxisAlignment) {}

    const SharedPtr<Widget>& title() const { return title_; }
    const std::vector<SharedPtr<Widget>>& children() const { return children_; }
    bool initiallyExpanded() const { return initiallyExpanded_; }
    EdgeInsets childrenPadding() const { return childrenPadding_; }
    Alignment expandedAlignment() const { return expandedAlignment_; }
    CrossAxisAlignment expandedCrossAxisAlignment() const {
        return expandedCrossAxisAlignment_;
    }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Widget> title_;
    std::vector<SharedPtr<Widget>> children_;
    bool initiallyExpanded_;
    EdgeInsets childrenPadding_;
    Alignment expandedAlignment_;
    CrossAxisAlignment expandedCrossAxisAlignment_;
};

class ExpansionTileState : public State {
public:
    void initState() override {
        expanded_ = tile()->initiallyExpanded();
        bodyMounted_ = expanded_;
    }
    SharedPtr<Widget> build(BuildContext&) override;
    bool isExpanded() const { return expanded_; }

private:
    void toggle();
    /// 淡入淡出收尾：收起动画跑完才把主体从树上摘掉。
    void onFadeSettled();

    ExpansionTile* tile() { return static_cast<ExpansionTile*>(widget()); }
    bool expanded_ = false;
    bool bodyMounted_ = false;
};

// ── Stepper ───────────────────────────────────────────────────────────────
/// 步骤指示器 + 内容。上游的 `Stepper`（material/stepper.dart）简化版。
struct Step {
    SharedPtr<Widget> title;
    SharedPtr<Widget> content;
    bool isActive = true;
};

class Stepper : public StatelessWidget {
public:
    Stepper(int currentStep, std::vector<Step> steps, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), currentStep_(currentStep),
          steps_(std::move(steps)) {}

    int currentStep() const { return currentStep_; }
    const std::vector<Step>& steps() const { return steps_; }

    SharedPtr<Widget> build(BuildContext&) override;

private:
    int currentStep_;
    std::vector<Step> steps_;
};

}  // namespace cui_core
