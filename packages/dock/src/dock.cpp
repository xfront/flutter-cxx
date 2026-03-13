/// @file dock.cpp
/// @brief 停靠框架状态机 + 静态门面 + 组合式渲染。
///
/// 对齐 rui `crates/rui/src/widgets/advanced/docking.rs`（逐段注释里标了对应行
/// 号区间）。本文件三段：数据模型 + 树操作 + 事件/序列化（rui 的状态机部分）、
/// 组合式渲染（rui `paint_tab_group` / `paint_drag_overlay` 的对应物）、
/// `RootDockingPanel` 的面级手势（rui `on_mouse_press/move/release_impl`）。
///
/// 手势是「面板级」的：整块 Stack 外面包一个 Listener（收全部指针事件），命中判定
/// 自己按渲染侧同一套度量重算（`walkForSplitter` / `walkForHeader`），不给每个标签
/// 单独挂识别器 —— 对齐 rui，也避免拖拽途中子树被换掉时识别器跟着消失。
/// 按下只记录、释放仍落在同一矩形才执行，与 rui 的 press/release 分离一致。
///
/// 有意未接：Esc 取消拖拽（rui 在窗口级按键回调里清 drag_state；本仓的键事件
/// 走 FocusNode，而停靠面板没有焦点域，接线留到后续批次）。
///
/// 与 rui 的三条有意差异（勿当 bug 排查）：
///   1. **锁换成单线程**：rui 用全局 Mutex/RefCell 是因为 Rust 要证明线程安全；
///      本仓 GUI 单线程，`DockManager::instance()` 直接返回引用，
///      所以 rui 里「回调不得重入门面」的锁纪律在这里天然不存在，但
///      **不要在监听器回调里再调门面**仍然成立（会重复 emit）。
///   2. **五个信号并成一个**：rui 除 `DockingEvent` 外还有
///      docked/undocked/closed/layoutChanged 四条 `Event<String>`；本仓全部
///      走 `DockingEventType` 的事件监听器列表，`layoutChanged` 走
///      `addListener` 那组（渲染器订阅的就是它）。
///   3. **监听器按句柄摘除**：rui 返回下标、删除时后面的索引会前移；本仓按
///      AGENTS 的「监听摘除铁律」发句柄，`removeListener(handle)` 精确定位。

#include "cui/dock/dock.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>

#include "cui/foundation/application.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/paint.h"

