#pragma once
/// @file framework/focus.h
/// @brief 键盘焦点模型（上游的 FocusNode / FocusManager / Focus 移植）。
///
/// 对应 上游源码：
///   上游 widgets/focus_manager.dart
///   上游 widgets/focus.dart
///
/// 分层说明：
///   - FocusManager 由 BuildOwner 持有（一个 owner 一个焦点域），是焦点
///     仲裁者；requestFocus 切换 primaryFocus 并回调 onFocusChange。
///   - FocusNode 是轻量句柄（被 Widget 持有），通过 setManager 接入
///     FocusManager；requestFocus() 经 manager 完成切换。
///   - Focus 是 StatefulWidget：挂载时把自身 node 注册到所在 owner 的
///     FocusManager，并以 Listener(onDown) 触发 requestFocus（点击聚焦）。

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "cui/foundation/enums.h"  // KeyEventResult
#include "cui/foundation/shared_ptr.h"
#include "framework.h"
#include "listener.h"

namespace cui {

class BuildOwner;

/// 键盘事件（控件层）。由 宿主桥 从宿主 onKeyDown 翻译而来。
struct KeyEvent {
    int key = 0;                  ///< KeyCode 值；0 表示可打印字符（经 text）。
    std::string text;             ///< 该键产生的 UTF-8 文本（可打印键）。
    int modifiers = 0;            ///< ModShift/ModCtrl/ModAlt/ModMeta 位掩码。
    bool isPreedit = false;       ///< 保留：组合（preedit）态标记。
};

/// 焦点仲裁者（上游的 FocusManager）。
///
/// 一个 BuildOwner 对应一个。requestFocus 切换 primaryFocus：先通知旧焦点
/// 失焦（onFocusChange(false)），再通知新焦点获焦（onFocusChange(true)）。
class FocusManager {
public:
    FocusManager() = default;
    /// 节点可以比 manager 活得久（Widget 被外部 shared_ptr 保活），因此析构时
    /// 必须逐个把节点的 manager_ 解绑；否则 FocusNode::~FocusNode 会回头去动
    /// 已释放的 nodes_ 存储（ASan: heap-use-after-free in unregisterNode）。
    ~FocusManager();

    /// 当前拥有键盘焦点的节点（可为 null）。
    class FocusNode* primaryFocus() const;

    /// 把焦点切换到 node（node 可为 null，表示清空焦点）。
    void requestFocus(class FocusNode* node);

    /// 清除 node 的焦点（仅当 node 正是当前焦点时生效）。
    void unfocus(class FocusNode* node);

    /// 焦点遍历：把焦点移到下一个（reverse=true 时上一个）可聚焦节点。
    /// 上游的 FocusTraversalGroup 语义；Tab/Shift+Tab 键盘导航用。
    /// 返回是否有节点被聚焦。只在仍挂载的节点中遍历（切页后被卸载的
    /// 旧节点不参与，避免焦点落进看不见的控件）。
    bool nextFocus(bool reverse = false);

    /// 上游 `FocusManager.handleKeyMessage` 中「沿焦点祖先链派发」那一段：
    /// 从 primaryFocus 起沿链向根逐个节点的键处理器**按序**调用 ——
    /// 每个节点先试单槽 onKeyEvent，再按注册先后试 addKeyHandler 挂上的处理器；
    /// Ignored 继续（同节点的下一个处理器、然后上溯），Handled 停止并返回 true，
    /// SkipRemainingHandlers 停止并返回 false（事件被吞掉但没被处理）。
    ///
    /// 与上游的偏离：上游的祖先链是焦点树（`FocusNode.ancestors`，由
    /// `FocusNode.setParent` 在建焦点树时串起来）；本仓还没有独立的焦点树，
    /// 链直接按 Element 祖先回溯到最近的 `Focus` 控件（焦点树本来就是从控件树
    /// 派生的，两种走法在纯 `Focus` 嵌套下等价）。
    bool sendKeyEvent(const KeyEvent& ev);

private:
    class FocusNode* primaryFocus_ = nullptr;
    /// 按 setManager 注册顺序记录的节点（≈ 构建顺序 ≈ Tab 序）。
    std::vector<class FocusNode*> nodes_;

    void registerNode(class FocusNode* node);
    void unregisterNode(class FocusNode* node);

    friend class FocusNode;
};

/// 焦点节点句柄（上游的 FocusNode）。
///
/// 由 Widget（通常是 Focus）持有。通过 setManager 接入 FocusManager 后才能
/// 真正切换焦点；未接入时 requestFocus/unfocus 静默无效。
class FocusNode {
public:
    FocusNode() = default;
    ~FocusNode();

    /// 经 manager 请求焦点。
    void requestFocus();
    /// 经 manager 释放焦点（仅当自身是当前焦点时）。
    void unfocus();

    /// 是否持有焦点。
    bool hasFocus() const { return hasFocus_; }

    /// 是否为可编辑文本输入（EditableText）的焦点节点。滚动容器在请求焦点时
    /// 会用它来避免从正在编辑文本的输入框「抢走」焦点。
    bool isEditable() const { return isEditable_; }
    void setEditable(bool v) { isEditable_ = v; }

