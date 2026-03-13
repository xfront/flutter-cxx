#pragma once
/// @file widgets/interactive_viewer.h
/// @brief InteractiveViewer / TransformationController（header-only）。对齐
///        上游 widgets/interactive_viewer.dart（3.47）的 `_InteractiveViewerState`
///        那一整套矩阵算法：`_matrixTranslate`（带边界夹取）、`_matrixScale`
///        （带 min/max 夹取）、`_transformViewport`、`_exceedsBy`、`_alignAxis`。
///
/// 结构（与上游同一棵树，只是把 GlobalKey 探尺寸换成沿渲染链下探）：
///   Listener → ClipRect → [OverflowBox] → Transform → child
///   `constrained=false` 时才插 OverflowBox；`clipBehavior=None` 时不插 ClipRect。
///
/// 有意偏离 / 未移植（勿当 bug 排查）：
///   1. **输入走 Listener，不走 GestureDetector.onScale**。上游靠
///      `ScaleGestureRecognizer` 合并单指平移与双指捏合；本仓的 GestureDetector
///      是单指针识别器（没有 scale），所以这里自带一个多指针表（按 pointerId
///      记 global/local 位置），逐条复刻上游 `_onScaleStart/_onScaleUpdate/
///      _onScaleEnd` 的判定顺序，包括「手势一开始 scale 恰好等于 1 时先判成 pan、
///      下一帧再改判成 scale」那段（上游注释在 _onScaleUpdate 开头）。
///   2. **无甩动惯性**（`_controller` / `_scaleController` + FrictionSimulation）。
///      本仓指针事件不带速度估计，抬手即停。`onInteractionEnd` 照样发。
///   3. **无旋转**：上游 `_rotateEnabled = false` 同样没接，`_currentRotation`
///      恒 0 ⇒ `_getAxisAlignedBoundingBoxWithRotation` 退化成边界框本身，
///      这里直接不写那段旋转扩框。
///   4. 滚轮：`PointerScrollEvent` 的 `pointerKind` 是 trackpad 且
///      `trackpadScrollCausesScale=false` 时按平移处理（上游同款），其余按
///      `exp(-dy / scaleFactor)` 缩放。
///   5. `InteractiveViewer.builder`（懒构建 + viewport Quad 回调）未移植；
///      需要时按上游 `_InteractiveViewerBuilt` 那条 LayoutBuilder 路径补。
///   6. 上游 `PointerCancelEvent` 在本仓的 `RenderPointerListener` 里没有分支
///      （switch 不认 Cancel），所以取消路径靠 `onUp`；宿主若要发 Cancel 需先补
///      渲染层。

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/matrix4.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/rendering/animated_size.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/widgets/layout.h"
#include "cui/framework/listener.h"
#include "cui/widgets/paint.h"

namespace cui {

// ── Matrix4 的 2D 辅助 ──────────────────────────────────────────────────────

/// 上游的 `Matrix4.getMaxScaleOnAxis()`：三行线性部分取模长的最大值。
inline double matrix4MaxScaleOnAxis(const Matrix4& m) {
    double best = 0.0;
    for (int row = 0; row < 3; ++row) {
        const double a = m.at(row, 0), b = m.at(row, 1), c = m.at(row, 2);
        best = std::max(best, std::sqrt(a * a + b * b + c * c));
    }
    return best;
}

/// 上游的 `Matrix4.getTranslation()`（只取 x/y）。
inline Offset matrix4Translation(const Matrix4& m) { return {m.at(0, 3), m.at(1, 3)}; }

/// 上游的 `matrix.clone()..setTranslation(v)`：只改平移列，线性部分原样。
inline Matrix4 matrix4WithTranslation(const Matrix4& m, const Offset& t) {
    Matrix4 out = m;
    out.set(0, 3, t.dx);
    out.set(1, 3, t.dy);
    return out;
}

/// 上游的 `matrix.clone()..scaleByDouble(s, s, s, 1)`。
inline Matrix4 matrix4Scaled(const Matrix4& m, double s) {
    Matrix4 out = m;
    out.scaleBy(s, s, s);
    return out;
}

/// 上游的 `matrix.clone()..translateByDouble(dx, dy, 0, 1)`。
inline Matrix4 matrix4Translated(const Matrix4& m, const Offset& t) {
    Matrix4 out = m;
    out.translate(t.dx, t.dy, 0.0);
    return out;
}

/// 上游 `clampDouble`。
inline double clampDoubleValue(double value, double lower, double upper) {
    return std::max(lower, std::min(upper, value));
}

// ── TransformationController ────────────────────────────────────────────────

/// 承载子节点变换的 ValueNotifier<Matrix4>。上游的 `TransformationController`。
class TransformationController : public ValueNotifier<Matrix4> {
public:
    TransformationController() : ValueNotifier<Matrix4>(Matrix4::identity()) {}
    explicit TransformationController(Matrix4 value) : ValueNotifier<Matrix4>(std::move(value)) {}

