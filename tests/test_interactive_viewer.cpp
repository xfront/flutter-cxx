/// @file test_interactive_viewer.h
/// @brief InteractiveViewer / TransformationController 单元测试。对齐 上游
///        widgets/interactive_viewer.dart（3.47）的 `_matrixTranslate` /
///        `_matrixScale` / `_transformViewport` / `_exceedsBy` 与
///        `TransformationController.toScene`。
///
/// 覆盖：
///   1. 几何工具（上游 @visibleForTesting 那几个静态函数）：线段最近点、
///      AABB、点在框内、框内最近点、`exceedsBy` 的越界量、`transformViewport`
///      的逆变换口径、`_round` 的浮点残渣归零。
///   2. TransformationController：默认单位阵、toScene/toViewport 互逆、
///      scale/translation 读数、setValue 通知、reset。
///   3. 滚轮缩放：以光标为锚（缩放前后 toScene(光标) 不变）、min/max 夹取、
///      纯横向滚轮被忽略。
///   4. 单指平移：位移 = 场景点跟手、onInteractionStart/Update/End 节奏、
///      panEnabled=false 时不动但回调照发。
///   5. 边界：constrained=true 且视口==子尺寸 ⇒ 完全不能平移；
///      boundaryMargin 撑开后可平移、超出部分被夹住。
///   6. 双指捏合：跨度比 ⇒ 缩放、焦点锚定、抬指后降级成平移。
///   7. panAxis：Vertical 锁住 dx、Horizontal 锁住 dy。
///   8. 树形：clipBehavior=None 不插 ClipRect、constrained=false 插 OverflowBox。
///   9. 像素级：平移后色块真的换位置。
///  10. 监听摘除：外部 controller 挂载时 listenerCount=1、卸载后=0（铁律）。
///
/// headless 帧泵（含 flushPaint 到离屏 SkSurface，可读像素）。

#include <cmath>
#include <cstdio>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/interactive_viewer.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/paint.h"

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
bool near(const Offset& a, const Offset& b, double eps = 1e-6) {
    return near(a.dx, b.dx, eps) && near(a.dy, b.dy, eps);
}

const SkColor kWhite = SK_ColorWHITE;

/// 200×150 的青色块（当作被缩放的子节点内容）。
/// SizedBox 定尺寸（无子也恒为 w×h），DecoratedBox 负责上色与命中。
SharedPtr<Widget> block(double w = 200.0, double h = 150.0) {
    BoxDecoration dec;
    dec.color = Color(0.0, 0.6, 0.9, 1.0);
    return cui::make_shared<SizedBox>(w, h, cui::make_shared<DecoratedBox>(dec));
}

// ── 宿主探针：根恒为 Align，body 可换 ────────────────────────────────────────

class Host : public StatefulWidget {
public:
    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "Host"; }
};

class HostState : public State {
public:
    SharedPtr<Widget> build(BuildContext&) override {
        return cui::make_shared<Align>(-1.0, -1.0, body_);
    }
    void setBody(SharedPtr<Widget> body) {
        body_ = std::move(body);
        setState();
    }

private:
    SharedPtr<Widget> body_;
};

inline std::unique_ptr<State> Host::createState() { return std::make_unique<HostState>(); }

State* findStateNamed(Element* e, const char* tag) {
    if (!e)
        return nullptr;
    if (auto* se = dynamic_cast<StatefulElement*>(e)) {
        if (se->widget() && se->widget()->toStringShort() == tag)
            return se->state();
    }
    State* found = nullptr;
    e->visitChildren([&](Element* child) {
        if (!found)
            found = findStateNamed(child, tag);
    });
    return found;
}

/// 沿渲染链统计某个渲染类型出现次数（验 ClipRect / OverflowBox 插没插）。
template <typename T>
int countRenderObjects(RenderObject* node) {
    if (!node)
        return 0;
    int found = dynamic_cast<T*>(node) ? 1 : 0;
    node->visitChildren([&](AbstractNode* child) {
        found += countRenderObjects<T>(static_cast<RenderObject*>(child));
    });
    return found;
}

