/// @file test_repaint_damage.cpp
/// @brief 「子树消失 → 脏区 → 重绘」链路回归。
///
/// 钉住本仓曾经的一个整帧级缺陷：`Element::unmount` 无条件罩住拆除模式，
/// 于是 setState 让子树消失时 `RenderObject::dropChild` 被抑制 —— 父容器既不
/// 标脏也不入脏链，`flushPaint` 收到空脏链，帧驱动按「no damage」跳过整帧，
/// surface 上的旧像素（Tooltip 气泡、菜单项）永远不消失。
///
/// 覆盖：
///   1. 移除子树的那一帧必须有脏区，且旧像素被背景顶掉。
///   2. 无变化的后续帧仍然跳过（修复不能退化成「每帧全量重录」）。
///   3. 新增子树同样有脏区（adoptChild 那条路一直是对的，钉住对称性）。
///   4. 根控件更换渲染类型：旧根卸载不能把悬空指针留在宿主上，新根要自动接回
///      `RenderView`（BuildOwner 的 root host），且能正常布局/绘制。
///   5. finalizeTree / BuildOwner 析构（真拆除模式）不崩。
///   6. setter 契约：值变了必须标脏（修复前 RenderFlex / 进度条是静默赋值），
///      值没变不得标脏（上游的 `if (x == _x) return;`）。
///   7. 参数更新走完整链路：setState 换 Flex 方向必须真的重排；等参数重建
///      不得产生脏区。

#include <functional>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/layer.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_material.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/rendering/repaint_boundary.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/material.h"

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

constexpr SkColor kRed = 0xFFFF0000;
constexpr SkColor kBlue = 0xFF0000FF;

SharedPtr<Widget> colorBox(double w, double h, const Color& color) {
    BoxDecoration d;
    d.color = color;
    return cui::make_shared<SizedBox>(w, h, cui::make_shared<DecoratedBox>(d));
}

/// 钉到左上角并给出松约束：RenderView 的紧约束会把 SizedBox 的指定尺寸顶掉。
SharedPtr<Widget> pinned(SharedPtr<Widget> w) {
    return cui::make_shared<Align>(-1.0, -1.0, std::move(w));
}

// ── 被测控件 ─────────────────────────────────────────────────────────────────

/// 300×300 红底 + 可选的 50×50 蓝块（叠在左上角，即「气泡」）。
class OverlayState;

class OverlayBox : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;

private:
    friend class OverlayState;
};

class OverlayState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(colorBox(300, 300, {1, 0, 0, 1}));
        if (show_)
            kids.push_back(colorBox(50, 50, {0, 0, 1, 1}));
        return pinned(cui::make_shared<Stack>(std::move(kids)));
    }
    void setShow(bool v) {
        if (show_ == v)
            return;
        show_ = v;
        setState();
    }
    bool showing() const { return show_; }

private:
    bool show_ = true;
};

inline std::unique_ptr<State> OverlayBox::createState() {
    return std::make_unique<OverlayState>();
}

/// 根渲染对象会随开关换类型：SizedBox(RenderConstrainedBox) ↔ Stack(RenderStack)。
class RootSwapState;

class RootSwap : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;

private:
    friend class RootSwapState;
};

class RootSwapState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        if (wide_)
            return pinned(colorBox(200, 200, {0, 1, 0, 1}));  // RenderAlign>RenderConstrainedBox
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(colorBox(200, 200, {0, 1, 0, 1}));
        return pinned(cui::make_shared<Stack>(std::move(kids)));  // RenderAlign>RenderStack
    }
    void swapRoot() {
        wide_ = !wide_;
        setState();
    }

private:
    bool wide_ = true;
};

inline std::unique_ptr<State> RootSwap::createState() {
    return std::make_unique<RootSwapState>();
}

/// 可调参数的树：根 `Column`（RenderFlex）里装 [内层 Flex(两个色块), 线性进度条]。
/// 用来验「控件参数变了 → updateRenderObject → setter → 必脏」这条链。
class TunerState;

class Tuner : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;

private:
    friend class TunerState;
};

