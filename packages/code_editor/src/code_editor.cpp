#include "cui/code_editor/code_editor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <regex>

#include "cui/framework/build_owner.h"
#include "cui/rendering/render_scroll.h"
#include "cui/widgets/container.h"  // Container / BoxDecoration / ClipRRect
#include "cui/widgets/layout.h"     // LayoutBuilder / Spacer
#include "cui/widgets/text.h"       // Text / RichText

namespace cui {

namespace {

bool isUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

std::size_t utf8Prev(const std::string& s, std::size_t offset) {
    if (offset == 0)
        return 0;
    std::size_t p = offset - 1;
    while (p > 0 && isUtf8Continuation(static_cast<unsigned char>(s[p])))
        --p;
    return p;
}

std::size_t utf8Next(const std::string& s, std::size_t offset) {
    if (offset >= s.size())
        return s.size();
    std::size_t p = offset + 1;
    while (p < s.size() && isUtf8Continuation(static_cast<unsigned char>(s[p])))
        ++p;
    return p;
}

bool isWordByte(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c >= 0x80;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i)
            out.push_back('\n');
        out += lines[i];
    }
    return out;
}

std::string escapeRegex(const std::string& text) {
    static const char* special = ".[]{}()*+?^$|\\";
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (std::strchr(special, c))
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string rtrimCopy(const std::string& s) {
    std::size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r'))
        --end;
    return s.substr(0, end);
}

int leadingIndentColumns(const std::string& line, int tabSize) {
    const int tab = std::max(1, tabSize);
    int columns = 0;
    for (char c : line) {
        if (c == ' ')
            ++columns;
        else if (c == '\t')
            columns += tab - (columns % tab);
        else
            break;
    }
    return columns;
}

std::size_t leadingWhitespaceBytes(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    return i;
}

bool isOpenBracket(char c) { return c == '(' || c == '[' || c == '{'; }
bool isCloseBracket(char c) { return c == ')' || c == ']' || c == '}'; }
char matchingClose(char c) {
    switch (c) {
    case '(':
        return ')';
    case '[':
        return ']';
    case '{':
        return '}';
    case '"':
        return '"';
    case '\'':
        return '\'';
    default:
        return 0;
    }
}

// ── 词法着色（沿用只读查看器的实现）─────────────────────────────────────────
const char* monoFamily() {
#if defined(_WIN32)
    return "Consolas";
#elif defined(__APPLE__)
    return "Menlo";
#else
    return "monospace";
#endif
}

const std::vector<std::string>& keywordsFor(CodeLanguage lang) {
    static const std::vector<std::string> cpp = {
        "alignas",    "alignof",  "asm",      "auto",     "break",    "case",
        "catch",      "class",    "const",    "constexpr", "const_cast", "continue",
        "decltype",   "default",  "delete",   "do",       "dynamic_cast", "else",
        "enum",       "explicit", "export",   "extern",   "false",    "for",
        "friend",     "goto",     "if",       "inline",   "mutable",  "namespace",
        "new",        "noexcept", "nullptr",  "operator", "override", "private",
        "protected",  "public",   "reinterpret_cast", "return", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this",
        "throw",      "true",     "try",      "typedef",  "typeid",   "typename",
        "union",      "using",    "virtual",  "volatile", "while"};
    static const std::vector<std::string> python = {
        "False",  "None",   "True",   "and",    "as",     "assert", "async",
        "await",  "break",  "case",   "class",  "continue", "def",  "del",
        "elif",   "else",   "except", "finally", "for",   "from",   "global",
        "if",     "import", "in",     "is",     "lambda", "match",  "nonlocal",
        "not",    "or",     "pass",   "raise",  "return", "try",    "while",
        "with",   "yield"};
    static const std::vector<std::string> rust = {
        "Self",  "as",    "async", "await", "break", "const", "continue",
        "crate", "dyn",   "else",  "enum",  "extern", "false", "fn",
        "for",   "if",    "impl",  "in",    "let",   "loop",  "match",
        "mod",   "move",  "mut",   "pub",   "ref",   "return", "self",
        "static", "struct", "super", "trait", "true", "type",  "unsafe",
        "use",   "where", "while", "yield"};
    static const std::vector<std::string> js = {
        "async",   "await",  "break", "case",  "catch",  "class",  "const",
        "continue", "debugger", "default", "delete", "do", "else", "export",
        "extends", "false",  "finally", "for", "function", "if",   "import",
        "in",      "instanceof", "let", "new", "null",   "of",     "return",
        "static",  "super",  "switch", "this", "throw",  "true",   "try",
        "typeof",  "undefined", "var", "void", "while", "with",   "yield"};
    static const std::vector<std::string> ts = {
        "abstract", "any",     "as",       "asserts", "async",  "await",
        "bigint",   "boolean", "break",    "case",    "catch",  "class",
        "const",    "continue", "declare", "default", "delete", "do",
        "else",     "enum",    "export",   "extends", "false",  "finally",
        "for",      "from",    "function", "if",      "implements", "import",
        "in",       "infer",   "instanceof", "interface", "keyof", "let",
        "namespace", "never",  "new",      "null",    "number", "object",
        "of",       "private", "protected", "public", "readonly", "return",
        "satisfies", "static", "string",   "super",   "switch", "symbol",
        "this",     "throw",   "true",     "try",     "type",   "typeof",
        "undefined", "unique", "unknown",  "var",     "void",   "while",
        "yield"};
    static const std::vector<std::string> json = {"false", "null", "true"};
    static const std::vector<std::string> none;
    switch (lang) {
    case CodeLanguage::Cpp:
    case CodeLanguage::C:
        return cpp;
    case CodeLanguage::Python:
        return python;
    case CodeLanguage::Rust:
        return rust;
    case CodeLanguage::JavaScript:
        return js;
    case CodeLanguage::TypeScript:
        return ts;
    case CodeLanguage::Json:
        return json;
    default:
        return none;
    }
}

const std::vector<std::string>& typesFor(CodeLanguage lang) {
    static const std::vector<std::string> cpp = {
        "bool",   "char",    "char8_t",  "char16_t", "char32_t", "double",
        "float",  "int",     "int8_t",   "int16_t",  "int32_t",  "int64_t",
        "long",   "short",   "signed",   "size_t",   "uint8_t",  "uint16_t",
        "uint32_t", "uint64_t", "unsigned", "void", "wchar_t"};
    static const std::vector<std::string> python = {
        "bool", "bytes", "dict", "float", "int", "list", "object", "set", "str",
        "tuple"};
    static const std::vector<std::string> rust = {
        "Option", "Result", "String", "Vec",   "bool", "char", "f32", "f64",
        "i8",     "i16",    "i32",    "i64",   "i128", "isize", "str", "u8",
        "u16",    "u32",    "u64",    "u128",  "usize"};
    static const std::vector<std::string> js = {"boolean", "number", "string"};
    static const std::vector<std::string> none;
    switch (lang) {
    case CodeLanguage::Cpp:
    case CodeLanguage::C:
        return cpp;
    case CodeLanguage::Python:
        return python;
    case CodeLanguage::Rust:
        return rust;
    case CodeLanguage::JavaScript:
        return js;
    default:
        return none;
    }
}

bool hasSlashComments(CodeLanguage lang) {
    return lang != CodeLanguage::Python && lang != CodeLanguage::Json &&
           lang != CodeLanguage::Plain;
}
bool hasBlockComments(CodeLanguage lang) { return hasSlashComments(lang); }
bool hasHashComments(CodeLanguage lang) { return lang == CodeLanguage::Python; }
bool hasPreprocessor(CodeLanguage lang) {
    return lang == CodeLanguage::Cpp || lang == CodeLanguage::C;
}

bool containsWord(const std::vector<std::string>& words, const std::string& s) {
    for (const auto& w : words)
        if (w == s)
            return true;
    return false;
}

CodeLanguage resolveLanguage(const std::string& filename, CodeLanguage requested) {
    if (requested != CodeLanguage::Auto)
        return requested;
    const std::size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot + 1 >= filename.size())
        return CodeLanguage::Plain;
    std::string ext = filename.substr(dot + 1);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" || ext == "hpp" ||
        ext == "inl")
        return CodeLanguage::Cpp;
    if (ext == "c")
        return CodeLanguage::C;
    if (ext == "py")
        return CodeLanguage::Python;
    if (ext == "rs")
        return CodeLanguage::Rust;
    if (ext == "js" || ext == "mjs" || ext == "cjs" || ext == "jsx")
        return CodeLanguage::JavaScript;
    if (ext == "ts" || ext == "tsx")
        return CodeLanguage::TypeScript;
    if (ext == "json")
        return CodeLanguage::Json;
    return CodeLanguage::Plain;
}

std::string languageLabel(CodeLanguage lang) {
    switch (lang) {
    case CodeLanguage::Cpp:
        return "C++";
    case CodeLanguage::C:
        return "C";
    case CodeLanguage::Python:
        return "Python";
    case CodeLanguage::Rust:
        return "Rust";
    case CodeLanguage::JavaScript:
        return "JavaScript";
    case CodeLanguage::TypeScript:
        return "TypeScript";
    case CodeLanguage::Json:
        return "JSON";
    default:
        return "";
    }
}

std::string expandTabs(const std::string& line, int tabSize) {
    const int width = std::max(1, tabSize);
    std::string out;
    out.reserve(line.size() + 8);
    for (char c : line) {
        if (c == '\t')
            out.append(static_cast<std::size_t>(width), ' ');
        else
            out.push_back(c);
    }
    return out;
}

/// 一行内字节偏移 → 展开制表符后的可视列。
int visualColumnInLine(const std::string& line, std::size_t byteOffset, int tabSize) {
    const int tab = std::max(1, tabSize);
    int columns = 0;
    for (std::size_t i = 0; i < byteOffset && i < line.size();) {
        if (line[i] == '\t') {
            columns += tab - (columns % tab);
            ++i;
        } else {
            ++columns;
            i = utf8Next(line, i);
        }
    }
    return columns;
}

int indentLevelOfLine(const std::string& line, int tabSize) {
    return leadingIndentColumns(line, tabSize) / std::max(1, tabSize);
}

enum class TokKind { Plain, Keyword, Type, String, Number, Comment, Preprocessor, Function };

struct Token {
    std::string text;
    TokKind kind;
};

TextStyle tokenStyle(TokKind kind, double size, const char* family,
                     const CodeEditorTheme& ed) {
    TextStyle ts = TextStyle().size(size).family(family);
    switch (kind) {
    case TokKind::Keyword:
        return ts.bold().fgColor(ed.keyword);
    case TokKind::Type:
        return ts.fgColor(ed.type);
    case TokKind::String:
        return ts.fgColor(ed.string);
    case TokKind::Number:
        return ts.fgColor(ed.number);
    case TokKind::Comment:
        return ts.setItalic().fgColor(ed.comment);
    case TokKind::Preprocessor:
        return ts.fgColor(ed.preprocessor);
    case TokKind::Function:
        return ts.fgColor(ed.function);
    case TokKind::Plain:
        break;
    }
    return ts.fgColor(ed.text);
}

