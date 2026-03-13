/// @file text_input.cpp — 上游 文本输入层实现。

#include "cui/widgets/text_input.h"

#include <algorithm>
#include <cctype>

#include "cui/framework/build_owner.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/render_editable.h"
#include "cui/widgets/basic.h"      // Column, EdgeInsets
#include "cui/widgets/container.h"  // Container, BoxDecoration
#include "cui/widgets/text.h"       // Text

namespace cui {
namespace {

/// UTF-8 首字节 → 该码点字节数。
inline std::size_t utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

}  // namespace

namespace detail {

/// 进程内剪贴板（声明见 text_input.h）。
std::string& clipboardBuf() {
    static std::string buf;
    return buf;
}

}  // namespace detail

// ── TextEditingController ─────────────────────────────────────────────────
std::size_t TextEditingController::prevBoundary(const std::string& s, std::size_t pos) {
    if (pos == 0) return 0;
    std::size_t i = pos;
    while (i > 0) {
        --i;
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
            break;  // 到达码点首字节
    }
    return i;
}

std::size_t TextEditingController::nextBoundary(const std::string& s, std::size_t pos) {
    if (pos >= s.size()) return s.size();
    std::size_t len = utf8Len(static_cast<unsigned char>(s[pos]));
    std::size_t next = pos + len;
    return next > s.size() ? s.size() : next;
}

void TextEditingController::setText(const std::string& t) {
    value_.setText(t);
    std::size_t n = t.size();
    value_.setSelection(TextSelection(n, n));
    value_.setComposing(TextRange::empty());
    notifyListeners();
}

void TextEditingController::insert(const std::string& s) {
    TextSelection sel = value_.selection();
    std::string t = value_.text();
    std::size_t a = sel.start();
    std::size_t b = sel.end();
    t.replace(a, b - a, s);
    std::size_t caret = a + s.size();
    value_.setText(t);
    value_.setSelection(TextSelection(caret, caret));
    value_.setComposing(TextRange::empty());
    notifyListeners();
}

void TextEditingController::replaceSelection(const std::string& s) {
    insert(s);
}

void TextEditingController::deleteCharBackward() {
    TextSelection sel = value_.selection();
    std::string t = value_.text();
    if (!sel.isCollapsed()) {
        t.replace(sel.start(), sel.end() - sel.start(), "");
        value_.setText(t);
        value_.setSelection(TextSelection(sel.start(), sel.start()));
    } else {
        if (sel.base() == 0)
            return;
        std::size_t prev = prevBoundary(t, sel.base());
        t.replace(prev, sel.base() - prev, "");
        value_.setText(t);
        value_.setSelection(TextSelection(prev, prev));
    }
    value_.setComposing(TextRange::empty());
    notifyListeners();
}

void TextEditingController::deleteCharForward() {
    TextSelection sel = value_.selection();
    std::string t = value_.text();
    if (!sel.isCollapsed()) {
        t.replace(sel.start(), sel.end() - sel.start(), "");
        value_.setText(t);
        value_.setSelection(TextSelection(sel.start(), sel.start()));
    } else {
        if (sel.extent() >= t.size())
            return;
        std::size_t next = nextBoundary(t, sel.extent());
        t.replace(sel.extent(), next - sel.extent(), "");
        value_.setText(t);
        value_.setSelection(TextSelection(sel.extent(), sel.extent()));
    }
    value_.setComposing(TextRange::empty());
    notifyListeners();
}

void TextEditingController::moveCursorLeft(bool select) {
    const std::string& t = value_.text();
    TextSelection sel = value_.selection();
    if (!select) {
        if (!sel.isCollapsed()) {
            std::size_t s = sel.start();
            value_.setSelection(TextSelection(s, s));
        } else if (sel.base() > 0) {
            std::size_t prev = prevBoundary(t, sel.base());
            value_.setSelection(TextSelection(prev, prev));
        }
    } else {
        std::size_t extent = sel.extent();
        std::size_t pos = (extent > 0) ? prevBoundary(t, extent) : 0;
        value_.setSelection(TextSelection(sel.base(), pos));
    }
    notifyListeners();
}

void TextEditingController::moveCursorRight(bool select) {
    const std::string& t = value_.text();
    TextSelection sel = value_.selection();
    if (!select) {
        if (!sel.isCollapsed()) {
            std::size_t e = sel.end();
            value_.setSelection(TextSelection(e, e));
        } else if (sel.extent() < t.size()) {
            std::size_t next = nextBoundary(t, sel.extent());
            value_.setSelection(TextSelection(next, next));
        }
    } else {
        std::size_t extent = sel.extent();
        std::size_t pos = (extent < t.size()) ? nextBoundary(t, extent) : t.size();
        value_.setSelection(TextSelection(sel.base(), pos));
    }
    notifyListeners();
}

void TextEditingController::moveCursorToStart(bool select) {
    TextSelection sel = value_.selection();
    if (select)
        value_.setSelection(TextSelection(sel.base(), 0));
    else
        value_.setSelection(TextSelection(0, 0));
    notifyListeners();
}

void TextEditingController::moveCursorToEnd(bool select) {
    const std::string& t = value_.text();
    std::size_t n = t.size();
    TextSelection sel = value_.selection();
    if (select)
        value_.setSelection(TextSelection(sel.base(), n));
    else
        value_.setSelection(TextSelection(n, n));
    notifyListeners();
}

void TextEditingController::selectAll() {
    std::size_t n = value_.text().size();
    value_.setSelection(TextSelection(0, n));
    notifyListeners();
}

void TextEditingController::clear() {
    value_.setText("");
    value_.setSelection(TextSelection(0, 0));
    value_.setComposing(TextRange::empty());
    notifyListeners();
}

void TextEditingController::updateComposing(const std::string& preedit) {
    TextSelection sel = value_.selection();
    std::string t = value_.text();
    std::size_t a = value_.composing().isValid() ? value_.composing().start() : sel.start();
    std::size_t b = value_.composing().isValid() ? value_.composing().end() : sel.start();
    t.replace(a, b - a, preedit);
    std::size_t endp = a + preedit.size();
    value_.setText(t);
    value_.setComposing(TextRange(a, endp));
    value_.setSelection(TextSelection(endp, endp));
    notifyListeners();
}

void TextEditingController::commitComposition(const std::string& committed) {
    std::string t = value_.text();
    std::size_t a, b;
    if (value_.composing().isValid()) {
        a = value_.composing().start();
        b = value_.composing().end();
    } else {
        TextSelection sel = value_.selection();
        a = sel.start();
        b = sel.end();
    }
    t.replace(a, b - a, committed);
    std::size_t endp = a + committed.size();
    value_.setText(t);
    value_.setSelection(TextSelection(endp, endp));
    value_.setComposing(TextRange::empty());
    notifyListeners();
}

// ── EditableText 内部叶子节点 ───────────────────────────────────────────────
class EditableTextLeaf : public LeafRenderObjectWidget {
public:
    EditableTextLeaf(SharedPtr<TextEditingController> c, TextStyle style,
                     TextAlign align, std::string placeholder, bool obscure)
        : controller_(std::move(c)), style_(std::move(style)), align_(align),
          placeholder_(std::move(placeholder)), obscure_(obscure) {}

