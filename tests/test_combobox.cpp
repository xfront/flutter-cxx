/// @file test_combobox.cpp
/// @brief ComboBox（上游 RawAutocomplete 移植）单元测试。
///
/// 覆盖：
///   1. 预选文本 / 焦点驱动展开（上游 `_canShowOptionsView`）。
///   2. 大小写不敏感子串过滤；文本回到选中项 ⇒ 恢复全量。
///   3. 键盘导航：↑↓ / PgUp·PgDn / Ctrl+↑↓ = 末·首项 / Enter 选中 / Esc 收起；
///      收起状态下一个键都不接（放行给祖先）。
///   4. 键派发顺序：字符键仍归输入框（同节点多处理器，注册序派发）。
///   5. 点击行选中并写回文本；barrier 外点收起。
///   6. 高亮落到 5 行窗口外时窗口平移。
///   7. 卸载后键处理器与控制器监听都摘干净。

#include <cstdio>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/platform/event.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/combobox.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/overlay.h"
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

const std::vector<std::string> kLangs = {"Rust", "Python", "JavaScript",
                                         "TypeScript", "Go", "C++", "Java",
                                         "Kotlin"};

/// 带 Navigator（自带 Overlay）的 Harness，抄 test_popup 的口径。
struct NavHarness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;
    NavigatorState* state = nullptr;
    BuildContext* pageCtx = nullptr;

    void attach() {
        rv.setWindowSize({400, 400});
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

    void pushPage(SharedPtr<Widget> page) {
        state->push(cui::make_shared<ContentRoute>(
            [this, page](BuildContext& ctx) {
                pageCtx = &ctx;
                return page;
            },
            /*opaque=*/true));
        pump();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    bool key(KeyCode code, int modifiers = 0) {
        KeyEvent ev{static_cast<int>(code), "", modifiers, false};
        return owner.focusManager().sendKeyEvent(ev);
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

    /// 点击某段文本所在行的中心（用它命中浮层行 / barrier 之外处）。
    void tapText(const std::string& sub) {
        RenderParagraph* p = findParagraphContaining(sub);
        if (!p)
            return;
        Offset c = p->localToGlobal(Offset(p->size().width / 2.0,
                                           p->size().height / 2.0));
        tap(c.dx, c.dy);
    }

    RenderParagraph* findParagraphContaining(const std::string& sub) {
        RenderParagraph* found = nullptr;
        std::function<void(RenderObject*)> visit = [&](RenderObject* ro) {
            if (!ro || found)
                return;
            if (auto* p = dynamic_cast<RenderParagraph*>(ro)) {
                if (p->text() &&
                    p->text()->toPlainText().find(sub) != std::string::npos)
                    found = p;
            }
            ro->visitChildren([&](AbstractNode* n) {
                visit(static_cast<RenderObject*>(n));
            });
        };
        visit(root->findRenderObject());
        return found;
    }

    ComboBoxState* comboState() {
        ComboBoxState* found = nullptr;
        std::function<void(Element*)> visit = [&](Element* e) {
            if (!e || found)
                return;
            if (auto* se = dynamic_cast<StatefulElement*>(e))
                if (dynamic_cast<ComboBox*>(se->widget())) {
                    found = static_cast<ComboBoxState*>(se->state());
                    return;
                }
            e->visitChildren([&](Element* child) { visit(child); });
        };
        visit(root);
        return found;
    }
};

/// 只在场/卸载两态之间切换 ComboBox 子树，用来验拆除链路。
class SwapProbe : public StatefulWidget {
public:
    explicit SwapProbe(SharedPtr<Widget> content, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), content_(std::move(content)) {}
    const SharedPtr<Widget>& content() const { return content_; }
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "SwapProbe"; }

private:
    SharedPtr<Widget> content_;
};

class SwapProbeState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        if (hidden_)
            return cui::make_shared<SizedBox>(200.0, 40.0);
        return static_cast<SwapProbe*>(widget())->content();
    }
    bool hidden_ = false;
};

inline std::unique_ptr<State> SwapProbe::createState() {
    return std::make_unique<SwapProbeState>();
}

