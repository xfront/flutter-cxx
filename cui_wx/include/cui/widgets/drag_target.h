#pragma once
/// @file widgets/drag_target.h
/// @brief 拖拽源与拖放目标（上游的 Draggable / LongPressDraggable / DragTarget）。
///
/// 对应 上游源码：上游 widgets/drag_target.dart（`Draggable` /
/// `LongPressDraggable` / `DragTarget` / `_DragAvatar` / `_DragTargetState`）。
/// 命中链标记那一半在上游 rendering/proxy_box.dart（`RenderMetaData`）+
/// widgets/basic.dart（`MetaData`），本仓一并补齐。
///
/// 五条有意偏离/铁律（勿当 bug 排查）：
///
/// 1. **没有 GestureArena，拖拽由 Listener 三段合成**。上游用
///    `MultiDragGestureRecognizer` 家族（Immediate/Horizontal/Vertical/Delayed）
///    识别拖拽起点；本仓的 widgets/gestures.h 头注释已声明不引入 recognizer
///    体系，所以：
///      * `Draggable` = 上游 `ImmediateMultiDrag` ⇒ **按下即开始**拖拽；
///      * `LongPressDraggable` = 上游 `DelayedMultiDrag` ⇒ 按下后等
///        `dragDelay()`（`kLongPressTimeout`，500ms）由 Ticker 到点触发。
///    子类的扩展点是虚函数 `Draggable::dragDelay()`，替代上游的
///    `createRecognizer()`。
///    「指针移到盒子外面还能继续收到 move/up」靠 `PipelineOwner::dispatchPointerEvent`
///    按 pointerId 缓存 down 命中链（= 上游 GestureBinding 的 hitTestResult 缓存）。
///    竞技场输赢的另一半：`handleMove` 对**已起拖**的那个 pointer 调
///    `stopPropagation()`，于是外层 `Scrollable` 收不到这段移动（上游是同一条语义：
///    内层拖拽识别器赢掉滚动）。延时未到的 pointer **不**拦截，长按期间照样能滚。
/// 2. **泛型只在 DragTarget 一侧**。上游 `Draggable<T>` / `DragTarget<T>` 的
///    `T extends Object` 只用于 `_DragTargetState.isExpectedDataType` 的
///    `data is T?` 过滤；本仓让 avatar 携带 `std::any`（它自带 typeid），
///    同一个判据在 `DragTargetState<T>::matchesDataType` 里逐字复刻，
///    含「data 为空 ⇒ 放行，但 onAccept/onMove 不触发」。
/// 3. **avatar 与 target 的互相摘除必须显式**。上游靠 GC：avatar 里的
///    `_enteredTargets`、`_DraggableState` 里跨 dispose 存活的 recognizer 都
///    不需要处理生命周期。C++ 里两边都可能先死，所以有一张进程内的活动
///    avatar 表（`drag_detail::liveAvatars()`）：`DragTargetStateBase::dispose`
///    反向通知每个 avatar 忘掉自己，`DraggableState::dispose` 直接丢弃 avatar
///    （`~DragAvatar` 摘掉 OverlayEntry，**不**触发用户回调）。GUI 单线程 ⇒
///    不需要锁（与全仓 SharedPtr 口径一致）。
/// 4. **feedback 的命中测试从根渲染对象开始**（= 上游 `hitTestInView`），而不是
///    从 Overlay 盒子开始：DragTarget 常常和 Draggable 同在 Overlay 之下但分属
///    不同条目，从 Overlay 盒子起步会漏掉不在该子树里的目标。
/// 5. **未接的上游参数**：`affinity`（没有竞争者，无从「竞争」，本仓的 Draggable
///    总是先于滚动识别）、`allowedButtonsFilter`、`hitTestBehavior`（Listener 恒
///    为 opaque 语义）、`rootOverlay`（BuildContext 只有 nearest 查找）、
///    `ignoringFeedbackSemantics`（本仓无 Semantics）、`hapticFeedbackOnStart`
///    （无平台触感）、`Velocity`（无速度估计器 ⇒ `DraggableDetails.velocity`
///    恒 `Velocity::zero()`）、上游已废弃的 `onWillAccept` / `onAccept`
///    （3.47 推荐 WithDetails 版，只接新的那两个）。

#include <any>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <typeinfo>
#include <vector>

#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_flex.h"
#include "cui/widgets/basic.h"
#include "cui/framework/listener.h"
#include "cui/widgets/overlay.h"

