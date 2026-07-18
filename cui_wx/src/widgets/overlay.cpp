/// @file widgets/overlay.cpp
/// @brief Overlay / OverlayState / OverlayEntry 实现。
///
/// 语义对齐 上游官方源码：
///   上游 widgets/overlay.dart

#include "cui/widgets/overlay.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>

#include "../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/framework/element.h"
#include "cui/rendering/render_theater.h"

namespace cui {

namespace {

std::atomic<uint64_t> g_nextOverlayEntryId{1};

/// Overlay 的渲染树节点。上游的 `_Theater`（overlay.dart:984-1026）。
///
/// `_Theater` 是 `MultiChildRenderObjectWidget`，带一个 `skipCount`：
/// 列表前 `skipCount` 个子节点是 offstage 的（被上方 opaque 条目遮挡），
/// 不参与布局 / 绘制 / 命中测试。
class Theater : public MultiChildRenderObjectWidget {
public:
    Theater(size_t skipCount, std::vector<SharedPtr<Widget>> children)
        : MultiChildRenderObjectWidget(nullptr, std::move(children)), skipCount_(skipCount) {}

    size_t skipCount() const { return skipCount_; }
    void setSkipCount(size_t count) { skipCount_ = count; }

    RenderObject* createRenderObject() override { return new RenderTheater(skipCount_); }

    void updateRenderObject(RenderObject* ro) override {
        if (auto* t = dynamic_cast<RenderTheater*>(ro))
            t->setSkipCount(skipCount_);
    }

private:
    size_t skipCount_ = 0;
};

/// 单个 OverlayEntry 的宿主 Widget。上游的 `_OverlayEntryWidget`
/// （overlay.dart:298-437）。
///
/// 它是 StatefulWidget：entry 的 builder 在这里被调用；entry 的
/// `markNeedsBuild()` 通过它的 Element 触发重建，从而重新执行 builder。
class OverlayEntryWidget : public StatefulWidget {
public:
    OverlayEntryWidget(SharedPtr<OverlayEntry> entry, KeyPtr key)
        : StatefulWidget(std::move(key)), entry_(std::move(entry)) {}

    const SharedPtr<OverlayEntry>& entry() const { return entry_; }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<OverlayEntry> entry_;
};

/// OverlayEntryWidget 的 State。上游的 `_OverlayEntryWidgetState`。
class OverlayEntryWidgetState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override;

    void initState() override {
        State::initState();
        // 上游 在 initState 里把自己登记到 entry 上（overlay.dart:390-396），
        // 使 entry.markNeedsBuild() 能定位到本 Element。
        auto* w = dynamic_cast<OverlayEntryWidget*>(widget());
        if (w && w->entry())
            w->entry()->setHostElement(element());
    }

    void dispose() override {
        // 上游的 _OverlayEntryWidgetState 在卸载时清掉与条目的关联
        // （overlay.dart:392-396），使 entry.markNeedsBuild() 不再指向
        // 已失效的 Element。
        auto* w = dynamic_cast<OverlayEntryWidget*>(widget());
        if (w && w->entry())
            w->entry()->setHostElement(nullptr);
        State::dispose();
    }
};

std::unique_ptr<State> OverlayEntryWidget::createState() {
    return std::make_unique<OverlayEntryWidgetState>();
}

SharedPtr<Widget> OverlayEntryWidgetState::build(BuildContext& context) {
    auto* w = dynamic_cast<OverlayEntryWidget*>(widget());
    if (!w || !w->entry())
        return nullptr;
    // 上游的 _OverlayEntryWidgetState.build（overlay.dart:419-430）：
    // 直接返回 entry.builder(context) 的结果。
    return w->entry()->builder()(context);
}

}  // namespace

// ── OverlayEntry ────────────────────────────────────────────────────────────

OverlayEntry::OverlayEntry(OverlayEntryBuilder builder, bool opaque, bool maintainState)
    : builder_(std::move(builder)), opaque_(opaque), maintainState_(maintainState),
      id_(g_nextOverlayEntryId.fetch_add(1)) {}

void OverlayEntry::setOpaque(bool opaque) {
    if (opaque_ == opaque)
        return;
    opaque_ = opaque;
    notifyOwnerChanged();
}