    double scale() const { return matrix4MaxScaleOnAxis(value()); }
    Offset translation() const { return matrix4Translation(value()); }

    /// 视口点 → 场景点（= 逆变换）。上游的 `toScene(viewportPoint)`。
    Offset toScene(const Offset& viewportPoint) const {
        return value().inverse().transformOffset(viewportPoint);
    }
    /// 场景点 → 视口点。
    Offset toViewport(const Offset& scenePoint) const { return value().transformOffset(scenePoint); }

    void reset() { setValue(Matrix4::identity()); }
};

using TransformationControllerPtr = std::shared_ptr<TransformationController>;

// ── InteractiveViewer 的参数与回调 ──────────────────────────────────────────

/// 上游的 `PanAxis`。
enum class PanAxis {
    Free,       ///< 任意方向。
    Horizontal, ///< 只允许水平移动。
    Vertical,   ///< 只允许垂直移动。
    Aligned,    ///< 手势开始时锁定主轴，之后只沿该轴。
};

/// 上游的 `ScaleStartDetails` 的可用子集。
struct InteractiveViewerStartDetails {
    Offset focalPoint;      ///< 全局（窗口）坐标
    Offset localFocalPoint; ///< 相对 InteractiveViewer 的坐标
};

/// 上游的 `ScaleUpdateDetails` 的可用子集（rotation 恒 0，见文件头第 3 条）。
struct InteractiveViewerUpdateDetails {
    Offset focalPoint;
    Offset localFocalPoint;
    Offset focalPointDelta;
    double scale = 1.0;
};

/// 上游的 `ScaleEndDetails`（无 velocity ⇒ 不做惯性）。
struct InteractiveViewerEndDetails {};

using InteractionStartCallback = std::function<void(const InteractiveViewerStartDetails&)>;
using InteractionUpdateCallback = std::function<void(const InteractiveViewerUpdateDetails&)>;
using InteractionEndCallback = std::function<void(const InteractiveViewerEndDetails&)>;

/// 内部用的四角形（上游的 `Quad`）。边界判定只需要轴对齐框与任意四边形，
/// 这里用一个固定大小的点集表达。
struct ViewerQuad {
    Offset points[4]{};

    const Offset& operator[](size_t i) const { return points[i]; }
    Offset& operator[](size_t i) { return points[i]; }
};

inline ViewerQuad viewerQuadFromRect(const Rect& r) {
    ViewerQuad q;
    q[0] = {r.left(), r.top()};
    q[1] = {r.right(), r.top()};
    q[2] = {r.right(), r.bottom()};
    q[3] = {r.left(), r.bottom()};
    return q;
}

/// 上游的 `InteractiveViewer`。平移 / 缩放子节点。
class InteractiveViewer : public StatefulWidget {
public:
    /// 参数顺序 = 上游命名参数顺序，`child` 置首（本仓 SingleChild 控件的写法），
    /// 新参一律追加在 `key` 之前。
    explicit InteractiveViewer(SharedPtr<Widget> child, Clip clipBehavior = Clip::HardEdge,
                               PanAxis panAxis = PanAxis::Free,
                               EdgeInsets boundaryMargin = EdgeInsets(), bool constrained = true,
                               double maxScale = 2.5, double minScale = 0.8, bool panEnabled = true,
                               bool scaleEnabled = true, double scaleFactor = 200.0,
                               bool trackpadScrollCausesScale = false,
                               TransformationControllerPtr transformationController = nullptr,
                               std::optional<Alignment> alignment = std::nullopt,
                               InteractionStartCallback onInteractionStart = {},
                               InteractionUpdateCallback onInteractionUpdate = {},
                               InteractionEndCallback onInteractionEnd = {}, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)), clipBehavior_(clipBehavior),
          panAxis_(panAxis), boundaryMargin_(boundaryMargin), constrained_(constrained),
          maxScale_(maxScale), minScale_(minScale), panEnabled_(panEnabled),
          scaleEnabled_(scaleEnabled), scaleFactor_(scaleFactor),
          trackpadScrollCausesScale_(trackpadScrollCausesScale),
          transformationController_(std::move(transformationController)), alignment_(alignment),
          onInteractionStart_(std::move(onInteractionStart)),
          onInteractionUpdate_(std::move(onInteractionUpdate)),
          onInteractionEnd_(std::move(onInteractionEnd)) {}

