#pragma once
/// @file framework/framework.h
/// @brief 上游的 Widget 体系移植（framework.dart 核心部分）。
///
/// 对应 上游源码：
///   上游 widgets/framework.dart
///
/// 与既有 cui_core::Widget 的根本差别：
///   - cui_core::Widget  = 「配置 + 布局 + 绘制 + 事件」四合一（ 式）。
///   - cui_core::Widget = 纯不可变配置（blueprint），不含任何行为。
///   - cui_core::Element = Widget 的挂载实例，负责生命周期与重建。
///   - cui_core::State   = StatefulWidget 的可变状态，生命周期钩子完备。
///
/// 这条「配置 / 挂载 / 状态」三分正是 上游 架构的核心。
///
/// 层级（对齐上游）：
///   Widget
///     StatelessWidget        — build(context) 返回子树
///     StatefulWidget         — createState() 返回 State
///     ProxyWidget            — 只向下传递配置，不渲染自身
///       InheritedWidget<T>    — 提供可被依赖的数据
///     ParentDataWidget       — 往子节点的 RenderObject.parentData 写数据
///                               （上游的 Expanded / Flexible 即是此类）

#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <vector>

#include "../utils/shared_ptr.h"
#include "cui/foundation/key.h"
#include "cui/rendering/render_object.h"

namespace cui {

class Element;
class BuildContext;
class Notification;  // widgets/notification.h
class BuildOwner;
class Widget;

/// 上游的 `ElementVisitor`（framework.dart:2192）：`visitChildElements` 的回调。
/// 在回调里再次 `visitChildElements` 是安全的。
using ElementVisitor = std::function<void(Element*)>;

/// 上游的 `ConditionalElementVisitor`（framework.dart:2199）：
/// `visitAncestorElements` 的回调，返回 false 停止向上走。
using ConditionalElementVisitor = std::function<bool(Element*)>;

/// 创建 InheritedElement。由 element.cpp 实现，避免头文件循环依赖。
std::unique_ptr<Element> createInheritedElement(SharedPtr<Widget> self);
/// 创建 ParentDataElement。由 element.cpp 实现。
std::unique_ptr<Element> createParentDataElement(SharedPtr<Widget> self);

// ── Widget ──────────────────────────────────────────────────────────────────

/// Widget 基类。不可变配置。
///
/// 上游的 `Widget` 有一个重要约定：`canUpdate` 用于判断两个 Widget
/// 是否可以复用同一个 Element（`runtimeType` 相同 && `key` 相同）。
class Widget {
public:
    explicit Widget(KeyPtr key = nullptr) : key_(std::move(key)) {}
    virtual ~Widget() = default;

    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    /// 该 Widget 的 Key（可为 null）。
    const KeyPtr& key() const { return key_; }

    /// 创建对应的 Element。上游的 `Widget.createElement()`。
    ///
    /// `self` 是指向本 Widget 的共享所有权（Widget 配置由父节点持有），
    /// Element 通过它读取配置。
    virtual std::unique_ptr<Element> createElement(SharedPtr<Widget> self) = 0;

    /// 是否可以复用旧 Element：同类型 + 同 Key。上游的 `canUpdate`。
    bool canUpdate(const Widget& oldWidget) const;

    /// 诊断名。上游的 `toStringShort()`。
    virtual std::string toStringShort() const { return "Widget"; }

protected:
    KeyPtr key_;
};

// ── StatelessWidget ─────────────────────────────────────────────────────────

/// 无状态 Widget：每次重建直接调用 build()。
class StatelessWidget : public Widget {
public:
    using Widget::Widget;

    /// 构建子树。这是 StatelessWidget 唯一需要实现的方法。
    virtual SharedPtr<Widget> build(BuildContext& context) = 0;

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override;
};

// ── StatefulWidget ─────────────────────────────────────────────────────────

/// 有状态 Widget：状态保存在独立的 State 对象里。
class StatefulWidget : public Widget {
public:
    using Widget::Widget;

    /// 创建 State 实例。上游的 `createState()`。
    virtual std::unique_ptr<class State> createState() = 0;

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override;
};

// ── State ───────────────────────────────────────────────────────────────────

/// StatefulWidget 的可变状态。
///
/// 生命周期（与 上游 完全一致的顺序）：
///   initState() → didChangeDependencies() → build()
///   → (父重建时) didUpdateWidget(oldWidget) → build()
///   → deactivate() → dispose()
class State {
public:
    virtual ~State() = default;

    /// 当前挂载该 State 的 Widget。
    Widget* widget() const { return widget_; }

    /// 当前 State 的 BuildContext。
    BuildContext* context() const { return context_; }

    /// 承载本 State 的 Element。上游的 `State.element`。
    ///
    /// 上游 中 `State.element` 是 protected 的（仅 State 子类可访问），
    /// 这里放宽为 public 以便宿主 Widget（如 OverlayEntry 的宿主 State）
    /// 定位自身 Element。
    Element* element() const { return element_; }

    /// State 首次挂载后调用一次。
    virtual void initState() {}

    /// 依赖的 InheritedWidget 变化时调用。
    virtual void didChangeDependencies() {}

