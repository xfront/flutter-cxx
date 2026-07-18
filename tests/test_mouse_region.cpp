/// @file test_mouse_region.cpp
/// @brief MouseRegion / RenderMouseRegion 单元测试。对齐上游
///        rendering/proxy_box.dart 的 `RenderMouseRegion` +
///        widgets/basic.dart 的 `MouseRegion` + rendering/mouse_tracker.dart
///        的 enter/exit 差分算法（`_handleDeviceUpdateMouseEvents`）。
///
/// 覆盖：
///   1. 一次事件一个节点：进入区域时每个变化的节点各收一条 Enter，
///      不沿链重复（本仓修复前的「向上冒泡」写法会重复派发）。
///   2. 顺序：Enter 走 hit-test 逆序（最浅→最深），Exit 走 hit-test 序
///      （最深→最浅）——与上游一致。
///   3. 区域内移动不重复 Enter/Exit（集合差分）。
///   4. 拖拽期间静默：按下缓存的命中链不做差分；onHover 只回应无键悬停。
///      按下会清空 hover 链（防悬空指针），故抬手后首次悬停会对仍覆盖的
///      节点**重发** Enter —— 本仓与上游的已知偏差。
///   5. 链上非 annotation 节点（Listener）同样收到合成的 Enter/Exit：本仓的
///      差分派发给命中链全体，Tooltip/InkWell 的悬停高亮依赖这一点。
///   6. Enter/Exit 不参与冒泡，stopPropagation 拦不住链上的其它节点。
///   7. opaque=true 挡住身后兄弟；opaque=false 双方都命中。
///   8. 事件坐标：position 已按节点本地变换，globalPosition 保持全局。
///   9. 无子节点的区域按上游 computeSizeForNoChild 撑满约束（不塌成 0×0）。
///  10. 命中链缓存的裸指针在节点销毁时被摘除（回归：Exit 差分打在已释放
///      内存上，段错误落在 `Offset::operator-`）。真正的牙齿在 ASan 树。

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/framework/listener.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"

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

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

struct Log {
    std::vector<std::string> entries;
    std::vector<Offset> positions;
    std::vector<Offset> globalPositions;

    void clear() {
        entries.clear();
        positions.clear();
        globalPositions.clear();
    }
};

/// 记下 Enter/Exit/Hover 的名字与事件坐标。
void note(Log& log, const std::string& tag, const PointerEvent& e) {
    log.entries.push_back(tag);
    log.positions.push_back(e.position);
    log.globalPositions.push_back(e.globalPosition);
}

SharedPtr<Widget> region(Log& log, const std::string& name,
                               SharedPtr<Widget> child = nullptr, bool opaque = true) {
    return cui::make_shared<MouseRegion>(
        std::move(child),
        [&log, name](const PointerEvent& e) { note(log, "enter:" + name, e); },
        [&log, name](const PointerEvent& e) { note(log, "exit:" + name, e); },
        [&log, name](const PointerEvent& e) { note(log, "hover:" + name, e); },
        opaque);
}

/// 一块可被命中的固定尺寸盒子（SizedBox + DecoratedBox）。
SharedPtr<Widget> box(double w, double h, SharedPtr<Widget> child = nullptr) {
    if (!child)
        child = cui::make_shared<DecoratedBox>();
    return cui::make_shared<SizedBox>(w, h, std::move(child));
}

/// 钉到窗口左上角并给出**松约束**：RenderView 的紧约束会把 SizedBox 的指定
/// 尺寸顶掉（Flutter 真实行为），外包 Align(-1,-1) 才能拿到想要的矩形。
SharedPtr<Widget> pinned(SharedPtr<Widget> w) {
    return cui::make_shared<Align>(-1.0, -1.0, std::move(w));
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) {
        if (!s.empty())
            s += ", ";
        s += x;
    }
    return s;
}