// ── 1. 预选与焦点展开 ──────────────────────────────────────────────────────
void testInitialAndFocus() {
    std::printf("[test] ComboBox 预选 / 焦点驱动展开\n");
    NavHarness h;
    h.attach();

    auto combo = cui::make_shared<ComboBox>(kLangs, /*selectedIndex=*/2);
    h.pushPage(cui::make_shared<SizedBox>(400.0, 400.0, combo));

    auto* st = h.comboState();
    check(st != nullptr, "拿到 ComboBoxState");
    if (!st)
        return;
    const SharedPtr<TextEditingController>& text = st->controller();
    check(text->text() == "JavaScript", "预选项文本写进输入框");
    check(st->options().size() == kLangs.size(),
          "文本=选中项 ⇒ 选项集合是全量而不是单条");
    check(!st->isExpanded(), "未聚焦时浮层收起");

    st->focusNode()->requestFocus();
    h.pump();
    check(st->isExpanded(), "获得焦点 ⇒ 展开（上游 _canShowOptionsView）");
    check(h.findParagraphContaining("Rust") != nullptr, "浮层里有首项 Rust");
    check(h.findParagraphContaining("Go") != nullptr, "5 行窗口里第 5 项 Go 在场");
    check(h.findParagraphContaining("Kotlin") == nullptr,
          "窗口外的末项 Kotlin 不在场（见 §6 窗口平移）");

    st->focusNode()->unfocus();
    h.pump();
    check(!st->isExpanded(), "失去焦点 ⇒ 收起");
    check(h.findParagraphContaining("Rust") == nullptr, "收起后浮层不在场");
}

// ── 2. 过滤 ────────────────────────────────────────────────────────────────
void testFiltering() {
    std::printf("[test] ComboBox 子串过滤\n");
    NavHarness h;
    h.attach();
    h.pushPage(cui::make_shared<SizedBox>(
        400.0, 400.0, cui::make_shared<ComboBox>(kLangs)));
    auto* st = h.comboState();
    if (!st) {
        check(false, "拿到 ComboBoxState");
        return;
    }

    st->focusNode()->requestFocus();
    st->controller()->setText("ja");
    h.pump();
    check(st->options().size() == 2, "「ja」过滤到 2 项（JavaScript / Java）");
    check(st->options()[0] == 2 && st->options()[1] == 6,
          "过滤结果是原始下标序列");
    check(st->isExpanded(), "有焦点 + 有结果 ⇒ 展开");

    st->controller()->setText("JA");
    h.pump();
    check(st->options().size() == 2, "大小写不敏感");

    st->controller()->setText("zz");
    h.pump();
    check(st->options().empty(), "无匹配 ⇒ 集合空");
    check(!st->isExpanded(), "集合空 ⇒ 收起（上游 _options.isNotEmpty）");

    st->controller()->setText("");
    h.pump();
    check(st->options().size() == kLangs.size(), "清空查询 ⇒ 回到全量");
    check(st->isExpanded(), "重新有结果 ⇒ 再展开");
}

