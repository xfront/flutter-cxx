/// @file test_code_editor.cpp
/// @brief CodeEditor（对齐 code_forge 11.7.0）的模型层 / 视图层单元测试。
///
/// 全部 headless：控制器（文档 / 选区 / 撤销合并 / 行操作 / 折叠 / 查找）直接
/// 断言；控件层挂到 BuildOwner + RenderView + PipelineOwner 上验证焦点键路由、
/// IME 提交与监听器摘除。

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/code_editor/code_editor.h"
#include "cui/foundation/events.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/framework/focus.h"
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

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }
};

// ── 1：文档结构与行索引 ─────────────────────────────────────────────────────
void testDocument() {
    std::printf("[test] 文档结构与行索引\n");

    CodeEditorController c("int main() {\n  return 0;\n}\n");
    check(c.lineCount() == 4, "尾随换行 ⇒ 末尾多一个空行（4 行）");
    check(c.lines()[0] == "int main() {" && c.lines()[1] == "  return 0;",
          "lines() 按 '\\n' 切分");
    check(c.getLineStartOffset(0) == 0, "第 0 行起点 0");
    check(c.getLineStartOffset(1) == 13, "第 1 行起点 = 12 + 1");
    check(c.getLineAtOffset(0) == 0 && c.getLineAtOffset(12) == 0,
          "行内偏移归属当前行");
    check(c.getLineAtOffset(13) == 1, "换行后的首字节属于下一行");
    check(c.findLineEnd(0) == 12, "findLineEnd 不含换行");
    check(c.findLineStart(15) == 13, "findLineStart 回退到行首");
    check(c.getOffsetAt(1, 2) == 15, "getOffsetAt(line, column) 换算");
    check(c.getOffsetAt(99, 99) == c.length(), "越界行列夹到文末");

    // 选区夹取
    c.setSelectionImmediately(1000, 5);
    check(c.selection().start() == 5 && c.selection().end() == c.length(),
          "setSelectionImmediately 夹到 [0, length]");

    // 光标行 / 可视列（制表符按 tabSize 展开）
    CodeEditorController t("a\tb\nc");
    t.setSelectionImmediately(2, 2);  // 'b'
    check(t.caretLine() == 0, "caretLine 跟随偏移");
    check(t.caretColumn() == 4, "caretColumn 展开制表符（tabSize 4）");
}

// ── 2：键入 / 自动配对 / 自动缩进 / UTF-8 ──────────────────────────────────
void testTyping() {
    std::printf("[test] 键入、自动配对与自动缩进\n");

    CodeEditorController c("a");
    c.insertTypedText("(");
    check(c.text() == "a()" && c.selection().extent() == 2, "左括号自动补右括号，光标在中间");
    c.insertTypedText("x");
    check(c.text() == "a(x)" && c.selection().extent() == 3, "括号内继续键入");
    c.insertTypedText(")");
    check(c.text() == "a(x)" && c.selection().extent() == 4, "同字符右括号 ⇒ 跳过而不是重复插入");

    CodeEditorController q("\"\"");
    q.setSelectionImmediately(1, 1);
    q.insertTypedText("\"");
    check(q.text() == "\"\"" && q.selection().extent() == 2, "引号同样成对跳过");

    CodeEditorController d("void f() {");
    d.setSelectionImmediately(d.length(), d.length());
    d.insertTypedText("\n");
    check(d.text() == "void f() {\n    ", "回车在 '{' 后追加一级缩进");
    check(d.selection().extent() == d.length(), "回车后光标在缩进之后");

    CodeEditorController e("{}");
    e.setSelectionImmediately(1, 1);
    e.insertTypedText("\n");
    check(e.text() == "{\n    \n}", "括号间回车劈成三行");
    check(e.selection().extent() == 6, "光标落在中间缩进行");

    // 选区被键入替换
    CodeEditorController s("hello");
    s.setSelectionImmediately(0, 5);
    s.insertTypedText("x");
    check(s.text() == "x" && s.selection().isCollapsed(), "选区被输入替换");

    // UTF-8 退格整码点
    CodeEditorController u("\xe4\xbd\xa0\xe5\xa5\xbd");  // 你好
    u.deleteBackward();
    check(u.text() == "\xe4\xbd\xa0" && u.text().size() == 3,
          "Backspace 删整码点（你好 → 你）");

    // replaceRange 重映射旧光标
    CodeEditorController r("abcdef");
    r.setSelectionImmediately(2, 2);
    r.replaceRange(0, 2, "Q", /*preserveOldCursor=*/true);
    check(r.text() == "Qcdef", "replaceRange 替换区间");
    check(r.selection().extent() == 1, "preserveOldCursor 按位移重映射光标");

    // 选区复制 / 粘贴走共享剪贴板
    CodeEditorController b("abc");
    b.setSelectionImmediately(0, 3);
    b.copySelection();
    check(detail::clipboardBuf() == "abc", "copySelection 写入共享剪贴板");
    b.setSelectionImmediately(0, 0);
    b.pasteClipboard();
    check(b.text() == "abcabc", "pasteClipboard 在光标处插入");
}

