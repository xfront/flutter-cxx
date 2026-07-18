#pragma once
/// @file widgets/overlay.h
/// @brief 上游的 Overlay 移植。
///
/// 对应 上游源码：
///   上游 widgets/overlay.dart
///
/// Overlay 是一叠「可独立管理的条目」：每个条目由一个 `OverlayEntry`
/// 描述，条目通过 `OverlayState::insert` / `insertAll` / `rearrange`
/// 插入或重排，通过 `OverlayEntry::remove` 移除。
///
/// 上游的关键语义（全部以官方源码为准）：
///
/// 1. **opaque 截断构建**（overlay.dart:888-919）
///    `OverlayState::build` 从**栈顶**向下遍历条目。一旦遇到
///    `opaque == true` 的条目，它之下的条目就不再被构建——除非该条目
///    `maintainState == true`（此时它仍会被构建，但标记为 offstage，
///    不参与布局与命中测试）。
///    因此「一个 opaque 条目遮住下方所有内容」这一行为是在 **build
///    阶段**实现的，而不是靠裁剪。
///
/// 2. **条目顺序 = 绘制顺序**（overlay.dart:888-919）
///    `_entries` 的第一个元素在**最底部**，最后一个在**最顶部**。
///    build 时先倒序填充 children，再整体反转，得到「从底到顶」顺序。
///
/// 3. **skipCount 表达 offstage**（overlay.dart:984-1020）
///    `_Theater.skipCount` = 位于列表**前面**（即更底部）的 offstage
///    条目个数。渲染层据此只布局/绘制/命中 `children[skipCount..]`。
///
/// 4. **条目状态独立**（overlay.dart:111-296）
///    `OverlayEntry` 自身是 `Listenable`：`mounted` 变化时通知监听器；
///    `remove()` 把自身从所属 Overlay 摘除；`markNeedsBuild()` 让条目
///    的 builder 重新执行。
///
/// 5. **OverlayState 是插入的唯一入口**
///    条目不能在构造时直接挂到 Overlay 上，必须经由 `OverlayState`
///    的 insert 系列方法，否则 上游 会断言失败。

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/framework/framework.h"
#include "cui/framework/render_object_widget.h"

namespace cui {

class OverlayState;

/// 条目的内容构造器。上游的 `WidgetBuilder`。
///
/// 参数是当前的 BuildContext，返回要构建的子树。
using OverlayEntryBuilder = std::function<SharedPtr<Widget>(BuildContext&)>;

/// Overlay 中的一条内容。上游的 `OverlayEntry`。
///
/// 生命周期：
///   构造 → `OverlayState::insert` 插入 → （可选）`remove()` 移除
///
/// `remove()` 之后条目不再属于任何 Overlay；`markNeedsBuild()` 让该条目
/// 的 builder 重新执行（上游 通过 GlobalKey 定位到条目对应的
/// Element，本移植通过条目内部持有的 Element 指针达成同样效果）。
class OverlayEntry : public EnableSharedFromThis<OverlayEntry> {
public:
    /// 构造一个条目。
    ///
    /// @param builder     内容构造器
    /// @param opaque      是否完全遮挡下方条目
    /// @param maintainState 是否在下方存在 opaque 条目时仍保持构建
    explicit OverlayEntry(OverlayEntryBuilder builder, bool opaque = false,
                          bool maintainState = false);

    /// 内容构造器。
    const OverlayEntryBuilder& builder() const { return builder_; }

    /// 是否完全遮挡下方条目。上游的 `OverlayEntry.opaque`。
    bool opaque() const { return opaque_; }
    /// 设置 opaque。变更时会通知所属 Overlay 重新构建。
    void setOpaque(bool opaque);

    /// 是否在被 opaque 条目遮挡时仍保持构建。上游的
    /// `OverlayEntry.maintainState`。
    bool maintainState() const { return maintainState_; }
    /// 设置 maintainState。变更时会通知所属 Overlay 重新构建。
    void setMaintainState(bool maintainState);

    /// 是否已挂载到某个 Overlay。上游的 `OverlayEntry.mounted`。
    bool mounted() const { return overlay_ != nullptr; }

    /// 所属的 Overlay（未挂载为 nullptr）。
    OverlayState* overlay() const { return overlay_; }

    /// 从所属 Overlay 中移除自己。上游的 `OverlayEntry.remove`。
    ///
    /// 只能调用一次；重复调用是编程错误（上游断言）。
    void remove();

    /// 让本条目的 builder 重新执行。上游的
    /// `OverlayEntry.markNeedsBuild`。
    void markNeedsBuild();

