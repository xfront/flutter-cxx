#pragma once
/// @file framework/element.h
/// @brief 上游的 Element 体系移植。
///
/// 对应 上游源码：
///   上游 widgets/framework.dart（Element 段落）
///
/// Element 层级（严格对齐上游）：
///   Element
///     ComponentElement            — 持有单个子 Element（build 出来的子树）
///       StatelessElement         — 无状态：每次重建调用 build()
///       StatefulElement          — 有状态：持有 State，驱动生命周期
///     ProxyElement                — 代理节点：只把 Widget 配置往下传
///       InheritedElement          — 依赖分发中心
///       ParentDataElement         — 往子 RenderObject 写 parentData
///     RenderObjectElement         — 持有 RenderObject
///       LeafRenderObjectElement
///       SingleChildRenderObjectElement
///       MultiChildRenderObjectElement — 多子节点 reconcile
///
/// 生命周期（与 上游 一致）：
///   mount → (首次) build → updateChild
///   → markNeedsBuild → rebuild
///   → deactivate → unmount
///
/// 关键语义（全部以 上游官方源码为准）：
///   - `Element.update` 只替换 `_widget`，不重建（framework.dart:4395）。
///   - `rebuild(force)` 在 `!_dirty && !force` 时直接返回（framework.dart:5523）。
///     `_dirty` 初值为 true（framework.dart:5343），首次 build 因此必然发生。
///   - `updateChild` 三分支（framework.dart:4002-4095）：
///       newWidget == null            → 卸载旧 child
///       child.widget == newWidget    → 直接复用，**不 update、不重建**
///       Widget.canUpdate(...)        → child.update(newWidget)
///       否则                         → 卸载旧 child + 挂载新 Element
///   - `updateChildren` 六步算法（framework.dart:4145-4328）：
///       顶部同步 → 底部扫描 → 中部旧节点建 Key 表 → 中部新节点按 Key 匹配
///       → 尾部同步 → 清理残余 Key 节点。
///   - `Element.didChangeDependencies` 内部即 `markNeedsBuild()`
///     （framework.dart:5210-5214）。
///   - `StatefulElement.didChangeDependencies` 只置标志位，
///     真正的 `state.didChangeDependencies()` 在下次 build 前触发
///     （framework.dart:6136-6140 / 5997-6003）。
///
/// Element ↔ RenderObject 的连接方式（上游的 attachRenderObject）：
///   RenderObjectElement 挂载时把自己的 RenderObject 插入到
///   「最近的祖先 RenderObjectElement」的 render object 中；卸载时移除。
///   非渲染 Element（StatelessElement / ParentDataElement 等）会被跳过，
///   它们的 RenderObject 后代直接挂到更外层的渲染容器上。
///
/// ParentData 的应用方式（上游的 _findAncestorParentDataElements）：
///   不是「ParentDataElement 向下推」，而是「RenderObject 挂载时向上找
///   祖先链上的 ParentDataElement」，由它们对本 RenderObject 调用
///   applyParentData。这样 Expanded 的 flex 值才能落到真正被插入
///   Flex 的那个 RenderObject 上。

#include <functional>
#include <memory>
#include <typeindex>
#include <vector>

#include "cui/foundation/shared_ptr.h"
#include "framework.h"

namespace cui {

class BuildOwner;
class InheritedElement;
class ParentDataElement;
class RenderObjectElement;

/// Element 基类：Widget 的挂载实例。
class Element : public BuildContext {
public:
    explicit Element(SharedPtr<Widget> widget);
    ~Element() override;

    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;

    /// 当前挂载的 Widget 配置。
    Widget* widget() const { return widget_.get(); }

    /// 父 Element。
    Element* parent() const { return parent_; }

    /// 所属的 BuildOwner。
    BuildOwner* owner() const { return owner_; }

    /// 挂载到树上。上游的 `mount()`。
    virtual void mount(Element* parent, BuildOwner* owner);

    /// 用新的 Widget 配置更新本 Element。上游的 `Element.update()`。
    ///
    /// 官方语义（framework.dart:4395）：只替换 `_widget`，**不**触发重建；
    /// 由各子类在 `update` 末尾主动 `rebuild(force: true)`。
    virtual void update(SharedPtr<Widget> newWidget);