    /// 上游的 `InteractiveViewer.simpleScale`（只有缩放，拖拽留给调用方）。
    static SharedPtr<InteractiveViewer> simpleScale(SharedPtr<Widget> child,
                                                          TransformationControllerPtr controller,
                                                          double maxScale = 4.0,
                                                          double minScale = 0.5) {
        return cui::make_shared<InteractiveViewer>(
            std::move(child), Clip::HardEdge, PanAxis::Free, EdgeInsets(), true, maxScale, minScale,
            /*panEnabled=*/false, /*scaleEnabled=*/true, 200.0, false, std::move(controller));
    }

    // ── 访问器 / 就地 setter（宿主每帧原地改，updateRenderObject/build 读新值）──

    const SharedPtr<Widget>& child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    Clip clipBehavior() const { return clipBehavior_; }
    void setClipBehavior(Clip v) { clipBehavior_ = v; }

    PanAxis panAxis() const { return panAxis_; }
    void setPanAxis(PanAxis v) { panAxis_ = v; }

    const EdgeInsets& boundaryMargin() const { return boundaryMargin_; }
    void setBoundaryMargin(const EdgeInsets& v) { boundaryMargin_ = v; }

    bool constrained() const { return constrained_; }
    void setConstrained(bool v) { constrained_ = v; }

    double maxScale() const { return maxScale_; }
    void setMaxScale(double v) { maxScale_ = v; }
    double minScale() const { return minScale_; }
    void setMinScale(double v) { minScale_ = v; }

    bool panEnabled() const { return panEnabled_; }
    void setPanEnabled(bool v) { panEnabled_ = v; }
    bool scaleEnabled() const { return scaleEnabled_; }
    void setScaleEnabled(bool v) { scaleEnabled_ = v; }
    double scaleFactor() const { return scaleFactor_; }
    void setScaleFactor(double v) { scaleFactor_ = v; }
    bool trackpadScrollCausesScale() const { return trackpadScrollCausesScale_; }
    void setTrackpadScrollCausesScale(bool v) { trackpadScrollCausesScale_ = v; }

    const TransformationControllerPtr& transformationController() const {
        return transformationController_;
    }
    void setTransformationController(TransformationControllerPtr c) {
        transformationController_ = std::move(c);
    }

    const std::optional<Alignment>& alignment() const { return alignment_; }
    void setAlignment(std::optional<Alignment> a) { alignment_ = a; }

