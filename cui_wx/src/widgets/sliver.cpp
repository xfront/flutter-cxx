/// @file widgets/sliver.cpp
/// @brief Sliver 懒加载控件层：SliverMultiBoxAdaptorElement（子节点托管人）。
///
/// 对应 上游源码：
///   上游 widgets/sliver.dart（SliverMultiBoxAdaptorElement）
///   上游 widgets/scroll_delegate.dart（delegate 契约的调用侧）
///
/// 逐段移植 上游 performRebuild / createChild / removeChild / childCount /
/// estimateMaxScrollOffset；两处结构性偏离：
///   1. 上游 用 `slot`（= 子序号）驱动 attachRenderObject，搬动子节点时经
///      `moveRenderObjectChild` 复位；本仓 Element 没有 slot，搬动来的子节点
///      在 processElement 里显式 `insertChild(RO, currentBeforeChild)` 复位。
///   2. 上游 的 `_childElements` 是 SplayTreeMap，槽位值可空（null = 占位）；
///      本仓用「槽位表 slots + 新序号 → 槽位」两阶段表达同一语义，避免在
///      迭代中改 map 的键。

#include "cui/widgets/sliver.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {

namespace {

/// 上游的 `_extrapolateMaxScrollOffset`：按已物化子节点的均值外推总长。
double extrapolateMaxScrollOffset(int firstIndex, int lastIndex, double leadingScrollOffset,
                                  double trailingScrollOffset, int childCount) {
    if (lastIndex == childCount - 1)
        return trailingScrollOffset;
    const int reifiedCount = lastIndex - firstIndex + 1;
    const double averageExtent = (trailingScrollOffset - leadingScrollOffset) / reifiedCount;
    const int remainingCount = childCount - lastIndex - 1;
    return trailingScrollOffset + averageExtent * remainingCount;
}

}  // namespace

// ── SliverMultiBoxAdaptorWidget ─────────────────────────────────────────────

std::unique_ptr<Element> SliverMultiBoxAdaptorWidget::createElement(SharedPtr<Widget> self) {
    return std::make_unique<SliverMultiBoxAdaptorElement>(std::move(self));
}

// ── SliverMultiBoxAdaptorElement ────────────────────────────────────────────

SliverMultiBoxAdaptorElement::SliverMultiBoxAdaptorElement(SharedPtr<Widget> widget)
    : RenderObjectElement(std::move(widget)) {}

RenderSliverMultiBoxAdaptor* SliverMultiBoxAdaptorElement::adaptor() const {
    return dynamic_cast<RenderSliverMultiBoxAdaptor*>(renderObject());
}

SliverMultiBoxAdaptorWidget* SliverMultiBoxAdaptorElement::adaptorWidget() const {
    return dynamic_cast<SliverMultiBoxAdaptorWidget*>(widget_.get());
}

std::unique_ptr<RenderObject> SliverMultiBoxAdaptorElement::createRenderObject() {
    auto ro = RenderObjectElement::createRenderObject();
    if (auto* a = dynamic_cast<RenderSliverMultiBoxAdaptor*>(ro.get()))
        a->setChildManager(this);
    return ro;
}

SliverMultiBoxAdaptorParentData* SliverMultiBoxAdaptorElement::parentDataOf(Element* element) {
    RenderObject* ro = element ? element->findRenderObject() : nullptr;
    return ro && ro->parentData() ? dynamic_cast<SliverMultiBoxAdaptorParentData*>(ro->parentData())
                                  : nullptr;
}

SliverMultiBoxAdaptorParentData* SliverMultiBoxAdaptorElement::parentDataOfBox(RenderBox* box) {
    return box && box->parentData()
               ? dynamic_cast<SliverMultiBoxAdaptorParentData*>(box->parentData())
               : nullptr;
}

RenderBox* SliverMultiBoxAdaptorElement::boxOfElement(Element* element) {
    RenderObject* ro = element ? element->findRenderObject() : nullptr;
    return ro ? dynamic_cast<RenderBox*>(ro) : nullptr;
}