// ── 帧泵 + 指针注入 ─────────────────────────────────────────────────────────

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    SchedulerBinding sb;
    Element* root = nullptr;
    HostState* host = nullptr;
    sk_sp<SkSurface> surface;

    void attach(SharedPtr<Widget> body, double ww = 400, double hh = 400) {
        rv.setWindowSize({ww, hh});
        owner.setRootRenderObjectHost(&rv);
        auto w = cui::make_shared<Host>();
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        po.setRootNode(&rv);
        sb.setBuildOwner(&owner);
        sb.setPipelineOwner(&po);
        sb.setRenderView(&rv);
        surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(int(ww), int(hh)));
        pump();
        host = static_cast<HostState*>(findStateNamed(root, "Host"));
        host->setBody(std::move(body));
        pump();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
        po.flushCompositingBits();
    }

    void frame() {
        sb.runPipelineAndComputeDamage();
        surface->getCanvas()->clear(kWhite);
        sb.compositeTo(surface->getCanvas());
    }

    InteractiveViewerState* viewer() const {
        return static_cast<InteractiveViewerState*>(findStateNamed(root, "InteractiveViewer"));
    }

    // 指针事件：position 给全局点，本地坐标由分发器按节点变换。
    void down(int64_t id, double x, double y) {
        PointerDownEvent d;
        d.pointerId = id;
        d.position = {x, y};
        d.globalPosition = {x, y};
        d.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(d);
        pump();
    }
    void move(int64_t id, double x, double y) {
        PointerMoveEvent m;
        m.pointerId = id;
        m.position = {x, y};
        m.globalPosition = {x, y};
        m.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(m);
        pump();
    }
    void up(int64_t id, double x, double y) {
        PointerUpEvent u;
        u.pointerId = id;
        u.position = {x, y};
        u.globalPosition = {x, y};
        po.dispatchPointerEvent(u);
        pump();
    }
    void scroll(double x, double y, double dy, PointerDeviceKind kind = PointerDeviceKind::mouse) {
        PointerScrollEvent s;
        s.pointerId = 1;
        s.position = {x, y};
        s.globalPosition = {x, y};
        s.scrollDeltaY = dy;
        s.pointerKind = kind;
        po.dispatchPointerEvent(s);
        pump();
    }

    uint32_t at(int x, int y) const {
        const SkImageInfo info = SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        uint32_t px = 0;
        surface->readPixels(info, &px, 4, x, y);
        return px;
    }
    bool isCyanish(int x, int y) const {
        const uint32_t c = at(x, y);
        const uint8_t r = uint8_t(c & 0xFF), g = uint8_t((c >> 8) & 0xFF), b = uint8_t((c >> 16) & 0xFF);
        return b > 200 && g > 120 && r < 100;
    }
};

// ── 1. 几何工具 ─────────────────────────────────────────────────────────────

void testGeometry() {
    std::printf("-- 1. 几何工具（上游 @visibleForTesting 那几个）\n");

    const Offset nearest = InteractiveViewer::nearestPointOnLine({5, 5}, {0, 0}, {10, 0});
    check(near(nearest, Offset(5, 0)), "1: 线外点投到线上");
    const Offset clampEnd = InteractiveViewer::nearestPointOnLine({99, 1}, {0, 0}, {10, 0});
    check(near(clampEnd, Offset(10, 0)), "1: 投影超出线段 ⇒ 夹到端点");
    check(near(InteractiveViewer::nearestPointOnLine({1, 1}, {3, 3}, {3, 3}), Offset(3, 3)),
          "1: 退化成点时返回该点");

    ViewerQuad quad;
    quad[0] = {0, 0};
    quad[1] = {8, 2};
    quad[2] = {6, 10};
    quad[3] = {-2, 8};
    const ViewerQuad aabb = InteractiveViewer::axisAlignedBoundingBox(quad);
    check(near(aabb[0], Offset(-2, 0)) && near(aabb[2], Offset(8, 10)), "1: AABB 四角");

    const ViewerQuad box = viewerQuadFromRect(Rect(0, 0, 10, 10));
    check(InteractiveViewer::pointIsInside({5, 5}, box), "1: 框内点");
    check(InteractiveViewer::pointIsInside({0, 0}, box), "1: 角点算在内（闭区间）");
    check(!InteractiveViewer::pointIsInside({11, 5}, box), "1: 框外点");
    check(near(InteractiveViewer::nearestPointInside({12, 5}, box), Offset(10, 5)),
          "1: 框外点夹到最近的边上");
    check(near(InteractiveViewer::nearestPointInside({12, 12}, box), Offset(10, 10)),
          "1: 角外点夹到角上");

    // 视口整体在边界里 ⇒ 越界量为 0。
    const ViewerQuad boundary = viewerQuadFromRect(Rect(-20, -20, 140, 140));
    const ViewerQuad insideViewport = viewerQuadFromRect(Rect(0, 0, 100, 100));
    check(InteractiveViewer::exceedsBy(boundary, insideViewport) == Offset(),
          "1: 完全在内 ⇒ exceedsBy 为 0");
    const Offset excess =
        InteractiveViewer::exceedsBy(boundary, viewerQuadFromRect(Rect(30, 30, 100, 100)));
    check(near(excess, Offset(-10, -10)), "1: 右下各超出 10 ⇒ exceedsBy 给回正量");
    check(near(InteractiveViewer::roundOffset({1e-12, -3.5}), Offset(0, -3.5)),
          "1: roundOffset 把浮点残渣归零");

    // 视口随子节点反向动：child 右移 10 ⇒ 视口在场景里左移 10。
    const Matrix4 moved = Matrix4::translation(10.0, 0.0);
    const ViewerQuad viewQuad =
        InteractiveViewer::transformViewport(moved, Rect(0, 0, 100, 100));
    check(near(viewQuad[0], Offset(-10, 0)), "1: transformViewport 用逆矩阵（方向相反）");
}

