/// @file widgets/build_owner.cpp
/// @brief BuildOwner 实现 —— 重建调度与 inactive element 管理。

#include "cui/framework/build_owner.h"

#include <algorithm>

#include "cui/rendering/render_object.h"

namespace cui {

BuildOwner::~BuildOwner() {
    // 根 Element 递归卸载，释放 GlobalKey 挂载。
    // 拆除期间渲染树正在散架：祖先 RenderObject 可能已被释放，
    // markNeedsLayout 不能再沿父链向上走（否则野指针崩溃）。
    pushRenderTeardown();
    if (rootElement_)
        rootElement_->unmount();
    popRenderTeardown();
}

void BuildOwner::scheduleBuildFor(Element* element) {
    if (!element)
        return;
    if (std::find(dirtyElements_.begin(), dirtyElements_.end(), element) != dirtyElements_.end())
        return;
    // 上游：脏列表刚出现待办时调一次 onBuildScheduled（宿主据此请求一帧）。
    if (!scheduledFlushDirty_) {
        scheduledFlushDirty_ = true;
        if (onBuildScheduled)
            onBuildScheduled();
    }
    dirtyElements_.push_back(element);
}

void BuildOwner::forgetElement(Element* element) {
    if (!element)
        return;
    auto it = std::find(dirtyElements_.begin(), dirtyElements_.end(), element);
    if (it != dirtyElements_.end())
        dirtyElements_.erase(it);
    // 本次 flush 期间卸载的元素可能已被立即释放（本移植为同步 unmount，
    // 不同于 上游 推迟到 finalizeTree）。记录其指针值（不对其解引用），
    // 供 flushBuildScope 跳过，避免遍历 batch 快照时打到悬空指针。
    if (flushing_)
        droppedDuringFlush_.push_back(element);
}

void BuildOwner::buildScope(const std::function<void()>& callback) {
    ++buildScopeDepth_;
    if (callback)
        callback();
    --buildScopeDepth_;
    if (buildScopeDepth_ == 0)
        flushBuildScope();
}

void BuildOwner::flushBuildScope() {
    // 上游的 flushBuildScope：循环处理，直到没有新的 dirty element。
    // 重建过程中可能再次 markNeedsBuild（例如 setState 触发链）。
    int guard = 0;
    while (!dirtyElements_.empty()) {
        if (++guard > 1000) {
            dirtyElements_.clear();
            break;
        }
        flushing_ = true;
        auto batch = std::move(dirtyElements_);
        dirtyElements_.clear();
        for (Element* element : batch) {
            if (!element)
                continue;
            // 本次 flush 期间已被卸载（内存可能已释放）的 Element 跳过。
            // 只比较指针值、不对其解引用，杜绝 UAF。
            if (std::find(droppedDuringFlush_.begin(), droppedDuringFlush_.end(),
                          element) != droppedDuringFlush_.end())
                continue;
            element->rebuild();
        }
        flushing_ = false;
        droppedDuringFlush_.clear();
    }
    // 上游 buildScope 的 finally：本批冲刷结束，下一次 scheduleBuildFor
    // 重新走 onBuildScheduled 上升沿（冲刷期间新标脏的已在同批循环里处理）。
    scheduledFlushDirty_ = false;
}

void BuildOwner::deactivateChild(Element* element) {
    if (!element)
        return;
    element->deactivate();
    inactiveElements_.push_back(std::unique_ptr<Element>(element));
}

void BuildOwner::finalizeTree() {
    // 上游的 finalizeTree：帧末统一卸载 inactive elements。
    // 切页/弹栈时旧页面元素在这里卸载：此时渲染树正在拆散，
    // markNeedsLayout 不能沿父链向上走（祖先可能已释放）。
    auto inactive = std::move(inactiveElements_);
    inactiveElements_.clear();
    pushRenderTeardown();
    for (auto& element : inactive) {
        if (element)
            element->unmount();
    }
    popRenderTeardown();
}

void BuildOwner::adoptRootElement(std::unique_ptr<Element> element) {
    rootElement_ = std::move(element);
    if (rootElement_) {
        rootElement_->mount(nullptr, this);
        rootElement_->rebuild();
    }
}

void BuildOwner::addPostFrameCallback(FrameCallback callback) {
    if (callback)
        postFrameCallbacks_.push_back(std::move(callback));
}

void BuildOwner::flushPostFrameCallbacks(std::optional<Duration> timeStamp) {
    // 上游的 handleDrawFrame 收尾：先 List.of(_postFrameCallbacks) + clear，
    // 再逐个执行 ⇒ 回调里新加的 post-frame 回调落在**下一帧**。
    auto batch = std::move(postFrameCallbacks_);
    postFrameCallbacks_.clear();
    if (batch.empty())
        return;

    const auto now = std::chrono::steady_clock::now();
    if (!frameClockBase_.has_value())
        frameClockBase_ = now;
    const Duration stamp = timeStamp.value_or(
            std::chrono::duration_cast<Duration>(now - *frameClockBase_));

    flushingPostFrame_ = true;
    for (const auto& callback : batch)
        callback(stamp);
    flushingPostFrame_ = false;
}

}