SharedPtr<Widget> SliverMultiBoxAdaptorElement::buildAt(int index) {
    SliverMultiBoxAdaptorWidget* widget = adaptorWidget();
    SliverChildDelegate* delegate = widget ? widget->delegate() : nullptr;
    return delegate ? delegate->build(*this, index) : nullptr;
}

std::unique_ptr<Element> SliverMultiBoxAdaptorElement::updateChildPreserving(
    std::unique_ptr<Element> child, const SharedPtr<Widget>& newWidget) {
    // 上游覆写的 updateChild：子节点换了 RenderObject（Widget 类型或 Key 变了）
    // 时，把旧 parentData 上的布局偏移搬过去——否则懒建出来的子节点会被当作
    // 「还没有布局偏移」，触发一轮无谓的前导抓取。
    SliverMultiBoxAdaptorParentData* oldParentData = parentDataOf(child.get());
    std::optional<double> oldLayoutOffset =
        oldParentData ? oldParentData->layoutOffset : std::optional<double>();
    std::unique_ptr<Element> newChild = Element::updateChild(std::move(child), newWidget);
    SliverMultiBoxAdaptorParentData* newParentData = parentDataOf(newChild.get());
    if (newParentData && newParentData != oldParentData)
        newParentData->layoutOffset = oldLayoutOffset;
    return newChild;
}

void SliverMultiBoxAdaptorElement::update(SharedPtr<Widget> newWidget) {
    // 上游的 `final oldWidget = widget;`：本仓 delegate 由旧 widget 用
    // shared_ptr 持有，super.update 换掉 widget_ 后旧 widget 可能当场销毁，
    // 故先把它的引用抓住，后面的 oldDelegate 才是活对象。
    SharedPtr<Widget> previous = widget_;
    auto* previousAdaptor =
        previous ? dynamic_cast<SliverMultiBoxAdaptorWidget*>(previous.get()) : nullptr;
    SliverChildDelegate* oldDelegate = previousAdaptor ? previousAdaptor->delegate() : nullptr;

    // 上游：super.update(newWidget) 里 _performRebuild 已经按旧序号 reconcile
    // 一轮；delegate 换内容时再整体重建一次。
    RenderObjectElement::update(std::move(newWidget));

    SliverMultiBoxAdaptorWidget* next = adaptorWidget();
    SliverChildDelegate* newDelegate = next ? next->delegate() : nullptr;

    // 上游：newDelegate != oldDelegate &&（runtimeType 不同 || shouldRebuild）。
    if (newDelegate && newDelegate != oldDelegate &&
        (typeid(*newDelegate) != typeid(*oldDelegate) || newDelegate->shouldRebuild(*oldDelegate))) {
        performRebuild();
        // 上游紧接着 renderObject.markNeedsLayout()：换 delegate 意味着子项
        // 集合变了，即便本轮 reconcile 没碰到渲染树也要重排。
        if (RenderObject* ro = findRenderObject())
            ro->markNeedsLayout();
    }
}

