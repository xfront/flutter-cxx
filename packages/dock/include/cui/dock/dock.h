#pragma once
/// @file dock.h
/// @brief 基于树的停靠（dock）框架 —— 状态机 + 渲染。
///
/// 对齐 rui `crates/rui/src/widgets/advanced/docking.rs`（那份本身是 CUI
/// `docking.h/.cpp` 的移植）：`DockNode` 是纯布局树（Split 恰有两个子节点 +
/// 一个分割比例，TabGroup 是 ID 列表 + 活动 ID），`DockManager` 持有树与全部
/// 运行时状态（隐藏/浮动/最大化/展开），`Docking` 是进程级静态门面，
/// `RootDockingPanel` 负责绘制与手势。
///
/// 三条口径（勿当 bug 排查）：
///   1. **树是不可变风格**：所有结构性变更走「重建路径」的静态树操作
///      （removeNode/insertRelative/activateNode/updateDivider/...），只有
///      `setSplitRatioByPath` 就地改分割比例（拖分割条每帧都要写）。
///   2. **`hidden_`/`floating_` 以「组」为单位**：map 的键是这一组面板的第一个
///      ID，值里记 `SavedTreePosition`（原兄弟 + 相对它的方向 + 原分割比例），
///      所以自动隐藏/浮动恢复后尺寸与隐藏前一致。
///   3. **监听器一律走句柄**：捕获 `this` 的 lambda 无法靠目标比较摘除（见
///      AGENTS 的「监听摘除铁律」），所以 add*Listener 返回句柄、持有者必须
///      在 dispose 里 remove*Listener。

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/framework/framework.h"
#include "cui/widgets/material.h"

namespace cui {

// ═══════════════════════════════════════════════════════════════════════════
// 枚举
// ═══════════════════════════════════════════════════════════════════════════

/// 停靠方向（相对另一个面板）。Left/Right/Top/Bottom 把目标拆开，Center 并入
/// 目标所在的标签组。
enum class DockingRegion { Left, Right, Top, Bottom, Center };

/// 面板可自动隐藏到的边缘。
enum class DockEdge { Left, Right, Top, Bottom };

/// Split 节点方向：Horizontal = 子节点从左到右排布（分割条是竖线）。
enum class SplitOrientation { Horizontal, Vertical };

/// 可停靠面板的方向约束。
enum class DockableStyle { Both, Horizontal, Vertical, CenterOnly };

/// 标签栏位置（对齐 rui `TabMode`，默认 Bottom = VS 风格）。
enum class TabMode { Bottom, Top };

/// 标签放不下时的行为（对齐 rui `TabLayout`）。
enum class TabLayout { Scroll, Wrap };

/// 停靠状态变化事件类型。
enum class DockingEventType {
    Docked, Undocked, Shown, Hidden,
    AutoHidden, AutoShown, Pinned, Unpinned,
    Maximized, Restored, Closed, Floated, Unfloated,
};

/// 停靠状态变化事件（只持持久 ID，实例用 `Docking::findDockable` 取回）。
struct DockingEvent {
    std::string dockableId;
    DockingEventType eventType = DockingEventType::Docked;
    /// 拖拽过程中为 true（非永久变更）。
    bool temporary = false;
};

/// 面板被隐藏/浮动前在树中的位置，用于「恢复到原位、原比例」。
struct SavedTreePosition {
    std::string siblingId;
    /// 相对 `siblingId` 的方向（Left 表示自己在兄弟左边）。
    DockingRegion direction = DockingRegion::Left;
    double dividerRatio = 0.5;
    /// 没有兄弟节点（根就是标签组）时为 false。
    bool valid = false;
};

/// 自动隐藏到某边缘的一组面板（map 键 = groupIds 的第一个 ID）。
struct HiddenPanelInfo {
    DockEdge edge = DockEdge::Left;
    SavedTreePosition savedPosition;
    std::vector<std::string> groupIds;
};

/// 浮动的一组面板（map 键 = ids 的第一个 ID）。位置相对面板区域，尺寸下限
/// 200×150 由 `updateFloatingSize` 夹住。
struct FloatingPanelInfo {
    std::vector<std::string> ids;
    std::string activeId;
    double offsetX = 100.0;
    double offsetY = 100.0;
    double width = 400.0;
    double height = 300.0;
    SavedTreePosition savedPosition;
};

// ═══════════════════════════════════════════════════════════════════════════
// DockNode — 纯布局树
// ═══════════════════════════════════════════════════════════════════════════

/// 布局树节点：Split（first/second + dividerRatio）或 TabGroup（items +
/// activeId）。不持有 widget、不渲染。
class DockNode {
public:
    enum class Kind { Split, TabGroup };

