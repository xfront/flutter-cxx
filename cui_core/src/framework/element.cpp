/// @file widgets/element.cpp
/// @brief Element 体系实现 —— 上游 reconcile 算法与生命周期。
///
/// 所有语义均以 上游官方源码为准：
///   上游 widgets/framework.dart
///   （行号在注释中标出）

#include "cui/framework/element.h"

#include <algorithm>
#include <cassert>
#include <map>

#include "../../include/cui/utils/shared_ptr.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"

namespace cui {

namespace {

/// 「按 Key 归拢的旧节点」容器。上游的 `Map<Key, Element>`
/// （framework.dart:4261-4280）。
///
/// 上游 用**哈希表**按 Key 的 `==` 语义归拢旧节点。C++ 没有直接可用的
/// 「按 `operator==` 语义」的哈希容器，而 `std::map` 需要严格弱序比较器——
/// 而 Key 的相等性（`operator==`）不满足严格弱序（`a<b` 与 `b<a` 可能
/// 同时成立），用它做比较器会导致 `std::map` 行为未定义、按键查找失效。
///
/// 因此这里用「线性查找 + `operator==` 比较」的等价容器：
///   - 查找：按 Key 相等性找到旧节点；
///   - 归拢：同 Key 的旧节点覆盖前一个（上游的哈希表语义）。
///
/// 子节点数量通常很小，线性查找的代价可忽略。
class KeyedChildMap {
public:
    /// 归拢一个带 Key 的旧节点。
    void put(const KeyPtr& key, std::unique_ptr<Element> element) {
        for (auto& entry : entries_) {
            if (entry.first && key && *entry.first == *key) {
                entry.second = std::move(element);
                return;
            }
        }
        entries_.emplace_back(key, std::move(element));
    }

    /// 按 Key 查找并取走旧节点（找到后从容器中移除）。
    std::unique_ptr<Element> take(const KeyPtr& key) {
        if (!key)
            return nullptr;
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->first && *it->first == *key) {
                std::unique_ptr<Element> result = std::move(it->second);
                entries_.erase(it);
                return result;
            }
        }
        return nullptr;
    }

    /// 容器是否为空。
    bool empty() const { return entries_.empty(); }

    /// 释放所有剩余节点（上游的「清理残余的 Key 节点」步骤）。
    void disposeAll() {
        for (auto& entry : entries_) {
            if (entry.second)
                entry.second->deactivate();
            if (entry.second)
                entry.second->unmount();
        }
        entries_.clear();
    }

private:
    std::vector<std::pair<KeyPtr, std::unique_ptr<Element>>> entries_;
};

}  // namespace

// ── Element ─────────────────────────────────────────────────────────────────

Element::Element(SharedPtr<Widget> widget) : widget_(std::move(widget)) {}

Element::~Element() = default;

void Element::mount(Element* parent, BuildOwner* owner) {
    parent_ = parent;
    owner_ = owner;

    // 挂载 GlobalKey 到注册表（上游的 _registerGlobalKey）。
    if (widget_ && widget_->key()) {
        auto* gk = dynamic_cast<const GlobalKey*>(widget_->key().get());
        if (gk)
            const_cast<GlobalKey*>(gk)->setCurrentElement(this);
    }

    mounted_ = true;
    didMount();
}

void Element::update(SharedPtr<Widget> newWidget) {
    // 上游的 Element.update（framework.dart:4395）：**只替换 _widget**，
    // 不触发重建。重建由各子类的 update 显式调用 rebuild(force: true) 触发。
    if (!newWidget)
        return;
    // GlobalKey 换了 → 旧的注销、新的注册。
    if (widget_ && widget_->key()) {
        auto* oldGk = dynamic_cast<const GlobalKey*>(widget_->key().get());
        if (oldGk && (!newWidget->key() || newWidget->key().get() != oldGk))
            const_cast<GlobalKey*>(oldGk)->setCurrentElement(nullptr);
    }
    if (newWidget->key()) {
        auto* newGk = dynamic_cast<const GlobalKey*>(newWidget->key().get());
        if (newGk)
            const_cast<GlobalKey*>(newGk)->setCurrentElement(this);
    }
    widget_ = std::move(newWidget);
}

void Element::rebuild(bool force) {
    // 上游的 Element.rebuild（framework.dart:5523）：
    //   if (!_dirty && !force) return;
    //   performRebuild();
    if (!mounted_)
        return;
    if (!dirty_ && !force)
        return;
    performRebuild();
}

void Element::performRebuild() {
    // 上游的 Element.performRebuild（framework.dart:5568-5570）：
    // 基类实现只清除 _dirty 标志。
    dirty_ = false;
}

