#pragma once
/// @file widgets/scaffold.h
/// @brief 上游的 Scaffold / ScaffoldState 移植。
///
/// 对应 上游源码：
///   上游 material/scaffold.dart
///     - `Scaffold`（:1683-2170）
///     - `ScaffoldState`（:2192-2500）
///     - `Scaffold.of`（:2043）/ `Scaffold.maybeOf`（:2087）
///
/// 架构对齐说明：
///   上游的 Scaffold 用 `CustomMultiChildLayout(delegate: _ScaffoldLayout)`
///   组织子节点，并用 `LayoutId` 给每个子节点标注槽位。本移植保持同一结构：
///     - `ScaffoldLayout`   = 上游的 `CustomMultiChildLayout`
///     - `ScaffoldSlotId`   = 上游的 `LayoutId`（ParentDataWidget）
///     - `RenderScaffold`   = 上游的 `RenderCustomMultiChildLayout`
///       + `_ScaffoldLayout` delegate
///
/// Material 层（2026-11-05 补齐）：`ScaffoldState.build` 的产物与上游
/// scaffold.dart:3232-3238 同形，整棵子树铺在一层
/// `Material(color: backgroundColor ?? 主题底色)` 上（`widgets/material.h`）。
/// 上游那层 `_ScaffoldScope` / `ScrollNotificationObserver` / `Actions`
/// 本仓没有对应物，未接。
///
/// 与官方 Material 层的差异（已在各处注明）：
///   1. `appBar` 等于普通 `Widget`（官方要求 `PreferredSizeWidget`）。
///   2. `persistentFooterButtons` 直接以列表给出，不套 `OverflowBar`。
///   3. 抽屉无滑入动画：以「关闭时平移出屏」表达展开/收起语义。
///   4. `minInsets` / `minViewPadding` 由 Widget 显式给出（官方取自 MediaQuery）。
///   5. 不含 `SnackBar` / `MaterialBanner` / `ScaffoldMessenger`（属 Material 层）。

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/framework/framework.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_scaffold.h"

namespace cui {

class ScaffoldState;

/// 给子节点标注 Scaffold 槽位。上游的 `LayoutId`。
///
/// 它是 `ParentDataWidget`：自身不产生 RenderObject，只把槽位号写进
/// 子 RenderObject 的 `ScaffoldParentData`（对齐 上游的
/// `MultiChildLayoutParentData.id`）。
class ScaffoldSlotId : public ParentDataWidget {
public:
    ScaffoldSlotId(ScaffoldSlot slot, SharedPtr<Widget> child = nullptr,
                   KeyPtr key = nullptr)
        : ParentDataWidget(std::move(key), std::move(child)), slot_(slot) {}

    /// 槽位号。
    ScaffoldSlot slot() const { return slot_; }

    void applyParentData(RenderObject* renderObject) override {
        auto* pd = dynamic_cast<ScaffoldParentData*>(
                renderObject ? renderObject->parentData() : nullptr);
        if (!pd)
            return;
        pd->slot = slot_;
    }

private:
    ScaffoldSlot slot_;
};

/// Scaffold 的布局容器。上游的 `CustomMultiChildLayout` +
/// `_ScaffoldLayout` delegate 的组合。
class ScaffoldLayout : public MultiChildRenderObjectWidget {
public:
    ScaffoldLayout(std::vector<SharedPtr<Widget>> children = {}, bool extendBody = false,
                   bool extendBodyBehindAppBar = false, EdgeInsets minInsets = {},
                   EdgeInsets minViewPadding = {},
                   ScaffoldFabLocation fabLocation = ScaffoldFabLocation::endFloat,
                   bool drawerOpen = false, bool endDrawerOpen = false, KeyPtr key = nullptr)
        : MultiChildRenderObjectWidget(std::move(key), std::move(children)),
          extendBody_(extendBody), extendBodyBehindAppBar_(extendBodyBehindAppBar),
          minInsets_(minInsets), minViewPadding_(minViewPadding), fabLocation_(fabLocation),
          drawerOpen_(drawerOpen), endDrawerOpen_(endDrawerOpen) {}

    RenderObject* createRenderObject() override { return new RenderScaffold(); }