    /// 工厂：持有 `ids` 且选中 `active` 的标签组；`active` 为空取 `ids[0]`。
    static SharedPtr<DockNode> newTab(std::vector<std::string> ids, std::string active = "");
    /// 工厂：`first` 与 `second` 的分割，`ratio` 是 first 占据的比例。
    static SharedPtr<DockNode> newSplit(SplitOrientation orientation, SharedPtr<DockNode> first,
                                       SharedPtr<DockNode> second, double ratio = 0.5);

    bool isSplit() const { return kind == Kind::Split; }
    bool isTabGroup() const { return kind == Kind::TabGroup; }

    /// 子树中全部面板 ID（标签组按 items 顺序）。
    std::vector<std::string> collectIds() const;

    /// 深拷贝（结构与 ratio 一起复制）。
    SharedPtr<DockNode> cloneNode() const;

    /// 标签组标识 = 首个面板 ID（隐藏/浮动 map 的键、拖拽落点的组键都用它）。
    const std::string& groupId() const {
        static const std::string kEmpty;
        return items.empty() ? kEmpty : items.front();
    }

    Kind kind = Kind::TabGroup;
    // ── Split ──
    SplitOrientation orientation = SplitOrientation::Horizontal;
    SharedPtr<DockNode> first;
    SharedPtr<DockNode> second;
    double dividerRatio = 0.5;
    // ── TabGroup ──
    std::vector<std::string> items;
    std::string activeId;
};

// ═══════════════════════════════════════════════════════════════════════════
// Dockable — 可停靠面板接口
// ═══════════════════════════════════════════════════════════════════════════

/// 可停靠面板接口。`examples/ide.cpp` 的 `IdePanel` 实现它，用 `content()`
/// 交出一棵控件子树。
class Dockable {
public:
    virtual ~Dockable() = default;

    /// 用于持久化与编程式停靠的唯一标识。
    virtual std::string persistentID() const = 0;
    /// 标签页上显示的文本。
    virtual std::string tabText() const = 0;
    /// 面板内容（控件子树）。
    virtual SharedPtr<Widget> content() const = 0;

    /// 单面板头部的标题（默认取 tabText）。
    virtual std::string titleText() const { return tabText(); }
    /// 标签页提示。
    virtual std::string tabTooltip() const { return {}; }
    /// 停靠方向约束。
    virtual DockableStyle style() const { return DockableStyle::Both; }
    /// 自动隐藏的边缘约束。
    virtual DockableStyle autoHideStyle() const { return style(); }
    /// 是否允许浮动。
    virtual bool isFloatingAllowed() const { return true; }
    /// 是否可关闭。
    virtual bool isClosable() const { return true; }
    /// 是否允许自动隐藏到边缘（对齐 rui：默认 false，显式开启）。
    virtual bool isAutoHideAllowed() const { return false; }
    /// 是否允许最大化/还原。
    virtual bool isMinMaxAllowed() const { return false; }
    /// 分组类型（用于「同类型才能并组」的限制）。
    virtual int panelType() const { return 0; }
    /// 首选浮动尺寸（<=0 用默认 400×300）。
    virtual double floatingWidth() const { return 0.0; }
    virtual double floatingHeight() const { return 0.0; }
    /// 关闭前回调；返回 false 可否决关闭。
    virtual bool requestClose() const { return true; }
    /// 自定义右键菜单动作（标签页右键菜单末尾追加）。
    virtual std::vector<std::string> contextMenuActions() const { return {}; }
    /// 自定义右键菜单动作被选中时回调。
    virtual void onContextMenuAction(const std::string&) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 布局状态机
// ═══════════════════════════════════════════════════════════════════════════

using DockingEventCallback = std::function<void(const DockingEvent&)>;
using MaximizeCallback = std::function<void(const std::string& dockableId, bool maximized)>;
using LayoutCallback = std::function<void()>;

/// 拥有停靠布局树与全部状态。纯状态持有者，不做渲染。
class DockManager {
public:
    static DockManager& instance();

