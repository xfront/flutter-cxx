/// @file test_popup.cpp
/// @brief 弹层组件单元测试（Dialog / AlertDialog / SimpleDialog /
///        showDialog / PopupMenuButton）。
///
///  覆盖：
///   1. DialogRoute：push 后 history+1、barrier 遮罩在场、barrier 点击
///      pop（barrierDismissible）、barrierDismissible=false 时点击不关。
///   2. AlertDialog：标题/内容在场；action 点击回调并关闭。
///   3. SimpleDialog：选项点击触发 onSelected(value)。
///   4. PopupMenuButton：按下打开菜单（上层 Overlay）、条目点击触发
///      onSelected 并关闭、barrier 外点关闭、dispose 安全。
///   5. PopupMenuButton 菜单宽度：默认与按钮同宽、条目文字更长时才变宽、
///      显式 menuWidth 覆盖。
///   6. ContextMenu：右键在按下点弹出（左键不触发）、菜单挂上层 Overlay（不在
///      宿主 Column 子树里），于是既不被后继兄弟盖住绘制，也不被兄弟抢走命中；
///      左右键点菜单外都经 barrier 收起。
///   7. MenuBar：展开某个菜单后，鼠标左右移到相邻标签即自动切换下拉（收起态
///      扫过标签不转发 hover，同项反复进出不重复切换）。

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/controls.h"
#include "cui/widgets/dialog.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/popup_menu.h"
#include "cui/widgets/text.h"

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

bool near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

/// 带 Navigator 的 Harness：Navigator 作为根（自带 Overlay），手动 push 页面。
struct NavHarness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;
    NavigatorState* state = nullptr;
    BuildContext* pageCtx = nullptr;

    void attach() {
        rv.setWindowSize({200, 200});
        auto navigator = cui::make_shared<Navigator>();
        auto e = navigator->createElement(navigator);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        state = static_cast<NavigatorState*>(
            static_cast<StatefulElement*>(root)->state());
        pump();
    }

    /// push 一个记录 pageCtx 的不透明页面（内容即返回的 widget）。
    void pushPage(SharedPtr<Widget> page) {
        state->push(cui::make_shared<ContentRoute>(
            [this, page](BuildContext& ctx) {
                pageCtx = &ctx;
                return page;
            },
            /*opaque=*/true));
        pump();
    }

    void dispatch(const PointerEvent& ev) { po.dispatchPointerEvent(ev); }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    /// DFS 找文本内容包含 sub 的 RenderParagraph。
    RenderParagraph* findParagraphContaining(const std::string& sub) {
        struct Visitor {
            const std::string* sub;
            RenderParagraph* found = nullptr;
            void visit(RenderObject* ro) {
                if (!ro || found)
                    return;
                if (auto* p = dynamic_cast<RenderParagraph*>(ro)) {
                    if (p->text() && p->text()->toPlainText().find(*sub) !=
                                         std::string::npos)
                        found = p;
                }
                ro->visitChildren([this](AbstractNode* n) {
                    visit(static_cast<RenderObject*>(n));
                });
            }
        } v{&sub};
        v.visit(root->findRenderObject());
        return v.found;
    }

    /// 派发一次点击（down + up）到全局坐标。
    void tap(double x, double y) {
        PointerDownEvent d;
        d.position = {x, y};
        d.buttons = kPrimaryMouseButton;
        dispatch(d);
        PointerUpEvent u;
        u.position = {x, y};
        dispatch(u);
    }

    /// 派发一次右键点击（secondary down + up）到全局坐标。
    void rightTap(double x, double y) {
        PointerDownEvent d;
        d.position = {x, y};
        d.buttons = kSecondaryMouseButton;
        dispatch(d);
        PointerUpEvent u;
        u.position = {x, y};
        u.buttons = 0;  // 抬手后按键位已清空
        dispatch(u);
    }
};