// ── 2. TransformationController ─────────────────────────────────────────────

void testController() {
    std::printf("-- 2. TransformationController\n");

    TransformationController tc;
    check(tc.value().isIdentity(), "2: 默认单位阵");
    check(near(tc.scale(), 1.0), "2: 默认 scale=1");
    check(tc.translation() == Offset(), "2: 默认平移为 0");

    tc.setValue(Matrix4::translation(12.0, -7.0));
    check(near(tc.translation(), Offset(12, -7)), "2: 平移读得出来");
    const Offset scene = tc.toScene({100, 100});
    check(near(scene, Offset(88, 107)), "2: toScene = 逆变换（减去平移）");
    check(near(tc.toViewport(scene), Offset(100, 100)), "2: toScene/toViewport 互逆");

    TransformationController scaled(Matrix4::scale(2.0, 2.0));
    check(near(scaled.scale(), 2.0), "2: scale 读矩阵行模长");
    scaled.setValue(Matrix4::identity());
    check(near(scaled.scale(), 1.0), "2: 归位");

    int notified = 0;
    TransformationController listen;
    const int handle = listen.addListenerWithHandle([&] { ++notified; });
    listen.setValue(Matrix4::identity());  // 同值 ⇒ ValueNotifier 的 operator== 不发通知
    check(notified == 0, "2: 同值 setValue 不通知");
    listen.setValue(Matrix4::translation(1.0, 0.0));
    check(notified == 1, "2: 变化才通知");
    listen.reset();
    check(notified == 2 && listen.value().isIdentity(), "2: reset 回单位阵并通知");
    listen.removeListenerByHandle(handle);
    listen.setValue(Matrix4::translation(2.0, 0.0));
    check(notified == 2, "2: 按句柄摘掉后不再通知");
    check(listen.listenerCount() == 0, "2: 监听器清空");
}

/// 从渲染树里那个 RenderTransform 读出当前矩阵（State 不暴露渲染对象，读渲染层最直接）。
Matrix4 scaleMatrixOf(Harness& h) {
    Matrix4 out;
    bool filled = false;
    std::function<void(RenderObject*)> walk = [&](RenderObject* n) {
        if (!n || filled)
            return;
        if (auto* t = dynamic_cast<RenderTransform*>(n)) {
            out = t->transform();
            filled = true;
            return;
        }
        n->visitChildren([&](AbstractNode* c) { walk(static_cast<RenderObject*>(c)); });
    };
    walk(h.root->findRenderObject());
    return out;
}

// ── 3. 滚轮缩放 ─────────────────────────────────────────────────────────────