void expectSeq(Log& log, const std::vector<std::string>& want, const std::string& what) {
    bool ok = log.entries.size() == want.size();
    for (size_t i = 0; ok && i < want.size(); ++i)
        ok = log.entries[i] == want[i];
    if (!ok)
        std::printf("      实际: [%s]\n      期望: [%s]\n", joined(log.entries).c_str(),
                    joined(want).c_str());
    check(ok, what);
    log.clear();
}

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        pump();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    void hover(double x, double y) {
        PointerHoverEvent e;
        e.position = {x, y};
        po.dispatchPointerEvent(e);
        pump();
    }
    void down(double x, double y) {
        PointerDownEvent e;
        e.position = {x, y};
        e.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(e);
        pump();
    }
    void drag(double x, double y) {
        PointerMoveEvent e;
        e.position = {x, y};
        e.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(e);
        pump();
    }
    void up(double x, double y) {
        PointerUpEvent e;
        e.position = {x, y};
        po.dispatchPointerEvent(e);
        pump();
    }
};

/// 三层同心矩形：o=(0,0)-(300,300) ⊃ m=(50,50)-(250,250) ⊃ i=(100,100)-(200,200)。
///
/// 用 Align 而不是 Padding 定位：Padding 在紧约束下 `deflate` 出来的仍是紧约束，
/// 会把内层 SizedBox 的指定尺寸一路顶大；Align 的 `loosen()` 才让内层拿到松约束。
SharedPtr<Widget> nestedRegions(Log& log) {
    auto inner = region(log, "i", box(100, 100));
    auto middle = region(log, "m", box(200, 200, cui::make_shared<Align>(0.0, 0.0, inner)));
    return region(log, "o", box(300, 300, cui::make_shared<Align>(0.0, 0.0, middle)));
}

// ── 1/2/3：进入/离开差分的次数与顺序 ───────────────────────────────────────

void testEnterExitSequence() {
    std::printf("--- 进入/离开序列 ---\n");
    Log log;
    Harness h;
    h.attach(pinned(nestedRegions(log)), 400, 400);

    // 三个区域此前都未悬停 ⇒ Enter 走 hit-test 逆序（o → m → i），
    // 随后的 Hover 按冒泡走（i → m → o）。
    h.hover(150, 150);
    expectSeq(log, {"enter:o", "enter:m", "enter:i", "hover:i", "hover:m", "hover:o"},
              "进入三层嵌套：Enter 由外向内，每个节点恰好一次");

    // 仍在 o/m 内，只离开 i ⇒ 只有 i 收到 Exit。
    h.hover(75, 220);
    expectSeq(log, {"exit:i", "hover:m", "hover:o"}, "只离开最内层：Exit 仅发给 i");

    // 再离开 m，只剩 o；Exit 走 hit-test 序（最深 → 最浅）。
    h.hover(25, 25);
    expectSeq(log, {"exit:m", "hover:o"}, "再离开 m：Exit 发给 m，o 仍悬停");

    // 完全离开 ⇒ 空链，最后一个 Exit 是 o，且没有 hover（无节点可冒泡）。
    h.hover(350, 350);
    expectSeq(log, {"exit:o"}, "移出全部区域：只剩 exit:o");

    // 重新进入：差分是集合语义，顺序依旧外→内。
    h.hover(150, 150);
    expectSeq(log, {"enter:o", "enter:m", "enter:i", "hover:i", "hover:m", "hover:o"},
              "重新进入：Enter 再次 o → m → i");

    // 区域内小幅移动：链未变 ⇒ 没有任何 enter/exit。
    h.hover(160, 160);
    expectSeq(log, {"hover:i", "hover:m", "hover:o"}, "区域内移动不重复 Enter/Exit");
}

// ── 4：拖拽期间的静默 ──────────────────────────────────────────────────────

void testDragIsQuiet() {
    std::printf("--- 拖拽期间静默 ---\n");
    Log log;
    Harness h;
    h.attach(pinned(nestedRegions(log)), 400, 400);

    // 按下：命中链缓存，差分不跑；本仓按下会清空 hover 链。
    h.down(150, 150);
    check(log.entries.empty(), "Down 不发 Enter/Exit/Hover");

    // 有键拖动走缓存链：既不重算悬停，也不算 hover（上游同义）。
    h.drag(180, 180);
    h.drag(290, 60);
    check(log.entries.empty(), "拖拽中的 Move 不触发 onHover，也不发 Enter/Exit");

    h.up(290, 60);
    check(log.entries.empty(), "Up 不发 Enter/Exit/Hover");

    // 已知偏差：按下清空了 hover 链 ⇒ 抬手后首次悬停会重发 Enter。
    // (290,60) 只被 o 覆盖。
    h.hover(290, 60);
    expectSeq(log, {"enter:o", "hover:o"}, "抬手后首次悬停：对仍覆盖的 o 重发 Enter");
}