void tokenizeLine(const std::string& raw, int tabSize, CodeLanguage lang,
                  bool& inBlockComment, std::vector<Token>& out) {
    const std::string line = expandTabs(raw, tabSize);
    const std::size_t n = line.size();
    const bool slashComments = hasSlashComments(lang);
    const bool blockComments = hasBlockComments(lang);
    const bool hashComments = hasHashComments(lang);
    const bool preprocessor = hasPreprocessor(lang);
    const auto& keywords = keywordsFor(lang);
    const auto& types = typesFor(lang);
    std::size_t i = 0;
    bool lineStart = true;
    auto push = [&](TokKind kind, std::size_t begin, std::size_t end) {
        if (end > begin)
            out.push_back({line.substr(begin, end - begin), kind});
    };

    while (i < n) {
        if (inBlockComment) {
            const std::size_t end = line.find("*/", i);
            if (end == std::string::npos) {
                push(TokKind::Comment, i, n);
                i = n;
            } else {
                push(TokKind::Comment, i, end + 2);
                i = end + 2;
                inBlockComment = false;
            }
            lineStart = false;
            continue;
        }

        const char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            const std::size_t begin = i;
            while (i < n && std::isspace(static_cast<unsigned char>(line[i])))
                ++i;
            push(TokKind::Plain, begin, i);
            continue;
        }
        if (slashComments && c == '/' && i + 1 < n && line[i + 1] == '/') {
            push(TokKind::Comment, i, n);
            i = n;
            break;
        }
        if (blockComments && c == '/' && i + 1 < n && line[i + 1] == '*') {
            inBlockComment = true;
            continue;
        }
        if (hashComments && c == '#') {
            push(TokKind::Comment, i, n);
            i = n;
            break;
        }
        if (preprocessor && c == '#' && lineStart) {
            push(TokKind::Preprocessor, i, n);
            i = n;
            break;
        }

        lineStart = false;

        if (c == '"' || c == '\'') {
            const char quote = c;
            const std::size_t begin = i++;
            while (i < n) {
                if (line[i] == '\\') {
                    i = std::min(i + 2, n);
                    continue;
                }
                if (line[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            }
            push(TokKind::String, begin, i);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t begin = i;
            while (i < n) {
                const char d = line[i];
                if (std::isalnum(static_cast<unsigned char>(d)) || d == '.' || d == '_' ||
                    d == '\'') {
                    ++i;
                    continue;
                }
                if ((d == '+' || d == '-') && i > begin &&
                    (line[i - 1] == 'e' || line[i - 1] == 'E')) {
                    ++i;
                    continue;
                }
                break;
            }
            push(TokKind::Number, begin, i);
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::size_t begin = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(line[i])) ||
                             line[i] == '_'))
                ++i;
            const std::string word = line.substr(begin, i - begin);
            TokKind kind = TokKind::Plain;
            if (containsWord(keywords, word))
                kind = TokKind::Keyword;
            else if (containsWord(types, word))
                kind = TokKind::Type;
            else if (i < n && line[i] == '(')
                kind = TokKind::Function;
            push(kind, begin, i);
            continue;
        }
        // 非 ASCII 字节在这里逐个落入：必须整码点切分，
        // 否则会把一个 UTF-8 字符拆成多个非法片段交给 RichText（Skia 崩溃）。
        const std::size_t next = utf8Next(line, i);
        push(TokKind::Plain, i, next);
        i = next;
    }
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════════════
// CodeEditOperation
// ══════════════════════════════════════════════════════════════════════════════
int64_t CodeEditorController::nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

CodeEditOperation CodeEditOperation::makeInsert(std::size_t offset, std::string text,
                                                TextSelection before,
                                                TextSelection after) {
    CodeEditOperation op;
    op.kind = CodeEditKind::Insert;
    op.offset = offset;
    op.text = std::move(text);
    op.selectionBefore = before;
    op.selectionAfter = after;
    op.timestampMs = CodeEditorController::nowMs();
    return op;
}

CodeEditOperation CodeEditOperation::makeDelete(std::size_t offset, std::string text,
                                                TextSelection before,
                                                TextSelection after) {
    CodeEditOperation op;
    op.kind = CodeEditKind::Delete;
    op.offset = offset;
    op.text = std::move(text);
    op.selectionBefore = before;
    op.selectionAfter = after;
    op.timestampMs = CodeEditorController::nowMs();
    return op;
}

CodeEditOperation CodeEditOperation::makeReplace(std::size_t offset, std::string deleted,
                                                 std::string inserted,
                                                 TextSelection before,
                                                 TextSelection after) {
    CodeEditOperation op;
    op.kind = CodeEditKind::Replace;
    op.offset = offset;
    op.deletedText = std::move(deleted);
    op.insertedText = std::move(inserted);
    op.selectionBefore = before;
    op.selectionAfter = after;
    op.timestampMs = CodeEditorController::nowMs();
    return op;
}

CodeEditOperation CodeEditOperation::inverse() const {
    if (kind == CodeEditKind::Insert) {
        CodeEditOperation op = makeDelete(offset, text, selectionAfter, selectionBefore);
        op.timestampMs = timestampMs;
        return op;
    }
    if (kind == CodeEditKind::Delete) {
        CodeEditOperation op = makeInsert(offset, text, selectionAfter, selectionBefore);
        op.timestampMs = timestampMs;
        return op;
    }
    if (kind == CodeEditKind::Replace) {
        CodeEditOperation op =
            makeReplace(offset, insertedText, deletedText, selectionAfter, selectionBefore);
        op.timestampMs = timestampMs;
        return op;
    }
    CodeEditOperation out;
    out.kind = CodeEditKind::Compound;
    out.operations.reserve(operations.size());
    for (auto it = operations.rbegin(); it != operations.rend(); ++it)
        out.operations.push_back(it->inverse());
    out.selectionBefore = selectionAfter;
    out.selectionAfter = selectionBefore;
    out.timestampMs = timestampMs;
    return out;
}

bool CodeEditOperation::canMergeWith(const CodeEditOperation& other) const {
    if (kind != other.kind)
        return false;
    const int64_t diff = other.timestampMs - timestampMs;
    if (std::llabs(diff) > 500)
        return false;

    if (kind == CodeEditKind::Insert) {
        if (other.offset != offset + text.size())
            return false;
        if (text.find('\n') != std::string::npos ||
            other.text.find('\n') != std::string::npos)
            return false;
        const bool thisEndsWithSpace =
            !text.empty() && (text.back() == ' ' || text.back() == '\t');
        const bool otherStartsWithSpace =
            !other.text.empty() && (other.text.front() == ' ' || other.text.front() == '\t');
        if (thisEndsWithSpace != otherStartsWithSpace && !text.empty() &&
            !other.text.empty())
            return false;
        return true;
    }

    if (kind == CodeEditKind::Delete) {
        if (text.find('\n') != std::string::npos ||
            other.text.find('\n') != std::string::npos)
            return false;
        if (other.offset + other.text.size() == offset)
            return true;
        if (other.offset == offset)
            return true;
        return false;
    }

    return false;
}

CodeEditOperation CodeEditOperation::mergedWith(const CodeEditOperation& other) const {
    if (kind != other.kind)
        return *this;

    if (kind == CodeEditKind::Insert) {
        CodeEditOperation out = makeInsert(offset, text + other.text, selectionBefore,
                                           other.selectionAfter);
        out.timestampMs = other.timestampMs;
        return out;
    }

    if (kind == CodeEditKind::Delete) {
        if (other.offset + other.text.size() == offset) {
            CodeEditOperation out = makeDelete(other.offset, other.text + text,
                                               selectionBefore, other.selectionAfter);
            out.timestampMs = other.timestampMs;
            return out;
        }
        if (other.offset == offset) {
            CodeEditOperation out = makeDelete(offset, text + other.text, selectionBefore,
                                               other.selectionAfter);
            out.timestampMs = other.timestampMs;
            return out;
        }
    }

    return *this;
}

// ══════════════════════════════════════════════════════════════════════════════
// CodeEditorUndoController
// ══════════════════════════════════════════════════════════════════════════════
CodeEditorUndoController::CodeEditorUndoController(int maxStackSize, bool groupEdits)
    : maxStackSize_(std::max(1, maxStackSize)), groupEdits_(groupEdits) {}

void CodeEditorUndoController::recordEdit(const CodeEditOperation& operation) {
    if (applying_)
        return;

    if (!redoStack_.empty())
        redoStack_.clear();

    if (groupEdits_ && !undoStack_.empty() && !suppressNextMerge_) {
        auto& last = undoStack_.back();
        if (last.canMergeWith(operation)) {
            last = last.mergedWith(operation);
            suppressNextMerge_ = false;
            notifyListeners();
            return;
        }
    }
    suppressNextMerge_ = false;

    undoStack_.push_back(operation);
    while (static_cast<int>(undoStack_.size()) > maxStackSize_)
        undoStack_.erase(undoStack_.begin());
    notifyListeners();
}

bool CodeEditorUndoController::undo() {
    if (undoStack_.empty() || !apply_)
        return false;

    CodeEditOperation operation = undoStack_.back();
    undoStack_.pop_back();

    applying_ = true;
    apply_(operation.inverse());
    applying_ = false;

    redoStack_.push_back(std::move(operation));
    notifyListeners();
    return true;
}

bool CodeEditorUndoController::redo() {
    if (redoStack_.empty() || !apply_)
        return false;

    CodeEditOperation operation = redoStack_.back();
    redoStack_.pop_back();

    applying_ = true;
    apply_(operation);
    applying_ = false;

    undoStack_.push_back(operation);
    notifyListeners();
    return true;
}

void CodeEditorUndoController::clear() {
    undoStack_.clear();
    redoStack_.clear();
    notifyListeners();
}

CodeEditorUndoController::CompoundHandle::CompoundHandle(CodeEditorUndoController* owner)
    : owner_(owner), startStackSize_(owner ? static_cast<int>(owner->undoStack_.size()) : 0),
      ended_(owner == nullptr) {
    if (owner_)
        owner_->suppressNextMerge_ = true;
}

CodeEditorUndoController::CompoundHandle::CompoundHandle(CompoundHandle&& other) noexcept
    : owner_(other.owner_), startStackSize_(other.startStackSize_), ended_(other.ended_) {
    other.owner_ = nullptr;
    other.ended_ = true;
}

CodeEditorUndoController::CompoundHandle& CodeEditorUndoController::CompoundHandle::operator=(
    CompoundHandle&& other) noexcept {
    if (this != &other) {
        end();
        owner_ = other.owner_;
        startStackSize_ = other.startStackSize_;
        ended_ = other.ended_;
        other.owner_ = nullptr;
        other.ended_ = true;
    }
    return *this;
}

void CodeEditorUndoController::CompoundHandle::end() {
    if (ended_ || !owner_)
        return;
    ended_ = true;

    auto& stack = owner_->undoStack_;
    const int start = std::min(startStackSize_, static_cast<int>(stack.size()));
    if (start >= static_cast<int>(stack.size())) {
        owner_->suppressNextMerge_ = false;
        return;
    }

    std::vector<CodeEditOperation> operations(stack.begin() + start, stack.end());
    stack.erase(stack.begin() + start, stack.end());

    CodeEditOperation compound;
    compound.kind = CodeEditKind::Compound;
    compound.operations = std::move(operations);
    compound.selectionBefore = compound.operations.front().selectionBefore;
    compound.selectionAfter = compound.operations.back().selectionAfter;
    compound.timestampMs = compound.operations.back().timestampMs;
    stack.push_back(std::move(compound));
    owner_->notifyListeners();
}

CodeEditorUndoController::CompoundHandle
CodeEditorUndoController::beginCompoundOperation() {
    return CompoundHandle(this);
}

// ══════════════════════════════════════════════════════════════════════════════
// CodeEditorController
// ══════════════════════════════════════════════════════════════════════════════
CodeEditorController::CodeEditorController(std::string text) : text_(std::move(text)) {
    rebuildLines();
    selection_ = TextSelection(text_.size(), text_.size());
    undoController_ = make_shared<CodeEditorUndoController>();
    bindUndoController();
}

CodeEditorController::~CodeEditorController() {
    if (undoController_)
        undoController_->setApplyEditCallback(nullptr);
}

void CodeEditorController::bindUndoController() {
    if (!undoController_)
        return;
    undoController_->setApplyEditCallback(
        [this](const CodeEditOperation& op) { applyOperation(op); });
}

void CodeEditorController::rebuildLines() {
    lines_.clear();
    lineStarts_.clear();
    lineStarts_.push_back(0);
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = text_.find('\n', start);
        if (nl == std::string::npos) {
            lines_.push_back(text_.substr(start));
            break;
        }
        lines_.push_back(text_.substr(start, nl - start));
        start = nl + 1;
        lineStarts_.push_back(start);
    }
}

void CodeEditorController::setText(const std::string& text) {
    if (text_ == text)
        return;
    text_ = text;
    rebuildLines();
    selection_ = TextSelection(text_.size(), text_.size());
    foldings_.clear();
    searchHighlights_.clear();
    ++version_;
    if (undoController_)
        undoController_->clear();
    requestScrollToCaret();
    notifyListeners();
}

std::string CodeEditorController::getLineText(int line) const {
    if (line < 0 || line >= lineCount())
        return {};
    return lines_[static_cast<std::size_t>(line)];
}

int CodeEditorController::getLineAtOffset(std::size_t offset) const {
    offset = std::min(offset, text_.size());
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    int index = static_cast<int>(it - lineStarts_.begin()) - 1;
    return std::clamp(index, 0, lineCount() - 1);
}

std::size_t CodeEditorController::getLineStartOffset(int line) const {
    line = std::clamp(line, 0, lineCount() - 1);
    return lineStarts_[static_cast<std::size_t>(line)];
}