void SliverMultiBoxAdaptorElement::performRebuild() {
    RenderObjectElement::performRebuild();

    SliverMultiBoxAdaptorWidget* boxWidget = adaptorWidget();
    SliverChildDelegate* delegate = boxWidget ? boxWidget->delegate() : nullptr;
    currentBeforeChild_ = nullptr;
    if (!delegate || !adaptor())
        return;

    const bool replaceMovedChildren = boxWidget->replaceMovedChildren();
    bool childrenUpdated = false;

    struct Slot {
        int originalIndex = -1;
        /// 上游的「该子节点从 _childElements 里搬走了」：源序号随即视为空槽。
        bool movedAway = false;
        std::unique_ptr<Element> element;
    };
    std::vector<Slot> slots;
    std::map<int, size_t> byOriginal;                 ///< 旧序号 → 槽
    std::map<int, std::optional<size_t>> planned;     ///< 新序号 → 槽（空 = 占位）
    std::map<int, double> indexToLayoutOffset;        ///< 上游同名

    // ── ① 扫描存量子节点：记下布局偏移，按 Key 找出新序号 ─────────────────
    for (auto& entry : childElements_) {
        if (!entry.second)
            continue;
        const int index = entry.first;
        Element* element = entry.second.get();
        const KeyPtr key = element->widget() ? element->widget()->key() : nullptr;
        const std::optional<int> newIndex = key ? delegate->findIndexByKey(key) : std::nullopt;

        if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(element); pd && pd->layoutOffset)
            indexToLayoutOffset[index] = *pd->layoutOffset;

        slots.push_back(Slot{index, false, std::move(entry.second)});
        const size_t slot = slots.size() - 1;
        byOriginal[index] = slot;

        if (newIndex && *newIndex != index) {
            // 上游：搬动后该子节点的布局偏移不再准确。
            slots[slot].movedAway = true;
            if (SliverMultiBoxAdaptorParentData* pd = parentDataOf(slots[slot].element.get()); pd)
                pd->layoutOffset = std::nullopt;
            planned[*newIndex] = slot;  ///< 上游是直接赋值，可挤掉同序号的原主
            if (replaceMovedChildren)
                planned.emplace(index, std::nullopt);  ///< putIfAbsent(index, () => null)
        } else {
            planned.emplace(index, slot);  ///< putIfAbsent
        }
    }
    childElements_.clear();

    // ── ② 逐序号 reconcile（上游 的 processElement）───────────────────────
    auto processElement = [&](int index) {
        // 旧表在本序号上、既没被本序号采用又没搬走的子节点：停用卸载。
        Element* previousAtThisIndex = nullptr;
        auto original = byOriginal.find(index);
        if (original != byOriginal.end()) {
            Slot& occupant = slots[original->second];
            previousAtThisIndex = occupant.element.get();
            auto plan = planned.find(index);
            const bool takenBySelf =
                plan != planned.end() && plan->second && *plan->second == original->second;
            if (!occupant.movedAway && occupant.element && !takenBySelf) {
                currentlyUpdatingChildIndex_ = index;
                occupant.element = updateChildPreserving(std::move(occupant.element), nullptr);
                currentlyUpdatingChildIndex_ = std::nullopt;
                childrenUpdated = true;
                previousAtThisIndex = nullptr;
            }
        }

        std::optional<size_t> incomingSlot;
        auto plan = planned.find(index);
        if (plan != planned.end() && plan->second)
            incomingSlot = plan->second;

        std::unique_ptr<Element> incoming;
        const bool cameFromMove = incomingSlot && slots[*incomingSlot].movedAway;
        if (incomingSlot)
            incoming = std::move(slots[*incomingSlot].element);

        currentlyUpdatingChildIndex_ = index;
        std::unique_ptr<Element> newChild =
            updateChildPreserving(std::move(incoming), buildAt(index));
        // 搬来的子节点：本仓 Element 无 slot，显式按新位置重插（上游经
        // attachRenderObject(newSlot) → moveRenderObjectChild 完成同一步）。
        RenderBox* box = newChild ? boxOfElement(newChild.get()) : nullptr;
        if (cameFromMove && box)
            adaptor()->insertChild(static_cast<RenderObject*>(box), currentBeforeChild_);
        currentlyUpdatingChildIndex_ = std::nullopt;

        if (!newChild) {
            childrenUpdated = true;
            return;
        }
        childrenUpdated = childrenUpdated || (newChild.get() != previousAtThisIndex);
        childElements_[index] = std::move(newChild);
        if (SliverMultiBoxAdaptorParentData* pd = parentDataOfBox(box); pd) {
            if (index == 0)
                pd->layoutOffset = 0.0;
            else if (auto cached = indexToLayoutOffset.find(index);
                     cached != indexToLayoutOffset.end())
                pd->layoutOffset = cached->second;
        }
        // 上游：keepAlive 子节点不推进 _currentBeforeChild（本仓桶未移植）。
        if (box)
            currentBeforeChild_ = box;
    };

    for (const auto& entry : planned)
        processElement(entry.first);

    // ── ③ 尾部 lookahead ──────────────────────────────────────────────────
    // 上游注释：本轮只更新了存量子节点，若更新前就停在末尾，多看一个子节点，
    // 好让布局阶段有机会刷新 max scroll offset；否则布局可能被跳过，滚动视图
    // 卡在旧的末尾。任一既有子节点变化过都不需要这一步。
    if (!childrenUpdated && didUnderflow_) {
        const int lastKey = childElements_.empty() ? -1 : childElements_.rbegin()->first;
        const int rightBoundary = lastKey + 1;
        // 上游：newChildren[rightBoundary] = _childElements[rightBoundary]
        // —— 此刻 rightBoundary 必然还没被采用（否则它就在 lastKey 之前）。
        auto original = byOriginal.find(rightBoundary);
        if (original != byOriginal.end() && !slots[original->second].movedAway &&
            slots[original->second].element)
            planned[rightBoundary] = original->second;
        processElement(rightBoundary);
    }

    // ── ④ 清扫 ────────────────────────────────────────────────────────────
    // 只有两个子节点被 findIndexByKey 判给同一序号时才会剩（上游 的
    // newChildren 同键覆盖会让被挤掉的那个 Element 脱离所有表）：本仓若不管，
    // 它持有的 RenderObject 还挂在渲染树上却没人负责卸载。
    for (Slot& slot : slots) {
        if (!slot.element)
            continue;
        currentlyUpdatingChildIndex_ = slot.originalIndex;
        slot.element = updateChildPreserving(std::move(slot.element), nullptr);
        currentlyUpdatingChildIndex_ = std::nullopt;
    }
}