// ── 测试 1：DialogRoute + showDialog ───────────────────────────────────────
void testDialogRoute() {
    std::printf("[test] DialogRoute / showDialog\n");

    NavHarness h;
    h.attach();
    h.pushPage(cui::make_shared<SizedBox>(200.0, 200.0));

    int popped = 0;
    auto dialog =
        cui::make_shared<AlertDialog>("确认", cui::make_shared<Text>("确定要删除吗？"));
    auto route = showDialog(*h.pageCtx, dialog);
    route->whenPopped([&](SharedPtr<void>) { ++popped; });
    h.pump();

    check(h.state->history().size() == 2, "showDialog 后 history+1");
    check(h.state->currentRoute() == route.get(), "对话框路由在栈顶");
    check(h.findParagraphContaining("确认") != nullptr, "对话框标题在场");
    check(h.findParagraphContaining("确定要删除吗") != nullptr, "对话框内容在场");

    // barrier 点击（左上角，远离居中的卡片）→ 关闭。
    h.tap(10.0, 10.0);
    h.pump();
    check(h.state->history().size() == 1, "barrier 点击后对话框关闭");
    check(popped == 1, "Route::whenPopped 收到弹出通知");

    // barrierDismissible=false：barrier 点击不关闭。
    auto route2 = showDialog(*h.pageCtx, cui::make_shared<AlertDialog>("阻塞"),
                             /*barrierDismissible=*/false);
    h.pump();
    check(h.state->history().size() == 2, "第二个对话框压栈");
    h.tap(10.0, 10.0);
    h.pump();
    check(h.state->history().size() == 2, "barrierDismissible=false 点击不关闭");
    h.state->pop();
    h.pump();
    check(h.state->history().size() == 1, "手动 pop 恢复");
}

// ── 测试 2：AlertDialog actions ────────────────────────────────────────────
void testAlertDialogActions() {
    std::printf("[test] AlertDialog actions\n");

    NavHarness h;
    h.attach();
    h.pushPage(cui::make_shared<SizedBox>(200.0, 200.0));

    int confirmed = 0;
    auto okBtn = cui::make_shared<GestureDetector>(
        cui::make_shared<Container>(cui::make_shared<Text>("确定")),
        [&]() { ++confirmed; });
    auto dialog = cui::make_shared<AlertDialog>(
        "删除", nullptr, std::vector<SharedPtr<Widget>>{okBtn});
    showDialog(*h.pageCtx, dialog);
    h.pump();

    // 点击 action 按钮（对话框卡片右下区域）。
    auto* p = h.findParagraphContaining("确定");
    check(p != nullptr, "action 按钮文本在场");
    if (p) {
        Offset c = p->localToGlobal(
            Offset(p->size().width / 2.0, p->size().height / 2.0));
        h.tap(c.dx, c.dy);
        h.pump();
        check(confirmed == 1, "action 点击触发回调");
    }

    // action 本身不自动关窗（由回调决定），这里手动 pop 验证结构。
    h.state->pop();
    h.pump();
    check(h.state->history().size() == 1, "手动 pop 关闭");
}

// ── 测试 3：SimpleDialog ───────────────────────────────────────────────────
void testSimpleDialog() {
    std::printf("[test] SimpleDialog\n");

    NavHarness h;
    h.attach();
    h.pushPage(cui::make_shared<SizedBox>(200.0, 200.0));

    int selected = -1;
    auto dialog = cui::make_shared<SimpleDialog>(
        "选择",
        std::vector<SimpleDialogOption>{{"选项A", 1}, {"选项B", 2}},
        [&](int v) { selected = v; });
    showDialog(*h.pageCtx, dialog);
    h.pump();

    check(h.findParagraphContaining("选项A") != nullptr, "选项 A 在场");

    // 点击选项 B（按渲染盒全局中心派发）。
    auto* p = h.findParagraphContaining("选项B");
    check(p != nullptr, "选项 B 在场");
    if (p) {
        Offset c = p->localToGlobal(
            Offset(p->size().width / 2.0, p->size().height / 2.0));
        h.tap(c.dx, c.dy);
        h.pump();
        check(selected == 2, "选项点击触发 onSelected(2)");
    }
}