namespace cui {

class Draggable;
class DragAvatar;
class DragTargetStateBase;

/// 上游的 `kLongPressTimeout`（gestures/constants.dart）。本仓没有
/// gestures/constants.dart，常量就近放在唯一的消费者旁边。
inline constexpr Duration kLongPressTimeout(500);

namespace drag_detail {

/// 正在进行的 avatar（铁律 3）。
inline std::vector<DragAvatar*>& liveAvatars() {
    static std::vector<DragAvatar*> sLive;
    return sLive;
}

/// 上游的 `_DragEndKind`。
enum class DragEndKind { Dropped, Canceled };

}  // namespace drag_detail

/// 上游的 `DragUpdateDetails`（本仓只留两个量）。
struct DragUpdateDetails {
    Offset globalPosition{};  ///< avatar 当前全局位置（已按 axis 限制）
    Offset delta{};           ///< 相对上一帧的位移（已按 axis 限制）
};

/// 上游的 `DraggableDetails`。
struct DraggableDetails {
    bool wasAccepted = false;
    Velocity velocity{};
    Offset offset{};          ///< 上游的 `offset`：`位置 − dragStartPoint`（非全局点）
};

/// 上游的 `DragTargetDetails<T>`。
template <typename T>
struct DragTargetDetails {
    T data;
    Offset offset{};
};

/// 上游的 `DragAnchorStrategy`：反馈盒子相对按下点的锚点。
using DragAnchorStrategy =
    std::function<Offset(const Draggable&, BuildContext&, const Offset&)>;

/// 上游的同名函数：反馈盒子锚在**原子节点**的位置上。
inline Offset childDragAnchorStrategy(const Draggable&, BuildContext& context,
                                     const Offset& position) {
    auto* element = context.element();
    auto* box = element ? dynamic_cast<RenderBox*>(element->findRenderObject()) : nullptr;
    return box ? box->globalToLocal(position) : Offset(0, 0);
}

/// 上游的同名函数：反馈盒子锚在**指针**位置（等价于零锚点）。
inline Offset pointerDragAnchorStrategy(const Draggable&, BuildContext&, const Offset&) {
    return Offset(0, 0);
}

/// 可被拖到 `DragTarget` 上的源。上游的 `Draggable`。
///
/// 泛型去掉（铁律 2）：载荷是 `std::any`。
class Draggable : public StatefulWidget {
public:
    /// 上游里 `_DraggableState` 是私有嵌套类，天然能读 protected 的
    /// `createRecognizer()`；本仓的 State 是平铺类，靠 friend 拿到同样的权限。
    friend class DraggableState;

    using DragStartCallback = std::function<void()>;
    using DragUpdateCallback = std::function<void(const DragUpdateDetails&)>;
    using DragEndCallback = std::function<void(const DraggableDetails&)>;
    using DraggableCanceledCallback = std::function<void(const Velocity&, const Offset&)>;

    explicit Draggable(SharedPtr<Widget> child, SharedPtr<Widget> feedback,
                       std::any data = {}, SharedPtr<Widget> childWhenDragging = nullptr,
                       std::optional<Axis> axis = {}, Offset feedbackOffset = {},
                       DragAnchorStrategy dragAnchorStrategy = childDragAnchorStrategy,
                       std::optional<int> maxSimultaneousDrags = {},
                       DragStartCallback onDragStarted = {},
                       DragUpdateCallback onDragUpdate = {},
                       DraggableCanceledCallback onDraggableCanceled = {},
                       DragEndCallback onDragEnd = {},
                       DragStartCallback onDragCompleted = {},
                       bool ignoringFeedbackPointer = true, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)),
          feedback_(std::move(feedback)), data_(std::move(data)),
          childWhenDragging_(std::move(childWhenDragging)), axis_(axis),
          feedbackOffset_(feedbackOffset),
          dragAnchorStrategy_(std::move(dragAnchorStrategy)),
          maxSimultaneousDrags_(maxSimultaneousDrags),
          onDragStarted_(std::move(onDragStarted)),
          onDragUpdate_(std::move(onDragUpdate)),
          onDraggableCanceled_(std::move(onDraggableCanceled)),
          onDragEnd_(std::move(onDragEnd)),
          onDragCompleted_(std::move(onDragCompleted)),
          ignoringFeedbackPointer_(ignoringFeedbackPointer) {}

