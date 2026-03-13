/// @file framework/focus.cpp
/// @brief 焦点模型实现（FocusNode / FocusManager / Focus）。

#include "cui/framework/focus.h"

#include <optional>

#include "../../include/cui/framework/build_owner.h"
#include "../../include/cui/framework/element.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {

// ── FocusNode ───────────────────────────────────────────────────────────────

FocusNode::~FocusNode() {
    if (manager_ && manager_->primaryFocus_ == this)
        manager_->primaryFocus_ = nullptr;
    // 从遍历注册表注销，避免 Tab 导航聚焦到已销毁的节点。
    if (manager_)
        manager_->unregisterNode(this);
}

void FocusNode::setManager(FocusManager* manager) {
    if (manager_ == manager)
        return;
    if (manager_) {
        // 脱离仲裁者前先摘掉焦点指针：节点可能由 State 用 shared_ptr 持有，
        // 卸载后还会活得比 manager 里那条记录久，留着就是悬挂句柄
        // （~FocusNode 那时 manager_ 已空，再想清也清不掉）。
        // 刻意不发 onFocusChange(false)：这是销毁路径，回调里的 setState 无意义。
        if (manager_->primaryFocus_ == this)
            manager_->primaryFocus_ = nullptr;
        manager_->unregisterNode(this);
    }
    manager_ = manager;
    if (manager_)
        manager_->registerNode(this);
}

void FocusNode::requestFocus() {
    if (manager_)
        manager_->requestFocus(this);
}

void FocusNode::unfocus() {
    if (manager_)
        manager_->unfocus(this);
}

int FocusNode::addKeyHandler(std::function<KeyEventResult(const KeyEvent&)> handler) {
    if (!handler)
        return 0;
    int id = nextKeyHandlerId_++;
    keyHandlers_.push_back(KeySlot{id, std::move(handler)});
    return id;
}

void FocusNode::removeKeyHandler(int handle) {
    if (handle == 0)
        return;
    for (auto it = keyHandlers_.begin(); it != keyHandlers_.end(); ++it) {
        if (it->id == handle) {
            keyHandlers_.erase(it);
            return;
        }
    }
}

// ── FocusManager ─────────────────────────────────────────────────────────────

FocusManager::~FocusManager() {
    // 先摘走整张表再解绑，避免解绑过程中回调改写正在遍历的 nodes_。
    std::vector<FocusNode*> nodes = std::move(nodes_);
    nodes_.clear();
    for (FocusNode* n : nodes) {
        if (n && n->manager_ == this)
            n->manager_ = nullptr;
    }
    primaryFocus_ = nullptr;
}

FocusNode* FocusManager::primaryFocus() const { return primaryFocus_; }

void FocusManager::requestFocus(FocusNode* node) {
    if (primaryFocus_ == node)
        return;
    if (primaryFocus_) {
        primaryFocus_->hasFocus_ = false;
        if (primaryFocus_->onFocusChange)
            primaryFocus_->onFocusChange(false);
    }
    primaryFocus_ = node;
    if (node) {
        node->hasFocus_ = true;
        if (node->onFocusChange)
            node->onFocusChange(true);
    }
}

void FocusManager::unfocus(FocusNode* node) {
    if (primaryFocus_ != node)
        return;
    // 先落状态再通知（与 requestFocus 同一口径）：监听者读 hasFocus() 必须看到
    // 「已经失焦」，否则「失焦 ⇒ 收起浮层」这类派生逻辑会拿到旧值。
    node->hasFocus_ = false;
    primaryFocus_ = nullptr;
    if (node->onFocusChange)
        node->onFocusChange(false);
}

void FocusManager::registerNode(FocusNode* node) {
    if (node)
        nodes_.push_back(node);
}