    /// 接入/脱离焦点仲裁者。Focus Widget 在 initState 中调用。
    /// 接入时向 manager 注册（供 Tab 遍历枚举），换 manager 时先从旧的注销。
    void setManager(FocusManager* manager);
    FocusManager* manager() const { return manager_; }

    /// 拥有焦点的 Widget 对应的 Element（用于焦点查找 / 遍历）。
    Element* context() const { return context_; }
    void setContext(Element* element) { context_ = element; }

    /// 焦点变化回调：参数为「是否获得焦点」。
    std::function<void(bool focused)> onFocusChange;

    /// 键盘事件处理器（EditableText 等输入控件在获焦时注册）。
    /// 返回 KeyEventResult::Handled 表示已消费该键。
    std::function<KeyEventResult(const KeyEvent&)> onKeyEvent;

    /// IME 提交（组合结束，commit 文本）。TextEditingController 注册。
    std::function<void(const std::string&)> onIMCommit;

    /// IME 组合进行中（preedit 文本，未提交）。
    std::function<void(const std::string&)> onPreedit;

    ///@{ 上游 `_KeyboardHandler`（hardware_keyboard.dart）：同一个节点可以挂
    /// 多个「按 owner 区分」的键处理器。上游的 `Focus` 控件走的是
    /// `registerKeyHandler(node, this)`，所以共享同一节点的「外层控件」
    /// （ComboBox 的选项导航）和「内层 EditableText」都能收到同一个按键，
    /// 而不会像单槽 `onKeyEvent` 那样互相顶掉。
    /// 派发顺序见 `FocusManager::sendKeyEvent`：先单槽，再按注册先后遍历本表。
    /// 返回句柄，用完必须 `removeKeyHandler`（处理器可能比 State 活得久）。
    int addKeyHandler(std::function<KeyEventResult(const KeyEvent&)> handler);
    void removeKeyHandler(int handle);
    size_t keyHandlerCount() const { return keyHandlers_.size(); }
    ///@}

private:
    struct KeySlot {
        int id;
        std::function<KeyEventResult(const KeyEvent&)> handler;
    };

    bool hasFocus_ = false;
    bool isEditable_ = false;
    FocusManager* manager_ = nullptr;
    Element* context_ = nullptr;
    std::vector<KeySlot> keyHandlers_;
    int nextKeyHandlerId_ = 1;

    friend class FocusManager;
};

/// 焦点容器 Widget（上游的 Focus）。
///
/// 自身持有并管理一个 FocusNode：
///   - initState 时把 node 注册到所在 BuildOwner 的 FocusManager；
///   - build 时以 Listener(onDown) 包裹 child，点击即 requestFocus；
///   - 暴露 focusNode() 供父级或测试读取焦点状态。
class Focus : public StatefulWidget {
public:
    explicit Focus(SharedPtr<Widget> child = nullptr,
                   std::function<void(bool)> onFocusChange = {},
                   bool autofocus = false, KeyPtr key = nullptr,
                   std::function<KeyEventResult(const KeyEvent&)> onKeyEvent = {})
        : StatefulWidget(std::move(key)),
          child_(std::move(child)),
          onFocusChange_(std::move(onFocusChange)),
          autofocus_(autofocus),
          onKeyEvent_(std::move(onKeyEvent)) {
        focusNode_ = cui::make_shared<FocusNode>();
    }

    /// 使用外部传入（可跨重建持久化）的焦点节点：适合需要长期持有焦点的场景
    /// （如侧栏选中导航），避免每次 build 重建节点导致焦点丢失。
    explicit Focus(SharedPtr<FocusNode> node, SharedPtr<Widget> child,
                   std::function<void(bool)> onFocusChange = {},
                   bool autofocus = false, KeyPtr key = nullptr,
                   std::function<KeyEventResult(const KeyEvent&)> onKeyEvent = {})
        : StatefulWidget(std::move(key)),
          child_(std::move(child)),
          onFocusChange_(std::move(onFocusChange)),
          autofocus_(autofocus),
          onKeyEvent_(std::move(onKeyEvent)), focusNode_(std::move(node)) {}

    std::unique_ptr<State> createState() override;

    /// 本 Focus 管理的焦点节点。
    SharedPtr<FocusNode> focusNode() const { return focusNode_; }

    /// 子 Widget。
    const SharedPtr<Widget>& child() const { return child_; }

    /// 焦点变化回调（参数为「是否获得焦点」）。
    const std::function<void(bool)>& onFocusChange() const { return onFocusChange_; }

    /// 挂载即请求焦点（仅当当前无其它焦点时仍会抢占）。
    bool autofocus() const { return autofocus_; }

    /// 上游 `Focus.onKeyEvent`：子树（含可编辑文本）返回 Ignored 时，
    /// 事件沿 `FocusManager::sendKeyEvent` 上溯到这里。
    const std::function<KeyEventResult(const KeyEvent&)>& onKeyEvent() const {
        return onKeyEvent_;
    }

private:
    SharedPtr<Widget> child_;
    std::function<void(bool)> onFocusChange_;
    bool autofocus_ = false;
    std::function<KeyEventResult(const KeyEvent&)> onKeyEvent_;
    SharedPtr<FocusNode> focusNode_;
};

}  // namespace cui_core
