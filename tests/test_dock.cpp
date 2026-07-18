/// @file test_dock.cpp
/// @brief 停靠框架（widgets/dock.h + widgets/dock.cpp）单元测试。
///
/// 覆盖（对齐 rui docking.rs 的状态机口径）：
///   1. WindowLayoutBuilder 回放：条目式描述 → DockManager 建树，Split 恰好两个
///      子节点 + 一个 dividerRatio；Center 相对停靠并入标签组并**立刻激活新面板**。
///   2. dock / undock / close / display / bringToFront / setDividerRatio 语义。
///   3. 浮动：整组浮出、树位置快照、恢复后分割比例与隐藏前一致。
///   4. 自动隐藏：隐藏到指定边缘 → 恢复回原兄弟旁。
///   5. 最大化：树被换成单面板、原树进字符串快照，restore 精确复原。
///   6. 序列化：saveFullState / restoreFullState 往返（mdl1 + hidden/float 行）。
///   7. 监听器句柄：布局/事件/最大化三组各自的挂载-摘除计数。
///   8. RootDockingPanel 订阅单例与元素树可见性（hide / maximize）。
///   9. 分割条手势：中心线 ±3px 命中、拖拽实时改比例、越界夹取、400ms 内
///      同点二次按下复位 0.5、嵌套分割按结构路径寻址。
///  10. 标签与 chrome 按钮：按下不切换/释放才切换、× 需按下与释放同矩形、
///      单面板标题栏空白双击最大化、再双击还原。
///  11. 标签拖拽：组内横向重排、拖出标签条转成浮动、根级 Left 落点停靠、
///      越界 40px 内自动隐藏到该边、isFloatingAllowed=false 时不启动拖拽。
///
/// 全部 headless：帧泵不做 flushPaint()（需真实 SkCanvas）。

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/dock/dock.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/painter.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/text.h"

using namespace cui;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

/// 把 Widget 树接入 BuildOwner + RenderView + PipelineOwner 并布局好。
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        owner.adoptRootElement(w->createElement(w));
        root = owner.rootElement();
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        po.flushLayout();
    }
    void rebuildAndLayout() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    // ── 指针事件 ────────────────────────────────────────────────────────────
    // Down 现场命中测试并按 pointerId 缓存命中链，Move/Up 复用同一条链 ——
    // 所以拖拽途中子树重建（undock ⇒ 分组换人）不会让事件失联，坐标也能跑到
    // 面板矩形之外（越界自动隐藏那条就是这么触发的）。
    void down(double x, double y) {
        PointerDownEvent e;
        e.position = {x, y};
        e.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(e);
        rebuildAndLayout();
    }
    void drag(double x, double y) {
        PointerMoveEvent e;
        e.position = {x, y};
        e.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(e);
        rebuildAndLayout();
    }
    void up(double x, double y) {
        PointerUpEvent e;
        e.position = {x, y};
        po.dispatchPointerEvent(e);
        rebuildAndLayout();
    }
    /// 无按键的悬停移动：`Listener` 的第四路回调（onHover），边条展开/延时
    /// 收起全靠它 —— GestureDetector 只在 paning 时转发 Move，走不到这条路。
    void hover(double x, double y) {
        PointerHoverEvent e;
        e.position = {x, y};
        po.dispatchPointerEvent(e);
        rebuildAndLayout();
    }
};

/// 测试用面板：内容就是一段文本，标签文本用 tab_ 前缀便于计数。
/// 四个能力开关默认与 `Dockable` 一致（可关、可浮、不可藏、不可最大化），
/// 手势用例按需打开 —— 渲染层的按钮矩形、重排预留都由它们决定。
class TestDockable : public Dockable {
public:
    TestDockable(std::string id, std::string label)
        : id_(std::move(id)), label_(std::move(label)) {}

    std::string persistentID() const override { return id_; }
    std::string tabText() const override { return "tab_" + id_; }
    SharedPtr<Widget> content() const override {
        return cui::make_shared<Text>(label_);
    }

    void setFlags(bool closable, bool floatable, bool autoHide, bool minMax) {
        closable_ = closable;
        floatable_ = floatable;
        autoHide_ = autoHide;
        minMax_ = minMax;
    }
    bool isClosable() const override { return closable_; }
    bool isFloatingAllowed() const override { return floatable_; }
    bool isAutoHideAllowed() const override { return autoHide_; }
    bool isMinMaxAllowed() const override { return minMax_; }

private:
    std::string id_;
    std::string label_;
    bool closable_ = true, floatable_ = true, autoHide_ = false, minMax_ = false;
};

/// 统计元素树里 data == text 的 Text 控件个数。
int countText(Element* root, const std::string& text) {
    int n = 0;
    std::function<void(Element*)> walk = [&](Element* e) {
        if (!e)
            return;
        if (auto* t = dynamic_cast<Text*>(e->widget())) {
            if (t->data() == text)
                ++n;
        }
        e->visitChildren([&](Element* c) { walk(c); });
    };
    walk(root);
    return n;
}

/// 注册一组面板，返回拥有者（析构前需 unregisterPanels）。
std::vector<std::unique_ptr<TestDockable>> registerPanels(
    const std::vector<std::string>& ids) {
    std::vector<std::unique_ptr<TestDockable>> owners;
    for (const auto& id : ids) {
        owners.push_back(std::make_unique<TestDockable>(id, id));
        Docking::registerDockable(owners.back().get());
    }
    return owners;
}