void OverlayEntry::setMaintainState(bool maintainState) {
    if (maintainState_ == maintainState)
        return;
    maintainState_ = maintainState;
    notifyOwnerChanged();
}

void OverlayEntry::setOverlay(OverlayState* overlay) {
    overlay_ = overlay;
}

void OverlayEntry::setHostElement(Element* element) {
    hostElement_ = element;
}

void OverlayEntry::notifyOwnerChanged() {
    // 上游的 _didChangeEntryOpacity（overlay.dart:880-886）：
    // 条目属性变化 → OverlayState 重新构建。
    if (overlay_)
        overlay_->markDirty();
}

void OverlayEntry::remove() {
    // 上游的 OverlayEntry.remove（overlay.dart:227-247）：
    // 从所属 Overlay 的条目列表中摘除自身，然后标脏等待重建。
    if (!overlay_)
        return;
    OverlayState* overlay = overlay_;
    overlay_ = nullptr;
    overlay->removeEntry(shared_from_this());
}

void OverlayEntry::markNeedsBuild() {
    // 上游的 OverlayEntry.markNeedsBuild（overlay.dart:249-252）：
    // 定位到条目对应的 Element 并请求重建，使 builder 重新执行。
    if (hostElement_)
        hostElement_->markNeedsBuild();
}

// ── OverlayState ────────────────────────────────────────────────────────────

void OverlayState::initState() {
    State::initState();
    // 上游的 OverlayState.initState（overlay.dart:655-657）：
    // 初始条目在 initState 时插入。
    auto* overlay = dynamic_cast<Overlay*>(widget());
    if (!overlay)
        return;
    const auto& initial = overlay->initialEntries();
    for (const auto& entry : initial) {
        if (!entry)
            continue;
        entry->setOverlay(this);
        entries_.push_back(entry);
    }
}

size_t OverlayState::insertionIndex(const SharedPtr<OverlayEntry>& below,
                                    const SharedPtr<OverlayEntry>& above) const {
    // 上游的 _insertionIndex（overlay.dart:665-677）。
    if (below) {
        auto it = std::find(entries_.begin(), entries_.end(), below);
        return it == entries_.end() ? entries_.size()
                                    : static_cast<size_t>(std::distance(entries_.begin(), it));
    }
    if (above) {
        auto it = std::find(entries_.begin(), entries_.end(), above);
        return it == entries_.end() ? entries_.size()
                                    : static_cast<size_t>(std::distance(entries_.begin(), it)) + 1;
    }
    return entries_.size();
}

void OverlayState::insert(const SharedPtr<OverlayEntry>& entry,
                          const SharedPtr<OverlayEntry>& below,
                          const SharedPtr<OverlayEntry>& above) {
    if (!entry)
        return;
    if (entry->overlay() && entry->overlay() != this)
        throw std::logic_error("The specified OverlayEntry is already present in another Overlay.");
    entry->setOverlay(this);
    size_t index = insertionIndex(below, above);
    entries_.insert(entries_.begin() + static_cast<long>(index), entry);
    markDirty();
}

void OverlayState::insertAll(const std::vector<SharedPtr<OverlayEntry>>& newEntries,
                             const SharedPtr<OverlayEntry>& below,
                             const SharedPtr<OverlayEntry>& above) {
    if (newEntries.empty())
        return;
    for (const auto& entry : newEntries) {
        if (!entry)
            continue;
        if (entry->overlay() && entry->overlay() != this)
            throw std::logic_error(
                    "The specified OverlayEntry is already present in another Overlay.");
        entry->setOverlay(this);
    }
    size_t index = insertionIndex(below, above);
    entries_.insert(entries_.begin() + static_cast<long>(index), newEntries.begin(),
                    newEntries.end());
    markDirty();
}