std::size_t CodeEditorController::findLineStart(std::size_t offset) const {
    return getLineStartOffset(getLineAtOffset(offset));
}

std::size_t CodeEditorController::findLineEnd(std::size_t offset) const {
    const std::size_t nl = text_.find('\n', offset);
    return nl == std::string::npos ? text_.size() : nl;
}

std::size_t CodeEditorController::getOffsetAt(int line, int character) const {
    const std::size_t start = getLineStartOffset(line);
    const std::string& text = lines_[static_cast<std::size_t>(std::clamp(line, 0, lineCount() - 1))];
    const std::size_t within =
        character <= 0 ? 0 : std::min(static_cast<std::size_t>(character), text.size());
    return start + within;
}

int CodeEditorController::caretLine() const { return getLineAtOffset(selection_.extent()); }

int CodeEditorController::caretColumn() const {
    return visualColumnAtOffset(selection_.extent());
}

int CodeEditorController::visualColumnAtOffset(std::size_t offset) const {
    const int line = getLineAtOffset(offset);
    const std::size_t start = getLineStartOffset(line);
    return visualColumnInLine(lines_[static_cast<std::size_t>(line)],
                              offset >= start ? offset - start : 0, tabSize_);
}

std::size_t CodeEditorController::lineOffsetAtVisualColumn(int line, int column) const {
    line = std::clamp(line, 0, lineCount() - 1);
    const std::string& text = lines_[static_cast<std::size_t>(line)];
    const std::size_t start = getLineStartOffset(line);
    int col = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (col >= column)
            return start + i;
        if (text[i] == '\t') {
            col += tabSize_ - (col % tabSize_);
            ++i;
        } else {
            ++col;
            i = utf8Next(text, i);
        }
    }
    return start + text.size();
}

void CodeEditorController::setSelection(TextSelection selection) {
    selection.setBase(std::min(selection.base(), text_.size()));
    selection.setExtent(std::min(selection.extent(), text_.size()));
    if (selection_ == selection) {
        requestScrollToCaret();
        return;
    }
    selection_ = selection;
    requestScrollToCaret();
    notifyListeners();
}

void CodeEditorController::setSelectionSilently(TextSelection selection) {
    selection.setBase(std::min(selection.base(), text_.size()));
    selection.setExtent(std::min(selection.extent(), text_.size()));
    if (selection_ == selection)
        return;
    selection_ = selection;
    notifyListeners();
}

void CodeEditorController::setSelectionImmediately(std::size_t start, std::size_t end) {
    start = std::min(start, text_.size());
    end = std::min(end, text_.size());
    selection_ = TextSelection(start, end);
    notifyListeners();
}

void CodeEditorController::selectAll() { setSelection(TextSelection(0, text_.size())); }

void CodeEditorController::requestScrollToCaret() {
    if (scrollCallback_)
        scrollCallback_(caretLine());
}

void CodeEditorController::setScrollCallback(std::function<void(int)> cb) {
    scrollCallback_ = std::move(cb);
}

void CodeEditorController::scrollToLine(int line) {
    if (scrollCallback_)
        scrollCallback_(line);
}

void CodeEditorController::editRange(std::size_t start, std::size_t end,
                                     const std::string& replacement,
                                     const TextSelection& after, bool record) {
    start = std::min(start, text_.size());
    end = std::min(std::max(end, start), text_.size());

    std::string deleted = text_.substr(start, end - start);
    if (deleted == replacement) {
        selection_ = after;
        selection_.setBase(std::min(selection_.base(), text_.size()));
        selection_.setExtent(std::min(selection_.extent(), text_.size()));
        notifyListeners();
        return;
    }

    const TextSelection before = selection_;
    text_.replace(start, end - start, replacement);
    rebuildLines();
    selection_ = after;
    selection_.setBase(std::min(selection_.base(), text_.size()));
    selection_.setExtent(std::min(selection_.extent(), text_.size()));
    ++version_;

    if (record && undoController_) {
        if (!deleted.empty() && !replacement.empty())
            undoController_->recordEdit(CodeEditOperation::makeReplace(
                start, deleted, replacement, before, selection_));
        else if (deleted.empty() && !replacement.empty())
            undoController_->recordEdit(CodeEditOperation::makeInsert(
                start, replacement, before, selection_));
        else if (!deleted.empty() && replacement.empty())
            undoController_->recordEdit(
                CodeEditOperation::makeDelete(start, deleted, before, selection_));
    }

    requestScrollToCaret();
    notifyListeners();
}

void CodeEditorController::replaceRange(std::size_t start, std::size_t end,
                                        const std::string& replacement,
                                        bool preserveOldCursor) {
    if (start > end)
        std::swap(start, end);
    start = std::min(start, text_.size());
    end = std::min(end, text_.size());

    TextSelection after;
    if (preserveOldCursor) {
        const TextSelection old = selection_;
        auto remap = [&](std::size_t o) -> std::size_t {
            if (o <= start)
                return o;
            if (o >= end)
                return o - (end - start) + replacement.size();
            return start + replacement.size();
        };
        after = TextSelection(remap(old.base()), remap(old.extent()));
    } else {
        const std::size_t caret = start + replacement.size();
        after = TextSelection(caret, caret);
    }
    editRange(start, end, replacement, after);
}

void CodeEditorController::insertAtCurrentCursor(const std::string& text) {
    if (selection_.isCollapsed())
        replaceRange(selection_.extent(), selection_.extent(), text);
    else
        replaceRange(selection_.start(), selection_.end(), text);
}

void CodeEditorController::insertText(int line, int character, const std::string& text) {
    const std::size_t offset = getOffsetAt(line, character);
    replaceRange(offset, offset, text);
}

void CodeEditorController::insertTypedText(const std::string& text) {
    if (text.empty())
        return;

    if (!selection_.isCollapsed()) {
        replaceRange(selection_.start(), selection_.end(), text);
        return;
    }

    if (text == "\n") {
        const std::size_t off = selection_.extent();
        const int line = getLineAtOffset(off);
        const std::size_t lineStart = getLineStartOffset(line);
        const std::string before = text_.substr(lineStart, off - lineStart);

        std::string indent;
        for (char c : before) {
            if (c == ' ' || c == '\t')
                indent.push_back(c);
            else
                break;
        }

        bool shouldIndent = false;
        for (std::size_t i = before.size(); i-- > 0;) {
            const char c = before[i];
            if (c == ' ' || c == '\t' || c == '\r')
                continue;
            shouldIndent = c == ':' || c == '{' || c == '[' || c == '(';
            break;
        }
        const std::string extra = shouldIndent ? tabSpace() : "";

        char lastNonSpace = 0;
        for (std::size_t i = before.size(); i-- > 0;) {
            const char c = before[i];
            if (c == ' ' || c == '\t' || c == '\r')
                continue;
            lastNonSpace = c;
            break;
        }
        const char next = off < text_.size() ? text_[off] : 0;

        if (isOpenBracket(lastNonSpace) && matchingClose(lastNonSpace) == next) {
            const std::string insertion = "\n" + indent + extra + "\n" + indent;
            const std::size_t caret = off + 1 + indent.size() + extra.size();
            editRange(off, off, insertion, TextSelection(caret, caret));
            return;
        }

        const std::string insertion = "\n" + indent + extra;
        const std::size_t caret = off + insertion.size();
        editRange(off, off, insertion, TextSelection(caret, caret));
        return;
    }

    if (text.size() == 1) {
        const char typed = text[0];
        const std::size_t off = selection_.extent();

        // 右括号/引号：下一个字符与键入相同 ⇒ 直接跳过去（不重复插入）。
        if (off < text_.size() && text_[off] == typed &&
            (typed == ')' || typed == ']' || typed == '}' || typed == '"' ||
             typed == '\'')) {
            selection_ = TextSelection(off + 1, off + 1);
            requestScrollToCaret();
            notifyListeners();
            return;
        }

        if (isOpenBracket(typed) || typed == '"' || typed == '\'') {
            std::string pair;
            pair.push_back(typed);
            pair.push_back(matchingClose(typed));
            const std::size_t caret = off + 1;
            editRange(off, off, pair, TextSelection(caret, caret));
            return;
        }
    }

    replaceRange(selection_.extent(), selection_.extent(), text);
}

void CodeEditorController::deleteBackward() {
    if (!selection_.isCollapsed()) {
        replaceRange(selection_.start(), selection_.end(), "");
        return;
    }
    const std::size_t off = selection_.extent();
    if (off == 0)
        return;
    replaceRange(utf8Prev(text_, off), off, "");
}

void CodeEditorController::deleteForward() {
    if (!selection_.isCollapsed()) {
        replaceRange(selection_.start(), selection_.end(), "");
        return;
    }
    const std::size_t off = selection_.extent();
    if (off >= text_.size())
        return;
    replaceRange(off, utf8Next(text_, off), "");
}

void CodeEditorController::deleteWordBackward() {
    if (!selection_.isCollapsed()) {
        replaceRange(selection_.start(), selection_.end(), "");
        return;
    }
    std::size_t off = selection_.extent();
    std::size_t start = off;
    while (start > 0 && !isWordByte(static_cast<unsigned char>(text_[utf8Prev(text_, start)])))
        start = utf8Prev(text_, start);
    while (start > 0 && isWordByte(static_cast<unsigned char>(text_[utf8Prev(text_, start)])))
        start = utf8Prev(text_, start);
    if (start == off)
        return;
    replaceRange(start, off, "");
}

void CodeEditorController::deleteWordForward() {
    if (!selection_.isCollapsed()) {
        replaceRange(selection_.start(), selection_.end(), "");
        return;
    }
    std::size_t off = selection_.extent();
    std::size_t end = off;
    while (end < text_.size() && isWordByte(static_cast<unsigned char>(text_[end])))
        end = utf8Next(text_, end);
    while (end < text_.size() && !isWordByte(static_cast<unsigned char>(text_[end])))
        end = utf8Next(text_, end);
    if (end == off)
        return;
    replaceRange(off, end, "");
}

std::string CodeEditorController::selectedText() const {
    if (selection_.isCollapsed())
        return {};
    return text_.substr(selection_.start(), selection_.end() - selection_.start());
}

void CodeEditorController::copySelection() {
    const std::string selected = selectedText();
    if (!selected.empty())
        detail::clipboardBuf() = selected;
}

void CodeEditorController::cutSelection() {
    if (selection_.isCollapsed())
        return;
    detail::clipboardBuf() = selectedText();
    replaceRange(selection_.start(), selection_.end(), "");
}

void CodeEditorController::pasteClipboard() {
    const std::string& data = detail::clipboardBuf();
    if (data.empty())
        return;
    insertAtCurrentCursor(data);
}

void CodeEditorController::moveCaretTo(std::size_t offset, bool extend) {
    offset = std::min(offset, text_.size());
    if (extend)
        selection_ = TextSelection(selection_.base(), offset);
    else
        selection_ = TextSelection(offset, offset);
    requestScrollToCaret();
    notifyListeners();
}

void CodeEditorController::moveLeft(bool extend) {
    if (!extend && !selection_.isCollapsed()) {
        moveCaretTo(selection_.start(), false);
        return;
    }
    moveCaretTo(utf8Prev(text_, selection_.extent()), extend);
}

void CodeEditorController::moveRight(bool extend) {
    if (!extend && !selection_.isCollapsed()) {
        moveCaretTo(selection_.end(), false);
        return;
    }
    moveCaretTo(utf8Next(text_, selection_.extent()), extend);
}

void CodeEditorController::moveVertical(int deltaLines, bool extend) {
    if (deltaLines == 0)
        return;
    const int goal = caretColumn();
    int line = caretLine();
    const int step = deltaLines < 0 ? -1 : 1;
    const int count = std::abs(deltaLines);
    int target = line;
    for (int i = 0; i < count; ++i) {
        int next = -1;
        if (step < 0) {
            if (target <= 0)
                break;
            next = target - 1;
            const int foldStart = getFoldStartForLine(next);
            if (foldStart >= 0)
                next = foldStart;
        } else {
            auto it = foldings_.find(target);
            if (it != foldings_.end() && it->second.isFolded)
                next = it->second.endIndex + 1;
            else
                next = target + 1;
            if (next > lineCount() - 1)
                break;
            const int foldStart = getFoldStartForLine(next);
            if (foldStart >= 0) {
                auto fit = foldings_.find(foldStart);
                next = fit != foldings_.end() ? fit->second.endIndex + 1 : next;
            }
            if (next > lineCount() - 1)
                break;
        }
        target = next;
    }

    if (target == line) {
        if (step < 0)
            moveCaretTo(0, extend);
        else
            moveCaretTo(text_.size(), extend);
        return;
    }
    moveCaretTo(lineOffsetAtVisualColumn(target, goal), extend);
}

