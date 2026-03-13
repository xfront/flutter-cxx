#pragma once
/// @file rendering/render_scaffold.h
/// @brief 上游的 Scaffold 布局渲染对象移植。
///
/// 对应 上游源码：
///   上游 material/scaffold.dart
///     - `_ScaffoldSlot`（:64-77）
///     - `_ScaffoldLayout.performLayout`（:1024-1293）
///     - `_ScaffoldLayout.shouldRelayout`（:1295-1304）
///   上游 material/floating_action_button_location.dart
///     - `StandardFabLocation` / `FabEndOffsetX` / `FabCenterOffsetX` /
///       `FabStartOffsetX` / `FabFloatOffsetY` / `FabDockedOffsetY`
///
/// 架构对齐说明：
///   上游 用 `CustomMultiChildLayout(delegate: _ScaffoldLayout(...))`
///   + `LayoutId` 给子节点打槽位号。本移植保留同一套机制：
///     - `ScaffoldLayout`（widgets 层）= 上游的 CustomMultiChildLayout
///     - `ScaffoldSlotId`（widgets 层，ParentDataWidget）= 上游的 LayoutId
///     - `RenderScaffold`（本文件）= 上游的 RenderCustomMultiChildLayout
///       + `_ScaffoldLayout` delegate 的合并体
///     - `ScaffoldParentData` = 上游的 `MultiChildLayoutParentData`
///
/// 布局语义（与官方逐条对齐，注释标出官方行号）：
///   1. appBar 全宽置顶（:1038-1042）；`extendBodyBehindAppBar` 时
///      `contentTop = 0`，body 从屏幕顶端开始。
///   2. bottomNavigationBar 贴底（:1045-1053）。
///   3. persistentFooter 在 bottomNavigationBar 之上（:1055-1069）。
///   4. `contentBottom = max(0, bottom - max(minInsets.bottom, bottomWidgetsHeight))`
///      （:1085-1088）——键盘（minInsets）优先于底部挂件。
///   5. body 布局在 `contentTop .. contentBottom`（:1090-1112）；
///      `extendBody` 时 body 延伸到 bottomNavigationBar 之下。
///   6. bottomSheet / snackBar 贴底居中（:1143-1153、:1191-1245）。
///   7. FAB 按 `FloatingActionButtonLocation` 的几何公式定位（:1156-1189）。
///   8. drawer / endDrawer 覆盖式铺满（:1280-1287）；关闭时平移出屏。

#include <algorithm>
#include <memory>
#include <vector>

#include "cui/rendering/render_object.h"
#include "cui/rendering/render_box.h"

namespace cui {

/// Scaffold 的槽位。上游的 `_ScaffoldSlot`（material_scaffold.dart:64-77）。
///
/// 枚举顺序与官方完全一致，便于对照。
enum class ScaffoldSlot {
    body,
    appBar,
    bodyScrim,
    bottomSheet,
    snackBar,
    materialBanner,
    persistentFooter,
    bottomNavigationBar,
    floatingActionButton,
    drawer,
    endDrawer,
    statusBar,
};

/// 子节点的父数据。上游的 `MultiChildLayoutParentData`。
///
/// `slot` 由 `ScaffoldSlotId`（对应 上游的 `LayoutId`）写入。
struct ScaffoldParentData : public ParentData {
    /// 该子节点占据的槽位。
    ScaffoldSlot slot = ScaffoldSlot::body;
};

/// 浮动按钮的位置。上游的 `FloatingActionButtonLocation`。
///
/// 上游的实现是一组 mixin 组合（`FabEndOffsetX` 等），本移植用等价的
/// 枚举 + 同一套几何公式（见 `scaffoldFabOffset`）表达。
///
/// 只移植 Material 层的 6 个常用位置（LTR 语义；本移植尚无 Directionality，
/// 因此固定按 LTR 计算）。
enum class ScaffoldFabLocation {
    /// 默认位置：右下角悬浮。上游的 `FloatingActionButtonLocation.endFloat`。
    endFloat,
    /// 底部居中悬浮。上游的 `centerFloat`。
    centerFloat,
    /// 左下角悬浮。上游的 `startFloat`。
    startFloat,
    /// 右下角「嵌入」底栏。上游的 `endDocked`。
    endDocked,
    /// 底部居中「嵌入」底栏。上游的 `centerDocked`。
    centerDocked,
    /// 左下角「嵌入」底栏。上游的 `startDocked`。
    startDocked,
};

/// Scaffold 的布局几何。上游的 `ScaffoldPrelayoutGeometry`（:665-811）。
///
/// FAB 定位所需的全部输入都在这里，语义与官方一一对应。
struct ScaffoldGeometry {
    /// Scaffold 自身尺寸。上游的 `scaffoldSize`。
    Size scaffoldSize;
    /// 内容顶边（appBar 之下）。上游的 `contentTop`。
    double contentTop = 0.0;
    /// 内容底边。上游的 `contentBottom`。
    double contentBottom = 0.0;
    /// 最小安全内边距（键盘等）。上游的 `minInsets`。
    EdgeInsets minInsets;
    /// 最小视图内边距（刘海 / home 条）。上游的 `minViewPadding`。
    EdgeInsets minViewPadding;
    /// FAB 尺寸。上游的 `floatingActionButtonSize`。
    Size fabSize;
    /// SnackBar 尺寸。上游的 `snackBarSize`。
    Size snackBarSize;
    /// BottomSheet 尺寸。上游的 `bottomSheetSize`。
    Size bottomSheetSize;
};

/// FAB 的边距常量。上游的 `kFloatingActionButtonMargin`（= 16.0）。
inline constexpr double kFloatingActionButtonMargin = 16.0;

/// 计算 FAB 的偏移。上游的 `FloatingActionButtonLocation.getOffset`。
///
/// 公式严格取自官方 `floating_action_button_location.dart`：
///   - Float 系列（endFloat / centerFloat / startFloat）用 `FabFloatOffsetY`：
///       safeMargin = max(16, minViewPadding.bottom - bottomContentHeight + 16)
///       fabY = contentBottom - fabHeight - safeMargin
///   - Docked 系列（endDocked / centerDocked / startDocked）用 `FabDockedOffsetY`：
///       safeMargin 按 contentMargin / minInsets.bottom 三分支决定
///       fabY = contentBottom - fabHeight / 2 - safeMargin
///       y = min(maxFabY, fabY)
///   - SnackBar / BottomSheet 存在时进一步上移（官方同款 min 逻辑）。
///
/// 本移植固定 LTR（无 Directionality）。
Offset scaffoldFabOffset(ScaffoldFabLocation location, const ScaffoldGeometry& geometry);

/// Scaffold 的布局渲染对象。
///
/// 对应 上游的 `RenderCustomMultiChildLayout` + `_ScaffoldLayout`。
class RenderScaffold : public RenderBoxContainer {
public:
    RenderScaffold() = default;