    /// 丢弃整个布局（树 + 隐藏 + 浮动 + 最大化/展开），保留注册表与监听器。
    void reset();

    // ── 注册 ──────────────────────────────────────────────────────────────
    void registerDockable(Dockable* d);
    void unregisterDockable(Dockable* d);
    void unregisterDockableById(const std::string& id);
    Dockable* find(const std::string& id) const;
    bool isRegistered(const std::string& id) const;
    std::vector<Dockable*> allDockables() const;

    // ── 树访问（渲染器用）────────────────────────────────────────────────
    SharedPtr<DockNode> root() const { return root_; }
    const std::unordered_map<std::string, HiddenPanelInfo>& hiddenPanels() const {
        return hiddenPanels_;
    }
    const std::unordered_map<std::string, FloatingPanelInfo>& floatingPanels() const {
        return floatingPanels_;
    }
    const std::string& maximized() const { return maximizedPanelId_; }
    const std::string& expandedPanelId() const { return expandedPanelId_; }

    // ── 停靠 ──────────────────────────────────────────────────────────────
    /// 在根级 `region` 方向停靠 `id`（ratio 是新面板占的比例）。
    void dock(const std::string& id, DockingRegion region, double ratio = 0.5);
    /// 相对 `targetId` 停靠 `sourceId`；Center = 并入目标所在的标签组。
    void dockRelative(const std::string& sourceId, const std::string& targetId,
                      DockingRegion region, double ratio = 0.5);
    /// 从树中移除（保持注册）。
    void undock(const std::string& id);
    /// 关闭（尊重 isClosable 与 requestClose 否决）。
    void close(const std::string& id);
    /// 让 `id` 成为所在标签组的活动标签。
    void bringToFront(const std::string& id);
    bool isDocked(const std::string& id) const;
    /// 已停靠→前置；已隐藏→从边缘恢复；已浮动→前置；否则停靠到中心。
    void display(const std::string& id);
    /// 设置「直接子节点包含 id」的那个分割节点的比例。
    void setDividerRatio(const std::string& id, double ratio);

    void switchTab(const std::string& id) { bringToFront(id); }
    void removeTabItem(const std::string& id) { close(id); }
    void reorderTabItem(const std::string& tabId, int fromIndex, int toIndex);

    // ── 浮动 ──────────────────────────────────────────────────────────────
    bool isFloating(const std::string& id) const;
    /// `id` 所在浮动组的 map 键（不在浮动里时为空）。
    std::string floatingKeyFor(const std::string& id) const;
    void floatPanel(const std::string& id);
    void unfloatPanel(const std::string& id);
    void updateFloatingPosition(const std::string& key, double x, double y);
    void updateFloatingSize(const std::string& key, double w, double h);
    void switchFloatingTab(const std::string& key, const std::string& activeId);

    // ── 自动隐藏 ──────────────────────────────────────────────────────────
    bool isHidden(const std::string& id) const;
    std::string hiddenKeyFor(const std::string& id) const;
    /// 整个标签组隐藏到指定边缘。
    void autoHide(const std::string& id, DockEdge edge);
    /// 整个标签组隐藏到「离它最近的边缘」（由树内矩形算出）。
    void hideToEdge(const std::string& id);
    void restoreFromEdge(const std::string& id);
    void toggleAutoHide(const std::string& id);
    /// 兼容旧调用点：hide == 隐藏到最近的边缘。
    void hide(const std::string& id) { hideToEdge(id); }
    /// 从边条展开浮出（不改树）。
    void expandFromEdge(const std::string& id);
    void collapseToEdge();