    const InteractionStartCallback& onInteractionStart() const { return onInteractionStart_; }
    const InteractionUpdateCallback& onInteractionUpdate() const { return onInteractionUpdate_; }
    const InteractionEndCallback& onInteractionEnd() const { return onInteractionEnd_; }
    void setOnInteractionStart(InteractionStartCallback cb) {
        onInteractionStart_ = std::move(cb);
    }
    void setOnInteractionUpdate(InteractionUpdateCallback cb) {
        onInteractionUpdate_ = std::move(cb);
    }
    void setOnInteractionEnd(InteractionEndCallback cb) { onInteractionEnd_ = std::move(cb); }

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "InteractiveViewer"; }

    // ── 上游 @visibleForTesting 的几何工具 ──────────────────────────────────

    /// 上游的 `getNearestPointOnLine`。
    static Offset nearestPointOnLine(const Offset& point, const Offset& l1, const Offset& l2) {
        const double lengthSquared = std::pow(l2.dx - l1.dx, 2.0) + std::pow(l2.dy - l1.dy, 2.0);
        if (lengthSquared == 0.0)
            return l1;
        const Offset l1p = point - l1;
        const Offset l1l2 = l2 - l1;
        const double fraction =
            clampDoubleValue((l1p.dx * l1l2.dx + l1p.dy * l1l2.dy) / lengthSquared, 0.0, 1.0);
        return {l1.dx + l1l2.dx * fraction, l1.dy + l1l2.dy * fraction};
    }

    /// 上游的 `getAxisAlignedBoundingBox`。
    static ViewerQuad axisAlignedBoundingBox(const ViewerQuad& quad) {
        double minX = quad[0].dx, minY = quad[0].dy, maxX = quad[0].dx, maxY = quad[0].dy;
        for (const Offset& p : quad.points) {
            minX = std::min(minX, p.dx);
            minY = std::min(minY, p.dy);
            maxX = std::max(maxX, p.dx);
            maxY = std::max(maxY, p.dy);
        }
        ViewerQuad out;
        out[0] = {minX, minY};
        out[1] = {maxX, minY};
        out[2] = {maxX, maxY};
        out[3] = {minX, maxY};
        return out;
    }

    /// 上游的 `pointIsInside`（闭区间；只对轴对齐框严格成立，与上游同口径）。
    static bool pointIsInside(const Offset& point, const ViewerQuad& quad) {
        const Offset aM = point - quad[0];
        const Offset aB = quad[1] - quad[0];
        const Offset aD = quad[3] - quad[0];
        const double aMAB = aM.dx * aB.dx + aM.dy * aB.dy;
        const double aBAB = aB.dx * aB.dx + aB.dy * aB.dy;
        const double aMAD = aM.dx * aD.dx + aM.dy * aD.dy;
        const double aDAD = aD.dx * aD.dx + aD.dy * aD.dy;
        return 0.0 <= aMAB && aMAB <= aBAB && 0.0 <= aMAD && aMAD <= aDAD;
    }

    /// 上游的 `getNearestPointInside`。
    static Offset nearestPointInside(const Offset& point, const ViewerQuad& quad) {
        if (pointIsInside(point, quad))
            return point;
        Offset best;
        double bestDistance = -1.0;
        for (size_t i = 0; i < 4; ++i) {
            const Offset candidate =
                nearestPointOnLine(point, quad[i], quad[(i + 1) % 4]);
            const double dx = point.dx - candidate.dx, dy = point.dy - candidate.dy;
            const double distance = std::sqrt(dx * dx + dy * dy);
            if (bestDistance < 0.0 || distance < bestDistance) {
                bestDistance = distance;
                best = candidate;
            }
        }
        return best;
    }

    /// 上游的 `_round`：把 1e-10 量级的浮点残渣收成 0，避免「夹到边界后仍算越界」。
    static Offset roundOffset(const Offset& o) {
        auto snap = [](double v) {
            return std::abs(v) < 1e-9 ? 0.0 : std::round(v * 1e9) / 1e9;
        };
        return {snap(o.dx), snap(o.dy)};
    }

    /// 上游的 `_transformViewport`：视口四角乘**逆**矩阵（子节点动 = 视口反向动）。
    static ViewerQuad transformViewport(const Matrix4& matrix, const Rect& viewport) {
        const Matrix4 inverse = matrix.inverse();
        ViewerQuad q;
        const Offset corners[4] = {{viewport.left(), viewport.top()},
                                   {viewport.right(), viewport.top()},
                                   {viewport.right(), viewport.bottom()},
                                   {viewport.left(), viewport.bottom()}};
        for (size_t i = 0; i < 4; ++i)
            q[i] = inverse.transformOffset(corners[i]);
        return q;
    }

    /// 上游的 `_exceedsBy`：视口超出边界多少；完全在内则 (0,0)。
    static Offset exceedsBy(const ViewerQuad& boundary, const ViewerQuad& viewport) {
        Offset largest{0.0, 0.0};
        for (const Offset& point : viewport.points) {
            const Offset inside = nearestPointInside(point, boundary);
            const Offset excess = {inside.dx - point.dx, inside.dy - point.dy};
            if (std::abs(excess.dx) > std::abs(largest.dx))
                largest = {excess.dx, largest.dy};
            if (std::abs(excess.dy) > std::abs(largest.dy))
                largest = {largest.dx, excess.dy};
        }
        return roundOffset(largest);
    }

private:
    SharedPtr<Widget> child_;
    Clip clipBehavior_ = Clip::HardEdge;
    PanAxis panAxis_ = PanAxis::Free;
    EdgeInsets boundaryMargin_;
    bool constrained_ = true;
    double maxScale_ = 2.5;
    double minScale_ = 0.8;
    bool panEnabled_ = true;
    bool scaleEnabled_ = true;
    double scaleFactor_ = 200.0;
    bool trackpadScrollCausesScale_ = false;
    TransformationControllerPtr transformationController_;
    std::optional<Alignment> alignment_;
    InteractionStartCallback onInteractionStart_;
    InteractionUpdateCallback onInteractionUpdate_;
    InteractionEndCallback onInteractionEnd_;
};

// ── InteractiveViewerState ──────────────────────────────────────────────────

class InteractiveViewerState : public State {
public:
    /// 上游的 `_GestureType`（rotate 未接，见文件头第 3 条）。
    enum class GestureType { None, Pan, Scale };

    void initState() override {
        auto* w = widget_();
        controller_ = w && w->transformationController()
                          ? w->transformationController()
                          : std::make_shared<TransformationController>();
        controllerExternal_ = bool(w && w->transformationController());
        listenerHandle_ = controller_->addListenerWithHandle([this] { handleTransformation(); });
    }

    void dispose() override {
        // 铁律：捕获 this 的 lambda 只能按句柄摘（比较裸函数指针永远摘不掉）。
        if (controller_ && listenerHandle_ >= 0)
            controller_->removeListenerByHandle(listenerHandle_);
        listenerHandle_ = -1;
    }