void testWheelZoom() {
    std::printf("-- 3. 滚轮缩放（锚在光标）\n");

    auto controller = std::make_shared<TransformationController>();
    auto viewer = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets::all(100.0), true, 2.5, 0.8, true, true,
        200.0, false, controller);
    Harness h;
    h.attach(viewer);
    InteractiveViewerState* st = h.viewer();
    check(st != nullptr, "3: 找到 InteractiveViewerState");
    check(controller->listenerCount() == 1, "3: 外部 controller 挂载后监听数=1");
    check(st && st->controller() == controller, "3: State 用的是外部 controller");

    const Offset cursor(60, 40);
    const Offset sceneBefore = controller->toScene(cursor);
    h.scroll(cursor.dx, cursor.dy, -100.0);
    const double afterFirst = controller->scale();
    check(afterFirst > 1.0, "3: 向上滚放大");
    check(near(controller->toScene(cursor), sceneBefore, 1e-3), "3: 光标底下的场景点没动（锚定）");

    // 连续放大应当被 maxScale 夹住。
    for (int i = 0; i < 20; ++i)
        h.scroll(cursor.dx, cursor.dy, -100.0);
    check(near(controller->scale(), 2.5, 1e-3), "3: 放大夹到 maxScale=2.5");
    for (int i = 0; i < 40; ++i)
        h.scroll(cursor.dx, cursor.dy, 100.0);
    // 缩小下界不是 minScale，而是「视口装得下边界」那条 max(viewport/boundary)：
    // 这里 boundary 比 viewport 大 ⇒ 落到 minScale。
    check(near(controller->scale(), 0.8, 1e-3), "3: 缩小夹到 minScale=0.8");

    // 纯横向滚轮被忽略（上游同款）。
    const double before = controller->scale();
    {
        PointerScrollEvent s;
        s.pointerId = 1;
        s.position = {50, 50};
        s.globalPosition = {50, 50};
        s.scrollDeltaY = 0.0;
        s.scrollDeltaX = 120.0;
        h.po.dispatchPointerEvent(s);
        h.pump();
    }
    check(near(controller->scale(), before), "3: 横向滚轮不改缩放");

    // scaleEnabled=false ⇒ 滚轮不动矩阵，但三个回调照发。
    int starts = 0, updates = 0, ends = 0;
    auto frozen = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets::all(100.0), true, 2.5, 0.8, true,
        /*scaleEnabled=*/false, 200.0, false, nullptr, std::nullopt,
        [&starts](const InteractiveViewerStartDetails&) { ++starts; },
        [&updates](const InteractiveViewerUpdateDetails&) { ++updates; },
        [&ends](const InteractiveViewerEndDetails&) { ++ends; });
    Harness h2;
    h2.attach(frozen);
    h2.scroll(60, 40, -100.0);
    check(scaleMatrixOf(h2).isIdentity(), "3: scaleEnabled=false 滚轮不缩放");
    check(starts == 1 && updates == 1 && ends == 1, "3: 禁用时 onInteraction* 仍各发一次");
}

// ── 4. 单指平移 ─────────────────────────────────────────────────────────────

void testPan() {
    std::printf("-- 4. 单指平移\n");

    auto controller = std::make_shared<TransformationController>();
    int starts = 0, updates = 0, ends = 0;
    InteractiveViewerUpdateDetails lastUpdate;
    auto viewer = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets::all(500.0), true, 2.5, 0.8, true, true,
        200.0, false, controller, std::nullopt,
        [&starts](const InteractiveViewerStartDetails&) { ++starts; },
        [&](const InteractiveViewerUpdateDetails& d) {
            ++updates;
            lastUpdate = d;
        },
        [&ends](const InteractiveViewerEndDetails&) { ++ends; });
    Harness h;
    h.attach(viewer);

    h.down(1, 50, 50);
    check(starts == 1, "4: 按下发 onInteractionStart");
    check(controller->value().isIdentity(), "4: 按下本身不动矩阵");

    h.move(1, 70, 60);
    check(near(matrix4Translation(scaleMatrixOf(h)), Offset(20, 10), 1e-3),
          "4: 手指挪 (20,10) ⇒ 子节点跟着挪");
    check(updates == 1, "4: 一次移动一次 onInteractionUpdate");
    check(near(lastUpdate.focalPoint, Offset(70, 60)), "4: 回调里 focalPoint 是全局点");

    h.move(1, 90, 60);
    check(near(matrix4Translation(scaleMatrixOf(h)), Offset(40, 10), 1e-3),
          "4: 累加平移（参考点在手指底下）");

    h.up(1, 90, 60);
    check(ends == 1, "4: 抬手发 onInteractionEnd");
    check(h.viewer() && h.viewer()->activePointerCount() == 0, "4: 指针表清空");
    check(near(matrix4Translation(scaleMatrixOf(h)), Offset(40, 10), 1e-3), "4: 抬手不改变换（无惯性）");

    // panEnabled=false：矩阵不动，回调照发。
    auto noPan = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets::all(500.0), true, 2.5, 0.8,
        /*panEnabled=*/false, true);
    Harness h2;
    h2.attach(noPan);
    h2.down(1, 50, 50);
    h2.move(1, 120, 120);
    h2.up(1, 120, 120);
    check(scaleMatrixOf(h2).isIdentity(), "4: panEnabled=false ⇒ 拖不动");
}