    // ── 最大化 ────────────────────────────────────────────────────────────
    bool isMaximized(const std::string& id) const { return maximizedPanelId_ == id; }
    void maximize(const std::string& id);
    void restore();
    void toggleMaximize(const std::string& id);

    // ── 查询 / 手势 ───────────────────────────────────────────────────────
    /// 包含 `id` 的标签组的全部 ID（组内只有一个面板时返回 {id}）。
    std::vector<std::string> tabGroupOf(const std::string& id) const;
    /// `id` 相对兄弟节点的树位置（用于恢复到原位）。
    SavedTreePosition findTreePosition(const std::string& id) const;
    /// 基于树内矩形判断 `id` 最近的边缘（1000×800 虚拟画布）。
    DockEdge findEdgeOfPanel(const std::string& id) const;
    /// 按从根出发的结构路径（0=first，1=second）设置分割节点比例。
    void setSplitRatioByPath(const std::vector<int>& path, double ratio);
    /// `id` 所在标签组的当前活动面板。
    std::string activeTabOfGroup(const std::string& groupId) const;

    // ── 序列化 ────────────────────────────────────────────────────────────
    /// 只序列化布局树。
    std::string saveLayoutState() const;
    bool restoreLayoutState(const std::string& state);
    /// 树 + 隐藏 + 浮动 + 最大化（`mdl1` 头）。
    std::string saveFullState() const;
    bool restoreFullState(const std::string& state);

    // ── 事件与重绘 ────────────────────────────────────────────────────────
    /// 结构变化后调用：发事件 → 通知重绘监听器 → 自动持久化。
    void emitLayoutChanged();
    /// 重新发一次 layoutChanged（面板标题等在线更新后触发重绘）。
    void refresh();

    int addListener(LayoutCallback cb);
    void removeListener(int handle);
    size_t listenerCount() const { return layoutListeners_.size(); }

    int addDockingListener(DockingEventCallback cb);
    void removeDockingListener(int handle);
    size_t dockingListenerCount() const { return eventListeners_.size(); }

    int addMaximizeListener(MaximizeCallback cb);
    void removeMaximizeListener(int handle);
    size_t maximizeListenerCount() const { return maximizeListeners_.size(); }

    // ── 自动持久化（由 Docking 门面转发）─────────────────────────────────
    void setPersist(bool enabled);
    bool isPersistEnabled() const;
    void setPersistFile(const std::string& path);
    const std::string& persistFile() const;

private:
    DockManager() = default;

    struct HandleEntry {
        int handle = 0;
        std::function<void()> fn;
    };
    struct EventEntry {
        int handle = 0;
        DockingEventCallback fn;
    };
    struct MaximizeEntry {
        int handle = 0;
        MaximizeCallback fn;
    };

    // 纯树操作（不可变风格：重建路径）。
    static SharedPtr<DockNode> removeNode(const SharedPtr<DockNode>& node, const std::string& id);
    static bool directlyContains(const SharedPtr<DockNode>& node, const std::string& id);
    static std::string firstIdInSubtree(const SharedPtr<DockNode>& node);
    static SharedPtr<DockNode> makeSplitDir(const SharedPtr<DockNode>& existing,
                                            const SharedPtr<DockNode>& addition,
                                            DockingRegion direction, double ratio);
    static SharedPtr<DockNode> insertRelative(const SharedPtr<DockNode>& node,
                                              const std::string& targetId,
                                              const SharedPtr<DockNode>& toInsert,
                                              DockingRegion direction, double ratio);
    static SharedPtr<DockNode> activateNode(const SharedPtr<DockNode>& node, const std::string& id);
    static SharedPtr<DockNode> updateDivider(const SharedPtr<DockNode>& node,
                                             const std::string& id, double ratio);
    static SharedPtr<DockNode> reorderInTabGroup(const SharedPtr<DockNode>& node,
                                                 const std::string& tabId, int from, int to);
    static const DockNode* findTabGroupNode(const SharedPtr<DockNode>& node, const std::string& id);

