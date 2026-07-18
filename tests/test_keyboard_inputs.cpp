/// @file test_keyboard_inputs.cpp
/// @brief SearchBar / SearchableList / OtpInput 的真实键盘输入路由测试。
///
/// 三个控件以前都靠「点按钮模拟输入」，现在全部走真实的焦点 + 键事件路由：
///   1. SearchBar —— 点输入框上焦、逐字符键入、写回 widget.value()、清除按钮、回车。
///   2. SearchableList —— 键入即过滤（大小写不敏感子串），结果行随文本增删。
///   3. OtpInput —— 每格一个 FocusNode；数字写入并自动跳格，Backspace 退格，
///      左右移焦，非数字键与 Up/Down 一律放行给祖先（页面照常滚）。
///   4. Scrollable 包在外层时，它 onDown 的「点击即接焦点」不得抢走内层刚拿到
///      焦点的控件（story「输入+」页实测的失效场景），但点空白处仍要吃下焦点。
///   5. SpinBox —— 值区是真的输入框：键入实时改值并夹到 [min, max]、非法字符整串
///      退回、回车与失焦把文本规范化、▲▼ 回写文本、卸载摘干净控制器监听。
///   6. CommandPalette —— 查询框键入即按标题子串过滤（大小写不敏感），↑↓ 移动高亮
///      （到顶停住、无匹配时放行），回车执行高亮项（传出原始下标），Esc 清空；
///      点行与点「全部」筛选条的鼠标路径仍在，卸载后节点键处理器与监听都摘干净。
///
/// 全部 headless：帧泵只做 flushBuildScope → flushLayout（不碰 paint）。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/focus.h"
#include "cui/framework/framework.h"
#include "cui/platform/event.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_editable.h"
#include "cui/rendering/render_icon.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/controls.h"
#include "cui/widgets/data.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/scroll.h"
#include "cui/widgets/text.h"
#include "cui/widgets/text_input.h"

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