class TunerState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> pair;
        pair.push_back(colorBox(100, 50, {1, 0, 0, 1}));
        pair.push_back(colorBox(60, 30, {0, 0, 1, 1}));
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(cui::make_shared<Flex>(vertical_ ? Axis::Vertical : Axis::Horizontal,
                                              std::move(pair)));
        kids.push_back(cui::make_shared<LinearProgressIndicator>(progress_));
        return cui::make_shared<Column>(std::move(kids));
    }
    void setVertical(bool v) {
        if (vertical_ == v)
            return;
        vertical_ = v;
        setState();
    }
    void setProgress(std::optional<double> v) {
        if (progress_ == v)
            return;
        progress_ = v;
        setState();
    }
    /// 参数一个都不改，只是重跑一遍 build + updateRenderObject。
    void rebuildSame() { setState(); }

private:
    bool vertical_ = false;
    std::optional<double> progress_ = 0.5;
};

inline std::unique_ptr<State> Tuner::createState() {
    return std::make_unique<TunerState>();
}

// ── 宿主：RenderView + PipelineOwner + BuildOwner + SchedulerBinding + raster surface ──
struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;  // 晚于 rv/po 声明：析构时元素树先散
    SchedulerBinding sb;
    Element* root = nullptr;
    sk_sp<SkSurface> surface;

    void attach(SharedPtr<Widget> w, double ww = 400, double hh = 400) {
        rv.setWindowSize({ww, hh});
        owner.setRootRenderObjectHost(&rv);
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        po.setRootNode(&rv);
        sb.setBuildOwner(&owner);
        sb.setPipelineOwner(&po);
        sb.setRenderView(&rv);
        surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul((int)ww, (int)hh));
        surface->getCanvas()->clear(SK_ColorWHITE);
        frame();  // 首帧
    }

    /// 一帧：跑管线拿脏区，再把 LayerTree 全量合成到 surface（脏区裁剪不是本
    /// 测试要钉的东西，「有没有脏区」才是）。
    SchedulerBinding::FrameDamage frame() {
        SchedulerBinding::FrameDamage d = sb.runPipelineAndComputeDamage();
        sb.compositeTo(surface->getCanvas());
        return d;
    }

    SkColor at(int x, int y) const {
        uint32_t px = 0;
        surface->readPixels(SkImageInfo::MakeN32Premul(1, 1), &px, 4, x, y);
        return px;
    }
};

// ── 用例 ─────────────────────────────────────────────────────────────────────

void testRemovalProducesDamage() {
    std::printf("--- 子树消失必须产生脏区 ---\n");
    Harness h;
    auto box = cui::make_shared<OverlayBox>();
    h.attach(box);

    check(h.at(10, 10) == kBlue, "首帧：蓝块（气泡）已绘制");
    check(h.at(200, 200) == kRed, "首帧：红底已绘制");

    auto* st = dynamic_cast<OverlayState*>(
        static_cast<StatefulElement*>(h.root)->state());
    check(st != nullptr, "拿到 OverlayState");
    if (!st)
        return;

    st->setShow(false);
    SchedulerBinding::FrameDamage d = h.frame();
    check(d.any(), "移除子树的那一帧有脏区（旧缺陷：no damage ⇒ 整帧跳过）");
    check(h.at(10, 10) == kRed, "旧像素被重绘掉：蓝块位置现在是红底");
    check(h.at(200, 200) == kRed, "未变区域仍正确");

    SchedulerBinding::FrameDamage idle = h.frame();
    check(!idle.any(), "无变化的后续帧仍然无脏区（没退化成每帧全量）");

    st->setShow(true);
    SchedulerBinding::FrameDamage d2 = h.frame();
    check(d2.any(), "重新新增子树同样有脏区");
    check(h.at(10, 10) == kBlue, "蓝块回来了");
}

/// 往下遍历渲染树找那个 RenderStack（pinned 的根链是 Align → Stack）。
RenderObject* findStack(RenderObject* ro) {
    if (!ro)
        return nullptr;
    if (dynamic_cast<RenderStack*>(ro))
        return ro;
    RenderObject* found = nullptr;
    ro->visitChildren([&](AbstractNode* c) {
        if (!found)
            found = findStack(static_cast<RenderObject*>(c));
    });
    return found;
}

/// 元素树里找「渲染对象恰好是 ro」的那个 RenderObjectElement。
Element* findRenderObjectElement(Element* e, RenderObject* ro) {
    if (!e || !ro)
        return nullptr;
    if (dynamic_cast<RenderObjectElement*>(e) && e->findRenderObject() == ro)
        return e;
    Element* found = nullptr;
    e->visitChildren([&](Element* c) {
        if (!found)
            found = findRenderObjectElement(c, ro);
    });
    return found;
}