    /// 外部/内部持有的 controller。宿主在 build 之前原地换掉时也会跟上。
    const TransformationControllerPtr& controller() const { return controller_; }

    /// 当前正在跟踪的指针数（单指 = 平移，两指 = 捏合）。
    size_t activePointerCount() const { return pointers_.size(); }
    GestureType gestureType() const { return gestureType_; }

    SharedPtr<Widget> build(BuildContext&) override {
        auto* w = widget_();
        if (!w)
            return nullptr;
        adoptController(w);

        SharedPtr<Widget> content = cui::make_shared<Transform>(
            controller_->value(), w->child(), Offset(), true, nullptr, w->alignment());
        if (!w->constrained()) {
            content = cui::make_shared<OverflowBox>(
                0.0, BoxConstraints::kInfinity, 0.0, BoxConstraints::kInfinity,
                Alignment::topLeft(), std::move(content));
        }
        if (w->clipBehavior() != Clip::None)
            content = cui::make_shared<ClipRect>(std::move(content));

        return cui::make_shared<Listener>(
            [this](const PointerEvent& e) { handleDown(e); },
            [this](const PointerEvent& e) { handleMove(e); },
            [this](const PointerEvent& e) { handleUp(e); },
            Listener::PointerCallback{},   // hover 不参与手势
            [this](const PointerEvent& e) { handleScroll(e); },
            std::move(content));
    }

    // ── 上游公开的矩阵算法（暴露出来便于单测，与上游 static 工具同形）────────

    /// 上游的 `_matrixTranslate`：先试平移，越界就夹到最近的可落点。
    Matrix4 matrixTranslate(const Matrix4& matrix, Offset translation) const {
        auto* w = widget_();
        if (translation == Offset())
            return matrix;
        // 上游的 switch（rendering 里那句）按 **widget.panAxis** 分派，
        // `_currentAxis` 只在 PanAxis::Aligned 时才用到；且只有已经锁定过主轴
        // （currentAxis 非空）才做对齐。
        if (w && currentAxis_) {
            switch (w->panAxis()) {
                case PanAxis::Horizontal: translation = {translation.dx, 0.0}; break;
                case PanAxis::Vertical: translation = {0.0, translation.dy}; break;
                case PanAxis::Aligned:
                    translation = *currentAxis_ == Axis::Horizontal
                                      ? Offset{translation.dx, 0.0}
                                      : Offset{0.0, translation.dy};
                    break;
                case PanAxis::Free: break;
            }
        }
        const Matrix4 next = matrix4Translated(matrix, translation);
        Rect boundary;
        if (!boundaryRect(&boundary))
            return next;  // 边界无限 ⇒ 不夹
        const ViewerQuad boundaryQuad = viewerQuadFromRect(boundary);
        const Offset offending = InteractiveViewer::exceedsBy(
            boundaryQuad, InteractiveViewer::transformViewport(next, viewportRect()));
        if (offending == Offset())
            return next;

        const Offset nextTotal = matrix4Translation(next);
        const double currentScale = matrix4MaxScaleOnAxis(matrix);
        const Offset corrected = {nextTotal.dx - offending.dx * currentScale,
                                  nextTotal.dy - offending.dy * currentScale};
        const Matrix4 correctedMatrix = matrix4WithTranslation(matrix, corrected);

        const Offset offending2 = InteractiveViewer::exceedsBy(
            boundaryQuad,
            InteractiveViewer::transformViewport(correctedMatrix, viewportRect()));
        if (offending2 == Offset())
            return correctedMatrix;
        // 两个方向都放不下 ⇒ 视口比边界还大，完全不许动。
        if (offending2.dx != 0.0 && offending2.dy != 0.0)
            return matrix;
        const Offset unidirectional = {offending2.dx == 0.0 ? corrected.dx : 0.0,
                                       offending2.dy == 0.0 ? corrected.dy : 0.0};
        return matrix4WithTranslation(matrix, unidirectional);
    }

    /// 上游的 `_matrixScale`：把总缩放夹进 [minScale, maxScale]，并且永远不
    /// 让子节点小到视口装不下（`max(viewport/boundary)` 那条下界）。
    Matrix4 matrixScale(const Matrix4& matrix, double scale) const {
        auto* w = widget_();
        if (scale == 1.0 || !w)
            return matrix;
        const double currentScale = matrix4MaxScaleOnAxis(controller_->value());
        double totalScale = std::max(currentScale * scale, 0.0);
        Rect boundary;
        if (boundaryRect(&boundary)) {
            const Rect view = viewportRect();
            if (boundary.width > 0.0 && boundary.height > 0.0)
                totalScale = std::max(totalScale, std::max(view.width / boundary.width,
                                                            view.height / boundary.height));
        }
        const double clamped = clampDoubleValue(totalScale, w->minScale(), w->maxScale());
        if (currentScale <= 0.0)
            return matrix4Scaled(matrix, clamped);
        return matrix4Scaled(matrix, clamped / currentScale);
    }

private:
    InteractiveViewer* widget_() const { return static_cast<InteractiveViewer*>(widget()); }

