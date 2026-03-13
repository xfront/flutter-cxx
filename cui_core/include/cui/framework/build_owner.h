#pragma once
/// @file framework/build_owner.h
/// @brief 上游的 BuildOwner 移植。
///
/// 对应 上游源码：
///   上游 widgets/framework.dart（BuildOwner 段落）
///
/// BuildOwner 负责：
///   - 维护待重建的 Element 列表（dirty elements）
///   - buildScope：在一个受控范围内批量执行重建
///   - 管理 inactive elements（被替换但尚未销毁的子树），帧末 finalizeTree 释放
///   - 管理 GlobalKey 的挂载/卸载

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cui/foundation/ticker.h"
#include "focus.h"
#include "element.h"

namespace cui {

class RenderObject;

/// 重建调度中心。
class BuildOwner {
public:
    BuildOwner() = default;
    ~BuildOwner();

    /// 标记 Element 需要重建。上游的 `scheduleBuildFor()`。
    void scheduleBuildFor(Element* element);

    /// 上游 `BuildOwner.onBuildScheduled`：脏列表从「没有待办」进入「有待办」
    /// 时回调一次。宿主（Application）把它接到 `scheduleRender()`。
    ///
    /// 没有这条线时 `setState()` 只把 Element 放进脏列表、不请求帧——界面
    /// 要等下一个输入事件（点击 / 滚动）才刷新，异步进度条因此「不点不动」。
    std::function<void()> onBuildScheduled;

    /// 从待重建列表中摘除（Element 卸载时调用）。上游的
    /// `Element.unmount` 会执行 `owner._dirtyElements.remove(this)`：否则已
    /// 释放的 Element 仍以悬空指针残留在脏列表，下一帧 flushBuildScope
    /// 对其调用 mounted()/rebuild()→markNeedsBuild 时触发 UAF。
    void forgetElement(Element* element);

    /// 是否有待重建的 Element。
    bool hasScheduledBuilds() const { return !dirtyElements_.empty(); }

    /// 待重建 Element 数量（测试用）。
    size_t dirtyCount() const { return dirtyElements_.size(); }

    /// 在受控范围内执行重建。上游的 `buildScope()`。
    ///
    /// 范围内的重建被合并；范围结束后统一 flush。
    void buildScope(const std::function<void()>& callback);

    /// 执行所有待重建 Element 的重建。
    void flushBuildScope();

    /// 把 Element 停用并放入待销毁队列。上游的 `deactivateChild()`。
    void deactivateChild(Element* element);

    /// 帧末释放所有 inactive element。上游的 `finalizeTree()`。
    void finalizeTree();

    // ── 帧末回调（上游的 `SchedulerBinding.addPostFrameCallback`）─────────
    //
    // 本仓没有独立的 SchedulerBinding，帧由 BuildOwner + PipelineOwner 驱动，
    // 所以这条队列挂在 BuildOwner 上：任何 Element 都能经 `owner()` 拿到它。
    // 语义与上游 一致：本帧末尾统一执行，**执行前快照并清空**，
    // 因此回调里再 `addPostFrameCallback` 会留到下一帧。

    /// 上游的 `FrameCallback`：入参是本帧时间戳（本仓用帧步进 dt）。
    using FrameCallback = std::function<void(Duration)>;

    void addPostFrameCallback(FrameCallback callback);

    /// 执行并清空当前队列。帧驱动（`FrameScheduler::pump` /
    /// `Application::renderFrame`）在一帧的绘制之后调用它。
    ///
    /// `timeStamp` 对应上游的 `SchedulerBinding.currentFrameTimeStamp`；本仓没有
    /// 全局 binding，不给时由 BuildOwner 以自己首次冲刷的时刻为基准现算。
    void flushPostFrameCallbacks(std::optional<Duration> timeStamp = std::nullopt);

    /// 队列长度（测试用）。
    size_t postFrameCallbackCount() const { return postFrameCallbacks_.size(); }

    /// 上游的 `SchedulerPhase.postFrameCallbacks`：正在跑帧末回调。
    /// Navigator 的 `_handleHistoryChanged` 靠它决定「能否当场 dispatch 通知」——
    /// 上游 只在这个阶段当场派发，其余阶段一律推到下一帧末尾。
    bool inPostFrameCallbacks() const { return flushingPostFrame_; }

    /// 上游的 `SchedulerPhase.persistentCallbacks`（build 阶段）：
    /// 正在冲刷脏 Element。此期间不得改树/派发通知。
    bool inBuildPhase() const { return flushing_; }

    /// 当前根 Element。
    Element* rootElement() const { return rootElement_.get(); }

    /// 设置根 Element（会接管所有权）。
    void adoptRootElement(std::unique_ptr<Element> element);

    /// 根级 RenderObject 的宿主（一般是 RenderView）。
    ///
    /// 上游不存在「根渲染对象换类型」的问题——`RenderView` 本身就是元素树的
    /// 根 RenderObject。本仓的 RenderView 由宿主手动创建，根控件一旦更换渲染
    /// 类型（如面板最大化时整棵子树的根从 Column 变成裸内容），旧根卸载、新
    /// 根挂载就必须重新接回宿主，否则 `RenderView::child_` 指向已释放的对象。
    /// 声明后由 `RenderObjectElement` 的挂载/卸载自动维护，无需宿主自己 setChild。
    void setRootRenderObjectHost(RenderObject* host) { rootHost_ = host; }
    RenderObject* rootRenderObjectHost() const { return rootHost_; }

    /// 焦点管理器（键盘焦点模型）。每个 BuildOwner 持有一个。
    FocusManager& focusManager() { return focusManager_; }

private:
    std::vector<Element*> dirtyElements_;
    /// 上游 `_scheduledFlushDirtyElements`：脏列表已挂上待办（onBuildScheduled
    /// 只在该上升沿触发一次），flushBuildScope 收尾时复位。
    bool scheduledFlushDirty_ = false;
    std::vector<std::unique_ptr<Element>> inactiveElements_;
    // 本次 flush 期间被卸载（可能已释放）的 Element 指针，仅用于指针值比较，
    // 不对其解引用。见 forgetElement / flushBuildScope。
    std::vector<Element*> droppedDuringFlush_;
    std::unique_ptr<Element> rootElement_;
    RenderObject* rootHost_ = nullptr;
    int buildScopeDepth_ = 0;
    bool dirtyDuringBuild_ = false;
    bool flushing_ = false;
    FocusManager focusManager_;

    std::vector<FrameCallback> postFrameCallbacks_;
    bool flushingPostFrame_ = false;
    /// `flushPostFrameCallbacks()` 未给时间戳时的基准（首次冲刷那刻）。
    std::optional<std::chrono::steady_clock::time_point> frameClockBase_;
};

}