    /// 重建子树。上游的 `rebuild({bool force = false})`。
    ///
    /// 官方语义（framework.dart:5523）：未标脏且未 force 时**直接返回**。
    void rebuild(bool force = false);

    /// 标记需要重建。上游的 `markNeedsBuild()`。
    ///
    /// 重建被合并到当前帧的 build scope，避免同一帧内重复构建。
    void markNeedsBuild();

    /// 是否已被标记为待重建。
    bool dirty() const { return dirty_; }

    /// 是否已挂载。
    bool mounted() const { return mounted_; }

    /// 从树上卸载。上游的 `unmount()`。
    virtual void unmount();

    /// 停用（暂时从树上摘下但可能重新挂载）。上游的 `deactivate()`。
    virtual void deactivate();

    /// 重新激活。上游的 `activate()`。
    virtual void activate();

    /// 访问子 Element。
    virtual void visitChildren(const std::function<void(Element*)>& visitor) = 0;

    /// 冒泡路径上的钩子。上游的 `NotifiableElementMixin.onNotification`：
    /// 返回 true 表示已处理、停止冒泡。默认不处理（`NotificationListenerElement`
    /// 与视口 Element 覆写它）。
    virtual bool onNotification(Notification& notification) {
        (void)notification;
        return false;
    }

    /// 依赖的 InheritedElement 变化时回调。上游的 `didChangeDependencies()`。
    ///
    /// 官方语义（framework.dart:5210-5214）：本方法内部即调用
    /// `markNeedsBuild()`——依赖变化意味着本 Element 需要重建。
    virtual void didChangeDependencies() { markNeedsBuild(); }

    /// 依赖变化后的具体处理。上游的 `handleDependencyChanged`。
    ///
    /// 上游的 `InheritedElement.notifyClients` 直接调用依赖者的
    /// `didChangeDependencies()`，因此这里只做转发。
    virtual void handleDependencyChanged() { didChangeDependencies(); }

    /// 最近的、持有 RenderObject 的祖先 Element。上游的
    /// `_findAncestorRenderObjectElement`。
    RenderObjectElement* ancestorRenderObjectElement();

    /// 本 Element 子树中的第一个 RenderObject。上游的
    /// `Element.renderObject`（沿 renderObjectAttachingChild 链向下）。
    RenderObject* findRenderObject();

    /// 会把 RenderObject 插入「本 Element 的祖先」的那个子 Element。
    /// 上游的 `Element.renderObjectAttachingChild`。
    virtual Element* renderObjectAttachingChild() { return nullptr; }

    Element* element() const override { return const_cast<Element*>(this); }

    /// 供 BuildContext 模板方法使用的类型擦除实现。
    void* dependOnInheritedWidgetOfExactTypeRaw(const std::type_index& type) override;
    void* getInheritedWidgetOfExactTypeRaw(const std::type_index& type) override;
    void* findAncestorWidgetOfExactTypeRaw(const std::type_index& type) override;
    void* findAncestorStateOfTypeRaw(const std::type_index& type) override;

protected:
    /// 重建的具体实现。上游的 `performRebuild()`。
    ///
    /// 官方语义（framework.dart:5568-5570）：基类实现只清除 `_dirty` 标志；
    /// 子类在完成自己的构建后调用 `Element::performRebuild()`。
    virtual void performRebuild();

    /// 挂载后的初始化钩子。
    virtual void didMount() {}

    /// 卸载后的清理钩子。
    virtual void didUnmount() {}

    /// 上游的 `updateChild`（framework.dart:4002-4095）。
    ///
    /// 接管 `child` 的所有权，返回新的 child：
    ///   - newWidget == null         → 卸载旧 child，返回 nullptr
    ///   - child.widget == newWidget → 直接复用，不 update、不重建
    ///   - canUpdate(old, new)       → child.update(newWidget)
    ///   - 否则                      → 卸载旧 child + 挂载新 Element
    std::unique_ptr<Element> updateChild(std::unique_ptr<Element> child,
                                         const SharedPtr<Widget>& newWidget);

    /// reconcile 一个子槽位。等价于 `slot = updateChild(std::move(slot), w)`。
    void reconcileChild(std::unique_ptr<Element>& slot, const SharedPtr<Widget>& newWidget);