    struct TrackedPointer {
        Offset global;
        Offset local;
    };

    // ── 尺寸探测（上游用两个 GlobalKey，本仓沿渲染链下探）───────────────────

    RenderObject* listenerRenderObject() const {
        Element* e = element();
        return e ? e->findRenderObject() : nullptr;
    }

    /// 找到树里的那个 RenderTransform（它的 size == 子节点 size，代理盒）。
    static RenderTransform* findTransform(RenderObject* node) {
        if (!node)
            return nullptr;
        if (auto* t = dynamic_cast<RenderTransform*>(node))
            return t;
        RenderTransform* found = nullptr;
        node->visitChildren([&](AbstractNode* child) {
            if (found)
                return;
            found = findTransform(static_cast<RenderObject*>(child));
        });
        return found;
    }

    Rect viewportRect() const {
        RenderObject* ro = listenerRenderObject();
        auto* box = ro ? dynamic_cast<RenderBox*>(ro) : nullptr;
        if (!box)
            return Rect();
        const Size s = box->size();
        return Rect(0.0, 0.0, s.width, s.height);
    }

    /// 子盒子（未变换前）的尺寸。
    Size childSize() const {
        RenderTransform* transform = findTransform(listenerRenderObject());
        if (!transform || !transform->child())
            return Size();
        auto* box = dynamic_cast<RenderBox*>(transform->child());
        return box ? box->size() : Size();
    }

    /// 上游的 `_boundaryRect`。返回 false 表示边界无限（不做夹取）。
    bool boundaryRect(Rect* out) const {
        auto* w = widget_();
        Size child = childSize();
        if (child.width <= 0.0 || child.height <= 0.0)
            return false;
        const EdgeInsets m = w ? w->boundaryMargin() : EdgeInsets();
        const bool infinite = m.left >= BoxConstraints::kInfinity ||
                              m.top >= BoxConstraints::kInfinity ||
                              m.right >= BoxConstraints::kInfinity ||
                              m.bottom >= BoxConstraints::kInfinity;
        if (infinite)
            return false;
        *out = Rect(-m.left, -m.top, child.width + m.left + m.right,
                    child.height + m.top + m.bottom);
        return true;
    }

    // ── 手势表 ──────────────────────────────────────────────────────────────

    /// 两指时的跨度；单指恒 0（上游的 details.scale 在 pan 时也恒 1）。
    double span() const {
        if (pointers_.size() < 2)
            return 0.0;
        auto it = pointers_.begin();
        const Offset a = it->second.global;
        const Offset b = (++it)->second.global;
        return (b - a).distance();
    }

    void focalPoint(Offset* global, Offset* local) const {
        double gx = 0.0, gy = 0.0, lx = 0.0, ly = 0.0;
        for (const auto& kv : pointers_) {
            gx += kv.second.global.dx;
            gy += kv.second.global.dy;
            lx += kv.second.local.dx;
            ly += kv.second.local.dy;
        }
        const double n = double(std::max<size_t>(pointers_.size(), 1));
        *global = {gx / n, gy / n};
        *local = {lx / n, ly / n};
    }

    bool gestureIsSupported(GestureType type) const {
        auto* w = widget_();
        if (!w)
            return false;
        return type == GestureType::Scale ? w->scaleEnabled() : w->panEnabled();
    }

    /// 上游的 `_getGestureType`：本仓无 rotation，所以只看 scale 是否偏离 1。
    GestureType getGestureType(double scale) const {
        auto* w = widget_();
        const double effective = (w && !w->scaleEnabled()) ? 1.0 : scale;
        return effective == 1.0 ? GestureType::Pan : GestureType::Scale;
    }

    InteractiveViewerStartDetails startDetails() const {
        InteractiveViewerStartDetails d;
        focalPoint(&d.focalPoint, &d.localFocalPoint);
        return d;
    }

    void beginGesture() {
        auto* w = widget_();
        gestureType_ = pointers_.size() >= 2 ? GestureType::Scale : GestureType::Pan;
        currentAxis_.reset();
        if (!w || !controller_)
            return;
        const InteractiveViewerStartDetails d = startDetails();
        scaleStart_ = controller_->scale();
        referenceFocalPoint_ = controller_->toScene(d.localFocalPoint);
        startSpan_ = span();
        if (w->onInteractionStart())
            w->onInteractionStart()(d);
    }