void CodeEditorController::moveUp(bool extend) { moveVertical(-1, extend); }
void CodeEditorController::moveDown(bool extend) { moveVertical(1, extend); }
void CodeEditorController::movePage(int lines, bool extend) { moveVertical(lines, extend); }

void CodeEditorController::moveToLineStart(bool extend) {
    moveCaretTo(findLineStart(selection_.extent()), extend);
}

void CodeEditorController::moveToLineEnd(bool extend) {
    moveCaretTo(findLineEnd(selection_.extent()), extend);
}

void CodeEditorController::moveToDocumentStart(bool extend) { moveCaretTo(0, extend); }
void CodeEditorController::moveToDocumentEnd(bool extend) {
    moveCaretTo(text_.size(), extend);
}

void CodeEditorController::moveWordLeft(bool extend) {
    std::size_t off = selection_.extent();
    while (off > 0 && !isWordByte(static_cast<unsigned char>(text_[utf8Prev(text_, off)])))
        off = utf8Prev(text_, off);
    while (off > 0 && isWordByte(static_cast<unsigned char>(text_[utf8Prev(text_, off)])))
        off = utf8Prev(text_, off);
    moveCaretTo(off, extend);
}

void CodeEditorController::moveWordRight(bool extend) {
    std::size_t off = selection_.extent();
    while (off < text_.size() && isWordByte(static_cast<unsigned char>(text_[off])))
        off = utf8Next(text_, off);
    while (off < text_.size() && !isWordByte(static_cast<unsigned char>(text_[off])))
        off = utf8Next(text_, off);
    moveCaretTo(off, extend);
}

int CodeEditorController::lineFirstSelected(int firstLine, int) const { return firstLine; }

int CodeEditorController::lineLastSelected(int, int lastLine) const {
    if (selection_.start() < selection_.end() && lastLine > 0 &&
        selection_.end() == lineStarts_[static_cast<std::size_t>(lastLine)])
        return lastLine - 1;
    return lastLine;
}

void CodeEditorController::moveLineUp() {
    const int first = lineFirstSelected(getLineAtOffset(selection_.start()),
                                        getLineAtOffset(selection_.end()));
    const int last = lineLastSelected(getLineAtOffset(selection_.start()),
                                      getLineAtOffset(selection_.end()));
    if (first <= 0)
        return;
    const std::size_t regionStart = lineStarts_[static_cast<std::size_t>(first - 1)];
    const std::size_t regionEnd = findLineEnd(lineStarts_[static_cast<std::size_t>(last)]);
    const std::string prev = lines_[static_cast<std::size_t>(first - 1)];
    std::string block;
    for (int i = first; i <= last; ++i) {
        if (i > first)
            block += '\n';
        block += lines_[static_cast<std::size_t>(i)];
    }
    const std::string replacement = block + "\n" + prev;

    // 区域总长度不变：上半段（prev 行）落到下半段，块整体上移一行。
    const std::size_t blockStart = regionStart + prev.size() + 1;
    auto mapOffset = [&](std::size_t off) {
        if (off < regionStart || off > regionEnd)
            return off;
        if (off >= blockStart)
            return off - (prev.size() + 1);
        return regionStart + block.size() + 1 + (off - regionStart);
    };
    const TextSelection old = selection_;
    const TextSelection after(mapOffset(old.base()), mapOffset(old.extent()));
    editRange(regionStart, regionEnd, replacement, after);
}

void CodeEditorController::moveLineDown() {
    const int first = lineFirstSelected(getLineAtOffset(selection_.start()),
                                        getLineAtOffset(selection_.end()));
    const int last = lineLastSelected(getLineAtOffset(selection_.start()),
                                      getLineAtOffset(selection_.end()));
    if (last >= lineCount() - 1)
        return;
    const std::size_t regionStart = lineStarts_[static_cast<std::size_t>(first)];
    const std::size_t regionEnd =
        findLineEnd(lineStarts_[static_cast<std::size_t>(last + 1)]);
    const std::string next = lines_[static_cast<std::size_t>(last + 1)];
    std::string block;
    for (int i = first; i <= last; ++i) {
        if (i > first)
            block += '\n';
        block += lines_[static_cast<std::size_t>(i)];
    }
    const std::string replacement = next + "\n" + block;

    const std::size_t nextStart = regionStart + block.size() + 1;
    auto mapOffset = [&](std::size_t off) {
        if (off < regionStart || off > regionEnd)
            return off;
        if (off < nextStart)
            return regionStart + next.size() + 1 + (off - regionStart);
        return regionStart + (off - nextStart);
    };
    const TextSelection old = selection_;
    const TextSelection after(mapOffset(old.base()), mapOffset(old.extent()));
    editRange(regionStart, regionEnd, replacement, after);
}

void CodeEditorController::duplicateLine() {
    if (selection_.isCollapsed()) {
        const int line = caretLine();
        const std::size_t end = findLineEnd(selection_.extent());
        const std::string lineText = lines_[static_cast<std::size_t>(line)];
        const std::size_t caret = end + 1;
        editRange(end, end, "\n" + lineText, TextSelection(caret, caret));
        return;
    }
    const std::size_t start = selection_.start();
    const std::size_t end = selection_.end();
    const std::string selected = text_.substr(start, end - start);
    editRange(end, end, selected, TextSelection(end, end + selected.size()));
}

void CodeEditorController::indentSelection() {
    const std::string unit = tabSpace();
    if (unit.empty())
        return;

    if (selection_.isCollapsed()) {
        const std::size_t off = selection_.extent();
        editRange(off, off, unit, TextSelection(off + unit.size(), off + unit.size()));
        return;
    }

    int first = getLineAtOffset(selection_.start());
    int last = getLineAtOffset(selection_.end());
    last = lineLastSelected(first, last);
    if (last < first)
        last = first;

    const std::size_t regionStart = lineStarts_[static_cast<std::size_t>(first)];
    const std::size_t regionEnd = findLineEnd(lineStarts_[static_cast<std::size_t>(last)]);

    std::string replacement;
    for (int l = first; l <= last; ++l) {
        if (l > first)
            replacement.push_back('\n');
        replacement += unit;
        replacement += lines_[static_cast<std::size_t>(l)];
    }

    auto shift = [&](std::size_t off) {
        std::size_t result = off;
        for (int l = first; l <= last; ++l) {
            if (lineStarts_[static_cast<std::size_t>(l)] <= off)
                result += unit.size();
            else
                break;
        }
        return result;
    };
    const TextSelection old = selection_;
    editRange(regionStart, regionEnd, replacement,
              TextSelection(shift(old.base()), shift(old.extent())));
}

void CodeEditorController::unindentSelection() {
    const std::string unit = tabSpace();

    auto removedForLine = [&](const std::string& line) -> std::size_t {
        if (!unit.empty() && line.compare(0, unit.size(), unit) == 0)
            return unit.size();
        std::size_t n = 0;
        while (n < line.size() && line[n] == ' ')
            ++n;
        return n;
    };

    if (selection_.isCollapsed()) {
        const int line = caretLine();
        const std::size_t start = lineStarts_[static_cast<std::size_t>(line)];
        const std::string& text = lines_[static_cast<std::size_t>(line)];
        const std::size_t removed = removedForLine(text);
        if (removed == 0)
            return;
        const std::size_t caret = std::max(start, selection_.extent() - removed);
        editRange(start, start + removed, "", TextSelection(caret, caret));
        return;
    }

    int first = getLineAtOffset(selection_.start());
    int last = getLineAtOffset(selection_.end());
    last = lineLastSelected(first, last);
    if (last < first)
        last = first;

    const std::size_t regionStart = lineStarts_[static_cast<std::size_t>(first)];
    const std::size_t regionEnd = findLineEnd(lineStarts_[static_cast<std::size_t>(last)]);

    std::vector<std::size_t> removed(static_cast<std::size_t>(last - first + 1), 0);
    std::string replacement;
    for (int l = first; l <= last; ++l) {
        const std::string& line = lines_[static_cast<std::size_t>(l)];
        const std::size_t r = removedForLine(line);
        removed[static_cast<std::size_t>(l - first)] = r;
        if (l > first)
            replacement.push_back('\n');
        replacement += line.substr(r);
    }

    auto shift = [&](std::size_t off) {
        std::size_t result = off;
        for (int l = first; l <= last; ++l) {
            const std::size_t start = lineStarts_[static_cast<std::size_t>(l)];
            if (off >= start)
                result -= std::min(removed[static_cast<std::size_t>(l - first)], off - start);
            else
                break;
        }
        return result;
    };
    const TextSelection old = selection_;
    editRange(regionStart, regionEnd, replacement,
              TextSelection(shift(old.base()), shift(old.extent())));
}

int CodeEditorController::detectIndentation() const {
    std::map<int, int> counts;
    int previous = -1;
    for (const auto& line : lines_) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        const int spaces = leadingIndentColumns(line, tabSize_);
        if (previous >= 0 && spaces != previous) {
            const int diff = std::abs(spaces - previous);
            if (diff >= 1 && diff <= 32)
                ++counts[diff];
        }
        previous = spaces;
    }
    int best = 0;
    int bestCount = 0;
    for (const auto& [width, count] : counts) {
        if (count > bestCount) {
            best = width;
            bestCount = count;
        }
    }
    if (best > 0)
        return best;
    return tabSize_ > 0 ? tabSize_ : 2;
}

void CodeEditorController::reindentDocument(int newTabSize) {
    newTabSize = std::max(1, newTabSize);
    // 上游用 detectIndentation()（当前文档实测宽度）当旧宽度，而不是 tabSize_。
    const int oldTab = detectIndentation();
    if (oldTab <= 0 || oldTab == newTabSize) {
        setTabSize(newTabSize);
        return;
    }

    std::vector<std::string> newLines;
    newLines.reserve(lines_.size());
    std::vector<std::size_t> oldIndentBytes(lines_.size(), 0);
    std::vector<std::size_t> newIndentBytes(lines_.size(), 0);

    for (std::size_t i = 0; i < lines_.size(); ++i) {
        const std::string& line = lines_[i];
        std::size_t bytes = 0;
        int visual = 0;
        for (; bytes < line.size(); ++bytes) {
            if (line[bytes] == ' ')
                ++visual;
            else if (line[bytes] == '\t')
                visual += oldTab - (visual % oldTab);
            else
                break;
        }
        const int levels = visual / oldTab;
        const int remainder = visual % oldTab;
        const std::size_t newIndent =
            static_cast<std::size_t>(levels * newTabSize + remainder);
        oldIndentBytes[i] = bytes;
        newIndentBytes[i] = newIndent;
        newLines.push_back(std::string(newIndent, ' ') + line.substr(bytes));
    }

    std::vector<std::size_t> newStarts;
    newStarts.reserve(newLines.size());
    newStarts.push_back(0);
    for (std::size_t i = 0; i < newLines.size(); ++i) {
        if (i + 1 < newLines.size())
            newStarts.push_back(newStarts.back() + newLines[i].size() + 1);
    }

    auto remap = [&](std::size_t off) {
        const int line = getLineAtOffset(off);
        const std::size_t lineStart = lineStarts_[static_cast<std::size_t>(line)];
        const std::size_t newStart = newStarts[static_cast<std::size_t>(line)];
        const std::size_t within = off >= lineStart ? off - lineStart : 0;
        if (within < oldIndentBytes[static_cast<std::size_t>(line)])
            return newStart + std::min(within, newIndentBytes[static_cast<std::size_t>(line)]);
        return newStart + newIndentBytes[static_cast<std::size_t>(line)] +
               (within - oldIndentBytes[static_cast<std::size_t>(line)]);
    };

    const TextSelection old = selection_;
    const TextSelection after(remap(old.base()), remap(old.extent()));
    const std::string newText = joinLines(newLines);
    tabSize_ = newTabSize;
    if (newText == text_) {
        notifyListeners();
        return;
    }
    editRange(0, text_.size(), newText, after);
}

void CodeEditorController::setFoldings(std::map<int, CodeFoldRange> ranges, bool notify) {
    foldings_ = std::move(ranges);
    if (notify)
        notifyListeners();
}

void CodeEditorController::recomputeFoldings() {
    setFoldings(detectCodeFoldRanges(lines_, tabSize_));
}