// ── 测试 4：PopupMenuButton ────────────────────────────────────────────────
void testPopupMenuButton() {
    std::printf("[test] PopupMenuButton\n");

    NavHarness h;
    h.attach();

    int selected = -1;
    auto page = cui::make_shared<Center>(cui::make_shared<PopupMenuButton>(
        std::vector<PopupMenuItem>{{"打开", 1}, {"保存", 2}, {"删除", 3}},
        [&](int v) { selected = v; }, cui::make_shared<Text>("菜单")));
    h.pushPage(page);

    check(h.findParagraphContaining("打开") == nullptr, "初始菜单未打开");

    // 按下按钮（窗口中心）→ onTapDown 打开菜单，菜单左上角 = 按下点。
    PointerDownEvent d;
    d.position = {100, 100};
    d.buttons = kPrimaryMouseButton;
    h.dispatch(d);
    PointerUpEvent u;
    u.position = {100, 100};
    h.dispatch(u);
    h.pump();
    check(h.findParagraphContaining("打开") != nullptr, "按下后菜单打开");
    check(h.findParagraphContaining("删除") != nullptr, "菜单条目齐全");

    // 点击条目「保存」（渲染盒全局中心）→ onSelected(2) 且菜单关闭。
    auto* p = h.findParagraphContaining("保存");
    check(p != nullptr, "「保存」条目在场");
    if (p) {
        Offset c = p->localToGlobal(
            Offset(p->size().width / 2.0, p->size().height / 2.0));
        h.tap(c.dx, c.dy);
        h.pump();
        check(selected == 2, "条目点击触发 onSelected(2)");
        check(h.findParagraphContaining("打开") == nullptr, "选中后菜单关闭");
    }

    // 再次打开 → 点击 barrier（左上角）→ 关闭且不触发 onSelected。
    h.dispatch(d);
    h.dispatch(u);
    h.pump();
    check(h.findParagraphContaining("打开") != nullptr, "菜单再次打开");
    h.tap(5.0, 5.0);
    h.pump();
    check(h.findParagraphContaining("打开") == nullptr, "barrier 外点关闭");
    check(selected == 2, "外点关闭不触发 onSelected");
}

// ── 测试 5：PopupMenuButton 的菜单宽度 ─────────────────────────────────────
/// 条目行盒子：Container(padding + decoration) ⇒ RenderDecoratedBox，
/// 文本的 RenderPadding 是它唯一的子 ⇒ 从段落往上走两级。
RenderBox* itemRowBox(RenderParagraph* para) {
    if (!para || !para->parent() || !para->parent()->parent())
        return nullptr;
    return dynamic_cast<RenderBox*>(para->parent()->parent());
}

void openAndMeasureMenu(NavHarness& h, double tapX, double tapY, const char* itemLabel,
                        double& outRowWidth, double& outColumnWidth) {
    outRowWidth = outColumnWidth = -1.0;
    h.tap(tapX, tapY);
    h.pump();
    auto* para = h.findParagraphContaining(itemLabel);
    RenderBox* row = itemRowBox(para);
    if (!row)
        return;
    outRowWidth = row->size().width;
    if (auto* col = dynamic_cast<RenderBox*>(row->parent()))
        outColumnWidth = col->size().width;
}

void testPopupMenuMenuWidth() {
    std::printf("[test] PopupMenuButton 菜单宽度\n");

    const std::string longLabel = "a menu item label much wider than the button";
    const double expectLong =
        Painter::measureTextWidth(nullptr, longLabel, PopupMenuList::itemStyle().fontSize) +
        2.0 * PopupMenuList::kItemPaddingH;

    // ① 按钮比条目宽 ⇒ 菜单与按钮同宽（旧实现恒为 160，窄按钮被撑成大空卡）。
    NavHarness h1;
    h1.attach();
    h1.pushPage(cui::make_shared<Center>(cui::make_shared<PopupMenuButton>(
        std::vector<PopupMenuItem>{{"打开", 1}, {"保存", 2}}, [](int) {},
        cui::make_shared<SizedBox>(150.0, 32.0, cui::make_shared<Text>("菜单")))));
    double rowW = 0.0, colW = 0.0;
    openAndMeasureMenu(h1, 100, 100, "打开", rowW, colW);
    std::printf("      按钮宽 150，菜单宽 %.2f，条目列宽 %.2f\n", rowW, colW);
    check(rowW > 0.0, "① 菜单已打开并找到条目行");
    check(near(rowW, 150.0, 0.5), "① 菜单宽度 == 按钮宽度");
    check(near(colW, 150.0, 0.5), "① 条目列与卡片同宽（行被拉满，高亮不留白）");

    // ② 条目文字（含行内边距）比按钮宽 ⇒ 菜单变宽，且刚好包住文字。
    NavHarness h2;
    h2.attach();
    h2.pushPage(cui::make_shared<Center>(cui::make_shared<PopupMenuButton>(
        std::vector<PopupMenuItem>{{"打开", 1}, {longLabel, 2}}, [](int) {},
        cui::make_shared<SizedBox>(60.0, 32.0, cui::make_shared<Text>("菜单")))));
    openAndMeasureMenu(h2, 100, 100, "a menu item", rowW, colW);
    std::printf("      按钮宽 60，菜单宽 %.2f（期望 %.2f）\n", rowW, expectLong);
    check(rowW > 60.0, "② 长条目把菜单撑得比按钮宽");
    check(near(rowW, expectLong, 1.0), "② 菜单宽度 = 最长条目文字 + 行左右内边距");

    // ③ 显式传 menuWidth ⇒ 固定用它（调用方说了算）。
    NavHarness h3;
    h3.attach();
    h3.pushPage(cui::make_shared<Center>(cui::make_shared<PopupMenuButton>(
        std::vector<PopupMenuItem>{{"打开", 1}}, [](int) {},
        cui::make_shared<SizedBox>(60.0, 32.0, cui::make_shared<Text>("菜单")), 120.0)));
    openAndMeasureMenu(h3, 100, 100, "打开", rowW, colW);
    std::printf("      显式 120，菜单宽 %.2f\n", rowW);
    check(near(rowW, 120.0, 0.5), "③ 显式 menuWidth 覆盖自动宽度");
}