void Element::markNeedsBuild() {
    // 上游的 Element.markNeedsBuild（framework.dart:5386-5417）：
    // 已标脏则直接返回（避免同帧重复构建）。
    if (!mounted_)
        return;
    if (dirty_)
        return;
    dirty_ = true;
    if (owner_)
        owner_->scheduleBuildFor(this);
}

void Element::unmount() {
    // 注意：**这里不再 push 拆除模式**。上游没有「拆除开关」，靠的是
    // 「RenderObject 已 detach ⇒ owner 为空 ⇒ markNeeds* 自动 no-op」。本仓
    // 同样有 attached()/owner() 兜底，而真正危险的整树拆除（BuildOwner 析构、
    // finalizeTree）各自在调用点 push/pop，无需在此叠加。
    // 之前这里无条件罩住拆除，导致「setState 让子树消失」这条路径上
    // RenderObject::dropChild 被抑制 → 父容器既不脏也无脏区 → 整帧被
    // 「no damage」跳过 → 旧像素残影（Tooltip 气泡收起后不消失即此症）。
    // 从所有依赖的 InheritedElement 的依赖列表中摘除。
    for (InheritedElement* dep : dependencies_)
        dep->removeDependent(this);
    dependencies_.clear();

    if (widget_ && widget_->key()) {
        auto* gk = dynamic_cast<const GlobalKey*>(widget_->key().get());
        if (gk)
            const_cast<GlobalKey*>(gk)->setCurrentElement(nullptr);
    }
    didUnmount();
    mounted_ = false;
    // 上游的 Element.unmount：从 BuildOwner 的待重建列表摘除自身，
    // 否则已释放的 Element 会以悬空指针残留在 dirtyElements_，下一帧
    // flushBuildScope 对其调用 mounted()/rebuild()→markNeedsBuild 时 UAF。
    if (owner_)
        owner_->forgetElement(this);
}

void Element::deactivate() {
    // 上游的 Element.deactivate：解除与所有 InheritedElement 的依赖关系，
    // 这样依赖者不再收到 didChangeDependencies 通知。
    for (InheritedElement* dep : dependencies_)
        dep->removeDependent(this);
    dependencies_.clear();
}

void Element::activate() {
    // 重新激活时需要重新登记依赖：依赖关系由 dependOnInheritedWidgetOfExactType
    // 在下次 build 时重新建立（上游的 _updateInheritance）。
}

RenderObjectElement* Element::ancestorRenderObjectElement() {
    // 上游的 _findAncestorRenderObjectElement：向上找最近的
    // RenderObjectElement（跳过非渲染 Element）。
    for (Element* p = parent_; p; p = p->parent_) {
        if (auto* ro = dynamic_cast<RenderObjectElement*>(p))
            return ro;
    }
    return nullptr;
}

RenderObject* Element::findRenderObject() {
    // 上游的 Element.renderObject：沿 renderObjectAttachingChild 链向下，
    // 遇到第一个 RenderObjectElement 即返回其 RenderObject。
    Element* current = this;
    while (current) {
        if (auto* ro = dynamic_cast<RenderObjectElement*>(current))
            return ro->renderObject();
        current = current->renderObjectAttachingChild();
    }
    return nullptr;
}

std::unique_ptr<Element> Element::createChildFor(const SharedPtr<Widget>& widget) {
    if (!widget)
        return nullptr;
    return widget->createElement(widget);
}

void Element::disposeChild(std::unique_ptr<Element>& child) {
    if (!child)
        return;
    // 上游的 deactivateChild：先 deactivate（解除依赖），随后在帧末
    // unmount。这里直接完成两步，语义等价。
    child->deactivate();
    child->unmount();
    child.reset();
}

std::unique_ptr<Element> Element::updateChild(std::unique_ptr<Element> child,
                                               const SharedPtr<Widget>& newWidget) {
    // ── 上游的 updateChild（framework.dart:4002-4095）─────────────────
    // 1. newWidget == null            → 卸载旧 child
    // 2. child.widget == newWidget    → 同一实例，直接复用，**不 update、不重建**
    // 3. Widget.canUpdate(old, new)   → child.update(newWidget)
    // 4. 否则                          → 卸载旧 child + 挂载新 Element
    if (!newWidget) {
        disposeChild(child);
        return nullptr;
    }

    if (child) {
        Widget* oldWidget = child->widget();
        if (oldWidget == newWidget.get()) {
            // 同一 Widget 实例：上游 直接复用旧 Element，
            // 既不调用 update，也不触发重建。
            return child;
        }
        if (oldWidget && newWidget->canUpdate(*oldWidget)) {
            child->update(newWidget);
            return child;
        }
        disposeChild(child);
    }

    // inflateWidget：创建并挂载新的 Element。
    auto newChild = createChildFor(newWidget);
    if (newChild) {
        newChild->mount(this, owner_);
        // 上游 中 ComponentElement.mount 内部即 _firstBuild() → rebuild()；
        // 这里显式触发首次构建。
        newChild->rebuild();
    }
    return newChild;
}