    /// 上游的一组同名 getter。
    const SharedPtr<Widget>& child() const { return child_; }
    const SharedPtr<Widget>& feedback() const { return feedback_; }
    const std::any& data() const { return data_; }
    const SharedPtr<Widget>& childWhenDragging() const { return childWhenDragging_; }
    const std::optional<Axis>& axis() const { return axis_; }
    Offset feedbackOffset() const { return feedbackOffset_; }
    const DragAnchorStrategy& dragAnchorStrategy() const { return dragAnchorStrategy_; }
    const std::optional<int>& maxSimultaneousDrags() const { return maxSimultaneousDrags_; }
    const DragStartCallback& onDragStarted() const { return onDragStarted_; }
    const DragUpdateCallback& onDragUpdate() const { return onDragUpdate_; }
    const DraggableCanceledCallback& onDraggableCanceled() const {
        return onDraggableCanceled_;
    }
    const DragEndCallback& onDragEnd() const { return onDragEnd_; }
    const DragStartCallback& onDragCompleted() const { return onDragCompleted_; }
    bool ignoringFeedbackPointer() const { return ignoringFeedbackPointer_; }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "Draggable"; }

protected:
    /// 上游 `createRecognizer()` 在本仓的替代（铁律 1）：
    /// 0 ⇒ 按下即开始（ImmediateMultiDrag），>0 ⇒ 延时到点才开始（DelayedMultiDrag）。
    virtual Duration dragDelay() const { return Duration(0); }

private:
    SharedPtr<Widget> child_;
    SharedPtr<Widget> feedback_;
    std::any data_;
    SharedPtr<Widget> childWhenDragging_;
    std::optional<Axis> axis_;
    Offset feedbackOffset_{};
    DragAnchorStrategy dragAnchorStrategy_;
    std::optional<int> maxSimultaneousDrags_;
    DragStartCallback onDragStarted_;
    DragUpdateCallback onDragUpdate_;
    DraggableCanceledCallback onDraggableCanceled_;
    DragEndCallback onDragEnd_;
    DragStartCallback onDragCompleted_;
    bool ignoringFeedbackPointer_ = true;
};

/// 按下后延时才进入拖拽。上游的 `LongPressDraggable`。
class LongPressDraggable : public Draggable {
public:
    using Draggable::Draggable;

    /// 上游的 `delay`（默认 `kLongPressTimeout`）。
    void setDelay(Duration delay) { delay_ = delay; }
    Duration delay() const { return delay_; }

    std::string toStringShort() const override { return "LongPressDraggable"; }

protected:
    Duration dragDelay() const override { return delay_; }

private:
    Duration delay_ = kLongPressTimeout;
};

/// `DragTarget` 的 State 的共同基类。上游的 `_DragTargetState<Object>`
/// （avatar 只用 Object 视角存取它，具体的类型过滤由虚函数完成）。
class DragTargetStateBase : public State {
public:
    /// 上游的 `isExpectedDataType`：`data is T?`（空载荷放行）。
    virtual bool matchesDataType(const std::any& data) const = 0;

    /// 上游的 `didEnter/didLeave/didDrop/didMove`。
    virtual bool didEnter(DragAvatar& avatar) = 0;
    virtual void didLeave(DragAvatar& avatar) = 0;
    virtual void didDrop(DragAvatar& avatar) = 0;
    virtual void didMove(DragAvatar& avatar) = 0;

    /// 铁律 3：目标先于 avatar 销毁时，让所有在飞的 avatar 忘掉自己。
    /// 定义在 DragAvatar 之后。
    void dispose() override;

    /// 上游的 `_candidateAvatars`/`_rejectedAvatars`（builder 的两列数据源）。
    const std::vector<DragAvatar*>& candidateAvatars() const { return candidateAvatars_; }
    const std::vector<DragAvatar*>& rejectedAvatars() const { return rejectedAvatars_; }

protected:
    bool contains(const std::vector<DragAvatar*>& list, DragAvatar* a) const {
        for (auto* x : list)
            if (x == a)
                return true;
        return false;
    }
    void removeOne(std::vector<DragAvatar*>& list, DragAvatar* a) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i] == a) {
                list.erase(list.begin() + static_cast<long>(i));
                return;
            }
        }
    }

    std::vector<DragAvatar*> candidateAvatars_;
    std::vector<DragAvatar*> rejectedAvatars_;
};

template <typename T>
class DragTargetState;

