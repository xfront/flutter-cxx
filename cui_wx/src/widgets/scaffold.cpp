/// @file widgets/scaffold.cpp
/// @brief Scaffold / ScaffoldState 实现。
///
/// 语义对齐 上游官方源码：
///   上游 material/scaffold.dart
///     - `ScaffoldState`（:2192-2500）
///     - `ScaffoldState.build`（:3007-3263）
///     - `ScaffoldState._addIfNonNull`（:2903-2934）
///     - `ScaffoldState._buildDrawer`（:2956-2989）
///     - `ScaffoldState._buildEndDrawer`（:2936-2954）
///     - `Scaffold.of`（:2043-2072）/ `Scaffold.maybeOf`（:2087-2089）
///
/// 与官方 Material 层的差异（本移植尚无 Material 层）：
///   1. 不套 `MediaQuery`（`_addIfNonNull` 里的 padding 裁剪），
///      安全区由 `Scaffold.minInsets` / `minViewPadding` 显式传入。
///   2. 不套 `DrawerController`（无动画基础设施），抽屉的展开 / 收起
///      由 `RenderScaffold` 以「平移出屏」表达。
///   3. `persistentFooterButtons` 不套 `OverflowBar`，改为
///      `Padding(8) → Row(MainAxisSize::Min)`。
///   4. 不含 `SnackBar` / `MaterialBanner` / `ScaffoldMessenger` / `Theme`。

#include "cui/widgets/scaffold.h"

#include <stdexcept>
#include <utility>

#include "cui/widgets/basic.h"
#include "cui/widgets/material.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {

namespace {

/// 把一个可空 widget 包进槽位标注里。上游的 `_addIfNonNull`。
///
/// 官方 `_addIfNonNull`（:2903-2934）做两件事：
///   1. 用 `MediaQuery.removePadding` 裁掉不需要的安全区padding；
///   2. 把 child 包成 `LayoutId(id: childId, child: ...)`。
///
/// 本移植无 MediaQuery，第 1 步降级为「不裁剪」（安全区改由
/// `minInsets` / `minViewPadding` 参与布局计算）；第 2 步保留。
SharedPtr<Widget> addIfNonNull(SharedPtr<Widget> child, ScaffoldSlot slot) {
    if (!child)
        return nullptr;
    return cui::make_shared<ScaffoldSlotId>(slot, std::move(child));
}

}  // namespace

// ── ScaffoldState ───────────────────────────────────────────────────────────

bool ScaffoldState::hasAppBar() const {
    // 上游的 ScaffoldState.hasAppBar（:2213）。
    auto* scaffold = dynamic_cast<const Scaffold*>(widget());
    return scaffold && scaffold->appBar() != nullptr;
}

bool ScaffoldState::hasDrawer() const {
    // 上游的 ScaffoldState.hasDrawer（:2216）。
    auto* scaffold = dynamic_cast<const Scaffold*>(widget());
    return scaffold && scaffold->drawer() != nullptr;
}

bool ScaffoldState::hasEndDrawer() const {
    // 上游的 ScaffoldState.hasEndDrawer（:2219）。
    auto* scaffold = dynamic_cast<const Scaffold*>(widget());
    return scaffold && scaffold->endDrawer() != nullptr;
}

bool ScaffoldState::hasFloatingActionButton() const {
    // 上游的 ScaffoldState.hasFloatingActionButton（:2222）。
    auto* scaffold = dynamic_cast<const Scaffold*>(widget());
    return scaffold && scaffold->floatingActionButton() != nullptr;
}

void ScaffoldState::handleDrawerOpenedChanged(bool isOpened) {
    // 上游的 `_drawerOpenedCallback`（:2252-2259）。
    // 官方由 DrawerController 驱动；本移植由 openDrawer / closeDrawer
    // 直接调用，语义一致：状态变化 → setState → 通知 onDrawerChanged。
    if (drawerOpened_ == isOpened)
        return;
    drawerOpened_ = isOpened;
    setState();
    auto* scaffold = dynamic_cast<const Scaffold*>(widget());
    if (scaffold && scaffold->onDrawerChanged())
        scaffold->onDrawerChanged()(isOpened);
}

