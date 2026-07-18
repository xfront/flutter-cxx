#pragma once
/// @file widgets/code_editor.h
/// @brief 代码编辑器（对齐 code_forge 11.7.0 的公开能力面）。
///
/// 对应上游源码（https://pub.dev/packages/code_forge）：
///   - `CodeForgeController`（文档 / 选区 / 撤销重做 / 行操作 / 折叠 / 查找）
///   - `UndoRedoController` + `EditOperation`（code_forge_undo_redo.dart）
///   - `FindController`（code_forge_find_controller.dart）
///   - `CodeForge` 控件（code_forge_code_area.dart）
///
/// 分层：
///   - `CodeEditorController`：纯模型（不依赖渲染），持有文本、字节偏移选区、
///     撤销历史、折叠区间与查找高亮。
///   - `CodeEditorUndoController`：撤销/重做栈 + 快速输入合并（≤500ms）。
///   - `CodeEditorFindController`：搜索、上下跳转、替换。
///   - `CodeEditor`：三层控件（焦点 / 光标 / 选区 / 键盘 / IME / 滚动跟随）。
///
/// 与上游的有意偏离：
///   1. 上游 Dart 的 offset 是 UTF-16 code unit；本仓字符串是 UTF-8，按**字节**
///      计偏移，左右移动以 UTF-8 码点为单位（不会切开多字节字符）。
///   2. 上游把 controller 的选区与滚动位置耦合在 ScrollPosition；本仓控制器只
///      通过 `setScrollCallback` 通知视图「光标行变了」，滚动跟随由视图做。
///   3. `CodeEditorTheme`、词法着色与行号槽沿用本仓既有的只读查看器实现。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/theme.h"
#include "cui/framework/focus.h"
#include "cui/framework/framework.h"
#include "cui/widgets/scroll.h"
#include "cui/widgets/text_input.h"

namespace cui {

// ── 主题 ──────────────────────────────────────────────────────────────────────
/// 编辑器配色（参考 code_forge 的 `CodeForgeTheme`：编辑器主题与 App 主题
/// 解耦）。默认值全部从当前 `theme()` 派生，切换明暗主题时无需重新构造。
struct CodeEditorTheme {
    Color background;
    Color gutterBackground;
    Color gutterForeground;
    Color gutterActiveForeground;
    Color activeLine;
    Color indentGuide;
    Color border;
    Color titleBackground;
    Color titleForeground;
    Color titleDot;
    Color badgeBackground;
    Color badgeForeground;
    Color text;
    Color keyword;
    Color type;
    Color string;
    Color number;
    Color comment;
    Color preprocessor;
    Color function;
    // code_forge 的编辑态配色
    Color selection;      ///< 选区底色（上游 `selectionColor`）
    Color caret;          ///< 光标（上游 `cursorColor`）
    Color searchMatch;    ///< 查找命中底色
    Color searchCurrent;  ///< 当前命中底色
    Color foldIcon;       ///< 折叠图标

    static CodeEditorTheme fromApp() {
        CodeEditorTheme t;
        t.background = theme().surface;
        t.gutterBackground = theme().surfaceVariant;
        t.gutterForeground = theme().textDisabled;
        t.gutterActiveForeground = theme().primary;
        t.activeLine = theme().hoverOverlay;
        t.indentGuide = theme().divider;
        t.border = theme().border;
        t.titleBackground = theme().surfaceVariant;
        t.titleForeground = theme().textSecondary;
        t.titleDot = theme().primary;
        t.badgeBackground = theme().primaryContainer;
        t.badgeForeground = theme().onPrimaryContainer;
        t.text = theme().textPrimary;
        t.keyword = theme().primary;
        t.type = theme().tertiary;
        t.string = theme().success;
        t.number = theme().warning;
        t.comment = theme().textDisabled;
        t.preprocessor = theme().secondary;
        t.function = theme().secondary;
        t.selection = colorWithOpacity(theme().primary, 0.22);
        t.caret = theme().primary;
        t.searchMatch = colorWithOpacity(theme().warning, 0.35);
        t.searchCurrent = colorWithOpacity(theme().warning, 0.62);
        t.foldIcon = theme().textSecondary;
        return t;
    }
};

/// 词法语言。`Auto` 按文件扩展名推断。
enum class CodeLanguage {
    Auto,
    Cpp,
    C,
    Python,
    Rust,
    JavaScript,
    TypeScript,
    Json,
    Plain
};

// ── 折叠 ──────────────────────────────────────────────────────────────────────
/// 可折叠区间：隐藏 `startIndex + 1 .. endIndex`（首行保留可见，上游同款）。
struct CodeFoldRange {
    int startIndex = 0;
    int endIndex = 0;
    bool isFolded = false;
    /// 折叠本区间时顺带折上的嵌套区间（展开时原样恢复）。
    std::vector<int> originallyFoldedChildren;