void CodeEditorController::toggleFold(int line) {
    auto it = foldings_.find(line);
    if (it == foldings_.end())
        return;

    CodeFoldRange& range = it->second;
    if (range.isFolded) {
        range.isFolded = false;
        for (int child : range.originallyFoldedChildren) {
            auto ci = foldings_.find(child);
            if (ci != foldings_.end())
                ci->second.isFolded = true;
        }
        range.originallyFoldedChildren.clear();
    } else {
        range.originallyFoldedChildren.clear();
        for (auto& entry : foldings_) {
            if (entry.first == line)
                continue;
            CodeFoldRange& other = entry.second;
            if (entry.first > range.startIndex && other.endIndex <= range.endIndex &&
                other.isFolded) {
                other.isFolded = false;
                range.originallyFoldedChildren.push_back(entry.first);
            }
        }
        range.isFolded = true;
    }
    notifyListeners();
}

void CodeEditorController::foldAll() {
    int coveredUntil = -1;
    for (auto& entry : foldings_) {
        CodeFoldRange& range = entry.second;
        if (entry.first <= coveredUntil)
            continue;
        range.originallyFoldedChildren.clear();
        for (auto& child : foldings_) {
            if (child.first == entry.first)
                continue;
            if (child.first > range.startIndex && child.second.endIndex <= range.endIndex &&
                child.second.isFolded) {
                child.second.isFolded = false;
                range.originallyFoldedChildren.push_back(child.first);
            }
        }
        range.isFolded = true;
        coveredUntil = range.endIndex;
    }
    notifyListeners();
}

void CodeEditorController::unfoldAll() {
    bool changed = false;
    for (auto& entry : foldings_) {
        if (entry.second.isFolded) {
            entry.second.isFolded = false;
            changed = true;
        }
        entry.second.originallyFoldedChildren.clear();
    }
    if (changed)
        notifyListeners();
}

bool CodeEditorController::isLineInFoldedRegion(int line) const {
    for (const auto& entry : foldings_) {
        if (entry.second.isFolded && entry.second.containsLine(line))
            return true;
    }
    return false;
}

int CodeEditorController::getFoldStartForLine(int line) const {
    for (const auto& entry : foldings_) {
        if (entry.second.isFolded && entry.second.containsLine(line))
            return entry.first;
    }
    return -1;
}

std::vector<int> CodeEditorController::visibleLineIndices() const {
    std::vector<int> out;
    out.reserve(lines_.size());
    for (int i = 0; i < lineCount();) {
        out.push_back(i);
        auto it = foldings_.find(i);
        if (it != foldings_.end() && it->second.isFolded)
            i = it->second.endIndex + 1;
        else
            ++i;
    }
    return out;
}

void CodeEditorController::setSearchHighlights(std::vector<CodeSearchHighlight> highlights) {
    searchHighlights_ = std::move(highlights);
    notifyListeners();
}

void CodeEditorController::clearSearchHighlights() {
    if (searchHighlights_.empty())
        return;
    searchHighlights_.clear();
    notifyListeners();
}

std::string CodeEditorController::getCurrentWordPrefixAt(std::size_t offset) const {
    offset = std::min(offset, text_.size());
    std::size_t start = offset;
    while (start > 0 && isWordByte(static_cast<unsigned char>(text_[utf8Prev(text_, start)])))
        start = utf8Prev(text_, start);
    return text_.substr(start, offset - start);
}

void CodeEditorController::setUndoController(
    SharedPtr<CodeEditorUndoController> controller) {
    if (undoController_ == controller)
        return;
    if (undoController_)
        undoController_->setApplyEditCallback(nullptr);
    undoController_ = controller ? std::move(controller) : nullptr;
    if (!undoController_)
        undoController_ = make_shared<CodeEditorUndoController>();
    bindUndoController();
}

bool CodeEditorController::canUndo() const {
    return undoController_ && undoController_->canUndo();
}

bool CodeEditorController::canRedo() const {
    return undoController_ && undoController_->canRedo();
}

bool CodeEditorController::undo() { return undoController_ && undoController_->undo(); }
bool CodeEditorController::redo() { return undoController_ && undoController_->redo(); }

void CodeEditorController::recordEdit(const CodeEditOperation& operation) {
    if (undoController_)
        undoController_->recordEdit(operation);
}

void CodeEditorController::applyOperation(const CodeEditOperation& op) {
    auto rawReplace = [&](std::size_t offset, std::size_t count,
                          const std::string& replacement) {
        offset = std::min(offset, text_.size());
        count = std::min(count, text_.size() - offset);
        text_.replace(offset, count, replacement);
    };

    switch (op.kind) {
    case CodeEditKind::Insert:
        rawReplace(op.offset, 0, op.text);
        break;
    case CodeEditKind::Delete:
        rawReplace(op.offset, op.text.size(), "");
        break;
    case CodeEditKind::Replace:
        rawReplace(op.offset, op.deletedText.size(), op.insertedText);
        break;
    case CodeEditKind::Compound:
        for (const auto& child : op.operations) {
            switch (child.kind) {
            case CodeEditKind::Insert:
                rawReplace(child.offset, 0, child.text);
                break;
            case CodeEditKind::Delete:
                rawReplace(child.offset, child.text.size(), "");
                break;
            case CodeEditKind::Replace:
                rawReplace(child.offset, child.deletedText.size(), child.insertedText);
                break;
            case CodeEditKind::Compound:
                break;
            }
        }
        break;
    }

    rebuildLines();
    selection_ = op.selectionAfter;
    selection_.setBase(std::min(selection_.base(), text_.size()));
    selection_.setExtent(std::min(selection_.extent(), text_.size()));
    ++version_;
    requestScrollToCaret();
    notifyListeners();
}

void CodeEditorController::setTabSize(int value) {
    value = std::max(1, value);
    if (tabSize_ == value)
        return;
    tabSize_ = value;
    notifyListeners();
}

void CodeEditorController::setUseSpaceAsTab(bool value) {
    if (useSpaceAsTab_ == value)
        return;
    useSpaceAsTab_ = value;
    notifyListeners();
}

std::string CodeEditorController::tabSpace() const {
    if (useSpaceAsTab_)
        return std::string(static_cast<std::size_t>(std::max(1, tabSize_)), ' ');
    return "\t";
}

// ══════════════════════════════════════════════════════════════════════════════
// CodeEditorFindController
// ══════════════════════════════════════════════════════════════════════════════
CodeEditorFindController::CodeEditorFindController(
    SharedPtr<CodeEditorController> codeController)
    : code_(std::move(codeController)) {
    lastText_ = code_->text();
    codeListenerId_ = code_->addListenerWithHandle([this]() { onCodeChanged(); });
}

CodeEditorFindController::~CodeEditorFindController() {
    if (code_ && codeListenerId_ >= 0) {
        code_->removeListenerByHandle(codeListenerId_);
        codeListenerId_ = -1;
    }
    if (code_)
        code_->clearSearchHighlights();
    code_.reset();
}

void CodeEditorFindController::onCodeChanged() {
    const std::string now = code_->text();
    if (now == lastText_)
        return;
    lastText_ = now;
    if (!isActive_ && lastQuery_.empty())
        return;
    if (!lastQuery_.empty())
        find(lastQuery_, false);
}

std::vector<CodeEditorFindController::MatchRange> CodeEditorFindController::runSearch(
    const std::string& query) const {
    std::vector<MatchRange> matches;
    if (query.empty())
        return matches;

    std::string pattern = isRegex_ ? query : escapeRegex(query);
    if (wholeWord_)
        pattern = "\\b(?:" + pattern + ")\\b";

    std::regex_constants::syntax_option_type flags = std::regex::ECMAScript;
    if (!caseSensitive_)
        flags |= std::regex::icase;

    try {
        std::regex re(pattern, flags);
        const std::string& text = code_->text();
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
            const auto position = static_cast<std::size_t>(it->position());
            const auto length = static_cast<std::size_t>(it->length());
            matches.push_back({position, position + length});
        }
    } catch (const std::regex_error&) {
        matches.clear();
    }
    return matches;
}

void CodeEditorFindController::updateHighlights(bool notify) {
    highlights_.clear();
    highlights_.reserve(matches_.size());
    for (std::size_t i = 0; i < matches_.size(); ++i) {
        highlights_.push_back(CodeSearchHighlight{
            matches_[i].start, matches_[i].end,
            static_cast<int>(i) == currentMatchIndex_});
    }
    code_->setSearchHighlights(highlights_);
    if (notify)
        notifyListeners();
}

void CodeEditorFindController::find(const std::string& query, bool scrollToMatch) {
    lastQuery_ = query;
    if (query.empty()) {
        matches_.clear();
        currentMatchIndex_ = -1;
        highlights_.clear();
        code_->clearSearchHighlights();
        notifyListeners();
        return;
    }

    matches_ = runSearch(query);
    if (matches_.empty()) {
        currentMatchIndex_ = -1;
        updateHighlights(false);
        notifyListeners();
        return;
    }

    const std::size_t cursor = code_->selection().start();
    int index = 0;
    bool found = false;
    for (std::size_t i = 0; i < matches_.size(); ++i) {
        if (matches_[i].start >= cursor) {
            index = static_cast<int>(i);
            found = true;
            break;
        }
    }
    currentMatchIndex_ = found ? index : 0;

    updateHighlights(false);
    if (scrollToMatch)
        code_->scrollToLine(code_->getLineAtOffset(matches_[static_cast<std::size_t>(
            currentMatchIndex_)].start));
    notifyListeners();
}

void CodeEditorFindController::next() {
    if (matches_.empty())
        return;
    currentMatchIndex_ = (currentMatchIndex_ + 1) % static_cast<int>(matches_.size());
    updateHighlights(true);
    code_->scrollToLine(
        code_->getLineAtOffset(matches_[static_cast<std::size_t>(currentMatchIndex_)].start));
}

void CodeEditorFindController::previous() {
    if (matches_.empty())
        return;
    currentMatchIndex_ =
        (currentMatchIndex_ - 1 + static_cast<int>(matches_.size())) %
        static_cast<int>(matches_.size());
    updateHighlights(true);
    code_->scrollToLine(
        code_->getLineAtOffset(matches_[static_cast<std::size_t>(currentMatchIndex_)].start));
}

void CodeEditorFindController::clear() {
    lastQuery_.clear();
    matches_.clear();
    currentMatchIndex_ = -1;
    highlights_.clear();
    code_->clearSearchHighlights();
    notifyListeners();
}

void CodeEditorFindController::reSearch(bool scrollToMatch) {
    if (!lastQuery_.empty())
        find(lastQuery_, scrollToMatch);
    else
        notifyListeners();
}

void CodeEditorFindController::replace(const std::string& replacement) {
    if (currentMatchIndex_ < 0 ||
        currentMatchIndex_ >= static_cast<int>(matches_.size()))
        return;
    const MatchRange match = matches_[static_cast<std::size_t>(currentMatchIndex_)];
    code_->replaceRange(match.start, match.end, replacement);
}

void CodeEditorFindController::replaceAll(const std::string& replacement) {
    if (matches_.empty())
        return;

    const std::string text = code_->text();
    std::string out;
    out.reserve(text.size());
    std::size_t last = 0;
    for (const auto& match : matches_) {
        if (match.start < last || match.end > text.size())
            continue;
        out += text.substr(last, match.start - last);
        out += replacement;
        last = match.end;
    }
    out += text.substr(last);
    if (out == text)
        return;
    code_->replaceRange(0, text.size(), out);
}

void CodeEditorFindController::setCaseSensitive(bool value) {
    if (caseSensitive_ == value)
        return;
    caseSensitive_ = value;
    reSearch(false);
}

void CodeEditorFindController::setIsRegex(bool value) {
    if (isRegex_ == value)
        return;
    isRegex_ = value;
    reSearch(false);
}

void CodeEditorFindController::setWholeWord(bool value) {
    if (wholeWord_ == value)
        return;
    wholeWord_ = value;
    reSearch(false);
}

void CodeEditorFindController::setIsActive(bool value) {
    if (isActive_ == value)
        return;
    isActive_ = value;
    if (isActive_) {
        if (!lastQuery_.empty())
            reSearch(true);
        else
            notifyListeners();
    } else {
        matches_.clear();
        currentMatchIndex_ = -1;
        highlights_.clear();
        code_->clearSearchHighlights();
        notifyListeners();
    }
}

