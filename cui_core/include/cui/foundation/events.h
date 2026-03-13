#pragma once
/// @file foundation/events.h
/// @brief Framework-level pointer event types (post hit-test, local coords).
///
/// 这些类型位于 foundation 层（仅依赖 geometry），以便 rendering 层
/// （RenderObject::handleEvent）在不反向依赖 widgets 层的前提下使用。
///
/// 语义严格对齐 上游的 gestures/events.dart：
///   - 事件在命中链上自最深节点（target）向根冒泡；
///   - 任意处理者调用 stopPropagation() 即可终止冒泡；
///   - position 在每经过一个 RenderObject 时被变换为该节点的本地坐标。

#include <cstdint>
#include <functional>

#include "cui/foundation/geometry.h"

namespace cui {

class RenderObject;

/// 指针设备种类。
enum class PointerDeviceKind {
    mouse,
    touch,
    stylus,
    unknown,
};

/// 鼠标按键位掩码（上游的 kPrimaryButton / kSecondaryButton …）。
using PointerButtons = uint64_t;
constexpr PointerButtons kPrimaryMouseButton = 1ull << 0;    // 左键
constexpr PointerButtons kSecondaryMouseButton = 1ull << 1;  // 右键
constexpr PointerButtons kMiddleMouseButton = 1ull << 2;     // 中键
constexpr PointerButtons kBackMouseButton = 1ull << 3;
constexpr PointerButtons kForwardMouseButton = 1ull << 4;

/// 键盘修饰键位掩码（与 platform/event.h 的 KeyModifier 取值相同）。
using PointerModifiers = int;

/// 事件语义类型，用于在非多态的指针事件上区分 down/move/up/…。
enum class PointerEventType {
    Down,
    Move,
    Up,
    Hover,
    Scroll,
    Cancel,
    Enter,  ///< 指针进入某渲染对象（hover 目标变化时派发，对齐上游）
    Exit,   ///< 指针离开某渲染对象（hover 目标变化时派发，对齐上游）
};

/// 框架级指针事件（上游的 PointerEvent）。
///
/// position 在分发到某个 RenderObject 时已被变换为该节点的本地坐标；
/// delta 为相对于上一事件的位移（平移不变，无需变换）。
struct PointerEvent {
    PointerEventType type = PointerEventType::Down;
    PointerDeviceKind pointerKind = PointerDeviceKind::mouse;
    int64_t pointerId = 0;          ///< 物理指针唯一标识
    Offset position{};              ///< 当前目标节点的本地坐标
    Offset globalPosition{};        ///< 窗口全局坐标（分发器写入；识别器判 slop 用）
    Offset delta{};                 ///< 自上一事件以来的位移
    PointerButtons buttons = 0;     ///< 事件发生时按住的按键位
    PointerModifiers modifiers = 0; ///< 键盘修饰键（Shift/Ctrl/…）
    double pressure = 0;            ///< 压力（触摸/笔）
    double scrollDeltaX = 0;        ///< 滚轮/滚动横向增量（像素，正值向右）
    double scrollDeltaY = 0;        ///< 滚轮/滚动纵向增量（像素，正值向下）

    /// 冒泡控制（上游的 stopPropagation）。任意处理者置位后，
    /// 分发器停止继续向父链上行。
    mutable bool stopPropagation_ = false;
    void stopPropagation() const { stopPropagation_ = true; }
    bool propagationStopped() const { return stopPropagation_; }
};

struct PointerDownEvent : PointerEvent {
    PointerDownEvent() { type = PointerEventType::Down; }
};
struct PointerMoveEvent : PointerEvent {
    PointerMoveEvent() { type = PointerEventType::Move; }
};
struct PointerUpEvent : PointerEvent {
    PointerUpEvent() { type = PointerEventType::Up; }
};
struct PointerHoverEvent : PointerEvent {
    PointerHoverEvent() { type = PointerEventType::Hover; }
};
struct PointerEnterEvent : PointerEvent {
    PointerEnterEvent() { type = PointerEventType::Enter; }
};
struct PointerExitEvent : PointerEvent {
    PointerExitEvent() { type = PointerEventType::Exit; }
};
struct PointerScrollEvent : PointerEvent {
    PointerScrollEvent() { type = PointerEventType::Scroll; }
};

/// 命中链上的一个条目（上游的 HitTestEntry）。
///
/// 仅持有被命中的 RenderObject（target）。事件沿命中链分发时，
/// 每个 RenderObject 通过一个 HitTestEntry 收到事件。
class HitTestEntry {
public:
    explicit HitTestEntry(RenderObject* target) : target_(target) {}
    RenderObject* target() const { return target_; }

private:
    RenderObject* target_ = nullptr;
};

}  // namespace cui_core