// ── 3：撤销 / 重做（500ms 合并、换行断开、Compound） ───────────────────────
void testUndoRedo() {
    std::printf("[test] 撤销 / 重做与快速输入合并\n");

    CodeEditorController c("");
    c.insertTypedText("a");
    c.insertTypedText("b");
    c.insertTypedText("c");
    check(c.text() == "abc", "连续键入");
    check(c.undoController()->undoStackSize() == 1, "快速连续键入合并为一条历史");
    check(c.undo(), "undo 成功");
    check(c.text().empty(), "一次撤销撤掉整段连续输入");
    check(c.undoController()->redoStackSize() == 1, "redo 栈有记录");
    check(c.redo() && c.text() == "abc", "redo 恢复");

    CodeEditorController n("");
    n.insertTypedText("a");
    n.insertTypedText("\n");
    n.insertTypedText("b");
    check(n.undoController()->undoStackSize() == 3, "换行不参与合并（3 条历史）");

    // groupEdits=false：每次输入都单独成条
    auto cc = cui::make_shared<CodeEditorController>(std::string());
    cc->setUndoController(cui::make_shared<CodeEditorUndoController>(1000, false));
    cc->insertTypedText("x");
    cc->insertTypedText("y");
    check(cc->undoController()->undoStackSize() == 2, "groupEdits=false 不合并");

    // Compound：多步编辑收成一个撤销单元
    auto cp = cui::make_shared<CodeEditorController>(std::string());
    {
        auto handle = cp->undoController()->beginCompoundOperation();
        cp->insertTypedText("1");
        cp->insertTypedText(" ");  // 空格断开合并
        cp->insertTypedText("2");
        handle.end();
    }
    check(cp->text() == "1 2", "Compound 内编辑正常落字");
    check(cp->undoController()->undoStackSize() == 1, "Compound 收成一条历史");
    check(cp->undo() && cp->text().empty(), "撤销 Compound 还原全部");
}

// ── 4：行操作（移动 / 复制 / 缩进 / 自动检测） ────────────────────────────
void testLineOperations() {
    std::printf("[test] 行操作\n");

    CodeEditorController c("one\ntwo\nthree");
    c.setSelectionImmediately(0, 0);
    c.moveLineDown();
    check(c.text() == "two\none\nthree", "moveLineDown 交换相邻两行");
    check(c.caretLine() == 1, "光标跟随被移动的行");
    c.moveLineUp();
    check(c.text() == "one\ntwo\nthree", "moveLineUp 还原");

    CodeEditorController d("abc\ndef");
    d.setSelectionImmediately(1, 1);
    d.duplicateLine();
    check(d.text() == "abc\nabc\ndef", "Ctrl+D 复制当前行");
    check(d.caretLine() == 1 && d.selection().extent() == 4, "复制后光标在副本行首");

    // 多行块移动
    CodeEditorController m("a\nb\nc\nd");
    m.setSelectionImmediately(2, 5);  // 选中 "b\nc"
    m.moveLineUp();
    check(m.text() == "b\nc\na\nd", "多行块整体上移");
    check(m.selection().start() == 0 && m.selection().end() == 3,
          "多行块选区跟随上移");
    m.moveLineDown();
    check(m.text() == "a\nb\nc\nd", "多行块整体下移还原");

    CodeEditorController i("a\nb");
    i.selectAll();
    i.indentSelection();
    check(i.text() == "    a\n    b", "Tab 给选中行加一级缩进");
    i.unindentSelection();
    check(i.text() == "a\nb", "Shift+Tab 收回缩进");

    CodeEditorController det("void f() {\n    int x;\n    int y;\n}");
    check(det.detectIndentation() == 4, "detectIndentation 识别 4 空格");
    CodeEditorController det2("if (x):\n  a\n  b");
    check(det2.detectIndentation() == 2, "detectIndentation 识别 2 空格");

    CodeEditorController re("\ta");
    re.reindentDocument(2);
    // 旧宽度取 detectIndentation()（单行无缩进差 ⇒ 回退 tabSize 4）：
    // 一个制表符 = 4 列 = 1 级，按新宽度 2 重写 ⇒ "  a"。
    check(re.text() == "  a", "reindentDocument 按新宽度重写前导空白");
    check(re.tabSize() == 2, "reindentDocument 同时更新 tabSize");
}