void Element::reconcileChild(std::unique_ptr<Element>& slot,
                             const SharedPtr<Widget>& newWidget) {
    slot = updateChild(std::move(slot), newWidget);
}

void* Element::dependOnInheritedWidgetOfExactTypeRaw(const std::type_index& type) {
    // 上游的 dependOnInheritedWidgetOfExactType（framework.dart:2467-2490）：
    //   - 向上查找最近的、widget.runtimeType == T 的 InheritedElement；
    //   - 命中即登记依赖（数据变化时本 Element 收到 didChangeDependencies）；
    //   - 返回值是 **InheritedWidget 本身**（不是裸数据指针）。
    for (Element* p = parent_; p; p = p->parent_) {
        auto* inh = dynamic_cast<InheritedElement*>(p);
        if (!inh)
            continue;
        Widget* w = inh->widget();
        if (!w || typeid(*w) != type)
            continue;
        inh->addDependent(this);
        dependencies_.push_back(inh);
        return static_cast<void*>(w);
    }
    return nullptr;
}

void* Element::getInheritedWidgetOfExactTypeRaw(const std::type_index& type) {
    // 与上面的区别：不登记依赖（上游的 getInheritedWidgetOfExactType）。
    for (Element* p = parent_; p; p = p->parent_) {
        auto* inh = dynamic_cast<InheritedElement*>(p);
        if (!inh)
            continue;
        Widget* w = inh->widget();
        if (w && typeid(*w) == type)
            return static_cast<void*>(w);
    }
    return nullptr;
}

void* Element::findAncestorWidgetOfExactTypeRaw(const std::type_index& type) {
    // 按 Widget 自身的运行时类型查找（不限 InheritedWidget）。
    for (Element* p = parent_; p; p = p->parent_) {
        Widget* w = p->widget();
        if (w && typeid(*w) == type)
            return w;
    }
    return nullptr;
}

void* Element::findAncestorStateOfTypeRaw(const std::type_index& type) {
    // 上游的 findAncestorStateOfType（framework.dart:2509-2524）：
    // 沿父链向上找第一个「持有 T 类型 State」的 StatefulElement。
    // Overlay.of(context) / Navigator.of(context) 依赖此机制定位上层状态对象。
    for (Element* p = parent_; p; p = p->parent_) {
        auto* se = dynamic_cast<StatefulElement*>(p);
        if (!se)
            continue;
        State* s = se->state();
        if (s && typeid(*s) == type)
            return s;
    }
    return nullptr;
}

// ── ComponentElement ────────────────────────────────────────────────────────

void ComponentElement::visitChildren(const std::function<void(Element*)>& visitor) {
    if (child_)
        visitor(child_.get());
}

void ComponentElement::unmount() {
    if (child_) {
        child_->unmount();
        child_.reset();
    }
    Element::unmount();
}

void ComponentElement::deactivate() {
    if (child_)
        child_->deactivate();
    Element::deactivate();
}

void ComponentElement::activate() {
    if (child_)
        child_->activate();
    Element::activate();
}

std::unique_ptr<Element> ComponentElement::createChildFor(const SharedPtr<Widget>& widget) {
    return Element::createChildFor(widget);
}

// ── StatelessElement ────────────────────────────────────────────────────────

void StatelessElement::performRebuild() {
    auto* stateless = dynamic_cast<StatelessWidget*>(widget_.get());
    if (!stateless)
        return;
    BuildContext* ctx = this;
    auto built = stateless->build(*ctx);
    // 上游的 ComponentElement.performRebuild（framework.dart:5830-5874）：
    //   built = build(); super.performRebuild(); _child = updateChild(_child, built);
    Element::performRebuild();
    reconcileChild(child_, built);
}

void StatelessElement::update(SharedPtr<Widget> newWidget) {
    // 上游的 StatelessElement.update（framework.dart:5909-5915）：
    //   super.update(newWidget); rebuild(force: true);
    Element::update(std::move(newWidget));
    rebuild(true);
}

// ── StatefulElement ─────────────────────────────────────────────────────────

StatefulElement::StatefulElement(SharedPtr<Widget> widget) : ComponentElement(std::move(widget)) {
    auto* stateful = dynamic_cast<StatefulWidget*>(this->widget_.get());
    if (stateful) {
        state_ = stateful->createState();
        if (state_) {
            state_->widget_ = this->widget_.get();
            state_->context_ = this;
            state_->element_ = this;
        }
    }
}

StatefulElement::~StatefulElement() = default;