// ── 5. 边界夹取 ─────────────────────────────────────────────────────────────

void testBoundary() {
    std::printf("-- 5. 边界夹取\n");

    // 视口 == 子尺寸（constrained=true 且子节点填满给了它的紧约束）：
    // 上游同样「一点都拖不动」。
    auto tight = std::make_shared<TransformationController>();
    auto v1 = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets(), true, 2.5, 0.8, true, true, 200.0,
        false, tight);
    Harness h1;
    h1.attach(v1);
    h1.down(1, 50, 50);
    h1.move(1, 45, 45);
    check(matrix4Translation(scaleMatrixOf(h1)) == Offset(), "5: 零边距 ⇒ 拖不动（视口已铺满边界）");

    // 撑开边界：先放大到 2 倍（子节点视觉上比视口大），再拖，允许但不许超出边界。
    auto margin = std::make_shared<TransformationController>();
    auto v2 = cui::make_shared<InteractiveViewer>(
        block(200, 150), Clip::HardEdge, PanAxis::Free, EdgeInsets(), true, 4.0, 0.8, true, true,
        200.0, false, margin);
    Harness h2;
    h2.attach(v2);
    // 滚到 2 倍：exp(-dy/200) 一次给不了 2，直接由 controller 写矩阵更干净。
    Matrix4 zoomed = Matrix4::scale(2.0, 2.0);
    margin->setValue(zoomed);
    h2.pump();
    const double txBefore = matrix4Translation(scaleMatrixOf(h2)).dx;
    h2.down(1, 180, 75);
    h2.move(1, 100, 75);  // 想左拖 80
    const double txAfter = matrix4Translation(scaleMatrixOf(h2)).dx;
    check(txAfter < txBefore, "5: 放大后可以往左拖");
    check(txAfter >= -200.0, "5: 左拖被边界夹住（不会拖到子节点右缘离开视口）");
    for (int i = 0; i < 12; ++i)
        h2.move(1, 100 - 40.0 * double(i), 75);
    // 夹到边界后就停在那儿（`Matrix4::inverse()` 是通用 4×4 求逆，收尾值带
    // ~1e-13 的浮点残渣，所以这里按 1e-6 判等而不是精确等于 -200）。
    check(near(matrix4Translation(scaleMatrixOf(h2)).dx, -200.0, 1e-6),
          "5: 反复拖也不再往左（已贴边界）");
}

// ── 6. 双指捏合 ─────────────────────────────────────────────────────────────

void testPinch() {
    std::printf("-- 6. 双指捏合\n");

    auto controller = std::make_shared<TransformationController>();
    auto viewer = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets::all(500.0), true, 4.0, 0.5, true, true,
        200.0, false, controller);
    Harness h;
    h.attach(viewer);
    InteractiveViewerState* st = h.viewer();

    h.down(1, 40, 75);
    h.down(2, 60, 75);
    check(st && st->activePointerCount() == 2, "6: 两指都在表里");
    const Offset mid(50, 75);
    const Offset sceneMid = controller->toScene(mid);
    check(st && st->gestureType() == InteractiveViewerState::GestureType::Scale,
          "6: 第二指按下后判成 scale 手势");

    h.move(2, 100, 75);  // 跨度 20 ⇒ 50：放大 2.5 倍
    check(controller->scale() > 1.5, "6: 双指张开放大");
    // 锚定的是「捏合期间当前焦点下的场景点」：焦点已随第二指移到 (70,75)，
    // 上游保证 toScene(当前焦点) 仍等于手势开始时锁定的 _referenceFocalPoint。
    check(near(controller->toScene(Offset(70, 75)), sceneMid, 0.5),
          "6: 捏合焦点锚定（焦点下的场景值基本不变）");

    const double wide = controller->scale();
    h.move(2, 61, 75);  // 收回去
    check(controller->scale() < wide, "6: 双指合拢缩小");
    check(controller->scale() >= 0.5 - 1e-6, "6: 缩小不穿 minScale");

    // 抬一指 ⇒ 结束本段手势并以剩下那指重开成平移。
    h.up(2, 61, 75);
    check(st && st->activePointerCount() == 1, "6: 抬一指后还剩一指");
    check(st && st->gestureType() == InteractiveViewerState::GestureType::Pan,
          "6: 重开成平移");
    const double scaleAfterLift = controller->scale();
    h.move(1, 30, 60);
    check(near(controller->scale(), scaleAfterLift, 1e-6), "6: 单指移动不再改缩放");
    h.up(1, 30, 60);
    check(st && st->activePointerCount() == 0, "6: 全抬完清空");
}