    /// 是否让 body 延伸到 bottomNavigationBar 之下。
    /// 上游的 `Scaffold.extendBody`（:1734）。
    bool extendBody() const { return extendBody_; }
    void setExtendBody(bool value) {
        if (extendBody_ == value)
            return;
        extendBody_ = value;
        markNeedsLayout();
    }

    /// 是否让 body 延伸到 appBar 之下。
    /// 上游的 `Scaffold.extendBodyBehindAppBar`（:1757）。
    bool extendBodyBehindAppBar() const { return extendBodyBehindAppBar_; }
    void setExtendBodyBehindAppBar(bool value) {
        if (extendBodyBehindAppBar_ == value)
            return;
        extendBodyBehindAppBar_ = value;
        markNeedsLayout();
    }

    /// 最小安全内边距（替代 上游的 MediaQuery）。
    EdgeInsets minInsets() const { return minInsets_; }
    void setMinInsets(const EdgeInsets& value) {
        if (minInsets_.left == value.left && minInsets_.top == value.top &&
            minInsets_.right == value.right && minInsets_.bottom == value.bottom)
            return;
        minInsets_ = value;
        markNeedsLayout();
    }

    /// 最小视图内边距（替代 上游的 MediaQuery）。
    EdgeInsets minViewPadding() const { return minViewPadding_; }
    void setMinViewPadding(const EdgeInsets& value) {
        if (minViewPadding_.left == value.left && minViewPadding_.top == value.top &&
            minViewPadding_.right == value.right && minViewPadding_.bottom == value.bottom)
            return;
        minViewPadding_ = value;
        markNeedsLayout();
    }

    /// FAB 的位置。
    ScaffoldFabLocation fabLocation() const { return fabLocation_; }
    void setFabLocation(ScaffoldFabLocation value) {
        if (fabLocation_ == value)
            return;
        fabLocation_ = value;
        markNeedsLayout();
    }

    /// 抽屉是否展开。上游的 `ScaffoldState.isDrawerOpen`（:2239）。
    bool drawerOpen() const { return drawerOpen_; }
    void setDrawerOpen(bool value) {
        if (drawerOpen_ == value)
            return;
        drawerOpen_ = value;
        markNeedsLayout();
    }

    /// 末端抽屉是否展开。上游的 `ScaffoldState.isEndDrawerOpen`（:2250）。
    bool endDrawerOpen() const { return endDrawerOpen_; }
    void setEndDrawerOpen(bool value) {
        if (endDrawerOpen_ == value)
            return;
        endDrawerOpen_ = value;
        markNeedsLayout();
    }

    /// 创建 `ScaffoldParentData`（对应 上游的 setupParentData）。
    void setupParentData(RenderObject* child) override;

protected:
    void performLayout() override;

    /// 绘制子节点（按 children 顺序，即官方 build 中的入列顺序）。
    void paint(PaintingContext& context, const Offset& offset) override;

private:
    /// 收集「槽位 → 子节点」映射（上游的 `hasChild` / `layoutChild`）。
    void collectSlots(std::vector<RenderObject*>& bySlot, size_t slotCount);

    bool extendBody_ = false;
    bool extendBodyBehindAppBar_ = false;
    EdgeInsets minInsets_;
    EdgeInsets minViewPadding_;
    ScaffoldFabLocation fabLocation_ = ScaffoldFabLocation::endFloat;
    bool drawerOpen_ = false;
    bool endDrawerOpen_ = false;
};

}  // namespace cui_core