void OverlayState::rearrange(const std::vector<SharedPtr<OverlayEntry>>& newEntries,
                             const SharedPtr<OverlayEntry>& below,
                             const SharedPtr<OverlayEntry>& above) {
    // 上游的 OverlayState.rearrange（overlay.dart:814-847）：
    //   - newEntries 为空 → 直接返回；
    //   - 与当前列表相同 → 直接返回；
    //   - 新条目接管 overlay 归属；
    //   - 最终列表 = newEntries + 「原列表中不在 newEntries 里」的旧条目，
    //     旧条目被插入到 below/above 指定的位置。
    if (newEntries.empty())
        return;

    // 已在其它 Overlay 的条目 → 报错（对齐 上游的断言）。
    for (const auto& entry : newEntries) {
        if (!entry)
            continue;
        if (entry->overlay() && entry->overlay() != this)
            throw std::logic_error(
                    "One or more of the specified entries are already present in another Overlay.");
    }

    // 与当前列表相同则无需变更（对齐 上游的 listEquals 判断）。
    if (entries_.size() == newEntries.size()) {
        bool same = true;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i] != newEntries[i]) {
                same = false;
                break;
            }
        }
        if (same)
            return;
    }

    // 旧条目中「不在 newEntries 里」的部分，稍后被插回。
    std::vector<SharedPtr<OverlayEntry>> old;
    for (const auto& entry : entries_) {
        if (std::find(newEntries.begin(), newEntries.end(), entry) == newEntries.end())
            old.push_back(entry);
    }

    for (const auto& entry : newEntries) {
        if (entry)
            entry->setOverlay(this);
    }

    entries_ = newEntries;
    if (!old.empty()) {
        size_t index = insertionIndex(below, above);
        entries_.insert(entries_.begin() + static_cast<long>(index), old.begin(), old.end());
    }
    markDirty();
}

void OverlayState::removeEntry(const SharedPtr<OverlayEntry>& entry) {
    auto it = std::find(entries_.begin(), entries_.end(), entry);
    if (it == entries_.end())
        return;
    entries_.erase(it);
    markDirty();
}

void OverlayState::markDirty() {
    // 上游的 _markDirty（overlay.dart:849-853）：setState 触发重建。
    setState();
}

SharedPtr<Widget> OverlayState::build(BuildContext& context) {
    // ── 上游的 OverlayState.build（overlay.dart:888-919）──────────────
    // 从栈顶向下遍历条目：
    //   - 仍在 onstage 区域 → 加入 children；
    //   - 遇到 opaque 条目 → 其下条目转入 offstage；
    //   - offstage 且 maintainState → 仍加入 children（保持 State）；
    //   - offstage 且 !maintainState → 不构建。
    // 最后把 children 列表反转，得到「从底到顶」顺序；
    // skipCount = children.length - onstageCount，即位于底部的 offstage 数量。
    std::vector<SharedPtr<Widget>> children;
    bool onstage = true;
    size_t onstageCount = 0;

    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        const SharedPtr<OverlayEntry>& entry = *it;
        if (!entry)
            continue;
        if (onstage) {
            ++onstageCount;
            children.push_back(cui::make_shared<OverlayEntryWidget>(
                    entry, cui::make_shared<ValueKey<uint64_t>>(entry->id())));
            if (entry->opaque())
                onstage = false;
        } else if (entry->maintainState()) {
            children.push_back(cui::make_shared<OverlayEntryWidget>(
                    entry, cui::make_shared<ValueKey<uint64_t>>(entry->id())));
        }
    }

    std::reverse(children.begin(), children.end());

    size_t skipCount = children.size() >= onstageCount ? children.size() - onstageCount : 0;
    return cui::make_shared<Theater>(skipCount, std::move(children));
}

OverlayState* OverlayState::maybeOf(BuildContext& context) {
    // 上游的 Overlay.maybeOf（overlay.dart:635-641）通过
    // _RenderTheaterMarker 定位；本移植沿用「向上查找祖先 State」的通用机制，
    // 语义等价（OverlayState 是 Overlay 的 State）。
    return context.findAncestorStateOfType<OverlayState>();
}

OverlayState* OverlayState::of(BuildContext& context) {
    OverlayState* result = maybeOf(context);
    if (!result)
        throw std::logic_error("No Overlay widget found in the widget tree.");
    return result;
}

// ── Overlay ─────────────────────────────────────────────────────────────────

std::unique_ptr<State> Overlay::createState() { return std::make_unique<OverlayState>(); }

}