// ── 测试 6：ContextMenu 的浮层归属 ─────────────────────────────────────────
RenderBox* findBoxBySize(RenderObject* ro, double w, double h) {
    if (!ro)
        return nullptr;
    if (auto* b = dynamic_cast<RenderBox*>(ro)) {
        if (near(b->size().width, w, 0.01) && near(b->size().height, h, 0.01))
            return b;
    }
    RenderBox* found = nullptr;
    ro->visitChildren([&](AbstractNode* n) {
        if (found)
            return;
        found = findBoxBySize(static_cast<RenderObject*>(n), w, h);
    });
    return found;
}

RenderObject* firstFlexAncestor(RenderObject* n) {
    for (AbstractNode* a = n; a; a = a->parent()) {
        if (auto* f = dynamic_cast<RenderFlex*>(a))
            return f;
    }
    return nullptr;
}

bool hasAncestor(RenderObject* n, const RenderObject* target) {
    for (AbstractNode* a = n; a; a = a->parent()) {
        if (a == target)
            return true;
    }
    return false;
}

void testContextMenuOverlay() {
    std::printf("[test] ContextMenu 浮层归属\n");

    NavHarness h;
    h.attach();

    int selected = -1;
    int siblingTaps = 0;

    // 触发区 200×44，正下方一个 200×60 的兄弟盒（带自己的点击回调）。菜单面板
    // 140 宽、从触发区左上角 +4/+26 起，纵向必然压进兄弟盒 ⇒ 旧实现（画在页面
    // 那层 Stack 里）会被兄弟盖住绘制并抢走命中。
    auto trigger = cui::make_shared<Container>();
    trigger->width(200.0).height(44.0)
        .decoration(BoxDecoration{Color::fromRGB(0.92, 0.92, 0.94)});
    auto ctx = cui::make_shared<ContextMenu>(
        trigger, std::vector<std::string>{"复制", "粘贴"},
        [&](int v) { selected = v; });

    auto sibBox = cui::make_shared<Container>();
    sibBox->width(200.0).height(60.0)
        .decoration(BoxDecoration{Color::fromRGB(0.96, 0.90, 0.90)});
    auto sib = cui::make_shared<GestureDetector>(sibBox, [&siblingTaps] { ++siblingTaps; });

    std::vector<SharedPtr<Widget>> kids;
    kids.push_back(ctx);
    kids.push_back(sib);
    auto col = cui::make_shared<Column>(std::move(kids));
    col->setMainAxisSize(MainAxisSize::Min);
    h.pushPage(cui::make_shared<Align>(-1.0, -1.0, std::move(col)));

    check(h.findParagraphContaining("复制") == nullptr, "初始菜单未打开");

    // 左键不再是触发方式（旧的「点击代替右键」已废）。
    h.tap(100.0, 22.0);
    h.pump();
    check(h.findParagraphContaining("复制") == nullptr, "左键不打开菜单");

    h.rightTap(40.0, 22.0);
    h.pump();
    check(h.findParagraphContaining("复制") != nullptr, "右键触发区后菜单打开");
    check(h.findParagraphContaining("粘贴") != nullptr, "菜单条目齐全");

    auto* item = h.findParagraphContaining("粘贴");
    RenderBox* trig = findBoxBySize(h.root->findRenderObject(), 200.0, 44.0);
    auto* page = trig ? dynamic_cast<RenderFlex*>(firstFlexAncestor(trig)) : nullptr;
    check(trig != nullptr && page != nullptr, "找到触发区盒子与页面 Column");
    check(item && page && !hasAncestor(item, page),
          "菜单不在页面 Column 子树里（挂上层 Overlay）");

    // 面板左上角 = 右键按下点（旧实现是触发区 +4/+26）。
    Offset o = item ? item->localToGlobal(Offset{}) : Offset{};
    std::printf("      「粘贴」左上 (%.1f, %.1f)\n", o.dx, o.dy);
    check(o.dx > 45.0, "面板从右键按下点起算（不再固定 +4）");

    Offset c = item ? item->localToGlobal(
                          Offset(item->size().width / 2.0, item->size().height / 2.0))
                    : Offset{};
    std::printf("      「粘贴」中心 (%.1f, %.1f)\n", c.dx, c.dy);
    check(c.dy > 44.0 && c.dy < 104.0, "用例前提：菜单项确实压在兄弟盒上");
    if (item) {
        h.tap(c.dx, c.dy);
        h.pump();
        check(selected == 1, "菜单项点击触发 onSelect(1)");
        check(siblingTaps == 0, "下方兄弟没抢到点击");
        check(h.findParagraphContaining("复制") == nullptr, "选中后菜单关闭");
    }

    // 再打开 → 右键菜单外（兄弟盒下方空白）⇒ barrier 接住：菜单收起、两边都不回调。
    h.rightTap(10.0, 22.0);
    h.pump();
    check(h.findParagraphContaining("复制") != nullptr, "菜单再次打开");
    h.rightTap(150.0, 150.0);
    h.pump();
    check(h.findParagraphContaining("复制") == nullptr, "右键 barrier 外点关闭");
    check(selected == 1, "外点不触发 onSelect");
    check(siblingTaps == 0, "外点被 barrier 接住，兄弟仍没被点");

    // 左键点菜单外同样收起。
    h.rightTap(10.0, 22.0);
    h.pump();
    check(h.findParagraphContaining("复制") != nullptr, "菜单第三次打开");
    h.tap(150.0, 150.0);
    h.pump();
    check(h.findParagraphContaining("复制") == nullptr, "左键 barrier 外点也关闭");
    check(selected == 1 && siblingTaps == 0, "左键外点同样落到 barrier");
}