// ── 3. 键盘导航 ────────────────────────────────────────────────────────────
void testKeyboardNavigation() {
    std::printf("[test] ComboBox 键盘导航\n");
    NavHarness h;
    h.attach();

    int selected = -1;
    int callbacks = 0;
    h.pushPage(cui::make_shared<SizedBox>(
        400.0, 400.0,
        cui::make_shared<ComboBox>(
            kLangs, -1, "Select...",
            [&](int index) {
                ++callbacks;
                selected = index;
            })));
    auto* st = h.comboState();
    if (!st) {
        check(false, "拿到 ComboBoxState");
        return;
    }
    st->focusNode()->requestFocus();
    h.pump();
    check(st->isExpanded() && st->highlightedIndex() == 0, "展开且高亮首项");

    check(h.key(KeyCode::Down), "↓ 被 ComboBox 消费");
    check(st->highlightedIndex() == 1, "↓ 高亮 +1");
    h.key(KeyCode::Down);
    h.key(KeyCode::Up);
    check(st->highlightedIndex() == 1, "↑ 高亮 -1");
    h.key(KeyCode::PageDown);
    check(st->highlightedIndex() == 5, "PgDn 前进 _pageSize(4)");
    h.key(KeyCode::PageUp);
    check(st->highlightedIndex() == 1, "PgUp 后退 4");
    h.key(KeyCode::Down, ModCtrl);
    check(st->highlightedIndex() == static_cast<int>(kLangs.size()) - 1,
          "Ctrl+↓ = 末项（上游 AutocompleteLastOptionIntent）");
    h.key(KeyCode::Up, ModCtrl);
    check(st->highlightedIndex() == 0, "Ctrl+↑ = 首项");
    h.key(KeyCode::Down, ModCtrl);
    h.key(KeyCode::Down);
    check(st->highlightedIndex() == static_cast<int>(kLangs.size()) - 1,
          "末项再按 ↓ 夹住不越界");

    check(h.key(KeyCode::Return), "Enter 被消费");
    check(selected == 7 && callbacks == 1, "Enter 选中末项并回调 Kotlin(7)");
    check(st->controller()->text() == "Kotlin", "选中后文本写成该项");
    check(!st->isExpanded(), "选中后收起");

    // 收起状态下不接任何键（放行给祖先：滚动视图 / 页面 Enter）。
    check(!h.key(KeyCode::Down), "收起时 ↓ 返回未消费");
    check(!h.key(KeyCode::Return), "收起时 Enter 返回未消费");
    check(!h.key(KeyCode::Escape), "收起时 Esc 返回未消费");
    check(callbacks == 1, "未展开时的按键没有再触发回调");

    // Esc：展开后收起且不回调。选中后焦点没丢，所以「重新展开」要靠真失焦再获焦。
    st->focusNode()->unfocus();
    st->focusNode()->requestFocus();
    h.pump();
    check(st->isExpanded(), "重新聚焦 ⇒ 再次展开");
    check(h.key(KeyCode::Escape), "Esc 被消费");
    check(!st->isExpanded(), "Esc 收起浮层");
    check(callbacks == 1, "Esc 不触发选中回调");
}

// ── 4. 同节点多处理器：字符键仍归输入框 ────────────────────────────────────
void testHandlerOrdering() {
    std::printf("[test] ComboBox 与输入框共享节点的处理器顺序\n");
    NavHarness h;
    h.attach();
    auto combo = cui::make_shared<ComboBox>(kLangs);
    h.pushPage(cui::make_shared<SizedBox>(400.0, 400.0, combo));
    auto* st = h.comboState();
    if (!st) {
        check(false, "拿到 ComboBoxState");
        return;
    }
    auto node = st->focusNode();
    check(node->keyHandlerCount() == 1, "ComboBox 在字段节点上挂了 1 个键处理器");
    check(node->onKeyEvent != nullptr, "输入框自己占着单槽 onKeyEvent");

    st->controller()->setText("");
    st->focusNode()->requestFocus();
    h.pump();
    check(st->isExpanded() && st->highlightedIndex() == 0, "展开且高亮首项");

    KeyEvent print{0, "x", 0, false};
    check(h.owner.focusManager().sendKeyEvent(print), "可打印字符被输入框消费");
    check(st->controller()->text() == "x", "字符进到输入框而不是被浮层吞掉");
}

// ── 5. 点击行选中 / barrier 外点收起 ───────────────────────────────────────
void testTapSelectionAndBarrier() {
    std::printf("[test] ComboBox 点击选中与 barrier\n");
    NavHarness h;
    h.attach();
    int selected = -1;
    // 页根包一层 pinned `Align` 而不是 `SizedBox(400,400)`：RenderView 给的是窗口
    // 大小的紧约束，紧约束一路穿透到 ComboBox 的字段盒 ⇒ 字段被撑成 400 高，浮层
    // 按上游口径锚在「字段底边 + gap」就落到 y=404（窗口外，Stack 默认还会裁掉）。
    // 每条盒子尺寸闸门（`RenderBox::hitTest`）上线后，窗口外的浮层点不到 —— 正是
    // 上游的语义，所以这里让字段保持自然高度，浮层才真的出现在可视区内。
    h.pushPage(cui::make_shared<Align>(
        -1.0, -1.0,
        cui::make_shared<ComboBox>(kLangs, -1, "Select...",
                                   [&](int index) { selected = index; })));
    auto* st = h.comboState();
    if (!st) {
        check(false, "拿到 ComboBoxState");
        return;
    }
    st->focusNode()->requestFocus();
    h.pump();
    check(st->isExpanded(), "聚焦后展开");

    h.tapText("Go");
    check(selected == 4, "点行「Go」触发 onSelected(4)");
    check(st->controller()->text() == "Go", "点行后文本写成 Go");
    check(!st->isExpanded(), "点行后收起");

    // 再次展开，点 barrier（左上角，远离浮层）收起。
    st->focusNode()->unfocus();
    st->focusNode()->requestFocus();
    h.pump();
    check(st->isExpanded(), "再次展开");
    h.tap(380.0, 390.0);
    check(!st->isExpanded(), "点击浮层外（barrier）收起");
}