// ── 5：折叠 ────────────────────────────────────────────────────────────────
void testFolding() {
    std::printf("[test] 折叠检测与可见行\n");

    const std::vector<std::string> lines = {
        "int f() {", "    if (x) {", "        g();", "    }", "}", ""};
    auto folds = detectCodeFoldRanges(lines, 4);
    check(folds.count(0) == 1 && folds[0].endIndex == 4, "外层花括号折叠到匹配行");
    check(folds.count(1) == 1 && folds[1].endIndex == 3, "内层花括号独立成区间");

    auto py = detectCodeFoldRanges({"def f():", "    a", "    b", "x = 1"}, 4);
    check(py.count(0) == 1 && py[0].endIndex == 2, "Python 冒号缩进块折叠");

    CodeEditorController c("int f() {\n    if (x) {\n        g();\n    }\n}\n");
    c.setFoldings(folds, false);
    c.toggleFold(0);
    check(c.visibleLineIndices() == std::vector<int>({0, 5}),
          "折叠外层后只剩首行与末尾空行");
    check(c.isLineInFoldedRegion(3), "隐藏行落在折叠区");
    check(c.getFoldStartForLine(3) == 0, "隐藏行回溯到折叠首行");
    c.toggleFold(0);
    check(c.visibleLineIndices().size() == 6, "展开恢复全部行");

    c.toggleFold(1);
    check(c.isLineInFoldedRegion(2) && c.isLineInFoldedRegion(3),
          "内层区间隐藏 2..3 行");
    check(!c.isLineInFoldedRegion(4), "区间外的行不受内层折叠影响");
    c.foldAll();
    check(c.visibleLineIndices() == std::vector<int>({0, 5}),
          "foldAll 只保留顶层区间（嵌套的折上后由父区间恢复）");
    c.unfoldAll();
    check(c.visibleLineIndices().size() == 6, "unfoldAll 全部展开");

    // 光标向下跳过折叠块
    c.toggleFold(0);
    c.setSelectionImmediately(0, 0);
    c.moveDown();
    check(c.caretLine() == 5, "下移跳过整个折叠区（0 → 5）");
}

// ── 6：查找 / 替换 ─────────────────────────────────────────────────────────
void testFind() {
    std::printf("[test] 查找与替换\n");

    auto code = cui::make_shared<CodeEditorController>("alpha beta alpha beta\nALPHA");
    auto find = cui::make_shared<CodeEditorFindController>(code);
    find->find("alpha", false);
    check(find->matchCount() == 3, "大小写不敏感默认命中 3 处");
    check(find->currentMatchIndex() == 0, "初始当前命中为第一处");
    check(find->matches().size() == 3 && find->matches()[0].isCurrentMatch,
          "matches() 带当前项标记");

    find->setCaseSensitive(true);
    check(find->matchCount() == 2, "区分大小写后 2 处");
    find->setWholeWord(true);
    check(find->matchCount() == 2, "整词过滤不影响独立单词");

    find->previous();
    check(find->currentMatchIndex() == 1, "previous 循环到末尾");
    find->next();
    check(find->currentMatchIndex() == 0, "next 绕回开头");

    auto w = cui::make_shared<CodeEditorController>("cat category cat");
    auto wf = cui::make_shared<CodeEditorFindController>(w);
    wf->find("cat", false);
    check(wf->matchCount() == 3, "未开整词时子串也命中");
    wf->setWholeWord(true);
    check(wf->matchCount() == 2, "整词模式排除 category 内部命中");

    auto rx = cui::make_shared<CodeEditorController>("a1 b22 c333");
    auto rf = cui::make_shared<CodeEditorFindController>(rx);
    rf->setIsRegex(true);
    rf->find("\\d+", false);
    check(rf->matchCount() == 3, "正则命中三段数字");
    rf->find("[", false);
    check(rf->matchCount() == 0, "非法正则不抛错、结果为空");

    // 替换
    auto rep = cui::make_shared<CodeEditorController>("alpha alpha");
    auto repf = cui::make_shared<CodeEditorFindController>(rep);
    repf->find("alpha", false);
    repf->replace("X");
    check(rep->text() == "X alpha", "replace 只换当前命中");
    check(repf->matchCount() == 1, "替换后重新搜索剩余命中");
    repf->replaceAll("Y");
    check(rep->text() == "X Y" && repf->matchCount() == 0, "replaceAll 一次换完");

    // 文本变化驱动重新搜索
    rep->insertTypedText("\nZ");
    check(repf->matchCount() == 0 && rep->text().find("Z") != std::string::npos,
          "键入后查找随文本刷新");

    // 监听器摘除
    const std::size_t before = rep->listenerCount();
    repf.reset();
    check(rep->listenerCount() == before - 1, "FindController 析构摘掉 code 监听");
}