void CodeEditorFindController::setIsReplaceMode(bool value) {
    if (isReplaceMode_ == value)
        return;
    isReplaceMode_ = value;
    notifyListeners();
}

// ══════════════════════════════════════════════════════════════════════════════
// 折叠检测
// ══════════════════════════════════════════════════════════════════════════════
std::map<int, CodeFoldRange> detectCodeFoldRanges(const std::vector<std::string>& lines,
                                                  int tabSize) {
    std::map<int, CodeFoldRange> folds;

    struct OpenBracket {
        char open = 0;
        int line = 0;
    };
    std::vector<OpenBracket> stack;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        bool inString = false;
        bool escape = false;
        char quote = 0;
        for (std::size_t j = 0; j < line.size(); ++j) {
            const char c = line[j];
            if (inString) {
                if (escape) {
                    escape = false;
                } else if (c == '\\') {
                    escape = true;
                } else if (c == quote) {
                    inString = false;
                }
                continue;
            }
            if (c == '/' && j + 1 < line.size() && line[j + 1] == '/')
                break;
            if (c == '/' && j + 1 < line.size() && line[j + 1] == '*') {
                const std::size_t end = line.find("*/", j + 2);
                if (end == std::string::npos)
                    break;
                j = end + 1;
                continue;
            }
            if (c == '"' || c == '\'') {
                inString = true;
                quote = c;
                continue;
            }
            if (isOpenBracket(c)) {
                stack.push_back({c, static_cast<int>(i)});
                continue;
            }
            if (!isCloseBracket(c))
                continue;

            char want = 0;
            switch (c) {
            case ')':
                want = '(';
                break;
            case ']':
                want = '[';
                break;
            case '}':
                want = '{';
                break;
            default:
                break;
            }
            for (std::size_t k = stack.size(); k-- > 0;) {
                if (stack[k].open != want)
                    continue;
                const OpenBracket entry = stack[k];
                stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(k), stack.end());
                if (static_cast<int>(i) > entry.line)
                    folds[entry.line] = CodeFoldRange{entry.line, static_cast<int>(i), false, {}};
                break;
            }
        }
    }

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = rtrimCopy(lines[i]);
        if (trimmed.empty() || trimmed.back() != ':')
            continue;
        const int line = static_cast<int>(i);
        if (folds.count(line))
            continue;
        const int startIndent = leadingIndentColumns(lines[i], tabSize);
        int endLine = line;
        for (std::size_t j = i + 1; j < lines.size(); ++j) {
            const std::string t = rtrimCopy(lines[j]);
            if (t.empty())
                continue;
            const int indent = leadingIndentColumns(lines[j], tabSize);
            if (indent <= startIndent)
                break;
            endLine = static_cast<int>(j);
        }
        if (endLine > line)
            folds[line] = CodeFoldRange{line, endLine, false, {}};
    }

    return folds;
}

// ══════════════════════════════════════════════════════════════════════════════
// CodeEditor（控件）
// ══════════════════════════════════════════════════════════════════════════════
CodeEditor::CodeEditor(SharedPtr<CodeEditorController> controller, std::string filename,
                       KeyPtr key, double fontSize, std::optional<double> height,
                       double width, bool showLineNumbers, int tabSize,
                       CodeLanguage codeLanguage, bool showIndentGuides,
                       bool enableFolding, bool readOnly, bool showTitleBar,
                       std::optional<CodeEditorTheme> editorTheme,
                       SharedPtr<CodeEditorFindController> findController,
                       SharedPtr<CodeEditorUndoController> undoController,
                       SharedPtr<FocusNode> focusNode, std::string language)
    : StatefulWidget(std::move(key)),
      controller_(controller ? std::move(controller)
                             : make_shared<CodeEditorController>()),
      findController_(std::move(findController)),
      undoController_(std::move(undoController)),
      focusNode_(focusNode ? std::move(focusNode) : make_shared<FocusNode>()),
      filename_(std::move(filename)),
      fontSize_(fontSize),
      height_(height),
      width_(width),
      showLineNumbers_(showLineNumbers),
      tabSize_(tabSize),
      language_(std::move(language)),
      codeLanguage_(codeLanguage),
      showIndentGuides_(showIndentGuides),
      enableFolding_(enableFolding),
      readOnly_(readOnly),
      showTitleBar_(showTitleBar),
      editorTheme_(std::move(editorTheme)) {}

CodeEditor::CodeEditor(std::vector<std::string> lines, std::string filename, KeyPtr key,
                       double fontSize, std::optional<int> highlightLine,
                       std::optional<double> height, double width, bool showLineNumbers,
                       int tabSize, std::string language, CodeLanguage codeLanguage,
                       bool showIndentGuides,
                       std::optional<CodeEditorTheme> editorTheme, bool enableFolding,
                       bool readOnly)
    : StatefulWidget(std::move(key)),
      // 参数里有 std::string ⇒ ADL 会把 std::make_shared 拉进重载集，必须限定。
      controller_(cui::make_shared<CodeEditorController>(joinLines(lines))),
      undoController_(nullptr),
      focusNode_(make_shared<FocusNode>()),
      filename_(std::move(filename)),
      fontSize_(fontSize),
      highlightLine_(highlightLine),
      height_(height),
      width_(width),
      showLineNumbers_(showLineNumbers),
      tabSize_(tabSize),
      language_(std::move(language)),
      codeLanguage_(codeLanguage),
      showIndentGuides_(showIndentGuides),
      enableFolding_(enableFolding),
      readOnly_(readOnly),
      showTitleBar_(true),
      editorTheme_(std::move(editorTheme)) {
    controller_->setTabSize(tabSize);
    if (highlightLine && *highlightLine >= 0)
        controller_->setSelectionImmediately(controller_->getLineStartOffset(*highlightLine),
                                             controller_->getLineStartOffset(*highlightLine));
}

// ══════════════════════════════════════════════════════════════════════════════
// CodeEditorState
// ══════════════════════════════════════════════════════════════════════════════
class CodeEditorState : public State {
public:
    void initState() override {
        auto* w = static_cast<CodeEditor*>(widget());
        controller_ = w->controller();
        findController_ = w->findController();
        if (w->undoController())
            controller_->setUndoController(w->undoController());
        if (w->tabSize() > 0)
            controller_->setTabSize(w->tabSize());

        lastFoldText_ = controller_->text();
        if (w->enableFolding())
            controller_->setFoldings(
                detectCodeFoldRanges(controller_->lines(), controller_->tabSize()), false);

        controller_->setScrollCallback([this](int) { ensureCaretVisible(); });
        controllerListenerId_ = controller_->addListenerWithHandle([this]() {
            onControllerChanged();
        });

        wireFocusNode(w->focusNode());
    }

    void dispose() override {
        if (controller_ && controllerListenerId_ >= 0) {
            controller_->removeListenerByHandle(controllerListenerId_);
            controllerListenerId_ = -1;
        }
        if (controller_)
            controller_->setScrollCallback(nullptr);
        if (focusNode_) {
            if (focusNode_->hasFocus())
                focusNode_->unfocus();
            focusNode_->onFocusChange = nullptr;
            focusNode_->onKeyEvent = nullptr;
            focusNode_->onIMCommit = nullptr;
            focusNode_->onPreedit = nullptr;
            focusNode_->setContext(nullptr);
        }
        focusNode_ = nullptr;
        findController_ = nullptr;
        controller_ = nullptr;
        State::dispose();
    }

    void didUpdateWidget(cui::Widget* oldWidget) override {
        (void)oldWidget;
        auto* w = static_cast<CodeEditor*>(widget());

        if (controller_ != w->controller()) {
            if (controller_ && controllerListenerId_ >= 0) {
                controller_->removeListenerByHandle(controllerListenerId_);
                controller_->setScrollCallback(nullptr);
            }
            controller_ = w->controller();
            if (w->undoController())
                controller_->setUndoController(w->undoController());
            if (w->tabSize() > 0)
                controller_->setTabSize(w->tabSize());
            lastFoldText_ = controller_->text();
            if (w->enableFolding())
                controller_->setFoldings(
                    detectCodeFoldRanges(controller_->lines(), controller_->tabSize()), false);
            controller_->setScrollCallback([this](int) { ensureCaretVisible(); });
            controllerListenerId_ = controller_->addListenerWithHandle([this]() {
                onControllerChanged();
            });
        }

        if (focusNode_ != w->focusNode())
            wireFocusNode(w->focusNode());
        findController_ = w->findController();
    }

    SharedPtr<Widget> build(BuildContext& context) override;

private:
    void wireFocusNode(SharedPtr<FocusNode> node) {
        if (focusNode_) {
            if (focusNode_->hasFocus())
                focusNode_->unfocus();
            focusNode_->onFocusChange = nullptr;
            focusNode_->onKeyEvent = nullptr;
            focusNode_->onIMCommit = nullptr;
            focusNode_->onPreedit = nullptr;
            focusNode_->setContext(nullptr);
        }
        focusNode_ = std::move(node);

        BuildOwner* bo = nullptr;
        Element* elem = nullptr;
        if (BuildContext* ctx = context())
            if ((elem = ctx->element()) != nullptr)
                bo = elem->owner();
        if (bo)
            focusNode_->setManager(&bo->focusManager());
        focusNode_->setContext(elem);
        focusNode_->onFocusChange = [this](bool) { setState(); };
        focusNode_->onKeyEvent = [this](const KeyEvent& ev) { return handleKey(ev); };
        focusNode_->onIMCommit = [this](const std::string& text) {
            if (!readOnly())
                controller_->insertTypedText(text);
        };
        focusNode_->onPreedit = [](const std::string&) {};
        focusNode_->setEditable(true);
    }

    void onControllerChanged() {
        auto* w = static_cast<CodeEditor*>(widget());
        if (w->enableFolding()) {
            const std::string& text = controller_->text();
            if (text != lastFoldText_) {
                lastFoldText_ = text;
                auto ranges = detectCodeFoldRanges(controller_->lines(), controller_->tabSize());
                const auto old = controller_->foldings();
                for (auto& entry : ranges) {
                    auto it = old.find(entry.first);
                    if (it != old.end() && it->second.endIndex == entry.second.endIndex &&
                        it->second.isFolded)
                        entry.second.isFolded = true;
                }
                controller_->setFoldings(std::move(ranges), false);
            }
        }
        setState();
    }

    bool readOnly() const { return static_cast<CodeEditor*>(widget())->readOnly(); }

    KeyEventResult handleKey(const KeyEvent& ev);
    void scrollToLine(int line);
    void ensureCaretVisible();
    SharedPtr<Widget> buildWithWidth(double width);

    bool hitTest(const Offset& position, std::size_t& offset) const;
    int lineAtY(double y) const;
    std::size_t offsetAtPosition(const Offset& position) const;
    static double columnX(int column, double charWidth) {
        return static_cast<double>(column) * charWidth;
    }

    CodeEditorTheme editorTheme() const;

    void onPointerDown(const PointerEvent& ev);
    void onPointerMove(const PointerEvent& ev);

    SharedPtr<CodeEditorController> controller_;
    SharedPtr<CodeEditorFindController> findController_;
    SharedPtr<FocusNode> focusNode_;
    int controllerListenerId_ = -1;
    std::string lastFoldText_;

    ScrollController hScroll_;
    ScrollController vScroll_;

    bool dragging_ = false;
    std::size_t dragAnchor_ = 0;

    // 最近一次 build 的度量（指针命中 / 滚动跟随共享）。
    double lineHeight_ = 18.0;
    double charWidth_ = 8.0;
    double gutterNumberWidth_ = 40.0;
    double gutterFoldWidth_ = 0.0;
    double gutterWidth_ = 40.0;
    double gutterTotal_ = 41.0;
    double codePadX_ = 10.0;
    double codeCellWidth_ = 400.0;
    double contentWidth_ = 441.0;
    double contentHeight_ = 0.0;
    double viewportHeight_ = 0.0;
    double viewportWidth_ = 0.0;
    std::vector<int> visibleLines_;
};