/// 接收拖放的目标。上游的 `DragTarget<T>`。
///
/// 上游 3.47 已把 `onWillAccept` / `onAccept` 标成 deprecated（不带 details 的
/// 旧版），本仓只接 `*WithDetails` 那两个。
template <typename T>
class DragTarget : public StatefulWidget {
public:
    /// 上游的 `DragTargetBuilder<T>`：candidateData 是 `List<T?>`，
    /// rejectedData 是 `List<dynamic>`。
    using Builder = std::function<SharedPtr<Widget>(
        BuildContext&, const std::vector<std::optional<T>>& candidateData,
        const std::vector<std::any>& rejectedData)>;
    using WillAcceptWithDetails = std::function<bool(const DragTargetDetails<T>&)>;
    using AcceptWithDetails = std::function<void(const DragTargetDetails<T>&)>;
    using LeaveCallback = std::function<void(const std::optional<T>&)>;
    using MoveCallback = std::function<void(const DragTargetDetails<T>&)>;

    explicit DragTarget(Builder builder, WillAcceptWithDetails onWillAcceptWithDetails = {},
                        AcceptWithDetails onAcceptWithDetails = {},
                        LeaveCallback onLeave = {}, MoveCallback onMove = {},
                        KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), builder_(std::move(builder)),
          onWillAcceptWithDetails_(std::move(onWillAcceptWithDetails)),
          onAcceptWithDetails_(std::move(onAcceptWithDetails)),
          onLeave_(std::move(onLeave)), onMove_(std::move(onMove)) {}

    const Builder& builder() const { return builder_; }
    const WillAcceptWithDetails& onWillAcceptWithDetails() const {
        return onWillAcceptWithDetails_;
    }
    const AcceptWithDetails& onAcceptWithDetails() const { return onAcceptWithDetails_; }
    const LeaveCallback& onLeave() const { return onLeave_; }
    const MoveCallback& onMove() const { return onMove_; }

    std::unique_ptr<State> createState() override {
        return std::make_unique<DragTargetState<T>>();
    }

    std::string toStringShort() const override { return "DragTarget"; }

private:
    Builder builder_;
    WillAcceptWithDetails onWillAcceptWithDetails_;
    AcceptWithDetails onAcceptWithDetails_;
    LeaveCallback onLeave_;
    MoveCallback onMove_;
};

/// 上游的 `_DragAvatar`：一次在飞的拖拽。
///
/// 生命周期由发起它的 `DraggableState` 持有（SharedPtr），抬起/取消即结束并从
/// 持有表里摘除；`DraggableState` 提前销毁时由析构函数兜底摘掉 OverlayEntry。
class DragAvatar {
public:
    using DragUpdateCallback = std::function<void(const DragUpdateDetails&)>;
    using DragEndCallback = std::function<void(const Velocity&, const Offset&, bool)>;

    DragAvatar(OverlayState& overlayState, std::any data, std::optional<Axis> axis,
               const Offset& initialPosition, const Offset& dragStartPoint,
               SharedPtr<Widget> feedback, const Offset& feedbackOffset,
               bool ignoringFeedbackPointer, DragUpdateCallback onDragUpdate,
               DragEndCallback onDragEnd)
        : data_(std::move(data)), axis_(axis), position_(initialPosition),
          lastGlobalPosition_(initialPosition), dragStartPoint_(dragStartPoint),
          feedback_(std::move(feedback)), feedbackOffset_(feedbackOffset),
          ignoringFeedbackPointer_(ignoringFeedbackPointer),
          onDragUpdate_(std::move(onDragUpdate)), onDragEnd_(std::move(onDragEnd)),
          overlayState_(&overlayState) {
        entry_ = cui::make_shared<OverlayEntry>(
            [this](BuildContext&) { return buildFeedback(); });
        overlayState_->insert(entry_);
        drag_detail::liveAvatars().push_back(this);
        updateDrag(position_);
    }

    ~DragAvatar() {
        auto& live = drag_detail::liveAvatars();
        for (size_t i = 0; i < live.size(); ++i) {
            if (live[i] == this) {
                live.erase(live.begin() + static_cast<long>(i));
                break;
            }
        }
        // 异常路径（源被拆掉但没走完 finishDrag）兜底；正常结束时 entry_ 已空。
        if (entry_ && entry_->mounted())
            entry_->remove();
    }

    DragAvatar(const DragAvatar&) = delete;
    DragAvatar& operator=(const DragAvatar&) = delete;

    const std::any& data() const { return data_; }
    Offset lastOffset() const { return lastOffset_; }

