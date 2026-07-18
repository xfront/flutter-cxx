#pragma once
/// @file widgets/notification.h
/// @brief 沿 Element 树向上冒泡的通知。上游的 widgets/notification_listener.dart。
///
/// 对应 上游源码：
///   - Notification / Notification.dispatch（framework 之上的通知基类）
///   - NotificationListener<T> + _NotificationElement（ProxyWidget/ProxyElement
///     + NotifiableElementMixin）
///   - LayoutChangedNotification
///   - ViewportNotificationMixin / ViewportElementMixin（depth 计数）
///
/// 语义要点（都在此逐条对齐）：
///   - 通知从 dispatch 目标的**父链**开始向上冒泡（自己不算监听者）；
///   - 回调返回 true ⇒ 冒泡终止；返回 false ⇒ 继续；
///   - `NotificationListener<T>` 只在通知是 T 的实例时才回调，其它类型原样放行
///     （本仓用 `typed<T>()` 工厂 + dynamic_cast 复刻泛型，见下）；
///   - 穿过 `Viewport` 这类视口 Element 时 `depth` 加一，监听者用
///     `depth == 0` 判断「是不是我最近的那个滚动视图」（上游 NestedScrollView
///     就靠这个区分本地/远端滚动）。
///
/// 与 上游 的差别：
///   - 上游是 Dart 泛型 `NotificationListener<T extends Notification>`；C++ 里
///     `onNotification` 统一收 `const Notification&`，类型过滤交给
///     `NotificationListener::typed<T>()` 包装的 dynamic_cast ⇒ 行为等价。
///   - 上游 `dispatch` 返回 void（`BuildContext.dispatchNotification` 才返回
///     bool）；本仓把结果透出，方便测试断言「有没有人接」。

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"

namespace cui {

class Notification;

/// 上游的 `NotificationListenerCallback<T>`。返回 true 取消冒泡。
using NotificationCallback = std::function<bool(Notification&)>;

/// 可沿 Widget 树向上冒泡的通知。上游的 `Notification`。
class Notification {
public:
    virtual ~Notification() = default;

    /// 从 `target` 的父链开始冒泡。上游的 `Notification.dispatch`。
    /// 返回是否被某个监听者处理（= 冒泡被中断）。
    bool dispatch(BuildContext* target) const {
        return target ? target->dispatchNotification(const_cast<Notification&>(*this)) : false;
    }

    virtual std::string toString() const { return "Notification"; }
};

/// 上游的 `ViewportNotificationMixin`：滚动/视口类通知带一个 depth 计数。
class ViewportNotification {
public:
    /// 本通知已经穿过多少个视口。监听者通常只回应 depth == 0 的通知。
    int depth() const { return depth_; }
    /// 上游 `ViewportElementMixin.onNotification` 里的那次 `+= 1`。
    void bumpDepth() { ++depth_; }

private:
    int depth_ = 0;
};

/// 上游的 `ViewportElementMixin.onNotification` 里的那次 `+= 1`。
/// 视口 Element（ViewportElement / SingleChildViewportElement）覆写
/// `onNotification` 时调用它。
inline void bumpViewportDepth(Notification& notification) {
    if (auto* vp = dynamic_cast<ViewportNotification*>(&notification))
        vp->bumpDepth();
}

/// 滚动/视口类通知的宿主 Element 基类。上游的 `ViewportElementMixin`（混进
/// `_ViewportElement`）。C++ 没有 mixin，改成两个各按子节点数派生的基类。
class ViewportElement : public MultiChildRenderObjectElement {
public:
    using MultiChildRenderObjectElement::MultiChildRenderObjectElement;

    /// 上游：`if (notification is ViewportNotificationMixin) depth += 1`，
    /// 且**不**中断冒泡（return false）。
    bool onNotification(Notification& n) override {
        bumpViewportDepth(n);
        return false;
    }
};

/// 盒子模式单子视口的 Element。上游的 `_SingleChildViewportElement`
/// （`_SingleChildViewport.createElement` 产出，混 NotifiableElementMixin +
/// ViewportElementMixin）。
class SingleChildViewportElement : public SingleChildRenderObjectElement {
public:
    using SingleChildRenderObjectElement::SingleChildRenderObjectElement;

    bool onNotification(Notification& n) override {
        bumpViewportDepth(n);
        return false;
    }
};

/// 子孙布局发生变化。上游的 `LayoutChangedNotification`。
class LayoutChangedNotification : public Notification {
public:
    std::string toString() const override { return "LayoutChangedNotification"; }
};

/// 监听冒泡通知的 Widget。上游的 `NotificationListener<T>`。
class NotificationListener : public ProxyWidget {
public:
    explicit NotificationListener(SharedPtr<Widget> child,
                                  NotificationCallback onNotification = {}, KeyPtr key = nullptr)
        : ProxyWidget(std::move(key), std::move(child)),
          onNotification_(std::move(onNotification)) {}

    /// 上游的 `NotificationListener<T>`：只有 T 及其子类才触发 cb，其余放行。
    template <class T>
    static SharedPtr<NotificationListener> typed(
        SharedPtr<Widget> child, std::function<bool(T&)> cb, KeyPtr key = nullptr) {
        return cui::make_shared<NotificationListener>(
            std::move(child),
            [cb = std::move(cb)](Notification& n) {
                auto* typed = dynamic_cast<T*>(&n);
                return typed && cb ? cb(*typed) : false;
            },
            std::move(key));
    }

    const NotificationCallback& onNotification() const { return onNotification_; }

    std::string toStringShort() const override { return "NotificationListener"; }

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override;

private:
    NotificationCallback onNotification_;
};

/// 上游的 `_NotificationElement`：本身不渲染，只在冒泡路径上插一个回调。
class NotificationListenerElement : public ProxyElement {
public:
    explicit NotificationListenerElement(SharedPtr<Widget> widget)
        : ProxyElement(std::move(widget)) {}

    bool onNotification(Notification& n) override {
        auto* w = static_cast<NotificationListener*>(widget());
        return w->onNotification() ? w->onNotification()(n) : false;
    }
};

inline std::unique_ptr<Element> NotificationListener::createElement(
    SharedPtr<Widget> self) {
    return std::make_unique<NotificationListenerElement>(std::move(self));
}

}  // namespace cui_core