    void endGesture() {
        auto* w = widget_();
        scaleStart_ = 1.0;
        startSpan_ = 0.0;
        referenceFocalPoint_ = Offset();
        currentAxis_.reset();
        gestureType_ = GestureType::None;
        if (w && w->onInteractionEnd())
            w->onInteractionEnd()(InteractiveViewerEndDetails());
    }

    void handleDown(const PointerEvent& e) {
        if (pointers_.count(e.pointerId))
            return;
        pointers_[e.pointerId] = {e.globalPosition, e.position};
        beginGesture();
    }

    void handleMove(const PointerEvent& e) {
        auto it = pointers_.find(e.pointerId);
        if (it == pointers_.end())
            return;  // 不是在本视图上按下的指针
        it->second = {e.globalPosition, e.position};
        updateGesture();
    }

    void handleUp(const PointerEvent& e) {
        auto it = pointers_.find(e.pointerId);
        if (it == pointers_.end())
            return;
        pointers_.erase(it);
        endGesture();
        // 还剩手指 ⇒ 以剩余手指为新的基准继续（上游 ScaleGestureRecognizer
        // 在指针减少时也是先 end 再重新 start）。
        if (!pointers_.empty())
            beginGesture();
    }

    void updateGesture() {
        auto* w = widget_();
        if (!w || !controller_)
            return;
        InteractiveViewerUpdateDetails details;
        focalPoint(&details.focalPoint, &details.localFocalPoint);
        details.scale = 1.0;
        const double currentScale = controller_->scale();
        if (pointers_.size() >= 2 && startSpan_ > 0.0)
            details.scale = span() / startSpan_;
        details.focalPointDelta = Offset();

        if (gestureType_ == GestureType::Pan)
            gestureType_ = getGestureType(details.scale);
        else if (gestureType_ == GestureType::None)
            gestureType_ = getGestureType(details.scale);

        if (!gestureIsSupported(gestureType_)) {
            if (w->onInteractionUpdate())
                w->onInteractionUpdate()(details);
            return;
        }

        const Offset focalPointScene = controller_->toScene(details.localFocalPoint);

        switch (gestureType_) {
            case GestureType::Scale: {
                const double desiredScale = scaleStart_ * details.scale;
                const double scaleChange = currentScale > 0.0 ? desiredScale / currentScale : 1.0;
                controller_->setValue(matrixScale(controller_->value(), scaleChange));
                // 缩放时让手指底下的场景点保持不动。
                const Offset scaled = controller_->toScene(details.localFocalPoint);
                controller_->setValue(matrixTranslate(controller_->value(),
                                                      scaled - referenceFocalPoint_));
                details.focalPointDelta = scaled - referenceFocalPoint_;
                const Offset check = controller_->toScene(details.localFocalPoint);
                if (InteractiveViewer::roundOffset(referenceFocalPoint_) !=
                    InteractiveViewer::roundOffset(check))
                    referenceFocalPoint_ = check;
                break;
            }
            case GestureType::Pan: {
                // 两指但 scaleEnabled=false 时 details.scale 仍可能≠1；上游选择
                // 直接丢掉这类手势。
                if (details.scale != 1.0)
                    break;
                // 上游是 `_currentAxis ??= …`：一段手势只在首次有真实位移时
                // 锁定主轴，之后不再被后续帧的抖动改判。
                if (!currentAxis_)
                    currentAxis_ = panAxisOf(referenceFocalPoint_, focalPointScene);
                const Offset translation = focalPointScene - referenceFocalPoint_;
                controller_->setValue(matrixTranslate(controller_->value(), translation));
                details.focalPointDelta = translation;
                referenceFocalPoint_ = controller_->toScene(details.localFocalPoint);
                break;
            }
            case GestureType::None:
                break;
        }
        if (w->onInteractionUpdate())
            w->onInteractionUpdate()(details);
    }

    /// 上游的 `_getPanAxis`：两点重合返回 nullopt。
    std::optional<Axis> panAxisOf(const Offset& a, const Offset& b) const {
        if (a == b)
            return std::nullopt;
        const double x = b.dx - a.dx, y = b.dy - a.dy;
        return std::abs(x) > std::abs(y) ? std::optional<Axis>(Axis::Horizontal)
                                         : std::optional<Axis>(Axis::Vertical);
    }