    /// 上游的 `Drag.update`：`globalPosition` 是**绝对**全局位置（本仓的
    /// 指针事件带绝对坐标，上游 recognizer 递的是 delta，两者等价）。
    void update(const Offset& globalPosition) {
        const Offset oldPosition = position_;
        position_ = position_ + restrictAxis(globalPosition - lastGlobalPosition_);
        lastGlobalPosition_ = globalPosition;
        updateDrag(position_);
        if (onDragUpdate_ && !(position_ == oldPosition)) {
            DragUpdateDetails details;
            details.globalPosition = position_;
            details.delta = position_ - oldPosition;
            onDragUpdate_(details);
        }
    }

    /// 上游的 `Drag.end`（velocity 恒 0：本仓无速度估计器）。
    void end() { finishDrag(drag_detail::DragEndKind::Dropped, Velocity::zero()); }

    /// 上游的 `Drag.cancel`。
    void cancel() { finishDrag(drag_detail::DragEndKind::Canceled, Velocity::zero()); }

    /// 铁律 3：目标销毁 ⇒ 从进入表里忘掉它（不回调 onLeave，那一刻 widget
    /// 已经不在了）。
    void handleTargetDisposed(DragTargetStateBase* target) {
        removeTarget(enteredTargets_, target);
        if (activeTarget_ == target)
            activeTarget_ = nullptr;
    }

    /// 上游的 `_DragAvatar.updateDrag`。
    void updateDrag(const Offset& globalPosition) {
        lastOffset_ = globalPosition - dragStartPoint_;

        if (overlayState_->mounted()) {
            if (RenderBox* box = overlayBox()) {
                overlayOffset_ = box->globalToLocal(globalPosition) - dragStartPoint_;
            }
            // 上游同样只在 overlayState.mounted 里 markNeedsBuild。
            if (entry_)
                entry_->markNeedsBuild();
        }

        // 铁律 4：从根渲染对象开始命中（= 上游 hitTestInView）。
        std::vector<RenderObject*> path;
        if (RenderObject* root = rootRenderObject()) {
            root->hitTest(globalPosition + feedbackOffset_, path);
        }

        // 上游的 isExpectedDataType 过滤，命中链顺序 = 根→叶，firstWhere 取
        // 第一个「接受」的目标。
        std::vector<DragTargetStateBase*> targets;
        for (RenderObject* ro : path) {
            auto* meta = dynamic_cast<RenderMetaData*>(ro);
            if (!meta || !meta->metaData())
                continue;
            auto* target = static_cast<DragTargetStateBase*>(meta->metaData());
            if (target->matchesDataType(data_))
                targets.push_back(target);
        }

        bool listsMatch = false;
        if (targets.size() >= enteredTargets_.size() && !enteredTargets_.empty()) {
            listsMatch = true;
            for (size_t i = 0; i < enteredTargets_.size(); ++i) {
                if (targets[i] != enteredTargets_[i]) {
                    listsMatch = false;
                    break;
                }
            }
        }
        // 目标集合没变 ⇒ 只报 move。
        if (listsMatch) {
            auto snapshot = enteredTargets_;
            for (auto* target : snapshot)
                target->didMove(*this);
            return;
        }

        // 先离开旧目标。
        leaveAllEntered();

        // 再进入新目标：上游 firstWhere 的谓词有副作用（逐个 add 进
        // _enteredTargets 并调 didEnter），到第一个接受者为止。
        DragTargetStateBase* newTarget = nullptr;
        for (auto* target : targets) {
            enteredTargets_.push_back(target);
            if (target->didEnter(*this)) {
                newTarget = target;
                break;
            }
        }

        auto snapshot = enteredTargets_;
        for (auto* target : snapshot)
            target->didMove(*this);

        activeTarget_ = newTarget;
    }