// ── 测试 7：MenuBar 展开后 hover 切换下拉 ─────────────────────────────────
/// 复刻壳层用法：MenuBar 只画菜单条，展开状态（openIndex）与下拉面板都由上层维护。
class MenuBarProbe : public StatefulWidget {
public:
    explicit MenuBarProbe(std::vector<MenuEntry> menus, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), menus_(std::move(menus)) {}
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "MenuBarProbe"; }
    const std::vector<MenuEntry>& menus() const { return menus_; }

private:
    std::vector<MenuEntry> menus_;
};

class MenuBarProbeState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        auto* self = this;
        auto bar = cui::make_shared<MenuBar>(probe()->menus());
        bar->setOpenIndex(open_);
        bar->setOnLabelTap([self](int i) {
            self->open_ = (self->open_ == i) ? -1 : i;
            ++self->taps_;
            self->setState();
        });
        bar->setOnLabelHover([self](int i) {
            if (self->open_ == i)
                return;
            // 不该发生：MenuBar 只在展开态转发 hover
            if (self->open_ < 0)
                ++self->hoversWhileClosed_;
            self->open_ = i;
            ++self->hovers_;
            self->setState();
        });
        return bar;
    }
    int open() const { return open_; }
    int taps() const { return taps_; }
    int hovers() const { return hovers_; }
    int hoversWhileClosed() const { return hoversWhileClosed_; }

private:
    MenuBarProbe* probe() { return static_cast<MenuBarProbe*>(widget()); }
    int open_ = -1;
    int taps_ = 0;
    int hovers_ = 0;
    int hoversWhileClosed_ = 0;
};

inline std::unique_ptr<State> MenuBarProbe::createState() {
    return std::make_unique<MenuBarProbeState>();
}

int countMouseRegions(RenderObject* ro) {
    if (!ro)
        return 0;
    int n = dynamic_cast<RenderMouseRegion*>(ro) ? 1 : 0;
    ro->visitChildren([&](AbstractNode* c) {
        n += countMouseRegions(static_cast<RenderObject*>(c));
    });
    return n;
}

