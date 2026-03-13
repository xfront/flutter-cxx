/// @file rendering/render_scaffold.cpp
/// @brief RenderScaffold 实现 —— Scaffold 的槽位布局。
///
/// 布局算法逐条对齐 上游官方
/// `上游 material/scaffold.dart` 的
/// `_ScaffoldLayout.performLayout`（:1024-1293）。

#include "cui/rendering/render_scaffold.h"

#include <algorithm>

#include "cui/rendering/painting_context.h"

namespace cui {

namespace {

/// 槽位总数。上游的 `_ScaffoldSlot.values.length`。
constexpr size_t kSlotCount = static_cast<size_t>(ScaffoldSlot::statusBar) + 1;

}  // namespace

// ── FAB 定位（floating_action_button_location.dart） ─────────────────────

Offset scaffoldFabOffset(ScaffoldFabLocation location, const ScaffoldGeometry& geometry) {
    const double fabWidth = geometry.fabSize.width;
    const double fabHeight = geometry.fabSize.height;
    const double contentBottom = geometry.contentBottom;
    const double scaffoldWidth = geometry.scaffoldSize.width;
    const double scaffoldHeight = geometry.scaffoldSize.height;

    // ── 横向（官方 FabEndOffsetX / FabCenterOffsetX / FabStartOffsetX）──
    // 官方按 textDirection 决定 start/end；本移植固定 LTR。
    double x = 0.0;
    switch (location) {
        case ScaffoldFabLocation::endFloat:
        case ScaffoldFabLocation::endDocked:
            x = scaffoldWidth - kFloatingActionButtonMargin - geometry.minInsets.right - fabWidth;
            break;
        case ScaffoldFabLocation::centerFloat:
        case ScaffoldFabLocation::centerDocked:
            x = (scaffoldWidth - fabWidth) / 2.0;
            break;
        case ScaffoldFabLocation::startFloat:
        case ScaffoldFabLocation::startDocked:
            x = kFloatingActionButtonMargin + geometry.minInsets.left;
            break;
    }

    // ── 纵向 ────────────────────────────────────────────────────────────
    const bool docked = location == ScaffoldFabLocation::endDocked ||
                        location == ScaffoldFabLocation::centerDocked ||
                        location == ScaffoldFabLocation::startDocked;

    double fabY = 0.0;
    if (!docked) {
        // 官方 FabFloatOffsetY。
        const double bottomContentHeight = scaffoldHeight - contentBottom;
        const double safeMargin = std::max(
                kFloatingActionButtonMargin,
                geometry.minViewPadding.bottom - bottomContentHeight + kFloatingActionButtonMargin);
        fabY = contentBottom - fabHeight - safeMargin;
    } else {
        // 官方 FabDockedOffsetY。
        const double contentMargin = scaffoldHeight - contentBottom;
        const double bottomViewPadding = geometry.minViewPadding.bottom;
        const double bottomMinInset = geometry.minInsets.bottom;

        double safeMargin = 0.0;
        if (contentMargin > bottomMinInset + fabHeight / 2.0) {
            safeMargin = 0.0;
        } else if (bottomMinInset == 0.0) {
            safeMargin = bottomViewPadding;
        } else {
            safeMargin = fabHeight / 2.0 + kFloatingActionButtonMargin;
        }

        fabY = contentBottom - fabHeight / 2.0 - safeMargin;
        const double maxFabY = scaffoldHeight - fabHeight - safeMargin;
        fabY = std::min(maxFabY, fabY);
    }

    // SnackBar 存在时 FAB 上移（官方同款逻辑）。
    if (geometry.snackBarSize.height > 0.0) {
        fabY = std::min(fabY, contentBottom - geometry.snackBarSize.height - fabHeight -
                                  kFloatingActionButtonMargin);
    }
    // BottomSheet 存在时 FAB 上移（官方同款逻辑）。
    if (geometry.bottomSheetSize.height > 0.0) {
        fabY = std::min(fabY, contentBottom - geometry.bottomSheetSize.height - fabHeight / 2.0);
    }

    return Offset(x, fabY);
}

// ── RenderScaffold ─────────────────────────────────────────────────────────

void RenderScaffold::setupParentData(RenderObject* child) {
    // 上游的 RenderCustomMultiChildLayout.setupParentData：
    // 为子节点创建 MultiChildLayoutParentData。本移植用 ScaffoldParentData。
    if (!child)
        return;
    child->setParentData(new ScaffoldParentData());
}

void RenderScaffold::collectSlots(std::vector<RenderObject*>& bySlot, size_t slotCount) {
    bySlot.assign(slotCount, nullptr);
    for (RenderObject* child : children()) {
        if (!child)
            continue;
        auto* pd = dynamic_cast<ScaffoldParentData*>(child->parentData());
        if (!pd)
            continue;
        const size_t index = static_cast<size_t>(pd->slot);
        if (index < slotCount)
            bySlot[index] = child;
    }
}

void RenderScaffold::performLayout() {
    // ── 上游的 _ScaffoldLayout.performLayout（:1024-1293）────────────
    const Size size = constraints_.biggest();
    setSize(size);

    std::vector<RenderObject*> slot;
    collectSlots(slot, kSlotCount);

    auto hasChild = [&](ScaffoldSlot s) { return slot[static_cast<size_t>(s)] != nullptr; };
    auto layoutChild = [&](ScaffoldSlot s, const BoxConstraints& c) -> Size {
        RenderObject* child = slot[static_cast<size_t>(s)];
        if (!child)
            return {};
        child->layout(c);
        return child->size();
    };
    auto positionChild = [&](ScaffoldSlot s, const Offset& offset) {
        RenderObject* child = slot[static_cast<size_t>(s)];
        if (!child)
            return;
        if (auto* pd = dynamic_cast<ScaffoldParentData*>(child->parentData()))
            pd->offset = offset;
    };

    // 上游: `final BoxConstraints looseConstraints = BoxConstraints.loose(size);`（:1025）
    const BoxConstraints looseConstraints = BoxConstraints::loose(size);
    // 上游: `fullWidthConstraints = looseConstraints.tighten(width: size.width);`（:1032）
    const BoxConstraints fullWidthConstraints = looseConstraints.tighten(size.width, std::nullopt);

    const double bottom = size.height;
    double contentTop = 0.0;
    double bottomWidgetsHeight = 0.0;
    double appBarHeight = 0.0;

    // ── appBar：全宽置顶（:1038-1042）────────────────────────────────────
    if (hasChild(ScaffoldSlot::appBar)) {
        appBarHeight = layoutChild(ScaffoldSlot::appBar, fullWidthConstraints).height;
        contentTop = extendBodyBehindAppBar_ ? 0.0 : appBarHeight;
        positionChild(ScaffoldSlot::appBar, Offset(0, 0));
    }

    // ── bottomNavigationBar：贴底（:1045-1053）──────────────────────────
    double bottomNavigationBarTop = 0.0;
    bool hasBottomNavigationBar = false;
    if (hasChild(ScaffoldSlot::bottomNavigationBar)) {
        const double h = layoutChild(ScaffoldSlot::bottomNavigationBar, fullWidthConstraints).height;
        bottomWidgetsHeight += h;
        bottomNavigationBarTop = std::max(0.0, bottom - bottomWidgetsHeight);
        hasBottomNavigationBar = true;
        positionChild(ScaffoldSlot::bottomNavigationBar, Offset(0, bottomNavigationBarTop));
    }

    // ── persistentFooter：在 bottomNavigationBar 之上（:1055-1069）───────
    if (hasChild(ScaffoldSlot::persistentFooter)) {
        const BoxConstraints footerConstraints(
                0.0, fullWidthConstraints.maxWidth, 0.0,
                std::max(0.0, bottom - bottomWidgetsHeight - contentTop));
        const double h = layoutChild(ScaffoldSlot::persistentFooter, footerConstraints).height;
        bottomWidgetsHeight += h;
        positionChild(ScaffoldSlot::persistentFooter,
                      Offset(0, std::max(0.0, bottom - bottomWidgetsHeight)));
    }

    // ── materialBanner：位于 appBar 之下（:1071-1080）──────────────────
    Size materialBannerSize;
    if (hasChild(ScaffoldSlot::materialBanner)) {
        materialBannerSize = layoutChild(ScaffoldSlot::materialBanner, fullWidthConstraints);
        positionChild(ScaffoldSlot::materialBanner, Offset(0, appBarHeight));
        // 官方仅在 elevation == 0（extendBodyBehindMaterialBanner == false）时下压内容。
        contentTop += materialBannerSize.height;
    }

    // ── contentBottom：键盘（minInsets）优先于底部挂件（:1085-1088）────
    const double contentBottom =
            std::max(0.0, bottom - std::max(minInsets_.bottom, bottomWidgetsHeight));

    // ── body：占据 contentTop .. contentBottom（:1090-1112）──────────────
    if (hasChild(ScaffoldSlot::body)) {
        double bodyMaxHeight = std::max(0.0, contentBottom - contentTop);

        // extendBody 时 body 延伸到 bottom widgets 之下（:1093-1101）。
        if (extendBody_ && minInsets_.bottom <= bottomWidgetsHeight) {
            bodyMaxHeight += bottomWidgetsHeight;
            bodyMaxHeight = std::min(bodyMaxHeight,
                                     std::max(0.0, looseConstraints.maxHeight - contentTop));
        } else {
            bottomWidgetsHeight = 0.0;
        }

        const BoxConstraints bodyConstraints(0.0, fullWidthConstraints.maxWidth, 0.0, bodyMaxHeight);
        layoutChild(ScaffoldSlot::body, bodyConstraints);
        positionChild(ScaffoldSlot::body, Offset(0, contentTop));
    }

    // ── bodyScrim：铺满内容区（:1128-1135）─────────────────────────────
    if (hasChild(ScaffoldSlot::bodyScrim)) {
        const BoxConstraints scrimConstraints(0.0, fullWidthConstraints.maxWidth, 0.0, contentBottom);
        layoutChild(ScaffoldSlot::bodyScrim, scrimConstraints);
        positionChild(ScaffoldSlot::bodyScrim, Offset(0, 0));
    }

    // ── bottomSheet：贴底居中（:1143-1153）─────────────────────────────
    Size bottomSheetSize;
    if (hasChild(ScaffoldSlot::bottomSheet)) {
        const BoxConstraints sheetConstraints(
                0.0, fullWidthConstraints.maxWidth, 0.0,
                std::max(0.0, contentBottom - contentTop));
        bottomSheetSize = layoutChild(ScaffoldSlot::bottomSheet, sheetConstraints);
        positionChild(ScaffoldSlot::bottomSheet,
                      Offset((size.width - bottomSheetSize.width) / 2.0,
                             contentBottom - bottomSheetSize.height));
    }

    // ── snackBar：贴底（fixed 行为，:1191-1245）────────────────────────
    Size snackBarSize;
    if (hasChild(ScaffoldSlot::snackBar)) {
        snackBarSize = layoutChild(ScaffoldSlot::snackBar, fullWidthConstraints);
        positionChild(ScaffoldSlot::snackBar, Offset(0, contentBottom - snackBarSize.height));
    }

    // ── floatingActionButton：按位置公式摆放（:1156-1189）──────────────
    if (hasChild(ScaffoldSlot::floatingActionButton)) {
        const Size fabSize = layoutChild(ScaffoldSlot::floatingActionButton, looseConstraints);

        ScaffoldGeometry geometry;
        geometry.scaffoldSize = size;
        // 官方用 appBarHeight 而非 contentTop，避免 extendBodyBehindAppBar 影响。
        geometry.contentTop = appBarHeight;
        geometry.contentBottom = contentBottom;
        geometry.minInsets = minInsets_;
        geometry.minViewPadding = minViewPadding_;
        geometry.fabSize = fabSize;
        geometry.snackBarSize = snackBarSize;
        geometry.bottomSheetSize = bottomSheetSize;

        positionChild(ScaffoldSlot::floatingActionButton, scaffoldFabOffset(fabLocation_, geometry));
    }

    // ── statusBar：顶部安全区（:1272-1276）─────────────────────────────
    if (hasChild(ScaffoldSlot::statusBar)) {
        layoutChild(ScaffoldSlot::statusBar,
                    fullWidthConstraints.tighten(std::nullopt, minInsets_.top));
        positionChild(ScaffoldSlot::statusBar, Offset(0, 0));
    }

    // ── drawer / endDrawer：覆盖式铺满（:1280-1287）────────────────────
    //
    // 官方用 DrawerController 做滑入动画，本移植无动画基础设施，
    // 以「关闭时平移出屏」表达同一语义。
    if (hasChild(ScaffoldSlot::drawer)) {
        layoutChild(ScaffoldSlot::drawer, BoxConstraints::tight(size));
        positionChild(ScaffoldSlot::drawer,
                      drawerOpen_ ? Offset(0, 0) : Offset(-size.width, 0));
    }
    if (hasChild(ScaffoldSlot::endDrawer)) {
        layoutChild(ScaffoldSlot::endDrawer, BoxConstraints::tight(size));
        positionChild(ScaffoldSlot::endDrawer,
                      endDrawerOpen_ ? Offset(0, 0) : Offset(size.width, 0));
    }

    (void)hasBottomNavigationBar;
    (void)bottomNavigationBarTop;
}

void RenderScaffold::paint(PaintingContext& context, const Offset& offset) {
    // 子节点按 children 顺序绘制，即官方 build 中的入列顺序：
    // body → appBar → ... → drawer/endDrawer。
    // 因此 appBar 的阴影会覆盖在 body 之上（官方 :1027-1030 的注释语义）。
    //
    // 坐标约定：本仓库的 `PaintingContext::paintChild` 会把传入的偏移
    // **平移到画布上**，因此这里传的是「子节点相对本 Scaffold 的偏移」，
    // 本 Scaffold 自身的偏移已由上层绘制调用平移过了。
    (void)offset;
    for (RenderObject* child : children()) {
        if (!child)
            continue;
        Offset childOffset;
        if (auto* pd = child->parentData())
            childOffset = pd->offset;
        context.paintChild(child, childOffset);
    }
}

}  // namespace cui_core