KeyEventResult CodeEditorState::handleKey(const KeyEvent& ev) {
    if (!controller_)
        return KeyEventResult::Ignored;

    const bool shift = (ev.modifiers & ModShift) != 0;
    const bool ctrl = (ev.modifiers & ModCtrl) != 0;
    const bool alt = (ev.modifiers & ModAlt) != 0;
    const bool meta = (ev.modifiers & ModMeta) != 0;
    const bool readOnly = this->readOnly();

    // 可打印字符（含 Ctrl 组合键：宿主把 Ctrl+A 之类也作为 key==0 的文本送达）。
    if (ev.key == 0 && !ev.text.empty()) {
        if (ctrl || meta) {
            if (!ev.text.empty()) {
                const char c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(ev.text[0])));
                if (!shift && c == 'a') {
                    controller_->selectAll();
                    return KeyEventResult::Handled;
                }
                if (!shift && c == 'c') {
                    controller_->copySelection();
                    return KeyEventResult::Handled;
                }
                if (!shift && c == 'x') {
                    if (!readOnly)
                        controller_->cutSelection();
                    return KeyEventResult::Handled;
                }
                if (!shift && c == 'v') {
                    if (!readOnly)
                        controller_->pasteClipboard();
                    return KeyEventResult::Handled;
                }
                if (c == 'z') {
                    if (shift)
                        controller_->redo();
                    else
                        controller_->undo();
                    return KeyEventResult::Handled;
                }
                if (!shift && c == 'y') {
                    controller_->redo();
                    return KeyEventResult::Handled;
                }
                if (!shift && c == 'd') {
                    if (!readOnly)
                        controller_->duplicateLine();
                    return KeyEventResult::Handled;
                }
            }
            return KeyEventResult::Ignored;
        }
        if (alt)
            return KeyEventResult::Ignored;
        if (!readOnly)
            controller_->insertTypedText(ev.text);
        return KeyEventResult::Handled;
    }

    if (ev.key == static_cast<int>(KeyCode::Tab)) {
        if (!readOnly) {
            if (shift)
                controller_->unindentSelection();
            else
                controller_->indentSelection();
        }
        return KeyEventResult::Handled;
    }

    switch (static_cast<KeyCode>(ev.key)) {
    case KeyCode::Return:
        if (!readOnly)
            controller_->insertTypedText("\n");
        return KeyEventResult::Handled;
    case KeyCode::Backspace:
        if (readOnly)
            return KeyEventResult::Ignored;
        if (ctrl)
            controller_->deleteWordBackward();
        else
            controller_->deleteBackward();
        return KeyEventResult::Handled;
    case KeyCode::Delete:
        if (readOnly)
            return KeyEventResult::Ignored;
        if (ctrl)
            controller_->deleteWordForward();
        else
            controller_->deleteForward();
        return KeyEventResult::Handled;
    case KeyCode::Left:
        if (ctrl)
            controller_->moveWordLeft(shift);
        else
            controller_->moveLeft(shift);
        return KeyEventResult::Handled;
    case KeyCode::Right:
        if (ctrl)
            controller_->moveWordRight(shift);
        else
            controller_->moveRight(shift);
        return KeyEventResult::Handled;
    case KeyCode::Up:
        if (ctrl && shift) {
            if (!readOnly)
                controller_->moveLineUp();
        } else {
            controller_->moveUp(shift);
        }
        return KeyEventResult::Handled;
    case KeyCode::Down:
        if (ctrl && shift) {
            if (!readOnly)
                controller_->moveLineDown();
        } else {
            controller_->moveDown(shift);
        }
        return KeyEventResult::Handled;
    case KeyCode::Home:
        if (ctrl)
            controller_->moveToDocumentStart(shift);
        else
            controller_->moveToLineStart(shift);
        return KeyEventResult::Handled;
    case KeyCode::End:
        if (ctrl)
            controller_->moveToDocumentEnd(shift);
        else
            controller_->moveToLineEnd(shift);
        return KeyEventResult::Handled;
    case KeyCode::PageUp:
        controller_->movePage(-std::max(1, static_cast<int>(viewportHeight_ / std::max(1.0, lineHeight_))), shift);
        return KeyEventResult::Handled;
    case KeyCode::PageDown:
        controller_->movePage(std::max(1, static_cast<int>(viewportHeight_ / std::max(1.0, lineHeight_))), shift);
        return KeyEventResult::Handled;
    case KeyCode::Escape:
        controller_->setSelectionImmediately(controller_->selection().extent(),
                                             controller_->selection().extent());
        return KeyEventResult::Handled;
    default:
        break;
    }
    return KeyEventResult::Ignored;
}

int CodeEditorState::lineAtY(double y) const {
    if (visibleLines_.empty())
        return 0;
    int row = static_cast<int>(std::floor(y / std::max(1.0, lineHeight_)));
    row = std::clamp(row, 0, static_cast<int>(visibleLines_.size()) - 1);
    return visibleLines_[static_cast<std::size_t>(row)];
}

std::size_t CodeEditorState::offsetAtPosition(const Offset& position) const {
    const int line = lineAtY(position.dy);
    const std::string& text = controller_->getLineText(line);
    const double x = position.dx - gutterTotal_ - codePadX_;
    if (x <= 0.0)
        return controller_->getLineStartOffset(line);
    const int target = static_cast<int>(std::lround(x / std::max(1.0, charWidth_)));
    int col = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (col >= target)
            return controller_->getLineStartOffset(line) + i;
        if (text[i] == '\t') {
            col += std::max(1, controller_->tabSize());
            ++i;
        } else {
            ++col;
            i = utf8Next(text, i);
        }
    }
    return controller_->getLineStartOffset(line) + text.size();
}

bool CodeEditorState::hitTest(const Offset& position, std::size_t& offset) const {
    if (visibleLines_.empty())
        return false;
    if (position.dy < 0 || position.dy >= contentHeight_)
        return false;
    offset = offsetAtPosition(position);
    return true;
}

void CodeEditorState::onPointerDown(const PointerEvent& ev) {
    auto* w = static_cast<CodeEditor*>(widget());
    focusNode_->requestFocus();

    if (w->enableFolding() && ev.position.dx >= 0.0 && ev.position.dx < gutterFoldWidth_ &&
        visibleLines_.size() > 0 && ev.position.dy >= 0.0 &&
        ev.position.dy < contentHeight_) {
        const int line = lineAtY(ev.position.dy);
        if (controller_->foldings().count(line)) {
            controller_->toggleFold(line);
            dragging_ = false;
            return;
        }
    }

    // 行号槽：整行选中（与常见编辑器一致），不落光标到行首。
    if (ev.position.dx < gutterTotal_) {
        if (ev.position.dy >= 0.0 && ev.position.dy < contentHeight_ &&
            !visibleLines_.empty()) {
            const int line = lineAtY(ev.position.dy);
            const std::size_t start = controller_->getLineStartOffset(line);
            controller_->setSelectionImmediately(start, controller_->findLineEnd(start));
        }
        dragging_ = false;
        return;
    }

    std::size_t offset = 0;
    if (!hitTest(ev.position, offset))
        return;

    const bool shift = (ev.modifiers & ModShift) != 0;
    if (shift) {
        controller_->setSelectionImmediately(controller_->selection().base(), offset);
        dragAnchor_ = controller_->selection().base();
    } else {
        controller_->setSelection(TextSelection(offset, offset));
        dragAnchor_ = offset;
    }
    dragging_ = true;
}

void CodeEditorState::onPointerMove(const PointerEvent& ev) {
    if (!dragging_)
        return;
    std::size_t offset = 0;
    if (!hitTest(ev.position, offset))
        return;
    controller_->setSelectionImmediately(dragAnchor_, offset);
}

void CodeEditorState::scrollToLine(int line) {
    auto* w = static_cast<CodeEditor*>(widget());
    if (!w->height() || lineHeight_ <= 0.0)
        return;

    const int foldStart = controller_->getFoldStartForLine(line);
    const int target = foldStart >= 0 ? foldStart : line;
    auto it = std::find(visibleLines_.begin(), visibleLines_.end(), target);
    if (it == visibleLines_.end())
        return;

    const double row = static_cast<double>(it - visibleLines_.begin());
    const double y = row * lineHeight_;
    const double viewport = viewportHeight_ > 0.0 ? viewportHeight_ : *w->height();
    const double top = vScroll_.offset();
    const double bottom = top + viewport;
    if (y < top)
        vScroll_.jumpTo(y);
    else if (y + lineHeight_ > bottom)
        vScroll_.jumpTo(y + lineHeight_ - viewport);
}

void CodeEditorState::ensureCaretVisible() {
    auto* w = static_cast<CodeEditor*>(widget());
    scrollToLine(controller_->caretLine());

    const double x =
        gutterTotal_ + codePadX_ + columnX(controller_->caretColumn(), charWidth_);
    const double width = viewportWidth_ > 0.0 ? viewportWidth_ : w->width();
    const double left = hScroll_.offset();
    const double right = left + width;
    if (x < left)
        hScroll_.jumpTo(std::max(0.0, x - charWidth_));
    else if (x + 2.0 > right)
        hScroll_.jumpTo(x + 2.0 - width);
}

CodeEditorTheme CodeEditorState::editorTheme() const {
    auto* w = static_cast<CodeEditor*>(widget());
    if (w->editorTheme())
        return *w->editorTheme();
    return CodeEditorTheme::fromApp();
}

SharedPtr<Widget> CodeEditorState::build(BuildContext&) {
    auto* w = static_cast<CodeEditor*>(widget());
    if (w->width() > 0.0)
        return buildWithWidth(w->width());
    // width <= 0 ⇒ 水平占满父级给的空间：把父约束交给 LayoutBuilder，按可用
    // 最大宽度重建（Flutter 侧 0 / 负数同样表示「尺寸由父约束决定」）。
    return cui::make_shared<LayoutBuilder>(
        [this](BuildContext&, const BoxConstraints& c) {
            const double width =
                c.hasBoundedWidth() && c.maxWidth > 0.0 ? c.maxWidth : 420.0;
            return buildWithWidth(width);
        });
}

