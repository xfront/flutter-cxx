/// @file widgets/framework.cpp
/// @brief Widget / State / StatelessWidget / StatefulWidget 实现。

#include "cui/framework/framework.h"

#include "../../include/cui/utils/shared_ptr.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"

namespace cui {

// ── Widget ──────────────────────────────────────────────────────────────────

bool Widget::canUpdate(const Widget& oldWidget) const {
    // 上游的 canUpdate：runtimeType 相同 && key 相同。
    if (typeid(*this) != typeid(oldWidget))
        return false;
    const KeyPtr& a = key_;
    const KeyPtr& b = oldWidget.key();
    if (!a && !b)
        return true;
    if (!a || !b)
        return false;
    return *a == *b;
}

// ── StatelessWidget ─────────────────────────────────────────────────────────

std::unique_ptr<Element> StatelessWidget::createElement(SharedPtr<Widget> self) {
    return std::make_unique<StatelessElement>(std::move(self));
}

// ── StatefulWidget ─────────────────────────────────────────────────────────

std::unique_ptr<Element> StatefulWidget::createElement(SharedPtr<Widget> self) {
    return std::make_unique<StatefulElement>(std::move(self));
}

// ── State ───────────────────────────────────────────────────────────────────

void State::setState(std::function<void()> fn) {
    if (!mounted_)
        return;  // 上游: setState() called after dispose() 是错误，此处忽略
    if (fn)
        fn();
    if (element_)
        element_->markNeedsBuild();
}

void State::setState() {
    // 与带回调的 setState 一致：已卸载（dispose 之后）的 State 直接忽略，
    // 否则 element_ 已是悬挂指针（控制器等长生命周期对象再次通知时触发）。
    if (!mounted_)
        return;
    if (element_)
        element_->markNeedsBuild();
}

// ── BuildContext ────────────────────────────────────────────────────────────

bool BuildContext::dispatchNotification(Notification& notification) const {
    // 上游 Element._visitAncestors：从**父节点**开始，逐个问 onNotification，
    // 第一个返回 true 的监听者中断冒泡。
    for (Element* a = element() ? element()->parent() : nullptr; a; a = a->parent())
        if (a->onNotification(notification))
            return true;
    return false;
}

void BuildContext::visitChildElements(const ElementVisitor& visitor) const {
    Element* self = element();
    if (!self || !visitor)
        return;
    // 上游是一条 debug 断言（「visitChildElements() called during build」）：
    // build 期间 child 列表仍在增删，此刻访问到的可能是即将被换掉的旧 child。
    // 本仓没有断言体系，等价处理是直接不执行。
    if (self->owner() && self->owner()->inBuildPhase())
        return;
    self->visitChildren(visitor);
}

bool BuildContext::visitAncestorElements(const ConditionalElementVisitor& visitor) const {
    // 上游 Element.visitAncestorElements：返回「是否一路走到了根」。
    Element* ancestor = element() ? element()->parent() : nullptr;
    while (ancestor != nullptr && (!visitor || visitor(ancestor)))
        ancestor = ancestor->parent();
    return ancestor == nullptr;
}

}