    RenderObject* createRenderObject() override {
        auto* ro = new RenderEditable(style_, align_, placeholder_, obscure_);
        syncTo(ro);
        return ro;
    }
    void updateRenderObject(RenderObject* ro) override {
        syncTo(dynamic_cast<RenderEditable*>(ro));
    }

private:
    void syncTo(RenderEditable* ro) {
        if (!ro) return;
        const TextSelection& sel = controller_->selection();
        ro->setText(controller_->text());
        ro->setSelection(sel.base(), sel.extent());
        ro->setHasFocus(controller_->hasFocus());
        ro->setStyle(style_);
        ro->setTextAlign(align_);
        ro->setPlaceholder(placeholder_);
        ro->setObscure(obscure_);
    }
    SharedPtr<TextEditingController> controller_;
    TextStyle style_;
    TextAlign align_;
    std::string placeholder_;
    bool obscure_;
};

// ── EditableTextState ──────────────────────────────────────────────────────
class EditableTextState : public State {
public:
    void initState() override {
        auto* w = static_cast<EditableText*>(widget());
        focusNode_ = w->focusNode();

        BuildOwner* bo = nullptr;
        Element* elem = nullptr;
        if (BuildContext* ctx = context())
            if ((elem = ctx->element()) != nullptr)
                bo = elem->owner();
        if (bo)
            focusNode_->setManager(&bo->focusManager());
        // 键事件沿祖先链上溯（FocusManager::sendKeyEvent）要从节点找回自己的
        // Element，故这里也记一份 context（上游是直接把 EditableText 包进
        // 一个 Focus 控件，由 _FocusState 负责登记）。
        focusNode_->setContext(elem);

        focusNode_->onFocusChange = [this](bool f) { controller()->setHasFocus(f); };
        focusNode_->onKeyEvent = [this](const KeyEvent& ev) { return handleKey(ev); };
        focusNode_->onIMCommit = [this](const std::string& s) { controller()->commitComposition(s); };
        focusNode_->onPreedit = [this](const std::string& s) { controller()->updateComposing(s); };
        focusNode_->setEditable(true);  // 防止祖先滚动容器抢焦点

        // 注意：autofocus 的 requestFocus 会经 onFocusChange → setHasFocus →
        // notifyListeners 同步触发 setState；注册监听必须在 requestFocus 之后，
        // 否则会在 mount 过程中标记脏元素导致 render 子树二次 adopt（断言崩溃）。
        if (w->autofocus())
            focusNode_->requestFocus();
        lastText_ = controller()->text();
        listenerId_ = controller()->addListener([this]() {
            // 上游 `_updateEditingValue`：只有文本真的变了才回调 onChanged
            // （焦点、选区、组合态的变化都不算）。
            const std::string now = controller()->text();
            if (now != lastText_) {
                lastText_ = now;
                if (auto* w = static_cast<EditableText*>(widget()))
                    if (w->onChanged())
                        w->onChanged()(now);
            }
            setState();
        });
    }