    /// 上游的 `_receivedPointerSignal`（滚轮 / 触控板）。
    void handleScroll(const PointerEvent& e) {
        auto* w = widget_();
        if (!w || !controller_)
            return;
        const Offset local = e.position;
        const Offset global = e.globalPosition;
        const bool trackpad = scrollActsAsPan(e.pointerKind);

        if (trackpad && !w->trackpadScrollCausesScale()) {
            // 触控板滚动 = 平移。
            const Offset scrollDelta = {e.scrollDeltaX, e.scrollDeltaY};
            InteractiveViewerUpdateDetails details;
            details.focalPoint = global - scrollDelta;
            details.localFocalPoint = local - scrollDelta;
            details.focalPointDelta = {-scrollDelta.dx, -scrollDelta.dy};
            if (w->onInteractionStart())
                w->onInteractionStart()(InteractiveViewerStartDetails{global, local});
            if (!gestureIsSupported(GestureType::Pan)) {
                if (w->onInteractionUpdate())
                    w->onInteractionUpdate()(details);
                if (w->onInteractionEnd())
                    w->onInteractionEnd()(InteractiveViewerEndDetails());
                return;
            }
            const Offset focalPointScene = controller_->toScene(local);
            const Offset newFocalPointScene = controller_->toScene(local - scrollDelta);
            controller_->setValue(matrixTranslate(controller_->value(),
                                                  newFocalPointScene - focalPointScene));
            if (w->onInteractionUpdate())
                w->onInteractionUpdate()(details);
            if (w->onInteractionEnd())
                w->onInteractionEnd()(InteractiveViewerEndDetails());
            return;
        }

        if (e.scrollDeltaY == 0.0)
            return;  // 上游同样忽略纯横向滚轮
        const double scaleChange = std::exp(-e.scrollDeltaY / w->scaleFactor());

        if (w->onInteractionStart())
            w->onInteractionStart()(InteractiveViewerStartDetails{global, local});
        InteractiveViewerUpdateDetails details;
        details.focalPoint = global;
        details.localFocalPoint = local;
        details.scale = scaleChange;
        if (!gestureIsSupported(GestureType::Scale)) {
            if (w->onInteractionUpdate())
                w->onInteractionUpdate()(details);
            if (w->onInteractionEnd())
                w->onInteractionEnd()(InteractiveViewerEndDetails());
            return;
        }
        const Offset focalPointScene = controller_->toScene(local);
        controller_->setValue(matrixScale(controller_->value(), scaleChange));
        const Offset scaled = controller_->toScene(local);
        controller_->setValue(matrixTranslate(controller_->value(), scaled - focalPointScene));
        if (w->onInteractionUpdate())
            w->onInteractionUpdate()(details);
        if (w->onInteractionEnd())
            w->onInteractionEnd()(InteractiveViewerEndDetails());
    }

    /// 上游的 `_handleTransformation`：controller 变 = 一次重建。
    void handleTransformation() {
        if (mounted())
            setState();
    }

    /// 上游在 didUpdateWidget 里换 controller；本仓宿主常原地改同一个 Widget 对象，
    /// 所以在 build 开头跟上（同 AnimatedSwitcher 那条口径）。
    void adoptController(InteractiveViewer* w) {
        const TransformationControllerPtr& wanted = w->transformationController();
        if (wanted) {
            if (wanted == controller_)
                return;
        } else if (controller_ && !controllerExternal_) {
            return;  // 外部一直没给，继续用自建的那条
        }
        if (controller_ && listenerHandle_ >= 0)
            controller_->removeListenerByHandle(listenerHandle_);
        controllerExternal_ = bool(wanted);
        controller_ = wanted ? wanted : std::make_shared<TransformationController>();
        listenerHandle_ = controller_->addListenerWithHandle([this] { handleTransformation(); });
    }

    /// 上游的 `SimpleViewer` 没有这条：本仓的 PointerDeviceKind 只有
    /// mouse/touch/stylus/unknown，触控板档不存在，所以「滚动 = 平移」挂在
    /// 非鼠标设备上；鼠标滚轮恒为缩放（与上游默认一致）。
    static bool scrollActsAsPan(PointerDeviceKind kind) {
        return kind != PointerDeviceKind::mouse;
    }

    std::shared_ptr<TransformationController> controller_;
    bool controllerExternal_ = false;
    int listenerHandle_ = -1;

    /// pointerId → 该指针当前的 global/local 位置（本视图上按下的那些）。
    std::map<int64_t, TrackedPointer> pointers_;
    GestureType gestureType_ = GestureType::None;
    /// 上游的 `_referenceFocalPoint`：本段手势开始时手指底下的**场景**点。
    Offset referenceFocalPoint_;
    double scaleStart_ = 1.0;
    double startSpan_ = 0.0;
    std::optional<Axis> currentAxis_;
};

inline std::unique_ptr<State> InteractiveViewer::createState() {
    return std::make_unique<InteractiveViewerState>();
}

}  // namespace cui_core