    /// 从每个隐藏/浮动组里擦除 `id`（组空则删掉整组）。
    void clearPanelRefs(const std::string& id);
    void removeFromTree(const std::string& id) {
        if (root_)
            root_ = removeNode(root_, id);
    }
    void fireEvent(const std::string& dockableId, DockingEventType type, bool temporary);
    int nextHandle() { return nextHandle_++; }

    std::unordered_map<std::string, Dockable*> dockables_;
    SharedPtr<DockNode> root_;
    std::unordered_map<std::string, HiddenPanelInfo> hiddenPanels_;
    std::unordered_map<std::string, FloatingPanelInfo> floatingPanels_;
    std::string maximizedPanelId_;
    std::string expandedPanelId_;
    std::string savedLayoutBeforeMaximize_;

    std::vector<HandleEntry> layoutListeners_;
    std::vector<EventEntry> eventListeners_;
    std::vector<MaximizeEntry> maximizeListeners_;
    int nextHandle_ = 1;

    bool persistEnabled_ = false;
    std::string persistFile_;
};

// ═══════════════════════════════════════════════════════════════════════════
// WindowLayoutBuilder — 编程式布局描述
// ═══════════════════════════════════════════════════════════════════════════

/// 一条布局描述：`targetId` 为空表示停靠到根。
struct LayoutEntry {
    std::string sourceId;
    std::string targetId;
    DockingRegion region = DockingRegion::Center;
    double proportion = 0.25;
};

/// 只描述「按顺序做哪些停靠」，建树由 `DockManager` 执行——这样同一份描述
/// 既能首帧应用，也能用于 `resetLayout()` 与默认布局。
class WindowLayoutBuilder {
public:
    /// 停靠到根级 `region`（proportion 默认 0.25）。
    WindowLayoutBuilder& dock(const std::string& sourceId, DockingRegion region);
    /// 相对 `targetId` 停靠。
    WindowLayoutBuilder& dock(const std::string& sourceId, const std::string& targetId,
                              DockingRegion region, double proportion = 0.5);
    WindowLayoutBuilder& dockRelative(const std::string& sourceId, const std::string& targetId,
                                      DockingRegion region, double proportion);
    /// 改写最近一次停靠操作的比例。
    WindowLayoutBuilder& setDividerProportion(double proportion);

    const std::vector<LayoutEntry>& entries() const { return entries_; }

private:
    std::vector<LayoutEntry> entries_;
};

// ═══════════════════════════════════════════════════════════════════════════
// DockingSettings — 全局外观参数
// ═══════════════════════════════════════════════════════════════════════════

struct DockingSettings {
    static DockingSettings& instance();

    TabMode tabMode = TabMode::Bottom;
    TabLayout tabLayout = TabLayout::Scroll;
    bool activeHighlighter = true;
    /// 标签开始拖拽所需的位移（rui 口径 5）。
    int dragThreshold = 5;
    /// 自动隐藏边条的厚度。
    double stripThickness = 28.0;
    /// 面板头部/标签栏高度。
    double headerHeight = 30.0;
    /// 分割条宽度（命中区是它的 ±1）。
    double splitGap = 4.0;
};

// ═══════════════════════════════════════════════════════════════════════════
// Docking — 静态门面
// ═══════════════════════════════════════════════════════════════════════════

/// 代理到进程全局 `DockManager`，并持有门面状态（命名布局、默认布局）。
class Docking {
public:
    /// 初始化（幂等）。已设置默认布局时立即应用。
    static void initialize();

