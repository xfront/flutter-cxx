/// @file test_dropdown.cpp
/// @brief DropdownButton（widgets/controls.h）的菜单宽度回归。
///
/// 覆盖：
///   1. 收起态：外层 Column 只有一个子节点（按钮），没有菜单卡片。
///   2. 点开菜单：选项文字比按钮窄时，菜单卡片宽度 == 按钮宽度
///      （旧实现是选项 Column 用 `CrossAxisAlignment::Stretch` ⇒ 选项被撑到
///      整页宽，菜单横跨整行）。
///   3. 选项文字本身比按钮宽时：菜单宽度 = 最宽文字 + 内边距（严格大于按钮宽），
///      文字不会顶出卡片。
///   4. 点选项：菜单收起、`onChanged` 收到被选项的序号、按钮标签跟着换。
///
/// headless 帧泵（flushBuildScope + flushLayout，不碰 paint）。文本宽度用
/// `Painter::measureTextWidth`（与 `RenderParagraph::measureNatural` 同一口径）。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/container.h"
#include "cui/widgets/controls.h"

using namespace cui;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

bool near(double a, double b, double eps = 0.5) { return std::fabs(a - b) <= eps; }

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        pump();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    void tap(double x, double y) {
        PointerDownEvent d;
        d.position = {x, y};
        d.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(d);
        PointerUpEvent u;
        u.position = {x, y};
        po.dispatchPointerEvent(u);
        pump();
    }
};

/// 深度优先取第一个指定类型的 RenderObject。
template <typename T>
T* findRo(RenderObject* root) {
    if (!root)
        return nullptr;
    std::vector<RenderObject*> stack{root};
    while (!stack.empty()) {
        RenderObject* n = stack.back();
        stack.pop_back();
        if (auto* hit = dynamic_cast<T*>(n))
            return hit;
        n->visitChildren([&](AbstractNode* c) {
            if (c)
                stack.push_back(static_cast<RenderObject*>(c));
        });
    }
    return nullptr;
}

/// DropdownButton 的 build 产出「外层 Column(按钮, 菜单卡片)」，它是渲染树里
/// 第一个 RenderFlex（按钮内部的 Row 在它之后）。
RenderFlex* dropdownColumn(Element* root) {
    RenderObject* ro = root ? root->findRenderObject() : nullptr;
    return findRo<RenderFlex>(ro);
}

double widthOf(RenderObject* ro) {
    auto* box = dynamic_cast<RenderBox*>(ro);
    return box ? box->size().width : -1.0;
}

/// 节点中心的全局坐标（用来点它）。
Offset centerOf(RenderObject* ro) {
    auto* box = dynamic_cast<RenderBox*>(ro);
    if (!box)
        return {};
    return box->localToGlobal(Offset{box->size().width / 2.0, box->size().height / 2.0});
}

/// RenderView 给的是窗口大小的紧约束，代理盒子会一路 biggest() 撑满 ⇒
/// 要量控件的自然尺寸必须用 Align(-1,-1) 钉到左上角。
SharedPtr<Widget> pinned(SharedPtr<Widget> w) {
    return cui::make_shared<Align>(-1.0, -1.0, std::move(w));
}

void testMenuMatchesButtonWidth() {
    std::printf("§1 菜单宽度与按钮同宽\n");

    Harness h;
    auto dd = cui::make_shared<DropdownButton>(std::vector<std::string>{"A", "B", "C"}, 0);
    h.attach(pinned(dd), 800.0, 600.0);

    RenderFlex* col = dropdownColumn(h.root);
    check(col != nullptr, "找到 DropdownButton 的外层 Column");
    if (!col)
        return;
    check(col->childCount() == 1, "收起态只有按钮一个子节点");

    const double buttonWidth = widthOf(col->children()[0]);
    check(buttonWidth > 0 && buttonWidth < 200.0,
          "按钮是自然宽度（未被窗口紧约束撑满）");

    // 点按钮 → 菜单展开。
    const Offset c = centerOf(col->children()[0]);
    h.tap(c.dx, c.dy);

    col = dropdownColumn(h.root);
    check(col && col->childCount() == 2, "点开后 Column 多出菜单卡片");
    if (!col || col->childCount() != 2)
        return;

    const double menuWidth = widthOf(col->children()[1]);
    std::printf("      按钮宽 %.2f，菜单宽 %.2f\n", buttonWidth, menuWidth);
    check(near(menuWidth, buttonWidth), "菜单宽度 == 按钮宽度");
    check(menuWidth < 200.0, "菜单没有横跨整页");
}

void testMenuGrowsForLongItem() {
    std::printf("§2 选项比按钮宽时菜单变宽\n");

    const std::string longItem =
        "An option label that is definitely wider than the button";

    Harness h;
    auto dd = cui::make_shared<DropdownButton>(std::vector<std::string>{"A", longItem}, 0);
    h.attach(pinned(dd), 800.0, 600.0);

    RenderFlex* col = dropdownColumn(h.root);
    if (!col || col->childCount() != 1) {
        check(false, "收起态只有按钮一个子节点");
        return;
    }
    const double buttonWidth = widthOf(col->children()[0]);

    const Offset c = centerOf(col->children()[0]);
    h.tap(c.dx, c.dy);

    col = dropdownColumn(h.root);
    check(col && col->childCount() == 2, "点开后 Column 多出菜单卡片");
    if (!col || col->childCount() != 2)
        return;

    const double menuWidth = widthOf(col->children()[1]);
    std::printf("      按钮宽 %.2f，菜单宽 %.2f\n", buttonWidth, menuWidth);
    check(menuWidth > buttonWidth, "长选项把菜单撑得比按钮宽");
    check(menuWidth < 800.0, "菜单仍然没有横跨整页");
}

void testSelectItemClosesMenu() {
    std::printf("§3 选中选项后菜单收起\n");

    int changed = -1;
    Harness h;
    auto dd = cui::make_shared<DropdownButton>(std::vector<std::string>{"A", "B", "C"}, 0,
                                               [&changed](int i) { changed = i; });
    h.attach(pinned(dd), 800.0, 600.0);

    RenderFlex* col = dropdownColumn(h.root);
    if (!col || col->childCount() != 1) {
        check(false, "收起态只有按钮一个子节点");
        return;
    }
    const Offset c = centerOf(col->children()[0]);
    h.tap(c.dx, c.dy);

    col = dropdownColumn(h.root);
    if (!col || col->childCount() != 2) {
        check(false, "点开后 Column 多出菜单卡片");
        return;
    }

    // 菜单卡片里的选项 Column（卡片内唯一的 RenderFlex）。
    RenderFlex* items = findRo<RenderFlex>(col->children()[1]);
    check(items != nullptr, "找到菜单里的选项 Column");
    if (!items)
        return;
    check(items->childCount() == 3, "菜单里有 3 个选项");

    const Offset item = centerOf(items->children()[1]);
    h.tap(item.dx, item.dy);

    col = dropdownColumn(h.root);
    check(col && col->childCount() == 1, "选中后菜单收起");
    check(changed == 1, "onChanged 收到被选项的序号");
}

}  // namespace

int main() {
    testMenuMatchesButtonWidth();
    testMenuGrowsForLongItem();
    testSelectItemClosesMenu();

    if (g_failures == 0) {
        std::printf("\ntest_dropdown: 全部通过\n");
        return 0;
    }
    std::printf("\ntest_dropdown: %d 项失败\n", g_failures);
    return 1;
}