void unregisterPanels(std::vector<std::unique_ptr<TestDockable>>& owners) {
    for (auto& o : owners)
        Docking::unregisterDockable(o.get());
    owners.clear();
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// ── 1/2：建树与停靠操作 ─────────────────────────────────────────────────────
void testLayoutTree() {
    std::printf("--- WindowLayoutBuilder 回放与停靠 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "output", "err"});

    WindowLayoutBuilder b;
    b.dock("editor", DockingRegion::Center);
    b.dock("output", "editor", DockingRegion::Bottom, 0.3);
    b.dock("err", "output", DockingRegion::Center);  // 并入 output 所在组
    check(b.entries().size() == 3, "builder 只记录操作序列（3 条）");
    Docking::applyLayout(b);

    SharedPtr<DockNode> root = DockManager::instance().root();
    check(root != nullptr, "applyLayout 产出根节点");
    check(root && root->isSplit(), "editor 与 output 拆成 Split");
    check(root && root->orientation == SplitOrientation::Vertical, "Bottom 走纵向分割");
    check(root && root->first && root->first->isTabGroup() && root->first->items.size() == 1,
          "first 是 editor 单标签组");
    check(root && root->second && root->second->items.size() == 2,
          "Center 相对停靠并入同组（2 个标签）");
    check(root && root->first && root->first->groupId() == "editor",
          "标签组标识取组内首个面板 id");
    // Bottom 的 ratio 是新面板占的份额 ⇒ first（editor）拿 1-ratio。
    check(root && near(root->dividerRatio, 0.7), "dividerRatio = 1 - 0.3");

    // rui 口径：Center 并入时新面板立刻成为活动标签。
    check(DockManager::instance().activeTabOfGroup("output") == "err",
          "Center 并入的面板立刻激活");
    Docking::bringToFront("output");
    check(DockManager::instance().activeTabOfGroup("output") == "output",
          "bringToFront 改活动标签");
    check(DockManager::instance().maximized().empty(), "bringToFront 不影响最大化态");
    check(!DockManager::instance().isMaximized("output"), "isMaximized 默认 false");

    Docking::setDividerRatio("output", 0.4);
    check(root && near(root->dividerRatio, 0.7), "旧 root 句柄不受就地更新影响");
    check(near(DockManager::instance().root()->dividerRatio, 0.4), "setDividerRatio 改比例");

    // 结构路径同样能定位分割节点（分割条拖拽走这条）。
    DockManager::instance().setSplitRatioByPath({}, 0.6);
    check(near(DockManager::instance().root()->dividerRatio, 0.6), "空路径 = 根分割节点");
    DockManager::instance().setSplitRatioByPath({0}, 0.5);
    check(near(DockManager::instance().root()->dividerRatio, 0.6),
          "路径末端不是分割节点 ⇒ 就地更新不生效");

    // 嵌套分割要靠路径才能消歧（基于 id 的 setDividerRatio 分不清两个兄弟）。
    DockManager::instance().setSplitRatioByPath({1, 0}, 0.4);
    check(DockManager::instance().root()->second &&
              near(DockManager::instance().root()->second->dividerRatio, 0.5) &&
              near(DockManager::instance().root()->dividerRatio, 0.6),
          "路径 {1,0} 仍停在标签组上 ⇒ 不改比例");

    check(DockManager::instance().isDocked("editor"), "editor 已停靠");
    Docking::undock("editor");
    check(!DockManager::instance().isDocked("editor"), "undock 移出树但保留注册");
    check(DockManager::instance().isRegistered("editor"), "undock 不注销");
    Docking::display("editor");
    check(DockManager::instance().isDocked("editor"), "display 未停靠 ⇒ 停靠到中心");
    check(DockManager::instance().isRegistered("editor"), "display 后仍注册");

    Docking::close("err");
    check(!DockManager::instance().isDocked("err"), "close 移出树");
    check(DockManager::instance().isRegistered("err"), "close 不注销");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

// ── 3：浮动 ────────────────────────────────────────────────────────────────
void testFloating() {
    std::printf("--- 浮动与恢复原位 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "output", "err"});

    WindowLayoutBuilder b;
    b.dock("editor", DockingRegion::Center);
    b.dock("output", "editor", DockingRegion::Bottom, 0.3);
    b.dock("err", "output", DockingRegion::Center);
    Docking::applyLayout(b);

    Docking::floatPanel("editor");
    check(DockManager::instance().isFloating("editor"), "floatPanel 进浮动表");
    check(!DockManager::instance().isDocked("editor"), "浮动面板离开布局树");
    check(DockManager::instance().floatingKeyFor("editor") == "editor", "浮动键 = 首个 ID");
    const auto& f = DockManager::instance().floatingPanels();
    auto it = f.find("editor");
    check(it != f.end() && near(it->second.width, 400.0) && near(it->second.height, 300.0),
          "默认浮动尺寸 400×300");
    check(it != f.end() && it->second.savedPosition.valid &&
              it->second.savedPosition.siblingId == "output" &&
              near(it->second.savedPosition.dividerRatio, 0.7),
          "浮动前记录原树位置");
    // editor 单独成组 ⇒ 浮动只带走它；output 组仍在树里且只剩 Split 塌缩后的一个组。
    SharedPtr<DockNode> root = DockManager::instance().root();
    check(root && root->isTabGroup() && root->items.size() == 2, "Split 塌缩成 output 标签组");

    DockManager::instance().updateFloatingPosition("editor", 30.0, 40.0);
    DockManager::instance().updateFloatingSize("editor", 50.0, 50.0);  // 小于下限
    check(near(f.at("editor").offsetX, 30.0) && near(f.at("editor").offsetY, 40.0),
          "updateFloatingPosition 写偏移");
    check(near(f.at("editor").width, 200.0) && near(f.at("editor").height, 150.0),
          "updateFloatingSize 夹到 200×150 下限");
    DockManager::instance().switchFloatingTab("editor", "editor");
    check(f.at("editor").activeId == "editor", "switchFloatingTab 记录活动页");

    Docking::unfloatPanel("editor");
    check(!DockManager::instance().isFloating("editor"), "unfloat 移出浮动表");
    root = DockManager::instance().root();
    check(root && root->isSplit() && root->first && root->first->items.size() == 1 &&
              root->first->items.front() == "editor",
          "unfloat 按原兄弟位置插回");
    check(root && near(root->dividerRatio, 0.7), "恢复后分割比例与浮动前一致");

    // display：已浮动 ⇒ 前置而不是重新停靠。
    Docking::floatPanel("editor");
    Docking::display("editor");
    check(DockManager::instance().isFloating("editor"), "display 对浮动面板不改浮动态");
    Docking::unfloatPanel("editor");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

// ── 4：自动隐藏 ─────────────────────────────────────────────────────────────
void testAutoHide() {
    std::printf("--- 自动隐藏与从边缘恢复 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "solution", "properties"});

    WindowLayoutBuilder b;
    b.dock("editor", DockingRegion::Center);
    b.dock("solution", "editor", DockingRegion::Left, 0.2);
    b.dock("properties", "editor", DockingRegion::Right, 0.25);
    Docking::applyLayout(b);

    Docking::autoHide("solution", DockEdge::Left);
    check(DockManager::instance().isHidden("solution"), "autoHide 置隐藏");
    check(!DockManager::instance().isDocked("solution"), "隐藏面板离开布局树");
    check(DockManager::instance().hiddenKeyFor("solution") == "solution", "隐藏键 = 首个 ID");
    check(DockManager::instance().hiddenPanels().at("solution").edge == DockEdge::Left,
          "记录的边缘 = 指定边缘");

    Docking::restoreFromEdge("solution");
    check(!DockManager::instance().isHidden("solution"), "restoreFromEdge 复位");
    check(DockManager::instance().isDocked("solution"), "恢复后回到树里");
    // 原兄弟 editor 仍在树里 ⇒ 按 Top/Left 语义插回原位，比例不变。
    check(near(DockManager::instance().findTreePosition("solution").dividerRatio, 0.2),
          "恢复后分割比例与隐藏前一致");

    // toggle 与 hideToEdge（边缘由树内矩形推出）。
    Docking::toggleAutoHide("properties");
    check(DockManager::instance().isHidden("properties"), "toggleAutoHide 隐藏");
    Docking::toggleAutoHide("properties");
    check(!DockManager::instance().isHidden("properties"), "toggleAutoHide 再按恢复");

    Docking::hide("editor");
    check(DockManager::instance().isHidden("editor"), "hide == hideToEdge");
    Docking::display("editor");
    check(!DockManager::instance().isHidden("editor"), "display 对隐藏面板走恢复");

    // 展开浮出只改 expandedPanelId，不动树。
    Docking::hide("solution");
    Docking::autoShow("solution");
    check(DockManager::instance().expandedPanelId() == "solution", "autoShow 记展开面板");
    Docking::collapseToEdge();
    check(DockManager::instance().expandedPanelId().empty(), "collapseToEdge 清空展开态");
    Docking::restoreFromEdge("solution");

    // findEdgeOfPanel：最左的分组必然贴左边缘。
    check(DockManager::instance().findEdgeOfPanel("solution") == DockEdge::Left,
          "findEdgeOfPanel 取最近边缘");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

// ── 5：最大化 ──────────────────────────────────────────────────────────────
void testMaximize() {
    std::printf("--- 最大化快照与还原 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "output"});

    WindowLayoutBuilder b;
    b.dock("editor", DockingRegion::Center);
    b.dock("output", "editor", DockingRegion::Bottom, 0.3);
    Docking::applyLayout(b);

    SharedPtr<DockNode> before = DockManager::instance().root();
    Docking::maximize("output");
    check(DockManager::instance().maximized() == "output", "maximize 记录 id");
    SharedPtr<DockNode> mxRoot = DockManager::instance().root();
    check(mxRoot && mxRoot->isTabGroup() && mxRoot->items.size() == 1 &&
              mxRoot->items.front() == "output",
          "最大化把树换成单面板标签组");
    Docking::maximize("editor");
    check(DockManager::instance().maximized() == "output", "已最大化时再次 maximize 无效");

    Docking::toggleMaximize("editor");
    check(DockManager::instance().maximized() == "output", "toggle 别的 id 不改变最大化态");
    Docking::toggleMaximize("output");
    check(DockManager::instance().maximized().empty(), "toggle 同一 id ⇒ 还原");
    check(DockManager::instance().root() &&
              DockManager::instance().root()->isSplit() == before->isSplit(),
          "restore 复原分割树");
    check(near(DockManager::instance().root()->dividerRatio, 0.7), "restore 复原分割比例");

    Docking::maximize("editor");
    Docking::restore();
    check(DockManager::instance().maximized().empty(), "restore 清空最大化");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

// ── 6：序列化 ──────────────────────────────────────────────────────────────
void testSerialization() {
    std::printf("--- 序列化往返 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "output", "err", "solution"});

    WindowLayoutBuilder b;
    b.dock("editor", DockingRegion::Center);
    b.dock("output", "editor", DockingRegion::Bottom, 0.3);
    b.dock("err", "output", DockingRegion::Center);
    b.dock("solution", "editor", DockingRegion::Left, 0.2);
    Docking::applyLayout(b);
    Docking::bringToFront("output");

    // 四层布局是 Split(V) 套 Split(H) 套两个标签组 ⇒ 树串两种节点都出现。
    std::string tree = DockManager::instance().saveLayoutState();
    check(!tree.empty(), "saveLayoutState 非空");
    check(tree.find("T(") != std::string::npos && tree.find("S(") != std::string::npos,
          "树串含 T() 与 S()");

    Docking::autoHide("solution", DockEdge::Left);
    Docking::floatPanel("editor");

    std::string full = DockManager::instance().saveFullState();
    check(full.rfind("mdl1\n", 0) == 0, "完整状态带 mdl1 头");
    check(full.find("hidden=") != std::string::npos, "完整状态含 hidden 行");
    check(full.find("float=") != std::string::npos, "完整状态含 float 行");

    DockManager::instance().reset();
    check(!DockManager::instance().root(), "reset 清树");
    check(Docking::loadLayout(full), "loadLayout 还原成功");
    check(DockManager::instance().isHidden("solution"), "hidden 组还原");
    check(DockManager::instance().isFloating("editor"), "float 组还原");
    SharedPtr<DockNode> root = DockManager::instance().root();
    // solution 在 hidden 表、editor 在 float 表 ⇒ 树里只剩 output 组（含并入的 err）。
    check(root && root->isTabGroup() && root->collectIds().size() == 2,
          "树里只剩未隐藏/未浮动的面板");
    check(DockManager::instance().activeTabOfGroup("output") == "output",
          "活动标签随树串还原");

    // 未注册的面板在解析期被过滤掉（旧文件里的面板不该复活）。解析失败本身
    // 不算读档失败（对齐 rui restore_full_state：状态字段照样接受）。
    check(Docking::loadLayout("mdl1\nT(ghost|ghost)\n"), "全未注册的树串仍返回成功");
    check(!DockManager::instance().root(), "未注册的面板不复活");
    check(DockManager::instance().isRegistered("editor"), "读档不清注册表");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

// ── 7：监听器句柄 ──────────────────────────────────────────────────────────
void testListeners() {
    std::printf("--- 监听器句柄与事件派发 ---\n");
    DockManager::instance().reset();
    DockManager& mgr = DockManager::instance();

    int dockedEvents = 0;
    int layoutEvents = 0;
    int maximizeEvents = 0;
    std::string lastEventId;
    DockingEventType lastEventType = DockingEventType::Docked;

    int eh = mgr.addDockingListener([&](const DockingEvent& e) {
        lastEventId = e.dockableId;
        lastEventType = e.eventType;
        if (e.eventType == DockingEventType::Docked) ++dockedEvents;
    });
    int lh = mgr.addListener([&] { ++layoutEvents; });
    int mh = mgr.addMaximizeListener([&](const std::string&, bool) { ++maximizeEvents; });
    check(mgr.dockingListenerCount() == 1 && mgr.listenerCount() == 1 &&
              mgr.maximizeListenerCount() == 1,
          "三组监听器各 1 个");

    auto owners = registerPanels({"a", "b"});
    Docking::dock("a", DockingRegion::Center, 0.5);
    check(dockedEvents == 1 && lastEventId == "a" && layoutEvents >= 1,
          "dock 派发 Docked 事件并重发布局变更");
    Docking::maximize("a");
    check(maximizeEvents == 1 && lastEventType == DockingEventType::Maximized,
          "maximize 另走最大化监听器");
    Docking::restore();
    check(maximizeEvents == 2 && lastEventType == DockingEventType::Restored,
          "restore 同样通知最大化监听器");

    mgr.removeListener(lh);
    mgr.removeDockingListener(eh);
    mgr.removeMaximizeListener(mh);
    int before = layoutEvents;
    Docking::dock("b", DockingRegion::Right, 0.3);
    check(layoutEvents == before, "removeListener 后不再收布局变更");
    check(mgr.listenerCount() == 0 && mgr.dockingListenerCount() == 0 &&
              mgr.maximizeListenerCount() == 0,
          "按句柄摘除后计数归零");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

// ── 8：RootDockingPanel 订阅与可见性 ───────────────────────────────────────
void testPanelSubscription() {
    std::printf("--- RootDockingPanel 监听器生命周期与可见性 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"left", "main"});

    WindowLayoutBuilder b;
    b.dock("main", DockingRegion::Center);
    b.dock("left", "main", DockingRegion::Left, 0.25);
    Docking::applyLayout(b);

    auto panel = cui::make_shared<RootDockingPanel>();
    check(DockManager::instance().listenerCount() == 0, "挂载前无监听器");
    {
        Harness h;
        h.attach(panel, 800, 600);
        h.rebuildAndLayout();

        check(DockManager::instance().listenerCount() == 1, "initState 注册恰好 1 个监听器");
        check(countText(h.root, "tab_main") == 1, "main 分组渲染出标签");
        check(countText(h.root, "tab_left") == 1, "left 分组渲染出标签");
        check(countText(h.root, "left") == 1, "left 面板内容进入元素树");
        check(countText(h.root, "main") == 1, "main 面板内容进入元素树");

        // 标签点击 → bringToFront ⇒ 活动标签变色，内容换成另一面板。
        Docking::bringToFront("left");
        h.rebuildAndLayout();
        check(DockManager::instance().activeTabOfGroup("left") == "left",
              "分组内活动标签即自身");

        // hide 走 emitLayoutChanged → setState → 重建：被隐藏面板消失。
        Docking::hide("left");
        h.rebuildAndLayout();
        check(countText(h.root, "tab_left") == 0, "hide 后标签不再重建");
        check(countText(h.root, "left") == 0, "hide 后面板内容消失");
        Docking::display("left");
        h.rebuildAndLayout();
        check(countText(h.root, "tab_left") == 1, "display 后标签回来");

        // 最大化：树被换成单面板组 ⇒ 只剩该面板的标签与内容。
        Docking::maximize("left");
        h.rebuildAndLayout();
        check(countText(h.root, "left") == 1 && countText(h.root, "main") == 0,
              "最大化只渲染该面板");
        check(countText(h.root, "tab_main") == 0, "最大化不画别的面板");
        Docking::restore();
        h.rebuildAndLayout();
        check(countText(h.root, "tab_main") == 1, "restore 后回到分组布局");
    }
    // 树已析构：单例不得再握着任何指向已销毁 State 的回调。
    check(DockManager::instance().listenerCount() == 0, "dispose 摘除监听器");
    DockManager::instance().refresh();  // 悬挂回调会在此处炸；无监听器时是空操作
    check(true, "卸载后 refresh() 不再触碰已销毁的 State");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

void testReattachIsNotDuplicated() {
    std::printf("--- 重新挂载不泄漏监听器 ---\n");
    DockManager::instance().reset();
    auto panel = cui::make_shared<RootDockingPanel>();

    for (int i = 0; i < 3; ++i) {
        Harness h;
        h.attach(panel, 400, 400);
        h.rebuildAndLayout();
        check(DockManager::instance().listenerCount() == 1, "每一轮挂载只持有 1 个监听器");
    }
    check(DockManager::instance().listenerCount() == 0, "全部卸载后监听器归零");
    DockManager::instance().reset();
}

// ── 9/10/11：面板级手势 ─────────────────────────────────────────────────────
//
// 测试里的度量和 dock.cpp 渲染侧同源（同一套公式，不是抄数字）：
//   splitGap 4 ⇒ 分割节点两子各让出 2px；headerHeight 30；TabMode::Bottom
//   ⇒ 头部贴组的底边；标签宽 = max(文字宽 + 2*9, 48)，活动且可关闭再 +24；
//   标签间隔 2、条内左边距 4；按钮 22×22，by = header.y + 4。
// 800×600 的窗口下标签条远未溢出，layoutTabRects 的压缩分支不参与。

constexpr double kHdrH = 30.0;
constexpr double kSplitGap = 4.0;
constexpr double kTabFontPx = 12.0;
constexpr double kTabMinW = 48.0;
constexpr double kTabGap = 2.0;
constexpr double kBtnW = 22.0;

double tabW(const std::string& label) {
    return std::max(Painter::measureTextWidth(nullptr, label, kTabFontPx) + 18.0, kTabMinW);
}

/// 纵向分割的两子矩形（分割条居中）。
std::pair<Rect, Rect> vSplit(const Rect& r, double ratio) {
    const double fh = r.height * ratio - kSplitGap / 2.0;
    const double sh = r.height * (1.0 - ratio) - kSplitGap / 2.0;
    return {Rect{r.x, r.y, r.width, fh}, Rect{r.x, r.y + fh + kSplitGap, r.width, sh}};
}

/// TabMode::Bottom ⇒ 头部在组的底边（30 高）。
Rect bottomHeader(const Rect& group) {
    return Rect{group.x, group.y + group.height - kHdrH, group.width, kHdrH};
}

/// 第 index 个标签的矩形。
Rect tabRect(const std::vector<std::string>& items, const std::string& activeId, size_t index,
             const Rect& header) {
    const double th = header.height - 4.0;
    const double ty = header.y + (header.height - th) / 2.0;
    double x = header.x + 4.0;
    for (size_t i = 0; i < index; ++i) {
        x += tabW("tab_" + items[i]) + (items[i] == activeId ? kBtnW + 2.0 : 0.0) + kTabGap;
    }
    const double w =
        tabW("tab_" + items[index]) + (items[index] == activeId ? kBtnW + 2.0 : 0.0);
    return Rect{x, ty, w, th};
}

/// editor 在上、output/err 两标签在下的三面板布局。
WindowLayoutBuilder threePanelLayout() {
    WindowLayoutBuilder b;
    b.dock("editor", DockingRegion::Center);
    b.dock("output", "editor", DockingRegion::Bottom, 0.3);
    b.dock("err", "output", DockingRegion::Center);
    return b;
}

/// 面板 + 布局的最小装配：返回挂在树上的 Harness（调用方负责先 unregister）。
struct Rig {
    Harness h;
    SharedPtr<RootDockingPanel> panel = cui::make_shared<RootDockingPanel>();

    Rig() {
        h.attach(panel, 800, 600);
        h.rebuildAndLayout();
    }
};

void testSplitterGestures() {
    std::printf("--- 手势：分割条拖拽、夹取与双击复位 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "output", "err", "solution"});
    Rig rig;

    Docking::applyLayout(threePanelLayout());
    const double band = 600.0 * 0.7;  // 根分割中心线 y = 420（命中 ±3）
    rig.h.down(400.0, band);
    check(near(DockManager::instance().root()->dividerRatio, 0.7), "按下分割条不改比例");
    rig.h.drag(400.0, 300.0);
    check(near(DockManager::instance().root()->dividerRatio, 0.5), "拖到 y=300 ⇒ ratio=0.5");
    rig.h.up(400.0, 300.0);
    check(near(DockManager::instance().root()->dividerRatio, 0.5), "抬手不改值");

    rig.h.down(400.0, 300.0);
    rig.h.drag(400.0, 700.0);
    check(near(DockManager::instance().root()->dividerRatio, 1.0), "越界拖拽夹到 1.0");
    rig.h.up(400.0, 700.0);

    // 双击复位：两次按下同一条（±6px、400ms 内）⇒ 该分割回到 0.5。
    DockManager::instance().setSplitRatioByPath({}, 0.7);
    rig.h.down(400.0, band);
    rig.h.up(400.0, band);
    check(near(DockManager::instance().root()->dividerRatio, 0.7), "第一次按下只记时间戳");
    rig.h.down(400.0, band);
    check(near(DockManager::instance().root()->dividerRatio, 0.5), "同点二次按下 ⇒ 复位 0.5");
    rig.h.up(400.0, band);

    // 嵌套分割用结构路径寻址：基于 id 的 setDividerRatio 分不清兄弟分割。
    WindowLayoutBuilder nb;
    nb.dock("editor", DockingRegion::Center);
    nb.dock("output", "editor", DockingRegion::Bottom, 0.3);
    nb.dock("solution", "editor", DockingRegion::Left, 0.2);
    Docking::applyLayout(nb);
    const Rect upper = vSplit(Rect{0, 0, 800, 600}, 0.7).first;
    const double innerX = upper.x + upper.width * 0.2;  // 160
    const double innerY = upper.y + upper.height * 0.5;
    rig.h.down(innerX, innerY);
    rig.h.drag(240.0, innerY);
    rig.h.up(240.0, innerY);
    SharedPtr<DockNode> root = DockManager::instance().root();
    check(root && root->first && near(root->first->dividerRatio, 0.3),
          "路径 {0} 只改内层分割（240/800=0.3）");
    check(root && near(root->dividerRatio, 0.7), "外层分割比例不受影响");

    Docking::close("solution");
    unregisterPanels(owners);
    DockManager::instance().reset();
}

void testTabClickGestures() {
    std::printf("--- 手势：标签切换、chrome 按钮与标题栏双击 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "output", "err"});
    Rig rig;

    Docking::applyLayout(threePanelLayout());
    const Rect lower = vSplit(Rect{0, 0, 800, 600}, 0.7).second;
    const Rect header = bottomHeader(lower);
    const std::vector<std::string> items{"output", "err"};
    const Rect tab0 = tabRect(items, "err", 0, header);
    const Point tab0Center{tab0.x + tab0.width / 2.0, tab0.y + tab0.height / 2.0};

    // 按下不切换，释放才切换（对齐 rui press/release 分离）。
    rig.h.down(tab0Center.x, tab0Center.y);
    check(DockManager::instance().activeTabOfGroup("output") == "err", "按下时活动标签未变");
    rig.h.up(tab0Center.x, tab0Center.y);
    check(DockManager::instance().activeTabOfGroup("output") == "output", "释放才切换标签");

    // × 贴活动标签右缘：按下记录，释放落在别处 ⇒ 不关闭。
    const Rect active = tabRect(items, "output", 0, header);
    const double closeCx = active.x + active.width - kBtnW / 2.0;
    const double closeCy = header.y + 4.0 + (header.height - 8.0) / 2.0;
    rig.h.down(closeCx, closeCy);
    rig.h.up(closeCx + 200.0, closeCy);
    check(DockManager::instance().isDocked("output"), "释放在按钮外 ⇒ 不关闭");
    rig.h.down(closeCx, closeCy);
    rig.h.up(closeCx, closeCy);
    check(!DockManager::instance().isDocked("output"), "按下+释放同在 × 内 ⇒ 关闭");

    // 单面板标题栏空白处双击 ⇒ 最大化 / 再双击还原。
    owners[0]->setFlags(true, true, false, true);  // editor：允许最大化
    Docking::applyLayout(threePanelLayout());
    const Rect upperRect = vSplit(Rect{0, 0, 800, 600}, 0.7).first;
    const Rect upperHeader = bottomHeader(upperRect);
    const double blankX = upperHeader.x + 300.0;
    const double blankY = upperHeader.y + upperHeader.height / 2.0;
    rig.h.down(blankX, blankY);
    rig.h.up(blankX, blankY);
    check(DockManager::instance().maximized().empty(), "首次按下标题栏不大最大化");
    rig.h.down(blankX, blankY);
    rig.h.up(blankX, blankY);
    check(DockManager::instance().maximized() == "editor", "标题栏双击 ⇒ 最大化");
    // 最大化后树只剩一个组、头部移到窗口底边。
    const Rect mxHeader = bottomHeader(Rect{0, 0, 800, 600});
    rig.h.down(mxHeader.x + 300.0, mxHeader.y + mxHeader.height / 2.0);
    rig.h.up(mxHeader.x + 300.0, mxHeader.y + mxHeader.height / 2.0);
    rig.h.down(mxHeader.x + 300.0, mxHeader.y + mxHeader.height / 2.0);
    rig.h.up(mxHeader.x + 300.0, mxHeader.y + mxHeader.height / 2.0);
    check(DockManager::instance().maximized().empty(), "再双击 ⇒ 还原");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

void testTabDragGestures() {
    std::printf("--- 手势：组内重排、拖出浮动与越界自动隐藏 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "output", "err"});
    Rig rig;

    auto pressTab0 = [&]() {
        Docking::applyLayout(threePanelLayout());
        const Rect header = bottomHeader(vSplit(Rect{0, 0, 800, 600}, 0.7).second);
        const std::vector<std::string> items{"output", "err"};
        const Rect tab0 = tabRect(items, "err", 0, header);
        const Rect tab1 = tabRect(items, "err", 1, header);
        return std::make_pair(Point{tab0.x + tab0.width / 2.0, tab0.y + tab0.height / 2.0},
                              tab1.x + tab1.width / 2.0);
    };

    // 组内横向拖拽 ⇒ 重排（不 undock、不浮动）。
    auto [p0, tab1cx] = pressTab0();
    rig.h.down(p0.x, p0.y);
    rig.h.drag(tab1cx + 10.0, p0.y);
    rig.h.up(tab1cx + 10.0, p0.y);
    check(!DockManager::instance().isFloating("output"), "组内重排不浮动");
    check(DockManager::instance().tabGroupOf("err") == std::vector<std::string>{"err", "output"},
          "横向拖过另一标签中心 ⇒ 顺序换位");

    // 拖出标签条且纵向超过一个头部高度 ⇒ 退出重排、转成面板拖拽。
    auto [q0, q1cx] = pressTab0();
    rig.h.down(q0.x, q0.y);
    rig.h.drag(q1cx + 10.0, q0.y);
    rig.h.drag(q1cx + 10.0, q0.y + kHdrH + 20.0);
    check(!DockManager::instance().isDocked("output") &&
              DockManager::instance().isFloating("output"),
          "拖出标签条后 undock + 浮动");
    rig.h.up(q1cx + 10.0, q0.y + kHdrH + 20.0);
    check(DockManager::instance().isDocked("output") &&
              !DockManager::instance().isFloating("output"),
          "落回面板内 ⇒ 重新停靠");

    // 拖到根级 Left 区域（头部条上命中不到内容 ⇒ 退化为根级落点）。
    auto [r0, r1cx] = pressTab0();
    const Point rootLeft{100.0,
                         bottomHeader(vSplit(Rect{0, 0, 800, 600}, 0.7).first).y + 10.0};
    rig.h.down(r0.x, r0.y);
    rig.h.drag(rootLeft.x, rootLeft.y);
    rig.h.up(rootLeft.x, rootLeft.y);
    SharedPtr<DockNode> root = DockManager::instance().root();
    check(DockManager::instance().isDocked("output"), "根级落点重新停靠");
    check(root && root->isSplit() && root->orientation == SplitOrientation::Horizontal &&
              root->first && root->first->isTabGroup() && root->first->items.front() == "output",
          "根级 Left ⇒ 新分割把 output 放最左");
    check(root && near(root->dividerRatio, 0.3), "根级非 Center 停靠比例 0.3");

    // 越界 ≤40px 且允许自动隐藏 ⇒ 藏到该边缘。
    owners[1]->setFlags(true, true, true, false);  // output：允许自动隐藏
    auto [s0, s1cx] = pressTab0();
    rig.h.down(s0.x, s0.y);
    rig.h.drag(-20.0, s0.y);
    check(DockManager::instance().isFloating("output"), "越界拖拽中仍是浮动");
    rig.h.up(-20.0, s0.y);
    check(DockManager::instance().isHidden("output"), "越界 20px ⇒ 自动隐藏");
    check(!DockManager::instance().isFloating("output"), "隐藏前先把浮动组归树");
    check(DockManager::instance().hiddenPanels().at("output").edge == DockEdge::Left,
          "藏到光标越界的那条边");

    // 不允许浮动的面板：拖多远都不摘出树。
    Docking::restoreFromEdge("output");
    owners[1]->setFlags(true, false, false, false);
    auto [t0, t1cx] = pressTab0();
    rig.h.down(t0.x, t0.y);
    rig.h.drag(t0.x, t0.y - 200.0);
    rig.h.up(t0.x, t0.y - 200.0);
    check(!DockManager::instance().isFloating("output"), "isFloatingAllowed=false ⇒ 不启动拖拽");
    check(DockManager::instance().isDocked("output"), "不可浮动的面板仍在树里");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

// ── 12/13/14：边条、展开浮层、浮动窗（批次 4）───────────────────────────────
// 几何全部按 layoutStrips / layoutExpanded / layoutFloats 的公式手算，坐标即
// 面板本地坐标（800×600 窗口，RootDockingPanel 撑满）。
//   边条：stripThickness=28，只有真藏着面板的边占位；tab 长 = max(文本宽
//   +2×8, 48)，从边带首端起算、间隔 2 ⇒ 单边隐藏时 tab 矩形 = (0,2,28,len)。
//   浮层：树区域（缩进后）的 0.32，头部恒在顶边 30 高，pin 在最右。
//   浮窗：offset(100,100)、400×300，标题 30；两标签组再加一行 30 高标签，
//   左边距 K_TAB_PAD=9；dock/close 各 22×22 靠右上，把手 16×16 靠右下。

constexpr double kStripT = 28.0;      ///< DockingSettings::stripThickness
constexpr double kStripPad = 8.0;     ///< K_STRIP_TAB_PAD
constexpr double kFlyoutFrac = 0.32;  ///< K_FLYOUT_FRAC
constexpr double kTabInnerPad = 9.0;  ///< K_TAB_PAD（浮层/浮窗标签行的左边距）
constexpr double kWinW = 800.0;
constexpr double kWinH = 600.0;
constexpr double kFloatX = 100.0;
constexpr double kFloatY = 100.0;
constexpr double kFloatW = 400.0;
constexpr double kFloatH = 300.0;
constexpr double kTabRowMinW = 60.0;  ///< 浮层/浮窗标签的最小宽

/// 边条 tab 的沿边长度（与 layoutStrips 同式）。
double stripLen(const std::string& label) {
    return std::max(Painter::measureTextWidth(nullptr, label, kTabFontPx) + kStripPad * 2.0,
                    kTabMinW);
}

/// editor 在上、output/err 两标签在下、solution 在左的四面板布局。
WindowLayoutBuilder fourPanelLayout() {
    WindowLayoutBuilder b;
    b.dock("editor", DockingRegion::Center);
    b.dock("solution", "editor", DockingRegion::Left, 0.25);
    b.dock("output", "editor", DockingRegion::Bottom, 0.3);
    b.dock("err", "output", DockingRegion::Center);
    return b;
}

void testStripAndFlyout() {
    std::printf("--- 手势：auto-hide 边条与展开浮层 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "output", "err", "solution"});
    for (int i = 1; i < 4; ++i) owners[i]->setFlags(true, true, true, false);
    Rig rig;

    // ── A：单组藏左边 ──
    Docking::applyLayout(threePanelLayout());
    Docking::autoHide("output", DockEdge::Left);
    check(DockManager::instance().isHidden("output"), "隐藏整组：output 进隐藏表");
    check(!DockManager::instance().isDocked("err"), "同组的 err 一起摘出树");

    const double len = stripLen("tab_output");
    const double stripCx = kStripT / 2.0;
    const double stripCy = 2.0 + len / 2.0;
    const double treeX = kStripT, treeW = kWinW - kStripT;
    const double flyW = treeW * kFlyoutFrac;
    const double pinCx = treeX + flyW - 4.0 - kBtnW / 2.0;
    const double pinCy = 4.0 + (kHdrH - 8.0) / 2.0;

    // 悬停边条 ⇒ 立即浮出（rui on_hover_move_impl 第一段）。
    rig.h.hover(stripCx, stripCy);
    check(DockManager::instance().expandedPanelId() == "output", "悬停边条 ⇒ 展开该隐藏组");

    // 离开浮层：第一条只是起计时，满 300ms 的第二条才收。
    rig.h.hover(treeX + 300.0, 300.0);
    check(DockManager::instance().expandedPanelId() == "output", "刚离开浮层不立即收起");
    std::this_thread::sleep_for(std::chrono::milliseconds(320));
    rig.h.hover(treeX + 300.0, 300.0);
    check(DockManager::instance().expandedPanelId().empty(), "离开满 300ms ⇒ 收回边条");

    // 点边条 tab：未展开 ⇒ 浮出；再点 ⇒ 收起（走「浮层外按下」那条，同样收）。
    rig.h.down(stripCx, stripCy);
    rig.h.up(stripCx, stripCy);
    check(DockManager::instance().expandedPanelId() == "output", "点边条 tab ⇒ 浮出");
    rig.h.down(stripCx, stripCy);
    rig.h.up(stripCx, stripCy);
    check(DockManager::instance().expandedPanelId().empty(), "再点 ⇒ 收起");

    // pin ⇒ 归树并清隐藏表（边条随之消失，树回到整幅）。
    rig.h.hover(stripCx, stripCy);
    rig.h.down(pinCx, pinCy);
    rig.h.up(pinCx, pinCy);
    check(DockManager::instance().hiddenPanels().empty(), "pin ⇒ 清除隐藏表");
    check(DockManager::instance().isDocked("output") && DockManager::instance().isDocked("err"),
          "pin ⇒ 整组停回原树位置");
    check(DockManager::instance().expandedPanelId().empty(), "pin 后浮层收起");

    // ── B：两组藏两边，验证按下优先级 ──
    Docking::applyLayout(fourPanelLayout());
    Docking::autoHide("output", DockEdge::Left);
    Docking::autoHide("solution", DockEdge::Right);
    const double rightLen = stripLen("tab_solution");
    // 两条边带各占 28 ⇒ 树 = (28,0,744,600)；右带 = (772,0,28,600)，其 tab 从
    // 带顶往下偏移 len+2 ⇒ 矩形 (772,2,28,len)。
    const double rightCx = kWinW - kStripT / 2.0;
    const double rightCy = 2.0 + rightLen / 2.0;
    rig.h.hover(stripCx, stripCy);
    check(DockManager::instance().expandedPanelId() == "output", "左边条悬停展开 output 组");
    // 浮层开着时点另一条边：只收起浮层，这次按下被吃掉（不展开 solution）。
    rig.h.down(rightCx, rightCy);
    rig.h.up(rightCx, rightCy);
    check(DockManager::instance().expandedPanelId().empty(), "浮层外按下 ⇒ 收起且被吞掉");
    rig.h.down(rightCx, rightCy);
    rig.h.up(rightCx, rightCy);
    check(DockManager::instance().expandedPanelId() == "solution",
          "再点同一条 ⇒ 展开该组（证明上一次没展开）");

    // 浮层内切换标签只改面板本地缓存：隐藏组 {output, err} 有两个 tab。
    rig.h.hover(stripCx, stripCy);  // 换回两标签的那组（solution 组只有一个）
    check(DockManager::instance().expandedPanelId() == "output", "悬停左边条切回 output 组");
    const double errTabX = treeX + kTabInnerPad +
                           std::max(tabW("tab_output"), kTabRowMinW) + kTabGap +
                           std::max(tabW("tab_err"), kTabRowMinW) / 2.0;
    const double expTabCy = kHdrH / 2.0;  // 浮层头部 = rect 顶边起、30 高
    check(countText(rig.h.root, "err") == 0, "切换前浮层里没有 err 的内容");
    rig.h.down(errTabX, expTabCy);
    rig.h.up(errTabX, expTabCy);
    check(countText(rig.h.root, "err") >= 1, "浮层内点第二个标签 ⇒ 内容切到 err");
    check(DockManager::instance().hiddenPanels().at("output").groupIds.size() == 2,
          "浮层内切换不动隐藏组数据");

    Docking::restoreFromEdge("output");
    Docking::restoreFromEdge("solution");
    unregisterPanels(owners);
    DockManager::instance().reset();
}

void testFloatWindow() {
    std::printf("--- 手势：浮动窗标题、把手、按钮与标签 ---\n");
    DockManager::instance().reset();
    auto owners = registerPanels({"editor", "output", "err"});
    Rig rig;
    Docking::applyLayout(threePanelLayout());
    Docking::floatPanel("output");

    const auto& floats = DockManager::instance().floatingPanels();
    check(floats.size() == 1 && floats.count("output") == 1, "整组浮出，键 = 组首 id");
    check(floats.at("output").ids.size() == 2, "组员一起进浮动组");
    check(!DockManager::instance().isDocked("output"), "浮动后不在树里");

    const double tw0 = std::max(tabW("tab_output"), kTabRowMinW);
    const double tw1 = std::max(tabW("tab_err"), kTabRowMinW);
    const double tab0x = kFloatX + kTabInnerPad;
    const double tab1Cx = tab0x + tw0 + kTabGap + tw1 / 2.0;
    const double tabCy = kFloatY + kHdrH * 1.5;  // 标签行 = bodyTop 起、30 高
    const double dockCx = kFloatX + kFloatW - 4.0 - kBtnW * 1.5;
    const double closeCx = kFloatX + kFloatW - 4.0 - kBtnW / 2.0;
    const double btnCy = kFloatY + 4.0 + (kHdrH - 8.0) / 2.0;
    const double gripCx = kFloatX + kFloatW - 8.0;
    const double gripCy = kFloatY + kFloatH - 8.0;

    // 标题栏拖拽 = 「按下时的 offset + 位移」，抬手后保持。
    rig.h.down(kFloatX + 150.0, kFloatY + 15.0);
    rig.h.drag(kFloatX + 200.0, kFloatY + 65.0);
    check(near(floats.at("output").offsetX, 150.0) && near(floats.at("output").offsetY, 150.0),
          "拖标题 ⇒ 位置 +50/+50");
    rig.h.up(kFloatX + 200.0, kFloatY + 65.0);
    check(near(floats.at("output").offsetX, 150.0), "抬手不改位置");
    Docking::updateFloatingPosition("output", kFloatX, kFloatY);
    rig.h.rebuildAndLayout();

    // 右下把手拖拽 = 相对按下时宽高的位移；updateFloatingSize 夹住 200×150 下限。
    rig.h.down(gripCx, gripCy);
    rig.h.drag(gripCx + 50.0, gripCy + 50.0);
    check(near(floats.at("output").width, 450.0) && near(floats.at("output").height, 350.0),
          "拖把手 ⇒ 尺寸 +50/+50");
    rig.h.drag(gripCx - 300.0, gripCy - 300.0);
    check(near(floats.at("output").width, 200.0) && near(floats.at("output").height, 150.0),
          "缩小越界 ⇒ 夹到 200×150 下限");
    rig.h.up(gripCx - 300.0, gripCy - 300.0);
    Docking::updateFloatingSize("output", kFloatW, kFloatH);
    rig.h.rebuildAndLayout();

    // 浮动窗标签行：按下不切换，释放才切。
    rig.h.down(tab1Cx, tabCy);
    check(floats.at("output").activeId == "output", "按下浮动标签不切换");
    rig.h.up(tab1Cx, tabCy);
    check(floats.at("output").activeId == "err", "释放 ⇒ 切换浮动窗活动标签");

    // dock 按钮 = 归树（键 = 组键）。
    rig.h.down(dockCx, btnCy);
    rig.h.up(dockCx, btnCy);
    check(DockManager::instance().floatingPanels().empty(), "dock 按钮 ⇒ 浮动组归树");
    check(DockManager::instance().isDocked("output") && DockManager::instance().isDocked("err"),
          "归树后两个组员都在树里");

    // close 按钮：面板从树/浮动表里彻底消失（activeId 是 close 的目标 id）。
    Docking::floatPanel("output");
    rig.h.down(closeCx, btnCy);
    rig.h.up(closeCx, btnCy);
    check(!DockManager::instance().isFloating("output"), "close 按钮 ⇒ 关掉浮动窗");
    check(!DockManager::instance().isDocked("output"), "close 后不在树里");

    unregisterPanels(owners);
    DockManager::instance().reset();
}

}  // namespace

int main() {
    std::printf("=== test_dock ===\n");
    testLayoutTree();
    testFloating();
    testAutoHide();
    testMaximize();
    testSerialization();
    testListeners();
    testPanelSubscription();
    testReattachIsNotDuplicated();
    testSplitterGestures();
    testTabClickGestures();
    testTabDragGestures();
    testStripAndFlyout();
    testFloatWindow();
    std::printf("\n=== %s: %d failure(s) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