// ── 6. 窗口平移 ────────────────────────────────────────────────────────────
void testWindowShift() {
    std::printf("[test] ComboBox 高亮窗口平移\n");
    NavHarness h;
    h.attach();
    h.pushPage(cui::make_shared<SizedBox>(
        400.0, 400.0, cui::make_shared<ComboBox>(kLangs)));
    auto* st = h.comboState();
    if (!st) {
        check(false, "拿到 ComboBoxState");
        return;
    }
    st->focusNode()->requestFocus();
    h.pump();

    check(h.findParagraphContaining("Rust") != nullptr, "窗口从首项开始：Rust 在场");
    check(h.findParagraphContaining("Kotlin") == nullptr,
          "5 行窗口里不含末项 Kotlin");

    for (int i = 0; i < 7; ++i)
        h.key(KeyCode::Down);
    h.pump();
    check(st->highlightedIndex() == 7, "连续 ↓ 走到末项");
    check(h.findParagraphContaining("Kotlin") != nullptr, "末项进窗口");
    check(h.findParagraphContaining("Rust") == nullptr, "窗口平移后 Rust 出局");

    for (int i = 0; i < 7; ++i)
        h.key(KeyCode::Up);
    h.pump();
    check(st->highlightedIndex() == 0, "连续 ↑ 回到首项");
    check(h.findParagraphContaining("Rust") != nullptr, "窗口平移回起点");
}

// ── 7. 拆除后监听摘干净 ────────────────────────────────────────────────────
void testTeardown() {
    std::printf("[test] ComboBox 拆除：键处理器与监听摘除\n");
    NavHarness h;
    h.attach();

    auto text = cui::make_shared<TextEditingController>();
    auto node = cui::make_shared<FocusNode>();
    auto combo = cui::make_shared<ComboBox>(kLangs, -1, "Select...", nullptr, 240.0,
                                            nullptr, text, node);
    auto probe = cui::make_shared<SwapProbe>(combo);
    h.pushPage(cui::make_shared<SizedBox>(400.0, 400.0, probe));

    auto* st = h.comboState();
    if (!st) {
        check(false, "拿到 ComboBoxState");
        return;
    }
    node->requestFocus();
    h.pump();
    check(st->isExpanded(), "卸载前处于展开态");
    const size_t listenersMounted = text->listenerCount();
    check(listenersMounted >= 2,
          "挂载期监听 = 输入框 + ComboBox（含 EditableText 自己）");
    check(node->keyHandlerCount() == 1, "挂载期键处理器 = 1");

    // 切走 ComboBox 子树 ⇒ State 卸载，浮层条目必须一并移除。
    auto* ps = static_cast<SwapProbeState*>(
        [&] {
            State* found = nullptr;
            std::function<void(Element*)> visit = [&](Element* e) {
                if (!e || found)
                    return;
                if (auto* se = dynamic_cast<StatefulElement*>(e))
                    if (dynamic_cast<SwapProbe*>(se->widget())) {
                        found = static_cast<State*>(se->state());
                        return;
                    }
                e->visitChildren([&](Element* c) { visit(c); });
            };
            visit(h.root);
            return found;
        }());
    if (!ps) {
        check(false, "拿到 SwapProbeState");
        return;
    }
    ps->hidden_ = true;
    ps->setState();
    h.pump();

    check(text->listenerCount() < listenersMounted, "卸载后 ComboBox 监听已摘除");
    check(node->keyHandlerCount() == 0, "卸载后键处理器已摘除");

    // 控制器活在控件之外：之后再通知不得打到已销毁的 State。
    text->setText("Rust");
    text->setHasFocus(true);
    h.pump();
    check(true, "摘除后再通知不崩（悬挂回调会在这里炸）");
}

}  // namespace

int main() {
    testInitialAndFocus();
    testFiltering();
    testKeyboardNavigation();
    testHandlerOrdering();
    testTapSelectionAndBarrier();
    testWindowShift();
    testTeardown();

    if (g_failures == 0)
        std::printf("ALL PASS\n");
    else
        std::printf("%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