    /// 上游的 `_DragAvatar.finishDrag`。
    void finishDrag(drag_detail::DragEndKind endKind, const Velocity& velocity) {
        bool wasAccepted = false;
        if (endKind == drag_detail::DragEndKind::Dropped && activeTarget_) {
            activeTarget_->didDrop(*this);
            wasAccepted = true;
            removeTarget(enteredTargets_, activeTarget_);
        }
        leaveAllEntered();
        activeTarget_ = nullptr;
        if (entry_) {
            if (entry_->mounted())
                entry_->remove();
            entry_.reset();
        }
        if (onDragEnd_)
            onDragEnd_(velocity, lastOffset_, wasAccepted);
    }

private:
    /// 上游 `List.remove`：从进入表里摘掉一个目标。
    static void removeTarget(std::vector<DragTargetStateBase*>& list,
                             DragTargetStateBase* target) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i] == target) {
                list.erase(list.begin() + static_cast<long>(i));
                return;
            }
        }
    }

    Offset restrictAxis(const Offset& offset) const {
        if (!axis_)
            return offset;
        if (*axis_ == Axis::Horizontal)
            return Offset(offset.dx, 0.0);
        if (*axis_ == Axis::Vertical)
            return Offset(0.0, offset.dy);
        return offset;
    }

    /// 上游先遍历后 `clear()`；本仓的 didLeave 回调可能销毁目标自己（→ 反向
    /// 摘除 `enteredTargets_`），所以先取快照并当场清空。
    void leaveAllEntered() {
        auto snapshot = enteredTargets_;
        enteredTargets_.clear();
        for (auto* target : snapshot)
            target->didLeave(*this);
    }

    /// Overlay 自己的渲染盒子（用来把全局点换算成条目坐标系）。
    RenderBox* overlayBox() const {
        auto* ctx = overlayState_->context();
        auto* element = ctx ? ctx->element() : nullptr;
        return element ? dynamic_cast<RenderBox*>(element->findRenderObject()) : nullptr;
    }

    /// 沿 parent 链爬到根渲染对象。
    RenderObject* rootRenderObject() const {
        RenderObject* node = overlayBox();
        while (node && node->parent())
            node = static_cast<RenderObject*>(node->parent());
        return node;
    }

    /// 上游的 `_DragAvatar._build`：`Positioned(left/top) → IgnorePointer(feedback)`。
    ///
    /// 本仓多包一层 Stack：`RenderTheater::performLayout` 给每个 onstage 子节点
    /// 写死 `offset = (0,0)`（= 上游「条目本身不定位，内容由条目自己定」），
    /// 所以 Positioned 必须落在一个 Stack 里面才拿得到 left/top ——与
    /// PopupMenuButton 的菜单同一条路子。
    SharedPtr<Widget> buildFeedback() {
        auto pos = cui::make_shared<Positioned>(
            cui::make_shared<IgnorePointer>(ignoringFeedbackPointer_, feedback_));
        pos->hasLeft = true;
        pos->left = overlayOffset_.dx;
        pos->hasTop = true;
        pos->top = overlayOffset_.dy;
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(std::move(pos));
        return cui::make_shared<Stack>(std::move(kids));
    }

    std::any data_;
    std::optional<Axis> axis_;
    Offset position_{};
    Offset lastGlobalPosition_{};
    Offset dragStartPoint_{};
    SharedPtr<Widget> feedback_;
    Offset feedbackOffset_{};
    bool ignoringFeedbackPointer_ = true;
    DragUpdateCallback onDragUpdate_;
    DragEndCallback onDragEnd_;
    OverlayState* overlayState_ = nullptr;

    DragTargetStateBase* activeTarget_ = nullptr;
    std::vector<DragTargetStateBase*> enteredTargets_;
    Offset lastOffset_{};
    Offset overlayOffset_{};
    SharedPtr<OverlayEntry> entry_;
};

inline void DragTargetStateBase::dispose() {
    for (DragAvatar* avatar : drag_detail::liveAvatars())
        avatar->handleTargetDisposed(this);
    State::dispose();
}

/// `DragTarget<T>` 的状态。上游的 `_DragTargetState<T>`。
template <typename T>
class DragTargetState : public DragTargetStateBase {
public:
    bool matchesDataType(const std::any& data) const override {
        return !data.has_value() || data.type() == typeid(T);
    }

    bool didEnter(DragAvatar& avatar) override {
        auto* w = target();
        const bool accepted = willAccept(avatar, *w);
        setState([this, &avatar, accepted] {
            if (accepted)
                candidateAvatars_.push_back(&avatar);
            else
                rejectedAvatars_.push_back(&avatar);
        });
        return accepted;
    }

    void didLeave(DragAvatar& avatar) override {
        if (!mounted())
            return;
        setState([this, &avatar] {
            removeOne(candidateAvatars_, &avatar);
            removeOne(rejectedAvatars_, &avatar);
        });
        auto* w = target();
        if (w->onLeave())
            w->onLeave()(dataOf(avatar));
    }

    void didDrop(DragAvatar& avatar) override {
        if (!mounted())
            return;
        setState([this, &avatar] { removeOne(candidateAvatars_, &avatar); });
        auto* w = target();
        const std::optional<T> data = dataOf(avatar);
        if (!data.has_value())
            return;  // 上游：data 为 null 时不调 onAccept
        DragTargetDetails<T> details{*data, avatar.lastOffset()};
        if (w->onAcceptWithDetails())
            w->onAcceptWithDetails()(details);
    }