SharedPtr<Widget> CodeEditorState::buildWithWidth(double width) {
    auto* w = static_cast<CodeEditor*>(widget());
    const CodeEditorTheme ed = editorTheme();
    const CodeLanguage lang = resolveLanguage(w->filename(), w->codeLanguage());
    const char* family = monoFamily();
    const double fs = w->fontSize();
    const int tab = std::max(1, controller_->tabSize());
    const double padY = 2.0;

    lineHeight_ = Painter::textHeight(family, fs) + padY * 2.0;
    charWidth_ = Painter::measureTextWidth(family, "0", fs);
    codePadX_ = 10.0;
    viewportWidth_ = width;

    const int digits = static_cast<int>(
                           std::to_string(std::max(1, controller_->lineCount())).size()) +
                       1;
    gutterFoldWidth_ = w->enableFolding() ? 16.0 : 0.0;
    gutterNumberWidth_ = std::ceil(charWidth_ * digits) + 14.0;
    gutterWidth_ = (w->showLineNumbers() || w->enableFolding())
                       ? gutterFoldWidth_ + gutterNumberWidth_
                       : 0.0;
    gutterTotal_ = gutterWidth_ > 0.0 ? gutterWidth_ + 1.0 : 0.0;

    visibleLines_ = controller_->visibleLineIndices();
    contentHeight_ = lineHeight_ * static_cast<double>(std::max<std::size_t>(
                                       1, visibleLines_.size()));

    // 词法（块注释状态跨行延续）。只渲染可见行，但 token 化要跑全量。
    std::vector<std::vector<Token>> tokens(controller_->lines().size());
    bool inBlock = false;
    for (std::size_t i = 0; i < controller_->lines().size(); ++i)
        tokenizeLine(controller_->lines()[i], tab, lang, inBlock, tokens[i]);

    // 逐行宽（展开制表符后测量），用于横向内容宽度。
    std::vector<double> lineWidths(controller_->lines().size(), 0.0);
    double contentW = 0.0;
    for (std::size_t i = 0; i < controller_->lines().size(); ++i) {
        const std::string plain = expandTabs(controller_->lines()[i], tab);
        lineWidths[i] = Painter::measureTextWidth(family, plain, fs);
        contentW = std::max(contentW, lineWidths[i]);
    }
    const double availW = std::max(1.0, width - gutterTotal_);
    codeCellWidth_ = std::max(contentW + 2.0 * codePadX_, availW);
    contentWidth_ = gutterTotal_ + codeCellWidth_;

    // 空行沿用下一非空行的缩进层级（VS Code 口径）。
    std::vector<int> levels(controller_->lines().size(), 0);
    int nextLevel = 0;
    for (std::size_t i = controller_->lines().size(); i-- > 0;) {
        const std::string& line = controller_->lines()[i];
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            levels[i] = nextLevel;
        } else {
            levels[i] = indentLevelOfLine(line, tab);
            nextLevel = levels[i];
        }
    }

    const TextSelection selection = controller_->selection();
    const int caretLine = controller_->caretLine();
    const int caretCol = controller_->caretColumn();
    const bool showCaret = !w->readOnly() && focusNode_ && focusNode_->hasFocus();

    std::vector<SharedPtr<Widget>> rows;
    rows.reserve(visibleLines_.size());

    for (int line : visibleLines_) {
        const std::string& raw = controller_->lines()[static_cast<std::size_t>(line)];
        const std::size_t lineStart = controller_->getLineStartOffset(line);
        const std::string expanded = expandTabs(raw, tab);
        const int expandedBytes = static_cast<int>(expanded.size());

        const auto fit = controller_->foldings().find(line);
        const bool hasFold = fit != controller_->foldings().end();
        const bool folded = hasFold && fit->second.isFolded;

        const bool active = w->highlightLine() ? (*w->highlightLine() == line)
                                               : (w->readOnly() ? false : caretLine == line);

        std::vector<SharedPtr<Widget>> overlay;

        // 缩进参考线
        if (w->showIndentGuides() && levels[static_cast<std::size_t>(line)] > 0) {
            for (int level = 1; level <= levels[static_cast<std::size_t>(line)]; ++level) {
                auto guide = cui::make_shared<Container>(nullptr);
                guide->width(1.0).height(lineHeight_)
                    .decoration(BoxDecoration{ed.indentGuide});
                auto pos = cui::make_shared<Positioned>(guide);
                pos->left = codePadX_ + static_cast<double>(level) * tab * charWidth_;
                pos->hasLeft = true;
                pos->top = 0.0;
                pos->hasTop = true;
                pos->width = 1.0;
                pos->hasWidth = true;
                pos->height = lineHeight_;
                pos->hasHeight = true;
                overlay.push_back(pos);
            }
        }

        // 查找高亮
        for (const auto& hl : controller_->searchHighlights()) {
            const std::size_t start = std::max(hl.start, lineStart);
            const std::size_t end = std::min(hl.end, lineStart + raw.size());
            if (start >= end)
                continue;
            const int c1 = visualColumnInLine(raw, start - lineStart, tab);
            const int c2 = visualColumnInLine(raw, end - lineStart, tab);
            auto box = cui::make_shared<Container>(nullptr);
            box->width(std::max(2.0, (c2 - c1) * charWidth_)).height(lineHeight_)
                .decoration(BoxDecoration{hl.isCurrentMatch ? ed.searchCurrent
                                                           : ed.searchMatch});
            auto pos = cui::make_shared<Positioned>(box);
            pos->left = codePadX_ + c1 * charWidth_;
            pos->hasLeft = true;
            pos->top = 0.0;
            pos->hasTop = true;
            pos->width = std::max(2.0, (c2 - c1) * charWidth_);
            pos->hasWidth = true;
            pos->height = lineHeight_;
            pos->hasHeight = true;
            overlay.push_back(pos);
        }

        // 选区（跟换行时在行尾补一小块）
        if (!selection.isCollapsed()) {
            const std::size_t lineEnd = lineStart + raw.size();
            const std::size_t start = std::max(selection.start(), lineStart);
            const std::size_t end = std::min(selection.end(), lineEnd);
            if (start < end) {
                const int c1 = visualColumnInLine(raw, start - lineStart, tab);
                const int c2 = visualColumnInLine(raw, end - lineStart, tab);
                auto box = cui::make_shared<Container>(nullptr);
                box->width(std::max(2.0, (c2 - c1) * charWidth_)).height(lineHeight_)
                    .decoration(BoxDecoration{ed.selection});
                auto pos = cui::make_shared<Positioned>(box);
                pos->left = codePadX_ + c1 * charWidth_;
                pos->hasLeft = true;
                pos->top = 0.0;
                pos->hasTop = true;
                pos->width = std::max(2.0, (c2 - c1) * charWidth_);
                pos->hasWidth = true;
                pos->height = lineHeight_;
                pos->hasHeight = true;
                overlay.push_back(pos);
            } else if (selection.start() <= lineEnd && selection.end() > lineEnd) {
                auto box = cui::make_shared<Container>(nullptr);
                box->width(charWidth_ * 0.6).height(lineHeight_)
                    .decoration(BoxDecoration{ed.selection});
                auto pos = cui::make_shared<Positioned>(box);
                pos->left = codePadX_ + expandedBytes * charWidth_;
                pos->hasLeft = true;
                pos->top = 0.0;
                pos->hasTop = true;
                pos->width = charWidth_ * 0.6;
                pos->hasWidth = true;
                pos->height = lineHeight_;
                pos->hasHeight = true;
                overlay.push_back(pos);
            }
        }

        // 词法着色文本
        std::vector<SharedPtr<TextSpan>> spans;
        spans.reserve(tokens[static_cast<std::size_t>(line)].size());
        for (const Token& t : tokens[static_cast<std::size_t>(line)])
            spans.push_back(TextSpan::make(t.text, tokenStyle(t.kind, fs, family, ed)));
        auto root = cui::make_shared<TextSpan>("", TextStyle().size(fs).family(family));
        root->children = std::move(spans);
        auto code = cui::make_shared<RichText>(root);
        auto codePos = cui::make_shared<Positioned>(code);
        codePos->left = codePadX_;
        codePos->hasLeft = true;
        codePos->top = padY;
        codePos->hasTop = true;
        overlay.push_back(codePos);

        // 光标
        if (showCaret && line == caretLine) {
            auto caret = cui::make_shared<Container>(nullptr);
            caret->width(1.6).height(std::max(2.0, lineHeight_ - 6.0))
                .decoration(BoxDecoration{ed.caret});
            auto pos = cui::make_shared<Positioned>(caret);
            pos->left = codePadX_ + caretCol * charWidth_;
            pos->hasLeft = true;
            pos->top = 3.0;
            pos->hasTop = true;
            pos->width = 1.6;
            pos->hasWidth = true;
            pos->height = std::max(2.0, lineHeight_ - 6.0);
            pos->hasHeight = true;
            overlay.push_back(pos);
        }

        auto cell = cui::make_shared<Container>(cui::make_shared<Stack>(std::move(overlay)));
        cell->width(codeCellWidth_).height(lineHeight_)
            .decoration(BoxDecoration{active ? ed.activeLine
                                              : Color::fromRGBA(0, 0, 0, 0.0)});

        std::vector<SharedPtr<Widget>> cells;
        if (gutterWidth_ > 0.0) {
            std::vector<SharedPtr<Widget>> gutterCells;
            if (w->enableFolding()) {
                const std::string icon = hasFold ? (folded ? "▸" : "▾") : "";
                auto iconText = cui::make_shared<Text>(
                    icon, TextStyle().size(std::max(9.0, fs - 1.0)).family(family)
                              .fgColor(ed.foldIcon));
                auto iconBox = cui::make_shared<Container>(iconText);
                iconBox->width(gutterFoldWidth_).height(lineHeight_)
                    .alignment(0.0, 0.0);
                gutterCells.push_back(iconBox);
            }
            if (w->showLineNumbers()) {
                auto num = cui::make_shared<Text>(
                    std::to_string(line + 1),
                    TextStyle().size(std::max(9.0, fs - 1.0)).family(family)
                        .fgColor(active ? ed.gutterActiveForeground : ed.gutterForeground));
                auto numBox = cui::make_shared<Container>(num);
                numBox->width(gutterNumberWidth_).height(lineHeight_)
                    .padding(EdgeInsets::symmetric(4.0, 0.0))
                    .alignment(1.0, 0.0);
                gutterCells.push_back(numBox);
            }
            auto gutterRow = cui::make_shared<Row>(std::move(gutterCells));
            gutterRow->setMainAxisSize(MainAxisSize::Min);
            gutterRow->setCrossAxisAlignment(CrossAxisAlignment::Center);

            auto gutter = cui::make_shared<Container>(gutterRow);
            gutter->width(gutterWidth_).height(lineHeight_)
                .decoration(BoxDecoration{ed.gutterBackground});
            cells.push_back(gutter);

            auto separator = cui::make_shared<Container>(nullptr);
            separator->width(1.0).height(lineHeight_)
                .decoration(BoxDecoration{ed.border});
            cells.push_back(separator);
        }
        cells.push_back(cell);

        auto row = cui::make_shared<Row>(std::move(cells));
        row->setMainAxisSize(MainAxisSize::Min);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        rows.push_back(row);
    }

    auto column = cui::make_shared<Column>(std::move(rows));
    column->setMainAxisSize(MainAxisSize::Min);
    column->setCrossAxisAlignment(CrossAxisAlignment::Start);

    auto contentBox = cui::make_shared<Container>(column);
    contentBox->width(contentWidth_).height(contentHeight_);

    auto interactive = cui::make_shared<Listener>(
        [this](const PointerEvent& ev) { onPointerDown(ev); },
        [this](const PointerEvent& ev) { onPointerMove(ev); },
        [this](const PointerEvent&) { dragging_ = false; }, nullptr, nullptr,
        contentBox);

    auto hScroller = cui::make_shared<SingleChildScrollView>(
        &hScroll_, interactive, Axis::Horizontal, false);

    SharedPtr<Widget> body = hScroller;

    if (w->height()) {
        auto vBox = cui::make_shared<Container>(cui::make_shared<SingleChildScrollView>(
            &vScroll_, hScroller, Axis::Vertical, true));
        vBox->height(*w->height());
        body = cui::make_shared<Scrollbar>(&vScroll_, vBox);
        viewportHeight_ = *w->height();
    } else {
        viewportHeight_ = contentHeight_;
    }

    auto bodyBox = cui::make_shared<Container>(body);
    bodyBox->padding(EdgeInsets::symmetric(0.0, 6.0))
        .decoration(BoxDecoration{ed.background});

    std::vector<SharedPtr<Widget>> chrome;
    const std::string badgeText =
        w->language().empty() ? languageLabel(lang) : w->language();
    const bool hasTitle = w->showTitleBar() &&
                          (!w->filename().empty() || !badgeText.empty() || !w->readOnly());

    if (hasTitle) {
        std::vector<SharedPtr<Widget>> bar;
        if (!w->filename().empty()) {
            auto dot = cui::make_shared<Container>(nullptr);
            dot->width(7.0).height(7.0).decoration(BoxDecoration{ed.titleDot, 3.5});
            bar.push_back(dot);
            bar.push_back(cui::make_shared<SizedBox>(8.0, 0.0));
            bar.push_back(cui::make_shared<Text>(
                w->filename(), TextStyle().size(fs).family(family)
                                   .fgColor(ed.titleForeground)));
        }
        bar.push_back(cui::make_shared<Spacer>());
        if (!w->readOnly()) {
            const std::string position = "Ln " + std::to_string(caretLine + 1) + ", Col " +
                                         std::to_string(caretCol + 1);
            bar.push_back(cui::make_shared<Text>(
                position, TextStyle().size(std::max(9.0, fs - 1.0)).family(family)
                              .fgColor(ed.titleForeground)));
            bar.push_back(cui::make_shared<SizedBox>(10.0, 0.0));
        }
        if (!badgeText.empty()) {
            auto badge = cui::make_shared<Container>(cui::make_shared<Text>(
                badgeText, TextStyle().size(std::max(9.0, fs - 2.0)).family(family)
                               .fgColor(ed.badgeForeground)));
            badge->padding(EdgeInsets::symmetric(7.0, 2.0))
                .decoration(BoxDecoration{ed.badgeBackground, 3.0});
            bar.push_back(badge);
        }
        auto barRow = cui::make_shared<Row>(std::move(bar));
        barRow->setMainAxisSize(MainAxisSize::Max);
        barRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
        auto barBox = cui::make_shared<Container>(barRow);
        barBox->padding(EdgeInsets::symmetric(10.0, 7.0))
            .decoration(BoxDecoration{ed.titleBackground});
        chrome.push_back(barBox);
    }
    chrome.push_back(bodyBox);

    auto inner = cui::make_shared<Column>(std::move(chrome));
    inner->setMainAxisSize(MainAxisSize::Min);
    inner->setCrossAxisAlignment(CrossAxisAlignment::Start);

    auto outer = cui::make_shared<Container>(cui::make_shared<ClipRRect>(5.0, inner));
    outer->width(width)
        .decoration(BoxDecoration{ed.background, 6.0, 1.0, ed.border});
    return outer;
}

std::unique_ptr<State> CodeEditor::createState() { return std::make_unique<CodeEditorState>(); }

}  // namespace cui_core
