/// @file ide/design_canvas.cpp
/// @brief 上游 流水线版设计画布实现（绘制 + 交互）。

#include "design_canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "cui/foundation/painter.h"
#include "cui/rendering/painting_context.h"

#include "design_model.h"

namespace cui {
namespace designer {

// ── 绘制 ───────────────────────────────────────────────────────────────────

void RenderDesignCanvas::performPaint(PaintingContext& context,
                                      const Offset& offset) {
    SkCanvas* canvas = context.canvas();
    Painter p(canvas, "DejaVu Sans");
    // 把绘制原点平移到盒左上角（之后全部用盒本地坐标）。
    p.translate(offset.dx, offset.dy);

    const Size s{size_.width, size_.height};
    // 预览模式：只画控件本身，不画网格 / 手柄 / 橡皮筋 / 对齐线。
    const bool preview = controller_ && controller_->previewMode();

    // 1. 背景。
    if (preview) {
        p.setSourceColor(Color::fromRGB(1, 1, 1));
        p.setFillStyle();
        p.rectangle(Rect{0, 0, s.width, s.height});
        p.fill();
    } else {
        paintGrid(p, s);
    }

    // 2. 平移滚动偏移。
    p.save();
    p.translate(canvasOffX_, canvasOffY_);

    // 3. 绘制整棵树。
    if (model() && model()->root())
        paintNode(p, model()->root(), 0);

    if (!preview) {
        // 4. 选中手柄。
        if (model()) {
            for (auto* sel : model()->multiSelection()) {
                auto r = nodeRect(sel);
                paintSelectionHandles(p, r);
            }
        }

        // 5. 橡皮筋。
        if (dragMode_ == DragMode::RubberBand)
            paintRubberBand(p);

        // 6. 拖放幽灵。
        if (dragMode_ == DragMode::Dropping || (controller_ && controller_->toolDragging()))
            paintDropGhost(p);

        // 7. 对齐线。
        paintGuides(p);
    }

    p.restore();
}

void RenderDesignCanvas::paintGrid(Painter& p, const Size& s) {
    p.setSourceColor(Color::fromRGB(0.96, 0.96, 0.97));
    p.setFillStyle();
    p.rectangle(Rect{0, 0, s.width, s.height});
    p.fill();

    p.setSourceColor(Color::fromRGBA(0, 0, 0, 0.08));
    p.setFillStyle();
    const double step = kGridStep;
    double ox = std::fmod(canvasOffX_, step);
    double oy = std::fmod(canvasOffY_, step);
    for (double gx = ox; gx < s.width; gx += step)
        for (double gy = oy; gy < s.height; gy += step)
            p.rectangle(Rect{gx, gy, 1, 1});
    p.fill();
}

RenderDesignCanvas::NodeRect RenderDesignCanvas::nodeRect(const DesignNode* node) const {
    double x = node->propInt("x", 0);
    double y = node->propInt("y", 0);
    double w = node->propInt("width", 80);
    double h = node->propInt("height", 30);
    return {x, y, w, h};
}

bool RenderDesignCanvas::isLayoutContainer(const DesignNode* node) {
    return node->widgetType == "Column" ||
           node->widgetType == "Row" ||
           node->widgetType == "VBoxContainer" ||
           node->widgetType == "HBoxContainer";
}

bool RenderDesignCanvas::isVerticalLayout(const DesignNode* node) {
    return node->widgetType == "Column" ||
           node->widgetType == "VBoxContainer";
}

void RenderDesignCanvas::paintNode(Painter& p, const DesignNode* node, int depth) {
    if (!node->propBool("visible", true)) return;

    auto r = nodeRect(node);
    bool isContainer = node->isContainer();
    bool isSelected = model()->isSelected(const_cast<DesignNode*>(node));
    bool isLayout = isLayoutContainer(node);
    (void)depth;

    if (isContainer) {
        p.setSourceColor(Color::fromRGBA(0.85, 0.90, 0.98, 0.30));
        p.setFillStyle();
        p.rectangle(Rect{r.x, r.y, r.w, r.h});
        p.fill();

        if (isSelected) {
            p.setSourceRGB(0.10, 0.45, 0.90);
            p.setLineWidth(2.0);
        } else {
            p.setSourceColor(Color::fromRGBA(0.50, 0.60, 0.80, 0.40));
            p.setLineWidth(1.0);
        }
        p.rectangle(Rect{r.x, r.y, r.w, r.h});
        p.stroke();

        p.setSourceColor(Color::fromRGBA(0.25, 0.50, 0.90, 0.90));
        p.setFillStyle();
        auto typeExt = p.textExtents(node->widgetType.c_str());
        double labelW = typeExt.width + 12;
        double labelH = 18;
        p.roundedRectangle(Rect{r.x, r.y, labelW, labelH}, 3);
        p.fill();

        p.setSourceRGB(1, 1, 1);
        p.setFontSize(10.0);
        p.save();
        p.translate(r.x + 6, r.y + 13);
        p.showText(node->widgetType.c_str());
        p.restore();

        if (isLayout && !node->children.empty()) {
            bool vertical = isVerticalLayout(node);
            p.setSourceColor(Color::fromRGBA(0.25, 0.50, 0.90, 0.50));
            p.setLineWidth(1.5);
            double arrowX = r.x + r.w - 14;
            double arrowY = r.y + 9;
            if (vertical) {
                p.moveTo(arrowX, arrowY);
                p.lineTo(arrowX + 4, arrowY + 5);
                p.lineTo(arrowX + 8, arrowY);
            } else {
                p.moveTo(arrowX, arrowY);
                p.lineTo(arrowX + 5, arrowY + 4);
                p.lineTo(arrowX, arrowY + 8);
            }
            p.stroke();
        }
    } else {
        p.setSourceColor(Color::fromRGBA(1, 1, 1, 0.95));
        p.setFillStyle();
        p.roundedRectangle(Rect{r.x, r.y, r.w, r.h}, 4);
        p.fill();

        if (isSelected) {
            p.setSourceRGB(0.10, 0.45, 0.90);
            p.setLineWidth(2.0);
        } else {
            p.setSourceColor(Color::fromRGBA(0.70, 0.70, 0.75, 0.50));
            p.setLineWidth(1.0);
        }
        p.roundedRectangle(Rect{r.x, r.y, r.w, r.h}, 4);
        p.stroke();

        p.setSourceRGB(0.25, 0.50, 0.90);
        p.setFontSize(9.0);
        p.save();
        p.translate(r.x + 5, r.y + 12);
        p.showText(node->widgetType.c_str());
        p.restore();

        if (r.h > 24 && r.w > 40) {
            p.setSourceRGB(0.20, 0.20, 0.25);
            p.setFontSize(11.0);
            auto ext = p.textExtents(node->displayName.c_str());
            double tx = r.x + (r.w - ext.width) / 2;
            double ty = r.y + (r.h + ext.height * 0.35) / 2;
            p.save();
            p.translate(tx, ty);
            p.showText(node->displayName.c_str());
            p.restore();
        }
    }

    if (isLayout) {
        double spacing = node->propDouble("spacing", 8);
        double padding = node->propDouble("padding", 4);
        bool vertical = isVerticalLayout(node);

        double childX = r.x + padding;
        double childY = r.y + padding + 16;
        double availW = r.w - padding * 2;
        double availH = r.h - padding * 2 - 16;

        for (auto& child : node->children) {
            if (!child->propBool("visible", true)) continue;
            double cw = child->propInt("width", 80);
            double ch = child->propInt("height", 30);

            NodeRect childRect;
            if (vertical) {
                childRect = {childX, childY, availW, ch};
                childY += ch + spacing;
            } else {
                childRect = {childX, childY, cw, availH};
                childX += cw + spacing;
            }
            paintNodeAt(p, child.get(), depth + 1, childRect);
        }
    } else {
        for (auto& child : node->children)
            paintNode(p, child.get(), depth + 1);
    }
}

void RenderDesignCanvas::paintNodeAt(Painter& p, const DesignNode* node, int depth,
                                     const NodeRect& overrideRect) {
    if (!node->propBool("visible", true)) return;

    NodeRect r = overrideRect;
    bool isContainer = node->isContainer();
    bool isSelected = model()->isSelected(const_cast<DesignNode*>(node));
    (void)depth;

    if (isContainer) {
        p.setSourceColor(Color::fromRGBA(0.85, 0.90, 0.98, 0.30));
        p.setFillStyle();
        p.rectangle(Rect{r.x, r.y, r.w, r.h});
        p.fill();

        if (isSelected) {
            p.setSourceRGB(0.10, 0.45, 0.90);
            p.setLineWidth(2.0);
        } else {
            p.setSourceColor(Color::fromRGBA(0.50, 0.60, 0.80, 0.40));
            p.setLineWidth(1.0);
        }
        p.rectangle(Rect{r.x, r.y, r.w, r.h});
        p.stroke();

        p.setSourceColor(Color::fromRGBA(0.25, 0.50, 0.90, 0.90));
        p.setFillStyle();
        auto typeExt = p.textExtents(node->widgetType.c_str());
        double labelW = typeExt.width + 12;
        p.roundedRectangle(Rect{r.x, r.y, labelW, 18}, 3);
        p.fill();

        p.setSourceRGB(1, 1, 1);
        p.setFontSize(10.0);
        p.save();
        p.translate(r.x + 6, r.y + 13);
        p.showText(node->widgetType.c_str());
        p.restore();
    } else {
        p.setSourceColor(Color::fromRGBA(1, 1, 1, 0.95));
        p.setFillStyle();
        p.roundedRectangle(Rect{r.x, r.y, r.w, r.h}, 4);
        p.fill();

        if (isSelected) {
            p.setSourceRGB(0.10, 0.45, 0.90);
            p.setLineWidth(2.0);
        } else {
            p.setSourceColor(Color::fromRGBA(0.70, 0.70, 0.75, 0.50));
            p.setLineWidth(1.0);
        }
        p.roundedRectangle(Rect{r.x, r.y, r.w, r.h}, 4);
        p.stroke();

        p.setSourceRGB(0.25, 0.50, 0.90);
        p.setFontSize(9.0);
        p.save();
        p.translate(r.x + 5, r.y + 12);
        p.showText(node->widgetType.c_str());
        p.restore();

        if (r.h > 24 && r.w > 40) {
            p.setSourceRGB(0.20, 0.20, 0.25);
            p.setFontSize(11.0);
            auto ext = p.textExtents(node->displayName.c_str());
            double tx = r.x + (r.w - ext.width) / 2;
            double ty = r.y + (r.h + ext.height * 0.35) / 2;
            p.save();
            p.translate(tx, ty);
            p.showText(node->displayName.c_str());
            p.restore();
        }
    }

    for (auto& child : node->children)
        paintNode(p, child.get(), depth + 1);
}

void RenderDesignCanvas::paintSelectionHandles(Painter& p, const NodeRect& r) {
    p.setSourceRGB(0.10, 0.45, 0.90);
    p.setLineWidth(1.5);
    p.rectangle(Rect{r.x - 1, r.y - 1, r.w + 2, r.h + 2});
    p.stroke();

    const double hs = kHandleSize;
    const double hh = hs / 2.0;

    auto drawHandle = [&](double cx, double cy) {
        p.setSourceRGB(1.0, 1.0, 1.0);
        p.setFillStyle();
        p.rectangle(Rect{cx - hh, cy - hh, hs, hs});
        p.fill();
        p.setSourceRGB(0.10, 0.45, 0.90);
        p.setLineWidth(1.0);
        p.rectangle(Rect{cx - hh, cy - hh, hs, hs});
        p.stroke();
    };

    drawHandle(r.x, r.y);
    drawHandle(r.x + r.w, r.y);
    drawHandle(r.x, r.y + r.h);
    drawHandle(r.x + r.w, r.y + r.h);
    drawHandle(r.x + r.w / 2, r.y);
    drawHandle(r.x + r.w / 2, r.y + r.h);
    drawHandle(r.x, r.y + r.h / 2);
    drawHandle(r.x + r.w, r.y + r.h / 2);

    p.setSourceColor(Color::fromRGBA(0.15, 0.15, 0.18, 0.90));
    p.setFillStyle();
    char dimBuf[32];
    std::snprintf(dimBuf, sizeof(dimBuf), "%d × %d",
                  static_cast<int>(r.w), static_cast<int>(r.h));
    p.setFontSize(9.0);
    auto ext = p.textExtents(dimBuf);
    double tipX = r.x + r.w + 8;
    double tipY = r.y + r.h + 4;
    p.roundedRectangle(Rect{tipX - 2, tipY - 10, ext.width + 8, 14}, 3);
    p.fill();
    p.setSourceRGB(0.85, 0.85, 0.90);
    p.save();
    p.translate(tipX + 2, tipY);
    p.showText(dimBuf);
    p.restore();
}

void RenderDesignCanvas::paintRubberBand(Painter& p) {
    double x1 = std::min(rubberOrigin_.dx, dropPos_.dx);
    double y1 = std::min(rubberOrigin_.dy, dropPos_.dy);
    double x2 = std::max(rubberOrigin_.dx, dropPos_.dx);
    double y2 = std::max(rubberOrigin_.dy, dropPos_.dy);

    p.setSourceColor(Color::fromRGBA(0.20, 0.55, 1.0, 0.12));
    p.setFillStyle();
    p.rectangle(Rect{x1, y1, x2 - x1, y2 - y1});
    p.fill();

    p.setSourceRGB(0.20, 0.55, 1.0);
    p.setLineWidth(1.0);
    p.rectangle(Rect{x1, y1, x2 - x1, y2 - y1});
    p.stroke();
}

void RenderDesignCanvas::paintDropGhost(Painter& p) {
    std::string type = controller_ ? controller_->dragType() : "";
    if (type.empty()) return;

    double w = 80, h = 30;
    if (auto* info = findWidgetType(type)) {
        for (auto& pm : info->props) {
            if (pm.name == "width")  w = std::stod(pm.defaultVal);
            if (pm.name == "height") h = std::stod(pm.defaultVal);
        }
    }

    // dropPos_ 是盒本地坐标，设计空间需减去滚动偏移。
    double gx = (dropPos_.dx - canvasOffX_) - w / 2;
    double gy = (dropPos_.dy - canvasOffY_) - h / 2;

    p.setSourceColor(Color::fromRGBA(0.20, 0.55, 1.0, 0.15));
    p.setFillStyle();
    p.roundedRectangle(Rect{gx, gy, w, h}, 3);
    p.fill();

    p.setSourceRGB(0.20, 0.55, 1.0);
    p.setLineWidth(1.5);
    p.roundedRectangle(Rect{gx, gy, w, h}, 3);
    p.stroke();

    p.setSourceColor(Color::fromRGBA(0.20, 0.55, 1.0, 0.40));
    p.setLineWidth(0.5);
    p.moveTo(gx - 20, gy + h / 2);
    p.lineTo(gx + w + 20, gy + h / 2);
    p.stroke();
    p.moveTo(gx + w / 2, gy - 20);
    p.lineTo(gx + w / 2, gy + h + 20);
    p.stroke();

    p.setSourceColor(Color::fromRGBA(0.15, 0.15, 0.18, 0.90));
    p.setFillStyle();
    p.setFontSize(10.0);
    auto ext = p.textExtents(type.c_str());
    double lx = gx + w / 2 - ext.width / 2 - 4;
    double ly = gy - 18;
    p.roundedRectangle(Rect{lx, ly, ext.width + 8, 15}, 3);
    p.fill();
    p.setSourceRGB(0.85, 0.85, 0.90);
    p.save();
    p.translate(lx + 4, ly + 11);
    p.showText(type.c_str());
    p.restore();
}

RenderDesignCanvas::NodeRect RenderDesignCanvas::computeSnapRect(const NodeRect& r) const {
    snapLines_.clear();
    if (!model()) return r;

    NodeRect result = r;
    double bestDx = kSnapThreshold + 1;
    double bestDy = kSnapThreshold + 1;

    auto allNodes = const_cast<DesignModel*>(model())->flatten();
    std::vector<double> vertLines;
    std::vector<double> horizLines;

    for (auto* node : allNodes) {
        if (node == model()->root()) continue;
        if (model()->isSelected(node)) continue;
        auto nr = nodeRect(node);
        vertLines.push_back(nr.x);
        vertLines.push_back(nr.x + nr.w);
        vertLines.push_back(nr.x + nr.w / 2);
        horizLines.push_back(nr.y);
        horizLines.push_back(nr.y + nr.h);
        horizLines.push_back(nr.y + nr.h / 2);
    }

    double ourLeft = r.x, ourRight = r.x + r.w, ourCX = r.x + r.w / 2;
    double ourTop = r.y, ourBottom = r.y + r.h, ourCY = r.y + r.h / 2;

    for (double vx : vertLines) {
        double d1 = std::abs(ourLeft - vx);
        double d2 = std::abs(ourRight - vx);
        double d3 = std::abs(ourCX - vx);
        double minD = std::min({d1, d2, d3});
        if (minD < bestDx) {
            bestDx = minD;
            if (d1 == minD) { result.x = vx; snapLines_.push_back({SnapLine::Vertical, vx, 0, 0}); }
            else if (d2 == minD) { result.x = vx - r.w; snapLines_.push_back({SnapLine::Vertical, vx, 0, 0}); }
            else { result.x = vx - r.w / 2; snapLines_.push_back({SnapLine::Vertical, vx, 0, 0}); }
        }
    }

    for (double hy : horizLines) {
        double d1 = std::abs(ourTop - hy);
        double d2 = std::abs(ourBottom - hy);
        double d3 = std::abs(ourCY - hy);
        double minD = std::min({d1, d2, d3});
        if (minD < bestDy) {
            bestDy = minD;
            if (d1 == minD) { result.y = hy; snapLines_.push_back({SnapLine::Horizontal, hy, 0, 0}); }
            else if (d2 == minD) { result.y = hy - r.h; snapLines_.push_back({SnapLine::Horizontal, hy, 0, 0}); }
            else { result.y = hy - r.h / 2; snapLines_.push_back({SnapLine::Horizontal, hy, 0, 0}); }
        }
    }

    for (auto& sl : snapLines_) {
        if (sl.orientation == SnapLine::Vertical) {
            sl.start = std::min(result.y, 0.0);
            sl.end = std::max(result.y + r.h, 1000.0);
        } else {
            sl.start = std::min(result.x, 0.0);
            sl.end = std::max(result.x + r.w, 1000.0);
        }
    }

    return result;
}

void RenderDesignCanvas::paintGuides(Painter& p) {
    for (auto& sl : snapLines_) {
        p.setSourceRGB(0.90, 0.10, 0.90);
        p.setLineWidth(1.0);
        if (sl.orientation == SnapLine::Vertical) {
            p.moveTo(sl.position, sl.start);
            p.lineTo(sl.position, sl.end);
        } else {
            p.moveTo(sl.start, sl.position);
            p.lineTo(sl.end, sl.position);
        }
        p.stroke();

        p.setFillStyle();
        if (sl.orientation == SnapLine::Vertical) {
            p.circle(sl.position, sl.start + 4, 2);
            p.fill();
        } else {
            p.circle(sl.start + 4, sl.position, 2);
            p.fill();
        }
    }
}

// ── 命中测试 ────────────────────────────────────────────────────────────────

DesignNode* RenderDesignCanvas::hitTestNodes(double lx, double ly) const {
    if (!model()) return nullptr;

    std::function<DesignNode*(const DesignNode*)> hitImpl;
    hitImpl = [&](const DesignNode* node) -> DesignNode* {
        auto r = nodeRect(node);
        if (lx >= r.x && lx <= r.x + r.w && ly >= r.y && ly <= r.y + r.h) {
            for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
                if (auto* h = hitImpl(it->get())) return h;
            return const_cast<DesignNode*>(node);
        }
        return nullptr;
    };

    return hitImpl(model()->root());
}

HandlePos RenderDesignCanvas::hitTestHandle(double lx, double ly) const {
    if (!model() || !model()->selected()) return HandlePos::None;

    auto r = nodeRect(model()->selected());
    const double hh = kHandleSize / 2.0 + 2.0;

    auto near = [&](double a, double b) { return std::abs(a - b) <= hh; };

    if (near(lx, r.x) && near(ly, r.y))           return HandlePos::TopLeft;
    if (near(lx, r.x + r.w) && near(ly, r.y))     return HandlePos::TopRight;
    if (near(lx, r.x) && near(ly, r.y + r.h))     return HandlePos::BottomLeft;
    if (near(lx, r.x + r.w) && near(ly, r.y + r.h)) return HandlePos::BottomRight;
    if (near(lx, r.x + r.w / 2) && near(ly, r.y))       return HandlePos::Top;
    if (near(lx, r.x + r.w / 2) && near(ly, r.y + r.h)) return HandlePos::Bottom;
    if (near(lx, r.x) && near(ly, r.y + r.h / 2))       return HandlePos::Left;
    if (near(lx, r.x + r.w) && near(ly, r.y + r.h / 2)) return HandlePos::Right;

    return HandlePos::None;
}

// ── 指针事件 ───────────────────────────────────────────────────────────────

void RenderDesignCanvas::handleEvent(const PointerEvent& event,
                                      const HitTestEntry& /*entry*/) {
    if (!model()) return;
    if (controller_ && controller_->toolDragging()) return;  // 遮罩接管
    if (controller_ && controller_->previewMode()) return;   // 预览模式不参与编辑

    double lx = event.position.dx - canvasOffX_;
    double ly = event.position.dy - canvasOffY_;

    if (event.type == PointerEventType::Down) {
        dragStart_ = event.position;

        auto handle = hitTestHandle(lx, ly);
        if (handle != HandlePos::None) {
            dragMode_ = DragMode::Resizing;
            resizeHandle_ = handle;
            auto r = nodeRect(model()->selected());
            dragOrigX_ = r.x; dragOrigY_ = r.y;
            dragOrigW_ = r.w; dragOrigH_ = r.h;
            return;
        }

        DesignNode* hit = hitTestNodes(lx, ly);
        if (hit && hit != model()->root()) {
            model()->select(hit);
            if (controller_) controller_->syncSelectionChanged();
            dragMode_ = DragMode::Moving;
            auto r = nodeRect(hit);
            dragOrigX_ = r.x; dragOrigY_ = r.y;
            dragOrigW_ = r.w; dragOrigH_ = r.h;
            return;
        }

        model()->clearSelection();
        if (controller_) controller_->syncSelectionChanged();
        dragMode_ = DragMode::RubberBand;
        rubberOrigin_ = {lx, ly};
        dropPos_ = {lx, ly};
    } else if (event.type == PointerEventType::Move) {
        switch (dragMode_) {
        case DragMode::Moving: {
            if (!model()->selected()) break;
            double dx = event.position.dx - dragStart_.dx;
            double dy = event.position.dy - dragStart_.dy;
            int newX = static_cast<int>(std::round(dragOrigX_ + dx));
            int newY = static_cast<int>(std::round(dragOrigY_ + dy));

            NodeRect testRect{static_cast<double>(newX), static_cast<double>(newY),
                              dragOrigW_, dragOrigH_};
            auto snapped = computeSnapRect(testRect);
            newX = static_cast<int>(std::round(snapped.x));
            newY = static_cast<int>(std::round(snapped.y));

            // 直接改属性（不通知），仅重绘画布，避免每帧重建整树。
            auto* sel = model()->selected();
            sel->properties["x"] = std::to_string(newX);
            sel->properties["y"] = std::to_string(newY);
            markNeedsPaint();
            break;
        }
        case DragMode::Resizing: {
            if (!model()->selected()) break;
            double dx = event.position.dx - dragStart_.dx;
            double dy = event.position.dy - dragStart_.dy;
            double nx = dragOrigX_, ny = dragOrigY_;
            double nw = dragOrigW_, nh = dragOrigH_;

            switch (resizeHandle_) {
            case HandlePos::Right:  nw = std::max(20.0, dragOrigW_ + dx); break;
            case HandlePos::Bottom: nh = std::max(16.0, dragOrigH_ + dy); break;
            case HandlePos::Left:   nx = dragOrigX_ + dx; nw = std::max(20.0, dragOrigW_ - dx); break;
            case HandlePos::Top:    ny = dragOrigY_ + dy; nh = std::max(16.0, dragOrigH_ - dy); break;
            case HandlePos::TopLeft:
                nx = dragOrigX_ + dx; ny = dragOrigY_ + dy;
                nw = std::max(20.0, dragOrigW_ - dx);
                nh = std::max(16.0, dragOrigH_ - dy); break;
            case HandlePos::TopRight:
                ny = dragOrigY_ + dy;
                nw = std::max(20.0, dragOrigW_ + dx);
                nh = std::max(16.0, dragOrigH_ - dy); break;
            case HandlePos::BottomLeft:
                nx = dragOrigX_ + dx;
                nw = std::max(20.0, dragOrigW_ - dx);
                nh = std::max(16.0, dragOrigH_ + dy); break;
            case HandlePos::BottomRight:
                nw = std::max(20.0, dragOrigW_ + dx);
                nh = std::max(16.0, dragOrigH_ + dy); break;
            default: break;
            }
            auto* sel = model()->selected();
            sel->properties["x"] = std::to_string(static_cast<int>(nx));
            sel->properties["y"] = std::to_string(static_cast<int>(ny));
            sel->properties["width"] = std::to_string(static_cast<int>(nw));
            sel->properties["height"] = std::to_string(static_cast<int>(nh));
            markNeedsPaint();
            break;
        }
        case DragMode::RubberBand:
            dropPos_ = {lx, ly};
            markNeedsPaint();
            break;
        default:
            break;
        }
    } else if (event.type == PointerEventType::Up) {
        if (dragMode_ == DragMode::RubberBand) {
            double rx1 = std::min(rubberOrigin_.dx, dropPos_.dx);
            double ry1 = std::min(rubberOrigin_.dy, dropPos_.dy);
            double rx2 = std::max(rubberOrigin_.dx, dropPos_.dx);
            double ry2 = std::max(rubberOrigin_.dy, dropPos_.dy);
            model()->clearSelection();
            for (auto* node : model()->flatten()) {
                if (node == model()->root()) continue;
                auto r = nodeRect(node);
                double cx = r.x + r.w / 2;
                double cy = r.y + r.h / 2;
                if (cx >= rx1 && cx <= rx2 && cy >= ry1 && cy <= ry2)
                    model()->addToSelection(node);
            }
            model()->notifyChanged();  // 刷新属性面板
            if (controller_) controller_->syncSelectionChanged();
        }
        dragMode_ = DragMode::None;
        resizeHandle_ = HandlePos::None;
        snapLines_.clear();
        markNeedsPaint();
    }
}

void RenderDesignCanvas::completeDrop(const std::string& type, Offset designPos) {
    if (type.empty() || !model()) return;

    double lx = designPos.dx;
    double ly = designPos.dy;

    DesignNode* target = hitTestNodes(lx, ly);
    while (target && !target->isContainer())
        target = target->parent;
    if (!target) target = model()->root();

    auto* newNode = model()->addChild(target, type);

    bool isLayoutContainer = (target->widgetType == "Column" ||
                               target->widgetType == "Row" ||
                               target->widgetType == "VBoxContainer" ||
                               target->widgetType == "HBoxContainer");

    if (!isLayoutContainer) {
        auto tr = nodeRect(target);
        int relX = static_cast<int>(lx - tr.x);
        int relY = static_cast<int>(ly - tr.y);
        newNode->properties["x"] = std::to_string(std::max(0, relX));
        newNode->properties["y"] = std::to_string(std::max(0, relY));
    }

    model()->select(newNode);
    if (controller_) controller_->syncSelectionChanged();
}

}  // namespace designer
}  // namespace cui_core