    static void applyLayout(const WindowLayoutBuilder& b);
    static void setDefaultApplicationLayout(const WindowLayoutBuilder& b);
    /// 回到默认布局（没有默认布局则清空）。
    static void resetLayout();

    static void registerDockable(Dockable* d);
    static void unregisterDockable(Dockable* d);
    static Dockable* findDockable(const std::string& id);
    static std::vector<Dockable*> allDockables();

    static void dock(const std::string& id, DockingRegion region, double ratio = 0.5);
    static void dockRelative(const std::string& sourceId, const std::string& targetId,
                             DockingRegion region, double proportion = 0.5);
    static void undock(const std::string& id);
    static void close(const std::string& id);
    static void bringToFront(const std::string& id);
    static void display(const std::string& id);
    static void hide(const std::string& id);
    static void reorderTabItem(const std::string& tabId, int fromIndex, int toIndex);
    static void setDividerRatio(const std::string& id, double ratio);

    static bool isDocked(const std::string& id);
    static bool isHidden(const std::string& id);
    static bool isFloating(const std::string& id);
    static bool isMaximizedById(const std::string& id);
    static bool isDockableRegistered(const std::string& id);

    static void floatPanel(const std::string& id);
    static void unfloatPanel(const std::string& id);
    static void switchFloatingTab(const std::string& key, const std::string& activeId);
    static void updateFloatingPosition(const std::string& key, double x, double y);
    static void updateFloatingSize(const std::string& key, double w, double h);

    static void autoHide(const std::string& id, DockEdge e);
    static void autoHide(Dockable* d, DockEdge e);
    static void hideToEdge(const std::string& id);
    static void restoreFromEdge(const std::string& id);
    static void toggleAutoHide(const std::string& id);
    static void autoShow(const std::string& id);
    static void collapseToEdge();

    static void maximize(const std::string& id);
    static void maximize(Dockable* d);
    static void restore();
    static void restore(Dockable* d);
    static bool isMaximized(Dockable* d);
    static void toggleMaximize(const std::string& id);

    static std::string saveLayout();
    static bool loadLayout(const std::string& state);
    static bool saveLayoutToFile(const std::string& path);
    static bool loadLayoutFromFile(const std::string& path);
    static bool saveNamedLayout(const std::string& name);
    static bool restoreNamedLayout(const std::string& name);
    static std::vector<std::string> layoutNames();

    static void setPersist(bool enabled);
    static bool isPersistEnabled();
    static void setPersistFile(const std::string& path);

    /// 面板标题等在线更新后请求重绘。
    static void repaint();

    static void updateTabText(const std::string& id);

    /// 菜单项应显示的文本（tabText 优先，未注册则回落到 id）。
    static std::string dockableMenuLabel(const std::string& id);
    /// 菜单项动作：已停靠/浮动 → close，否则 display。
    static std::function<void()> dockableMenuAction(const std::string& id);
    /// 已保存命名布局的菜单项：(名称, restore action) 对列表。
    static std::vector<std::pair<std::string, std::function<void()>>> layoutsMenuActions();

    /// 别名：show = display、newWindow = floatPanel（对齐 rui 门面命名）。
    static void show(const std::string& id) { display(id); }
    static void newWindow(const std::string& id) { floatPanel(id); }

private:
    /// 门面状态（命名布局 + 默认布局 + 是否已初始化）。
    struct FacadeState {
        bool initialized = false;
        std::vector<std::pair<std::string, std::string>> namedLayouts;
        std::shared_ptr<WindowLayoutBuilder> defaultLayout;
    };
    static FacadeState& facade();
};

// ═══════════════════════════════════════════════════════════════════════════
// RootDockingPanel
// ═══════════════════════════════════════════════════════════════════════════

/// 渲染 `DockManager` 的布局树并接管全部停靠手势。
class RootDockingPanel : public StatefulWidget {
public:
    RootDockingPanel(KeyPtr key = nullptr) : StatefulWidget(std::move(key)) {}
    std::unique_ptr<State> createState() override;
};

}  // namespace cui