// ── 7：控件挂载 / 焦点键路由 / IME / 监听器卫生 ────────────────────────────
void testWidgetMount() {
    std::printf("[test] 控件挂载、焦点键路由与监听器摘除\n");

    auto ctrl = cui::make_shared<CodeEditorController>("abc");
    auto find = cui::make_shared<CodeEditorFindController>(ctrl);
    auto focus = cui::make_shared<FocusNode>();

    {
        auto editor = cui::make_shared<CodeEditor>(
            ctrl, "demo.cpp", nullptr, 12.5, std::optional<double>(160.0), 320.0, true,
            4, CodeLanguage::Auto, true, /*enableFolding=*/true, /*readOnly=*/false,
            /*showTitleBar=*/true, std::nullopt, find, nullptr, focus);
        Harness h;
        h.attach(cui::make_shared<Align>(0.0, 0.0, editor), 800, 600);

        check(ctrl->listenerCount() == 2, "挂载后 controller 上有状态 + 查找两条监听");
        focus->requestFocus();
        check(focus->hasFocus(), "点击 / requestFocus 后获得焦点");

        focus->onKeyEvent(KeyEvent{0, "x", 0, false});
        check(ctrl->text() == "abcx", "键事件经焦点路由写入 controller");
        focus->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::Home), "", 0, false});
        focus->onKeyEvent(KeyEvent{0, "Y", 0, false});
        check(ctrl->text() == "Yabcx", "Home 后插入");
        focus->onKeyEvent(KeyEvent{static_cast<int>(KeyCode::Backspace), "", 0, false});
        check(ctrl->text() == "abcx", "Backspace 删除");

        // IME 提交（Backspace 后光标在 0，提交插到开头）
        focus->onIMCommit("\xe4\xbd\xa0");  // 你
        check(ctrl->text() == "\xe4\xbd\xa0" "abcx", "onIMCommit 插入 UTF-8 文本");

        // Ctrl+A / Ctrl+C 走共享剪贴板
        focus->onKeyEvent(KeyEvent{0, "a", ModCtrl, false});
        focus->onKeyEvent(KeyEvent{0, "c", ModCtrl, false});
        check(detail::clipboardBuf() == ctrl->text(), "Ctrl+A / Ctrl+C 复制全文");

        h.pump();
        check(ctrl->text().find("\xe4\xbd\xa0") != std::string::npos,
              "编辑后重建 + 布局不崩");
    }

    check(ctrl->listenerCount() == 1, "卸载后控件状态监听已摘除");
    find.reset();
    check(ctrl->listenerCount() == 0, "FindController 释放后 controller 无监听");

    // 旧入口（行数组）默认只读，并保留 highlightLine 语义
    CodeEditor legacy(std::vector<std::string>{"a", "b"}, "x.cpp", nullptr, 12.5,
                      std::optional<int>(1), std::optional<double>(80.0));
    check(legacy.readOnly(), "行数组入口默认只读");
    check(legacy.controller()->text() == "a\nb", "行数组入口拼回文本");
    check(legacy.controller()->caretLine() == 1, "highlightLine 落到对应行");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== CodeEditor (code_forge alignment) ==\n");
    testDocument();
    testTyping();
    testUndoRedo();
    testLineOperations();
    testFolding();
    testFind();
    testWidgetMount();
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