    /// 创建一个新 Element（默认按新 Widget 创建）。
    virtual std::unique_ptr<Element> createChildFor(const SharedPtr<Widget>& widget);

    /// 停用并卸载一个子 Element（上游的 `deactivateChild`）。
    void disposeChild(std::unique_ptr<Element>& child);

    SharedPtr<Widget> widget_;
    Element* parent_ = nullptr;
    BuildOwner* owner_ = nullptr;
    bool mounted_ = false;
    /// 上游的 `_dirty`：初值为 true（framework.dart:5343）。
    bool dirty_ = true;

    /// 本 Element 依赖到的 InheritedElement（上游的 `_dependencies`）。
    std::vector<InheritedElement*> dependencies_;

    friend class BuildOwner;
    friend class InheritedElement;
    friend class ParentDataElement;
};

// ── ComponentElement ────────────────────────────────────────────────────────

/// 拥有单个子 Element 的组合节点。
///
/// 上游的 `ComponentElement`：持有 `_child`，通过 `updateChild`
/// 完成 reconcile。
class ComponentElement : public Element {
public:
    using Element::Element;

    Element* child() const { return child_.get(); }

    void visitChildren(const std::function<void(Element*)>& visitor) override;

    void unmount() override;
    void deactivate() override;
    void activate() override;

    /// 上游的 `renderObjectAttachingChild` → `_child`。
    Element* renderObjectAttachingChild() override { return child_.get(); }

protected:
    /// 创建一个新 Element（未挂载）。
    std::unique_ptr<Element> createChildFor(const SharedPtr<Widget>& widget) override;

    std::unique_ptr<Element> child_;
};

// ── StatelessElement ────────────────────────────────────────────────────────

/// 无状态 Widget 的 Element：直接调用 widget->build(context)。
class StatelessElement : public ComponentElement {
public:
    explicit StatelessElement(SharedPtr<Widget> widget)
        : ComponentElement(std::move(widget)) {}

    void update(SharedPtr<Widget> newWidget) override;

protected:
    void performRebuild() override;
};

// ── StatefulElement ─────────────────────────────────────────────────────────

/// 有状态 Widget 的 Element：持有 State 并驱动其生命周期。
class StatefulElement : public ComponentElement {
public:
    explicit StatefulElement(SharedPtr<Widget> widget);
    ~StatefulElement() override;

    /// 当前 State。
    State* state() const { return state_.get(); }

    void update(SharedPtr<Widget> newWidget) override;

protected:
    void performRebuild() override;
    void didMount() override;
    void didUnmount() override;

    /// 依赖变化时只置标志位（上游的 `_didChangeDependencies`，
    /// framework.dart:6136-6140）。真正的 `state.didChangeDependencies()`
    /// 在下次 build 之前由 `performRebuild` 触发（framework.dart:5997-6003）。
    void didChangeDependencies() override;

    std::unique_ptr<State> state_;

    /// 上游的 `_didChangeDependencies`。
    bool didChangeDependenciesPending_ = false;
};

// ── ProxyElement ────────────────────────────────────────────────────────────

/// 代理节点：自身不产生内容，只把更新传给子树。
///
/// 上游的 `ProxyElement`（framework.dart:6155）：
///   build() => (widget as ProxyWidget).child;
///   update(newWidget) → updated(oldWidget) → notifyClients(oldWidget) → rebuild
class ProxyElement : public ComponentElement {
public:
    using ComponentElement::ComponentElement;

    void update(SharedPtr<Widget> newWidget) override;

protected:
    void performRebuild() override;

    /// Widget 配置变化时的回调。上游的 `ProxyElement.updated()`。
    virtual void updated(const Widget& oldWidget) { notifyClients(oldWidget); }

    /// 通知依赖者。上游的 `notifyClients()`。
    virtual void notifyClients(const Widget& oldWidget) { (void)oldWidget; }
};

// ── InheritedElement ────────────────────────────────────────────────────────

/// 依赖分发中心。
///
/// 记录所有依赖本节点的 Element；当数据变化时，只重建这些依赖者。
class InheritedElement : public ProxyElement {
public:
    explicit InheritedElement(SharedPtr<Widget> widget)
        : ProxyElement(std::move(widget)) {}

    /// 登记依赖者。
    void addDependent(Element* dependent);
    /// 移除依赖者。
    void removeDependent(Element* dependent);