    bool containsLine(int line) const { return line > startIndex && line <= endIndex; }
};

/// 查找高亮（对齐上游 `SearchHighlight`）。
struct CodeSearchHighlight {
    std::size_t start = 0;
    std::size_t end = 0;
    bool isCurrentMatch = false;
};

// ── 撤销 / 重做 ───────────────────────────────────────────────────────────────
enum class CodeEditKind { Insert, Delete, Replace, Compound };

/// 一次编辑操作（上游 `EditOperation` 的 sealed 类族压平为一个值类型）。
class CodeEditOperation {
public:
    CodeEditKind kind = CodeEditKind::Insert;
    std::size_t offset = 0;
    std::string text;          ///< Insert / Delete 的文本
    std::string deletedText;   ///< Replace
    std::string insertedText;  ///< Replace
    std::vector<CodeEditOperation> operations;  ///< Compound
    TextSelection selectionBefore;
    TextSelection selectionAfter;
    int64_t timestampMs = 0;

    static CodeEditOperation makeInsert(std::size_t offset, std::string text,
                                        TextSelection before, TextSelection after);
    static CodeEditOperation makeDelete(std::size_t offset, std::string text,
                                        TextSelection before, TextSelection after);
    static CodeEditOperation makeReplace(std::size_t offset, std::string deleted,
                                         std::string inserted, TextSelection before,
                                         TextSelection after);

    /// 逆操作（撤销时应用）。
    CodeEditOperation inverse() const;
    /// 是否可以与紧随其后的操作合并（上游 500ms 快速输入合并）。
    bool canMergeWith(const CodeEditOperation& other) const;
    /// 合并两个操作（时间戳取后者）。
    CodeEditOperation mergedWith(const CodeEditOperation& other) const;
};

/// 撤销/重做栈（上游 `UndoRedoController`）。
class CodeEditorUndoController : public ChangeNotifier {
public:
    using ApplyCallback = std::function<void(const CodeEditOperation&)>;

    explicit CodeEditorUndoController(int maxStackSize = 1000, bool groupEdits = true);
    ~CodeEditorUndoController() override = default;

    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    int undoStackSize() const { return static_cast<int>(undoStack_.size()); }
    int redoStackSize() const { return static_cast<int>(redoStack_.size()); }
    bool isApplying() const { return applying_; }
    int maxStackSize() const { return maxStackSize_; }
    bool groupEdits() const { return groupEdits_; }

    const std::vector<CodeEditOperation>& undoStack() const { return undoStack_; }
    const std::vector<CodeEditOperation>& redoStack() const { return redoStack_; }

    void setApplyEditCallback(ApplyCallback cb) { apply_ = std::move(cb); }

    /// 记录一次编辑（会先清空 redo 栈，再尝试与栈顶合并）。
    void recordEdit(const CodeEditOperation& operation);

    bool undo();
    bool redo();
    void clear();

    /// 开始一个合并操作：期间记录的编辑在 `end()` 时收成一个撤销单元。
    class CompoundHandle {
    public:
        CompoundHandle() = default;
        explicit CompoundHandle(CodeEditorUndoController* owner);
        CompoundHandle(CompoundHandle&& other) noexcept;
        CompoundHandle& operator=(CompoundHandle&& other) noexcept;
        CompoundHandle(const CompoundHandle&) = delete;
        CompoundHandle& operator=(const CompoundHandle&) = delete;
        ~CompoundHandle() { end(); }

        /// 收尾（幂等）。
        void end();

    private:
        CodeEditorUndoController* owner_ = nullptr;
        int startStackSize_ = 0;
        bool ended_ = true;
    };

    CompoundHandle beginCompoundOperation();

private:
    std::vector<CodeEditOperation> undoStack_;
    std::vector<CodeEditOperation> redoStack_;
    int maxStackSize_ = 1000;
    bool groupEdits_ = true;
    bool applying_ = false;
    bool suppressNextMerge_ = false;
    ApplyCallback apply_;
};

// ── 文档 / 选区控制器 ────────────────────────────────────────────────────────
/// 代码编辑器控制器（上游 `CodeForgeController` 的文档与编辑子集）。
///
/// 所有 offset 都是 UTF-8 **字节**偏移；`TextSelection` 的 base/extent 语义
/// 与 TextEditingController 一致。
class CodeEditorController : public ChangeNotifier {
public:
    explicit CodeEditorController(std::string text = "");
    ~CodeEditorController() override;