void StatefulElement::didMount() {
    // 上游的 StatefulElement._firstBuild（framework.dart:5968-5994）：
    //   state.initState(); state.didChangeDependencies(); super._firstBuild();
    if (state_) {
        state_->mounted_ = true;
        state_->initState();
        state_->didChangeDependencies();
    }
}

void StatefulElement::didUnmount() {
    if (state_) {
        state_->mounted_ = false;
        state_->dispose();
        // 上游的 StatefulElement.unmount 在 dispose 之后把 _element 置空，
        // 避免 dispose 后的任何 setState 经由悬空 element_ 调到 markNeedsBuild。
        state_->element_ = nullptr;
    }
}

void StatefulElement::performRebuild() {
    if (!state_)
        return;
    // 上游的 StatefulElement.performRebuild（framework.dart:5997-6003）：
    //   if (_didChangeDependencies) { state.didChangeDependencies(); _didChangeDependencies = false; }
    //   super.performRebuild();
    if (didChangeDependenciesPending_) {
        didChangeDependenciesPending_ = false;
        state_->didChangeDependencies();
    }
    BuildContext* ctx = this;
    auto built = state_->build(*ctx);
    Element::performRebuild();
    reconcileChild(child_, built);
}

void StatefulElement::didChangeDependencies() {
    // 上游的 StatefulElement.didChangeDependencies（framework.dart:6136-6140）：
    //   super.didChangeDependencies();  // → markNeedsBuild()
    //   _didChangeDependencies = true;
    // 真正的 state.didChangeDependencies() 在下次 build 之前由 performRebuild 触发。
    Element::didChangeDependencies();
    didChangeDependenciesPending_ = true;
}

void StatefulElement::update(SharedPtr<Widget> newWidget) {
    // 上游的 StatefulElement.update（framework.dart:6004-6031）：
    //   super.update(newWidget);
    //   state._widget = widget; state.didUpdateWidget(oldWidget);
    //   rebuild(force: true);
    // 必须像 ProxyElement.update 那样留住旧 Widget 的 shared_ptr：Element::update
    // 一移交所有权，旧对象就可能当场析构，而 didUpdateWidget 还要读它。
    SharedPtr<Widget> oldWidget = widget_;
    Element::update(std::move(newWidget));
    if (state_) {
        state_->widget_ = this->widget_.get();
        state_->didUpdateWidget(oldWidget.get());
    }
    rebuild(true);
}

// ── ProxyElement ────────────────────────────────────────────────────────────

void ProxyElement::performRebuild() {
    // 上游的 ProxyElement.build() => (widget as ProxyWidget).child。
    auto* proxy = dynamic_cast<ProxyWidget*>(widget_.get());
    SharedPtr<Widget> child = proxy ? proxy->child() : nullptr;
    Element::performRebuild();
    reconcileChild(child_, child);
}

void ProxyElement::update(SharedPtr<Widget> newWidget) {
    // 上游的 ProxyElement.update（framework.dart:6164-6170）：
    //   super.update(newWidget); updated(oldWidget); rebuild(force: true);
    // 保留旧配置的一份 shared_ptr，确保 updated() 回调期间旧 Widget 仍存活。
    SharedPtr<Widget> oldWidget = widget_;
    Element::update(std::move(newWidget));
    if (oldWidget)
        updated(*oldWidget);
    rebuild(true);
}

// ── InheritedElement ────────────────────────────────────────────────────────

void InheritedElement::addDependent(Element* dependent) {
    if (!dependent)
        return;
    if (std::find(dependents_.begin(), dependents_.end(), dependent) == dependents_.end())
        dependents_.push_back(dependent);
}

void InheritedElement::removeDependent(Element* dependent) {
    dependents_.erase(std::remove(dependents_.begin(), dependents_.end(), dependent),
                      dependents_.end());
}

void InheritedElement::updated(const Widget& oldWidget) {
    // 上游的 InheritedElement.updated（framework.dart:6421-6426）：
    //   if ((widget as InheritedWidget).updateShouldNotify(oldWidget)) {
    //     super.updated(oldWidget);   // → notifyClients(oldWidget)
    //   }
    // 只有数据确实变化时才通知依赖者，避免整棵子树的无谓重建。
    auto* proxy = dynamic_cast<const ProxyWidget*>(widget_.get());
    if (!proxy)
        return;
    if (proxy->shouldNotify(oldWidget))
        ProxyElement::updated(oldWidget);
}

void InheritedElement::notifyClients(const Widget& oldWidget) {
    (void)oldWidget;
    // 上游的 InheritedElement.notifyClients（framework.dart:6440-6459）：
    // 对每个依赖者调用 didChangeDependencies()（进而标记重建）。
    for (Element* dep : dependents_)
        dep->handleDependencyChanged();
}