void testMenuBarHoverSwitch() {
    std::printf("[test] MenuBar 展开后 hover 切换下拉\n");

    NavHarness h;
    h.attach();

    auto gk = cui::make_shared<GlobalKey>("menubar.probe");
    h.pushPage(cui::make_shared<Align>(
        -1.0, -1.0,
        cui::make_shared<MenuBarProbe>(
            std::vector<MenuEntry>{
                {"文件", {"新建", "打开", "保存"}},
                {"编辑", {"撤销", "重做", "剪切"}},
                {"视图", {"放大", "缩小", "重置"}}},
            gk)));

    auto* se = static_cast<StatefulElement*>(gk->currentElement<Element>());
    auto* st = se ? dynamic_cast<MenuBarProbeState*>(se->state()) : nullptr;
    check(st != nullptr, "拿到探针 State");
    if (!st)
        return;
    check(countMouseRegions(h.root->findRenderObject()) == 3, "三个标签各包一层悬停区域");

    // 标签中心（全局坐标）：文本盒就在 MouseRegion 正中，点它必落到该标签。
    auto center = [&](const std::string& label, double& x, double& y) {
        RenderParagraph* p = h.findParagraphContaining(label);
        if (!p)
            return false;
        Offset c = p->localToGlobal(Offset(p->size().width / 2.0, p->size().height / 2.0));
        x = c.dx;
        y = c.dy;
        return true;
    };
    double fx = 0, fy = 0, ex = 0, ey = 0, vx = 0, vy = 0;
    check(center("文件", fx, fy) && center("编辑", ex, ey) && center("视图", vx, vy),
          "三个标签都定位到了");
    std::printf("      标签中心 文件(%.0f,%.0f) 编辑(%.0f,%.0f) 视图(%.0f,%.0f)\n", fx, fy, ex,
                ey, vx, vy);
    check(fx < ex && ex < vx, "用例前提：三个标签自左向右排列");

    auto hover = [&](double x, double y) {
        PointerHoverEvent e;
        e.position = {x, y};
        h.dispatch(e);
        h.pump();
    };
    // 高亮跟随展开项（MenuBar 给展开标签换 theme().primary 字色）。
    auto highlighted = [&](const std::string& label) {
        RenderParagraph* p = h.findParagraphContaining(label);
        return p && p->text() && p->text()->style.color == theme().primary;
    };

    // ① 收起态：扫过标签不弹菜单。
    hover(ex, ey);
    check(st->open() == -1 && st->hovers() == 0, "收起态 hover 不切换");

    // ② 点「文件」展开。
    h.tap(fx, fy);
    h.pump();
    check(st->open() == 0 && highlighted("文件"), "点标签展开「文件」并高亮");

    // ③ 右移一个标签 ⇒ 自动切到「编辑」。
    hover(ex, ey);
    check(st->open() == 1 && st->hovers() == 1 && st->hoversWhileClosed() == 0,
          "右移到相邻标签自动切换下拉");
    check(highlighted("编辑") && !highlighted("文件"), "高亮跟着换到「编辑」");

    // ④ 继续右移 ⇒ 「视图」。
    hover(vx, vy);
    check(st->open() == 2 && st->hovers() == 2, "再右移切到「视图」");

    // ⑤ 左移回去同样切换（用户说的「左右移动」两个方向都要）。
    hover(ex, ey);
    check(st->open() == 1 && st->hovers() == 3, "左移切回「编辑」");

    // ⑥ 同一个标签上反复进出不再重建。
    hover(190.0, 190.0);  // 移到菜单条之外（条带只占左上角约 159×30）
    hover(ex, ey);
    check(st->open() == 1 && st->hovers() == 3, "同项反复 hover 不再切换");

    // ⑦ 点当前标签收起，之后 hover 恢复成「什么都不做」。
    h.tap(ex, ey);
    h.pump();
    check(st->open() == -1, "点同一标签收起");
    int hoversBefore = st->hovers();
    hover(vx, vy);
    check(st->open() == -1 && st->hovers() == hoversBefore, "收起后 hover 不再切换");
    check(st->hoversWhileClosed() == 0, "收起态从未收到过 hover 转发");

    // ⑧ 重新展开后仍能切（展开状态是每帧从上层推给 MenuBar 的）。
    h.tap(vx, vy);
    h.pump();
    check(st->open() == 2, "点「视图」重新展开");
    hover(fx, fy);
    check(st->open() == 0, "跨项左移两个标签也切换");
}

}  // namespace

int main() {
    testDialogRoute();
    testAlertDialogActions();
    testSimpleDialog();
    testPopupMenuButton();
    testPopupMenuMenuWidth();
    testContextMenuOverlay();
    testMenuBarHoverSwitch();

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILURES: %d\n", g_failures);
    return 1;
}