    void updateRenderObject(RenderObject* ro) override {
        auto* scaffold = dynamic_cast<RenderScaffold*>(ro);
        if (!scaffold)
            return;
        scaffold->setExtendBody(extendBody_);
        scaffold->setExtendBodyBehindAppBar(extendBodyBehindAppBar_);
        scaffold->setMinInsets(minInsets_);
        scaffold->setMinViewPadding(minViewPadding_);
        scaffold->setFabLocation(fabLocation_);
        scaffold->setDrawerOpen(drawerOpen_);
        scaffold->setEndDrawerOpen(endDrawerOpen_);
    }

private:
    bool extendBody_;
    bool extendBodyBehindAppBar_;
    EdgeInsets minInsets_;
    EdgeInsets minViewPadding_;
    ScaffoldFabLocation fabLocation_;
    bool drawerOpen_;
    bool endDrawerOpen_;
};

/// Scaffold 的状态。上游的 `ScaffoldState`（:2192-2500）。
///
/// 抽屉 API 的官方语义（逐条对齐）：
///   - `openDrawer`（:2283-2288）：若 endDrawer 展开则先关闭它，再展开 drawer。
///   - `closeDrawer`（:2432-2437）：仅在 `hasDrawer && isDrawerOpen` 时关闭。
///   - `openEndDrawer`（:2303-2308）：若 drawer 展开则先关闭它，再展开 endDrawer。
///   - `closeEndDrawer`（:2441-2446）：仅在 `hasEndDrawer && isEndDrawerOpen` 时关闭。
class ScaffoldState : public State {
public:
    /// 是否有 appBar。上游的 `ScaffoldState.hasAppBar`（:2213）。
    bool hasAppBar() const;

    /// 是否有 drawer。上游的 `ScaffoldState.hasDrawer`（:2216）。
    bool hasDrawer() const;

    /// 是否有 endDrawer。上游的 `ScaffoldState.hasEndDrawer`（:2219）。
    bool hasEndDrawer() const;

    /// 是否有浮动按钮。上游的 `ScaffoldState.hasFloatingActionButton`（:2222）。
    bool hasFloatingActionButton() const;

    /// drawer 是否展开。上游的 `ScaffoldState.isDrawerOpen`（:2239）。
    bool isDrawerOpen() const { return drawerOpened_; }

    /// endDrawer 是否展开。上游的 `ScaffoldState.isEndDrawerOpen`（:2250）。
    bool isEndDrawerOpen() const { return endDrawerOpened_; }

    /// 展开 drawer。上游的 `ScaffoldState.openDrawer`（:2283-2288）。
    void openDrawer();

    /// 关闭 drawer。上游的 `ScaffoldState.closeDrawer`（:2432-2437）。
    void closeDrawer();

    /// 展开 endDrawer。上游的 `ScaffoldState.openEndDrawer`（:2303-2308）。
    void openEndDrawer();

    /// 关闭 endDrawer。上游的 `ScaffoldState.closeEndDrawer`（:2441-2446）。
    void closeEndDrawer();

    /// 查找最近的 ScaffoldState。上游的 `Scaffold.of`（:2043-2072）。
    /// 找不到时抛异常。
    static ScaffoldState* of(BuildContext& context);

    /// 查找最近的 ScaffoldState。上游的 `Scaffold.maybeOf`（:2087-2089）。
    /// 找不到时返回 nullptr。
    static ScaffoldState* maybeOf(BuildContext& context);

protected:
    void initState() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    /// 抽屉展开状态变化回调（官方 `_drawerOpenedCallback`，:2252-2259）。
    void handleDrawerOpenedChanged(bool isOpened);

    /// 末端抽屉展开状态变化回调（官方 `_endDrawerOpenedCallback`，:2261-2268）。
    void handleEndDrawerOpenedChanged(bool isOpened);

    bool drawerOpened_ = false;
    bool endDrawerOpened_ = false;
};

/// Scaffold。上游的 `Scaffold`（:1683-2170）。
class Scaffold : public StatefulWidget {
public:
    Scaffold(SharedPtr<Widget> body = nullptr, SharedPtr<Widget> appBar = nullptr,
            SharedPtr<Widget> bottomNavigationBar = nullptr,
            SharedPtr<Widget> floatingActionButton = nullptr,
            SharedPtr<Widget> drawer = nullptr, SharedPtr<Widget> endDrawer = nullptr,
            std::vector<SharedPtr<Widget>> persistentFooterButtons = {},
            ScaffoldFabLocation floatingActionButtonLocation = ScaffoldFabLocation::endFloat,
            bool extendBody = false, bool extendBodyBehindAppBar = false,
            EdgeInsets minInsets = {}, EdgeInsets minViewPadding = {},
            std::function<void(bool)> onDrawerChanged = nullptr,
            std::function<void(bool)> onEndDrawerChanged = nullptr,
            std::optional<Color> backgroundColor = std::nullopt, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), body_(std::move(body)), appBar_(std::move(appBar)),
          bottomNavigationBar_(std::move(bottomNavigationBar)),
          floatingActionButton_(std::move(floatingActionButton)), drawer_(std::move(drawer)),
          endDrawer_(std::move(endDrawer)),
          persistentFooterButtons_(std::move(persistentFooterButtons)),
          floatingActionButtonLocation_(floatingActionButtonLocation), extendBody_(extendBody),
          extendBodyBehindAppBar_(extendBodyBehindAppBar), minInsets_(minInsets),
          minViewPadding_(minViewPadding), onDrawerChanged_(std::move(onDrawerChanged)),
          onEndDrawerChanged_(std::move(onEndDrawerChanged)),
          backgroundColor_(backgroundColor) {}