// ── ParentDataElement ───────────────────────────────────────────────────────

void ParentDataElement::notifyClients(const Widget& oldWidget) {
    (void)oldWidget;
    // 上游的 ParentDataElement.notifyClients：重新把 parentData 写到
    // 目标 RenderObject 上。
    applyParentDataToChild();
}

void ParentDataElement::applyParentDataToChild() {
    auto* pdw = dynamic_cast<ParentDataWidget*>(widget_.get());
    if (!pdw)
        return;
    // 上游的 _applyParentData：从 renderObjectAttachingChild 出发，
    // 找到第一个 RenderObjectElement，对其调用 applyParentData。
    Element* current = renderObjectAttachingChild();
    while (current) {
        if (auto* ro = dynamic_cast<RenderObjectElement*>(current)) {
            if (ro->renderObject()) {
                pdw->applyParentData(ro->renderObject());
                // 上游：写完 parentData 后必须让父布局失效。
                if (ro->renderObject()->parent())
                    static_cast<RenderObject*>(ro->renderObject()->parent())
                            ->markNeedsLayout();
            }
            return;
        }
        current = current->renderObjectAttachingChild();
    }
}

// ── RenderObjectElement ─────────────────────────────────────────────────────

RenderObjectElement::RenderObjectElement(SharedPtr<Widget> widget)
    : Element(std::move(widget)) {}

RenderObjectElement::~RenderObjectElement() = default;

std::unique_ptr<RenderObject> RenderObjectElement::createRenderObject() {
    auto* row = dynamic_cast<RenderObjectWidget*>(widget_.get());
    if (!row)
        return nullptr;
    return std::unique_ptr<RenderObject>(row->createRenderObject());
}

void RenderObjectElement::didMount() {
    // 上游的 RenderObjectElement.mount（framework.dart:6810-6829）：
    //   _renderObject = widget.createRenderObject(this);
    //   attachRenderObject(newSlot);
    //   super.performRebuild();  // clears the "dirty" flag
    if (!renderObject_)
        renderObject_ = createRenderObject();
    if (renderObject_) {
        if (auto* ancestor = ancestorRenderObjectElement())
            ancestor->insertRenderObjectChild(renderObject_.get(), this);
        else if (RenderObject* host = owner_ ? owner_->rootRenderObjectHost() : nullptr)
            host->insertChild(renderObject_.get(), nullptr);
        applyAncestorParentData();
    }
}

void RenderObjectElement::didUnmount() {
    // 上游的 detachRenderObject：从祖先移除。
    if (renderObject_) {
        if (auto* ancestor = ancestorRenderObjectElement()) {
            ancestor->removeRenderObjectChild(renderObject_.get());
        } else if (!renderTeardown()) {
            // 根级渲染对象没有祖先 Element，但它的 parent 是宿主（RenderView，
            // 或测试里手动 setChild 的那个）。必须走宿主的 removeChild 把链接断
            // 开——只 setParent(nullptr) 会让宿主的 child_ 指着即将释放的对象，
            // 下一帧 performLayout 直接打在悬空指针上。整树拆除期宿主可能已经
            // 先一步析构，故仅在非拆除模式下做这件事。
            if (auto* parent = static_cast<RenderObject*>(renderObject_->parent()))
                parent->removeChild(renderObject_.get());
        }
        renderObject_->setParent(nullptr);
        renderObject_.reset();
    }
}

void RenderObjectElement::update(SharedPtr<Widget> newWidget) {
    // 上游的 RenderObjectElement.update（framework.dart:6832-6838）：
    //   super.update(newWidget); _performRebuild();  // → widget.updateRenderObject()
    // 注意：**不**重建子树，子树的 reconcile 由 Single/MultiChild 的
    // update 覆写完成。
    Element::update(std::move(newWidget));
    performRebuild();
}

void RenderObjectElement::applyAncestorParentData() {
    if (!renderObject_)
        return;
    // 上游的 _findAncestorParentDataElements：从 _parent 向上遍历，
    // 直到遇到 RenderObjectElement 为止，收集途经的 ParentDataElement。
    for (Element* p = parent(); p; p = p->parent()) {
        if (dynamic_cast<RenderObjectElement*>(p))
            break;
        if (auto* pd = dynamic_cast<ParentDataElement*>(p)) {
            auto* pdw = dynamic_cast<ParentDataWidget*>(pd->widget());
            if (pdw)
                pdw->applyParentData(renderObject_.get());
        }
    }
    if (renderObject_->parent())
        static_cast<RenderObject*>(renderObject_->parent())->markNeedsLayout();
}

