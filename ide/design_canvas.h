#pragma once
/// @file ide/design_canvas.h
/// @brief 上游 流水线版设计画布（RenderBox + 交互），替代 legacy DesignCanvas。
///
/// 绘制逻辑（网格/节点/手柄/橡皮筋/吸附/拖放幽灵/对齐线）与 legacy
/// design_canvas.cpp 完全一致，仅把入口从控件的 paint 换成
/// RenderBox::performPaint(PaintingContext&, Offset)，并把 Painter 的
/// 坐标原点平移到盒左上角。指针交互从 onMouse* 换成 RenderObject::handleEvent
/// （ev.position 已是盒本地坐标）。

#include <memory>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/painter.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_box.h"
#include "ide_controller.h"

namespace cui {
namespace designer {

struct DesignNode;  // design_model.h（头文件只用到指针，无需完整定义）

// ── 缩放手柄位置 ──────────────────────────────────────────────────────────
enum class HandlePos {
    TopLeft, Top, TopRight,
    Left,                Right,
    BottomLeft, Bottom, BottomRight,
    None,
};

// ── 拖拽模式 ──────────────────────────────────────────────────────────────
enum class DragMode {
    None,
    Moving,
    Resizing,
    RubberBand,
    Dropping,
};

// ── RenderDesignCanvas ──────────────────────────────────────────────────
class RenderDesignCanvas : public RenderBox {
public:
    void setController(SharedPtr<IdeController> c) {
        controller_ = std::move(c);
        controller_->setCanvas(this);
    }
    SharedPtr<IdeController> controller() const { return controller_; }

    // ── RenderObject 重写 ───────────────────────────────────────────────
    void performLayout() override { setSize(constraints().biggest()); }
    void performPaint(PaintingContext& context, const Offset& offset) override;
    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 &&
               position.dx < size_.width && position.dy < size_.height;
    }
    void handleEvent(const PointerEvent& event, const HitTestEntry& entry) override;

    // ── 供 controller 使用的坐标换算 ─────────────────────────────────────
    /// 画布盒本地坐标 → 设计空间坐标（减去滚动偏移）。
    Offset toDesign(Offset boxLocal) const {
        return Offset{boxLocal.dx - canvasOffX_, boxLocal.dy - canvasOffY_};
    }
    double canvasOffX() const { return canvasOffX_; }
    double canvasOffY() const { return canvasOffY_; }

    // ── 供 controller 完成工具拖放 ───────────────────────────────────────
    void completeDrop(const std::string& type, Offset designPos);

    void markDirty() { markNeedsPaint(); }

private:
    SharedPtr<IdeController> controller_;
    DesignModel* model() const {
        return controller_ ? controller_->model().get() : nullptr;
    }

    // ── 拖拽状态 ────────────────────────────────────────────────────────
    DragMode dragMode_ = DragMode::None;
    HandlePos resizeHandle_ = HandlePos::None;
    Offset dragStart_{};          // 按下时盒本地坐标
    Offset dragCurrent_{};        // 当前盒本地坐标
    double dragOrigX_ = 0, dragOrigY_ = 0;
    double dragOrigW_ = 0, dragOrigH_ = 0;
    Offset rubberOrigin_{};       // 橡皮筋起点（设计空间）
    Offset dropPos_{};            // 拖放幽灵位置（盒本地）

    double canvasOffX_ = 0;
    double canvasOffY_ = 0;

    // 吸附线。
    struct SnapLine {
        enum Orientation { Horizontal, Vertical };
        Orientation orientation;
        double position;
        double start;
        double end;
    };
    mutable std::vector<SnapLine> snapLines_;

    // ── 几何 / 命中 ──────────────────────────────────────────────────────
    struct NodeRect { double x, y, w, h; };
    NodeRect nodeRect(const DesignNode* node) const;
    DesignNode* hitTestNodes(double lx, double ly) const;
    HandlePos hitTestHandle(double lx, double ly) const;

    // ── 绘制 ────────────────────────────────────────────────────────────
    void paintGrid(Painter& p, const Size& s);
    void paintNode(Painter& p, const DesignNode* node, int depth);
    void paintNodeAt(Painter& p, const DesignNode* node, int depth,
                     const NodeRect& overrideRect);
    void paintSelectionHandles(Painter& p, const NodeRect& r);
    void paintRubberBand(Painter& p);
    void paintDropGhost(Painter& p);
    void paintGuides(Painter& p);
    NodeRect computeSnapRect(const NodeRect& r) const;

    // 布局容器判定（与 legacy 一致）。
    static bool isLayoutContainer(const DesignNode* node);
    static bool isVerticalLayout(const DesignNode* node);

    static constexpr double kHandleSize = 7.0;
    static constexpr double kSnapThreshold = 6.0;
    static constexpr double kGridStep = 8.0;
};

// ── DesignCanvas（Widget）──────────────────────────────────────────
class DesignCanvas : public LeafRenderObjectWidget {
public:
    explicit DesignCanvas(SharedPtr<IdeController> controller,
                                 KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)),
          controller_(std::move(controller)) {}

    RenderObject* createRenderObject() override {
        auto* r = new RenderDesignCanvas();
        r->setController(controller_);
        return r;
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderDesignCanvas*>(ro);
        if (r) r->setController(controller_);
    }

private:
    SharedPtr<IdeController> controller_;
};

}  // namespace designer
}  // namespace cui
