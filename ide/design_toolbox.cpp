/// @file ide/design_toolbox.cpp
/// @brief 上游 流水线版控件面板实现（绘制 + 拖源）。

#include "design_toolbox.h"

#include <cmath>
#include <map>

#include "cui/foundation/painter.h"
#include "cui/rendering/painting_context.h"

#include "design_model.h"

namespace cui {
namespace designer {

static Color accentForCategory(const std::string& cat) {
    if (cat == "Basic")      return Color::fromRGB(0.29, 0.56, 0.89);
    if (cat == "Container")  return Color::fromRGB(0.30, 0.69, 0.31);
    if (cat == "Data")       return Color::fromRGB(0.91, 0.49, 0.13);
    if (cat == "Decoration") return Color::fromRGB(0.67, 0.28, 0.67);
    if (cat == "Layout")     return Color::fromRGB(0.55, 0.55, 0.55);
    return Color::fromRGB(0.50, 0.50, 0.50);
}

void RenderDesignToolbox::drawIcon(Painter& p, const std::string& type,
                                   double x, double y, double s, const Color& accent) {
    p.save();
    p.translate(x, y);

    const double h = s;
    const double w = s;
    const double cx = w / 2;
    const double cy = h / 2;

    p.setSourceColor(accent);
    p.setFillStyle();
    p.setLineWidth(1.2);

    if (type == "Button") {
        p.roundedRectangle(Rect{1, 3, w - 2, h - 6}, 3); p.fill();
    } else if (type == "Text") {
        p.setLineWidth(1.5);
        p.moveTo(3, h - 2); p.lineTo(cx, 2); p.lineTo(w - 3, h - 2);
        p.moveTo(5, h - 6); p.lineTo(w - 5, h - 6); p.stroke();
    } else if (type == "TextField") {
        p.roundedRectangle(Rect{1, 4, w - 2, h - 8}, 2); p.stroke();
        p.moveTo(5, 6); p.lineTo(5, h - 6); p.stroke();
    } else if (type == "Checkbox") {
        p.roundedRectangle(Rect{2, 3, h - 6, h - 6}, 1.5); p.stroke();
        p.setLineWidth(1.8);
        p.moveTo(5, cy); p.lineTo(cx, h - 5); p.lineTo(w - 3, 5); p.stroke();
    } else if (type == "Radio") {
        p.circle(cx, cy, cy - 2); p.stroke();
        p.circle(cx, cy, 2); p.fill();
    } else if (type == "Switch") {
        p.roundedRectangle(Rect{1, cy - 4, w - 2, 8}, 4); p.stroke();
        p.circle(w - 6, cy, 3); p.fill();
    } else if (type == "Slider") {
        p.setLineWidth(1.5);
        p.moveTo(2, cy); p.lineTo(w - 2, cy); p.stroke();
        p.circle(cx + 2, cy, 3); p.fill();
    } else if (type == "SpinBox") {
        p.roundedRectangle(Rect{1, 4, w - 6, h - 8}, 2); p.stroke();
        p.moveTo(w - 4, 5); p.lineTo(w - 2, 3); p.lineTo(w - 0.5, 5); p.stroke();
        p.moveTo(w - 4, h - 5); p.lineTo(w - 2, h - 3); p.lineTo(w - 0.5, h - 5); p.stroke();
    } else if (type == "DropdownButton") {
        p.roundedRectangle(Rect{1, 4, w - 2, h - 8}, 2); p.stroke();
        p.setLineWidth(1.5);
        p.moveTo(w - 6, cy - 1); p.lineTo(w - 4, cy + 2); p.lineTo(w - 2, cy - 1); p.stroke();
    } else if (type == "VBoxContainer") {
        for (int i = 0; i < 3; ++i) {
            double by = 2 + i * 5;
            p.roundedRectangle(Rect{3, by, w - 6, 3.5}, 1); p.fill();
        }
    } else if (type == "HBoxContainer") {
        for (int i = 0; i < 3; ++i) {
            double bx = 2 + i * 5;
            p.roundedRectangle(Rect{bx, 3, 3.5, h - 6}, 1); p.fill();
        }
    } else if (type == "Column") {
        p.setLineWidth(1.5);
        p.moveTo(cx, 2); p.lineTo(cx, h - 4); p.stroke();
        p.moveTo(cx - 3, h - 7); p.lineTo(cx, h - 3); p.lineTo(cx + 3, h - 7); p.stroke();
    } else if (type == "Row") {
        p.setLineWidth(1.5);
        p.moveTo(2, cy); p.lineTo(w - 4, cy); p.stroke();
        p.moveTo(w - 7, cy - 3); p.lineTo(w - 3, cy); p.lineTo(w - 7, cy + 3); p.stroke();
    } else if (type == "Stack") {
        p.roundedRectangle(Rect{3, 2, w - 5, h - 5}, 1.5); p.stroke();
        p.roundedRectangle(Rect{1, 4, w - 5, h - 5}, 1.5); p.stroke();
    } else if (type == "Card") {
        p.setSourceColor(Color::fromRGBA(accent.r, accent.g, accent.b, 0.3));
        p.roundedRectangle(Rect{3, 4, w - 3, h - 4}, 2); p.fill();
        p.setSourceColor(accent);
        p.roundedRectangle(Rect{1, 2, w - 3, h - 4}, 2); p.fill();
    } else if (type == "ScrollView") {
        p.roundedRectangle(Rect{1, 2, w - 4, h - 4}, 1.5); p.stroke();
        p.moveTo(w - 2, 3); p.lineTo(w - 2, h - 3); p.stroke();
        p.setFillStyle();
        p.roundedRectangle(Rect{w - 3.5, 5, 2.5, 5}, 1); p.fill();
    } else if (type == "TableView") {
        p.setLineWidth(1.0);
        p.rectangle(Rect{1, 2, w - 2, h - 4}); p.stroke();
        p.moveTo(1, cy); p.lineTo(w - 1, cy); p.stroke();
        p.moveTo(cx, 2); p.lineTo(cx, h - 2); p.stroke();
    } else if (type == "TreeView") {
        p.setLineWidth(1.2);
        p.moveTo(2, 3); p.lineTo(6, 3);
        p.moveTo(4, 3); p.lineTo(4, cy);
        p.moveTo(4, cy); p.lineTo(8, cy);
        p.moveTo(6, cy); p.lineTo(6, h - 4);
        p.moveTo(6, h - 4); p.lineTo(10, h - 4); p.stroke();
        p.circle(2, 3, 1.5); p.fill();
        p.circle(8, cy, 1.5); p.fill();
        p.circle(10, h - 4, 1.5); p.fill();
    } else if (type == "ListView") {
        p.setLineWidth(1.5);
        for (int i = 0; i < 3; ++i) {
            double ly = 4 + i * 4.5;
            p.moveTo(3, ly); p.lineTo(w - 3, ly);
        }
        p.stroke();
    } else if (type == "TabWidget") {
        p.roundedRectangle(Rect{1, 5, 5, h - 6}, 1); p.fill();
        p.roundedRectangle(Rect{7, 5, 5, h - 6}, 1); p.stroke();
        p.roundedRectangle(Rect{1, 1, w - 2, 4}, 1); p.stroke();
    } else if (type == "Divider") {
        p.setLineWidth(1.5);
        p.moveTo(2, cy); p.lineTo(w - 2, cy); p.stroke();
    } else if (type == "Spacer") {
        p.setLineWidth(1.0);
        double dx = 2, dy = 3, dw = w - 4, dh = h - 6;
        p.moveTo(dx, dy); p.lineTo(dx + dw * 0.4, dy);
        p.moveTo(dx + dw * 0.6, dy); p.lineTo(dx + dw, dy);
        p.moveTo(dx, dy + dh); p.lineTo(dx + dw * 0.4, dy + dh);
        p.moveTo(dx + dw * 0.6, dy + dh); p.lineTo(dx + dw, dy + dh);
        p.moveTo(dx, dy); p.lineTo(dx, dy + dh);
        p.moveTo(dx + dw, dy); p.lineTo(dx + dw, dy + dh); p.stroke();
    } else if (type == "LinearProgressIndicator") {
        p.roundedRectangle(Rect{1, cy - 3, w - 2, 6}, 3); p.stroke();
        p.setFillStyle();
        p.roundedRectangle(Rect{2, cy - 2, (w - 4) * 0.6, 4}, 2); p.fill();
    } else if (type == "RatingBar") {
        double sr = cy - 1;
        double innerR = sr * 0.4;
        double startX = cx + std::cos(-M_PI / 2) * sr;
        double startY = cy + std::sin(-M_PI / 2) * sr;
        p.moveTo(startX, startY);
        for (int i = 0; i < 5; ++i) {
            double outerA = -M_PI / 2 + i * 2 * M_PI / 5;
            double innerA = outerA + M_PI / 5;
            p.lineTo(cx + std::cos(outerA) * sr, cy + std::sin(outerA) * sr);
            p.lineTo(cx + std::cos(innerA) * innerR, cy + std::sin(innerA) * innerR);
        }
        p.lineTo(startX, startY); p.fill();
    } else if (type == "Badge") {
        p.circle(cx, cy, cy - 1); p.fill();
        p.setSourceRGB(1, 1, 1); p.setLineWidth(1.5);
        p.moveTo(cx, cy - 3); p.lineTo(cx, cy + 3);
        p.moveTo(cx - 1.5, cy - 1.5); p.lineTo(cx, cy - 3); p.stroke();
    } else if (type == "CircleAvatar") {
        p.circle(cx, cy - 2, 3.5); p.fill();
        p.moveTo(2, h - 1); p.lineTo(2, h - 3);
        p.lineTo(cx - 3, cy + 4); p.lineTo(cx + 3, cy + 4);
        p.lineTo(w - 2, h - 3); p.lineTo(w - 2, h - 1); p.lineTo(2, h - 1); p.fill();
    } else if (type == "Chip") {
        p.roundedRectangle(Rect{1, cy - 4, w - 2, 8}, 4); p.fill();
        p.setSourceRGB(1, 1, 1); p.setLineWidth(1.2);
        double xx = w - 5;
        p.moveTo(xx - 1.5, cy - 1.5); p.lineTo(xx + 1.5, cy + 1.5);
        p.moveTo(xx + 1.5, cy - 1.5); p.lineTo(xx - 1.5, cy + 1.5); p.stroke();
    } else if (type == "SizedBox") {
        p.setLineWidth(1.0);
        double dx = 2, dy = 3, dw = w - 4, dh = h - 6;
        p.moveTo(dx, dy); p.lineTo(dx + dw, dy);
        p.lineTo(dx + dw, dy + dh); p.lineTo(dx, dy + dh); p.lineTo(dx, dy); p.stroke();
    } else {
        p.roundedRectangle(Rect{2, 2, w - 4, h - 4}, 2); p.fill();
    }

    p.restore();
}

void RenderDesignToolbox::ensureCategories() {
    if (categoriesBuilt_) return;
    categoriesBuilt_ = true;
    std::map<std::string, std::vector<std::string>> groups;
    std::vector<std::string> catOrder;
    for (auto& info : widgetTypeRegistry()) {
        if (groups.find(info.category) == groups.end())
            catOrder.push_back(info.category);
        groups[info.category].push_back(info.type);
    }
    for (auto& cat : catOrder) {
        Category c;
        c.name = cat;
        c.expanded = true;
        c.types = std::move(groups[cat]);
        categories_.push_back(std::move(c));
    }
}

double RenderDesignToolbox::totalHeight() const {
    double h = kPadTop;
    for (auto& cat : categories_) {
        h += kCatH;
        if (cat.expanded)
            h += cat.types.size() * kItemH;
    }
    return h + kPadBottom;
}

RenderDesignToolbox::HitResult RenderDesignToolbox::hitTestItem(double localY) const {
    double y = kPadTop;
    for (int ci = 0; ci < static_cast<int>(categories_.size()); ++ci) {
        if (localY >= y && localY < y + kCatH)
            return {ci, -1};
        y += kCatH;
        if (categories_[ci].expanded) {
            for (int ti = 0; ti < static_cast<int>(categories_[ci].types.size()); ++ti) {
                if (localY >= y && localY < y + kItemH)
                    return {ci, ti};
                y += kItemH;
            }
        }
    }
    return {-1, -1};
}

void RenderDesignToolbox::performPaint(PaintingContext& context, const Offset& offset) {
    ensureCategories();
    SkCanvas* canvas = context.canvas();
    Painter p(canvas, "DejaVu Sans");
    p.translate(offset.dx, offset.dy);
    const Size s{size_.width, size_.height};

    p.setSourceColor(Color::fromRGB(0.97, 0.97, 0.98));
    p.setFillStyle();
    p.rectangle(Rect{0, 0, s.width, s.height});
    p.fill();

    double y = kPadTop;
    for (int ci = 0; ci < static_cast<int>(categories_.size()); ++ci) {
        auto& cat = categories_[ci];
        Color accent = accentForCategory(cat.name);

        p.setSourceColor(Color::fromRGB(0.18, 0.20, 0.24));
        p.setFillStyle();
        p.rectangle(Rect{0, y, s.width, kCatH});
        p.fill();

        p.setSourceColor(accent);
        p.rectangle(Rect{0, y + kCatH - 2, s.width, 2});
        p.fill();

        p.setSourceRGB(0.75, 0.75, 0.80);
        p.setFontSize(9.0);
        p.save();
        p.translate(kPadLeft, y + kCatH / 2 + 3);
        const char* arrow = cat.expanded ? "\xe2\x96\xbc" : "\xe2\x96\xb6";
        p.showText(arrow);
        p.restore();

        p.setSourceRGB(0.92, 0.92, 0.95);
        p.setFontSize(11.0);
        p.save();
        p.translate(kPadLeft + 16, y + kCatH / 2 + 3.5);
        p.showText(cat.name.c_str());
        p.restore();

        p.setSourceColor(Color::fromRGBA(accent.r, accent.g, accent.b, 0.6));
        p.setFontSize(9.0);
        auto cnt = std::to_string(cat.types.size());
        auto ext = p.textExtents(cnt.c_str());
        p.save();
        p.translate(s.width - ext.width - 10, y + kCatH / 2 + 3);
        p.showText(cnt.c_str());
        p.restore();

        y += kCatH;

        if (cat.expanded) {
            for (int ti = 0; ti < static_cast<int>(cat.types.size()); ++ti) {
                auto& typeName = cat.types[ti];
                bool isHovered = (hoverCat_ == ci && hoverType_ == ti);

                double itemX = kPadLeft - 2;
                double itemW = s.width - kPadLeft * 2 + 4;
                double itemY = y + 1;
                double itemH = kItemH - 2;

                if (isHovered) {
                    p.setSourceColor(Color::fromRGBA(1, 1, 1, 0.06));
                    p.setFillStyle();
                    p.roundedRectangle(Rect{itemX, itemY, itemW, itemH}, 4);
                    p.fill();
                }

                double iconX = kPadLeft + 6;
                double iconY = y + (kItemH - kIconSize) / 2;

                p.setSourceColor(Color::fromRGBA(accent.r, accent.g, accent.b, 0.12));
                p.setFillStyle();
                p.circle(iconX + kIconSize / 2, iconY + kIconSize / 2, kIconSize / 2 + 2);
                p.fill();

                drawIcon(p, typeName, iconX, iconY, kIconSize, accent);

                p.setSourceRGB(0.85, 0.85, 0.90);
                p.setFontSize(11.0);
                p.save();
                p.translate(iconX + kIconSize + 8, y + kItemH / 2 + 3.5);
                p.showText(typeName.c_str());
                p.restore();

                y += kItemH;
            }
        }
    }

    // 拖拽中的幽灵标签（跟随光标，覆盖在面板内）。
    if (pendingDrag_ && !dragType_.empty()) {
        double gx = dragStart_.dx + 12;
        double gy = dragStart_.dy - 10;
        auto ext = p.textExtents(dragType_.c_str());
        double pad = 6;
        double gw = ext.width + pad * 2;
        double gh = ext.height + pad * 2;
        if (gx + gw > s.width) gx = s.width - gw - 4;
        if (gy < 0) gy = 4;
        if (gy + gh > s.height) gy = s.height - gh - 4;

        Color accent = accentForCategory(pressCat_ >= 0 ? categories_[pressCat_].name : "Basic");
        p.setSourceColor(Color::fromRGBA(0, 0, 0, 0.3));
        p.setFillStyle();
        p.roundedRectangle(Rect{gx + 1, gy + 1, gw, gh}, 4); p.fill();
        p.setSourceColor(Color::fromRGBA(accent.r, accent.g, accent.b, 0.85));
        p.roundedRectangle(Rect{gx, gy, gw, gh}, 4); p.fill();
        p.setSourceRGB(1, 1, 1);
        p.setFontSize(11.0);
        p.save();
        p.translate(gx + pad, gy + pad + ext.height - 1);
        p.showText(dragType_.c_str());
        p.restore();
    }
}

void RenderDesignToolbox::handleEvent(const PointerEvent& event,
                                       const HitTestEntry& /*entry*/) {
    ensureCategories();

    if (event.type == PointerEventType::Down) {
        auto hit = hitTestItem(event.position.dy);
        if (hit.catIndex >= 0 && hit.typeIndex == -1) {
            categories_[hit.catIndex].expanded = !categories_[hit.catIndex].expanded;
            markNeedsPaint();
            return;
        }
        if (hit.catIndex >= 0 && hit.typeIndex >= 0) {
            pressCat_ = hit.catIndex;
            pressType_ = hit.typeIndex;
            dragStart_ = event.position;
            pendingDrag_ = false;
            dragType_ = categories_[hit.catIndex].types[hit.typeIndex];
        }
    } else if (event.type == PointerEventType::Move) {
        auto hit = hitTestItem(event.position.dy);
        if (hit.catIndex != hoverCat_ || hit.typeIndex != hoverType_) {
            hoverCat_ = hit.catIndex;
            hoverType_ = hit.typeIndex;
        }
        if (pressCat_ >= 0 && !pendingDrag_) {
            double dx = event.position.dx - dragStart_.dx;
            double dy = event.position.dy - dragStart_.dy;
            if (std::sqrt(dx * dx + dy * dy) > kDragThreshold) {
                pendingDrag_ = true;
                if (controller_)
                    controller_->beginToolDrag(dragType_, event.globalPosition);
            }
        }
        markNeedsPaint();
    } else if (event.type == PointerEventType::Hover) {
        auto hit = hitTestItem(event.position.dy);
        if (hit.catIndex != hoverCat_ || hit.typeIndex != hoverType_) {
            hoverCat_ = hit.catIndex;
            hoverType_ = hit.typeIndex;
            markNeedsPaint();
        }
    } else if (event.type == PointerEventType::Up) {
        // 若已发起跨面板拖放，遮罩会接管 endToolDrag；这里仅复位本地状态。
        pressCat_ = -1;
        pressType_ = -1;
        pendingDrag_ = false;
        markNeedsPaint();
    }
}

}  // namespace designer
}  // namespace cui_core