void RenderObjectElement::performRebuild() {
    // 上游的 _performRebuild（framework.dart:6864-6875）：
    //   (widget as RenderObjectWidget).updateRenderObject(this, renderObject);
    //   super.performRebuild();
    if (!renderObject_)
        renderObject_ = createRenderObject();
    if (!renderObject_)
        return;

    // 同步配置到 RenderObject（上游的 updateRenderObject）。
    if (auto* row = dynamic_cast<RenderObjectWidget*>(widget_.get()))
        row->updateRenderObject(renderObject_.get());

    Element::performRebuild();
}

void RenderObjectElement::visitChildren(const std::function<void(Element*)>& visitor) {
    // RenderObjectElement 默认没有 Element 子节点（子 RenderObject 由
    // 祖先机制插入）。
}

void RenderObjectElement::unmount() {
    Element::unmount();
}

// ── SingleChildRenderObjectElement ──────────────────────────────────────────

void SingleChildRenderObjectElement::didMount() {
    // 上游的 SingleChildRenderObjectElement.mount（framework.dart:7139-7142）：
    //   super.mount(parent, newSlot);
    //   _child = updateChild(_child, (widget as SingleChildRenderObjectWidget).child, null);
    RenderObjectElement::didMount();

    auto* single = dynamic_cast<SingleChildRenderObjectWidget*>(widget_.get());
    SharedPtr<Widget> child = single ? single->child() : nullptr;
    child_ = updateChild(std::move(child_), child);
}

void SingleChildRenderObjectElement::performRebuild() {
    // 上游的 SingleChildRenderObjectElement 不覆写 performRebuild：
    // 子树的 reconcile 发生在 mount / update 中。
    RenderObjectElement::performRebuild();
}

void SingleChildRenderObjectElement::update(SharedPtr<Widget> newWidget) {
    // 上游的 SingleChildRenderObjectElement.update（framework.dart:7144-7148）：
    //   super.update(newWidget);
    //   _child = updateChild(_child, newWidget.child, null);
    auto* single = dynamic_cast<SingleChildRenderObjectWidget*>(newWidget.get());
    SharedPtr<Widget> child = single ? single->child() : nullptr;

    RenderObjectElement::update(std::move(newWidget));
    child_ = updateChild(std::move(child_), child);
}

void SingleChildRenderObjectElement::insertRenderObjectChild(RenderObject* child, Element* owner) {
    // 单子节点：直接挂到本 Element 的 RenderObject 上。
    (void)owner;
    if (!renderObject_ || !child)
        return;
    renderObject_->insertChild(child, nullptr);
}

void SingleChildRenderObjectElement::removeRenderObjectChild(RenderObject* child) {
    if (!renderObject_ || !child)
        return;
    renderObject_->removeChild(child);
}

void SingleChildRenderObjectElement::visitChildren(const std::function<void(Element*)>& visitor) {
    if (child_)
        visitor(child_.get());
}

void SingleChildRenderObjectElement::unmount() {
    if (child_) {
        child_->unmount();
        child_.reset();
    }
    RenderObjectElement::unmount();
}

void SingleChildRenderObjectElement::deactivate() {
    if (child_)
        child_->deactivate();
    Element::deactivate();
}

void SingleChildRenderObjectElement::activate() {
    if (child_)
        child_->activate();
    Element::activate();
}

// ── MultiChildRenderObjectElement ───────────────────────────────────────────

void MultiChildRenderObjectElement::didMount() {
    // 上游的 MultiChildRenderObjectElement.mount（framework.dart:7296-7313）：
    //   super.mount(parent, newSlot);
    //   逐个 inflateWidget(children[i])。
    RenderObjectElement::didMount();

    auto* multi = dynamic_cast<MultiChildRenderObjectWidget*>(widget_.get());
    std::vector<SharedPtr<Widget>> newWidgets;
    if (multi)
        newWidgets = multi->children();
    updateChildren(newWidgets);
}

void MultiChildRenderObjectElement::performRebuild() {
    // 上游的 MultiChildRenderObjectElement 不覆写 performRebuild：
    // 子树的 reconcile 发生在 mount / update 中。
    RenderObjectElement::performRebuild();
}

void MultiChildRenderObjectElement::update(SharedPtr<Widget> newWidget) {
    // 上游的 MultiChildRenderObjectElement.update（framework.dart:7315-7323）：
    //   super.update(newWidget);
    //   _children = updateChildren(_children, newWidget.children);
    auto* multi = dynamic_cast<MultiChildRenderObjectWidget*>(newWidget.get());
    std::vector<SharedPtr<Widget>> newWidgets;
    if (multi)
        newWidgets = multi->children();

    RenderObjectElement::update(std::move(newWidget));
    updateChildren(newWidgets);
}