/// 挂载 Widget 树到 BuildOwner + RenderView + PipelineOwner（headless）。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    /// 根外包 pinned Align：RenderView 给的是窗口大小的紧约束，代理盒子会一路
    /// biggest() 撑满，区域尺寸既测不出来也点不准。
    void attach(SharedPtr<Widget> body, double ww = 400, double hh = 300) {
        rv.setWindowSize({ww, hh});
        auto pinned = cui::make_shared<Align>(-1.0, -1.0, std::move(body));
        auto e = pinned->createElement(pinned);
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

    FocusNode* primary() { return owner.focusManager().primaryFocus(); }

    /// 一个按键。可打印字符走 `key == 0 + text`（与 宿主桥 同一口径）。
    /// 每次按键后补一帧（宿主收完键就 scheduleRender），断言才看得到重建结果。
    bool key(KeyCode code, const std::string& text = std::string(), int mods = 0) {
        KeyEvent ev{static_cast<int>(code), text, mods, false};
        bool handled = owner.focusManager().sendKeyEvent(ev);
        pump();
        return handled;
    }
    bool type(const std::string& s) {
        bool all = true;
        for (char c : s)
            all = key(KeyCode::None, std::string(1, c)) && all;
        return all;
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

    /// 点击某段文本所在行的中心（用来点中列表里的某一行）。
    void tapText(const std::string& sub) {
        RenderParagraph* p = findParagraphContaining(sub);
        if (!p)
            return;
        Offset c = p->localToGlobal(Offset(p->size().width / 2.0, p->size().height / 2.0));
        tap(c.dx, c.dy);
    }

    RenderParagraph* findParagraphContaining(const std::string& sub) {
        RenderParagraph* found = nullptr;
        std::function<void(RenderObject*)> visit = [&](RenderObject* ro) {
            if (!ro || found)
                return;
            if (auto* p = dynamic_cast<RenderParagraph*>(ro)) {
                if (p->text() && p->text()->toPlainText().find(sub) != std::string::npos)
                    found = p;
            }
            ro->visitChildren([&](AbstractNode* n) {
                visit(static_cast<RenderObject*>(n));
            });
        };
        visit(root->findRenderObject());
        return found;
    }

    /// 输入框当前的文本。`RenderEditable` 不是 `RenderParagraph`，所以不能用
    /// findParagraphContaining 找键入的正文。
    std::string editableText() {
        RenderEditable* found = nullptr;
        std::function<void(RenderObject*)> visit = [&](RenderObject* ro) {
            if (!ro || found)
                return;
            if (auto* e = dynamic_cast<RenderEditable*>(ro))
                found = e;
            ro->visitChildren([&](AbstractNode* n) {
                visit(static_cast<RenderObject*>(n));
            });
        };
        visit(root->findRenderObject());
        return found ? found->text() : std::string("<no editable>");
    }

    /// 输入框（`RenderEditable`）的全局中心 —— 点查询/输入区用它，别猜布局偏移。
    Offset editableCenter() {
        RenderEditable* found = nullptr;
        std::function<void(RenderObject*)> visit = [&](RenderObject* ro) {
            if (!ro || found)
                return;
            if (auto* e = dynamic_cast<RenderEditable*>(ro))
                found = e;
            ro->visitChildren([&](AbstractNode* n) {
                visit(static_cast<RenderObject*>(n));
            });
        };
        visit(root->findRenderObject());
        if (!found)
            return Offset(-1, -1);
        return found->localToGlobal(Offset(found->size().width / 2.0,
                                           found->size().height / 2.0));
    }

    int countIcon(IconCode code) {        int n = 0;
        std::function<void(RenderObject*)> visit = [&](RenderObject* ro) {
            if (!ro)
                return;
            if (auto* i = dynamic_cast<RenderIcon*>(ro))
                if (i->code() == code)
                    ++n;
            ro->visitChildren([&](AbstractNode* c) {
                visit(static_cast<RenderObject*>(c));
            });
        };
        visit(root->findRenderObject());
        return n;
    }

    /// 某个图标的全局中心（用来点清除按钮 —— 图标没有文本可搜）。
    Offset iconCenter(IconCode code) {
        RenderIcon* found = nullptr;
        std::function<void(RenderObject*)> visit = [&](RenderObject* ro) {
            if (!ro || found)
                return;
            if (auto* i = dynamic_cast<RenderIcon*>(ro))
                if (i->code() == code)
                    found = i;
            ro->visitChildren([&](AbstractNode* c) {
                visit(static_cast<RenderObject*>(c));
            });
        };
        visit(root->findRenderObject());
        if (!found)
            return Offset(-1, -1);
        return found->localToGlobal(Offset(found->size().width / 2.0,
                                           found->size().height / 2.0));
    }

    template <typename WidgetT, typename StateT>
    StateT* findState() {
        StateT* found = nullptr;
        std::function<void(Element*)> visit = [&](Element* e) {
            if (!e || found)
                return;
            if (auto* se = dynamic_cast<StatefulElement*>(e))
                if (dynamic_cast<WidgetT*>(se->widget())) {
                    found = static_cast<StateT*>(se->state());
                    return;
                }
            e->visitChildren([&](Element* child) { visit(child); });
        };
        visit(root);
        return found;
    }
};

// ── 1. SearchBar ─────────────────────────────────────────────────────────────
void testSearchBar() {
    std::printf("[test] SearchBar 真实键盘输入\n");

    int changed = 0;
    std::string last;
    std::string submitted;
    auto bar = cui::make_shared<SearchBar>(
            "Search components…",
            [&](const std::string& t) { ++changed; last = t; }, nullptr,
            [&](const std::string& t) { submitted = t; });

    Harness h;
    h.attach(cui::make_shared<SizedBox>(300.0, 40.0, bar), 320, 120);

    auto* st = h.findState<SearchBar, SearchBarState>();
    check(st != nullptr, "拿到 SearchBarState");
    if (!st)
        return;
    check(st->controller() != nullptr && st->focusNode() != nullptr,
          "initState 建好 controller / focusNode");
    check(st->controller()->text().empty(), "初值为空（widget 没预置 value）");
    check(h.countIcon(IconCode::Close) == 0, "空文本 ⇒ 清除按钮不在场");
    check(h.primary() == nullptr, "挂载不抢焦点");

    // 点输入框 ⇒ 上焦（EditableText 自带的 Focus 监听按下）。
    h.tap(150.0, 20.0);
    check(h.primary() == st->focusNode().get(), "点输入框让状态节点上焦");

    // 逐字符键入。
    check(h.type("abc"), "三个可打印键都被消费");
    check(st->controller()->text() == "abc", "键入文本进 controller");
    check(bar->value() == "abc", "每次变化写回 widget.value()");
    check(changed == 3 && last == "abc", "onChanged 逐字符回调，末值 abc");
    check(h.editableText() == "abc", "键入文本上屏（RenderEditable 收到推送）");
    check(h.countIcon(IconCode::Close) == 1, "非空 ⇒ 清除按钮进场");

    // Ctrl+a 是全选而不是插字。
    h.key(KeyCode::None, "a", ModCtrl);
    check(st->controller()->text() == "abc", "Ctrl+a 不追加字面字符");
    check(!st->controller()->selection().isCollapsed(), "Ctrl+a 选中全文");

    // 回车：给了 onSubmitted 才消费。
    check(h.key(KeyCode::Return), "回车被消费（有 onSubmitted）");
    check(submitted == "abc", "onSubmitted 收到当前全文");

    // 点清除按钮：图标本身不命中，外面包了装饰盒子撑出命中区。
    Offset c = h.iconCenter(IconCode::Close);
    h.tap(c.dx, c.dy);
    check(st->controller()->text().empty(), "点清除把文本清空");
    check(bar->value().empty() && changed == 4, "清空同样走 onChanged 通道");
    check(h.countIcon(IconCode::Close) == 0, "清空 ⇒ 清除按钮退场");
    check(h.primary() == st->focusNode().get(), "清除后焦点回到输入框");

    // widget 侧预置的 value 是初值。
    auto seeded = cui::make_shared<SearchBar>("placeholder");
    seeded->setValue("seed");
    Harness h2;
    h2.attach(cui::make_shared<SizedBox>(300.0, 40.0, seeded), 320, 120);
    auto* st2 = h2.findState<SearchBar, SearchBarState>();
    check(st2 && st2->controller()->text() == "seed", "widget 预置值灌进 controller");
}

// ── 2. SearchableList ────────────────────────────────────────────────────────
void testSearchableList() {
    std::printf("[test] SearchableList 真实键盘输入\n");

    const std::vector<std::string> names = {"Alice", "Bob", "Carol", "David", "Eve",
                                            "Frank"};
    std::string submitted;
    auto list = cui::make_shared<SearchableList>(names, std::string(), nullptr,
                                                 nullptr,
                                                 [&](const std::string& t) { submitted = t; });
    Harness h;
    h.attach(cui::make_shared<SizedBox>(320.0, 300.0, list), 340, 320);

    auto* st = h.findState<SearchableList, SearchableListState>();
    check(st != nullptr, "拿到 SearchableListState");
    if (!st)
        return;
    check(h.findParagraphContaining("6 / 6") != nullptr, "初始显示全部 6 项");
    check(h.findParagraphContaining("(all)") != nullptr, "空查询写作 (all)");
    check(h.findParagraphContaining("Alice") != nullptr, "全部条目在场");

    st->focusNode()->requestFocus();
    h.pump();
    check(h.primary() == st->focusNode().get(), "查询框上焦");

    // 大写键入，命中仍大小写不敏感。
    check(h.type("BO"), "键入 B、O");
    check(st->controller()->text() == "BO", "文本进 controller");
    check(list->query() == "BO", "每次变化写回 widget.query()");
    check(h.findParagraphContaining("1 / 6") != nullptr, "过滤到 1 项");
    check(h.findParagraphContaining("Bob") != nullptr, "Bob 在场");
    check(h.findParagraphContaining("Alice") == nullptr, "Alice 已退场");
    check(h.countIcon(IconCode::Close) == 1, "非空查询 ⇒ 清除按钮进场");

    // 退格到只剩一个字母：仍是 1 项（Bob）。
    check(h.key(KeyCode::Backspace), "Backspace 被输入框消费");
    check(st->controller()->text() == "B", "退一格");
    check(h.findParagraphContaining("1 / 6") != nullptr, "「B」还命中 Bob");

    // 全删 ⇒ 回到全量。
    h.key(KeyCode::Backspace);
    check(st->controller()->text().empty(), "退空");
    check(h.findParagraphContaining("6 / 6") != nullptr, "空查询回到 6 项");
    check(h.findParagraphContaining("Alice") != nullptr, "Alice 回到场上");

    // 无匹配。
    h.type("zz");
    check(h.findParagraphContaining("0 / 6") != nullptr, "zz ⇒ 0 项");
    check(h.findParagraphContaining("无匹配") != nullptr, "空结果给占位文案");

    // 回车提交当前查询串。
    check(h.key(KeyCode::Return), "回车被消费（有 onSubmitted）");
    check(submitted == "zz", "onSubmitted 收到查询串");

    // 点清除按钮回到全量。
    Offset c = h.iconCenter(IconCode::Close);
    h.tap(c.dx, c.dy);
    check(st->controller()->text().empty() && list->query().empty(), "点清除清空查询");
    check(h.findParagraphContaining("6 / 6") != nullptr, "清空后回到 6 项");
}

// ── 3. OtpInput ──────────────────────────────────────────────────────────────
void testOtpInput() {
    std::printf("[test] OtpInput 真实键盘输入\n");

    auto otp = cui::make_shared<OtpInput>(4);
    Harness h;
    h.attach(cui::make_shared<SizedBox>(300.0, 60.0, otp), 320, 120);

    auto* st = h.findState<OtpInput, OtpInputState>();
    check(st != nullptr, "拿到 OtpInputState");
    if (!st)
        return;
    check(st->cells().size() == 4, "每格一个焦点节点");
    check(st->code() == "----", "初始全空");
    check(h.primary() == nullptr, "挂载不抢焦点");

    st->cells()[0]->requestFocus();
    h.pump();
    check(h.primary() == st->cells()[0].get(), "第 0 格上焦");

    // 非数字的可打印键：Ignored ⇒ 不写入，也不吞掉（无祖先接 ⇒ sendKeyEvent false）。
    check(!h.key(KeyCode::None, "x"), "字母 x 不被接住（放行给祖先）");
    check(st->code() == "----", "字母不写入格子");

    // 数字写入并自动跳格。
    check(h.key(KeyCode::None, "1"), "数字 1 被消费");
    check(st->code() == "1---", "第一位写入");
    check(st->cells()[1]->hasFocus(), "写完自动跳到下一格");
    check(h.findParagraphContaining("code: 1-") != nullptr, "码串提示上屏");

    h.key(KeyCode::None, "2");
    h.key(KeyCode::None, "3");
    check(st->code() == "123-", "连续键入");
    check(st->cells()[3]->hasFocus(), "焦点停在第 3 格");

    // 空格的 Backspace：退到上一格并清它。
    check(h.key(KeyCode::Backspace), "Backspace 被消费");
    check(st->code() == "12--", "退格清掉第 2 位");
    check(st->cells()[2]->hasFocus(), "空格的 Backspace 把焦点退回上一格");

    // 有数字的格子：只清自己，焦点不动。
    h.key(KeyCode::Backspace);
    check(st->code() == "1---", "再按一次清第 1 位");
    check(st->cells()[1]->hasFocus(), "本格有值时焦点不动");

    // 方向键移焦。
    check(h.key(KeyCode::Right), "Right 被消费");
    check(st->cells()[2]->hasFocus(), "Right 移焦到第 2 格");
    check(h.key(KeyCode::Left), "Left 被消费");
    check(h.key(KeyCode::Left), "Left 再被消费");
    check(st->cells()[0]->hasFocus(), "两次 Left 回到第 0 格");
    h.key(KeyCode::Left);  // 已在第 0 格，越界不动
    check(st->cells()[0]->hasFocus(), "越界的 Left 焦点不变");

    // Delete 清当前格（不动焦点）。
    h.key(KeyCode::None, "9");
    check(st->code() == "9---", "第 0 格写 9");
    check(st->cells()[1]->hasFocus(), "写后跳到第 1 格");
    h.key(KeyCode::Left);
    h.key(KeyCode::Delete);
    check(st->code() == "----", "Delete 清空当前格");
    check(st->cells()[0]->hasFocus(), "Delete 不改焦点");

    // Up/Down 一律放行（页面靠它滚动）。
    check(!h.key(KeyCode::Up), "Up 不被接住");
    check(!h.key(KeyCode::Down), "Down 不被接住");

    // 点格子选位（每格是带边框的装饰盒子，命中区就是格子本身）。
    st->cells()[0]->unfocus();
    h.pump();
    h.tap(19.0, 22.0);  // 第 0 格：宽 38、高 44，中心 (19, 22)
    check(h.primary() == st->cells()[0].get(), "点第 0 格上焦");
    check(st->cells()[0]->hasFocus(), "点中的格子 hasFocus");
    h.key(KeyCode::None, "7");
    check(st->code() == "7---", "点格子后可直接敲数字");
}

// ── 5. 外层 Scrollable 不抢内层控件的焦点 ──────────────────────────────────
// 事件自 target 向根冒泡：Scrollable 为了「点一下就能用方向键滚」也在 onDown
// 上请求焦点。它包在 OtpInput 外面时，内层刚上焦就被外层抢回去 ⇒ 格子高亮和
// 键盘路由一起失效（story「输入+」页实测）。
void testScrollableKeepsInnerFocus() {
    std::printf("[test] Scrollable 的点击聚焦不抢内层控件\n");
    ScrollController ctrl;
    auto otp = cui::make_shared<OtpInput>(4);
    // 内容要比视口高才滚得动；宽度必须显式给（本仓 SizedBox 的 0 是「恰好 0」，
    // 会把子树夹成 0 宽 ⇒ 命中测试直接剪掉，格子点不到）。
    auto body = cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
        otp, cui::make_shared<SizedBox>(400.0, 300.0, cui::make_shared<Text>("下方内容"))});
    auto scroll = cui::make_shared<Scrollable>(&ctrl, Axis::Vertical, body);

    Harness h;
    h.attach(scroll, 400, 200);
    auto* st = h.findState<OtpInput, OtpInputState>();
    check(st != nullptr, "拿到 OtpInputState");
    if (!st)
        return;

    // 格子由 Container 画边框（38×44）；按全局 x 排序取第 idx 个，避免猜布局偏移。
    auto cellCenters = [&] {
        std::vector<RenderDecoratedBox*> boxes;
        std::function<void(RenderObject*)> visit = [&](RenderObject* ro) {
            if (!ro)
                return;
            if (auto* b = dynamic_cast<RenderDecoratedBox*>(ro)) {
                Size s = b->size();
                if (std::abs(s.width - 38.0) < 0.5 && std::abs(s.height - 44.0) < 0.5)
                    boxes.push_back(b);
            }
            ro->visitChildren([&](AbstractNode* n) {
                visit(static_cast<RenderObject*>(n));
            });
        };
        visit(h.root->findRenderObject());
        std::sort(boxes.begin(), boxes.end(), [](RenderDecoratedBox* a, RenderDecoratedBox* b) {
            return a->localToGlobal(Offset(0, 0)).dx < b->localToGlobal(Offset(0, 0)).dx;
        });
        std::vector<Offset> out;
        for (RenderDecoratedBox* b : boxes) {
            Offset o = b->localToGlobal(Offset(b->size().width / 2.0, b->size().height / 2.0));
            if (!out.empty() && std::abs(out.back().dx - o.dx) < 1.0)
                continue;  // 同一个格子的嵌套装饰盒子只算一次
            out.push_back(o);
        }
        return out;
    };

    std::vector<Offset> centers = cellCenters();
    check(centers.size() == 4, "四格都可命中");
    if (centers.size() < 4)
        return;
    const Offset c2 = centers[2];
    h.tap(c2.dx, c2.dy);
    check(h.primary() == st->cells()[2].get(), "点第 2 格上焦（外层没抢）");
    check(st->cells()[2]->hasFocus(), "第 2 格 hasFocus 为真");
    h.key(KeyCode::None, "5");
    check(st->code() == "--5-", "键入落在点中的那一格");
    check(st->cells()[3]->hasFocus(), "写完自动跳到第 3 格");

    // 对照：点在没有可聚焦控件的位置，滚动容器仍要吃下焦点，方向键才滚得动。
    st->cells()[3]->unfocus();
    h.pump();
    h.tap(200.0, 150.0);
    check(h.primary() != nullptr && h.primary() != st->cells()[3].get(),
          "点空白处仍由滚动容器接焦点");
    double before = ctrl.offset();
    h.key(KeyCode::Down);
    check(ctrl.offset() > before, "方向键仍能滚动");
}