// ── RenderSliverBoxChildManager ─────────────────────────────────────────────

void SliverMultiBoxAdaptorElement::createChild(int index, RenderBox* after) {
    if (!adaptor())
        return;
    // 上游用 owner.buildScope 包住：期间被标脏的 Element 一并 flush。
    owner()->buildScope([this, index, after] {
        const bool insertFirst = after == nullptr;
        RenderBox* before = nullptr;
        if (!insertFirst) {
            auto prev = childElements_.find(index - 1);
            if (prev != childElements_.end() && prev->second)
                before = boxOfElement(prev->second.get());
        }
        currentBeforeChild_ = before;

        std::unique_ptr<Element> existing;
        auto slot = childElements_.find(index);
        if (slot != childElements_.end())
            existing = std::move(slot->second);

        currentlyUpdatingChildIndex_ = index;
        std::unique_ptr<Element> newChild =
            updateChildPreserving(std::move(existing), buildAt(index));
        currentlyUpdatingChildIndex_ = std::nullopt;

        if (newChild)
            childElements_[index] = std::move(newChild);
        else
            childElements_.erase(index);
    });
}

void SliverMultiBoxAdaptorElement::removeChild(RenderBox* child) {
    if (!adaptor())
        return;
    const int index = adaptor()->indexOf(child);
    owner()->buildScope([this, index] {
        auto slot = childElements_.find(index);
        if (slot == childElements_.end())
            return;
        currentlyUpdatingChildIndex_ = index;
        slot->second = updateChildPreserving(std::move(slot->second), nullptr);
        currentlyUpdatingChildIndex_ = std::nullopt;
        childElements_.erase(index);
    });
}

double SliverMultiBoxAdaptorElement::estimateMaxScrollOffset(
    const SliverConstraints& /*constraints*/, int firstIndex, int lastIndex,
    double leadingScrollOffset, double trailingScrollOffset) {
    SliverMultiBoxAdaptorWidget* boxWidget = adaptorWidget();
    SliverChildDelegate* delegate = boxWidget ? boxWidget->delegate() : nullptr;
    if (!delegate)
        return std::numeric_limits<double>::infinity();

    // 上游：estimatedChildCount 为空（无限列表）时无从估算，直接给无穷。
    const std::optional<int> count = delegate->estimatedChildCount();
    if (!count)
        return std::numeric_limits<double>::infinity();

    const std::optional<double> estimate = boxWidget->estimateMaxScrollOffset(
        firstIndex, lastIndex, leadingScrollOffset, trailingScrollOffset);
    return estimate.value_or(extrapolateMaxScrollOffset(firstIndex, lastIndex, leadingScrollOffset,
                                                        trailingScrollOffset, *count));
}