void MultiChildRenderObjectElement::updateChildren(
        const std::vector<SharedPtr<Widget>>& newWidgets) {
    // ── 上游的 updateChildren（framework.dart:4145-4328）───────────────
    // 六步官方算法：
    //   1. 顶部同步：从头开始，只要 canUpdate 就复用旧 Element
    //   2. 底部扫描：从尾开始扫描（暂不同步），缩小中间区间
    //   3. 中部旧节点建 Key 表：无 Key 的旧节点被 deactivate
    //   4. 中部新节点按 Key 匹配：有 Key 且 canUpdate 则复用
    //   5. 尾部同步：剩余区间按位置复用
    //   6. 清理残余的 Key 节点
    updating_ = true;
    std::vector<std::unique_ptr<Element>> oldChildren = std::move(children_);
    children_.clear();

    const size_t newCount = newWidgets.size();
    const size_t oldCount = oldChildren.size();
    std::vector<std::unique_ptr<Element>> newChildren(newCount);

    // 索引一律用 **有符号** long（上游官方用 int）：
    // 空列表时 bottom = -1，若用无符号比较，`-1` 会变成极大值，
    // 使循环条件恒真并越界访问。
    long newTop = 0;
    long oldTop = 0;
    long newBottom = static_cast<long>(newCount) - 1;
    long oldBottom = static_cast<long>(oldCount) - 1;

    // ── 步骤 1：顶部同步 ─────────────────────────────────────────────
    while (oldTop <= oldBottom && newTop <= newBottom) {
        Element* oldChild = oldChildren[static_cast<size_t>(oldTop)].get();
        const SharedPtr<Widget>& newWidget = newWidgets[static_cast<size_t>(newTop)];
        if (!oldChild || !newWidget || !oldChild->widget() ||
            !newWidget->canUpdate(*oldChild->widget()))
            break;
        newChildren[static_cast<size_t>(newTop)] =
                updateChild(std::move(oldChildren[static_cast<size_t>(oldTop)]), newWidget);
        ++newTop;
        ++oldTop;
    }

    // ── 步骤 2：底部扫描（只缩小区间，稍后统一同步）─────────────────
    while (oldTop <= oldBottom && newTop <= newBottom) {
        Element* oldChild = oldChildren[static_cast<size_t>(oldBottom)].get();
        const SharedPtr<Widget>& newWidget = newWidgets[static_cast<size_t>(newBottom)];
        if (!oldChild || !newWidget || !oldChild->widget() ||
            !newWidget->canUpdate(*oldChild->widget()))
            break;
        --oldBottom;
        --newBottom;
    }

    // ── 步骤 3：扫描中部旧节点 ──────────────────────────────────────
    const bool haveOldChildren = oldTop <= oldBottom;
    KeyedChildMap oldKeyedChildren;
    if (haveOldChildren) {
        while (oldTop <= oldBottom) {
            std::unique_ptr<Element>& slot = oldChildren[static_cast<size_t>(oldTop)];
            if (slot) {
                Widget* w = slot->widget();
                if (w && w->key()) {
                    oldKeyedChildren.put(w->key(), std::move(slot));
                } else {
                    // 无 Key 的中部旧节点：上游 直接 deactivate，
                    // 不再复用（位置语义已失效）。
                    disposeChild(slot);
                }
            }
            ++oldTop;
        }
    }

    // ── 步骤 4：更新中部新节点（按 Key 匹配）────────────────────────
    while (newTop <= newBottom) {
        const SharedPtr<Widget>& newWidget = newWidgets[static_cast<size_t>(newTop)];
        std::unique_ptr<Element> oldChild;
        if (haveOldChildren && newWidget) {
            const KeyPtr& key = newWidget->key();
            if (key) {
                // 按 Key 相等性取回旧节点。
                std::unique_ptr<Element> candidate = oldKeyedChildren.take(key);
                if (candidate) {
                    if (candidate->widget() && newWidget->canUpdate(*candidate->widget())) {
                        oldChild = std::move(candidate);
                    } else {
                        // Key 相同但类型不匹配：丢弃旧节点。
                        disposeChild(candidate);
                    }
                }
            }
        }
        newChildren[static_cast<size_t>(newTop)] = updateChild(std::move(oldChild), newWidget);
        ++newTop;
    }

    // ── 步骤 5：尾部同步 ────────────────────────────────────────────
    // 上游 在步骤 3/4 之后把底指针复位到末尾，再按位置同步剩余区间。
    newBottom = static_cast<long>(newCount) - 1;
    oldBottom = static_cast<long>(oldCount) - 1;
    while (oldTop <= oldBottom && newTop <= newBottom) {
        newChildren[static_cast<size_t>(newTop)] =
                updateChild(std::move(oldChildren[static_cast<size_t>(oldTop)]),
                            newWidgets[static_cast<size_t>(newTop)]);
        ++newTop;
        ++oldTop;
    }

    // ── 步骤 6：清理残余的 Key 节点 ─────────────────────────────────
    oldKeyedChildren.disposeAll();

    children_ = std::move(newChildren);
    updating_ = false;

    // ── 按 children_ 顺序同步 RenderObject 子节点顺序 ─────────────────
    syncRenderObjectOrder();
}

