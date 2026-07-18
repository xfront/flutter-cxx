/// @file test_text_input.cpp
/// @brief 上游 文本输入层单元测试（controller / 键盘路由 / IME / RenderEditable）。
///
/// 全部 headless：controller 逻辑与键盘路由直接验证；RenderEditable 几何用
/// Painter::measureTextWidth 静态测量，无需窗口。

#include <cstdio>
#include <memory>
#include <string>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/focus.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_editable.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
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

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        auto* ro = root->findRenderObject();
        rv.setChild(static_cast<RenderBox*>(ro));
        po.setRootNode(&rv);
        po.flushLayout();
    }
};

// ── 测试 1：TextEditingController 编辑操作 + UTF-8 码点边界 ───────────────
void testControllerEditing() {
    std::printf("[test] TextEditingController 编辑操作\n");

    TextEditingController c("hello");
    check(c.text() == "hello", "初始文本");
    check(c.selection().isCollapsed() && c.selection().extent() == 5, "光标在末尾");

    c.insert(" world");
    check(c.text() == "hello world", "insert 在光标处插入");
    check(c.selection().extent() == 11, "insert 后光标在插入末尾");

    c.moveCursorLeft(false);
    check(c.selection().extent() == 10, "左移一格");
    c.moveCursorRight(false);
    c.moveCursorToStart(false);
    check(c.selection().extent() == 0, "Home → 光标到起点");
    c.moveCursorToEnd(false);
    check(c.selection().extent() == 11, "End → 光标到末尾");

    // 选中全部并替换
    c.selectAll();
    check(!c.selection().isCollapsed() && c.selection().start() == 0 &&
              c.selection().end() == 11, "selectAll 选中 [0,11)");
    c.replaceSelection("x");
    check(c.text() == "x" && c.selection().isCollapsed(), "replaceSelection 替换选区");

    // 退格（collapsed）
    c.setText("abc");
    c.deleteCharBackward();
    check(c.text() == "ab", "Backspace 删前一个码点");

    // 删除键（collapsed）
    c.setText("abc");
    c.moveCursorToStart(false);
    c.deleteCharForward();
    check(c.text() == "bc", "Delete 删后一个码点");

    // UTF-8：中文 2 码点 = 6 字节，退格应删整码点而非半个
    c.setText("\xe4\xbd\xa0\xe5\xa5\xbd");  // 你好
    c.deleteCharBackward();
    check(c.text() == "\xe4\xbd\xa0", "Backspace 删整码点（你好→你，6→3 字节）");
    check(c.text().size() == 3, "退格后字节数为 3（非半个 UTF-8）");

    // clear
    c.clear();
    check(c.text().empty() && c.selection().isCollapsed(), "clear 清空");
}

// ── 测试 2：键盘路由（FocusManager::primaryFocus → onKeyEvent）────────────
void testKeyboardRouting() {
    std::printf("[test] 键盘路由（焦点节点 → EditableText 处理器）\n");

    auto ctrl = cui::make_shared<TextEditingController>("");
    auto ed = cui::make_shared<EditableText>(ctrl, TextStyle{}, TextAlign::Left, "", false,
                                             /*autofocus=*/true);

    Harness h;
    h.attach(ed, 300, 40);
    auto* node = h.owner.focusManager().primaryFocus();
    check(node != nullptr, "autofocus 后 primaryFocus 非空");
    check(node->hasFocus(), "焦点节点 hasFocus() == true");
    check(node->onKeyEvent != nullptr, "EditableText 注册了 onKeyEvent");

    // 逐字符键入
    const char* typed = "Hi";
    for (const char* p = typed; *p; ++p) {
        KeyEvent ev{0, std::string(1, *p), 0, false};
        node->onKeyEvent(ev);
    }
    check(ctrl->text() == "Hi", "键入 'Hi' 进 controller");
    check(ctrl->selection().extent() == 2, "键入后光标在末尾");

    // 退格
    node->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::Backspace), "", 0, false});
    check(ctrl->text() == "H", "Backspace 删除 'i'");

    // 左移 + 插入（在 'H' 前插 'X'）
    node->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::Home), "", 0, false});
    node->onKeyEvent(KeyEvent{0, "X", 0, false});
    check(ctrl->text() == "XH", "Home 后插入 'X' → 'XH'");

    // Shift+Right 选中 'X'
    node->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::Home), "", 0, false});
    node->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::Right), "", ModShift, false});
    check(!ctrl->selection().isCollapsed() && ctrl->selection().start() == 0 &&
              ctrl->selection().end() == 1, "Shift+Right 选中 [0,1)");

    // Ctrl+A 全选
    node->onKeyEvent(KeyEvent{0, "a", ModCtrl, false});
    check(!ctrl->selection().isCollapsed() && ctrl->selection().end() == 2, "Ctrl+A 全选");

    // 同焦点域内切换焦点：ed2 在独立 Harness（独立 BuildOwner → 独立焦点域）
    // 中 autofocus，验证其自身节点获焦（每 BuildOwner 一个 FocusManager）。
    auto ctrl2 = cui::make_shared<TextEditingController>("");
    auto ed2 = cui::make_shared<EditableText>(ctrl2, TextStyle{}, TextAlign::Left, "", false,
                                              true);
    Harness h2;
    h2.attach(ed2, 300, 40);
    auto* node2 = h2.owner.focusManager().primaryFocus();
    check(node2 != nullptr && node2->hasFocus(), "ed2 节点在其焦点域获焦");
    // 同域内焦点唯一：焦点域只能有一个 primaryFocus（与首个 harness 的
    // 焦点互不影响，印证「每 BuildOwner 一个 FocusManager」）。
    check(h.owner.focusManager().primaryFocus() == node, "两个焦点域各自独立、互不干扰");
}