    static int64_t nowMs();

    // ── 文档 ────────────────────────────────────────────────────────────────
    const std::string& text() const { return text_; }
    /// 整体替换文本：清空撤销历史，光标落到文档末尾。
    void setText(const std::string& text);
    std::size_t length() const { return text_.size(); }
    int version() const { return version_; }

    const std::vector<std::string>& lines() const { return lines_; }
    int lineCount() const { return static_cast<int>(lines_.size()); }
    std::string getLineText(int line) const;
    int getLineAtOffset(std::size_t offset) const;
    std::size_t getLineStartOffset(int line) const;
    std::size_t findLineStart(std::size_t offset) const;
    std::size_t findLineEnd(std::size_t offset) const;

    /// `line` 行内、列 `character`（字节列）处的绝对偏移。
    std::size_t getOffsetAt(int line, int character) const;

    // ── 选区 ────────────────────────────────────────────────────────────────
    const TextSelection& selection() const { return selection_; }
    std::size_t caretOffset() const { return selection_.extent(); }
    int caretLine() const;
    /// 光标在行内的可视列（制表符按 tabSize 展开，0 基）。
    int caretColumn() const;
    /// 设置选区并请求滚动跟随光标。
    void setSelection(TextSelection selection);
    /// 设置选区但不请求滚动跟随。
    void setSelectionSilently(TextSelection selection);
    /// 立即设选区（上游 `setSelectionImmediately`，不触发滚动）。
    void setSelectionImmediately(std::size_t start, std::size_t end);
    void selectAll();

    // ── 编辑 ────────────────────────────────────────────────────────────────
    /// 替换 [start, end) 为 replacement，并记录一次可撤销操作。
    /// `preserveOldCursor` 为 true 时按位移重映射旧选区（上游同款）。
    void replaceRange(std::size_t start, std::size_t end,
                      const std::string& replacement, bool preserveOldCursor = false);
    void insertAtCurrentCursor(const std::string& text);
    void insertText(int line, int character, const std::string& text);
    /// 键盘输入：自动配对括号/引号、回车自动缩进、在配对的括号间换行劈开。
    void insertTypedText(const std::string& text);
    void deleteBackward();
    void deleteForward();
    void deleteWordBackward();
    void deleteWordForward();
    std::string selectedText() const;
    void copySelection();
    void cutSelection();
    void pasteClipboard();

    // ── 光标移动 ────────────────────────────────────────────────────────────
    void moveLeft(bool extend = false);
    void moveRight(bool extend = false);
    void moveUp(bool extend = false);
    void moveDown(bool extend = false);
    /// 上下移动 n 行（PageUp/PageDown 由视图换算成行数后调用）。
    void movePage(int lines, bool extend = false);
    void moveToLineStart(bool extend = false);
    void moveToLineEnd(bool extend = false);
    void moveToDocumentStart(bool extend = false);
    void moveToDocumentEnd(bool extend = false);
    void moveWordLeft(bool extend = false);
    void moveWordRight(bool extend = false);

    // ── 行操作 ──────────────────────────────────────────────────────────────
    void moveLineUp();
    void moveLineDown();
    void duplicateLine();
    void indentSelection();
    void unindentSelection();
    /// 自动检测缩进宽度（对齐上游 `detectIndentation`）。
    int detectIndentation() const;
    /// 按新 tabSize 重写全文前导空白（上游 `reindentDocument`）。
    void reindentDocument(int newTabSize);

    // ── 折叠 ────────────────────────────────────────────────────────────────
    const std::map<int, CodeFoldRange>& foldings() const { return foldings_; }
    void setFoldings(std::map<int, CodeFoldRange> ranges, bool notify = true);
    void recomputeFoldings();
    void toggleFold(int line);
    void foldAll();
    void unfoldAll();
    bool isLineInFoldedRegion(int line) const;
    /// 命中折叠隐藏区的行 ⇒ 返回所属折叠的首行；否则 -1。
    int getFoldStartForLine(int line) const;
    /// 当前可见的行号（跳过被折叠隐藏的行）。
    std::vector<int> visibleLineIndices() const;

    // ── 查找高亮（由 CodeEditorFindController 写入）─────────────────────────
    const std::vector<CodeSearchHighlight>& searchHighlights() const {
        return searchHighlights_;
    }
    void setSearchHighlights(std::vector<CodeSearchHighlight> highlights);
    void clearSearchHighlights();

    // ── 杂项 ────────────────────────────────────────────────────────────────
    /// 光标前的标识符前缀（自动补全用；对齐上游 `getCurrentWordPrefixAt`）。
    std::string getCurrentWordPrefixAt(std::size_t offset) const;