    /// 主要内容。上游的 `Scaffold.body`（:1780）。
    const SharedPtr<Widget>& body() const { return body_; }

    /// 顶部栏。上游的 `Scaffold.appBar`（:1760）。
    const SharedPtr<Widget>& appBar() const { return appBar_; }

    /// 底部导航栏。上游的 `Scaffold.bottomNavigationBar`。
    const SharedPtr<Widget>& bottomNavigationBar() const { return bottomNavigationBar_; }

    /// 浮动按钮。上游的 `Scaffold.floatingActionButton`（:1785）。
    const SharedPtr<Widget>& floatingActionButton() const { return floatingActionButton_; }

    /// 侧边抽屉。上游的 `Scaffold.drawer`。
    const SharedPtr<Widget>& drawer() const { return drawer_; }

    /// 末端侧边抽屉。上游的 `Scaffold.endDrawer`。
    const SharedPtr<Widget>& endDrawer() const { return endDrawer_; }

    /// 常驻底栏按钮。上游的 `Scaffold.persistentFooterButtons`（:1806）。
    const std::vector<SharedPtr<Widget>>& persistentFooterButtons() const {
        return persistentFooterButtons_;
    }

    /// FAB 位置。上游的 `Scaffold.floatingActionButtonLocation`（:1790）。
    ScaffoldFabLocation floatingActionButtonLocation() const {
        return floatingActionButtonLocation_;
    }

    /// body 是否延伸到底部导航栏之下。上游的 `Scaffold.extendBody`（:1734）。
    bool extendBody() const { return extendBody_; }

    /// body 是否延伸到 appBar 之下。上游的 `Scaffold.extendBodyBehindAppBar`（:1757）。
    bool extendBodyBehindAppBar() const { return extendBodyBehindAppBar_; }

    /// 最小安全内边距（官方取自 MediaQuery）。
    const EdgeInsets& minInsets() const { return minInsets_; }

    /// 最小视图内边距（官方取自 MediaQuery）。
    const EdgeInsets& minViewPadding() const { return minViewPadding_; }

    /// 抽屉展开状态回调。上游的 `Scaffold.onDrawerChanged`。
    const std::function<void(bool)>& onDrawerChanged() const { return onDrawerChanged_; }

    /// 末端抽屉展开状态回调。上游的 `Scaffold.onEndDrawerChanged`。
    const std::function<void(bool)>& onEndDrawerChanged() const { return onEndDrawerChanged_; }

    /// 整页底色。上游的 `Scaffold.backgroundColor`（scaffold.dart:1893-1897）。
    /// 空表示取主题的 scaffold 底色——本仓无该 token，落在 `theme().surface`
    /// （M3 的 `scaffoldBackgroundColor` 就是 `colorScheme.surface`）。
    const std::optional<Color>& backgroundColor() const { return backgroundColor_; }

    /// 查找最近的 ScaffoldState。上游的 `Scaffold.of`（:2043-2072）。
    static ScaffoldState* of(BuildContext& context);

    /// 查找最近的 ScaffoldState。上游的 `Scaffold.maybeOf`（:2087-2089）。
    static ScaffoldState* maybeOf(BuildContext& context);

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Widget> body_;
    SharedPtr<Widget> appBar_;
    SharedPtr<Widget> bottomNavigationBar_;
    SharedPtr<Widget> floatingActionButton_;
    SharedPtr<Widget> drawer_;
    SharedPtr<Widget> endDrawer_;
    std::vector<SharedPtr<Widget>> persistentFooterButtons_;
    ScaffoldFabLocation floatingActionButtonLocation_;
    bool extendBody_;
    bool extendBodyBehindAppBar_;
    EdgeInsets minInsets_;
    EdgeInsets minViewPadding_;
    std::function<void(bool)> onDrawerChanged_;
    std::function<void(bool)> onEndDrawerChanged_;
    std::optional<Color> backgroundColor_;
};

}