/// 本用例是「钉死修复」的那个：绕开一切重建路径（setState 路径上控件的
/// setter 也会顺带标脏，会把缺陷掩盖掉），直接走 Element::unmount —— 这正是
/// `disposeChild` 在 updateChildren 里做的事。修复前 `Element::unmount` 罩着
/// 拆除模式，`RenderObject::dropChild` 的标脏被抑制，父链与宿主都不脏。
void testUnmountMarksHostContainer() {
    std::printf("--- 卸载子节点必须标脏父容器与根边界 ---\n");
    Harness h;
    h.attach(cui::make_shared<OverlayBox>());

    check(h.at(10, 10) == kBlue, "首帧：蓝块已绘制");
    check(!h.rv.needsPaint(), "首帧之后根边界是干净的（没有常驻脏位）");

    auto* stack = dynamic_cast<RenderBoxContainer*>(findStack(h.rv.child()));
    check(stack != nullptr, "根链上找到 RenderStack");
    check(stack && stack->children().size() == 2, "Stack 有两个子（红底 + 蓝块）");
    if (!stack || stack->children().size() != 2)
        return;
    RenderObject* blue = stack->children()[1];
    Element* blueEl = findRenderObjectElement(h.root, blue);
    check(blueEl != nullptr, "找到蓝块对应的 RenderObjectElement");
    if (!blueEl)
        return;

    blueEl->unmount();  // = disposeChild 的那一步
    check(stack->needsPaint(), "卸载后父 RenderStack needsPaint（修复前为 false）");
    check(h.rv.needsPaint(), "卸载后根边界进了脏链（修复前为 false）");

    SchedulerBinding::FrameDamage d = h.frame();
    check(d.any(), "卸载后那一帧有脏区（修复前 no damage ⇒ 整帧跳过）");
    check(h.at(10, 10) == kRed, "重绘后旧像素消失：蓝块位置现在是红底");
}

void testRootRenderObjectSwap() {
    std::printf("--- 根控件换渲染类型 ---\n");
    Harness h;
    auto swap = cui::make_shared<RootSwap>();
    h.attach(swap);

    RenderObject* initial = h.rv.child();
    check(initial == static_cast<RenderObject*>(h.root->findRenderObject()),
          "首帧：宿主的孩子就是根渲染对象");
    check(h.at(10, 10) != SK_ColorWHITE, "首帧：内容已绘制");

    auto* st = dynamic_cast<RootSwapState*>(static_cast<StatefulElement*>(h.root)->state());
    check(st != nullptr, "拿到 RootSwapState");
    if (!st)
        return;

    st->swapRoot();  // RenderConstrainedBox → RenderStack
    SchedulerBinding::FrameDamage d = h.frame();
    check(h.rv.child() != nullptr, "旧根卸载后宿主不留悬空指针（有当前根）");
    check(static_cast<RenderObject*>(h.rv.child()) ==
              static_cast<RenderObject*>(h.root->findRenderObject()),
          "新根自动接回宿主");
    check(d.any(), "换根帧有脏区");
    check(h.at(10, 10) != SK_ColorWHITE, "换根后仍绘制出内容");

    st->swapRoot();
    h.frame();
    check(static_cast<RenderObject*>(h.rv.child()) ==
              static_cast<RenderObject*>(h.root->findRenderObject()),
          "换回原类型同样接回");
}

void testTeardownStillSafe() {
    std::printf("--- 拆除路径不崩 ---\n");
    {
        Harness h;
        auto box = cui::make_shared<OverlayBox>();
        h.attach(box);
        auto* st = dynamic_cast<OverlayState*>(static_cast<StatefulElement*>(h.root)->state());
        if (st)
            st->setShow(false);
        h.owner.flushBuildScope();
        h.owner.finalizeTree();  // 帧末卸载 inactive 元素
        h.frame();
        check(true, "finalizeTree + 后续帧未崩溃");
    }  // ~BuildOwner 在 ~RenderView 之前：整树拆除
    check(true, "BuildOwner 析构卸载整树未崩溃");
}