    // ── 撤销 / 重做 ─────────────────────────────────────────────────────────
    SharedPtr<CodeEditorUndoController> undoController() const { return undoController_; }
    void setUndoController(SharedPtr<CodeEditorUndoController> controller);
    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    /// 由视图（或外部）显式记录一条历史（不改变文本）。
    void recordEdit(const CodeEditOperation& operation);

    // ── 制表符 ──────────────────────────────────────────────────────────────
    int tabSize() const { return tabSize_; }
    void setTabSize(int value);
    bool useSpaceAsTab() const { return useSpaceAsTab_; }
    void setUseSpaceAsTab(bool value);
    /// 一次缩进插入的字符串（空格 × tabSize 或单个 '\t'）。
    std::string tabSpace() const;

    // ── 滚动跟随 ────────────────────────────────────────────────────────────
    /// 视图注册「把某行带入可视区」的回调。
    void setScrollCallback(std::function<void(int line)> cb);
    void scrollToLine(int line);

private:
    void rebuildLines();
    void requestScrollToCaret();
    void applyOperation(const CodeEditOperation& op);
    void editRange(std::size_t start, std::size_t end, const std::string& replacement,
                   const TextSelection& after, bool record = true);
    void moveCaretTo(std::size_t offset, bool extend);
    void moveVertical(int deltaLines, bool extend);
    std::size_t lineOffsetAtVisualColumn(int line, int column) const;
    int visualColumnAtOffset(std::size_t offset) const;
    int lineFirstSelected(int firstLine, int lastLine) const;
    int lineLastSelected(int firstLine, int lastLine) const;
    void bindUndoController();

    std::string text_;
    std::vector<std::string> lines_;
    std::vector<std::size_t> lineStarts_;
    TextSelection selection_;
    int version_ = 0;
    int tabSize_ = 4;
    bool useSpaceAsTab_ = true;
    std::map<int, CodeFoldRange> foldings_;
    std::vector<CodeSearchHighlight> searchHighlights_;
    SharedPtr<CodeEditorUndoController> undoController_;
    std::function<void(int)> scrollCallback_;
};

// ── 查找 ──────────────────────────────────────────────────────────────────────
/// 查找控制器（对齐上游 `FindController`）。
class CodeEditorFindController : public ChangeNotifier {
public:
    explicit CodeEditorFindController(SharedPtr<CodeEditorController> codeController);
    ~CodeEditorFindController() override;

    int matchCount() const { return static_cast<int>(matches_.size()); }
    int currentMatchIndex() const { return currentMatchIndex_; }
    const std::string& query() const { return lastQuery_; }

    bool caseSensitive() const { return caseSensitive_; }
    void setCaseSensitive(bool value);
    bool isRegex() const { return isRegex_; }
    void setIsRegex(bool value);
    bool wholeWord() const { return wholeWord_; }
    void setWholeWord(bool value);
    bool isActive() const { return isActive_; }
    void setIsActive(bool value);
    bool isReplaceMode() const { return isReplaceMode_; }
    void setIsReplaceMode(bool value);
    void toggleReplaceMode() { setIsReplaceMode(!isReplaceMode_); }
    void toggleCaseSensitive() { setCaseSensitive(!caseSensitive_); }
    void toggleRegex() { setIsRegex(!isRegex_); }
    void toggleWholeWord() { setWholeWord(!wholeWord_); }

    /// 执行搜索；`scrollToMatch` 为 true 时把当前命中带入可视区。
    void find(const std::string& query, bool scrollToMatch = true);
    void next();
    void previous();
    void clear();
    /// 替换当前命中（无选中命中时忽略）。
    void replace(const std::string& replacement);
    /// 一次性替换全部命中（（合成一次编辑）。
    void replaceAll(const std::string& replacement);

    /// 命中区间（测试/UI 用；`isCurrentMatch` 标出当前项）。
    const std::vector<CodeSearchHighlight>& matches() const { return highlights_; }

private:
    struct MatchRange {
        std::size_t start = 0;
        std::size_t end = 0;
    };

    std::vector<MatchRange> runSearch(const std::string& query) const;
    void updateHighlights(bool notify);
    void reSearch(bool scrollToMatch);
    void onCodeChanged();