void FocusManager::unregisterNode(FocusNode* node) {
    if (!node)
        return;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i] == node) {
            nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

namespace {
/// 上游 `combineKeyEventResults` 在「只有一个结果」时的口径：Ignored 之外
/// 都停止上溯，其中只有 Handled 算「事件被消费」。
std::optional<bool> resultOf(KeyEventResult result) {
    if (result == KeyEventResult::Ignored)
        return std::nullopt;
    return result == KeyEventResult::Handled;
}
}  // namespace

bool FocusManager::sendKeyEvent(const KeyEvent& ev) {
    FocusNode* node = primaryFocus_;
    if (!node)
        return false;

    auto tryNode = [&ev](FocusNode* n) -> std::optional<bool> {
        if (!n)
            return std::nullopt;
        if (n->onKeyEvent) {
            if (auto handled = resultOf(n->onKeyEvent(ev)))
                return handled;
        }
        // 复制一份：处理器可能在回调里把自己摘掉（如选项选中后关闭浮层）。
        std::vector<FocusNode::KeySlot> slots = n->keyHandlers_;
        for (auto& slot : slots) {
            if (!slot.handler)
                continue;
            if (auto handled = resultOf(slot.handler(ev)))
                return handled;
        }
        return std::nullopt;
    };

    if (auto handled = tryNode(node))
        return *handled;

    // 沿 Element 祖先链回溯到一个个 `Focus` 控件（见头文件对偏离的说明）。
    for (Element* e = node->context() ? node->context()->parent() : nullptr; e;
         e = e->parent()) {
        auto* focus = dynamic_cast<Focus*>(e->widget());
        if (!focus)
            continue;
        FocusNode* ancestor = focus->focusNode().get();
        if (!ancestor || ancestor == node)
            continue;
        if (auto handled = tryNode(ancestor))
            return *handled;
    }
    return false;
}

bool FocusManager::nextFocus(bool reverse) {
    if (nodes_.empty())
        return false;
    // 只在仍挂载的节点中遍历：切页后被卸载的旧页面节点虽未注销
    // （Widget 仍持有 node），但已不在元素树上，聚焦它们毫无效果。
    std::vector<FocusNode*> live;
    live.reserve(nodes_.size());
    for (FocusNode* n : nodes_)
        if (n && (!n->context_ || n->context_->mounted()))
            live.push_back(n);
    if (live.empty())
        return false;

    int idx = -1;
    if (primaryFocus_) {
        for (size_t i = 0; i < live.size(); ++i) {
            if (live[i] == primaryFocus_) {
                idx = static_cast<int>(i);
                break;
            }
        }
    }
    const int count = static_cast<int>(live.size());
    // 当前焦点不在存活列表（已卸载/无焦点）时，从入口端开始。
    int next = (idx < 0) ? (reverse ? count - 1 : 0)
                         : (idx + (reverse ? -1 : 1) + count) % count;
    requestFocus(live[static_cast<size_t>(next)]);
    return true;
}

// ── Focus ───────────────────────────────────────────────────────────────────

class FocusState : public State {
public:
    void initState() override {
        auto* focus = static_cast<Focus*>(widget());
        auto* node = focus->focusNode().get();
        Element* elem = nullptr;
        BuildOwner* bo = nullptr;
        if (BuildContext* ctx = context())
            if ((elem = ctx->element()) != nullptr)
                bo = elem->owner();
        if (bo) {
            node->setManager(&bo->focusManager());
            node->setContext(elem);
        }
        node->onFocusChange = focus->onFocusChange();
        // 上游 `_FocusState.didChangeDependencies`：widget 的 onKeyEvent 就是
        // 节点的 onKeyEvent（未提供则清空，回到「本节点不接键」）。
        node->onKeyEvent = focus->onKeyEvent();
        if (focus->autofocus())
            node->requestFocus();
    }

    void didUpdateWidget(cui::Widget* oldWidget) override {
        (void)oldWidget;
        auto* focus = static_cast<Focus*>(widget());
        if (!focus || !focus->focusNode())
            return;
        focus->focusNode()->onFocusChange = focus->onFocusChange();
        focus->focusNode()->onKeyEvent = focus->onKeyEvent();
    }

    /// 卸载时解绑：Element 比 FocusNode 先死（Widget 可被外部 shared_ptr 保活），
    /// 残留的 context_ 会让 nextFocus 对已释放的 Element 调 mounted()。
    void dispose() override {
        auto* focus = static_cast<Focus*>(widget());
        if (!focus)
            return;
        if (FocusNode* node = focus->focusNode().get()) {
            node->setContext(nullptr);
            node->setManager(nullptr);
            node->onFocusChange = nullptr;
            node->onKeyEvent = nullptr;
        }
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        auto* focus = static_cast<Focus*>(widget());
        auto node = focus->focusNode();
        // 点击聚焦：onDown 触发 requestFocus。
        return cui::make_shared<Listener>(
                [node](const PointerEvent&) { node->requestFocus(); },
                nullptr, nullptr, nullptr, nullptr,
                focus->child());
    }
};

std::unique_ptr<State> Focus::createState() {
    return std::make_unique<FocusState>();
}

}  // namespace cui_core
