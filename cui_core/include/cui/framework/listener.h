#pragma once
/// @file framework/listener.h
/// @brief 输入事件交互组件（上游的 Listener 移植）。
///
/// 对应 上游源码：上游 widgets/basic.dart 的
/// `Listener` + rendering/box.dart 的 `RenderPointerListener`。
///
/// 设计要点：
///   - RenderPointerListener 是 RenderProxyBox，hitTestSelf 恒为 true
///     （始终进入命中链），从而能接收冒泡到它的指针事件；
///   - 事件按 PointerEventType 分发到 onPointerDown/Move/Up/Hover/Scroll；
///   - 处理者可在回调内调用 event.stopPropagation() 终止冒泡。

#include <functional>

#include "../utils/shared_ptr.h"
#include "cui/rendering/render_box.h"
#include "framework.h"
#include "render_object_widget.h"

namespace cui {

/// 接收指针事件的渲染对象。上游的 `RenderPointerListener`。
class RenderPointerListener : public RenderProxyBox {
public:
    using PointerCallback = std::function<void(const PointerEvent&)>;

    PointerCallback onPointerDown;
    PointerCallback onPointerMove;
    PointerCallback onPointerUp;
    PointerCallback onPointerHover;
    PointerCallback onPointerScroll;
    PointerCallback onPointerEnter;
    PointerCallback onPointerExit;

    /// 始终可被命中（在自身尺寸范围内）。
    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 && position.dx < size_.width &&
               position.dy < size_.height;
    }

    /// 单子节点代理：把布局下传给子节点，并以子节点尺寸作为自身尺寸。
    void performLayout() override {
        if (child_) {
            child_->layout(constraints(), false);
            setSize(child_->size());
        } else {
            setSize(constraints().biggest());
        }
    }

    void handleEvent(const PointerEvent& event, const HitTestEntry& entry) override {
        (void)entry;
        switch (event.type) {
            case PointerEventType::Down:
                if (onPointerDown) onPointerDown(event);
                break;
            case PointerEventType::Move:
                if (onPointerMove) onPointerMove(event);
                break;
            case PointerEventType::Up:
                if (onPointerUp) onPointerUp(event);
                break;
            case PointerEventType::Hover:
                if (onPointerHover) onPointerHover(event);
                break;
            case PointerEventType::Enter:
                if (onPointerEnter) onPointerEnter(event);
                break;
            case PointerEventType::Exit:
                if (onPointerExit) onPointerExit(event);
                break;
            case PointerEventType::Scroll:
                if (onPointerScroll) onPointerScroll(event);
                break;
            default:
                break;
        }
    }
};

/// 把指针事件暴露给回调的 Widget。上游的 `Listener`。
class Listener : public SingleChildRenderObjectWidget {
public:
    using PointerCallback = RenderPointerListener::PointerCallback;

    explicit Listener(PointerCallback onDown = {}, PointerCallback onMove = {},
                      PointerCallback onUp = {}, PointerCallback onHover = {},
                      PointerCallback onScroll = {},
                      SharedPtr<Widget> child = nullptr,
                      PointerCallback onEnter = {}, PointerCallback onExit = {},
                      KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          onDown_(std::move(onDown)), onMove_(std::move(onMove)),
          onUp_(std::move(onUp)), onHover_(std::move(onHover)),
          onScroll_(std::move(onScroll)), onEnter_(std::move(onEnter)),
          onExit_(std::move(onExit)) {}

    RenderObject* createRenderObject() override { return new RenderPointerListener(); }

    void updateRenderObject(RenderObject* ro) override {
        auto* l = dynamic_cast<RenderPointerListener*>(ro);
        if (!l)
            return;
        l->onPointerDown = onDown_;
        l->onPointerMove = onMove_;
        l->onPointerUp = onUp_;
        l->onPointerHover = onHover_;
        l->onPointerScroll = onScroll_;
        l->onPointerEnter = onEnter_;
        l->onPointerExit = onExit_;
    }

private:
    PointerCallback onDown_, onMove_, onUp_, onHover_, onScroll_, onEnter_, onExit_;
};

}  // namespace cui_core