    /// 条目在 Overlay 内的稳定标识（用于 reconcile 时的 Key 匹配）。
    uint64_t id() const { return id_; }

    /// 由 OverlayState 在挂载/卸载时写入所属的 Overlay。
    void setOverlay(OverlayState* overlay);

    /// 由 OverlayEntry 的宿主 State 在挂载/卸载时写入宿主 Element。
    ///
    /// 供 `markNeedsBuild()` 定位到需要重建的 Element。
    void setHostElement(Element* element);

    /// 通知所属 Overlay：某条目的属性变了，需要重新构建。
    void notifyOwnerChanged();

private:
    friend class OverlayState;

    OverlayEntryBuilder builder_;
    bool opaque_ = false;
    bool maintainState_ = false;
    OverlayState* overlay_ = nullptr;
    Element* hostElement_ = nullptr;
    uint64_t id_ = 0;
};

/// Overlay 的状态。上游的 `OverlayState`。
///
/// 条目插入 / 移除 / 重排都经由这里，且都会触发一次重建
/// （上游的 `setState`）。
class OverlayState : public State {
public:
    /// 当前条目列表。**顺序为从底到顶**（上游的 `_entries`）。
    const std::vector<SharedPtr<OverlayEntry>>& entries() const { return entries_; }

    /// 插入一个条目。
    ///
    /// `below` 非空 → 插到该条目之下；`above` 非空 → 插到该条目之上；
    /// 都为空 → 插到最顶部。`below` 与 `above` 不能同时给出。
    ///
    /// 上游的 `OverlayState.insert`（overlay.dart:743-757）。
    void insert(const SharedPtr<OverlayEntry>& entry,
                const SharedPtr<OverlayEntry>& below = nullptr,
                const SharedPtr<OverlayEntry>& above = nullptr);

    /// 批量插入条目。上游的 `OverlayState.insertAll`
    /// （overlay.dart:759-812）。
    void insertAll(const std::vector<SharedPtr<OverlayEntry>>& newEntries,
                   const SharedPtr<OverlayEntry>& below = nullptr,
                   const SharedPtr<OverlayEntry>& above = nullptr);

    /// 重新排列条目。
    ///
    /// `newEntries` 中不在当前 Overlay 里的条目按 `insertAll` 语义插入；
    /// 当前 Overlay 里但不在 `newEntries` 中的条目被移除。
    /// 上游的 `OverlayState.rearrange`（overlay.dart:814-847）。
    void rearrange(const std::vector<SharedPtr<OverlayEntry>>& newEntries,
                   const SharedPtr<OverlayEntry>& below = nullptr,
                   const SharedPtr<OverlayEntry>& above = nullptr);

    /// 移除一个条目（内部使用，通常由 `OverlayEntry::remove` 调用）。
    void removeEntry(const SharedPtr<OverlayEntry>& entry);

    /// 查找最近的上层 Overlay。上游的 `Overlay.of`（overlay.dart:576-633）。
    ///
    /// 找不到时返回 nullptr（对应 上游的 `Overlay.maybeOf`）。
    static OverlayState* maybeOf(BuildContext& context);
    /// 查找最近的上层 Overlay，找不到时抛异常。
    static OverlayState* of(BuildContext& context);

protected:
    void initState() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    friend class OverlayEntry;

    /// 计算插入位置。上游的 `_insertionIndex`（overlay.dart:665-677）。
    size_t insertionIndex(const SharedPtr<OverlayEntry>& below,
                          const SharedPtr<OverlayEntry>& above) const;

    /// 条目属性变化后请求重建（上游的 `_didChangeEntryOpacity`）。
    void markDirty();

    std::vector<SharedPtr<OverlayEntry>> entries_;
};

/// Overlay 部件。上游的 `Overlay`（StatefulWidget）。
///
/// 上游的 Overlay 需要一个 `Directionality` 祖先来处理
/// `Positioned.directional`；本移植尚未引入 Directionality，
/// 因此该约束暂不生效。
class Overlay : public StatefulWidget {
public:
    explicit Overlay(std::vector<SharedPtr<OverlayEntry>> initialEntries = {},
                     KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), initialEntries_(std::move(initialEntries)) {}

    /// 初始条目。上游的 `Overlay.initialEntries`。
    const std::vector<SharedPtr<OverlayEntry>>& initialEntries() const {
        return initialEntries_;
    }

    std::unique_ptr<State> createState() override;

private:
    std::vector<SharedPtr<OverlayEntry>> initialEntries_;
};

}