    /// 卸载时注销控制器监听：控制器可能比 State 活得久（页面被切走但控件仍被
    /// 引用），之后再来一次通知就会回调到已销毁的 State 上 → 野指针崩溃。
    void dispose() override {
        if (listenerId_ > 0) {
            if (auto* w = static_cast<EditableText*>(widget()))
                if (auto ctrl = w->controller())
                    ctrl->removeListener(listenerId_);
            listenerId_ = 0;
        }
        // 焦点在本控件上时主动释放，避免 FocusManager 持有已销毁节点。
        if (focusNode_ && focusNode_->hasFocus())
            focusNode_->unfocus();
        if (focusNode_)
            focusNode_->setContext(nullptr);  // Element 先于 Widget 死（同 FocusState）
        focusNode_ = nullptr;
        State::dispose();
    }

    void didUpdateWidget(cui::Widget* oldWidget) override {
        (void)oldWidget;
        auto* w = static_cast<EditableText*>(widget());
        if (w->focusNode() != focusNode_) {
            focusNode_ = w->focusNode();
            if (BuildContext* ctx = context())
                if (Element* e = ctx->element()) {
                    if (BuildOwner* bo = e->owner())
                        focusNode_->setManager(&bo->focusManager());
                    focusNode_->setContext(e);
                }
            focusNode_->onKeyEvent = [this](const KeyEvent& ev) { return handleKey(ev); };
            focusNode_->onIMCommit = [this](const std::string& s) { controller()->commitComposition(s); };
            focusNode_->onPreedit = [this](const std::string& s) { controller()->updateComposing(s); };
            focusNode_->setEditable(true);
        }
    }

    SharedPtr<Widget> build(BuildContext&) override {
        auto leaf = cui::make_shared<EditableTextLeaf>(
            controller(), style(), textAlign(), placeholder(), obscure());
        return cui::make_shared<Listener>(
            [this](const PointerEvent& ev) { onTap(ev); },
            nullptr, nullptr, nullptr, nullptr, leaf);
    }

private:
    SharedPtr<TextEditingController> controller() const {
        return static_cast<EditableText*>(widget())->controller();
    }
    const TextStyle& style() const { return static_cast<EditableText*>(widget())->style(); }
    TextAlign textAlign() const { return static_cast<EditableText*>(widget())->textAlign(); }
    const std::string& placeholder() const { return static_cast<EditableText*>(widget())->placeholder(); }
    bool obscure() const { return static_cast<EditableText*>(widget())->obscureText(); }

    RenderEditable* findEditable() const {
        Element* e = context() ? context()->element() : nullptr;
        if (!e) return nullptr;
        RenderEditable* found = nullptr;
        std::function<void(Element*)> visit = [&](Element* cur) {
            if (!cur || found) return;
            if (auto* roe = dynamic_cast<RenderObjectElement*>(cur))
                if (auto* re = dynamic_cast<RenderEditable*>(roe->renderObject())) {
                    found = re;
                    return;
                }
            cur->visitChildren(visit);
        };
        visit(e);
        return found;
    }

    void onTap(const PointerEvent& ev) {
        focusNode_->requestFocus();
        if (RenderEditable* re = findEditable()) {
            Offset local = re->globalToLocal(Offset{ev.position.dx, ev.position.dy});
            std::size_t idx = re->getPositionForX(local.dx);
            controller()->setSelection(TextSelection(idx, idx));
        }
    }

