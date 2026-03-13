#pragma once
/// @file widgets/gestures.h
/// @brief 手势识别组件（上游的 GestureDetector 移植，基础子集）。
///
/// 对应 上游源码：上游 widgets/gesture_detector.dart
/// 的 `GestureDetector` + gestures/ 下的 recognizer 体系。
///
/// 设计要点：
///   - 本实现不直接引入 GestureArena（多识别器竞争）子系统，而是用一个
///     轻量 `Recognizer` 把 Listener 的指针 down/move/up 合成高层语义：
///        * 同一指针 down→up（无位移要求）⇒ onTap（并先发 onTapDown/onTapUp）；
///        * down 后按住移动（指针仍按下）⇒ onPanDown / onPanUpdate / onPanEnd；
///     两者互不冲突：仅注册 onTap 的控件（如 Checkbox）只触发点击；仅注册
///     pan 回调的控件（如 Slider）只触发拖拽。
///   - Recognizer 状态存在 GestureDetector 持有的 shared_ptr 中，跨重建与
///     跨事件存活（Element 复用 Widget 实例，render object 的回调始终是
///     最新一次 build 写入的闭包）。
///   - 右键（次级按键，鼠标专用）：只按右键时走独立的次级通道，up 未超 slop
///     ⇒ onSecondaryTap；不触发 onTap/pan（右键菜单用）。
///   - 多指：只跟踪「第一根按下且未抬起的指针」（primary pointer），其余
///     指针的事件被忽略，等价于单指交互。

#include <cstdio>
#include <functional>
#include <memory>

#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/listener.h"

namespace cui {

/// 手势识别器（内部）。跟踪单指 down→up / 拖拽过程。
struct GestureRecognizer {
    static constexpr double kTouchSlop = 18.0;  ///< 上游的 kTouchSlop
    /// 上游的 `kLongPressTimeout`（drag_target.h 里另有同一口径的 Duration 版）。
    static constexpr double kLongPressTimeoutMs = 500.0;

    int64_t downId = -1;   ///< 当前按下且被跟踪的指针 id（-1 = 无）
    Offset downPos{};      ///< 按下时的本地坐标
    bool panning = false;  ///< 是否已经过 down（用于 up 时判定 tap）
    bool moved = false;    ///< 位移是否已超出 slop（超出则不算 tap）
    bool secondary = false;  ///< 本次跟踪的是否为右键（次级按键）：不参与 tap/pan
    /// 本次按下已经出过长按 ⇒ up 时不再补 tap（上游靠竞技场让长按胜出后
    /// 取消 tap 识别器，本仓没有竞技场，用这个标志达到同一效果）。
    bool longPressFired = false;
};

/// 手势识别组件。上游的 `GestureDetector`（基础子集）。
///
/// 和 上游 一样是 StatefulWidget：识别器状态（primary 指针跟踪）存在
/// State 里，跨 setState 重建存活——否则拖拽中途一次 rebuild 就会丢
/// down 状态，后续 move 全部失效。
class GestureDetector : public StatefulWidget {
public:
    using TapCallback = std::function<void()>;
    using PointerCallback = std::function<void(const PointerEvent&)>;

    /// `onSecondaryTap` 是上游 `onSecondaryTap` 的移植，唯一偏离是回调带
    /// 指针事件（上游无参）——右键菜单要按**按下点**定位面板。
    ///
    /// 上游的 `onLongPress`：按住 ≥ 500ms 且位移未超 slop ⇒ 触发，且这一次
    /// 按下不再出 tap（上游靠竞技场仲裁，本仓用 `longPressFired` 标志实现）。
    explicit GestureDetector(SharedPtr<Widget> child = nullptr,
                             TapCallback onTap = {}, PointerCallback onTapDown = {},
                             PointerCallback onTapUp = {},
                             PointerCallback onPanDown = {},
                             PointerCallback onPanUpdate = {},
                             PointerCallback onPanEnd = {},
                             PointerCallback onSecondaryTap = {},
                             TapCallback onLongPress = {},
                             KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)),
          child_(std::move(child)),
          onTap_(std::move(onTap)),
          onTapDown_(std::move(onTapDown)),
          onTapUp_(std::move(onTapUp)),
          onPanDown_(std::move(onPanDown)),
          onPanUpdate_(std::move(onPanUpdate)),
          onPanEnd_(std::move(onPanEnd)),
          onSecondaryTap_(std::move(onSecondaryTap)),
          onLongPress_(std::move(onLongPress)) {}

    const SharedPtr<Widget>& child() const { return child_; }
    const TapCallback& onTap() const { return onTap_; }
    const PointerCallback& onTapDown() const { return onTapDown_; }
    const PointerCallback& onTapUp() const { return onTapUp_; }
    const PointerCallback& onPanDown() const { return onPanDown_; }
    const PointerCallback& onPanUpdate() const { return onPanUpdate_; }
    const PointerCallback& onPanEnd() const { return onPanEnd_; }
    const PointerCallback& onSecondaryTap() const { return onSecondaryTap_; }
    const TapCallback& onLongPress() const { return onLongPress_; }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "GestureDetector"; }

private:
    SharedPtr<Widget> child_;
    TapCallback onTap_;
    PointerCallback onTapDown_, onTapUp_, onPanDown_, onPanUpdate_, onPanEnd_,
        onSecondaryTap_;
    TapCallback onLongPress_;
};

