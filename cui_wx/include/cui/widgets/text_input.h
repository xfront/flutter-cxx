#pragma once
/// @file text_input.h
/// @brief 上游 文本输入层（TextEditingController / EditableText / TextField）。
///
/// 对应 上游源码：
///   上游 services/text_editing.dart
///   上游 widgets/editable_text.dart
///   上游 material/text_field.dart
///
/// 接入点：宿主键盘事件经 宿主桥::onKeyDown 翻译为 KeyEvent，路由到
/// FocusManager::primaryFocus() 的 onKeyEvent（由 EditableTextState 注册）。
/// 文本/选区/组合态全部由 TextEditingController 持有，键盘与 IME 都只改它。

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/platform/event.h"  // KeyCode, KeyModifier
#include "cui/framework/focus.h"
#include "cui/framework/listener.h"
#include "cui/foundation/text_style.h"

namespace cui {

namespace detail {
/// 进程内剪贴板（headless 友好；真实窗口可替换为系统剪贴板）。
/// 定义在 src/widgets/text_input.cpp；单行输入（EditableText 的 Ctrl+C/X/V）与
/// CodeEditor 共用这一个缓冲区，所以两边可以互相粘帖。
std::string& clipboardBuf();
}  // namespace detail

// ── TextRange（组合区间）─────────────────────────────────────
class TextRange {
public:
    static TextRange empty() { return TextRange(0, 0); }
    TextRange() = default;
    TextRange(std::size_t start, std::size_t end) : start_(start), end_(end) {}
    std::size_t start() const { return start_; }
    std::size_t end() const { return end_; }
    void setStart(std::size_t v) { start_ = v; }
    void setEnd(std::size_t v) { end_ = v; }
    bool isValid() const { return start_ < end_; }
    bool isCollapsed() const { return start_ == end_; }
    bool operator==(const TextRange& o) const { return start_ == o.start_ && end_ == o.end_; }
    bool operator!=(const TextRange& o) const { return !(*this == o); }
private:
    std::size_t start_ = 0, end_ = 0;
};

// ── TextSelection（base/extent）─────────────────────────────
class TextSelection {
public:
    TextSelection() = default;
    TextSelection(std::size_t base, std::size_t extent) : base_(base), extent_(extent) {}
    std::size_t base() const { return base_; }
    std::size_t extent() const { return extent_; }
    void setBase(std::size_t v) { base_ = v; }
    void setExtent(std::size_t v) { extent_ = v; }
    std::size_t start() const { return base_ < extent_ ? base_ : extent_; }
    std::size_t end() const { return base_ < extent_ ? extent_ : base_; }
    bool isCollapsed() const { return base_ == extent_; }
    void collapse(std::size_t pos) { base_ = extent_ = pos; }
    void setRange(std::size_t a, std::size_t b) { base_ = a; extent_ = b; }
    bool operator==(const TextSelection& o) const { return base_ == o.base_ && extent_ == o.extent_; }
    bool operator!=(const TextSelection& o) const { return !(*this == o); }
private:
    std::size_t base_ = 0, extent_ = 0;
};

// ── TextEditingValue ────────────────────────────────────────
class TextEditingValue {
public:
    TextEditingValue() = default;
    TextEditingValue(std::string text, TextSelection sel,
                     TextRange composing = TextRange::empty())
        : text_(std::move(text)), selection_(sel), composing_(composing) {}
    const std::string& text() const { return text_; }
    const TextSelection& selection() const { return selection_; }
    const TextRange& composing() const { return composing_; }
    void setText(std::string t) { text_ = std::move(t); }
    void setSelection(TextSelection s) { selection_ = s; }
    void setComposing(TextRange r) { composing_ = r; }
    bool operator==(const TextEditingValue& o) const {
        return text_ == o.text_ && selection_ == o.selection_ && composing_ == o.composing_;
    }
    bool operator!=(const TextEditingValue& o) const { return !(*this == o); }
private:
    std::string text_;
    TextSelection selection_;
    TextRange composing_;
};

// ── TextEditingController ───────────────────────────────────
class TextEditingController {
public:
    explicit TextEditingController(std::string initial = "")
        : value_(initial, TextSelection(0, 0)) {
        std::size_t n = value_.text().size();
        value_.setSelection(TextSelection(n, n));
    }

    const TextEditingValue& value() const { return value_; }
    void setValue(TextEditingValue v) {
        value_ = std::move(v);
        notifyListeners();
    }
    const std::string& text() const { return value_.text(); }
    void setText(const std::string& t);
    TextSelection selection() const { return value_.selection(); }
    void setSelection(TextSelection s) {
        value_.setSelection(s);
        notifyListeners();
    }
    TextRange composing() const { return value_.composing(); }
    bool hasFocus() const { return hasFocus_; }
    void setHasFocus(bool f) {
        if (f == hasFocus_) return;
        hasFocus_ = f;
        notifyListeners();
    }
    bool isComposing() const { return value_.composing().isValid(); }

    /// 编辑操作（文本/选区以 UTF-8 字节偏移计）。
    void insert(const std::string& s);
    void replaceSelection(const std::string& s);
    void deleteCharBackward();
    void deleteCharForward();
    void moveCursorLeft(bool select);
    void moveCursorRight(bool select);
    void moveCursorToStart(bool select);
    void moveCursorToEnd(bool select);
    void selectAll();
    void clear();

    /// IME：组合进行中（preedit 文本逐帧更新）。
    void updateComposing(const std::string& preedit);
    /// IME：组合结束（commit 文本替换组合区间）。
    void commitComposition(const std::string& committed);