// ── 4. 卸载后不留悬挂焦点 ───────────────────────────────────────────────────
class SwapProbe : public StatefulWidget {
public:
    explicit SwapProbe(SharedPtr<Widget> content)
        : StatefulWidget(nullptr), content_(std::move(content)) {}
    const SharedPtr<Widget>& content() const { return content_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SwapProbe"; }

private:
    SharedPtr<Widget> content_;
};

class SwapProbeState : public State {
public:
    bool hidden = false;
    SharedPtr<Widget> build(BuildContext&) override {
        if (hidden)
            return cui::make_shared<SizedBox>(200.0, 20.0);
        return static_cast<SwapProbe*>(widget())->content();
    }
};

inline std::unique_ptr<State> SwapProbe::createState() {
    return std::make_unique<SwapProbeState>();
}

void testTeardown() {
    std::printf("[test] 子树卸载 ⇒ 焦点节点不再是 primaryFocus\n");

    auto otp = cui::make_shared<OtpInput>(3);
    auto probe = cui::make_shared<SwapProbe>(cui::make_shared<SizedBox>(300.0, 60.0, otp));
    Harness h;
    h.attach(probe, 320, 120);

    auto* st = h.findState<OtpInput, OtpInputState>();
    check(st != nullptr, "拿到 OtpInputState");
    if (!st)
        return;
    st->cells()[0]->requestFocus();
    h.pump();
    check(h.primary() != nullptr, "卸载前焦点在某格上");

    auto* probeState = h.findState<SwapProbe, SwapProbeState>();
    check(probeState != nullptr, "拿到 SwapProbeState");
    if (!probeState)
        return;
    // 注意：这一步之后 OtpInputState 连同它的格子节点一起销毁，不能再碰 st。
    probeState->setState([probeState] { probeState->hidden = true; });
    h.pump();
    check(h.primary() == nullptr, "卸载后 primaryFocus 归零（不留悬挂句柄）");
    check(!h.key(KeyCode::None, "5"), "无焦点时按键返回 false 且不崩");
}

// ── 5. SpinBox 直接键入数值 ─────────────────────────────────────────────────
// 值区以前是一枚只读 Text，现在是真的 EditableText：键入即改值、非法字符整串退回、
// 回车/失焦规范化文本、超范围夹到边界、卸载摘干净控制器监听。
void testSpinBox() {
    std::printf("[test] SpinBox 直接键入数值\n");

    int changed = 0;
    double last = -1.0;
    auto spin = cui::make_shared<SpinBox>(3.0, 0.0, 10.0, 1.0, 0,
                                          [&](double v) { ++changed; last = v; });
    Harness h;
    h.attach(spin, 320, 120);

    auto* st = h.findState<SpinBox, SpinBoxState>();
    check(st != nullptr, "拿到 SpinBoxState");
    if (!st)
        return;
    // 外框 96×28，右侧 20px 是箭头列（两段 13px 高、竖直居中），值区是剩下的 76px。
    const double kFieldX = 30.0, kFieldY = 14.0;
    const double kArrowX = 86.0, kUpY = 7.5, kDownY = 20.5;

    check(st->controller() != nullptr && st->focusNode() != nullptr,
          "initState 建好 controller / focusNode");
    check(st->controller()->text() == "3", "初值格式化进输入框");
    check(st->value() == 3.0, "状态值等于 widget 初值");
    check(h.primary() == nullptr, "挂载不抢焦点");

    h.tap(kFieldX, kFieldY);
    check(h.primary() == st->focusNode().get(), "点值区上焦");
    check(st->controller()->hasFocus(), "controller 收到焦点通知");

    h.key(KeyCode::None, "a", ModCtrl);
    check(!st->controller()->selection().isCollapsed(), "Ctrl+a 选中全文");
    check(h.type("7"), "键入数字被消费");
    check(st->controller()->text() == "7", "选中态下键入替换全文");
    check(st->value() == 7.0 && last == 7.0 && changed == 1, "键入实时改值并触发 onChanged");
    check(h.editableText() == "7", "键入文本上屏（RenderEditable 收到推送）");

    check(h.type("42"), "继续键入");
    check(st->controller()->text() == "742" && st->value() == 10.0,
          "超范围实时夹到 max，文本仍是用户打的原样");
    check(last == 10.0 && changed == 2, "夹到边界只算一次变化");

    // 字母仍会被 EditableText 的单槽 onKeyEvent 插进去（键级过滤拦在前面做不到），
    // 由 SpinBox 的校验把整串退回上一次合法文本。
    check(h.type("x"), "字母 x 仍被输入框消费");
    check(st->controller()->text() == "742", "非法字符整串退回上一次合法文本");
    check(st->value() == 10.0, "非法字符不动值");

    h.key(KeyCode::Return);
    check(st->controller()->text() == "10", "回车把文本规范化成当前值");

    // 失焦也规范化（此时文本还留着中间态）。
    h.type("8");
    check(st->controller()->text() == "108" && st->value() == 10.0, "光标在末尾续打");
    st->focusNode()->unfocus();
    h.pump();
    check(st->controller()->text() == "10", "失焦把文本规范化成当前值");
    check(!st->controller()->hasFocus(), "失焦通知进 controller");

    // ▲▼ 仍然可用，并把新值回写进输入框。
    h.tap(kArrowX, kDownY);
    check(st->value() == 9.0 && st->controller()->text() == "9", "点 ▼ 减一且文本回写");
    h.tap(kArrowX, kDownY);
    h.tap(kArrowX, kUpY);
    check(st->value() == 9.0 && st->controller()->text() == "9", "点 ▲ 加一回到 9");
    check(changed == 5, "每次步进各触发一次 onChanged");

    // 小数位：decimals=1 ⇒ 1.25 四舍五入成 1.3。
    double decLast = -1.0;
    auto dec = cui::make_shared<SpinBox>(1.5, 0.0, 5.0, 0.5, 1,
                                         [&](double v) { decLast = v; });
    Harness h2;
    h2.attach(dec, 320, 120);
    auto* ds = h2.findState<SpinBox, SpinBoxState>();
    check(ds != nullptr && ds->controller()->text() == "1.5", "decimals=1 初值带一位小数");
    h2.tap(kFieldX, kFieldY);
    h2.key(KeyCode::None, "a", ModCtrl);
    h2.type("1.25");
    check(ds->controller()->text() == "1.25", "小数点与后续数字都算合法输入");
    check(std::abs(ds->value() - 1.3) < 1e-9 && std::abs(decLast - 1.3) < 1e-9,
          "按 decimals 四舍五入落地");
    ds->focusNode()->unfocus();
    h2.pump();
    check(ds->controller()->text() == "1.3", "失焦后文本用 decimals 格式化");

    // 卸载后监听必须摘干净（控制器可能比 State 活得久）。
    auto spin2 = cui::make_shared<SpinBox>(2.0, 0.0, 10.0, 1.0, 0);
    auto probe = cui::make_shared<SwapProbe>(cui::make_shared<SizedBox>(200.0, 40.0, spin2));
    Harness h3;
    h3.attach(probe, 320, 120);
    auto* ps = h3.findState<SpinBox, SpinBoxState>();
    check(ps != nullptr, "拿到待卸载的 SpinBoxState");
    if (!ps)
        return;
    SharedPtr<TextEditingController> ctrlRef = ps->controller();  // State 死后仍可读计数
    check(ctrlRef->listenerCount() == 2, "挂载时SpinBox + EditableText 各挂一个监听");
    h3.tap(kFieldX, kFieldY);
    check(h3.primary() == ps->focusNode().get(), "卸载前输入框可上焦");
    auto* probeState = h3.findState<SwapProbe, SwapProbeState>();
    probeState->setState([probeState] { probeState->hidden = true; });
    h3.pump();
    check(ctrlRef->listenerCount() == 0, "卸载后控制器监听摘干净");
    check(h3.primary() == nullptr, "卸载后 primaryFocus 归零");
}

// ── 6. CommandPalette ───────────────────────────────────────────────────────
// 查询行以前是一枚只读 Text（「无键盘路由，用首字母筛选按钮代替输入」），现在是
// 真的 EditableText：键入即过滤，↑↓ 移动高亮、回车执行、Esc 清空。导航键处理器与
// 输入框共用同一个 FocusNode 的多槽（EditableText 的单槽对这些键一律 Ignored）。
void testCommandPalette() {
    std::printf("[test] CommandPalette 键盘输入与列表导航\n");

    const std::vector<CommandItem> cmds = {
        {"Open File", "打开文件", "Ctrl+P"},
        {"Go to Line", "跳转到行", "Ctrl+G"},
        {"Toggle Terminal", "切换终端", "Ctrl+`"},
        {"Format Document", "格式化文档", "Shift+Alt+F"},
        {"Run Build Task", "执行构建", "Ctrl+Shift+B"}};
    std::vector<int> ran;
    auto palette = cui::make_shared<CommandPalette>(cmds, [&](int i) { ran.push_back(i); });
    Harness h;
    h.attach(palette, 400, 300);

    auto* st = h.findState<CommandPalette, CommandPaletteState>();
    check(st != nullptr, "拿到 CommandPaletteState");
    if (!st)
        return;
    check(st->controller() != nullptr && st->focusNode() != nullptr,
          "initState 建好 controller / focusNode");
    check(st->query().empty(), "查询初值为空");
    check(st->matched().size() == 5, "空查询 ⇒ 五条命令全在场");
    check(st->matched()[4] == 4, "matched() 存的是原始下标");
    check(st->highlight() == 0, "首行默认高亮（回车可直接执行）");
    check(h.primary() == nullptr, "挂载不抢焦点");
    check(st->focusNode()->keyHandlerCount() == 1, "导航键处理器挂在共享节点的多槽上");

    Offset field = h.editableCenter();
    check(field.dx > 0, "找到查询框的命中中心");
    h.tap(field.dx, field.dy);
    check(h.primary() == st->focusNode().get(), "点查询框上焦");

    // 键入即过滤（大小写不敏感子串）。
    check(h.type("go"), "键入 g、o");
    check(st->query() == "go", "文本进 controller");
    check(st->matched().size() == 1 && st->matched()[0] == 1, "只剩「Go to Line」");
    check(st->highlight() == 0, "过滤后高亮回到第一条匹配");
    check(h.findParagraphContaining("Go to Line") != nullptr, "匹配行在场");
    check(h.findParagraphContaining("Open File") == nullptr, "不匹配行退场");

    h.key(KeyCode::None, "a", ModCtrl);
    h.type("OP");
    check(st->matched().size() == 1 && st->matched()[0] == 0, "大写「OP」命中 Open File");

    // 无匹配：占位文案在场，方向键与回车一律放行（不吞键）。
    h.key(KeyCode::None, "a", ModCtrl);
    h.type("zz");
    check(st->matched().empty() && st->highlight() == -1, "zz ⇒ 无匹配、高亮为空");
    check(h.findParagraphContaining("（无匹配命令）") != nullptr, "空结果给占位文案");
    check(!h.key(KeyCode::Down), "无匹配时 ↓ 放行给祖先");
    check(!h.key(KeyCode::Return), "无匹配时回车放行");
    check(ran.empty(), "无匹配时回车不执行任何命令");

    // Esc 清空查询（清空后 Esc 再按就不该消费）。
    check(h.key(KeyCode::Escape), "Esc 清空查询");
    check(st->query().empty() && st->matched().size() == 5, "清空后回到五条命令");
    check(st->highlight() == 0, "清空后高亮回首行");
    check(!h.key(KeyCode::Escape), "查询已空 ⇒ Esc 放行");

    // ↑↓ 移动高亮，到顶停住（不环绕）。
    check(h.key(KeyCode::Down), "↓ 被消费");
    check(h.key(KeyCode::Down), "↓ 再被消费");
    check(st->highlight() == 2, "两次 ↓ 高亮到第 2 槽");
    check(h.key(KeyCode::Up), "↑ 被消费");
    check(st->highlight() == 1, "↑ 回退一槽");
    for (int i = 0; i < 3; ++i)
        h.key(KeyCode::Up);
    check(st->highlight() == 0, "越界的 ↑ 停在首槽（不环绕）");

    // 回车执行高亮项 —— 传出去的是 commands() 的原始下标。
    check(h.key(KeyCode::Return), "回车被消费");
    check(ran.size() == 1 && ran[0] == 0, "回车执行第 0 条（Open File）");

    // 过滤后回车：槽位 0 ⇒ 原始下标 4。
    h.type("run");
    check(st->matched().size() == 1 && st->matched()[0] == 4, "「run」只剩 Run Build Task");
    check(h.key(KeyCode::Return), "过滤态回车被消费");
    check(ran.size() == 2 && ran[1] == 4, "执行的是原始下标 4");

    // 鼠标路径仍然通：点行执行、点「全部」筛选条清空查询。
    h.key(KeyCode::Escape);
    check(st->matched().size() == 5, "为鼠标用例先清空查询");
    h.tapText("Toggle Terminal");
    check(ran.size() == 3 && ran[2] == 2, "点行执行对应命令");
    h.type("go");
    h.tapText("全部");
    check(st->query().empty() && st->matched().size() == 5, "点「全部」筛选条清空查询");

    // 卸载后键处理器与控制器监听都必须摘干净。
    auto palette2 = cui::make_shared<CommandPalette>(cmds, nullptr);
    auto probe = cui::make_shared<SwapProbe>(palette2);
    Harness h2;
    h2.attach(probe, 400, 300);
    auto* ps = h2.findState<CommandPalette, CommandPaletteState>();
    check(ps != nullptr, "拿到待卸载的 CommandPaletteState");
    if (!ps)
        return;
    SharedPtr<TextEditingController> ctrlRef = ps->controller();
    SharedPtr<FocusNode> nodeRef = ps->focusNode();
    check(ctrlRef->listenerCount() == 2, "挂载时 State + EditableText 各挂一个监听");
    check(nodeRef->keyHandlerCount() == 1, "挂载时节点上有一条导航键处理器");
    h2.tap(h2.editableCenter().dx, h2.editableCenter().dy);
    check(h2.primary() == nodeRef.get(), "卸载前查询框可上焦");
    auto* probeState = h2.findState<SwapProbe, SwapProbeState>();
    probeState->setState([probeState] { probeState->hidden = true; });
    h2.pump();
    check(ctrlRef->listenerCount() == 0, "卸载后控制器监听摘干净");
    check(nodeRef->keyHandlerCount() == 0, "卸载后节点键处理器摘干净");
    check(h2.primary() == nullptr, "卸载后 primaryFocus 归零");
}

}  // namespace

int main() {
    std::printf("=== test_keyboard_inputs ===\n");
    testSearchBar();
    testSearchableList();
    testOtpInput();
    testScrollableKeepsInnerFocus();
    testTeardown();
    testSpinBox();
    testCommandPalette();
    std::printf(g_failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