    KeyEventResult handleKey(const KeyEvent& ev) {
        const int mod = ev.modifiers;
        bool ctrl = (mod & ModCtrl) != 0;
        bool shift = (mod & ModShift) != 0;
        bool meta = (mod & ModMeta) != 0;

        // 可打印字符（Ctrl/Meta 组合键不在这里处理）。
        if (ev.key == 0 && !ev.text.empty() && !ctrl && !meta) {
            controller()->insert(ev.text);
            return KeyEventResult::Handled;
        }

        switch (static_cast<KeyCode>(ev.key)) {
            case KeyCode::Backspace: controller()->deleteCharBackward(); return KeyEventResult::Handled;
            case KeyCode::Delete:    controller()->deleteCharForward();  return KeyEventResult::Handled;
            case KeyCode::Left:      controller()->moveCursorLeft(shift);  return KeyEventResult::Handled;
            case KeyCode::Right:     controller()->moveCursorRight(shift); return KeyEventResult::Handled;
            case KeyCode::Home:      controller()->moveCursorToStart(shift); return KeyEventResult::Handled;
            case KeyCode::End:       controller()->moveCursorToEnd(shift);   return KeyEventResult::Handled;
            // 上游 `_moveCursor`：单行（`!_isMultiline`）时 Up/Down 直接 return false，
            // 事件沿焦点祖先链继续上溯（滚动视图据此用方向键滚动、ComboBox 据此选项目）。
            case KeyCode::Up:
            case KeyCode::Down:      return KeyEventResult::Ignored;
            case KeyCode::Return: {
                // 上游 `onSubmit`：给了 onSubmitted 才消费回车，否则继续上溯
                // （ComboBox 用回车确认高亮项，页面用回车触发动作）。
                auto* w = static_cast<EditableText*>(widget());
                if (w && w->onSubmitted()) {
                    w->onSubmitted()(controller()->text());
                    return KeyEventResult::Handled;
                }
                return KeyEventResult::Ignored;
            }
            default: break;
        }

        // Ctrl/⌘ + 字母：选区编辑。
        if ((ctrl || meta) && !ev.text.empty()) {
            char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ev.text[0])));
            if (ch == 'a') { controller()->selectAll(); return KeyEventResult::Handled; }
            if (ch == 'c') {
                const TextSelection& s = controller()->selection();
                if (!s.isCollapsed())
                    detail::clipboardBuf() = controller()->text().substr(s.start(), s.end() - s.start());
                return KeyEventResult::Handled;
            }
            if (ch == 'x') {
                const TextSelection& s = controller()->selection();
                if (!s.isCollapsed()) {
                    detail::clipboardBuf() = controller()->text().substr(s.start(), s.end() - s.start());
                    controller()->replaceSelection("");
                }
                return KeyEventResult::Handled;
            }
            if (ch == 'v') {
                if (!detail::clipboardBuf().empty())
                    controller()->insert(detail::clipboardBuf());
                return KeyEventResult::Handled;
            }
        }
        return KeyEventResult::Ignored;
    }

    SharedPtr<FocusNode> focusNode_;
    int listenerId_ = 0;
    std::string lastText_;
};

std::unique_ptr<State> EditableText::createState() {
    return std::make_unique<EditableTextState>();
}

// ── TextField ───────────────────────────────────────────────────────────────
SharedPtr<Widget> TextField::build(BuildContext&) {
    auto ed = cui::make_shared<EditableText>(controller_, style_, textAlign_,
                                             decoration_.hintText, obscureText_, autofocus_,
                                             nullptr, focusNode_, onChanged_, onSubmitted_);

    BoxDecoration dec;
    dec.borderColor = decoration_.borderColor;
    dec.borderWidth = decoration_.borderWidth;
    dec.borderRadius = decoration_.borderRadius;
    dec.color = decoration_.filled ? decoration_.fillColor : Color::fromRGBA(0, 0, 0, 0);

    auto box = cui::make_shared<Container>(ed);
    box->padding(EdgeInsets::all(8.0));
    box->decoration(dec);

    std::vector<SharedPtr<Widget>> children;
    if (!decoration_.labelText.empty())
        children.push_back(cui::make_shared<Text>(decoration_.labelText, style_));
    children.push_back(box);

    auto col = cui::make_shared<Column>(children);
    col->setMainAxisSize(MainAxisSize::Min);
    // 标签贴行首：父级若给紧宽度，Column 默认 Center 会把标签甩到中间。
    col->setCrossAxisAlignment(CrossAxisAlignment::Start);
    return col;
}

}  // namespace cui_core