    /// 父 Widget 重建导致本 Widget 配置变化时调用。
    virtual void didUpdateWidget(cui::Widget* oldWidget) {}

    /// 构建子树。上游的 `State.build(BuildContext)`。
    virtual SharedPtr<Widget> build(BuildContext& context) = 0;

    /// 从树上卸载时调用。
    virtual void dispose() {}

    /// State 是否仍挂在树上。
    bool mounted() const { return mounted_; }

    /// 标记需要重建。上游的 `setState`。
    ///
    /// 回调内修改状态，随后安排一次重建。重建被合并到当前帧的
    /// build scope，语义与 上游 一致。
    void setState(std::function<void()> fn);

    /// 直接请求重建（不修改状态）。上游的 `setState(() {})`。
    void setState();

private:
    friend class StatefulElement;

    Widget* widget_ = nullptr;
    BuildContext* context_ = nullptr;
    Element* element_ = nullptr;
    bool mounted_ = false;
};

// ── ProxyWidget / InheritedWidget ───────────────────────────────────────────

/// 代理 Widget：不渲染自身，只把配置传给子树。
///
/// 上游的 ProxyWidget 持有一个 child。这里允许 child 为空，
/// 以便 InheritedWidget 这类「纯数据提供者」无需携带子树。
class ProxyWidget : public Widget {
public:
    explicit ProxyWidget(KeyPtr key = nullptr, SharedPtr<Widget> child = nullptr)
        : Widget(std::move(key)), child_(std::move(child)) {}

    /// 下方的子树。
    const SharedPtr<Widget>& child() const { return child_; }
    void setChild(SharedPtr<Widget> child) { child_ = std::move(child); }

    /// 配置变化时是否需要通知依赖者。上游的 `updateShouldNotify`。
    virtual bool updateShouldNotify(const ProxyWidget& oldWidget) const { return false; }
    /// 是否需要通知依赖者（通用形式）。
    bool shouldNotify(const Widget& oldWidget) const {
        auto* oldProxy = dynamic_cast<const ProxyWidget*>(&oldWidget);
        if (!oldProxy)
            return true;
        return updateShouldNotify(*oldProxy);
    }

private:
    SharedPtr<Widget> child_;
};

/// 把数据沿树向下传递的 Widget。
///
/// 子树通过 `BuildContext::dependOnInheritedWidgetOfExactType<T>()` 订阅；
/// 数据变化时只有真正依赖它的 Element 会被重建。
///
/// **查找键是 Widget 自身的类型**（上游的 `widget.runtimeType`），
/// 返回值也是 **InheritedWidget 本身**（上游 返回 `T?` 类型的
/// InheritedWidget，调用方再取 `data`），而不是裸数据指针。
template <typename T>
class InheritedWidget : public ProxyWidget {
public:
    using ProxyWidget::ProxyWidget;

    /// 传递给子树的数据。
    virtual SharedPtr<const T> data() const = 0;

    /// 上游的 `InheritedWidget.updateShouldNotify`。
    ///
    /// 默认比较 data 值；子类可覆写以自定义「数据是否变化」的判定。
    bool updateShouldNotify(const ProxyWidget& oldWidget) const override {
        auto* oldTyped = dynamic_cast<const InheritedWidget<T>*>(&oldWidget);
        if (!oldTyped)
            return true;
        auto a = data();
        auto b = oldTyped->data();
        if (!a || !b)
            return a != b;
        return *a != *b;
    }

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override {
        return createInheritedElement(std::move(self));
    }
};

// ── Builder ─────────────────────────────────────────────────────────────────

/// 把「构建子树」的动作推迟到自己的 Element 中执行。
///
/// 上游的 `Builder`（framework.dart:1546-1560）。
///
/// 它的唯一用途是**改变 build 的执行位置**：`Builder` 是一个独立的
/// StatelessElement，因此它 `build` 回调里拿到的 `BuildContext`
/// 位于 `Builder` 自身所在的位置——也就是它的父节点之下。
///
/// 上游的 `Router`（router.dart:840-856）正是靠它让
/// `routerDelegate.build(context)` 拿到的 context 能够
/// `dependOnInheritedWidgetOfExactType<_RouterScope>()`。
class Builder : public StatelessWidget {
public:
    using BuilderCallback = std::function<SharedPtr<Widget>(BuildContext&)>;

    explicit Builder(BuilderCallback builder, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), builder_(std::move(builder)) {}

    SharedPtr<Widget> build(BuildContext& context) override {
        return builder_ ? builder_(context) : nullptr;
    }

private:
    BuilderCallback builder_;
};

// ── KeyedSubtree ────────────────────────────────────────────────────────────

/// 只给子树补一个 key 的透传 Widget。上游的 `KeyedSubtree`（framework.dart）。
///
/// 用途：包装函数（如 `AnimatedSwitcher.transitionBuilder`）自己不带 key，
/// 但调用方需要让 Element 在 children 列表位移/增删时保持身份，就在外面补一层。
class KeyedSubtree : public StatelessWidget {
public:
    explicit KeyedSubtree(KeyPtr key, SharedPtr<Widget> child = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)) {}