    void didMove(DragAvatar& avatar) override {
        if (!mounted())
            return;
        auto* w = target();
        const std::optional<T> data = dataOf(avatar);
        if (!data.has_value() || !w->onMove())
            return;
        w->onMove()(DragTargetDetails<T>{*data, avatar.lastOffset()});
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        auto* w = target();
        return cui::make_shared<MetaData>(
            static_cast<void*>(this), HitTestBehavior::Translucent,
            w->builder() ? w->builder()(context, candidates(), rejected()) : nullptr);
    }

private:
    const DragTarget<T>* target() const {
        return static_cast<const DragTarget<T>*>(widget());
    }

    static std::optional<T> dataOf(const DragAvatar& avatar) {
        const std::any& data = avatar.data();
        if (!data.has_value() || data.type() != typeid(T))
            return std::nullopt;
        return std::any_cast<T>(data);
    }

    /// 上游的 `_mapAvatarsToData<T>`。
    std::vector<std::optional<T>> candidates() const {
        std::vector<std::optional<T>> out;
        for (const DragAvatar* avatar : candidateAvatars_)
            out.push_back(dataOf(*avatar));
        return out;
    }

    /// 上游的 `_mapAvatarsToData<Object>`（rejectedData 是 `List<dynamic>`）。
    std::vector<std::any> rejected() const {
        std::vector<std::any> out;
        for (const DragAvatar* avatar : rejectedAvatars_)
            out.push_back(avatar->data());
        return out;
    }

    /// 上游 `didEnter` 里的三段判定（本仓只接 WithDetails 版）。
    static bool willAccept(const DragAvatar& avatar, const DragTarget<T>& w) {
        if (!w.onWillAcceptWithDetails())
            return true;  // 两个回调都没给 ⇒ 一律接受（上游同款）
        const std::optional<T> data = dataOf(avatar);
        if (!data.has_value())
            return false;
        return w.onWillAcceptWithDetails()(DragTargetDetails<T>{*data, avatar.lastOffset()});
    }
};

/// `Draggable` 的状态。上游的 `_DraggableState`。
class DraggableState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        auto* self = this;
        auto* w = draggable();
        const bool canDrag =
            !w->maxSimultaneousDrags() || activeCount_ < *w->maxSimultaneousDrags();
        const bool showChild = drags_.empty() || !w->childWhenDragging();
        Listener::PointerCallback onDown;
        if (canDrag)
            onDown = [self](const PointerEvent& e) { self->routePointerDown(e); };
        return cui::make_shared<Listener>(
            onDown, [self](const PointerEvent& e) { self->handleMove(e); },
            [self](const PointerEvent& e) { self->handleUp(e); }, nullptr, nullptr,
            showChild ? w->child() : w->childWhenDragging());
    }

    void dispose() override {
        // 铁律 3：源先死 ⇒ 直接丢弃在飞的 avatar（~DragAvatar 负责摘条目，
        // 不触发用户的 onDragEnd —— 那一刻 widget 已经没了）。
        if (delayTicker_) {
            delayTicker_->stop();
            delayTicker_.reset();
        }
        pending_.clear();
        drags_.clear();
        State::dispose();
    }

    /// 在飞拖拽数（测试与宿主用）。上游的 `_activeCount`。
    int activeCount() const { return activeCount_; }