    SharedPtr<CodeEditorController> code_;
    std::vector<MatchRange> matches_;
    std::vector<CodeSearchHighlight> highlights_;
    int currentMatchIndex_ = -1;
    bool caseSensitive_ = false;
    bool isRegex_ = false;
    bool wholeWord_ = false;
    bool isActive_ = false;
    bool isReplaceMode_ = false;
    std::string lastQuery_;
    std::string lastText_;
    int codeListenerId_ = -1;
};

// ── 折叠检测 ──────────────────────────────────────────────────────────────────
/// 扫描源码得到可折叠区间（`{}`/`[]`/`()` 跨行配对 + Python 风格 `:` 缩进块）。
std::map<int, CodeFoldRange> detectCodeFoldRanges(const std::vector<std::string>& lines,
                                                  int tabSize);

// ── 控件 ──────────────────────────────────────────────────────────────────────
class CodeEditorState;

/// 代码编辑器。两种入口：
///   - 控制器入口（可编辑）：`CodeEditor(controller, filename, …)`
///   - 行文本入口（兼容既有只读查看器）：`CodeEditor(lines, filename, …)`
///     该入口默认 `readOnly = true`、`enableFolding = false`，参数顺序与旧版一致。
///
/// `width <= 0` 表示「水平占满父级可用宽度」（build 期经 LayoutBuilder 取
/// 父约束；默认 420 保持旧调用点不变）。`height` 只约束代码区，标题栏另计。
class CodeEditor : public StatefulWidget {
public:
    /// 控制器入口。
    explicit CodeEditor(SharedPtr<CodeEditorController> controller,
                        std::string filename = "", KeyPtr key = nullptr,
                        double fontSize = 12.5,
                        std::optional<double> height = std::nullopt,
                        double width = 420.0, bool showLineNumbers = true,
                        int tabSize = 4,
                        CodeLanguage codeLanguage = CodeLanguage::Auto,
                        bool showIndentGuides = true, bool enableFolding = true,
                        bool readOnly = false, bool showTitleBar = true,
                        std::optional<CodeEditorTheme> editorTheme = std::nullopt,
                        SharedPtr<CodeEditorFindController> findController = nullptr,
                        SharedPtr<CodeEditorUndoController> undoController = nullptr,
                        SharedPtr<FocusNode> focusNode = nullptr,
                        std::string language = "");

    /// 兼容入口：从行数组构造内部控制器（默认只读）。
    explicit CodeEditor(std::vector<std::string> lines, std::string filename = "",
                        KeyPtr key = nullptr, double fontSize = 12.5,
                        std::optional<int> highlightLine = std::nullopt,
                        std::optional<double> height = std::nullopt,
                        double width = 420.0, bool showLineNumbers = true,
                        int tabSize = 4, std::string language = "",
                        CodeLanguage codeLanguage = CodeLanguage::Auto,
                        bool showIndentGuides = true,
                        std::optional<CodeEditorTheme> editorTheme = std::nullopt,
                        bool enableFolding = false, bool readOnly = true);

    SharedPtr<CodeEditorController> controller() const { return controller_; }
    const SharedPtr<CodeEditorFindController>& findController() const { return findController_; }
    const SharedPtr<CodeEditorUndoController>& undoController() const { return undoController_; }
    const SharedPtr<FocusNode>& focusNode() const { return focusNode_; }

    const std::string& filename() const { return filename_; }
    double fontSize() const { return fontSize_; }
    const std::optional<int>& highlightLine() const { return highlightLine_; }
    const std::optional<double>& height() const { return height_; }
    double width() const { return width_; }
    bool showLineNumbers() const { return showLineNumbers_; }
    int tabSize() const { return tabSize_; }
    const std::string& language() const { return language_; }
    CodeLanguage codeLanguage() const { return codeLanguage_; }
    bool showIndentGuides() const { return showIndentGuides_; }
    bool enableFolding() const { return enableFolding_; }
    bool readOnly() const { return readOnly_; }
    bool showTitleBar() const { return showTitleBar_; }
    const std::optional<CodeEditorTheme>& editorTheme() const { return editorTheme_; }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "CodeEditor"; }

private:
    SharedPtr<CodeEditorController> controller_;
    SharedPtr<CodeEditorFindController> findController_;
    SharedPtr<CodeEditorUndoController> undoController_;
    SharedPtr<FocusNode> focusNode_;
    std::string filename_;
    double fontSize_ = 12.5;
    std::optional<int> highlightLine_;
    std::optional<double> height_;
    double width_ = 420.0;
    bool showLineNumbers_ = true;
    int tabSize_ = 4;
    std::string language_;
    CodeLanguage codeLanguage_ = CodeLanguage::Auto;
    bool showIndentGuides_ = true;
    bool enableFolding_ = true;
    bool readOnly_ = false;
    bool showTitleBar_ = true;
    std::optional<CodeEditorTheme> editorTheme_;
};

}  // namespace cui