/// GestureDetector 的状态：持有识别器（跨重建存活），每次 build 用最新
/// 的回调闭包装配 Listener。
class GestureDetectorState : public State {
public:
    void initState() override {
        // 长按计时器：只有注册了 onLongPress 的控件才会起表。按住期间
        // ticker 活跃 ⇒ 事件循环继续出帧（上游 TickerMode 同效果）。
        longPress_ = std::make_shared<Ticker>([this](Duration elapsed) {
            if (rec_->secondary || rec_->moved || rec_->downId < 0) {
                longPress_->stop();
                return;
            }
            if (elapsed.count() < GestureRecognizer::kLongPressTimeoutMs)
                return;
            rec_->longPressFired = true;
            longPress_->stop();
            auto* w = static_cast<GestureDetector*>(widget());
            if (w && mounted() && w->onLongPress())
                w->onLongPress()();
        });
    }

    void dispose() override {
        if (longPress_ && longPress_->isActive())
            longPress_->stop();
        longPress_.reset();
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        auto* w = static_cast<GestureDetector*>(widget());
        auto rec = rec_;
        auto ticker = longPress_;
        auto onTap = w->onTap();
        auto onTapDown = w->onTapDown();
        auto onTapUp = w->onTapUp();
        auto onPanDown = w->onPanDown();
        auto onPanUpdate = w->onPanUpdate();
        auto onPanEnd = w->onPanEnd();
        auto onSecondaryTap = w->onSecondaryTap();
        auto onLongPress = w->onLongPress();

        Listener::PointerCallback onDown = [rec, ticker, onTap, onLongPress, onTapDown,
                                            onPanDown](const PointerEvent& e) {
            if (e.type != PointerEventType::Down)
                return;
            // 鼠标需按左键；触摸/笔无按键位，直接视为 primary。
            // 只按右键（左键未按下）⇒ 次级点击：只等 up 出 onSecondaryTap，
            // 不触发 tap/pan（上游把 secondary 做成独立的 TapGestureRecognizer）。
            bool primary = (e.pointerKind != PointerDeviceKind::mouse) ||
                           ((e.buttons & kPrimaryMouseButton) != 0);
            bool secondary = !primary && ((e.buttons & kSecondaryMouseButton) != 0);
            if (!primary && !secondary)
                return;
            rec->downId = e.pointerId;
            rec->downPos = e.globalPosition;  // 全局坐标判 slop（内容滚动时本地坐标不变）
            rec->panning = true;
            rec->moved = false;
            rec->secondary = secondary;
            rec->longPressFired = false;
            if (secondary)
                return;
            if (onTapDown)
                onTapDown(e);
            if (onPanDown)
                onPanDown(e);
            if (onLongPress && ticker && !ticker->isActive())
                ticker->start();
        };

        Listener::PointerCallback onMove = [rec, ticker, onPanUpdate](const PointerEvent& e) {
            if (e.type != PointerEventType::Move)
                return;
            if (rec->panning && e.pointerId == rec->downId) {
                double dx = e.globalPosition.dx - rec->downPos.dx;
                double dy = e.globalPosition.dy - rec->downPos.dy;
                if (dx * dx + dy * dy > rec->kTouchSlop * rec->kTouchSlop) {
                    rec->moved = true;
                    // 一超出 slop，上游的长按识别器就被 pan 挤掉；这里直接停表。
                    if (ticker && ticker->isActive())
                        ticker->stop();
                }
                if (!rec->secondary && onPanUpdate)
                    onPanUpdate(e);
            }
        };

        Listener::PointerCallback onUp = [rec, ticker, onTap, onTapUp, onPanEnd,
                                          onSecondaryTap](const PointerEvent& e) {
            if (e.type != PointerEventType::Up)
                return;
            if (e.pointerId != rec->downId)
                return;
            bool wasPanning = rec->panning;
            bool moved = rec->moved;
            bool secondary = rec->secondary;
            bool longFired = rec->longPressFired;
            if (ticker && ticker->isActive())
                ticker->stop();
            rec->panning = false;
            rec->downId = -1;
            rec->secondary = false;
            rec->longPressFired = false;
            if (wasPanning && !moved && !longFired) {  // 位移超出 slop / 已出长按的不算 tap
                if (secondary) {
                    if (onSecondaryTap)
                        onSecondaryTap(e);
                } else {
                    if (onTapUp)
                        onTapUp(e);
                    if (onTap)
                        onTap();
                }
            }
            if (!secondary && onPanEnd)
                onPanEnd(e);
        };

        return cui::make_shared<Listener>(onDown, onMove, onUp, nullptr, nullptr,
                                          w->child());
    }

private:
    SharedPtr<GestureRecognizer> rec_ = cui::make_shared<GestureRecognizer>();
    /// 长按计时表（shared_ptr：build 里装配的 Listener 闭包各自持一份引用，
    /// 摘除子树后仍可能有一次 tick 在路上，指针必须一直有效）。
    std::shared_ptr<Ticker> longPress_;
};

inline std::unique_ptr<State> GestureDetector::createState() {
    return std::make_unique<GestureDetectorState>();
}

}  // namespace cui_core