void ScaffoldState::handleEndDrawerOpenedChanged(bool isOpened) {
    // 上游的 `_endDrawerOpenedCallback`（:2261-2268）。
    if (endDrawerOpened_ == isOpened)
        return;
    endDrawerOpened_ = isOpened;
    setState();
    auto* scaffold = dynamic_cast<const Scaffold*>(widget());
    if (scaffold && scaffold->onEndDrawerChanged())
        scaffold->onEndDrawerChanged()(isOpened);
}

void ScaffoldState::openDrawer() {
    // 上游的 ScaffoldState.openDrawer（:2283-2288）：
    // 若 endDrawer 展开，先关闭它，再展开 drawer。
    if (endDrawerOpened_)
        handleEndDrawerOpenedChanged(false);
    if (!hasDrawer())
        return;
    handleDrawerOpenedChanged(true);
}

void ScaffoldState::closeDrawer() {
    // 上游的 ScaffoldState.closeDrawer（:2432-2437）：
    // 仅在 hasDrawer && isDrawerOpen 时关闭。
    if (hasDrawer() && isDrawerOpen())
        handleDrawerOpenedChanged(false);
}

void ScaffoldState::openEndDrawer() {
    // 上游的 ScaffoldState.openEndDrawer（:2303-2308）：
    // 若 drawer 展开，先关闭它，再展开 endDrawer。
    if (drawerOpened_)
        handleDrawerOpenedChanged(false);
    if (!hasEndDrawer())
        return;
    handleEndDrawerOpenedChanged(true);
}

void ScaffoldState::closeEndDrawer() {
    // 上游的 ScaffoldState.closeEndDrawer（:2441-2446）：
    // 仅在 hasEndDrawer && isEndDrawerOpen 时关闭。
    if (hasEndDrawer() && isEndDrawerOpen())
        handleEndDrawerOpenedChanged(false);
}

ScaffoldState* ScaffoldState::maybeOf(BuildContext& context) {
    // 上游的 Scaffold.maybeOf（:2087-2089）：
    // `context.findAncestorStateOfType<ScaffoldState>()`。
    return context.findAncestorStateOfType<ScaffoldState>();
}

ScaffoldState* ScaffoldState::of(BuildContext& context) {
    // 上游的 Scaffold.of（:2043-2072）：找不到时抛异常。
    ScaffoldState* result = maybeOf(context);
    if (!result)
        throw std::logic_error("No Scaffold widget found in the widget tree.");
    return result;
}

void ScaffoldState::initState() {
    State::initState();
    // 官方在 initState 之外通过 RestorableBool 恢复抽屉状态（:2211-2212），
    // 本移植无 Restoration 层，初始状态即「关闭」。
}