// ── 5：链上非 annotation 节点同样收到 Enter/Exit ───────────────────────────

void testListenerInChain() {
    std::printf("--- 命中链全体派发 ---\n");
    Log log;
    auto listener = cui::make_shared<Listener>(
        Listener::PointerCallback{}, Listener::PointerCallback{}, Listener::PointerCallback{},
        [&log](const PointerEvent& e) { note(log, "hover:L", e); }, Listener::PointerCallback{},
        box(100, 100), [&log](const PointerEvent& e) { note(log, "enter:L", e); },
        [&log](const PointerEvent& e) { note(log, "exit:L", e); });
    Harness h;
    h.attach(pinned(region(log, "o", box(100, 100, listener))), 200, 200);

    h.hover(50, 50);
    expectSeq(log, {"enter:o", "enter:L", "hover:L", "hover:o"},
              "区域下的 Listener 子节点也收到合成的 Enter/Exit（Tooltip/InkWell 依赖）");

    h.hover(150, 150);
    expectSeq(log, {"exit:L", "exit:o"}, "离开：Exit 先给最深的 L，再给 o");
}

// ── 6：Enter/Exit 不冒泡，stopPropagation 拦不住其它节点 ───────────────────

void testNoBubblingForEnterExit() {
    std::printf("--- Enter/Exit 不参与冒泡 ---\n");
    Log log;
    auto stopper = cui::make_shared<Listener>(
        Listener::PointerCallback{}, Listener::PointerCallback{}, Listener::PointerCallback{},
        Listener::PointerCallback{}, Listener::PointerCallback{}, box(100, 100),
        [](const PointerEvent& e) { e.stopPropagation(); },
        [&log](const PointerEvent& e) {
            e.stopPropagation();
            note(log, "exit:L", e);
        });
    Harness h;
    h.attach(pinned(region(log, "o", box(100, 100, stopper))), 200, 200);

    h.hover(50, 50);
    log.clear();  // 进入时 o 先于 L 拿到 Enter；此处只验离开方向。

    h.hover(150, 150);
    expectSeq(log, {"exit:L", "exit:o"}, "子节点 stopPropagation 不能阻止父区域收到 Exit");
}

// ── 7：opaque 遮挡同层兄弟 ─────────────────────────────────────────────────

void testOpaqueSiblings() {
    std::printf("--- opaque 遮挡 ---\n");
    for (const bool opaque : {true, false}) {
        Log log;
        auto back = region(log, "back", box(100, 100));
        auto front = region(log, "front", box(100, 100), opaque);
        Harness h;
        h.attach(pinned(cui::make_shared<Stack>(
                     std::vector<SharedPtr<Widget>>{back, front})),
             200,
                 200);

        h.hover(50, 50);
        if (opaque) {
            expectSeq(log, {"enter:front", "hover:front"},
                      "opaque=true：身后的兄弟区域收不到任何事件");
        } else {
            expectSeq(log, {"enter:front", "enter:back", "hover:back", "hover:front"},
                      "opaque=false：前后两个区域都命中");
        }
    }
}

// ── 8：本地坐标变换 ────────────────────────────────────────────────────────

void testLocalPositionTransform() {
    std::printf("--- 坐标变换 ---\n");
    Log log;
    Harness h;
    h.attach(pinned(cui::make_shared<Padding>(EdgeInsets::only(20, 30),
                                  region(log, "r", box(100, 100)))),
             200, 200);

    h.hover(60, 80);
    check(joined(log.entries) == "enter:r, hover:r", "Padding 内的区域正常命中");
    // 区域原点 = (20, 30) ⇒ 全局 (60, 80) → 本地 (40, 50)。
    check(log.positions.size() == 2 && near(log.positions[0].dx, 40.0) &&
              near(log.positions[0].dy, 50.0),
          "Enter 事件的 position 是区域本地坐标");
    check(log.globalPositions.size() == 2 && near(log.globalPositions[0].dx, 60.0) &&
              near(log.globalPositions[0].dy, 80.0),
          "Enter 事件的 globalPosition 保持全局坐标");
    log.clear();
}