private:
    Draggable* draggable() { return static_cast<Draggable*>(widget()); }

    struct TrackedDrag {
        int64_t pointerId = 0;
        SharedPtr<DragAvatar> avatar;
    };
    struct PendingDrag {
        int64_t pointerId = 0;
        Offset position{};
        std::chrono::steady_clock::time_point since{};
    };

    void routePointerDown(const PointerEvent& e) {
        auto* self = this;
        if (draggable()->dragDelay() > Duration(0)) {
            pending_.push_back({e.pointerId, e.globalPosition,
                                std::chrono::steady_clock::now()});
            if (!delayTicker_) {
                delayTicker_ = std::make_unique<Ticker>([self](Duration) {
                    self->advancePending();
                });
            }
            delayTicker_->start();
            return;
        }
        startDrag(e.globalPosition, e.pointerId);
    }

    /// 延时版（上游 DelayedMultiDragGestureRecognizer 的定时器）到点起拖。
    void advancePending() {
        if (pending_.empty()) {
            if (delayTicker_)
                delayTicker_->stop();
            return;
        }
        const Duration delay = draggable()->dragDelay();
        auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < pending_.size();) {
            if (now - pending_[i].since >= delay) {
                const int64_t pointerId = pending_[i].pointerId;
                const Offset position = pending_[i].position;
                pending_.erase(pending_.begin() + static_cast<long>(i));
                startDrag(position, pointerId);
            } else {
                ++i;
            }
        }
    }

    void startDrag(const Offset& globalPosition, int64_t pointerId) {
        auto* w = draggable();
        if (w->maxSimultaneousDrags() && activeCount_ >= *w->maxSimultaneousDrags())
            return;
        const Offset dragStartPoint =
            w->dragAnchorStrategy()
                ? w->dragAnchorStrategy()(*w, *context(), globalPosition)
                : childDragAnchorStrategy(*w, *context(), globalPosition);

        setState([this] { ++activeCount_; });

        auto* self = this;
        auto avatar = cui::make_shared<DragAvatar>(
            *OverlayState::of(*context()), w->data(), w->axis(), globalPosition,
            dragStartPoint, w->feedback(), w->feedbackOffset(),
            w->ignoringFeedbackPointer(),
            [self](const DragUpdateDetails& details) {
                // 不捕获 widget 指针：重建会换掉 Draggable 对象，回调每次从
                // State 现取。
                if (!self->mounted())
                    return;
                const auto& cb = self->draggable()->onDragUpdate();
                if (cb)
                    cb(details);
            },
            [self](const Velocity& velocity, const Offset& offset, bool wasAccepted) {
                dragEnd(self, velocity, offset, wasAccepted);
            });

        drags_.push_back({pointerId, avatar});
        if (w->onDragStarted())
            w->onDragStarted()();
    }

    /// 上游 `_DraggableState._startDrag` 里传给 avatar 的 onDragEnd 闭包。
    ///
    /// 上游的 `else { _activeCount -= 1; _disposeRecognizerIfInactive(); }`
    /// 分支（State 已 dispose 但拖拽还在飞）在本仓不存在：avatar 由 State 持有，
    /// State 一死 avatar 就没了（铁律 3），所以这里只需要在飞的那一种。
    static void dragEnd(DraggableState* self, const Velocity& velocity,
                        const Offset& offset, bool wasAccepted) {
        if (!self->mounted())
            return;
        self->setState([self] { --self->activeCount_; });
        auto* w = self->draggable();
        if (w->onDragEnd()) {
            DraggableDetails details;
            details.wasAccepted = wasAccepted;
            details.velocity = velocity;
            details.offset = offset;
            w->onDragEnd()(details);
        }
        if (wasAccepted && w->onDragCompleted())
            w->onDragCompleted()();
        if (!wasAccepted && w->onDraggableCanceled())
            w->onDraggableCanceled()(velocity, offset);
    }

    void handleMove(const PointerEvent& e) {
        for (auto& d : drags_) {
            if (d.pointerId == e.pointerId) {
                // 铁律 1 的另一半：上游靠 GestureArena 让内层的拖拽识别器赢掉
                // 外层滚动的竖拖；本仓没有竞技场，改由「已起拖的那个 pointer 不再
                // 向上冒泡」达成同一效果（否则拖动源的同时页面也跟着滚）。
                e.stopPropagation();
                d.avatar->update(e.globalPosition);
                return;
            }
        }
        for (auto& p : pending_) {
            if (p.pointerId == e.pointerId)
                p.position = e.globalPosition;
        }
    }

    void handleUp(const PointerEvent& e) {
        for (size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].pointerId == e.pointerId) {
                pending_.erase(pending_.begin() + static_cast<long>(i));
                break;
            }
        }
        if (pending_.empty() && delayTicker_)
            delayTicker_->stop();
        SharedPtr<DragAvatar> avatar;
        for (size_t i = 0; i < drags_.size(); ++i) {
            if (drags_[i].pointerId == e.pointerId) {
                avatar = drags_[i].avatar;
                drags_.erase(drags_.begin() + static_cast<long>(i));
                break;
            }
        }
        if (!avatar)
            return;
        // avatar 已被本地 SharedPtr 保活：先出表再结束，回调里的 setState
        // 不会再看到这条拖拽（上游在 recognizer 里同样是先解绑再 end）。
        avatar->end();
    }

    int activeCount_ = 0;
    std::vector<TrackedDrag> drags_;
    std::vector<PendingDrag> pending_;
    std::unique_ptr<Ticker> delayTicker_;
};

inline std::unique_ptr<State> Draggable::createState() {
    return std::make_unique<DraggableState>();
}

}  // namespace cui_core