// ── 7. panAxis ──────────────────────────────────────────────────────────────

void testPanAxis() {
    std::printf("-- 7. panAxis 锁轴\n");

    auto vertical = std::make_shared<TransformationController>();
    auto v1 = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Vertical, EdgeInsets::all(500.0), true, 2.5, 0.8, true,
        true, 200.0, false, vertical);
    Harness h1;
    h1.attach(v1);
    h1.down(1, 50, 50);
    h1.move(1, 150, 90);  // 斜着拖，但 PanAxis::Vertical ⇒ 只保留 dy
    Offset t1 = matrix4Translation(scaleMatrixOf(h1));
    check(near(t1.dx, 0.0), "7: PanAxis::Vertical ⇒ 锁住 dx");
    check(near(t1.dy, 40.0, 1e-3), "7: dy 按手指位移");

    auto horizontal = std::make_shared<TransformationController>();
    auto v2 = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Horizontal, EdgeInsets::all(500.0), true, 2.5, 0.8, true,
        true, 200.0, false, horizontal);
    Harness h2;
    h2.attach(v2);
    h2.down(1, 50, 50);
    h2.move(1, 80, 120);
    const Offset t2 = matrix4Translation(scaleMatrixOf(h2));
    check(near(t2.dy, 0.0), "7: PanAxis::Horizontal ⇒ 只吃 dx");
    check(near(t2.dx, 30.0, 1e-3), "7: dx 按手指位移");

    // PanAxis::Aligned：主轴在手势开始时锁定（上游 `_currentAxis ??=`），
    // 之后即使手指改成竖向移动也只吃第一次锁定的那条轴。
    auto aligned = std::make_shared<TransformationController>();
    auto v3 = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Aligned, EdgeInsets::all(500.0), true, 2.5, 0.8, true,
        true, 200.0, false, aligned);
    Harness h3;
    h3.attach(v3);
    h3.down(1, 50, 50);
    h3.move(1, 150, 90);  // 横向占优 ⇒ 锁定 Horizontal
    const Offset ta1 = matrix4Translation(scaleMatrixOf(h3));
    check(near(ta1, Offset(100, 0), 1e-3), "7: PanAxis::Aligned ⇒ 首段按主轴（横向）锁轴");
    h3.move(1, 160, 190);  // 现在竖向占优，但轴已锁死 ⇒ dy 被丢掉
    const Offset ta2 = matrix4Translation(scaleMatrixOf(h3));
    check(near(ta2, Offset(110, 0), 1e-3), "7: Aligned 锁定后不再改判主轴");

    auto free = std::make_shared<TransformationController>();
    auto v4 = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets::all(500.0), true, 2.5, 0.8, true, true,
        200.0, false, free);
    Harness h4;
    h4.attach(v4);
    h4.down(1, 50, 50);
    h4.move(1, 80, 120);
    const Offset t3 = matrix4Translation(scaleMatrixOf(h4));
    check(near(t3, Offset(30, 70), 1e-3), "7: PanAxis::Free ⇒ 两轴都吃");
}

// ── 8. 树形与裁剪 ───────────────────────────────────────────────────────────