/// 直接打在渲染对象 setter 上的契约测试（上游 的 setter 形状：
/// `if (x == _x) return; _x = x; markNeeds…()`）。
/// 修复前两处都错：RenderFlex 四个参数 setter 是**静默赋值**，
/// RenderLinearProgressIndicator::setValue 同样静默 ⇒ 改值既不脏也不重绘。
void testSetterMarksDirty() {
    std::printf("--- setter 契约：变更必脏、等值不脏 ---\n");
    Harness h;
    h.attach(cui::make_shared<Tuner>());

    auto* outer = dynamic_cast<RenderFlex*>(h.rv.child());
    check(outer != nullptr, "根是 Column 的 RenderFlex");
    if (!outer)
        return;
    auto* outerC = static_cast<RenderBoxContainer*>(outer);
    check(outerC->children().size() == 2, "外层 Column 有两个子（内层 Flex + 进度条）");
    if (outerC->children().size() != 2)
        return;
    auto* inner = dynamic_cast<RenderFlex*>(outerC->children()[0]);
    auto* prog = dynamic_cast<RenderLinearProgressIndicator*>(outerC->children()[1]);
    check(inner != nullptr && prog != nullptr, "拿到内层 RenderFlex 与 RenderLinearProgressIndicator");
    if (!inner || !prog)
        return;

    check(!inner->needsLayout() && !prog->needsPaint(), "布局/绘制完成后自身是干净的");

    // 等值：不得把节点弄脏（上游的 early return）
    inner->setDirection(Axis::Horizontal);
    prog->setValue(prog->value());
    check(!inner->needsLayout(), "等值 setDirection 不标脏");
    check(!prog->needsPaint(), "等值 setValue 不标脏");

    // 变更：必须标脏，而且要沿父链冒到根边界（否则帧驱动按 no-damage 跳帧）
    inner->setDirection(Axis::Vertical);
    check(inner->needsLayout(), "改 setDirection 必须标脏布局");
    check(h.rv.needsLayout(), "布局脏要冒到根边界");
    h.frame();
    check(!prog->needsPaint(), "重绘完成后进度条是干净的");
    prog->setValue(0.9);
    check(prog->needsPaint(), "改 setValue 必须标脏绘制（修复前静默赋值）");
    check(h.rv.needsPaint(), "绘制脏要冒到根边界");
    SchedulerBinding::FrameDamage d = h.frame();
    check(d.any(), "改值那一帧有脏区");
}

/// 端到端：setState 换 Flex 方向必须真的重排；参数没变的 setState 不得产生脏区
/// （这就是 setter 等值早退存在的意义——否则每次重建都全树重排重绘）。
void testParamUpdateRepathsAndIdles() {
    std::printf("--- 参数更新走完整链路 ---\n");
    Harness h;
    auto tuner = cui::make_shared<Tuner>();
    h.attach(tuner);

    auto* st = dynamic_cast<TunerState*>(static_cast<StatefulElement*>(h.root)->state());
    check(st != nullptr, "拿到 TunerState");
    if (!st)
        return;

    auto kidsOfInnerFlex = [&h]() -> std::pair<RenderObject*, RenderObject*> {
        auto* outer = dynamic_cast<RenderFlex*>(h.rv.child());
        if (!outer)
            return {nullptr, nullptr};
        const auto& outerKids = static_cast<RenderBoxContainer*>(outer)->children();
        if (outerKids.empty())
            return {nullptr, nullptr};
        auto* inner = dynamic_cast<RenderFlex*>(outerKids[0]);
        if (!inner)
            return {nullptr, nullptr};
        const auto& innerKids = static_cast<RenderBoxContainer*>(inner)->children();
        if (innerKids.size() < 2)
            return {nullptr, nullptr};
        return {innerKids[0], innerKids[1]};
    };

    auto [a, b] = kidsOfInnerFlex();
    check(a && b, "拿到内层 Flex 的两个色块");
    if (!a || !b)
        return;
    // 初始为横排：第二个孩子的横向起点 = 第一个孩子的宽度。
    double aW = a->paintBounds().width;
    check(b->parentData()->offset.dx == aW, "横排：第二个孩子沿主轴排在第一个之后");

    st->setVertical(true);
    SchedulerBinding::FrameDamage d = h.frame();
    check(d.any(), "换向那一帧有脏区");
    auto [a2, b2] = kidsOfInnerFlex();
    check(a2 == a && b2 == b, "换向后两个色块的渲染对象被复用（没重建子树）");
    check(b2->parentData()->offset.dy == a2->paintBounds().height,
          "换向生效：纵排时主轴变成纵轴");

    // 参数一个都没改的 setState：不得留下任何脏区。
    st->rebuildSame();
    SchedulerBinding::FrameDamage idle = h.frame();
    check(!idle.any(), "等参数重建不产生脏区（setter 等值早退）");

    st->setProgress(0.2);
    check(h.frame().any(), "改进度值那一帧有脏区");

    st->setVertical(false);
    auto [a3, b3] = kidsOfInnerFlex();
    h.frame();
    check(b3->parentData()->offset.dx == a3->paintBounds().width,
          "换回横排：布局跟着参数回来");
}