int SliverMultiBoxAdaptorElement::childCount() {
    SliverMultiBoxAdaptorWidget* boxWidget = adaptorWidget();
    SliverChildDelegate* delegate = boxWidget ? boxWidget->delegate() : nullptr;
    std::optional<int> result = delegate ? delegate->estimatedChildCount() : std::nullopt;
    if (result)
        return *result;

    // 上游：走到这里说明 builder 已经对某个正索引返回过空 —— 列表是有限的，
    // 只是 delegate 没报数。用开放式二分找出末尾。
    int64_t lo = 0;
    int64_t hi = 1;
    const int64_t kMax = (int64_t{1} << 62) - 1;
    while (buildAt(static_cast<int>(hi - 1))) {
        lo = hi - 1;
        if (hi < kMax / 2)
            hi *= 2;
        else if (hi < kMax)
            hi = kMax;
        else
            return static_cast<int>(lo);  // 找不到末尾：停在已知最大有限值
    }
    while (hi - lo > 1) {
        const int64_t mid = (hi - lo) / 2 + lo;
        if (buildAt(static_cast<int>(mid - 1)))
            lo = mid;
        else
            hi = mid;
    }
    return static_cast<int>(lo);
}

void SliverMultiBoxAdaptorElement::didAdoptChild(RenderBox* child) {
    // 上游：childParentData.index = _currentlyUpdatingChildIndex。
    if (!currentlyUpdatingChildIndex_)
        return;
    if (SliverMultiBoxAdaptorParentData* pd = parentDataOfBox(child))
        pd->index = currentlyUpdatingChildIndex_;
}

void SliverMultiBoxAdaptorElement::didFinishLayout() {
    SliverMultiBoxAdaptorWidget* boxWidget = adaptorWidget();
    SliverChildDelegate* delegate = boxWidget ? boxWidget->delegate() : nullptr;
    if (!delegate)
        return;
    const int firstIndex = childElements_.empty() ? 0 : childElements_.begin()->first;
    const int lastIndex = childElements_.empty() ? 0 : childElements_.rbegin()->first;
    delegate->didFinishLayout(firstIndex, lastIndex);
}

// ── Element 侧的挂载 / 卸载 / 遍历 ──────────────────────────────────────────

void SliverMultiBoxAdaptorElement::insertRenderObjectChild(RenderObject* child, Element* owner) {
    (void)owner;
    if (!adaptor() || !child)
        return;
    adaptor()->insertChild(child, currentBeforeChild_);
}

void SliverMultiBoxAdaptorElement::removeRenderObjectChild(RenderObject* child) {
    if (!adaptor() || !child)
        return;
    adaptor()->removeChild(child);
}

void SliverMultiBoxAdaptorElement::visitChildren(const std::function<void(Element*)>& visitor) {
    for (auto& entry : childElements_)
        if (entry.second)
            visitor(entry.second.get());
}

void SliverMultiBoxAdaptorElement::unmount() {
    // 子节点先卸：它们的 didUnmount 要经本 Element 把 RenderObject 摘掉。
    for (auto& entry : childElements_)
        if (entry.second)
            entry.second->unmount();
    childElements_.clear();
    RenderObjectElement::unmount();
}

void SliverMultiBoxAdaptorElement::deactivate() {
    for (auto& entry : childElements_)
        if (entry.second)
            entry.second->deactivate();
    Element::deactivate();
}

void SliverMultiBoxAdaptorElement::activate() {
    for (auto& entry : childElements_)
        if (entry.second)
            entry.second->activate();
    Element::activate();
}

}  // namespace cui_core