    const SharedPtr<Widget>& child() const { return child_; }

    SharedPtr<Widget> build(BuildContext&) override { return child_; }

private:
    SharedPtr<Widget> child_;
};

// ── ParentDataWidget ────────────────────────────────────────────────────────

/// 往子节点 RenderObject 的 parentData 槽写入数据的 Widget。
///
/// 上游的 `ParentDataWidget<T extends ParentData>`（framework.dart:1596）。
/// 官方定义：`abstract class ParentDataWidget<T extends ParentData> extends
/// ProxyWidget`——即它本身也是一个 ProxyWidget，持有一个 child。
///
/// 典型使用者是 `Expanded` / `Flexible`（写入 `FlexParentData.flex`）与
/// `Positioned`（写入 `StackParentData.positioned`）。
///
/// 应用时机（上游的 `RenderObjectElement.attachRenderObject`）：
/// 子 RenderObject 挂载时向上查找**祖先链上的 ParentDataElement**，
/// 由它们依次对本 RenderObject 调用 `applyParentData`。因此
/// ParentDataWidget 自身不产生 RenderObject。
class ParentDataWidget : public ProxyWidget {
public:
    explicit ParentDataWidget(KeyPtr key = nullptr, SharedPtr<Widget> child = nullptr)
        : ProxyWidget(std::move(key), std::move(child)) {}

    /// 把数据写进 renderObject 的 parentData。上游的同名方法。
    virtual void applyParentData(RenderObject* renderObject) = 0;

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override {
        return createParentDataElement(std::move(self));
    }
};

// ── BuildContext ────────────────────────────────────────────────────────────

/// 子树访问上下文的入口。上游的 `BuildContext`。
class BuildContext {
public:
    virtual ~BuildContext() = default;

    /// 向上查找最近的 T 类型 InheritedWidget，并登记依赖。
    ///
    /// 返回 nullptr 表示树中没有该类型的 InheritedWidget。
    template <typename T>
    T* dependOnInheritedWidgetOfExactType() {
        return static_cast<T*>(dependOnInheritedWidgetOfExactTypeRaw(typeid(T)));
    }

    /// 向上查找最近的 T 类型 InheritedWidget，但不登记依赖。
    template <typename T>
    T* getInheritedWidgetOfExactType() {
        return static_cast<T*>(getInheritedWidgetOfExactTypeRaw(typeid(T)));
    }

    /// 向上查找最近的 T 类型 Widget（不限于 InheritedWidget），不登记依赖。
    template <typename T>
    T* findAncestorWidgetOfExactType() {
        return static_cast<T*>(findAncestorWidgetOfExactTypeRaw(typeid(T)));
    }

    /// 向上查找最近的 T 类型 State。上游的 `findAncestorStateOfType`。
    ///
    /// Overlay.of(context) / Navigator.of(context) 都依赖它定位上层的
    /// OverlayState / NavigatorState。
    template <typename T>
    T* findAncestorStateOfType() {
        return static_cast<T*>(findAncestorStateOfTypeRaw(typeid(T)));
    }

    /// 当前 Element。
    virtual Element* element() const = 0;

    /// 访问本节点的**直接子** Element。上游的 `BuildContext.visitChildElements`。
    ///
    /// 对 StatefulWidget / StatelessWidget 是 O(1)（只有一个子），对
    /// RenderObjectWidget 是 O(N)。上游的用法是「不等到下一帧就去改子树」，
    /// 例如 `NavigatorState` 找当前在场的 OverlayEntry、`Focus` 遍历焦点塔。
    ///
    /// 上游断言「build 期间不得调用」（此时 child 列表还在增删，可能拿到旧
    /// child）。本仓用 `BuildOwner::inBuildPhase()` 复刻该检查：命中时
    /// 直接不执行访问（等价于 debug 断言失败，但本仓没有 assert 体系）。
    void visitChildElements(const ElementVisitor& visitor) const;

    /// 向上访问祖先链。上游的 `Element.visitAncestorElements`：
    /// 回调返回 false 即停止。返回「是否走到了根」（`ancestor == null`）。
    bool visitAncestorElements(const ConditionalElementVisitor& visitor) const;

    /// 上游的 `BuildContext.dispatchNotification`：从本节点的**父链**开始向上
    /// 冒泡，第一个返回 true 的 `NotificationListener` 中断冒泡。
    /// 定义见 src/widgets/framework.cpp（要看到 Element::onNotification）。
    bool dispatchNotification(Notification& notification) const;

protected:
    /// 供模板方法调用的类型擦除版本。
    virtual void* dependOnInheritedWidgetOfExactTypeRaw(const std::type_index& type) = 0;
    virtual void* getInheritedWidgetOfExactTypeRaw(const std::type_index& type) = 0;
    virtual void* findAncestorWidgetOfExactTypeRaw(const std::type_index& type) = 0;
    /// 类型擦除版的 `findAncestorStateOfType`。
    virtual void* findAncestorStateOfTypeRaw(const std::type_index& type) = 0;
};

}