namespace cui {

// ═══════════════════════════════════════════════════════════════════════════
// 文件内辅助（对齐 rui docking.rs L274-520）
// ═══════════════════════════════════════════════════════════════════════════

namespace {

bool containsId(const std::vector<std::string>& v, const std::string& id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/// 紧凑、与区域无关的双精度格式化（"0.5"、"0.25"、"100"）。
std::string numStr(double v) {
    char buf[32];
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    else
        std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

double parseDoubleOr(const std::string& s, double fallback) {
    std::string t = trim(s);
    if (t.empty())
        return fallback;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    return end == t.c_str() ? fallback : v;
}

int parseIntOr(const std::string& s, int fallback) {
    std::string t = trim(s);
    if (t.empty())
        return fallback;
    char* end = nullptr;
    long v = std::strtol(t.c_str(), &end, 10);
    return end == t.c_str() ? fallback : static_cast<int>(v);
}

DockingRegion edgeToRegion(DockEdge e) {
    switch (e) {
        case DockEdge::Left: return DockingRegion::Left;
        case DockEdge::Right: return DockingRegion::Right;
        case DockEdge::Top: return DockingRegion::Top;
        case DockEdge::Bottom: return DockingRegion::Bottom;
    }
    return DockingRegion::Left;
}

/// 序列化整数映射（Center=4，与 rui/CUI 一致）。
int regionToInt(DockingRegion r) {
    switch (r) {
        case DockingRegion::Left: return 0;
        case DockingRegion::Right: return 1;
        case DockingRegion::Top: return 2;
        case DockingRegion::Bottom: return 3;
        case DockingRegion::Center: return 4;
    }
    return 4;
}

DockingRegion intToRegion(int i) {
    switch (i) {
        case 0: return DockingRegion::Left;
        case 1: return DockingRegion::Right;
        case 2: return DockingRegion::Top;
        case 3: return DockingRegion::Bottom;
        default: return DockingRegion::Center;
    }
}

int edgeToInt(DockEdge e) {
    switch (e) {
        case DockEdge::Left: return 0;
        case DockEdge::Right: return 1;
        case DockEdge::Top: return 2;
        case DockEdge::Bottom: return 3;
    }
    return 0;
}

DockEdge intToEdge(int i) {
    switch (i) {
        case 1: return DockEdge::Right;
        case 2: return DockEdge::Top;
        case 3: return DockEdge::Bottom;
        default: return DockEdge::Left;
    }
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string joinCsv(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out.push_back(',');
        out += v[i];
    }
    return out;
}

/// `s` 在 `open` 处的 '(' 匹配的 ')' 下标，找不到返回 npos。
size_t findMatchingParen(const std::string& s, size_t open) {
    if (open >= s.size() || s[open] != '(')
        return std::string::npos;
    int depth = 1;
    for (size_t i = open + 1; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

/// 按顶层 `delim` 分割（忽略括号内的）。
std::vector<std::string> splitTopLevel(const std::string& s, char delim) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == delim && depth == 0) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(s.substr(start));
    return out;
}

/// 树序列化（`T(id,id|active)` / `S(H|V,ratio,first,second)`）。
void serializeNode(const SharedPtr<DockNode>& node, std::string& out) {
    if (!node) return;
    if (node->isTabGroup()) {
        out += "T(";
        out += joinCsv(node->items);
        out += "|";
        out += node->activeId;
        out += ")";
    } else {
        out += "S(";
        out += node->orientation == SplitOrientation::Horizontal ? "H" : "V";
        out += ",";
        out += numStr(node->dividerRatio);
        out += ",";
        serializeNode(node->first, out);
        out += ",";
        serializeNode(node->second, out);
        out += ")";
    }
}

/// 递归下降解析。未注册的标签 ID 被过滤；无有效 ID 的标签组返回空。
SharedPtr<DockNode> parseNode(const std::string& s, size_t& pos, const DockManager& mgr) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    if (pos >= s.size()) return nullptr;
    char tag = s[pos];
    size_t open = s.find('(', pos);
    if (open == std::string::npos) return nullptr;
    size_t close = findMatchingParen(s, open);
    if (close == std::string::npos) return nullptr;
    std::string inner = s.substr(open + 1, close - open - 1);
    pos = close + 1;

    if (tag == 'T') {
        size_t pipe = inner.find('|');
        if (pipe == std::string::npos) return nullptr;
        std::string activeId = inner.substr(pipe + 1);
        std::vector<std::string> ids;
        for (auto& id : splitCsv(inner.substr(0, pipe)))
            if (mgr.isRegistered(id)) ids.push_back(id);
        if (ids.empty()) return nullptr;
        return DockNode::newTab(ids, containsId(ids, activeId) ? activeId : ids.front());
    }
    if (tag == 'S') {
        std::vector<std::string> parts = splitTopLevel(inner, ',');
        if (parts.size() < 4) return nullptr;
        SplitOrientation orientation =
            trim(parts[0]) == "H" ? SplitOrientation::Horizontal : SplitOrientation::Vertical;
        double ratio = parseDoubleOr(parts[1], 0.5);
        size_t dummy = 0;
        SharedPtr<DockNode> first = parseNode(trim(parts[2]), dummy, mgr);
        if (!first) return nullptr;
        dummy = 0;
        SharedPtr<DockNode> second = parseNode(trim(parts[3]), dummy, mgr);
        if (!second) return nullptr;
        return DockNode::newSplit(orientation, first, second, ratio);
    }
    return nullptr;
}

/// `id` 相对兄弟的树位置（标签组内部没有方向性关系 ⇒ false）。
bool findTreePosInNode(const SharedPtr<DockNode>& node, const std::string& id,
                       SavedTreePosition& out) {
    if (!node || !node->isSplit())
        return false;
    std::vector<std::string> firstIds = node->first ? node->first->collectIds() : std::vector<std::string>{};
    std::vector<std::string> secondIds = node->second ? node->second->collectIds() : std::vector<std::string>{};

    auto siblingOf = [](const std::vector<std::string>& v) {
        return v.empty() ? std::string{} : v.front();
    };

    if (containsId(firstIds, id)) {
        SavedTreePosition fallback;
        fallback.siblingId = siblingOf(secondIds);
        fallback.direction = node->orientation == SplitOrientation::Horizontal
                                 ? DockingRegion::Left
                                 : DockingRegion::Top;
        fallback.dividerRatio = node->dividerRatio;
        fallback.valid = !fallback.siblingId.empty();
        if (findTreePosInNode(node->first, id, out))
            return true;
        out = fallback;
        return out.valid;
    }
    if (containsId(secondIds, id)) {
        SavedTreePosition fallback;
        fallback.siblingId = siblingOf(firstIds);
        fallback.direction = node->orientation == SplitOrientation::Horizontal
                                 ? DockingRegion::Right
                                 : DockingRegion::Bottom;
        fallback.dividerRatio = 1.0 - node->dividerRatio;
        fallback.valid = !fallback.siblingId.empty();
        if (findTreePosInNode(node->second, id, out))
            return true;
        out = fallback;
        return out.valid;
    }
    return false;
}

/// 虚拟画布里的分割矩形（`sp` 是分割条宽度，对齐 rui compute_split_bounds）。
void computeSplitBounds(const DockNode& node, double ox, double oy, double w, double h,
                        Rect& first, Rect& second) {
    const double sp = 4.0;
    if (node.orientation == SplitOrientation::Horizontal) {
        double fw = w * node.dividerRatio - sp / 2.0;
        double sw = w * (1.0 - node.dividerRatio) - sp / 2.0;
        first = Rect{ox, oy, std::max(0.0, fw), h};
        second = Rect{ox + fw + sp, oy, std::max(0.0, sw), h};
    } else {
        double fh = h * node.dividerRatio - sp / 2.0;
        double sh = h * (1.0 - node.dividerRatio) - sp / 2.0;
        first = Rect{ox, oy, w, std::max(0.0, fh)};
        second = Rect{ox, oy + fh + sp, w, std::max(0.0, sh)};
    }
}

bool findNodeRectVirtual(const SharedPtr<DockNode>& node, const std::string& id,
                         double ox, double oy, double w, double h, Rect& out) {
    if (!node) return false;
    if (node->isTabGroup()) {
        if (containsId(node->items, id)) {
            out = Rect{ox, oy, w, h};
            return true;
        }
        return false;
    }
    Rect a, b;
    computeSplitBounds(*node, ox, oy, w, h, a, b);
    if (findNodeRectVirtual(node->first, id, a.x, a.y, a.width, a.height, out))
        return true;
    return findNodeRectVirtual(node->second, id, b.x, b.y, b.width, b.height, out);
}

std::vector<std::string> linesOf(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// DockNode
// ═══════════════════════════════════════════════════════════════════════════

SharedPtr<DockNode> DockNode::newTab(std::vector<std::string> ids, std::string active) {
    auto n = cui::make_shared<DockNode>();
    n->kind = Kind::TabGroup;
    n->items = std::move(ids);
    if (active.empty() && !n->items.empty())
        active = n->items.front();
    n->activeId = std::move(active);
    return n;
}

SharedPtr<DockNode> DockNode::newSplit(SplitOrientation orientation, SharedPtr<DockNode> first,
                                     SharedPtr<DockNode> second, double ratio) {
    auto n = cui::make_shared<DockNode>();
    n->kind = Kind::Split;
    n->orientation = orientation;
    n->first = std::move(first);
    n->second = std::move(second);
    n->dividerRatio = ratio;
    return n;
}

std::vector<std::string> DockNode::collectIds() const {
    if (isTabGroup()) return items;
    std::vector<std::string> out;
    if (first) {
        auto a = first->collectIds();
        out.insert(out.end(), a.begin(), a.end());
    }
    if (second) {
        auto b = second->collectIds();
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

SharedPtr<DockNode> DockNode::cloneNode() const {
    auto n = cui::make_shared<DockNode>();
    n->kind = kind;
    n->orientation = orientation;
    n->dividerRatio = dividerRatio;
    n->items = items;
    n->activeId = activeId;
    n->first = first ? first->cloneNode() : nullptr;
    n->second = second ? second->cloneNode() : nullptr;
    return n;
}

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 纯树操作（对齐 rui docking.rs L663-812）
// ═══════════════════════════════════════════════════════════════════════════

SharedPtr<DockNode> DockManager::removeNode(const SharedPtr<DockNode>& node,
                                            const std::string& id) {
    if (!node) return nullptr;
    if (node->isTabGroup()) {
        std::vector<std::string> items;
        for (const auto& i : node->items)
            if (i != id) items.push_back(i);
        if (items.empty()) return nullptr;
        if (items.size() == 1) return DockNode::newTab(items, items[0]);
        std::string active = node->activeId == id ? items.back() : node->activeId;
        if (!containsId(items, active)) active = items.back();
        return DockNode::newTab(items, active);
    }
    SharedPtr<DockNode> nf = removeNode(node->first, id);
    SharedPtr<DockNode> ns = removeNode(node->second, id);
    if (!nf && !ns) return nullptr;
    if (nf && !ns) return nf;
    if (!nf && ns) return ns;
    return DockNode::newSplit(node->orientation, nf, ns, node->dividerRatio);
}

bool DockManager::directlyContains(const SharedPtr<DockNode>& node, const std::string& id) {
    return node && node->isTabGroup() && containsId(node->items, id);
}

std::string DockManager::firstIdInSubtree(const SharedPtr<DockNode>& node) {
    if (!node) return "";
    if (node->isTabGroup()) return node->items.empty() ? "" : node->items.front();
    if (std::string f = firstIdInSubtree(node->first); !f.empty()) return f;
    return firstIdInSubtree(node->second);
}

SharedPtr<DockNode> DockManager::makeSplitDir(const SharedPtr<DockNode>& existing,
                                             const SharedPtr<DockNode>& addition,
                                             DockingRegion direction, double ratio) {
    switch (direction) {
        case DockingRegion::Left:
            return DockNode::newSplit(SplitOrientation::Horizontal, addition, existing, ratio);
        case DockingRegion::Right:
            return DockNode::newSplit(SplitOrientation::Horizontal, existing, addition, 1.0 - ratio);
        case DockingRegion::Top:
            return DockNode::newSplit(SplitOrientation::Vertical, addition, existing, ratio);
        case DockingRegion::Bottom:
            return DockNode::newSplit(SplitOrientation::Vertical, existing, addition, 1.0 - ratio);
        case DockingRegion::Center:
            return DockNode::newSplit(SplitOrientation::Horizontal, existing, addition, 1.0 - ratio);
    }
    return existing;
}

SharedPtr<DockNode> DockManager::insertRelative(const SharedPtr<DockNode>& node,
                                                const std::string& targetId,
                                                const SharedPtr<DockNode>& toInsert,
                                                DockingRegion direction, double ratio) {
    if (!node) return nullptr;
    if (node->isTabGroup()) {
        if (!containsId(node->items, targetId)) return node->cloneNode();
        if (direction == DockingRegion::Center) {
            std::vector<std::string> items = node->items;
            std::vector<std::string> add = toInsert ? toInsert->collectIds() : std::vector<std::string>{};
            // 新并入的面板立刻成为活动标签（对齐 rui/CUI）。
            std::string active = add.empty() ? node->activeId : add.front();
            items.insert(items.end(), add.begin(), add.end());
            return DockNode::newTab(items, active);
        }
        return makeSplitDir(node, toInsert, direction, ratio);
    }
    std::vector<std::string> firstIds = node->first ? node->first->collectIds() : std::vector<std::string>{};
    std::vector<std::string> secondIds = node->second ? node->second->collectIds() : std::vector<std::string>{};
    if (containsId(firstIds, targetId)) {
        // 不变式：target 在 first 子树里，递归必然命中。
        SharedPtr<DockNode> nf = insertRelative(node->first, targetId, toInsert, direction, ratio);
        return DockNode::newSplit(node->orientation,
                                  nf ? nf : (node->first ? node->first->cloneNode() : nullptr),
                                  node->second ? node->second->cloneNode() : nullptr,
                                  node->dividerRatio);
    }
    if (containsId(secondIds, targetId)) {
        SharedPtr<DockNode> ns = insertRelative(node->second, targetId, toInsert, direction, ratio);
        return DockNode::newSplit(node->orientation,
                                  node->first ? node->first->cloneNode() : nullptr,
                                  ns ? ns : (node->second ? node->second->cloneNode() : nullptr),
                                  node->dividerRatio);
    }
    return node->cloneNode();
}

SharedPtr<DockNode> DockManager::activateNode(const SharedPtr<DockNode>& node,
                                             const std::string& id) {
    if (!node) return nullptr;
    if (node->isTabGroup()) {
        SharedPtr<DockNode> c = node->cloneNode();
        if (containsId(c->items, id)) c->activeId = id;
        return c;
    }
    SharedPtr<DockNode> f = activateNode(node->first, id);
    SharedPtr<DockNode> s = activateNode(node->second, id);
    return DockNode::newSplit(node->orientation,
                              f ? f : (node->first ? node->first->cloneNode() : nullptr),
                              s ? s : (node->second ? node->second->cloneNode() : nullptr),
                              node->dividerRatio);
}

SharedPtr<DockNode> DockManager::updateDivider(const SharedPtr<DockNode>& node,
                                               const std::string& id, double ratio) {
    if (!node) return nullptr;
    if (node->isTabGroup()) return node->cloneNode();
    if (directlyContains(node->first, id) || directlyContains(node->second, id)) {
        return DockNode::newSplit(node->orientation,
                                  node->first ? node->first->cloneNode() : nullptr,
                                  node->second ? node->second->cloneNode() : nullptr, ratio);
    }
    SharedPtr<DockNode> f = updateDivider(node->first, id, ratio);
    SharedPtr<DockNode> s = updateDivider(node->second, id, ratio);
    return DockNode::newSplit(node->orientation,
                              f ? f : (node->first ? node->first->cloneNode() : nullptr),
                              s ? s : (node->second ? node->second->cloneNode() : nullptr),
                              node->dividerRatio);
}

SharedPtr<DockNode> DockManager::reorderInTabGroup(const SharedPtr<DockNode>& node,
                                                   const std::string& tabId, int from, int to) {
    if (!node) return nullptr;
    if (node->isTabGroup()) {
        SharedPtr<DockNode> c = node->cloneNode();
        if (!containsId(c->items, tabId)) return c;
        if (from < 0 || from >= static_cast<int>(c->items.size())) return c;
        std::string item = c->items[static_cast<size_t>(from)];
        c->items.erase(c->items.begin() + from);
        size_t insertAt = static_cast<size_t>(std::max(0, to));
        if (insertAt > c->items.size()) insertAt = c->items.size();
        c->items.insert(c->items.begin() + static_cast<long>(insertAt), item);
        return c;
    }
    SharedPtr<DockNode> f = reorderInTabGroup(node->first, tabId, from, to);
    SharedPtr<DockNode> s = reorderInTabGroup(node->second, tabId, from, to);
    return DockNode::newSplit(node->orientation,
                              f ? f : (node->first ? node->first->cloneNode() : nullptr),
                              s ? s : (node->second ? node->second->cloneNode() : nullptr),
                              node->dividerRatio);
}

const DockNode* DockManager::findTabGroupNode(const SharedPtr<DockNode>& node,
                                              const std::string& id) {
    if (!node) return nullptr;
    if (node->isTabGroup())
        return containsId(node->items, id) && node->items.size() > 1 ? node.get() : nullptr;
    if (const DockNode* r = findTabGroupNode(node->first, id)) return r;
    return findTabGroupNode(node->second, id);
}

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 单例、注册、引用清理
// ═══════════════════════════════════════════════════════════════════════════

DockManager& DockManager::instance() {
    static DockManager m;
    return m;
}

void DockManager::reset() {
    root_.reset();
    hiddenPanels_.clear();
    floatingPanels_.clear();
    maximizedPanelId_.clear();
    savedLayoutBeforeMaximize_.clear();
    expandedPanelId_.clear();
    emitLayoutChanged();
}

void DockManager::registerDockable(Dockable* d) {
    if (d) dockables_[d->persistentID()] = d;
}

void DockManager::unregisterDockable(Dockable* d) {
    if (d) unregisterDockableById(d->persistentID());
}

void DockManager::unregisterDockableById(const std::string& id) {
    dockables_.erase(id);
    removeFromTree(id);
    clearPanelRefs(id);
    if (maximizedPanelId_ == id) {
        maximizedPanelId_.clear();
        savedLayoutBeforeMaximize_.clear();
    }
    emitLayoutChanged();
}

Dockable* DockManager::find(const std::string& id) const {
    auto it = dockables_.find(id);
    return it == dockables_.end() ? nullptr : it->second;
}

bool DockManager::isRegistered(const std::string& id) const {
    return dockables_.count(id) != 0;
}

std::vector<Dockable*> DockManager::allDockables() const {
    std::vector<Dockable*> out;
    out.reserve(dockables_.size());
    for (auto& kv : dockables_) out.push_back(kv.second);
    return out;
}

void DockManager::clearPanelRefs(const std::string& id) {
    for (auto it = hiddenPanels_.begin(); it != hiddenPanels_.end();) {
        auto& gids = it->second.groupIds;
        gids.erase(std::remove(gids.begin(), gids.end(), id), gids.end());
        if (gids.empty() || it->first == id) it = hiddenPanels_.erase(it);
        else ++it;
    }
    for (auto it = floatingPanels_.begin(); it != floatingPanels_.end();) {
        auto& ids = it->second.ids;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        if (it->second.activeId == id)
            it->second.activeId = ids.empty() ? std::string{} : ids.front();
        if (ids.empty() || it->first == id) it = floatingPanels_.erase(it);
        else ++it;
    }
    if (expandedPanelId_ == id) expandedPanelId_.clear();
}

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 停靠（对齐 rui docking.rs L814-948）
// ═══════════════════════════════════════════════════════════════════════════

void DockManager::dock(const std::string& id, DockingRegion region, double ratio) {
    if (!isRegistered(id)) return;
    if (maximizedPanelId_.empty()) {
        removeFromTree(id);
        clearPanelRefs(id);
    } else {
        // 最大化中：用新停靠的面板替换单面板视图。
        root_.reset();
    }

    SharedPtr<DockNode> newTab = DockNode::newTab({id}, id);
    if (!root_) {
        root_ = newTab;
    } else if (region == DockingRegion::Center) {
        if (root_->isTabGroup()) {
            std::vector<std::string> items = root_->items;
            items.push_back(id);
            root_ = DockNode::newTab(items, id);
        } else {
            SharedPtr<DockNode> oldRoot = root_;
            root_ = DockNode::newSplit(SplitOrientation::Horizontal, oldRoot, newTab,
                                       1.0 - ratio);
        }
    } else {
        SharedPtr<DockNode> oldRoot = root_;
        root_ = makeSplitDir(oldRoot, newTab, region, ratio);
    }
    maximizedPanelId_.clear();
    savedLayoutBeforeMaximize_.clear();

    fireEvent(id, DockingEventType::Docked, false);
    emitLayoutChanged();
}

void DockManager::dockRelative(const std::string& sourceId, const std::string& targetId,
                               DockingRegion region, double ratio) {
    if (!isRegistered(sourceId) || !isRegistered(targetId)) return;
    if (sourceId == targetId) return;
    if (!maximizedPanelId_.empty()) restore();  // 先还原，确保 target 在树里
    removeFromTree(sourceId);
    clearPanelRefs(sourceId);
    SharedPtr<DockNode> newTab = DockNode::newTab({sourceId}, sourceId);
    root_ = insertRelative(root_, targetId, newTab, region, ratio);
    fireEvent(sourceId, DockingEventType::Docked, false);
    emitLayoutChanged();
}

void DockManager::undock(const std::string& id) {
    removeFromTree(id);
    clearPanelRefs(id);
    if (maximizedPanelId_ == id) {
        maximizedPanelId_.clear();
        savedLayoutBeforeMaximize_.clear();
    }
    fireEvent(id, DockingEventType::Undocked, false);
    emitLayoutChanged();
}

void DockManager::close(const std::string& id) {
    Dockable* d = find(id);
    if (!d) return;
    if (!(d->isClosable() && d->requestClose())) return;
    fireEvent(id, DockingEventType::Closed, false);
    removeFromTree(id);
    clearPanelRefs(id);
    if (maximizedPanelId_ == id) {
        maximizedPanelId_.clear();
        savedLayoutBeforeMaximize_.clear();
    }
    fireEvent(id, DockingEventType::Undocked, false);
    emitLayoutChanged();
}

void DockManager::bringToFront(const std::string& id) {
    if (!root_) return;
    root_ = activateNode(root_, id);
    emitLayoutChanged();
}

bool DockManager::isDocked(const std::string& id) const {
    return root_ && containsId(root_->collectIds(), id);
}

void DockManager::display(const std::string& id) {
    if (isDocked(id)) bringToFront(id);
    else if (isHidden(id)) restoreFromEdge(id);
    else if (isFloating(id)) bringToFront(id);
    else dock(id, DockingRegion::Center, 0.5);
}

void DockManager::setDividerRatio(const std::string& id, double ratio) {
    if (!root_) return;
    root_ = updateDivider(root_, id, clamp01(ratio));
    emitLayoutChanged();
}

void DockManager::reorderTabItem(const std::string& tabId, int fromIndex, int toIndex) {
    if (!root_) return;
    root_ = reorderInTabGroup(root_, tabId, fromIndex, toIndex);
    emitLayoutChanged();
}

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 浮动（对齐 rui docking.rs L950-1042）
// ═══════════════════════════════════════════════════════════════════════════

bool DockManager::isFloating(const std::string& id) const {
    for (auto& kv : floatingPanels_)
        if (containsId(kv.second.ids, id)) return true;
    return false;
}

std::string DockManager::floatingKeyFor(const std::string& id) const {
    if (floatingPanels_.count(id)) return id;
    for (auto& kv : floatingPanels_)
        if (containsId(kv.second.ids, id)) return kv.first;
    return "";
}

void DockManager::floatPanel(const std::string& id) {
    if (!isRegistered(id) || isFloating(id)) return;
    Dockable* d = find(id);
    if (!d || !d->isFloatingAllowed()) return;
    std::vector<std::string> groupIds = tabGroupOf(id);
    for (const auto& gid : groupIds) {
        Dockable* gd = find(gid);
        if (gd && !gd->isFloatingAllowed()) return;
    }
    SavedTreePosition saved = findTreePosition(id);
    for (const auto& gid : groupIds) removeFromTree(gid);
    std::string key = groupIds.empty() ? id : groupIds.front();
    FloatingPanelInfo info;
    info.ids = groupIds;
    info.activeId = id;
    info.offsetX = 100.0;
    info.offsetY = 100.0;
    info.width = d->floatingWidth() > 0.0 ? d->floatingWidth() : 400.0;
    info.height = d->floatingHeight() > 0.0 ? d->floatingHeight() : 300.0;
    info.savedPosition = saved;
    floatingPanels_[key] = info;
    if (!expandedPanelId_.empty() && containsId(groupIds, expandedPanelId_))
        expandedPanelId_.clear();
    fireEvent(id, DockingEventType::Floated, false);
    emitLayoutChanged();
}

void DockManager::unfloatPanel(const std::string& id) {
    std::string key = floatingKeyFor(id);
    if (key.empty()) return;
    auto it = floatingPanels_.find(key);
    if (it == floatingPanels_.end()) return;
    FloatingPanelInfo info = it->second;
    floatingPanels_.erase(it);
    if (info.ids.empty()) return;
    std::string first = info.ids.front();
    // 与 restoreFromEdge 同口径：优先插回保存的原兄弟旁（尺寸随原分割比例
    // 复原）；兄弟不在树中才退化到根级停靠。
    const SavedTreePosition& sp = info.savedPosition;
    if (sp.valid && !sp.siblingId.empty() && isDocked(sp.siblingId)) {
        dockRelative(first, sp.siblingId, sp.direction, sp.dividerRatio);
    } else {
        DockingRegion dir = sp.valid ? sp.direction : DockingRegion::Center;
        if (dir == DockingRegion::Center) dir = DockingRegion::Left;
        double ratio = sp.valid ? sp.dividerRatio : 0.3;
        dock(first, dir, ratio);
    }
    for (size_t i = 1; i < info.ids.size(); ++i)
        dockRelative(info.ids[i], first, DockingRegion::Center, 0.5);
    fireEvent(id, DockingEventType::Unfloated, false);
    emitLayoutChanged();
}

void DockManager::updateFloatingPosition(const std::string& key, double x, double y) {
    auto it = floatingPanels_.find(key);
    if (it == floatingPanels_.end()) return;
    it->second.offsetX = x;
    it->second.offsetY = y;
    emitLayoutChanged();
}

void DockManager::updateFloatingSize(const std::string& key, double w, double h) {
    auto it = floatingPanels_.find(key);
    if (it == floatingPanels_.end()) return;
    it->second.width = std::max(200.0, w);
    it->second.height = std::max(150.0, h);
    emitLayoutChanged();
}

void DockManager::switchFloatingTab(const std::string& key, const std::string& activeId) {
    auto it = floatingPanels_.find(key);
    if (it == floatingPanels_.end()) return;
    it->second.activeId = activeId;
    emitLayoutChanged();
}

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 自动隐藏（对齐 rui docking.rs L1044-1149）
// ═══════════════════════════════════════════════════════════════════════════

bool DockManager::isHidden(const std::string& id) const {
    for (auto& kv : hiddenPanels_)
        if (containsId(kv.second.groupIds, id)) return true;
    return false;
}

std::string DockManager::hiddenKeyFor(const std::string& id) const {
    if (hiddenPanels_.count(id)) return id;
    for (auto& kv : hiddenPanels_)
        if (containsId(kv.second.groupIds, id)) return kv.first;
    return "";
}

void DockManager::autoHide(const std::string& id, DockEdge edge) {
    if (!isRegistered(id)) return;
    std::vector<std::string> groupIds = tabGroupOf(id);
    SavedTreePosition saved = findTreePosition(id);
    for (const auto& gid : groupIds) removeFromTree(gid);
    // 清浮动/旧隐藏引用：拖拽落点路径上面板可能仍在 floatingPanels 里。
    clearPanelRefs(id);
    std::string key = groupIds.empty() ? id : groupIds.front();
    HiddenPanelInfo info;
    info.edge = edge;
    info.savedPosition = saved;
    info.groupIds = groupIds;
    hiddenPanels_[key] = info;
    if (!expandedPanelId_.empty() && containsId(groupIds, expandedPanelId_))
        expandedPanelId_.clear();
    fireEvent(id, DockingEventType::AutoHidden, false);
    emitLayoutChanged();
}

void DockManager::hideToEdge(const std::string& id) {
    // 已作为键直接隐藏过的面板不重复入表（对齐 rui hide_to_edge 的守卫）。
    if (!isRegistered(id) || hiddenPanels_.count(id)) return;
    std::vector<std::string> groupIds = tabGroupOf(id);
    // 把单独隐藏的组员并入本组。
    std::vector<std::string> allHidden;
    for (const auto& g : groupIds)
        if (hiddenPanels_.count(g)) allHidden.push_back(g);
    for (const auto& h : allHidden) hiddenPanels_.erase(h);
    std::vector<std::string> fullGroup = groupIds;
    for (const auto& h : allHidden)
        if (!containsId(fullGroup, h)) fullGroup.push_back(h);
    DockEdge edge = findEdgeOfPanel(id);
    SavedTreePosition saved = findTreePosition(id);
    for (const auto& gid : groupIds) removeFromTree(gid);
    std::string key = fullGroup.empty() ? id : fullGroup.front();
    bool wasExpanded = !expandedPanelId_.empty() && containsId(fullGroup, expandedPanelId_);
    HiddenPanelInfo info;
    info.edge = edge;
    info.savedPosition = saved;
    info.groupIds = fullGroup;
    hiddenPanels_[key] = info;
    if (wasExpanded) expandedPanelId_.clear();
    fireEvent(id, DockingEventType::AutoHidden, false);
    emitLayoutChanged();
}

void DockManager::restoreFromEdge(const std::string& id) {
    std::string key = hiddenKeyFor(id);
    if (key.empty()) return;
    auto it = hiddenPanels_.find(key);
    if (it == hiddenPanels_.end()) return;
    HiddenPanelInfo info = it->second;
    hiddenPanels_.erase(it);
    std::vector<std::string> groupIds =
        info.groupIds.empty() ? std::vector<std::string>{id} : info.groupIds;
    std::string first = groupIds.front();
    // 优先按保存的原树位置恢复：direction/ratio 都是相对兄弟的语义
    // （见 findTreePosInNode），比例不变 ⇒ 面板尺寸与隐藏前完全一致。
    const SavedTreePosition& sp = info.savedPosition;
    if (sp.valid && !sp.siblingId.empty() && isDocked(sp.siblingId)) {
        dockRelative(first, sp.siblingId, sp.direction, sp.dividerRatio);
    } else {
        DockingRegion dir = edgeToRegion(info.edge);
        double ratio = sp.valid ? sp.dividerRatio : 0.2;
        dock(first, dir, ratio);
    }
    for (size_t i = 1; i < groupIds.size(); ++i)
        dockRelative(groupIds[i], first, DockingRegion::Center, 0.5);
    if (!expandedPanelId_.empty() && containsId(groupIds, expandedPanelId_))
        expandedPanelId_.clear();
    fireEvent(id, DockingEventType::AutoShown, false);
    emitLayoutChanged();
}

void DockManager::toggleAutoHide(const std::string& id) {
    if (isHidden(id)) restoreFromEdge(id);
    else hideToEdge(id);
}

void DockManager::expandFromEdge(const std::string& id) {
    if (hiddenPanels_.count(id) || isHidden(id)) expandedPanelId_ = id;
    emitLayoutChanged();
}

void DockManager::collapseToEdge() {
    expandedPanelId_.clear();
    emitLayoutChanged();
}

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 最大化（对齐 rui docking.rs L1151-1183）
//
// 最大化把整棵树换成「只有这一个面板的标签组」，原树先存成字符串快照，
// 于是渲染器不需要任何特例分支（CUI 旧版靠 maximized_ 标志特例，已废弃）。
// ═══════════════════════════════════════════════════════════════════════════

void DockManager::maximize(const std::string& id) {
    if (!isRegistered(id) || !maximizedPanelId_.empty()) return;
    savedLayoutBeforeMaximize_ = saveLayoutState();
    root_ = DockNode::newTab({id}, id);
    maximizedPanelId_ = id;
    expandedPanelId_.clear();
    fireEvent(id, DockingEventType::Maximized, false);
    emitLayoutChanged();
}

void DockManager::restore() {
    if (maximizedPanelId_.empty()) return;
    std::string id = maximizedPanelId_;
    if (!savedLayoutBeforeMaximize_.empty()) restoreLayoutState(savedLayoutBeforeMaximize_);
    maximizedPanelId_.clear();
    savedLayoutBeforeMaximize_.clear();
    fireEvent(id, DockingEventType::Restored, false);
    emitLayoutChanged();
}

void DockManager::toggleMaximize(const std::string& id) {
    if (maximizedPanelId_ == id) restore();
    else if (maximizedPanelId_.empty()) maximize(id);
}

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 查询与手势（对齐 rui docking.rs L1185-1259）
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::string> DockManager::tabGroupOf(const std::string& id) const {
    if (const DockNode* g = findTabGroupNode(root_, id)) return g->items;
    return {id};
}

SavedTreePosition DockManager::findTreePosition(const std::string& id) const {
    SavedTreePosition out;
    findTreePosInNode(root_, id, out);
    return out;
}

DockEdge DockManager::findEdgeOfPanel(const std::string& id) const {
    Rect r{0, 0, 0, 0};
    if (root_ && findNodeRectVirtual(root_, id, 0.0, 0.0, 1000.0, 800.0, r)) {
        double cx = r.x + r.width / 2.0;
        double cy = r.y + r.height / 2.0;
        double distLeft = cx, distRight = 1000.0 - cx;
        double distTop = cy, distBottom = 800.0 - cy;
        double minH = std::min(distLeft, distRight);
        double minV = std::min(distTop, distBottom);
        if (minH < minV) return distLeft < distRight ? DockEdge::Left : DockEdge::Right;
        return distTop < distBottom ? DockEdge::Top : DockEdge::Bottom;
    }
    return DockEdge::Left;
}

void DockManager::setSplitRatioByPath(const std::vector<int>& path, double ratio) {
    DockNode* node = root_.get();
    if (!node) return;
    for (int step : path) {
        if (!node->isSplit()) return;
        node = (step == 0 ? node->first : node->second).get();
        if (!node) return;
    }
    if (node->isSplit()) {
        node->dividerRatio = clamp01(ratio);
        emitLayoutChanged();
    }
}

std::string DockManager::activeTabOfGroup(const std::string& groupId) const {
    if (!root_) return "";
    std::function<const DockNode*(const SharedPtr<DockNode>&)> walk =
        [&](const SharedPtr<DockNode>& n) -> const DockNode* {
            if (!n) return nullptr;
            if (n->isTabGroup()) return n->groupId() == groupId ? n.get() : nullptr;
            if (const DockNode* f = walk(n->first)) return f;
            return walk(n->second);
        };
    const DockNode* g = walk(root_);
    if (!g || g->items.empty()) return "";
    if (containsId(g->items, g->activeId)) return g->activeId;
    return g->items.front();
}

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 序列化（对齐 rui docking.rs L1261-1407）
// ═══════════════════════════════════════════════════════════════════════════

std::string DockManager::saveLayoutState() const {
    std::string out;
    serializeNode(root_, out);
    return out;
}

bool DockManager::restoreLayoutState(const std::string& state) {
    std::string s = trim(state);
    if (s.empty()) return false;
    size_t pos = 0;
    SharedPtr<DockNode> parsed = parseNode(s, pos, *this);
    if (!parsed) return false;
    root_ = parsed;
    maximizedPanelId_.clear();
    savedLayoutBeforeMaximize_.clear();
    expandedPanelId_.clear();
    emitLayoutChanged();
    return true;
}

std::string DockManager::saveFullState() const {
    std::string os = "mdl1\n";
    os += "tree=";
    os += saveLayoutState();
    os += "\n";
    os += "max=";
    os += maximizedPanelId_;
    os += "\n";
    os += "savedMax=";
    os += savedLayoutBeforeMaximize_;
    os += "\n";
    for (const auto& kv : hiddenPanels_) {
        const HiddenPanelInfo& info = kv.second;
        os += "hidden=";
        os += kv.first;
        os += "|" + std::to_string(edgeToInt(info.edge));
        os += "|" + info.savedPosition.siblingId;
        os += "|" + std::to_string(regionToInt(info.savedPosition.direction));
        os += "|" + numStr(info.savedPosition.dividerRatio);
        os += "|" + std::string(info.savedPosition.valid ? "1" : "0");
        os += "|" + joinCsv(info.groupIds);
        os += "\n";
    }
    for (const auto& kv : floatingPanels_) {
        const FloatingPanelInfo& info = kv.second;
        os += "float=";
        os += kv.first;
        os += "|" + info.activeId;
        os += "|" + numStr(info.offsetX);
        os += "|" + numStr(info.offsetY);
        os += "|" + numStr(info.width);
        os += "|" + numStr(info.height);
        os += "|" + info.savedPosition.siblingId;
        os += "|" + std::to_string(regionToInt(info.savedPosition.direction));
        os += "|" + numStr(info.savedPosition.dividerRatio);
        os += "|" + std::string(info.savedPosition.valid ? "1" : "0");
        os += "|" + joinCsv(info.ids);
        os += "\n";
    }
    return os;
}

bool DockManager::restoreFullState(const std::string& state) {
    std::string trimmed = trim(state);
    if (trimmed.empty()) return false;
    std::vector<std::string> lines = linesOf(trimmed);
    if (lines.empty() || trim(lines[0]) != "mdl1") return false;

    std::string treeStr, maxStr, savedMax;
    std::vector<std::pair<std::string, HiddenPanelInfo>> hidden;
    std::vector<std::pair<std::string, FloatingPanelInfo>> floating;

    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "tree") {
            treeStr = v;
        } else if (k == "max") {
            maxStr = v;
        } else if (k == "savedMax") {
            savedMax = v;
        } else if (k == "hidden") {
            std::vector<std::string> f = splitTopLevel(v, '|');
            HiddenPanelInfo info;
            if (f.size() >= 2) info.edge = intToEdge(parseIntOr(f[1], 0));
            if (f.size() >= 3) info.savedPosition.siblingId = f[2];
            if (f.size() >= 4) info.savedPosition.direction = intToRegion(parseIntOr(f[3], 0));
            if (f.size() >= 5) info.savedPosition.dividerRatio = parseDoubleOr(f[4], 0.5);
            if (f.size() >= 6) info.savedPosition.valid = f[5] == "1";
            if (f.size() >= 7) info.groupIds = splitCsv(f[6]);
            hidden.emplace_back(f.empty() ? std::string{} : f[0], info);
        } else if (k == "float") {
            std::vector<std::string> f = splitTopLevel(v, '|');
            FloatingPanelInfo info;
            if (f.size() >= 2) info.activeId = f[1];
            if (f.size() >= 3) info.offsetX = parseDoubleOr(f[2], 100.0);
            if (f.size() >= 4) info.offsetY = parseDoubleOr(f[3], 100.0);
            if (f.size() >= 5) info.width = parseDoubleOr(f[4], 400.0);
            if (f.size() >= 6) info.height = parseDoubleOr(f[5], 300.0);
            if (f.size() >= 7) info.savedPosition.siblingId = f[6];
            if (f.size() >= 8) info.savedPosition.direction = intToRegion(parseIntOr(f[7], 0));
            if (f.size() >= 9) info.savedPosition.dividerRatio = parseDoubleOr(f[8], 0.5);
            if (f.size() >= 10) info.savedPosition.valid = f[9] == "1";
            if (f.size() >= 11) info.ids = splitCsv(f[10]);
            floating.emplace_back(f.empty() ? std::string{} : f[0], std::move(info));
        }
    }

    // 先重建树（未注册的面板在 parse 阶段就被过滤掉）。
    root_.reset();
    if (!treeStr.empty()) {
        size_t pos = 0;
        root_ = parseNode(treeStr, pos, *this);
    }
    hiddenPanels_.clear();
    for (auto& kv : hidden) hiddenPanels_[kv.first] = std::move(kv.second);
    floatingPanels_.clear();
    for (auto& kv : floating) floatingPanels_[kv.first] = std::move(kv.second);
    maximizedPanelId_ = maxStr;
    savedLayoutBeforeMaximize_ = savedMax;
    expandedPanelId_.clear();
    emitLayoutChanged();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// DockManager — 事件、重绘与自动持久化（对齐 rui docking.rs L1420-1470）
// ═══════════════════════════════════════════════════════════════════════════

void DockManager::fireEvent(const std::string& dockableId, DockingEventType type, bool temporary) {
    DockingEvent ev{dockableId, type, temporary};
    auto snapshot = eventListeners_;  // 回调内可增删监听器
    for (const auto& l : snapshot)
        l.fn(ev);
    if (type == DockingEventType::Maximized || type == DockingEventType::Restored) {
        bool maximized = type == DockingEventType::Maximized;
        auto snap2 = maximizeListeners_;
        for (const auto& l : snap2)
            l.fn(dockableId, maximized);
    }
}

void DockManager::emitLayoutChanged() {
    auto snapshot = layoutListeners_;
    for (const auto& l : snapshot)
        l.fn();
    // 单一管线：State::setState 只标脏，dock 变化后要主动请求一帧。
    if (auto* app = Application::instance())
        app->scheduleRender();
    // 自动持久化（对齐 rui 的 layout_hook）：树还空着时不写盘。
    if (!persistEnabled_ || persistFile_.empty())
        return;
    std::string content = saveFullState();
    if (content.find("tree=\n") != std::string::npos)
        return;
    std::ofstream out(persistFile_, std::ios::binary | std::ios::trunc);
    if (out) out << content;
}

void DockManager::refresh() { emitLayoutChanged(); }

int DockManager::addListener(LayoutCallback cb) {
    int h = nextHandle();
    layoutListeners_.push_back(HandleEntry{h, std::move(cb)});
    return h;
}

void DockManager::removeListener(int handle) {
    for (auto it = layoutListeners_.begin(); it != layoutListeners_.end(); ++it) {
        if (it->handle == handle) {
            layoutListeners_.erase(it);
            return;
        }
    }
}

int DockManager::addDockingListener(DockingEventCallback cb) {
    int h = nextHandle();
    eventListeners_.push_back(EventEntry{h, std::move(cb)});
    return h;
}

void DockManager::removeDockingListener(int handle) {
    for (auto it = eventListeners_.begin(); it != eventListeners_.end(); ++it) {
        if (it->handle == handle) {
            eventListeners_.erase(it);
            return;
        }
    }
}

int DockManager::addMaximizeListener(MaximizeCallback cb) {
    int h = nextHandle();
    maximizeListeners_.push_back(MaximizeEntry{h, std::move(cb)});
    return h;
}

void DockManager::removeMaximizeListener(int handle) {
    for (auto it = maximizeListeners_.begin(); it != maximizeListeners_.end(); ++it) {
        if (it->handle == handle) {
            maximizeListeners_.erase(it);
            return;
        }
    }
}

void DockManager::setPersist(bool enabled) { persistEnabled_ = enabled; }
bool DockManager::isPersistEnabled() const { return persistEnabled_; }
void DockManager::setPersistFile(const std::string& path) { persistFile_ = path; }
const std::string& DockManager::persistFile() const { return persistFile_; }

// ═══════════════════════════════════════════════════════════════════════════
// WindowLayoutBuilder — 只描述操作序列，建树交给 DockManager
// ═══════════════════════════════════════════════════════════════════════════

WindowLayoutBuilder& WindowLayoutBuilder::dock(const std::string& sourceId,
                                              DockingRegion region) {
    entries_.push_back(LayoutEntry{sourceId, "", region, 0.25});
    return *this;
}

WindowLayoutBuilder& WindowLayoutBuilder::dock(const std::string& sourceId,
                                              const std::string& targetId,
                                              DockingRegion region, double proportion) {
    return dockRelative(sourceId, targetId, region, proportion);
}

WindowLayoutBuilder& WindowLayoutBuilder::dockRelative(const std::string& sourceId,
                                                      const std::string& targetId,
                                                      DockingRegion region,
                                                      double proportion) {
    entries_.push_back(LayoutEntry{sourceId, targetId, region, proportion});
    return *this;
}

WindowLayoutBuilder& WindowLayoutBuilder::setDividerProportion(double proportion) {
    if (!entries_.empty())
        entries_.back().proportion = proportion;
    return *this;
}

// ═══════════════════════════════════════════════════════════════════════════
// DockingSettings
// ═══════════════════════════════════════════════════════════════════════════

DockingSettings& DockingSettings::instance() {
    static DockingSettings s;
    return s;
}

// ═══════════════════════════════════════════════════════════════════════════
// Docking — 静态门面（对齐 rui docking.rs L1572-1968）
// ═══════════════════════════════════════════════════════════════════════════

Docking::FacadeState& Docking::facade() {
    static FacadeState f;
    return f;
}

void Docking::initialize() {
    FacadeState& f = facade();
    if (f.initialized) return;
    f.initialized = true;
    if (f.defaultLayout) applyLayout(*f.defaultLayout);
}

void Docking::applyLayout(const WindowLayoutBuilder& b) {
    DockManager& mgr = DockManager::instance();
    mgr.reset();
    for (const LayoutEntry& e : b.entries()) {
        if (e.targetId.empty() || !mgr.isRegistered(e.targetId))
            mgr.dock(e.sourceId, e.region, e.proportion);
        else
            mgr.dockRelative(e.sourceId, e.targetId, e.region, e.proportion);
    }
}

void Docking::setDefaultApplicationLayout(const WindowLayoutBuilder& b) {
    facade().defaultLayout = std::make_shared<WindowLayoutBuilder>(b);
}

void Docking::resetLayout() {
    if (facade().defaultLayout) applyLayout(*facade().defaultLayout);
    else DockManager::instance().reset();
}

void Docking::registerDockable(Dockable* d) { DockManager::instance().registerDockable(d); }
void Docking::unregisterDockable(Dockable* d) { DockManager::instance().unregisterDockable(d); }
Dockable* Docking::findDockable(const std::string& id) { return DockManager::instance().find(id); }
std::vector<Dockable*> Docking::allDockables() { return DockManager::instance().allDockables(); }

void Docking::dock(const std::string& id, DockingRegion region, double ratio) {
    DockManager::instance().dock(id, region, ratio);
}
void Docking::dockRelative(const std::string& sourceId, const std::string& targetId,
                           DockingRegion region, double proportion) {
    DockManager::instance().dockRelative(sourceId, targetId, region, proportion);
}
void Docking::undock(const std::string& id) { DockManager::instance().undock(id); }
void Docking::close(const std::string& id) { DockManager::instance().close(id); }
void Docking::bringToFront(const std::string& id) { DockManager::instance().bringToFront(id); }
void Docking::display(const std::string& id) { DockManager::instance().display(id); }
void Docking::hide(const std::string& id) { DockManager::instance().hideToEdge(id); }
void Docking::reorderTabItem(const std::string& tabId, int fromIndex, int toIndex) {
    DockManager::instance().reorderTabItem(tabId, fromIndex, toIndex);
}
void Docking::setDividerRatio(const std::string& id, double ratio) {
    DockManager::instance().setDividerRatio(id, ratio);
}

bool Docking::isDocked(const std::string& id) { return DockManager::instance().isDocked(id); }
bool Docking::isHidden(const std::string& id) { return DockManager::instance().isHidden(id); }
bool Docking::isFloating(const std::string& id) { return DockManager::instance().isFloating(id); }
bool Docking::isMaximizedById(const std::string& id) {
    return DockManager::instance().isMaximized(id);
}
bool Docking::isDockableRegistered(const std::string& id) {
    return DockManager::instance().isRegistered(id);
}

void Docking::floatPanel(const std::string& id) { DockManager::instance().floatPanel(id); }
void Docking::unfloatPanel(const std::string& id) { DockManager::instance().unfloatPanel(id); }
void Docking::switchFloatingTab(const std::string& key, const std::string& activeId) {
    DockManager::instance().switchFloatingTab(key, activeId);
}
void Docking::updateFloatingPosition(const std::string& key, double x, double y) {
    DockManager::instance().updateFloatingPosition(key, x, y);
}
void Docking::updateFloatingSize(const std::string& key, double w, double h) {
    DockManager::instance().updateFloatingSize(key, w, h);
}

void Docking::autoHide(const std::string& id, DockEdge e) {
    DockManager::instance().autoHide(id, e);
}
void Docking::autoHide(Dockable* d, DockEdge e) {
    if (d) autoHide(d->persistentID(), e);
}
void Docking::hideToEdge(const std::string& id) { DockManager::instance().hideToEdge(id); }
void Docking::restoreFromEdge(const std::string& id) {
    DockManager::instance().restoreFromEdge(id);
}
void Docking::toggleAutoHide(const std::string& id) { DockManager::instance().toggleAutoHide(id); }
void Docking::autoShow(const std::string& id) { DockManager::instance().expandFromEdge(id); }
void Docking::collapseToEdge() { DockManager::instance().collapseToEdge(); }

void Docking::maximize(const std::string& id) { DockManager::instance().maximize(id); }
void Docking::maximize(Dockable* d) {
    if (d) maximize(d->persistentID());
}
void Docking::restore() { DockManager::instance().restore(); }
void Docking::restore(Dockable* d) {
    if (d && DockManager::instance().isMaximized(d->persistentID())) restore();
}
bool Docking::isMaximized(Dockable* d) {
    return d && DockManager::instance().isMaximized(d->persistentID());
}
void Docking::toggleMaximize(const std::string& id) { DockManager::instance().toggleMaximize(id); }

std::string Docking::saveLayout() { return DockManager::instance().saveFullState(); }
bool Docking::loadLayout(const std::string& state) {
    return DockManager::instance().restoreFullState(state);
}

bool Docking::saveLayoutToFile(const std::string& path) {
    std::string content = saveLayout();
    if (content.find("tree=\n") != std::string::npos) return false;  // 树还空着，别写
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return true;
}

bool Docking::loadLayoutFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream buf;
    buf << in.rdbuf();
    return loadLayout(buf.str());
}

bool Docking::saveNamedLayout(const std::string& name) {
    std::string state = DockManager::instance().saveFullState();
    FacadeState& f = facade();
    for (auto& entry : f.namedLayouts) {
        if (entry.first == name) {
            entry.second = state;
            return true;
        }
    }
    f.namedLayouts.emplace_back(name, state);
    return true;
}

bool Docking::restoreNamedLayout(const std::string& name) {
    std::string state;
    bool found = false;
    for (const auto& entry : facade().namedLayouts) {
        if (entry.first == name) {
            state = entry.second;
            found = true;
            break;
        }
    }
    return found && loadLayout(state);
}

std::vector<std::string> Docking::layoutNames() {
    std::vector<std::string> out;
    for (const auto& entry : facade().namedLayouts) out.push_back(entry.first);
    return out;
}

void Docking::setPersist(bool enabled) { DockManager::instance().setPersist(enabled); }
bool Docking::isPersistEnabled() { return DockManager::instance().isPersistEnabled(); }
void Docking::setPersistFile(const std::string& path) {
    DockManager::instance().setPersistFile(path);
}

void Docking::repaint() { DockManager::instance().refresh(); }

void Docking::updateTabText(const std::string& id) {
    if (DockManager::instance().isRegistered(id)) DockManager::instance().refresh();
}

std::string Docking::dockableMenuLabel(const std::string& id) {
    Dockable* d = DockManager::instance().find(id);
    return d ? d->tabText() : id;
}

std::function<void()> Docking::dockableMenuAction(const std::string& id) {
    return [id] {
        if (isDocked(id) || isFloating(id)) close(id);
        else display(id);
    };
}

std::vector<std::pair<std::string, std::function<void()>>> Docking::layoutsMenuActions() {
    std::vector<std::pair<std::string, std::function<void()>>> out;
    for (const auto& entry : facade().namedLayouts) {
        std::string name = entry.first;
        out.emplace_back(name, [name] { restoreNamedLayout(name); });
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// 渲染（批次 2：面板本地坐标精确排布 + CustomPaint chrome）
//
// 几何纯函数逐条对齐 rui docking.rs：split_child_rects(L2295)、group_areas
// (L2278)、tab_width/fit_text(L2311-2337)、layout_tab_rects(L2380)、
// header_buttons(L2429)、paint_header_buttons(L2478)、paint_tab_group(L3072)。
//
// 与 rui 的两条结构差异（不是行为差异）：
//   1. rui 由一个 widget 自绘整棵树；本仓先算出每个标签组的矩形，再把它们排成
//      Stack 的 Positioned 层。chrome（渐变、活动标签的主色条、按钮图标、
//      分隔线）仍走 CustomPaint，文字仍是真正的 Text —— 本仓的文本度量与光栅
//      只在 RenderParagraph 里，自绘文字拿不到 CJK 字体回退。
//   2. 分割条不单独绘制：缝隙露出面板 background（rui paint() 只填 background，
//      同样不画分割条）。
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// ── 尺寸常量（rui 的 K_*）──────────────────────────────────────────────────
constexpr double kTabFont = 12.0;  ///< K_TAB_FONT
constexpr double kTabPad = 9.0;    ///< K_TAB_PAD
constexpr double kTabMinW = 48.0;  ///< K_TAB_MIN_W
constexpr double kTabGap = 2.0;    ///< K_TAB_GAP
constexpr double kBtn = 22.0;      ///< K_BTN
constexpr const char kEllipsis[] = "\xe2\x80\xa6";

const Color kWhite = Color::fromRGB(1.0, 1.0, 1.0);
const Color kBlack = Color::fromRGB(0.0, 0.0, 0.0);

Color mixColor(const Color& a, const Color& b, double t) {
    return Color::fromRGBA(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                           a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

double tabTextWidth(const std::string& text) {
    return Painter::measureTextWidth(nullptr, text, kTabFont);
}

/// UTF-8 拆码点：截断不能把多字节字符切成半个（乱码比省略号难查）。
std::vector<std::string> utf8Codepoints(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (i + len > s.size()) len = 1;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

/// 按最大宽度截断并补省略号（对齐 rui fit_text）。
std::string fitText(const std::string& text, double maxW) {
    if (maxW <= 0.0) return {};
    if (tabTextWidth(text) <= maxW) return text;
    double w = tabTextWidth(kEllipsis);
    std::string out;
    for (const auto& ch : utf8Codepoints(text)) {
        const double cw = tabTextWidth(ch);
        if (w + cw > maxW) break;
        w += cw;
        out += ch;
    }
    return out + kEllipsis;
}

double tabWidthOf(const std::string& text) {
    return std::max(tabTextWidth(text) + kTabPad * 2.0, kTabMinW);
}

std::string tabLabelOf(const std::string& id) {
    Dockable* d = DockManager::instance().find(id);
    return d ? d->tabText() : id;
}

std::string titleLabelOf(const std::string& id) {
    Dockable* d = DockManager::instance().find(id);
    return d ? d->titleText() : id;
}

/// ── 几何 ──────────────────────────────────────────────────────────────────

/// Split 两子矩形：各让出半个分割条（与 DockManager::computeSplitBounds 同构）。
std::pair<Rect, Rect> splitChildRects(const DockNode& node, const Rect& rect) {
    const double sp = DockingSettings::instance().splitGap;
    const double r = clamp01(node.dividerRatio);
    if (node.orientation == SplitOrientation::Horizontal) {
        const double fw = std::max(0.0, rect.width * r - sp / 2.0);
        const double sw = std::max(0.0, rect.width * (1.0 - r) - sp / 2.0);
        return {Rect{rect.x, rect.y, fw, rect.height},
                Rect{rect.x + fw + sp, rect.y, sw, rect.height}};
    }
    const double fh = std::max(0.0, rect.height * r - sp / 2.0);
    const double sh = std::max(0.0, rect.height * (1.0 - r) - sp / 2.0);
    return {Rect{rect.x, rect.y, rect.width, fh},
            Rect{rect.x, rect.y + fh + sp, rect.width, sh}};
}

struct GroupAreas {
    Rect header;   ///< 标签条 / 单面板标题栏
    Rect content;  ///< 面板内容区
};

/// TabMode 决定头部在组的顶边还是底边（默认 Bottom = VS 风格）。
GroupAreas groupAreas(const Rect& rect) {
    const double hh = std::min(DockingSettings::instance().headerHeight, rect.height);
    const bool top = DockingSettings::instance().tabMode == TabMode::Top;
    const Rect header{rect.x, rect.y + (top ? 0.0 : rect.height - hh), rect.width, hh};
    const Rect content{rect.x, rect.y + (top ? hh : 0.0), rect.width,
                       std::max(0.0, rect.height - hh)};
    return {header, content};
}

struct TabRect {
    std::string id;
    Rect rect;
    double reserved = 0.0;  ///< 活动标签右缘给 × 让出的宽度
};

bool closableTabOf(const std::string& id, const std::string& activeId) {
    if (id != activeId) return false;
    Dockable* d = DockManager::instance().find(id);
    return d && d->isClosable();
}

/// 标签条内各标签矩形（paint 与命中共用同一度量 ⇒ 视觉与命中一致）。
///
/// 总宽超出头部时均摊压缩文字段（× 预留与最小宽度不参与压缩），并为右端的
/// pin/max 预留空间 —— tab 条不得压过按钮或画出头部右缘。
std::vector<TabRect> layoutTabRects(const std::vector<std::string>& items,
                                    const std::string& activeId, const Rect& header) {
    std::vector<TabRect> out;
    const size_t n = items.size();
    if (n == 0) return out;
    const double th = std::max(0.0, header.height - 4.0);
    const double ty = header.y + (header.height - th) / 2.0;

    std::vector<double> widths;
    widths.reserve(n);
    for (const auto& id : items) {
        double tw = tabWidthOf(tabLabelOf(id));
        if (closableTabOf(id, activeId)) tw += kBtn + 2.0;
        widths.push_back(tw);
    }

    double reserve = 2.0;
    if (n > 1) {
        Dockable* d = DockManager::instance().find(activeId);
        if (d && d->isAutoHideAllowed()) reserve += kBtn;
        if (d && d->isMinMaxAllowed()) reserve += kBtn;
    }
    const double avail = std::max(0.0, header.width - 4.0 - reserve);
    double sum = 0.0;
    for (double w : widths) sum += w;
    const double overflow = sum + kTabGap * static_cast<double>(n - 1) - avail;
    if (overflow > 0.0) {
        const double per = overflow / static_cast<double>(n);
        for (size_t i = 0; i < n; ++i) {
            const double minW = kTabMinW + (closableTabOf(items[i], activeId) ? kBtn + 2.0 : 0.0);
            widths[i] = std::max(widths[i] - per, minW);
        }
    }

    double x = header.x + 4.0;
    for (size_t i = 0; i < n; ++i) {
        TabRect t;
        t.id = items[i];
        t.rect = Rect{x, ty, widths[i], th};
        t.reserved = closableTabOf(items[i], activeId) ? kBtn + 2.0 : 0.0;
        out.push_back(std::move(t));
        x += widths[i] + kTabGap;
    }
    return out;
}

/// 标题栏按钮矩形（未给 = 不显示）。
struct ChromeButtons {
    Rect close, pin, max;
    bool hasClose = false, hasPin = false, hasMax = false;
};

/// tabbed：× 贴活动标签右缘，右起 [pin][max]；
/// 单面板：右起 [max][pin][close]，且最大化中 max 恒在（还原出口）。
ChromeButtons layoutHeaderButtons(const std::string& activeId, const Rect& header,
                                  bool tabbed, const std::vector<TabRect>& tabs) {
    ChromeButtons b;
    if (activeId.empty()) return b;
    Dockable* d = DockManager::instance().find(activeId);
    auto allowed = [&](bool (Dockable::*test)() const) { return d && (d->*test)(); };
    const double btnH = std::max(0.0, header.height - 8.0);
    const double by = header.y + 4.0;

    auto take = [&b, btnH, by](Rect& slot, bool& flag, double& bx) {
        bx -= kBtn;
        slot = Rect{bx, by, kBtn, btnH};
        flag = true;
    };

    if (tabbed) {
        for (const auto& t : tabs)
            if (t.id == activeId && allowed(&Dockable::isClosable)) {
                b.close = Rect{t.rect.x + t.rect.width - kBtn - 2.0, by, kBtn, btnH};
                b.hasClose = true;
            }
        double bx = header.x + header.width - 4.0;
        if (allowed(&Dockable::isAutoHideAllowed)) take(b.pin, b.hasPin, bx);
        if (allowed(&Dockable::isMinMaxAllowed)) take(b.max, b.hasMax, bx);
    } else {
        double bx = header.x + header.width - 4.0;
        if (allowed(&Dockable::isClosable)) take(b.close, b.hasClose, bx);
        if (allowed(&Dockable::isAutoHideAllowed)) take(b.pin, b.hasPin, bx);
        const bool maximized = DockManager::instance().maximized() == activeId;
        if (allowed(&Dockable::isMinMaxAllowed) || maximized) take(b.max, b.hasMax, bx);
    }
    return b;
}

/// ── 绘制 ──────────────────────────────────────────────────────────────────

using ChromePaintFn = RenderCustomPaint::PainterCallback;

void paintButtonGlyphs(Painter& p, const ChromeButtons& b) {
    const Theme& t = theme();
    p.setSourceColor(t.textSecondary);
    if (b.hasMax) {
        const double cx = b.max.x + b.max.width / 2.0;
        const double cy = b.max.y + b.max.height / 2.0;
        p.setLineWidth(1.2);
        p.rectangle(Rect{cx - 4.5, cy - 4.5, 9.0, 9.0});
        p.stroke();
    }
    if (b.hasPin) {
        const double cx = b.pin.x + b.pin.width / 2.0;
        const double cy = b.pin.y + b.pin.height / 2.0;
        p.setLineWidth(1.4);
        p.moveTo(cx, cy - 5.0);
        p.lineTo(cx, cy + 5.0);
        p.stroke();
        p.moveTo(cx - 3.5, cy - 2.0);
        p.lineTo(cx + 3.5, cy - 2.0);
        p.stroke();
    }
    if (b.hasClose) {
        const double cx = b.close.x + b.close.width / 2.0;
        const double cy = b.close.y + b.close.height / 2.0;
        p.setLineWidth(1.4);
        p.moveTo(cx - 4.0, cy - 4.0);
        p.lineTo(cx + 4.0, cy + 4.0);
        p.stroke();
        p.moveTo(cx + 4.0, cy - 4.0);
        p.lineTo(cx - 4.0, cy + 4.0);
        p.stroke();
    }
}

/// 多标签头部：纵向微渐变（顶亮底暗）+ 与内容区的分隔线 + 活动标签三层 + 按钮。
ChromePaintFn tabStripPainter(std::vector<TabRect> tabs, std::string activeId,
                              ChromeButtons buttons, bool topMode) {
    return [tabs = std::move(tabs), activeId = std::move(activeId), buttons,
            topMode](Painter& p, const Size& s) {
        const Theme& t = theme();
        const Rect header{0.0, 0.0, s.width, s.height};
        p.setLinearGradient(Point{header.x, header.y}, Point{header.x, header.bottom()},
                            mixColor(t.background, kWhite, 0.05),
                            mixColor(t.background, kBlack, 0.03));
        p.rectangle(header);
        p.fill();
        p.resetGradient();

        p.setSourceColor(t.border);
        p.setLineWidth(1.0);
        const double sepY = topMode ? header.bottom() : header.y;
        p.moveTo(header.x, sepY);
        p.lineTo(header.right(), sepY);
        p.stroke();

        for (const auto& tab : tabs) {
            if (tab.id != activeId) continue;
            p.setSourceColor(t.surface);
            p.rectangle(tab.rect);
            p.fill();
            p.setSourceColor(Color::fromRGBA(1.0, 1.0, 1.0, 0.35));
            p.rectangle(Rect{tab.rect.x, tab.rect.y, tab.rect.width, 1.0});
            p.fill();
            // 主色条压在朝向内容区的那一边。
            const double uy = topMode ? tab.rect.bottom() - 2.0 : tab.rect.y;
            p.setSourceColor(t.primary);
            p.rectangle(Rect{tab.rect.x, uy, tab.rect.width, 2.0});
            p.fill();
        }
        paintButtonGlyphs(p, buttons);
    };
}

/// 单面板标题栏：surfaceVariant 微渐变 + 顶部受光亮线 + 底部边线 + 按钮。
ChromePaintFn titleBarPainter(ChromeButtons buttons) {
    return [buttons](Painter& p, const Size& s) {
        const Theme& t = theme();
        const Rect header{0.0, 0.0, s.width, s.height};
        p.setLinearGradient(Point{header.x, header.y}, Point{header.x, header.bottom()},
                            mixColor(t.surfaceVariant, kWhite, 0.04),
                            mixColor(t.surfaceVariant, kBlack, 0.03));
        p.rectangle(header);
        p.fill();
        p.resetGradient();
        p.setSourceColor(Color::fromRGBA(1.0, 1.0, 1.0, 0.35));
        p.rectangle(Rect{header.x, header.y, header.width, 1.0});
        p.fill();
        p.setSourceColor(t.border);
        p.rectangle(Rect{header.x, header.bottom() - 1.0, header.width, 1.0});
        p.fill();
        paintButtonGlyphs(p, buttons);
    };
}

/// ── 控件装配 ──────────────────────────────────────────────────────────────

/// 把 `child` 钉在面板本地矩形 `r` 上（可带 key 以便跨重建复用元素）。
SharedPtr<Widget> positionedRect(const Rect& r, SharedPtr<Widget> child,
                                 const std::string& keyId = {}) {
    KeyPtr key = keyId.empty() ? nullptr
                               : cui::make_shared<ValueKey<std::string>>(keyId);
    auto p = cui::make_shared<Positioned>(std::move(child), std::move(key));
    p->left = r.x;
    p->top = r.y;
    p->width = r.width;
    p->height = r.height;
    p->hasLeft = p->hasTop = p->hasWidth = p->hasHeight = true;
    return p;
}

/// 内容区：surface 底板 + 1px 边框，内容裁剪到矩形内；未注册内容画占位 ID。
SharedPtr<Widget> buildGroupContent(const std::string& activeId, const Rect& content) {
    Dockable* d = DockManager::instance().find(activeId);
    SharedPtr<Widget> inner;
    if (d && d->content()) {
        inner = cui::make_shared<ClipRect>(d->content());
    } else {
        auto ph = cui::make_shared<Text>(activeId,
                                         TextStyle().size(kTabFont).fgColor(theme().textDisabled));
        inner = cui::make_shared<Padding>(EdgeInsets{12.0, 20.0, 0.0, 0.0},
                                         cui::make_shared<Align>(-1.0, -1.0, ph));
    }
    auto box = cui::make_shared<Container>(std::move(inner));
    box->decoration(BoxDecoration{theme().surface, 0.0, 1.0, theme().border});
    return box;
}

/// 头部（标签条或标题栏）：CustomPaint 画 chrome，Stack 里放命中块与文字。
SharedPtr<Widget> buildGroupHeader(const std::vector<std::string>& items,
                                   const std::string& activeId, const Rect& headerLocal,
                                   bool tabbed) {
    const Theme& t = theme();
    const std::vector<TabRect> tabs = tabbed ? layoutTabRects(items, activeId, headerLocal)
                                             : std::vector<TabRect>{};
    const ChromeButtons buttons =
        layoutHeaderButtons(activeId, headerLocal, tabbed, tabs);
    const bool topMode = DockingSettings::instance().tabMode == TabMode::Top;

    std::vector<SharedPtr<Widget>> cells;
    if (tabbed) {
        for (const auto& tab : tabs) {
            const bool on = tab.id == activeId;
            const double avail =
                std::max(0.0, tab.rect.width - kTabPad * 2.0 - tab.reserved);
            auto label = cui::make_shared<Text>(
                fitText(tabLabelOf(tab.id), avail),
                TextStyle().size(kTabFont)
                    .fgColor(on ? t.textPrimary : t.textSecondary));
            auto padded = cui::make_shared<Padding>(
                EdgeInsets{kTabPad, 0.0, tab.reserved, 0.0},
                cui::make_shared<Align>(-1.0, 0.0, std::move(label)));
            cells.push_back(positionedRect(tab.rect, std::move(padded)));
        }
    } else {
        const double nb = (buttons.hasMax ? 1.0 : 0.0) + (buttons.hasPin ? 1.0 : 0.0) +
                          (buttons.hasClose ? 1.0 : 0.0);
        const double avail = std::max(0.0, headerLocal.width - 20.0 - nb * kBtn);
        auto title = cui::make_shared<Text>(fitText(titleLabelOf(activeId), avail),
                                            TextStyle().size(kTabFont).fgColor(t.textPrimary));
        cells.push_back(positionedRect(
            Rect{headerLocal.x + 10.0, headerLocal.y, avail, headerLocal.height},
            cui::make_shared<Align>(-1.0, 0.0, std::move(title))));
    }
    // 标签切换 / × / pin / max 全部由面板级手势统一命中（对齐 rui：按下记录、
    // 释放时仍落在同一矩形才触发），这里只放文字。

    auto stack = cui::make_shared<Stack>(
        std::move(cells), nullptr, RenderStack::Alignment::TopStart, RenderStack::Fit::Expand);
    return cui::make_shared<CustomPaint>(
        tabbed ? tabStripPainter(tabs, activeId, buttons, topMode)
               : titleBarPainter(buttons),
        nullptr, Size{0.0, 0.0}, std::move(stack));
}

SharedPtr<Widget> buildTabGroup(const SharedPtr<DockNode>& node, const Rect& rect) {
    if (!node || node->items.empty() || rect.width <= 0.0 || rect.height <= 0.0)
        return cui::make_shared<SizedBox>(0.0, 0.0);

    const std::string activeId =
        containsId(node->items, node->activeId) ? node->activeId : node->items.front();
    const GroupAreas areas = groupAreas(rect);
    // 头部内的元素用「相对头部原点」的坐标（CustomPaint 的画布已平移到盒子原点）。
    const Rect headerLocal{0.0, 0.0, areas.header.width, areas.header.height};

    std::vector<SharedPtr<Widget>> layers;
    layers.push_back(positionedRect(
        Rect{areas.content.x - rect.x, areas.content.y - rect.y, areas.content.width,
             areas.content.height},
        buildGroupContent(activeId, areas.content)));
    layers.push_back(positionedRect(
        Rect{areas.header.x - rect.x, areas.header.y - rect.y, areas.header.width,
             areas.header.height},
        buildGroupHeader(node->items, activeId, headerLocal, node->items.size() > 1)));
    return cui::make_shared<Stack>(std::move(layers),
                                   cui::make_shared<ValueKey<std::string>>(node->groupId()),
                                   RenderStack::Alignment::TopStart,
                                   RenderStack::Fit::Expand);
}

/// 把树里所有标签组收成面板 Stack 的层（Split 只决定矩形，不产出控件）。
void collectLayers(const SharedPtr<DockNode>& node, const Rect& rect,
                   std::vector<SharedPtr<Widget>>& layers) {
    if (!node || rect.width <= 0.0 || rect.height <= 0.0) return;
    if (node->isTabGroup()) {
        layers.push_back(
            positionedRect(rect, buildTabGroup(node, rect), "group:" + node->groupId()));
        return;
    }
    const auto kids = splitChildRects(*node, rect);
    collectLayers(node->first, kids.first, layers);
    collectLayers(node->second, kids.second, layers);
}

/// ── 边条 / 展开浮层 / 浮动窗口（批次 4）───────────────────────────────────
///
/// 布局逐条对齐 rui `layout_strips`(L2615)、`layout_floats`(L2677)、
/// `layout_expanded`(L2735)，绘制对齐 `paint_strips`(L2798)、
/// `paint_expanded`(L2848)、`paint_float`(L2915)。
///
/// 与 rui 的三条有意差异（不是行为差异）：
///   1. **命中与落点用缩进后的树区域**：rui 的 walk_for_splitter /
///      detect_drop_target 仍拿未缩进的 frame，边条存在时命中区会比绘制区
///      宽出一个条宽；本仓统一走 `tree`。
///   2. 边条文字只能自绘（旋转 −90°），走 `Painter::showText`（同一套烘焙
///      字体路径，CJK 回退有效）；浮层/浮窗的标题与标签仍是真 `Text`。
///   3. 展开浮层的底盘：rui 调 `fill_surface(Raised)`（柔影 + 微渐变 +
///      发丝边），本仓没有对应的材质助手，改用与浮窗同款的偏移投影。

constexpr double kStripTabPad = 8.0;        ///< K_STRIP_TAB_PAD
constexpr double kFlyoutFrac = 0.32;        ///< K_FLYOUT_FRAC
constexpr double kExpandedLeaveMs = 300.0;  ///< K_EXPANDED_LEAVE_MS
constexpr double kFloatShadowAlpha = 0.28;  ///< 浮窗/浮层投影浓度（BLACK α0.28）

/// 边缘序号（0=Left 1=Right 2=Top 3=Bottom），与 `StripLayout::bands` 同序。
int edgeIndex(DockEdge e) {
    switch (e) {
        case DockEdge::Left: return 0;
        case DockEdge::Right: return 1;
        case DockEdge::Top: return 2;
        case DockEdge::Bottom: return 3;
    }
    return 0;
}

/// 展开中的隐藏组键。`expandedPanelId` 存的可能是组员 id，统一归到组键，
/// 边条底板与收起判定才有同一个可比对象。
std::string expandedGroupKey() {
    const std::string& eid = DockManager::instance().expandedPanelId();
    if (eid.empty()) return {};
    return DockManager::instance().hiddenKeyFor(eid);
}

Rect shrinkRect(const Rect& r, double dx, double dy, double dw, double dh) {
    return Rect{r.x + dx, r.y + dy, std::max(0.0, r.width - dw),
                std::max(0.0, r.height - dh)};
}

struct StripTabView {
    DockEdge edge = DockEdge::Left;
    std::string key;  ///< 隐藏组键
    std::vector<std::string> ids;
    Rect rect;
};

struct StripLayout {
    Rect tree;  ///< 四条边带各占掉 stripThickness 后的树区域
    Rect bands[4];
    std::vector<StripTabView> tabs;
};

/// 边条布局：只有真的藏着面板的边才占位，tab 沿边带首尾相接（间距 2）。
StripLayout layoutStrips(const Rect& frame) {
    StripLayout out;
    out.tree = frame;
    const auto& hidden = DockManager::instance().hiddenPanels();
    if (hidden.empty()) return out;

    std::vector<const std::pair<const std::string, HiddenPanelInfo>*> items;
    items.reserve(hidden.size());
    for (const auto& kv : hidden) items.push_back(&kv);
    std::sort(items.begin(), items.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });

    const double t = DockingSettings::instance().stripThickness;
    bool has[4] = {false, false, false, false};
    for (const auto* kv : items) has[edgeIndex(kv->second.edge)] = true;
    Rect& tree = out.tree;
    if (has[0]) {
        out.bands[0] = Rect{tree.x, tree.y, t, tree.height};
        tree = shrinkRect(tree, t, 0.0, t, 0.0);
    }
    if (has[1]) {
        out.bands[1] = Rect{tree.right() - t, tree.y, t, tree.height};
        tree = shrinkRect(tree, 0.0, 0.0, t, 0.0);
    }
    if (has[2]) {
        out.bands[2] = Rect{tree.x, tree.y, tree.width, t};
        tree = shrinkRect(tree, 0.0, t, 0.0, t);
    }
    if (has[3]) {
        out.bands[3] = Rect{tree.x, tree.bottom() - t, tree.width, t};
        tree = shrinkRect(tree, 0.0, 0.0, 0.0, t);
    }

    double off[4] = {0.0, 0.0, 0.0, 0.0};
    for (const auto* kv : items) {
        const HiddenPanelInfo& info = kv->second;
        const int ei = edgeIndex(info.edge);
        const Rect b = out.bands[ei];
        if (b.width <= 0.0 || b.height <= 0.0) continue;
        const std::string label = info.groupIds.empty() ? kv->first
                                                        : tabLabelOf(info.groupIds.front());
        const double len = std::max(tabTextWidth(label) + kStripTabPad * 2.0, kTabMinW);
        off[ei] += len + 2.0;
        StripTabView st;
        st.edge = info.edge;
        st.key = kv->first;
        st.ids = info.groupIds;
        st.rect = (ei == 0 || ei == 1) ? Rect{b.x, b.y + off[ei] - len, b.width, len}
                                       : Rect{b.x + off[ei] - len, b.y, len, b.height};
        out.tabs.push_back(std::move(st));
    }
    return out;
}

struct FloatView {
    std::string key;
    std::vector<std::string> ids;
    std::string activeId;
    Rect rect;
    Rect titleRect;
    Rect bodyRect;
    Rect dockRect;
    Rect closeRect;
    Rect resizeRect;
    bool tabbed = false;
    std::vector<std::pair<std::string, Rect>> tabs;
};

/// 浮动窗口布局：offset 相对面板矩形，尺寸夹到面板内（下限由 manager 保证）。
std::vector<FloatView> layoutFloats(const Rect& frame) {
    const auto& floating = DockManager::instance().floatingPanels();
    std::vector<const std::pair<const std::string, FloatingPanelInfo>*> items;
    items.reserve(floating.size());
    for (const auto& kv : floating) items.push_back(&kv);
    std::sort(items.begin(), items.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });

    const double hh = DockingSettings::instance().headerHeight;
    std::vector<FloatView> out;
    for (const auto* kv : items) {
        const FloatingPanelInfo& info = kv->second;
        if (info.ids.empty()) continue;
        const double w = std::min(info.width, std::max(120.0, frame.width));
        const double h = std::min(info.height, std::max(120.0, frame.height));
        const double x = std::min(std::max(frame.x + info.offsetX, frame.x),
                                  std::max(frame.x, frame.x + frame.width - w));
        const double y = std::min(std::max(frame.y + info.offsetY, frame.y),
                                  std::max(frame.y, frame.y + frame.height - h));
        FloatView f;
        f.key = kv->first;
        f.ids = info.ids;
        f.tabbed = info.ids.size() > 1;
        f.rect = Rect{x, y, w, h};
        f.titleRect = Rect{x, y, w, hh};
        double bodyTop = y + hh;
        if (f.tabbed) {
            const double floatEnd = x + w;
            double tx = x + kTabPad;
            for (const auto& id : info.ids) {
                if (tx >= floatEnd) break;
                const double tw = std::max(tabWidthOf(tabLabelOf(id)), 60.0);
                Rect tr{tx, bodyTop, tw, hh};
                if (tr.x + tr.width > floatEnd) tr.width = std::max(0.0, floatEnd - tr.x);
                f.tabs.emplace_back(id, tr);
                tx += tw + kTabGap;
            }
            bodyTop += hh;
        }
        f.activeId = containsId(info.ids, info.activeId) ? info.activeId : info.ids.front();
        f.bodyRect = Rect{x, bodyTop, w, std::max(0.0, y + h - bodyTop)};
        f.dockRect = Rect{x + w - 4.0 - kBtn * 2.0, y + 4.0, kBtn, hh - 8.0};
        f.closeRect = Rect{x + w - 4.0 - kBtn, y + 4.0, kBtn, hh - 8.0};
        f.resizeRect = Rect{x + w - 16.0, y + h - 16.0, 16.0, 16.0};
        out.push_back(std::move(f));
    }
    return out;
}

struct ExpView {
    DockEdge edge = DockEdge::Left;
    std::string key;
    std::string activeId;
    std::vector<std::string> ids;
    Rect rect;
    Rect header;
    Rect body;
    Rect pin;
    std::vector<std::pair<std::string, Rect>> tabs;
};

/// 展开浮层布局：树区域的 `kFlyoutFrac` 分数，头部恒在顶边（不随 TabMode）。
/// `ioActive` 是浮层内选中标签的本地缓存（收起/换组时会被重置）。
bool layoutExpanded(const Rect& tree, std::string& ioActive, ExpView& out) {
    out = ExpView{};
    const std::string eid = DockManager::instance().expandedPanelId();
    if (eid.empty() || tree.width <= 0.0 || tree.height <= 0.0) return false;
    const std::string key = DockManager::instance().hiddenKeyFor(eid);
    if (key.empty()) return false;
    const auto& hidden = DockManager::instance().hiddenPanels();
    auto it = hidden.find(key);
    if (it == hidden.end()) return false;
    const HiddenPanelInfo& info = it->second;

    const double frac = kFlyoutFrac;
    Rect r;
    switch (info.edge) {
        case DockEdge::Left:   r = Rect{tree.x, tree.y, tree.width * frac, tree.height}; break;
        case DockEdge::Right:  r = Rect{tree.x + tree.width - tree.width * frac, tree.y,
                                        tree.width * frac, tree.height}; break;
        case DockEdge::Top:    r = Rect{tree.x, tree.y, tree.width, tree.height * frac}; break;
        case DockEdge::Bottom: r = Rect{tree.x, tree.y + tree.height - tree.height * frac,
                                         tree.width, tree.height * frac}; break;
    }
    if (r.width <= 0.0 || r.height <= 0.0) return false;

    out.edge = info.edge;
    out.key = key;
    out.ids = info.groupIds.empty() ? std::vector<std::string>{eid} : info.groupIds;
    if (ioActive.empty() || !containsId(out.ids, ioActive))
        ioActive = containsId(out.ids, eid) ? eid : out.ids.front();
    out.activeId = ioActive;
    out.rect = r;
    const double hh = std::min(DockingSettings::instance().headerHeight, r.height);
    out.header = Rect{r.x, r.y, r.width, hh};
    out.body = Rect{r.x, r.y + hh, r.width, std::max(0.0, r.height - hh)};
    out.pin = Rect{r.x + r.width - 4.0 - kBtn, r.y + 4.0, kBtn,
                   std::max(0.0, DockingSettings::instance().headerHeight - 8.0)};
    if (out.ids.size() > 1) {
        const double headerEnd = out.header.x + out.header.width - kBtn - 4.0;
        double x = out.header.x + kTabPad;
        for (const auto& id : out.ids) {
            if (x >= headerEnd) break;
            const double tw = std::max(tabWidthOf(tabLabelOf(id)), 60.0);
            Rect tr{x, out.header.y, tw, out.header.height};
            if (tr.x + tr.width > headerEnd) tr.width = std::max(0.0, headerEnd - tr.x);
            out.tabs.emplace_back(id, tr);
            x += tw + kTabGap;
        }
    }
    return true;
}

/// ── 命中与手势（rui walk_for_*(L2506-2595)、zone/drop(L2995-3060)、
///    press/move/release(L3367-3717)）────────────────────────────────────────

constexpr double kSplitterHalfHit = 3.0;  ///< K_SPLITTER_HALF_HIT
constexpr double kZoneThreshold = 0.28;   ///< K_ZONE_THRESHOLD
constexpr double kDirBtn = 28.0;          ///< K_DIR_BTN
constexpr double kDirGap = 4.0;           ///< K_DIR_GAP
constexpr double kDoubleClickMs = 400.0;  ///< K_DOUBLE_CLICK_MS
constexpr double kRootBtnMargin = 20.0;   ///< K_ROOT_BTN_MARGIN
constexpr double kAutoHideMargin = 40.0;  ///< K_AUTO_HIDE_MARGIN

double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// 拖拽悬停的放置目标；`targetPanelId` 为空 = 根级停靠。
struct DropTarget {
    std::string targetPanelId;
    DockingRegion region = DockingRegion::Center;
    Rect anchor;  ///< 方向按钮十字的锚定矩形（目标内容区或面板全域）
};

/// 分割条拖拽状态：按结构路径寻址（0 = first，1 = second）——两个直接孩子
/// 都是分割时，基于 id 的 `setDividerRatio` 无法消歧。
struct SplitterDrag {
    bool active = false;
    std::vector<int> path;
    SplitOrientation orientation = SplitOrientation::Horizontal;
    Rect splitRect;  ///< 按下瞬间的分割节点矩形（面板本地坐标）
};

/// 标签拖拽 / 组内重排状态（rui TabDragState）。
struct TabDrag {
    bool active = false;
    bool dragGroup = false;  ///< 按在标签条空白处 ⇒ 整组（全部标签页）一起拖
    std::string tabId;
    Point pressPos, currentPos;
    bool isDragging = false;
    bool hasDrop = false;
    DropTarget drop;
    bool isReorder = false;
    int reorderFrom = -1, reorderTo = -1;
    std::vector<std::string> otherIds;
    std::vector<double> centers;
    Rect reorderRow;
    bool hasHoverEdge = false;
    DockEdge hoverEdge = DockEdge::Left;

    void resetReorder() {
        isReorder = false;
        reorderFrom = reorderTo = -1;
        otherIds.clear();
        centers.clear();
        reorderRow = Rect{};
    }
    /// 光标落在重排插入位（对齐 rui update_reorder_to）。
    void updateReorderTo() {
        reorderTo = static_cast<int>(centers.size());
        for (size_t i = 0; i < centers.size(); ++i) {
            if (currentPos.x < centers[i]) {
                reorderTo = static_cast<int>(i);
                break;
            }
        }
    }
};

/// chrome 按钮的一次按下-释放判定（release 仍落在同一矩形内才触发）。
/// `FloatTab`/`ExpandedTab` 用 `extra` 带面板 id，`id` 是浮窗/隐藏组的键。
enum class PressKind {
    TabClose, HeaderClose, HeaderPin, HeaderMax,
    StripTab,              ///< 自动隐藏边条上的 tab
    FloatClose, FloatDock, ///< 浮窗标题栏按钮
    FloatTab, ExpandedPin, ExpandedTab,
};
struct PressClick {
    bool active = false;
    PressKind kind = PressKind::HeaderClose;
    std::string id;
    std::string extra;
    Rect rect;
};

/// 浮窗的移动 / 缩放（按下标题栏 = 移动，按下右下角把手 = 缩放）。
struct FloatDrag {
    bool active = false;
    std::string key;
    Point pressPos;
    double startX = 0.0, startY = 0.0, startW = 0.0, startH = 0.0;
    bool isResize = false;
};

/// 面板的全部瞬时手势状态。State 与每帧重建的手势回调共享同一个 shared_ptr，
/// 于是拖拽中途的 rebuild 不会丢按下状态（对齐 rui 把状态放在 widget 里）。
struct DockDragState {
    SplitterDrag splitter;
    TabDrag tab;
    PressClick press;
    FloatDrag floatDrag;
    double lastSplitT = 0.0, lastSplitX = 0.0, lastSplitY = 0.0;
    std::string lastHeaderId;
    double lastHeaderT = 0.0;  ///< <0 = 已判定为双击，等 release 执行最大化
    /// 展开浮层的离开时刻（0 = 在浮层/边条内或没展开）。
    double hoverLeaveMs = 0.0;
    /// 展开浮层里选中的标签（本地缓存，不改隐藏组的数据）。
    std::string expandedActive;
};
using DockDragPtr = std::shared_ptr<DockDragState>;

struct HeaderHit {
    bool found = false;
    std::vector<std::string> items;
    std::string activeId;
    Rect header;
    bool tabbed = false;
};

/// 标签组头部命中：按钮 / 标签 / 空白的细分留给调用方。
void walkForHeader(const SharedPtr<DockNode>& node, const Rect& rect, const Point& p,
                   HeaderHit& out) {
    if (!node || out.found) return;
    if (node->isTabGroup()) {
        if (node->items.empty()) return;
        const GroupAreas areas = groupAreas(rect);
        if (!areas.header.contains(p)) return;
        out.found = true;
        out.items = node->items;
        out.activeId = node->activeId.empty() ? node->items.front() : node->activeId;
        out.header = areas.header;
        out.tabbed = node->items.size() > 1;
        return;
    }
    const auto kids = splitChildRects(*node, rect);
    walkForHeader(node->first, kids.first, p, out);
    if (out.found) return;
    walkForHeader(node->second, kids.second, p, out);
}

/// 内容区命中：活跃面板 id + 内容矩形（拖拽时用于「点进内容就别动 chrome」）。
bool walkForContent(const SharedPtr<DockNode>& node, const Rect& rect, const Point& p,
                    std::string& idOut, Rect& rectOut) {
    if (!node) return false;
    if (node->isTabGroup()) {
        if (node->items.empty() || node->activeId.empty()) return false;
        const GroupAreas areas = groupAreas(rect);
        if (!areas.content.contains(p)) return false;
        idOut = node->activeId;
        rectOut = areas.content;
        return true;
    }
    const auto kids = splitChildRects(*node, rect);
    if (walkForContent(node->first, kids.first, p, idOut, rectOut)) return true;
    return walkForContent(node->second, kids.second, p, idOut, rectOut);
}

/// 分割条命中：中心线 ±3px，命中时把路径快照进 `out`。
void walkForSplitter(const SharedPtr<DockNode>& node, const Rect& rect, const Point& p,
                     std::vector<int>& path, SplitterDrag& out) {
    if (!node || out.active) return;
    if (node->isTabGroup()) return;
    const double r = clamp01(node->dividerRatio);
    bool onBand = false;
    if (node->orientation == SplitOrientation::Horizontal) {
        const double x = rect.x + rect.width * r;
        onBand = p.x >= x - kSplitterHalfHit && p.x <= x + kSplitterHalfHit &&
                 p.y >= rect.y && p.y <= rect.y + rect.height;
    } else {
        const double y = rect.y + rect.height * r;
        onBand = p.y >= y - kSplitterHalfHit && p.y <= y + kSplitterHalfHit &&
                 p.x >= rect.x && p.x <= rect.x + rect.width;
    }
    if (onBand) {
        out.active = true;
        out.path = path;
        out.orientation = node->orientation;
        out.splitRect = rect;
        return;
    }
    const auto kids = splitChildRects(*node, rect);
    path.push_back(0);
    walkForSplitter(node->first, kids.first, p, path, out);
    path.pop_back();
    if (out.active) return;
    path.push_back(1);
    walkForSplitter(node->second, kids.second, p, path, out);
    path.pop_back();
}

/// 面板级方向区域：归一化最近边距离，超过阈值即 Center。
DockingRegion zoneRegion(const Rect& rect, const Point& p) {
    const double rx = rect.width > 0.0 ? (p.x - rect.x) / rect.width : 0.5;
    const double ry = rect.height > 0.0 ? (p.y - rect.y) / rect.height : 0.5;
    const double dl = rx, dr = 1.0 - rx, dt = ry, db = 1.0 - ry;
    const double m = std::min(std::min(dl, dr), std::min(dt, db));
    if (m > kZoneThreshold) return DockingRegion::Center;
    if (m == dl) return DockingRegion::Left;
    if (m == dr) return DockingRegion::Right;
    if (m == dt) return DockingRegion::Top;
    return DockingRegion::Bottom;
}

/// 根级落点：frame 归一化判定，光标在 frame 内总有效（含 Center 兜底）。
bool rootZoneAlways(const Rect& frame, const Point& p, DropTarget& out) {
    if (frame.width <= 0.0 || frame.height <= 0.0) return false;
    out = DropTarget{{}, zoneRegion(frame, p), frame};
    return true;
}

/// 放置目标：优先面板级区域（在缩进后的树区域里走），退化到根级区域。
/// 根级锚点用**完整面板矩形**（含边条），与 rui `detect_drop_target` 同口径；
/// 因此根级落点恒有效 —— 拖出后「保持浮动」只在抬手点落到面板矩形之外才触发。
bool detectDropTarget(const SharedPtr<DockNode>& root, const Rect& tree, const Rect& frame,
                      const Point& p, DropTarget& out) {
    std::string pid;
    Rect content;
    if (tree.width > 0.0 && tree.height > 0.0 && root &&
        walkForContent(root, tree, p, pid, content)) {
        out = DropTarget{pid, zoneRegion(content, p), content};
        return true;
    }
    return rootZoneAlways(frame, p, out);
}

/// 预览矩形：边缘区域为目标区域的一半（Center 为全域）。
Rect previewRect(const DropTarget& target, const Rect& frame) {
    const Rect& r = target.targetPanelId.empty() ? frame : target.anchor;
    switch (target.region) {
        case DockingRegion::Left:   return Rect{r.x, r.y, r.width * 0.5, r.height};
        case DockingRegion::Right:  return Rect{r.x + r.width * 0.5, r.y, r.width * 0.5, r.height};
        case DockingRegion::Top:    return Rect{r.x, r.y, r.width, r.height * 0.5};
        case DockingRegion::Bottom: return Rect{r.x, r.y + r.height * 0.5, r.width, r.height * 0.5};
        case DockingRegion::Center: return r;
    }
    return r;
}

/// 执行标签落点：根级走 dock（非 Center 0.3 / Center 0.5），面板级 0.5。
void executeTabDrop(const std::string& tabId, const DropTarget& target) {
    // 整组拖拽（按在标签条空白处启动）：先记下浮动组里的同伙成员，dock 主面板
    // 后把它们逐个 Center 并进同一标签组（dock/dockRelative 都只摘单面板）。
    std::vector<std::string> companions;
    const std::string fkey = DockManager::instance().floatingKeyFor(tabId);
    if (!fkey.empty()) {
        const auto& fps = DockManager::instance().floatingPanels();
        auto it = fps.find(fkey);
        if (it != fps.end())
            for (const auto& gid : it->second.ids)
                if (gid != tabId) companions.push_back(gid);
    }
    if (target.targetPanelId.empty()) {
        const double ratio = target.region == DockingRegion::Center ? 0.5 : 0.3;
        Docking::dock(tabId, target.region, ratio);
    } else {
        Docking::dockRelative(tabId, target.targetPanelId, target.region, 0.5);
    }
    for (const auto& gid : companions)
        Docking::dockRelative(gid, tabId, DockingRegion::Center, 0.5);
}

/// 方向 → 按钮编号（0=NORTH 1=SOUTH 2=WEST 3=EAST 4=CENTER）。
int regionToDir(DockingRegion region) {
    switch (region) {
        case DockingRegion::Top:    return 0;
        case DockingRegion::Bottom: return 1;
        case DockingRegion::Left:   return 2;
        case DockingRegion::Right:  return 3;
        case DockingRegion::Center: return 4;
    }
    return 4;
}

/// 越界 40px 且允许自动隐藏 → 悬停边缘（优先于 drop_target）。
bool detectHoverEdge(const Rect& frame, const Point& p, const std::string& tabId,
                     DockEdge& out) {
    if (frame.contains(p)) return false;
    Dockable* d = DockManager::instance().find(tabId);
    if (!d || !d->isAutoHideAllowed()) return false;
    const std::pair<double, DockEdge> cands[4] = {
        {frame.x - p.x, DockEdge::Left},
        {p.x - (frame.x + frame.width), DockEdge::Right},
        {frame.y - p.y, DockEdge::Top},
        {p.y - (frame.y + frame.height), DockEdge::Bottom},
    };
    double best = -1.0;
    for (const auto& [dist, edge] : cands) {
        if (dist > 0.0 && dist <= kAutoHideMargin && (best < 0.0 || dist < best)) {
            best = dist;
            out = edge;
        }
    }
    return best >= 0.0;
}

/// ── 边条 / 浮层 / 浮窗绘制 ────────────────────────────────────────────────

/// 偏移投影（浮窗与展开浮层共用，对齐 paint_float 的 +3/+4 实心影）。
void drawOffsetShadow(Painter& p, const Rect& r, double dx, double dy, double alpha) {
    p.setSourceColor(Color::fromRGBA(0.0, 0.0, 0.0, alpha));
    p.rectangle(Rect{r.x + dx, r.y + dy, r.width, r.height});
    p.fill();
}

/// 边条：band 底板 + 内侧 1px 边线 + 展开中 tab 的 surface 底板 + 旋转文字。
ChromePaintFn stripsPainter(StripLayout strips, std::string expandedKey) {
    return [strips = std::move(strips),
            expandedKey = std::move(expandedKey)](Painter& p, const Size&) {
        const Theme& t = theme();
        for (int i = 0; i < 4; ++i) {
            const Rect& b = strips.bands[i];
            if (b.width <= 0.0 || b.height <= 0.0) continue;
            p.setSourceColor(t.surfaceVariant);
            p.rectangle(b);
            p.fill();
            // 边线画在朝向树区域的那一侧。
            const Rect line = i == 0 ? Rect{b.right() - 1.0, b.y, 1.0, b.height}
                            : i == 1 ? Rect{b.x, b.y, 1.0, b.height}
                            : i == 2 ? Rect{b.x, b.bottom() - 1.0, b.width, 1.0}
                                     : Rect{b.x, b.y, b.width, 1.0};
            p.setSourceColor(t.border);
            p.rectangle(line);
            p.fill();
        }
        for (const auto& st : strips.tabs) {
            if (st.rect.width <= 0.0 || st.rect.height <= 0.0) continue;
            const bool on = !expandedKey.empty() && st.key == expandedKey;
            if (on) {
                p.setSourceColor(t.surface);
                p.rectangle(st.rect);
                p.fill();
            }
            const std::string label =
                st.ids.empty() ? st.key : tabLabelOf(st.ids.front());
            const double tw = tabTextWidth(label);
            p.save();
            p.clipRect(st.rect);
            p.translate(st.rect.x + st.rect.width / 2.0, st.rect.y + st.rect.height / 2.0);
            // 垂直边一律 −90°（左右两边阅读方向一致：自下而上）。
            if (st.edge == DockEdge::Left || st.edge == DockEdge::Right) p.rotate(-90.0);
            // showText 的 y 是基线：放在视觉中心下方 0.3×字号，否则旋转后
            // 文字会整体偏到矩形一端（与全局 h/2+4 同款经验值）。
            p.translate(-tw / 2.0, kTabFont * 0.30);
            p.setSourceColor(on ? t.textPrimary : t.textSecondary);
            p.setFontSize(kTabFont);
            p.showText(label.c_str());
            p.restore();
        }
    };
}

/// 展开浮层底板：投影 + surface 实心（chrome 与文字在内容之上另起一层）。
ChromePaintFn expandedBasePainter(ExpView exp) {
    return [exp](Painter& p, const Size&) {
        drawOffsetShadow(p, exp.rect, 3.0, 4.0, kFloatShadowAlpha);
        p.setSourceColor(theme().surface);
        p.rectangle(exp.rect);
        p.fill();
    };
}

/// 展开浮层 chrome：头部渐变 + 活动标签三层 + pin 字形 + 1px 主色描边。
ChromePaintFn expandedChromePainter(ExpView exp) {
    return [exp](Painter& p, const Size&) {
        const Theme& t = theme();
        p.setLinearGradient(Point{exp.header.x, exp.header.y},
                            Point{exp.header.x, exp.header.bottom()},
                            mixColor(t.surfaceVariant, kBlack, 0.03),
                            mixColor(t.surfaceVariant, kWhite, 0.04));
        p.rectangle(exp.header);
        p.fill();
        p.resetGradient();

        for (const auto& [id, tr] : exp.tabs) {
            if (id != exp.activeId) continue;
            p.setSourceColor(t.surface);
            p.rectangle(tr);
            p.fill();
            p.setSourceColor(Color::fromRGBA(1.0, 1.0, 1.0, 0.35));
            p.rectangle(Rect{tr.x, tr.y, tr.width, 1.0});
            p.fill();
            p.setSourceColor(t.primary);
            p.rectangle(Rect{tr.x, tr.bottom() - 2.0, tr.width, 2.0});
            p.fill();
        }

        const double cx = exp.pin.x + exp.pin.width / 2.0;
        const double cy = exp.pin.y + exp.pin.height / 2.0;
        p.setSourceColor(t.textSecondary);
        p.setLineWidth(1.4);
        p.moveTo(cx, cy - 5.0);
        p.lineTo(cx, cy + 5.0);
        p.stroke();
        p.moveTo(cx - 3.5, cy - 2.0);
        p.lineTo(cx + 3.5, cy - 2.0);
        p.stroke();

        p.setSourceColor(t.primary);
        p.setLineWidth(1.0);
        p.rectangle(Rect{exp.rect.x + 0.5, exp.rect.y + 0.5, exp.rect.width - 1.0,
                         exp.rect.height - 1.0});
        p.stroke();
    };
}

/// 浮窗底板：投影 + surface 实心。
ChromePaintFn floatsBasePainter(std::vector<FloatView> floats) {
    return [floats = std::move(floats)](Painter& p, const Size&) {
        for (const auto& f : floats) {
            drawOffsetShadow(p, f.rect, 3.0, 4.0, kFloatShadowAlpha);
            p.setSourceColor(theme().surface);
            p.rectangle(f.rect);
            p.fill();
        }
    };
}

/// 浮窗 chrome：标题条 + 活动标签底板 + dock/close/resize 字形 + 1px 描边。
ChromePaintFn floatsChromePainter(std::vector<FloatView> floats) {
    return [floats = std::move(floats)](Painter& p, const Size&) {
        const Theme& t = theme();
        for (const auto& f : floats) {
            p.setSourceColor(t.surfaceVariant);
            p.rectangle(f.titleRect);
            p.fill();
            for (const auto& [id, tr] : f.tabs) {
                if (id != f.activeId) continue;
                p.setSourceColor(t.surface);
                p.rectangle(tr);
                p.fill();
            }

            p.setSourceColor(t.textSecondary);
            p.setLineWidth(1.4);
            if (f.dockRect.width > 0.0) {
                const double cx = f.dockRect.x + f.dockRect.width / 2.0;
                const double cy = f.dockRect.y + f.dockRect.height / 2.0;
                p.moveTo(cx - 4.0, cy);
                p.lineTo(cx + 4.0, cy);
                p.stroke();
                p.moveTo(cx + 1.0, cy - 3.0);
                p.lineTo(cx + 4.0, cy);
                p.stroke();
                p.moveTo(cx + 1.0, cy + 3.0);
                p.lineTo(cx + 4.0, cy);
                p.stroke();
            }
            if (f.closeRect.width > 0.0) {
                const double cx = f.closeRect.x + f.closeRect.width / 2.0;
                const double cy = f.closeRect.y + f.closeRect.height / 2.0;
                p.moveTo(cx - 4.0, cy - 4.0);
                p.lineTo(cx + 4.0, cy + 4.0);
                p.stroke();
                p.moveTo(cx + 4.0, cy - 4.0);
                p.lineTo(cx - 4.0, cy + 4.0);
                p.stroke();
            }
            if (f.resizeRect.width > 0.0) {
                p.setSourceColor(t.border);
                p.setLineWidth(1.0);
                for (int i = 0; i < 3; ++i) {
                    const double o = 4.0 + static_cast<double>(i) * 3.0;
                    p.moveTo(f.resizeRect.right() - o, f.resizeRect.bottom() - 2.0);
                    p.lineTo(f.resizeRect.right() - 2.0, f.resizeRect.bottom() - o);
                    p.stroke();
                }
            }

            p.setSourceColor(t.border);
            p.setLineWidth(1.0);
            p.rectangle(Rect{f.rect.x + 0.5, f.rect.y + 0.5, f.rect.width - 1.0,
                              f.rect.height - 1.0});
            p.stroke();
        }
    };
}

/// ── 浮层 / 浮窗的控件装配 ─────────────────────────────────────────────────

/// 浮层/浮窗的内容层：裁剪到给定矩形；未注册内容画占位 ID。
SharedPtr<Widget> overlayContent(const std::string& id) {
    Dockable* d = DockManager::instance().find(id);
    if (d && d->content()) return cui::make_shared<ClipRect>(d->content());
    auto ph = cui::make_shared<Text>(id,
                                     TextStyle().size(kTabFont).fgColor(theme().textDisabled));
    return cui::make_shared<Padding>(EdgeInsets{12.0, 8.0, 0.0, 0.0},
                                     cui::make_shared<Align>(-1.0, -1.0, std::move(ph)));
}

/// 浮层/浮窗头部的一行文字：面板本地矩形 `r` 内左侧缩进 `indent`、垂直居中。
SharedPtr<Widget> headerTextLabel(const Rect& r, const std::string& text, double indent,
                                  double avail, const Color& color, const std::string& keyId) {
    auto txt = cui::make_shared<Text>(fitText(text, avail),
                                      TextStyle().size(kTabFont).fgColor(color));
    return positionedRect(
        r, cui::make_shared<Padding>(EdgeInsets{indent, 0.0, 0.0, 0.0},
                                     cui::make_shared<Align>(-1.0, 0.0, std::move(txt))),
        keyId);
}

/// 展开浮层的文字层（单面板画标题，多面板画标签）。
void pushExpandedTexts(const ExpView& exp, std::vector<SharedPtr<Widget>>& layers) {
    const Theme& t = theme();
    if (exp.tabs.size() > 1) {
        for (const auto& [id, tr] : exp.tabs) {
            const bool on = id == exp.activeId;
            layers.push_back(headerTextLabel(
                tr, tabLabelOf(id), kTabPad, std::max(0.0, tr.width - kTabPad - 8.0),
                on ? t.textPrimary : t.textSecondary, "exp-tab:" + id));
        }
        return;
    }
    layers.push_back(headerTextLabel(
        exp.header, titleLabelOf(exp.activeId), 10.0,
        std::max(0.0, exp.header.width - kBtn - 20.0), t.textPrimary,
        "exp-title:" + exp.key));
}

/// 浮窗的文字层（标题 + 标签行）。
void pushFloatTexts(const FloatView& f, std::vector<SharedPtr<Widget>>& layers) {
    const Theme& t = theme();
    layers.push_back(headerTextLabel(
        f.titleRect, titleLabelOf(f.activeId), 10.0,
        std::max(0.0, f.titleRect.width - kBtn * 2.0 - 20.0), t.textPrimary,
        "float-title:" + f.key));
    if (!f.tabbed) return;
    for (const auto& [id, tr] : f.tabs) {
        const bool on = id == f.activeId;
        layers.push_back(headerTextLabel(
            tr, tabLabelOf(id), kTabPad, std::max(0.0, tr.width - kTabPad * 2.0),
            on ? t.textPrimary : t.textSecondary, "float-tab:" + id));
    }
}

/// ── 拖拽浮层绘制 ──────────────────────────────────────────────────────────

void drawTriangle(Painter& p, double ax, double ay, double bx, double by,
                  double cx, double cy) {
    p.moveTo(ax, ay);
    p.lineTo(bx, by);
    p.lineTo(cx, cy);
    p.lineTo(ax, ay);
    p.fill();
}

/// 单个方向按钮（28×28 圆角 4，悬停填主色）。
void drawDirButton(Painter& p, const Rect& rect, int dir, bool hovered, const Theme& t) {
    p.setSourceColor(hovered ? t.primary : t.surface);
    p.fillRoundedRect(rect, 4.0);
    p.setLineWidth(1.0);
    p.setSourceColor(hovered ? t.primaryDark : t.border);
    p.strokeRoundedRect(rect, 4.0);
    const double cx = rect.x + rect.width / 2.0;
    const double cy = rect.y + rect.height / 2.0;
    p.setSourceColor(hovered ? t.textOnPrimary : t.textSecondary);
    constexpr double s = 5.0;
    switch (dir) {
        case 0: drawTriangle(p, cx, cy - s, cx - s, cy + s * 0.6, cx + s, cy + s * 0.6); break;
        case 1: drawTriangle(p, cx, cy + s, cx - s, cy - s * 0.6, cx + s, cy - s * 0.6); break;
        case 2: drawTriangle(p, cx - s, cy, cx + s * 0.6, cy - s, cx + s * 0.6, cy + s); break;
        case 3: drawTriangle(p, cx + s, cy, cx - s * 0.6, cy - s, cx - s * 0.6, cy + s); break;
        default: p.rectangle(Rect{cx - 3.0, cy - 3.0, 6.0, 6.0}); p.fill(); break;
    }
}

/// VS 风格五按钮十字（N/S/W/E/CENTER，按钮 28px、间距 4px）。
void drawVsCross(Painter& p, double ccx, double ccy, int hovered, const Theme& t) {
    const double b = kDirBtn, g = kDirGap;
    drawDirButton(p, Rect{ccx - b / 2.0, ccy - b / 2.0, b, b}, 4, hovered == 4, t);
    drawDirButton(p, Rect{ccx - b / 2.0, ccy - b / 2.0 - g - b, b, b}, 0, hovered == 0, t);
    drawDirButton(p, Rect{ccx - b / 2.0, ccy + b / 2.0 + g, b, b}, 1, hovered == 1, t);
    drawDirButton(p, Rect{ccx - b / 2.0 - g - b, ccy - b / 2.0, b, b}, 2, hovered == 2, t);
    drawDirButton(p, Rect{ccx + b / 2.0 + g, ccy - b / 2.0, b, b}, 3, hovered == 3, t);
}

/// 根级四边按钮（距面板边缘 20px）。
void drawRootEdgeButtons(Painter& p, const Rect& area, int hovered, const Theme& t) {
    const double b = kDirBtn, m = kRootBtnMargin;
    const double cx = area.x + area.width / 2.0 - b / 2.0;
    const double cy = area.y + area.height / 2.0 - b / 2.0;
    drawDirButton(p, Rect{cx, area.y + m, b, b}, 0, hovered == 0, t);
    drawDirButton(p, Rect{cx, area.y + area.height - m - b, b, b}, 1, hovered == 1, t);
    drawDirButton(p, Rect{area.x + m, cy, b, b}, 2, hovered == 2, t);
    drawDirButton(p, Rect{area.x + area.width - m - b, cy, b, b}, 3, hovered == 3, t);
}

/// 幽灵 chip 矩形（跟随光标 +12/+12）。
Rect ghostChipRect(const std::string& tabId, const Point& pos) {
    Dockable* d = DockManager::instance().find(tabId);
    const std::string text = d ? d->tabText() : tabId;
    return Rect{pos.x + 12.0, pos.y + 12.0, tabWidthOf(text) + 8.0, 24.0};
}

/// 拖拽浮层：重排指示线 / 落点预览 + 方向按钮 + 幽灵 chip 底板。
ChromePaintFn dragOverlayPainter(DockDragPtr drag, Rect local) {
    return [drag, local](Painter& p, const Size&) {
        const Theme& t = theme();
        const TabDrag& tab = drag->tab;
        if (!tab.active) return;

        // ── 组内重排：标签条内的插入指示线 + 两端圆 ──
        if (tab.isReorder && !tab.isDragging) {
            if (tab.reorderRow.width <= 0.0 || tab.reorderTo < 0) return;
            const size_t n = tab.centers.size();
            double lineX = tab.reorderRow.x + kTabPad;
            if (n > 0) {
                if (static_cast<size_t>(tab.reorderTo) >= n) lineX = tab.centers[n - 1] + 20.0;
                else if (tab.reorderTo <= 0) lineX = tab.centers[0] - 20.0;
                else lineX = (tab.centers[tab.reorderTo - 1] + tab.centers[tab.reorderTo]) / 2.0;
            }
            p.setSourceColor(t.primary);
            p.setLineWidth(3.0);
            p.moveTo(lineX, tab.reorderRow.y);
            p.lineTo(lineX, tab.reorderRow.y + tab.reorderRow.height);
            p.stroke();
            p.setLineWidth(1.0);
            p.circle(lineX, tab.reorderRow.y, 4.0);
            p.fill();
            p.circle(lineX, tab.reorderRow.y + tab.reorderRow.height, 4.0);
            p.fill();
            return;
        }
        if (!tab.isDragging) return;

        auto preview = [&](const DropTarget& target, const Rect& frame) {
            if (target.region == DockingRegion::Center) return;
            const Rect prev = previewRect(target, frame);
            if (prev.width <= 0.0 || prev.height <= 0.0) return;
            p.setSourceColor(t.primary.withOpacity(0.12));
            p.rectangle(prev);
            p.fill();
            p.setSourceColor(t.primary.withOpacity(0.5));
            p.setLineWidth(2.0);
            p.rectangle(prev);
            p.stroke();
            p.setLineWidth(1.0);
        };

        // 根级指示常驻：光标不在任何面板上时仍展示根级落点。
        DropTarget rootTarget;
        const bool hasRoot = tab.hasDrop && tab.drop.targetPanelId.empty()
                                 ? (rootTarget = tab.drop, true)
                                 : rootZoneAlways(local, tab.currentPos, rootTarget);
        if (hasRoot) {
            preview(rootTarget, local);
            drawRootEdgeButtons(p, local, regionToDir(rootTarget.region), t);
        }

        // 面板级指示：五按钮十字 + 预览（Center 时高亮整个目标区）。
        if (tab.hasDrop && !tab.drop.targetPanelId.empty()) {
            preview(tab.drop, local);
            if (tab.drop.region == DockingRegion::Center) {
                p.setSourceColor(t.primary.withOpacity(0.10));
                p.rectangle(tab.drop.anchor);
                p.fill();
            }
            const double ccx = tab.drop.anchor.x + tab.drop.anchor.width / 2.0;
            const double ccy = tab.drop.anchor.y + tab.drop.anchor.height / 2.0;
            drawVsCross(p, ccx, ccy, regionToDir(tab.drop.region), t);
        }

        // 幽灵 chip 底板（文字另起一层，走真 Text 才有 CJK 回退）。
        const Rect chip = ghostChipRect(tab.tabId, tab.currentPos);
        p.setSourceColor(t.surface);
        p.fillRoundedRect(chip, 4.0);
        p.setLineWidth(1.5);
        p.setSourceColor(t.primary);
        p.strokeRoundedRect(chip, 4.0);
        p.setLineWidth(1.0);
    };
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// RootDockingPanel
// ═══════════════════════════════════════════════════════════════════════════

class RootDockingPanelState : public State {
public:
    void initState() override {
        drag_ = std::make_shared<DockDragState>();
        handle_ = DockManager::instance().addListener([this] { setState(); });
    }
    void dispose() override { DockManager::instance().removeListener(handle_); }

    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<LayoutBuilder>([this](BuildContext&, const BoxConstraints& c) {
            return buildPanel(c);
        });
    }

private:
    /// 事件坐标 → 面板本地坐标。分发器给链上每个节点都做过 `globalToLocal`，
    /// 而收事件的这层监听盒子就是面板盒子本身（Listener 包住整块
    /// Stack），所以 `position` 直接就是面板本地坐标。
    static Point toLocal(const PointerEvent& e) {
        return Point{e.position.dx, e.position.dy};
    }

    // ── 手势（rui on_mouse_press/move/release_impl 的「树」部分）───────────

    void onDown(const PointerEvent& e) {
        if (e.type != PointerEventType::Down) return;
        // 与 rui 同口径：鼠标只有左键参与停靠手势（Listener 会
        // 转发全部按键，右键/中键按下不得启动拖拽）。
        if (e.pointerKind == PointerDeviceKind::mouse &&
            (e.buttons & kPrimaryMouseButton) == 0)
            return;
        const Point p = toLocal(e);
        DockDragState& s = *drag_;
        s.splitter = SplitterDrag{};
        s.tab = TabDrag{};
        s.press = PressClick{};
        s.floatDrag = FloatDrag{};
        if (!local_.contains(p)) return;

        const StripLayout strips = layoutStrips(local_);
        tree_ = strips.tree;

        // 0) 展开浮层外的一次按下 = 只收起浮层（这次点击被吃掉，对齐 rui
        //    collapse_expanded_if_outside 的 return）。
        ExpView exp;
        const bool hasExp = layoutExpanded(strips.tree, s.expandedActive, exp);
        if (hasExp && !exp.rect.contains(p)) {
            Docking::collapseToEdge();
            return;
        }

        // 0a) 浮动窗口：倒序 = 视觉最上面的先命中；close → dock → 把手/标题
        //     → tab → body。body 的内容子树自己收事件，这里只负责吞掉树命中。
        const std::vector<FloatView> floats = layoutFloats(local_);
        for (size_t i = floats.size(); i-- > 0;) {
            const FloatView& f = floats[i];
            if (!f.rect.contains(p)) continue;
            if (f.closeRect.contains(p)) {
                s.press = PressClick{true, PressKind::FloatClose, f.activeId, {}, f.closeRect};
                return;
            }
            if (f.dockRect.contains(p)) {
                s.press = PressClick{true, PressKind::FloatDock, f.key, {}, f.dockRect};
                return;
            }
            if (f.resizeRect.contains(p) || f.titleRect.contains(p)) {
                auto it = DockManager::instance().floatingPanels().find(f.key);
                if (it != DockManager::instance().floatingPanels().end())
                    s.floatDrag = FloatDrag{true, f.key, p, it->second.offsetX,
                                            it->second.offsetY, it->second.width,
                                            it->second.height, f.resizeRect.contains(p)};
                return;
            }
            for (const auto& [id, tr] : f.tabs) {
                if (!tr.contains(p)) continue;
                s.press = PressClick{true, PressKind::FloatTab, f.key, id, tr};
                return;
            }
            return;
        }

        // 0b) 展开浮层：pin → tab → 其余（body / 头部空白）吞掉。
        if (hasExp) {
            if (exp.pin.contains(p)) {
                s.press = PressClick{true, PressKind::ExpandedPin, exp.key, {}, exp.pin};
                return;
            }
            for (const auto& [id, tr] : exp.tabs) {
                if (!tr.contains(p)) continue;
                s.press = PressClick{true, PressKind::ExpandedTab, exp.key, id, tr};
                return;
            }
            return;
        }

        // 0c) 边条 tab：按下记录，释放仍落在同一矩形才展开/收起。
        for (const auto& st : strips.tabs) {
            if (!st.rect.contains(p)) continue;
            s.press = PressClick{true, PressKind::StripTab, st.key, {}, st.rect};
            return;
        }

        auto root = DockManager::instance().root();
        if (!root) return;

        // 1) 分割条：命中即进入拖拽；400ms 内同点二次按下 = 双击 ⇒ 复位 0.5。
        std::vector<int> path;
        walkForSplitter(root, tree_, p, path, s.splitter);
        if (s.splitter.active) {
            const double now = nowMs();
            const bool dbl = (now - s.lastSplitT) < kDoubleClickMs &&
                             std::fabs(p.x - s.lastSplitX) <= 6.0 &&
                             std::fabs(p.y - s.lastSplitY) <= 6.0;
            s.lastSplitT = now;
            s.lastSplitX = p.x;
            s.lastSplitY = p.y;
            if (dbl) {
                DockManager::instance().setSplitRatioByPath(s.splitter.path, 0.5);
                s.splitter = SplitterDrag{};
            }
            return;
        }

        // 2) 标签组头部：按钮 / 标签 / 空白。
        HeaderHit hh;
        walkForHeader(root, tree_, p, hh);
        if (!hh.found) return;
        const std::vector<TabRect> tabs =
            hh.tabbed ? layoutTabRects(hh.items, hh.activeId, hh.header)
                      : std::vector<TabRect>{};
        const ChromeButtons btns = layoutHeaderButtons(hh.activeId, hh.header, hh.tabbed, tabs);

        // 2a) chrome 按钮：按下记录，释放仍落在同一矩形内才触发。
        auto takePress = [&](PressKind k, const Rect& r) {
            s.press = PressClick{true, k, hh.activeId, {}, r};
        };
        if (btns.hasClose && btns.close.contains(p)) {
            takePress(hh.tabbed ? PressKind::TabClose : PressKind::HeaderClose, btns.close);
            return;
        }
        if (btns.hasPin && btns.pin.contains(p)) { takePress(PressKind::HeaderPin, btns.pin); return; }
        if (btns.hasMax && btns.max.contains(p)) { takePress(PressKind::HeaderMax, btns.max); return; }

        // 2b) 双击判定基线（标签和头部空白都记录；-1 时间戳 = 已判定为双击，
        //     交给 release 执行最大化/还原）。
        const double now = nowMs();
        const bool isDouble = s.lastHeaderId == hh.activeId && (now - s.lastHeaderT) < kDoubleClickMs;
        s.lastHeaderId = hh.activeId;
        s.lastHeaderT = isDouble ? -1.0 : now;

        // 2c) 标签按下：记下重排基线（按下不切换，释放才切换）。
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (!tabs[i].rect.contains(p)) continue;
            TabDrag d;
            d.active = true;
            d.tabId = tabs[i].id;
            d.pressPos = p;
            d.currentPos = p;
            d.reorderFrom = static_cast<int>(i);
            for (size_t j = 0; j < hh.items.size(); ++j)
                if (j != i) d.otherIds.push_back(hh.items[j]);
            for (size_t j = 0; j < tabs.size(); ++j)
                if (j != i) d.centers.push_back(tabs[j].rect.x + tabs[j].rect.width / 2.0);
            d.reorderRow = hh.header;
            s.tab = std::move(d);
            return;
        }

        // 2d) 空白头部：进入潜在拖拽（otherIds 为空 ⇒ release 走双击而非切换）。
        //     拖动空白区 = 整组 drag & drop（对齐 VS：抓标签条空白拖走全部页）。
        TabDrag d;
        d.active = true;
        d.dragGroup = true;
        d.tabId = hh.activeId;
        d.pressPos = p;
        d.currentPos = p;
        s.tab = std::move(d);
    }

    void onMove(const PointerEvent& e) {
        if (e.type != PointerEventType::Move) return;
        const Point p = toLocal(e);
        DockDragState& s = *drag_;

        if (s.splitter.active) {
            const double ratio = s.splitter.orientation == SplitOrientation::Horizontal
                                     ? (p.x - s.splitter.splitRect.x) / s.splitter.splitRect.width
                                     : (p.y - s.splitter.splitRect.y) / s.splitter.splitRect.height;
            // setSplitRatioByPath 会 emitLayoutChanged ⇒ 监听器负责重排重绘。
            DockManager::instance().setSplitRatioByPath(s.splitter.path, ratio);
            return;
        }
        if (s.floatDrag.active) {
            // 位置/尺寸都相对「按下时的 offset/宽高 + 位移」，写回 manager 后
            // 由监听器重排（updateFloatingSize 内部夹住 200×150 下限）。
            const double dx = p.x - s.floatDrag.pressPos.x;
            const double dy = p.y - s.floatDrag.pressPos.y;
            if (s.floatDrag.isResize)
                Docking::updateFloatingSize(s.floatDrag.key, s.floatDrag.startW + dx,
                                            s.floatDrag.startH + dy);
            else
                Docking::updateFloatingPosition(s.floatDrag.key, s.floatDrag.startX + dx,
                                                s.floatDrag.startY + dy);
            return;
        }
        if (!s.tab.active) return;

        TabDrag& d = s.tab;
        const double headerH = DockingSettings::instance().headerHeight;
        const double threshold = static_cast<double>(DockingSettings::instance().dragThreshold);

        if (d.isReorder && !d.isDragging) {
            // 重排中：拖出标签条且纵向超出头部高度 ⇒ 退出重排，转回面板拖拽。
            const double dy = p.y - d.pressPos.y;
            if (!d.reorderRow.contains(p) && std::fabs(dy) > headerH) {
                d.resetReorder();
            } else {
                d.currentPos = p;
                d.updateReorderTo();
                setState();
                return;
            }
        }

        if (!d.isDragging) {
            const double dx = p.x - d.pressPos.x;
            const double dy = p.y - d.pressPos.y;
            // 组内横向拖动（|dx| > |dy|*1.5 且仍在标签条内）⇒ 重排，不 undock。
            const bool canReorder = !d.otherIds.empty() && d.reorderRow.contains(p) &&
                                    std::fabs(dx) > std::fabs(dy) * 1.5;
            if (canReorder && std::fabs(dx) > threshold) {
                d.isReorder = true;
                d.currentPos = p;
                d.updateReorderTo();
                setState();
                return;
            }
            if (std::sqrt(dx * dx + dy * dy) < threshold) return;
            // 拖拽启动：先摘出树并标记浮动（不允许浮动的面板不启动，否则摘掉就没了）。
            Dockable* dockable = DockManager::instance().find(d.tabId);
            if (!dockable || !dockable->isFloatingAllowed()) {
                d.active = false;
                return;
            }
            d.isDragging = true;
            if (d.dragGroup) {
                // 整组拖拽：floatPanel 自带「整组摘树 + 浮动 + 记原位置」，
                // 不先 undock 单面板（那会把组拆散成单个浮动页）。
                Docking::floatPanel(d.tabId);
                if (!DockManager::instance().isFloating(d.tabId)) {
                    d.active = false;  // 组内有不允许浮动的成员 ⇒ 取消
                    return;
                }
            } else {
                Docking::undock(d.tabId);
                Docking::floatPanel(d.tabId);
            }
        }
        d.currentPos = p;
        d.hasHoverEdge = detectHoverEdge(local_, p, d.tabId, d.hoverEdge);
        d.hasDrop = detectDropTarget(DockManager::instance().root(), tree_, local_, p, d.drop);
        setState();
    }

    /// 无按下的悬停移动（rui on_hover_move_impl）：边条即指即展开，离开展开
    /// 浮层满 300ms 收起。手势进行中不参与（拖拽落点/边缘隐藏提交优先）。
    /// 与 rui 相同：延迟收回靠后续悬停移动兜底，鼠标静止不再有事件时不自己计时。
    void onHoverMove(const PointerEvent& e) {
        if (e.type != PointerEventType::Hover) return;
        DockDragState& s = *drag_;
        if (s.splitter.active || s.floatDrag.active) return;
        if (s.tab.active && (s.tab.isDragging || s.tab.isReorder)) return;

        const Point p = toLocal(e);
        if (!local_.contains(p)) return;
        const StripLayout strips = layoutStrips(local_);
        tree_ = strips.tree;
        const std::string expandedKey = expandedGroupKey();

        for (const auto& st : strips.tabs) {
            if (!st.rect.contains(p)) continue;
            s.hoverLeaveMs = 0.0;
            if (expandedKey != st.key) Docking::autoShow(st.key);
            return;
        }

        ExpView exp;
        if (layoutExpanded(strips.tree, s.expandedActive, exp) && exp.rect.contains(p)) {
            s.hoverLeaveMs = 0.0;
            return;
        }
        if (expandedKey.empty()) {
            s.hoverLeaveMs = 0.0;
            return;
        }

        const double now = nowMs();
        if (s.hoverLeaveMs <= 0.0) {
            s.hoverLeaveMs = now;
        } else if (now - s.hoverLeaveMs >= kExpandedLeaveMs) {
            s.hoverLeaveMs = 0.0;
            Docking::collapseToEdge();
        }
    }

    void onUp(const PointerEvent& e) {
        if (e.type != PointerEventType::Up) return;
        const Point p = toLocal(e);
        DockDragState& s = *drag_;

        if (s.splitter.active) {
            s.splitter = SplitterDrag{};
            return;
        }
        if (s.floatDrag.active) {
            s.floatDrag = FloatDrag{};
            return;
        }
        if (s.press.active) {
            const PressClick pc = s.press;
            s.press = PressClick{};
            if (!pc.rect.contains(p)) return;
            switch (pc.kind) {
                case PressKind::TabClose:
                case PressKind::HeaderClose: Docking::close(pc.id); break;
                case PressKind::HeaderPin: Docking::hideToEdge(pc.id); break;
                case PressKind::HeaderMax: Docking::toggleMaximize(pc.id); break;
                // 边条 tab：已展开 → 收起，未展开 → 浮出（对齐 Kotlin
                // EdgeStripTab 的 clickHandler）。
                case PressKind::StripTab:
                    if (!pc.id.empty() && pc.id == expandedGroupKey())
                        Docking::collapseToEdge();
                    else
                        Docking::autoShow(pc.id);
                    break;
                case PressKind::FloatClose: Docking::close(pc.id); break;
                case PressKind::FloatDock: Docking::unfloatPanel(pc.id); break;
                // FloatTab / ExpandedTab：id = 组键，extra = 面板 id。
                case PressKind::FloatTab:
                    Docking::switchFloatingTab(pc.id, pc.extra);
                    break;
                case PressKind::ExpandedPin: Docking::restoreFromEdge(pc.id); break;
                case PressKind::ExpandedTab:
                    // 浮层内切换只改本地缓存（隐藏组的数据不动），manager 没有
                    // emit ⇒ 自己请求重绘。
                    s.expandedActive = pc.extra;
                    setState();
                    break;
            }
            return;
        }
        if (!s.tab.active) return;
        const TabDrag d = s.tab;
        s.tab = TabDrag{};

        if (d.isDragging) {
            if (d.hasHoverEdge) {
                // 越界自动隐藏：先把浮动组完整归树，再藏到边上。
                Docking::unfloatPanel(d.tabId);
                Docking::autoHide(d.tabId, d.hoverEdge);
            } else if (d.hasDrop) {
                executeTabDrop(d.tabId, d.drop);
            }
            // 无目标 ⇒ 保持浮动（拖拽启动时已 undock + float）。
            return;
        }
        if (d.isReorder) {
            if (d.reorderTo >= 0 && d.reorderTo != d.reorderFrom)
                Docking::reorderTabItem(d.tabId, d.reorderFrom, d.reorderTo);
            return;
        }
        // 未拖动：双击 ⇒ 最大化/还原；单击标签 ⇒ 切换（按下不切换）。
        const bool pending = s.lastHeaderT < 0.0 && s.lastHeaderId == d.tabId;
        if (pending) {
            s.lastHeaderId.clear();
            s.lastHeaderT = 0.0;
            Dockable* dockable = DockManager::instance().find(d.tabId);
            if (dockable && dockable->isMinMaxAllowed()) Docking::toggleMaximize(d.tabId);
        } else if (!d.otherIds.empty() && DockManager::instance().find(d.tabId)) {
            Docking::bringToFront(d.tabId);
        }
    }

    /// 面板本地坐标：background 底板 → 树 → 边缘条 → 展开浮层 → 浮动窗口，
    /// 最上面是拖拽浮层（绘制顺序对齐 rui paint()）。整块再包一层 `Listener`
    /// 收全部指针事件 —— 不用 `GestureDetector` 是因为它只在按下后才转发
    /// Move，边条的悬停展开收不到无按键的 Hover。
    SharedPtr<Widget> buildPanel(const BoxConstraints& c) {
        // 停靠面板始终由父级给紧约束（ide 的 dock 区 / gallery 的 Expanded）；
        // 无界约束下没有可排布的矩形，LayoutBuilder 会退化到 biggest()=1e18。
        if (!(c.maxWidth > 0.0) || !(c.maxHeight > 0.0) ||
            c.maxWidth >= BoxConstraints::kInfinity ||
            c.maxHeight >= BoxConstraints::kInfinity) {
            local_ = Rect{};
            tree_ = Rect{};
            return cui::make_shared<SizedBox>(0.0, 0.0);
        }

        const Rect local{0.0, 0.0, c.maxWidth, c.maxHeight};
        local_ = local;
        const StripLayout strips = layoutStrips(local);
        tree_ = strips.tree;

        std::vector<SharedPtr<Widget>> layers;
        if (auto root = DockManager::instance().root()) collectLayers(root, tree_, layers);

        const std::string expandedKey = expandedGroupKey();
        if (!strips.tabs.empty())
            layers.push_back(positionedRect(
                local, cui::make_shared<CustomPaint>(
                           stripsPainter(strips, expandedKey), nullptr, Size{0.0, 0.0}),
                "strips"));

        // 展开浮层：底盘 → 内容 → 头部 chrome → 文字（chrome 必须压住内容顶边）。
        ExpView exp;
        if (drag_ && layoutExpanded(tree_, drag_->expandedActive, exp)) {
            layers.push_back(positionedRect(
                local, cui::make_shared<CustomPaint>(expandedBasePainter(exp), nullptr,
                                                     Size{0.0, 0.0}),
                "exp-base"));
            layers.push_back(positionedRect(exp.body, overlayContent(exp.activeId),
                                            "exp-body:" + exp.key));
            layers.push_back(positionedRect(
                local, cui::make_shared<CustomPaint>(expandedChromePainter(exp), nullptr,
                                                     Size{0.0, 0.0}),
                "exp-chrome"));
            pushExpandedTexts(exp, layers);
        }

        const std::vector<FloatView> floats = layoutFloats(local);
        if (!floats.empty()) {
            layers.push_back(positionedRect(
                local, cui::make_shared<CustomPaint>(floatsBasePainter(floats), nullptr,
                                                     Size{0.0, 0.0}),
                "floats-base"));
            for (const auto& f : floats)
                layers.push_back(positionedRect(f.bodyRect, overlayContent(f.activeId),
                                                "float-body:" + f.key));
            layers.push_back(positionedRect(
                local, cui::make_shared<CustomPaint>(floatsChromePainter(floats), nullptr,
                                                     Size{0.0, 0.0}),
                "floats-chrome"));
            for (const auto& f : floats) pushFloatTexts(f, layers);
        }

        if (drag_ && (drag_->tab.isDragging || drag_->tab.isReorder)) {
            layers.push_back(positionedRect(
                local, cui::make_shared<CustomPaint>(
                           dragOverlayPainter(drag_, local_), nullptr, Size{0.0, 0.0})));
            if (drag_->tab.isDragging) {
                const Rect chip = ghostChipRect(drag_->tab.tabId, drag_->tab.currentPos);
                auto txt = cui::make_shared<Text>(
                    tabLabelOf(drag_->tab.tabId),
                    TextStyle().size(kTabFont).fgColor(theme().textPrimary));
                layers.push_back(positionedRect(
                    Rect{chip.x + 8.0, chip.y, std::max(0.0, chip.width - 8.0), chip.height},
                    cui::make_shared<Align>(-1.0, 0.0, std::move(txt))));
            }
        }

        auto stack = cui::make_shared<Stack>(
            std::move(layers), nullptr, RenderStack::Alignment::TopStart,
            RenderStack::Fit::Expand);
        auto bg = cui::make_shared<Container>(std::move(stack));
        bg->decoration(BoxDecoration{theme().background});
        return cui::make_shared<Listener>(
            [this](const PointerEvent& e) { onDown(e); },
            [this](const PointerEvent& e) { onMove(e); },
            [this](const PointerEvent& e) { onUp(e); },
            [this](const PointerEvent& e) { onHoverMove(e); }, nullptr, std::move(bg));
    }

    int handle_ = 0;
    DockDragPtr drag_;
    Rect local_;  ///< 最近一次布局的面板本地矩形（手势坐标系的基准）
    Rect tree_;   ///< 边条占位后的树区域（排布与命中都以它为准）
};

std::unique_ptr<State> RootDockingPanel::createState() {
    return std::make_unique<RootDockingPanelState>();
}

}  // namespace cui_core