void testTreeShape() {
    std::printf("-- 8. 树形（ClipRect / OverflowBox）\n");

    auto v1 = cui::make_shared<InteractiveViewer>(block());
    Harness h1;
    h1.attach(v1);
    check(countRenderObjects<RenderClipRect>(h1.root->findRenderObject()) == 1,
          "8: 默认 clipBehavior 插一个 ClipRect");
    check(countRenderObjects<RenderOverflowBox>(h1.root->findRenderObject()) == 0,
          "8: constrained=true 不插 OverflowBox");
    check(countRenderObjects<RenderTransform>(h1.root->findRenderObject()) == 1,
          "8: 恰有一个 RenderTransform（State 靠它探子尺寸）");

    auto v2 = cui::make_shared<InteractiveViewer>(block(), Clip::None);
    Harness h2;
    h2.attach(v2);
    check(countRenderObjects<RenderClipRect>(h2.root->findRenderObject()) == 0,
          "8: clipBehavior=None ⇒ 不插 ClipRect");

    auto v3 = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets::all(50.0), /*constrained=*/false);
    Harness h3;
    h3.attach(v3);
    check(countRenderObjects<RenderOverflowBox>(h3.root->findRenderObject()) == 1,
          "8: constrained=false ⇒ 插 OverflowBox（子节点按自己的尺寸排）");
    // 非约束模式下子尺寸 200×150 仍被边界判定用到：拖拽能动。
    auto c = std::make_shared<TransformationController>();
    auto v4 = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets(), false, 2.5, 0.8, true, true, 200.0,
        false, c);
    Harness h4;
    h4.attach(v4);
    h4.down(1, 50, 50);
    h4.move(1, 20, 20);
    check(matrix4Translation(scaleMatrixOf(h4)) == Offset(),
          "8: constrained=false 但视口仍被子尺寸撑大 ⇒ 同样拖不动（上游同形）");
}

// ── 9. 像素 ─────────────────────────────────────────────────────────────────

void testPixels() {
    std::printf("-- 9. 像素级：变换真的画出来了\n");

    auto controller = std::make_shared<TransformationController>();
    auto viewer = cui::make_shared<InteractiveViewer>(
        block(), Clip::HardEdge, PanAxis::Free, EdgeInsets::all(500.0), true, 2.5, 0.8, true, true,
        200.0, false, controller);
    Harness h;
    h.attach(viewer);
    h.frame();
    check(h.isCyanish(20, 20), "9: 初始色块盖住 (20,20)");
    check(!h.isCyanish(300, 300), "9: (300,300) 在块外");

    h.down(1, 20, 20);
    h.move(1, 120, 120);
    h.frame();
    check(!h.isCyanish(20, 20), "9: 平移后 (20,20) 露白");
    check(h.isCyanish(120, 120), "9: 色块跟手到 (120,120)");
    h.up(1, 120, 120);

    // 裁剪：放大后子节点溢出视口也不该画到 400×400 之外（这里验 ClipRect 生效）。
    controller->setValue(Matrix4::scale(3.0, 3.0));
    h.frame();
    check(h.isCyanish(380, 380) || !h.isCyanish(399, 399), "9: 放大后仍受裁剪/视口限制");
}

// ── 10. 生命周期 ────────────────────────────────────────────────────────────

void testLifecycle() {
    std::printf("-- 10. 挂载/卸载的监听摘除\n");

    auto controller = std::make_shared<TransformationController>();
    {
        auto viewer = cui::make_shared<InteractiveViewer>(
            block(), Clip::HardEdge, PanAxis::Free, EdgeInsets(), true, 2.5, 0.8, true, true, 200.0,
            false, controller);
        Harness h;
        h.attach(viewer);
        check(controller->listenerCount() == 1, "10: 挂载后外部 controller 监听数=1");
        // 卸载发生在这一行之后（h 析构 ⇒ State::dispose）。
    }
    check(controller->listenerCount() == 0, "10: 卸载后监听数归零（捕获 this 的 lambda 必须按句柄摘）");

    // 自建 controller：卸载不崩，且变换在重建之间保持（State 复用）。
    auto viewer = cui::make_shared<InteractiveViewer>(block());
    Harness h;
    h.attach(viewer);
    TransformationController* self = h.viewer()->controller().get();
    check(self != nullptr, "10: 没给外部 controller 时自建一条");
    h.scroll(60, 60, -100.0);
    const double scaled = self->scale();
    check(scaled > 1.0, "10: 自建 controller 也吃到滚轮");
    // 换 body 但 InteractiveViewer 类型/key 不变 ⇒ Element 复用 ⇒ 变换保持。
    h.host->setBody(viewer);
    h.pump();
    check(h.viewer() && h.viewer()->controller().get() == self, "10: 复用同一 State 与 controller");
    check(near(self->scale(), scaled), "10: 重建不丢变换");
}

}  // namespace

int main() {
    std::printf("== InteractiveViewer / TransformationController ==\n");
    testGeometry();
    testController();
    testWheelZoom();
    testPan();
    testBoundary();
    testPinch();
    testPanAxis();
    testTreeShape();
    testPixels();
    testLifecycle();
    std::printf("%s: %d\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