// ── 8. 直接绘制必须按绘制顺序切成多段，与推入的图层交错 ────────────────────
// 上游 PaintingContext 每推入一个图层（clip / transform / opacity / 重绘边界）
// 之前都会 `stopRecordingIfNeeded()` 把已录内容结算成 PictureLayer 追加进容器，
// 新层排在它之后，随后再开一段。少了这一步，整棵树的直接绘制会挤进**一个**排在
// 所有推入图层之前的 picture ⇒ 后画的直接内容被先画的图层盖住。
//
// 本仓的可见症状：Overlay/Navigator 的浮层是 RenderTheater 的**后一个**子节点，
// 而页面内容里有 AnimatedSize / Opacity / RepaintBoundary 这类推图层的控件 ⇒
// 弹出菜单、ComboBox 选项卡被页面内容压在下面（z 序整体颠倒）。
void testDirectPaintSegmentsInterleaveWithLayers() {
    std::printf("--- 直接绘制分段与图层交错 ---\n");

    Harness h;
    // Stack 的两个子：先是独立重绘边界的 300×300 红（推入一个 PictureLayer），
    // 后是直接绘制的 50×50 蓝。蓝绘制在后 ⇒ 必须盖在红之上。
    std::vector<SharedPtr<Widget>> kids;
    kids.push_back(cui::make_shared<RepaintBoundary>(colorBox(300, 300, {1, 0, 0, 1})));
    kids.push_back(colorBox(50, 50, {0, 0, 1, 1}));
    h.attach(pinned(cui::make_shared<Stack>(std::move(kids))));

    check(h.at(10, 10) == kBlue, "后绘制的直接内容压在先推入的图层之上（重叠处）");
    check(h.at(200, 200) == kRed, "先推入的图层仍在场（未重叠处）");

    const auto& rootChildren = h.rv.layerTree().rootLayer()->children();
    auto pictureAt = [&](size_t i) -> PictureLayer* {
        return i < rootChildren.size()
                   ? dynamic_cast<PictureLayer*>(rootChildren[i].get())
                   : nullptr;
    };
    auto opCount = [&](PictureLayer* pl) {
        return pl && pl->picture() ? pl->picture()->approximateOpCount() : 0;
    };
    check(rootChildren.size() >= 3, "根容器至少有「边界层 / 边界 / 尾段」三个子层");
    check(opCount(pictureAt(rootChildren.size() - 1)) > 0,
          "最后一个子层是「换段之后」的直接绘制（含蓝块）");
    // 边界层自己也在尾段**之前**：它若排在最后，蓝块就会被红盖住（旧缺陷的形状）。
    bool boundaryBeforeTail = false;
    for (size_t i = 0; i + 1 < rootChildren.size(); ++i)
        if (opCount(pictureAt(i)) > 0)
            boundaryBeforeTail = true;
    check(boundaryBeforeTail, "推入的图层排在尾段之前（交错成立）");
}

/// 渲染树里找重绘边界。
RenderObject* findRepaintBoundary(RenderObject* ro) {
    if (!ro)
        return nullptr;
    if (dynamic_cast<RenderRepaintBoundary*>(ro))
        return ro;
    RenderObject* found = nullptr;
    ro->visitChildren([&](AbstractNode* c) {
        if (!found)
            found = findRepaintBoundary(static_cast<RenderObject*>(c));
    });
    return found;
}