SharedPtr<Widget> ScaffoldState::build(BuildContext& context) {
    // 上游的 ScaffoldState.build（:3007-3263）。
    //
    // 关键：**子节点的入列顺序即绘制顺序**。官方把 body 放在最前、
    // appBar 放在 body 之后，因此 appBar 的阴影会覆盖在 body 之上
    // （官方 :1027-1030 的注释）。
    auto* scaffold = dynamic_cast<const Scaffold*>(widget());
    if (!scaffold)
        return nullptr;

    std::vector<SharedPtr<Widget>> children;

    // ── body（官方 :3014-3029）───────────────────────────────────────────
    // 官方把 body 包在 `_BodyBuilder` + `KeyedSubtree(_bodyKey)` 里，
    // 并通过 MediaQuery 裁剪安全区。本移植直接放 body。
    children.push_back(addIfNonNull(scaffold->body(), ScaffoldSlot::body));

    // ── appBar（官方 :3043-3060）─────────────────────────────────────────
    // 官方用 `ConstrainedBox(maxHeight: _appBarMaxHeight)` 限制高度，
    // 高度来自 `PreferredSizeWidget.preferredSize`。本移植无
    // PreferredSizeWidget 体系，直接放 appBar，由其自身决定高度。
    children.push_back(addIfNonNull(scaffold->appBar(), ScaffoldSlot::appBar));

    // ── persistentFooter（官方 :3127-3156）───────────────────────────────
    // 官方：Container(decoration) → SafeArea → IntrinsicHeight →
    //       Padding(8) → Align(alignment) → OverflowBar(spacing: 8)
    // 本移植：Padding(8) → Align(bottomEnd) → Row(MainAxisSize::Min)
    if (!scaffold->persistentFooterButtons().empty()) {
        auto row = cui::make_shared<Row>(scaffold->persistentFooterButtons());
        row->setMainAxisSize(MainAxisSize::Min);
        auto align = cui::make_shared<Align>(1.0, 1.0, row);
        auto padded = cui::make_shared<Padding>(EdgeInsets::all(8), align);
        children.push_back(addIfNonNull(padded, ScaffoldSlot::persistentFooter));
    }

    // ── bottomNavigationBar（官方 :3158-3170）────────────────────────────
    children.push_back(
            addIfNonNull(scaffold->bottomNavigationBar(), ScaffoldSlot::bottomNavigationBar));

    // ── floatingActionButton（官方 :3172-3185）───────────────────────────
    // 官方包在 `_FloatingActionButtonTransition` 里做进出场动画与
    // 位置切换动画；本移植直接放 FAB，位置由 RenderScaffold 计算。
    children.push_back(
            addIfNonNull(scaffold->floatingActionButton(), ScaffoldSlot::floatingActionButton));

    // ── statusBar（官方 :3188-3206）──────────────────────────────────────
    // 官方仅在 iOS / macOS 上给出一个高度为 statusBar 的命中测试区，
    // Android / Linux / Windows 上为 null。本移植同此：不给 statusBar。

    // ── drawer / endDrawer（官方 :3208-3214）─────────────────────────────
    // 官方：
    //   if (_endDrawerOpened.value) {
    //     _buildDrawer(children, textDirection);
    //     _buildEndDrawer(children, textDirection);
    //   } else {
    //     _buildEndDrawer(children, textDirection);
    //     _buildDrawer(children, textDirection);
    //   }
    // 即「未展开 endDrawer 时 endDrawer 先入列」，从而绘制层级上
    // drawer 覆盖 endDrawer。
    if (endDrawerOpened_) {
        children.push_back(addIfNonNull(scaffold->drawer(), ScaffoldSlot::drawer));
        children.push_back(addIfNonNull(scaffold->endDrawer(), ScaffoldSlot::endDrawer));
    } else {
        children.push_back(addIfNonNull(scaffold->endDrawer(), ScaffoldSlot::endDrawer));
        children.push_back(addIfNonNull(scaffold->drawer(), ScaffoldSlot::drawer));
    }

    // ── CustomMultiChildLayout（官方 :3237-3258）────────────────────────
    auto layout = cui::make_shared<ScaffoldLayout>(std::move(children), scaffold->extendBody(),
                                                   scaffold->extendBodyBehindAppBar(),
                                                   scaffold->minInsets(),
                                                   scaffold->minViewPadding(),
                                                   scaffold->floatingActionButtonLocation(),
                                                   drawerOpened_, endDrawerOpened_);

    // ── Material（官方 :3232-3238）─────────────────────────────────────
    // 官方：`Material(color: widget.backgroundColor ?? themeData.scaffoldBackgroundColor,
    //        child: Builder(…))`。本植无 _ScaffoldScope / ScrollNotificationObserver /
    // Actions，故只剩这层材质：整页底色 + 阴影载体。
    return cui::make_shared<Material>(layout, MaterialType::Canvas,
                                      scaffold->backgroundColor());
}

// ── Scaffold ────────────────────────────────────────────────────────────────

ScaffoldState* Scaffold::maybeOf(BuildContext& context) {
    // 上游的 Scaffold.maybeOf（:2087-2089）。
    return ScaffoldState::maybeOf(context);
}

ScaffoldState* Scaffold::of(BuildContext& context) {
    // 上游的 Scaffold.of（:2043-2072）。
    return ScaffoldState::of(context);
}

std::unique_ptr<State> Scaffold::createState() {
    return std::make_unique<ScaffoldState>();
}

}