    /// 依赖者数量（测试用）。
    size_t dependentCount() const { return dependents_.size(); }

protected:
    /// 只在数据确实变化时才通知依赖者。上游的 `InheritedElement.updated()`
    /// （framework.dart:6421-6426）：`(widget as InheritedWidget)
    /// .updateShouldNotify(oldWidget)` 为真才调用 `super.updated`。
    void updated(const Widget& oldWidget) override;

    /// 通知所有依赖者。上游的 `notifyClients()`。
    void notifyClients(const Widget& oldWidget) override;

private:
    std::vector<Element*> dependents_;
};

// ── ParentDataElement ───────────────────────────────────────────────────────

/// 往子 RenderObject 写 parentData 的 Element。上游的
/// `ParentDataElement<T>`（framework.dart:6192）。
///
/// 它本身不产生 RenderObject：真正的子 RenderObject 会越过本 Element，
/// 直接插入到「最近的祖先 RenderObjectElement」（即 Flex / Stack）。
///
/// 写入时机：
///   1. 子 RenderObject 挂载时，由 RenderObjectElement 向上查找到本
///      Element 并调用 applyParentData（上游的
///      _findAncestorParentDataElements）。
///   2. 本 Element 自身的 Widget 配置变化时（notifyClients）重新写入。
class ParentDataElement : public ProxyElement {
public:
    explicit ParentDataElement(SharedPtr<Widget> widget)
        : ProxyElement(std::move(widget)) {}

    /// 把 parentData 写到目标 RenderObject 上。上游的 `_applyParentData`。
    void applyParentDataToChild();

protected:
    void notifyClients(const Widget& oldWidget) override;
};

// ── RenderObjectElement ─────────────────────────────────────────────────────

/// 持有 RenderObject 的 Element。上游的 `RenderObjectElement`。
///
/// 关键机制（上游的 attachRenderObject / detachRenderObject）：
///   - 挂载时：找到最近的祖先 RenderObjectElement，把自己（的 RenderObject）
///     插入祖先的 render object；随后向上查找祖先链上的
///     ParentDataElement，让它们写入 parentData。
///   - 卸载时：从祖先的 render object 中移除。
///
/// 官方语义（framework.dart:6832-6838）：`update` 只做两件事——
/// 替换 `_widget` 与同步 RenderObject 配置（`updateRenderObject`），
/// **不**重建子树。
class RenderObjectElement : public Element {
public:
    explicit RenderObjectElement(SharedPtr<Widget> widget);
    ~RenderObjectElement() override;

    /// 当前 RenderObject。
    RenderObject* renderObject() const { return renderObject_.get(); }

    void visitChildren(const std::function<void(Element*)>& visitor) override;

    void unmount() override;

    void update(SharedPtr<Widget> newWidget) override;

protected:
    void didMount() override;
    void didUnmount() override;
    void performRebuild() override;

    /// 创建一个新 RenderObject（由 widget->createRenderObject() 提供）。
    virtual std::unique_ptr<RenderObject> createRenderObject();

    /// 把子 RenderObject 插入本 Element 的 RenderObject。上游的
    /// `insertRenderObjectChild`。
    ///
    /// `owner` 是正在挂载的那个 Element（可能比本节点的直接子节点更深）。
    /// 上游靠 slot（新列表里的前一个兄弟）定位；本仓无 slot，多子容器据此
    /// 反查插入位置（见 `MultiChildRenderObjectElement`）。
    virtual void insertRenderObjectChild(RenderObject* child, Element* owner = nullptr) {
        (void)child;
        (void)owner;
    }
    /// 从本 Element 的 RenderObject 移除。上游的 `removeRenderObjectChild`。
    virtual void removeRenderObjectChild(RenderObject* child) { (void)child; }
    /// 移动子 RenderObject。上游的 `moveRenderObjectChild`。
    virtual void moveRenderObjectChild(RenderObject* child, RenderObject* after) {
        (void)child;
        (void)after;
    }

    /// 向上查找祖先链上的 ParentDataElement 并让它们写入 parentData。
    /// 上游的 `_findAncestorParentDataElements` + `_updateParentData`。
    void applyAncestorParentData();