// ── 9：无子节点区域撑满约束 ────────────────────────────────────────────────

void testChildlessRegionFills() {
    std::printf("--- 无子节点的区域 ---\n");
    Log log;
    {
        Harness h;
        h.attach(region(log, "r"), 200, 200);
        h.hover(190, 190);
        expectSeq(log, {"enter:r", "hover:r"}, "根下无子区域拿到窗口尺寸（紧约束）");

        h.hover(100, 100);
        expectSeq(log, {"hover:r"}, "区域内移动不重复 Enter");
    }
    {
        Log log2;
        Harness h;
        h.attach(pinned(box(80, 60, region(log2, "r2"))), 200, 200);
        h.hover(70, 50);
        expectSeq(log2, {"enter:r2", "hover:r2"}, "SizedBox 限定的无子区域按约束撑满");
        h.hover(150, 150);
        expectSeq(log2, {"exit:r2"}, "无子区域的边界 = 约束尺寸");
    }
}

// ── 10：命中链里的节点被销毁（回归：heap-use-after-free） ───────────────────

/// 三态探针：0 = MouseRegion("a")，1 = 光板 SizedBox（**控件类型不同** ⇒ a 的
/// Element 不能复用、被卸载，其 RenderObject 当场销毁），2 = MouseRegion("b")。
class SwapState;

class Swap : public StatefulWidget {
public:
    explicit Swap(Log& log) : log_(log) {}
    std::unique_ptr<State> createState() override;

private:
    friend class SwapState;
    Log& log_;
};

class SwapState : public State {
public:
    explicit SwapState(Log& log) : log_(log) {}
    SharedPtr<Widget> build(BuildContext&) override {
        if (mode_ == 1)
            return pinned(box(100, 100));
        return pinned(region(log_, mode_ == 2 ? "b" : "a", box(100, 100)));
    }
    void setMode(int m) {
        if (mode_ == m)
            return;
        mode_ = m;
        setState();
    }

private:
    Log& log_;
    int mode_ = 0;
};

inline std::unique_ptr<State> Swap::createState() {
    return std::make_unique<SwapState>(log_);
}

/// 悬停/按下命中链缓存的是裸 RenderObject*，而事件在两帧之间到达：目标子树
/// 一旦被重建销毁，下一次 Hover 的 Exit 差分就会对着已释放内存调
/// globalToLocal（段错误落在 Offset::operator-）。修法 = 节点 detach 时把自己
/// 从 PipelineOwner 的 hoverPath_ / downPaths_ 里摘掉。
void testDestroyedTargetIsPurged() {
    std::printf("--- 命中链里的节点被销毁 ---\n");
    Log log;
    Harness h;
    h.attach(cui::make_shared<Swap>(log), 200, 200);
    auto* st = dynamic_cast<SwapState*>(static_cast<StatefulElement*>(h.root)->state());
    check(st != nullptr, "拿到 SwapState");

    h.hover(50, 50);
    expectSeq(log, {"enter:a", "hover:a"}, "悬停进入 a");

    // 换成光板：a 的 RenderObject 被销毁，但它还留在 hoverPath_ 里。
    st->setMode(1);
    h.pump();
    h.hover(60, 60);
    check(log.entries.empty(), "已销毁的区域不再被派发 Exit（也没有崩溃）");

    // 摘除只针对死节点：新子树照常收到 Enter。
    st->setMode(2);
    h.pump();
    h.hover(50, 50);
    expectSeq(log, {"enter:b", "hover:b"}, "换回区域后差分继续工作");

    // down 链同样要摘：按下 b → 销毁 b → 拖拽/抬手不得回调已释放的对象。
    h.down(50, 50);
    st->setMode(1);
    h.pump();
    h.drag(60, 60);
    h.up(60, 60);
    check(log.entries.empty(), "拖拽途中目标被销毁：down 链已摘除，无回调、无崩溃");
}

}  // namespace

int main() {
    std::printf("=== mouse_region tests ===\n");
    testEnterExitSequence();
    testDragIsQuiet();
    testListenerInChain();
    testNoBubblingForEnterExit();
    testOpaqueSiblings();
    testLocalPositionTransform();
    testChildlessRegionFills();
    testDestroyedTargetIsPurged();
    std::printf(g_failures ? "FAILED %d\n" : "ALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