// ── 测试 3：IME 组合（preedit → commit）────────────────────────────────────
void testImeComposition() {
    std::printf("[test] IME 组合（preedit → commit）\n");

    auto ctrl = cui::make_shared<TextEditingController>("");
    auto ed = cui::make_shared<EditableText>(ctrl, TextStyle{}, TextAlign::Left, "", false,
                                             true);
    Harness h;
    h.attach(ed, 300, 40);
    auto* node = h.owner.focusManager().primaryFocus();

    // preedit 进行中
    node->onPreedit("\xe4\xbd\xa0");  // 你
    check(ctrl->text() == "\xe4\xbd\xa0", "preedit 写入组合文本");
    check(ctrl->isComposing(), "isComposing() == true");

    // 继续组合
    node->onPreedit("\xe4\xbd\xa0\xe5\xa5\xbd");  // 你好
    check(ctrl->text() == "\xe4\xbd\xa0\xe5\xa5\xbd", "preedit 更新组合文本");

    // 提交
    node->onIMCommit("\xe4\xbd\xa0\xe5\xa5\xbd");  // 你好
    check(ctrl->text() == "\xe4\xbd\xa0\xe5\xa5\xbd", "commit 后文本保留");
    check(!ctrl->isComposing(), "commit 后组合态清除");
    check(ctrl->selection().extent() == 6, "commit 后光标在组合末尾");

    // 无组合态时直接 commit（等价于插入）
    node->onIMCommit("!");
    check(ctrl->text() == "\xe4\xbd\xa0\xe5\xa5\xbd!", "无组合态 commit 追加文本");
}

// ── 测试 4：RenderEditable 几何（光标定位 / 选区偏移）──────────────────────
void testRenderEditableGeometry() {
    std::printf("[test] RenderEditable 字符定位几何\n");

    auto re = cui::make_shared<RenderEditable>(TextStyle{}, TextAlign::Left, "", false);
    re->setText("abc");
    // 单调：索引 0→x0，索引 3（末尾）→ 文本宽度
    double x0 = re->offsetForIndex(0);
    double x3 = re->offsetForIndex(3);
    check(x0 == 0.0, "offsetForIndex(0) == 0");
    check(x3 > x0, "末尾偏移大于起点（文本有宽度）");
    // getPositionForX 在起点附近落回 0
    check(re->getPositionForX(0.0) == 0, "getPositionForX(0) 命中索引 0");
    // 中点附近落到一个中间索引（0、1、2 或 3）
    std::size_t mid = re->getPositionForX(x3 * 0.5);
    check(mid >= 1 && mid <= 2, "中点附近命中中间码点");

    // 占位符与聚焦态不崩溃（触发 performPaint 前的状态设置）
    re->setHasFocus(true);
    re->setSelection(0, 3);  // 选区高亮路径
    check(re->hasFocus(), "setHasFocus 生效");

    // 中文文本宽度应为多码点累计（非 0）
    auto re2 = cui::make_shared<RenderEditable>(TextStyle{}, TextAlign::Left, "", false);
    re2->setText("\xe4\xbd\xa0\xe5\xa5\xbd");  // 你好
    check(re2->offsetForIndex(6) > 0, "中文文本有正宽度");
    check(re2->getPositionForX(re2->offsetForIndex(6) * 0.5) >= 3,
          "中文中点定位到第二个码点（字节≥3）");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_text_input ===\n");
    testControllerEditing();
    testKeyboardRouting();
    testImeComposition();
    testRenderEditableGeometry();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