void MultiChildRenderObjectElement::syncRenderObjectOrder() {
    if (!renderObject_)
        return;
    // 计算期望的 RenderObject 顺序：每个子 Element 沿
    // renderObjectAttachingChild 链找到它挂载的那个 RenderObject。
    std::vector<RenderObject*> desired;
    for (auto& child : children_) {
        if (!child)
            continue;
        RenderObject* ro = child->findRenderObject();
        if (ro)
            desired.push_back(ro);
    }
    renderObject_->reorderChildren(desired);
}

void MultiChildRenderObjectElement::insertRenderObjectChild(RenderObject* child, Element* owner) {
    if (!renderObject_ || !child)
        return;
    // 先追加（RenderBoxContainer 的 `after == nullptr` 语义）。正常 reconcile
    // 结束后由 updateChildren 末尾的 syncRenderObjectOrder 统一按 Element
    // 顺序重排。
    renderObject_->insertChild(child, nullptr);
    if (updating_)
        return;

    // 深层子树重建时本节点不会走 updateChildren（例如 Hero 占位者在
    // Offstage ↔ SizedBox 之间切换根 RenderObject 类型），尾部的重排也就
    // 不会发生，新 RenderObject 会停在末尾 ⇒ 布局顺序错乱。此时 children_
    // 是就位的，沿 owner 找到本节点的直接子节点，把它这一格换成新挂载的
    // RenderObject 再整体重排。
    //
    // 不能直接调 syncRenderObjectOrder：owner 的祖先此刻正在 `updateChild`
    // 中途（父 Element 的 child_ 已被 move 走），`findRenderObject()` 沿
    // `renderObjectAttachingChild` 链会返回 nullptr，新节点的位置就丢了。
    Element* direct = owner;
    while (direct && direct->parent() != this)
        direct = direct->parent();
    if (!direct)
        return;
    const auto it = std::find_if(
            children_.begin(), children_.end(),
            [direct](const std::unique_ptr<Element>& c) { return c.get() == direct; });
    if (it == children_.end())
        return;
    const size_t index = static_cast<size_t>(it - children_.begin());
    std::vector<RenderObject*> desired;
    desired.reserve(children_.size());
    for (size_t i = 0; i < children_.size(); ++i) {
        RenderObject* ro = i == index ? child : (children_[i] ? children_[i]->findRenderObject() : nullptr);
        if (ro)
            desired.push_back(ro);
    }
    renderObject_->reorderChildren(desired);
}

void MultiChildRenderObjectElement::removeRenderObjectChild(RenderObject* child) {
    if (!renderObject_ || !child)
        return;
    renderObject_->removeChild(child);
}

void MultiChildRenderObjectElement::moveRenderObjectChild(RenderObject* child, RenderObject* after) {
    if (!renderObject_ || !child)
        return;
    renderObject_->moveChild(child, after);
}

void MultiChildRenderObjectElement::visitChildren(const std::function<void(Element*)>& visitor) {
    for (auto& child : children_)
        if (child)
            visitor(child.get());
}

void MultiChildRenderObjectElement::unmount() {
    for (auto& child : children_)
        if (child)
            child->unmount();
    children_.clear();
    RenderObjectElement::unmount();
}

void MultiChildRenderObjectElement::deactivate() {
    for (auto& child : children_)
        if (child)
            child->deactivate();
    Element::deactivate();
}

void MultiChildRenderObjectElement::activate() {
    for (auto& child : children_)
        if (child)
            child->activate();
    Element::activate();
}

// ── Element 工厂函数 ───────────────────────────────────────────────────────
//
// 上游 里这些是 framework.dart 顶层的顶层函数：
//   Element inflateWidget(Widget widget) => widget is InheritedElement
//       ? InheritedElement(widget)
//       : throw ...;
//   Element inflateWidget(Widget widget) => widget is ParentDataElement
//       ? ParentDataElement(widget)
//       : throw ...;
// C++ 不能重载顶层函数，这里用两个不同名字，由 framework.h 声明。

std::unique_ptr<Element> createInheritedElement(SharedPtr<Widget> self) {
    return std::make_unique<InheritedElement>(std::move(self));
}

std::unique_ptr<Element> createParentDataElement(SharedPtr<Widget> self) {
    return std::make_unique<ParentDataElement>(std::move(self));
}

}