// ── 9. 变换之上的重绘边界（Tooltip 悬停崩溃 + 位移泄漏回归）──────────────────
// 重绘边界的推入会「结算录制段」：旧画布被换下，外层作用域的 save/restore
// 落在退役画布上。修复前两个症状：① 悬停弹 Tooltip（Transform → CustomPaint
// 边界）直接段错误；② 气泡画回未变换的位置，且 Transform 的位移泄漏到作用域
// 之外 —— 整页后续内容上移 34px。这里用纯盒子树钉住：边界带祖先变换、边界
// 之后的作用域内兄弟仍带变换、作用域之外的兄弟不受影响、增量重录同样正确。
void testTransformAboveRepaintBoundary() {
    std::printf("--- 变换之上的重绘边界 ---\n");

    Harness h;
    std::vector<SharedPtr<Widget>> inner;
    inner.push_back(cui::make_shared<RepaintBoundary>(colorBox(20, 20, {0, 0, 1, 1})));
    inner.push_back(colorBox(20, 20, {0, 1, 0, 1}));  // 蓝（边界）之上的直接绘制
    std::vector<SharedPtr<Widget>> kids;
    kids.push_back(colorBox(300, 300, {1, 0, 0, 1}));
    kids.push_back(Transform::translate(
        Offset{0, 100}, cui::make_shared<Column>(std::move(inner))));
    kids.push_back(colorBox(20, 20, {0, 1, 1, 1}));  // 青：Transform 作用域之外
    h.attach(pinned(cui::make_shared<Stack>(std::move(kids))));

    check(h.at(10, 10) == 0xFF00FFFF, "作用域外的兄弟画在原点（位移未泄漏）");
    check(h.at(10, 110) == kBlue, "边界内容带祖先 Transform（下移 100）绘制");
    check(h.at(10, 130) == 0xFF00FF00, "边界之后的兄弟仍在 Transform 作用域内");

    RenderObject* rb = findRepaintBoundary(h.rv.child());
    check(rb != nullptr, "找到重绘边界");
    if (!rb)
        return;

    // 增量重录：只重录该边界，必须复用整帧重录时的坐标帧（含祖先变换）。
    rb->markNeedsPaint();
    SchedulerBinding::FrameDamage d = h.frame();
    check(d.any(), "边界增量重绘有脏区");
    check(h.at(10, 110) == kBlue, "增量重录仍带祖先 Transform（修复前画回原点）");
    check(h.at(10, 130) == 0xFF00FF00, "增量重录不影响边界之后的兄弟");
}

// ── 10. 边界被祖先推移必须重录（Custom Widget 页滚动重叠回归）─────────────────
// 边界的 picture 把窗口坐标烘进录制矩阵（不同于上游 OffsetLayer 只在合成时加
// offset），所以「边界自身没脏、却被祖先（滚动/重排）挪了位置」时复用缓存图 =
// 内容钉在旧位置，和移动后的兄弟内容叠在一起。
class ShiftState;

class Shifter : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;

private:
    friend class ShiftState;
};

class ShiftState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(colorBox(300, low_ ? 100.0 : 160.0, {1, 0, 0, 1}));
        kids.push_back(cui::make_shared<RepaintBoundary>(colorBox(50, 50, {0, 0, 1, 1})));
        return pinned(cui::make_shared<Column>(std::move(kids)));
    }
    void setLow(bool v) {
        if (low_ == v)
            return;
        low_ = v;
        setState();
    }

private:
    bool low_ = true;
};

inline std::unique_ptr<State> Shifter::createState() {
    return std::make_unique<ShiftState>();
}

void testMovedBoundaryReRecords() {
    std::printf("--- 边界被祖先推移 ---\n");

    Harness h;
    auto box = cui::make_shared<Shifter>();
    h.attach(box);

    // Column 交叉轴取最宽子（300），50 宽的蓝块水平居中 ⇒ x∈[125,175]。
    check(h.at(130, 110) == kBlue, "首帧：边界内容在红块之下（y=100 起）");
    check(h.at(130, 90) == kRed, "首帧：红块占 y<100");

    auto* st = dynamic_cast<ShiftState*>(static_cast<StatefulElement*>(h.root)->state());
    check(st != nullptr, "拿到 ShiftState");
    if (!st)
        return;

    st->setLow(false);  // 红块 100→160，把下面的边界整体下推 60
    SchedulerBinding::FrameDamage d = h.frame();
    check(d.any(), "推移那一帧有脏区");
    check(h.at(130, 170) == kBlue, "边界跟着祖先挪到新位置");
    check(h.at(130, 110) == kRed, "旧位置被重绘掉（修复前：蓝块留在原地与红块重叠）");

    SchedulerBinding::FrameDamage idle = h.frame();
    check(!idle.any(), "位置稳定后的空帧仍无脏区（没退化成每帧重录）");
}

}  // namespace

int main() {
    std::printf("=== test_repaint_damage ===\n");
    testRemovalProducesDamage();
    testUnmountMarksHostContainer();
    testRootRenderObjectSwap();
    testTeardownStillSafe();
    testSetterMarksDirty();
    testParamUpdateRepathsAndIdles();
    testDirectPaintSegmentsInterleaveWithLayers();
    testTransformAboveRepaintBoundary();
    testMovedBoundaryReRecords();

    if (g_failures == 0) {
        std::printf("\nALL PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURES\n", g_failures);
    return 1;
}