    std::unique_ptr<RenderObject> renderObject_;
};

// ── LeafRenderObjectElement ─────────────────────────────────────────────────

/// 叶子渲染对象 Element。上游的 `LeafRenderObjectElement`。
class LeafRenderObjectElement : public RenderObjectElement {
public:
    using RenderObjectElement::RenderObjectElement;
};

// ── SingleChildRenderObjectElement ──────────────────────────────────────────

/// 单子节点渲染对象 Element。上游的 `SingleChildRenderObjectElement`。
///
/// 子 Widget 挂载在本 Element 之下；其 RenderObject 会被插入本 Element
/// 的 RenderObject（因为本 Element 就是它最近的渲染祖先）。
///
/// 官方语义：
///   - mount（framework.dart:7139-7142）：`_child = updateChild(_child,
///     (widget as SingleChildRenderObjectWidget).child, null)`；
///   - update（framework.dart:7144-7148）：`super.update(newWidget)` 后
///     `_child = updateChild(_child, newWidget.child, null)`。
class SingleChildRenderObjectElement : public RenderObjectElement {
public:
    using RenderObjectElement::RenderObjectElement;

    /// 子 Element。
    Element* child() const { return child_.get(); }

    void visitChildren(const std::function<void(Element*)>& visitor) override;

    void unmount() override;
    void deactivate() override;
    void activate() override;

    void update(SharedPtr<Widget> newWidget) override;

protected:
    void didMount() override;
    void performRebuild() override;
    void insertRenderObjectChild(RenderObject* child, Element* owner) override;
    void removeRenderObjectChild(RenderObject* child) override;

    std::unique_ptr<Element> child_;
};

// ── MultiChildRenderObjectElement ───────────────────────────────────────────

/// 拥有多个子 Element 的渲染节点。上游的 `MultiChildRenderObjectElement`。
///
/// reconcile 规则与 上游 一致（framework.dart:4145-4328）：
///   - 顶部 / 尾部：按位置顺序同步
///   - 中部：按 Key 匹配（可跨位置重排，保持 State），无 Key 的旧节点被卸载
class MultiChildRenderObjectElement : public RenderObjectElement {
public:
    using RenderObjectElement::RenderObjectElement;

    /// 子 Element 列表。
    const std::vector<std::unique_ptr<Element>>& children() const { return children_; }

    void visitChildren(const std::function<void(Element*)>& visitor) override;

    void unmount() override;
    void deactivate() override;
    void activate() override;

    void update(SharedPtr<Widget> newWidget) override;

protected:
    void didMount() override;
    void performRebuild() override;

    void insertRenderObjectChild(RenderObject* child, Element* owner) override;
    void removeRenderObjectChild(RenderObject* child) override;
    void moveRenderObjectChild(RenderObject* child, RenderObject* after) override;

    /// 上游的 `updateChildren`（framework.dart:4145-4328）。
    void updateChildren(const std::vector<SharedPtr<Widget>>& newWidgets);

    /// reconcile 结束后按 children_ 顺序重排 RenderObject 子节点。
    void syncRenderObjectOrder();

    bool updating_ = false;
    std::vector<std::unique_ptr<Element>> children_;
};

// ── LayoutBuilderElement ─────────────────────────────────────────────────

/// LayoutBuilder 的 Element。上游的 `_LayoutBuilderElement`。
///
/// 在 RenderObject 的布局阶段拿到当前约束后，于约束**变化**时才同步重建
/// 子节点 —— 避免 performLayout 自调用导致的死循环。约束回调由
/// `RenderLayoutBuilder.layoutCallback` 触发。
class LayoutBuilderElement : public SingleChildRenderObjectElement {
public:
    using SingleChildRenderObjectElement::SingleChildRenderObjectElement;

    void mount(Element* parent, BuildOwner* owner) override;

    /// 跳过 SingleChildRenderObjectElement::update 的 child reconcile：
    /// LayoutBuilder 的子节点由 layoutCallback（约束驱动）全权管理，
    /// 基类 update 会用 widget->child()（恒为 null）把动态子树清掉。
    void update(SharedPtr<Widget> newWidget) override;

    /// 由 RenderLayoutBuilder 的 layoutCallback 调用：约束变化时同步重建。
    void rebuild(const BoxConstraints& constraints);

protected:
    void performRebuild() override;

private:
    BoxConstraints constraints_;
};

}