    /// 注册监听，返回监听 id（可用 removeListener 注销）。
    ///
    /// 控件必须在 dispose 时注销：否则控制器（可能比 State 活得久）再通知时
    /// 会回调到已销毁的 State 上 → 野指针。
    int addListener(std::function<void()> fn) {
        int id = nextListenerId_++;
        listeners_.emplace_back(id, std::move(fn));
        return id;
    }
    /// 注销监听（按 addListener 返回的 id）。
    void removeListener(int id) {
        for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
            if (it->first == id) {
                listeners_.erase(it);
                return;
            }
        }
    }
    /// 在场监听数（上游 ChangeNotifier 没有这个读取口；控件卸载后仍为 0 是
    /// 「监听已摘干净」的直接证据，测试用它钉住，别等 ASan 报悬挂回调）。
    std::size_t listenerCount() const { return listeners_.size(); }
    void notifyListeners() {
        // 复制一份再回调：回调里可能会增删监听（自注销）。
        auto copy = listeners_;
        for (auto& kv : copy)
            if (kv.second) kv.second();
    }

private:
    static std::size_t prevBoundary(const std::string& s, std::size_t pos);
    static std::size_t nextBoundary(const std::string& s, std::size_t pos);

    TextEditingValue value_;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
    int nextListenerId_ = 1;
    bool hasFocus_ = false;
};

// ── EditableText ────────────────────────────────────────────
class EditableText : public StatefulWidget {
public:
    EditableText(SharedPtr<TextEditingController> controller,
                 TextStyle style = TextStyle{},
                 TextAlign textAlign = TextAlign::Left,
                 std::string placeholder = std::string(),
                 bool obscureText = false,
                 bool autofocus = false,
                 KeyPtr key = nullptr,
                 SharedPtr<FocusNode> focusNode = nullptr,
                 std::function<void(const std::string&)> onChanged = nullptr,
                 std::function<void(const std::string&)> onSubmitted = nullptr)
        : StatefulWidget(std::move(key)),
          controller_(std::move(controller)),
          style_(std::move(style)),
          textAlign_(textAlign),
          placeholder_(std::move(placeholder)),
          obscureText_(obscureText),
          autofocus_(autofocus),
          onChanged_(std::move(onChanged)),
          onSubmitted_(std::move(onSubmitted)) {
        // 上游 `EditableText.focusNode`：外部传入的节点可跨重建持久化
        // （ComboBox 要在输入框外面挂 Focus/监听，需要拿到同一个节点）。
        focusNode_ = focusNode ? std::move(focusNode) : cui::make_shared<FocusNode>();
    }

    std::unique_ptr<State> createState() override;
    SharedPtr<TextEditingController> controller() const { return controller_; }
    const TextStyle& style() const { return style_; }
    TextAlign textAlign() const { return textAlign_; }
    const std::string& placeholder() const { return placeholder_; }
    bool obscureText() const { return obscureText_; }
    bool autofocus() const { return autofocus_; }
    SharedPtr<FocusNode> focusNode() const { return focusNode_; }
    /// 上游 `EditableText.onChanged`：文本变化时回调当前全文。
    const std::function<void(const std::string&)>& onChanged() const { return onChanged_; }
    /// 上游 `EditableText.onSubmitted`：回车时回调（给了才消费回车键）。
    const std::function<void(const std::string&)>& onSubmitted() const {
        return onSubmitted_;
    }

private:
    SharedPtr<TextEditingController> controller_;
    TextStyle style_;
    TextAlign textAlign_;
    std::string placeholder_;
    bool obscureText_;
    bool autofocus_;
    std::function<void(const std::string&)> onChanged_;
    std::function<void(const std::string&)> onSubmitted_;
    SharedPtr<FocusNode> focusNode_;
};

// ── TextField ───────────────────────────────────────────────
struct InputDecoration {
    std::string labelText;
    std::string hintText;
    std::string helperText;
    Color borderColor = Color::fromRGB(0.62, 0.62, 0.62);
    double borderRadius = 4.0;
    double borderWidth = 1.0;
    Color fillColor = Color::fromRGBA(0, 0, 0, 0);
    bool filled = false;
};

class TextField : public StatelessWidget {
public:
    TextField(SharedPtr<TextEditingController> controller,
              InputDecoration decoration = InputDecoration{},
              TextStyle style = TextStyle{},
              TextAlign textAlign = TextAlign::Left,
              bool obscureText = false,
              bool autofocus = false,
              KeyPtr key = nullptr,
              SharedPtr<FocusNode> focusNode = nullptr,
              std::function<void(const std::string&)> onChanged = nullptr,
              std::function<void(const std::string&)> onSubmitted = nullptr)
        : StatelessWidget(std::move(key)),
          controller_(std::move(controller)),
          decoration_(std::move(decoration)),
          style_(std::move(style)),
          textAlign_(textAlign),
          obscureText_(obscureText),
          autofocus_(autofocus),
          focusNode_(std::move(focusNode)),
          onChanged_(std::move(onChanged)),
          onSubmitted_(std::move(onSubmitted)) {}

    SharedPtr<Widget> build(BuildContext& context) override;
    SharedPtr<TextEditingController> controller() const { return controller_; }
    /// 上游 `TextField.focusNode` / `onChanged` / `onSubmitted`。
    const SharedPtr<FocusNode>& focusNode() const { return focusNode_; }
    const std::function<void(const std::string&)>& onChanged() const { return onChanged_; }
    const std::function<void(const std::string&)>& onSubmitted() const {
        return onSubmitted_;
    }

private:
    SharedPtr<TextEditingController> controller_;
    InputDecoration decoration_;
    TextStyle style_;
    TextAlign textAlign_;
    bool obscureText_;
    bool autofocus_;
    SharedPtr<FocusNode> focusNode_;
    std::function<void(const std::string&)> onChanged_;
    std::function<void(const std::string&)> onSubmitted_;
};

}  // namespace cui_core
