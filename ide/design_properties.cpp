/// @file ide/design_properties.cpp
/// @brief 上游 流水线版属性面板实现（绘制 + 交互 + 键盘编辑）。
///
/// 绘制/交互逻辑与 legacy design_properties.cpp 一致，仅把入口从
/// 控件的 paint / onMouse* 换成 RenderBox::performPaint /
/// RenderObject::handleEvent，键盘编辑则由 IdeController::handleKey 转发到
/// onKey（IdeShell 的 Focus 捕获按键后路由）。

#include "design_properties.h"

#include <cstdio>

#include "cui/foundation/painter.h"
#include "cui/foundation/theme.h"
#include "cui/platform/event.h"
#include "cui/rendering/painting_context.h"

#include "design_model.h"

namespace cui {
namespace designer {

// ── 行构建 ─────────────────────────────────────────────────────────────────

void RenderDesignProperties::rebuildRows() {
    rows_.clear();

    DesignModel* m = model();
    DesignNode* sel = m ? m->selected() : nullptr;

    // 选中项变化 → 结束正在进行的编辑（与 legacy 的 onChanged 行为一致）。
    if (sel != editingNode_) {
        editingNode_ = sel;
        editingRow_ = -1;
        editingValue_.clear();
    }
    if (!sel) return;

    auto* info = findWidgetType(sel->widgetType);
    if (!info) return;

    for (auto& pm : info->props) {
        Row r;
        r.group = pm.group;
        r.name = pm.name;
        switch (pm.kind) {
        case PropKind::String: r.kind = "String"; break;
        case PropKind::Int:    r.kind = "Int";    break;
        case PropKind::Double: r.kind = "Double"; break;
        case PropKind::Bool:   r.kind = "Bool";   break;
        case PropKind::Color:  r.kind = "Color";  break;
        case PropKind::Enum:   r.kind = "Enum";   break;
        }
        r.value = sel->prop(pm.name, pm.defaultVal);
        rows_.push_back(std::move(r));
    }
}

int RenderDesignProperties::hitTestRow(double localY) const {
    double y = kHeaderH;
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        if (localY >= y && localY < y + kRowH)
            return i;
        y += kRowH;
    }
    return -1;
}

// ── 绘制 ───────────────────────────────────────────────────────────────────

void RenderDesignProperties::performPaint(PaintingContext& context,
                                          const Offset& offset) {
    SkCanvas* canvas = context.canvas();
    Painter p(canvas, "DejaVu Sans");
    p.translate(offset.dx, offset.dy);

    const Size s{size_.width, size_.height};

    // 每次绘制前重建行：模型变更由外壳重建 + 本盒标脏驱动。
    rebuildRows();

    // Background.
    p.setSourceColor(theme().surface);
    p.setFillStyle();
    p.rectangle(Rect{0, 0, s.width, s.height});
    p.fill();

    // Header.
    p.setSourceRGB(0.25, 0.28, 0.32);
    p.setFillStyle();
    p.rectangle(Rect{0, 0, s.width, kHeaderH});
    p.fill();

    DesignNode* sel = model() ? model()->selected() : nullptr;
    p.setSourceRGB(0.9, 0.9, 0.92);
    p.setFontSize(11.0);
    p.save();
    p.translate(kPadLeft, kHeaderH / 2 + 3.5);
    if (sel) {
        std::string header = sel->widgetType + " — " + sel->id;
        p.showText(header.c_str());
    } else {
        p.showText("(no selection)");
    }
    p.restore();

    // Property rows.
    double y = kHeaderH;
    std::string lastGroup;
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        auto& row = rows_[i];

        // Group header.
        if (row.group != lastGroup) {
            p.setSourceRGB(0.3, 0.33, 0.38);
            p.setFillStyle();
            p.rectangle(Rect{0, y, s.width, kRowH});
            p.fill();
            p.setSourceRGB(0.8, 0.82, 0.85);
            p.setFontSize(10.0);
            p.save();
            p.translate(kPadLeft, y + kRowH / 2 + 3);
            p.showText(row.group.c_str());
            p.restore();
            y += kRowH;
            lastGroup = row.group;
        }

        bool isEditing = (i == editingRow_);

        // Row background (alternating).
        if (isEditing) {
            p.setSourceRGB(0.85, 0.9, 1.0);
        } else if (i % 2 == 0) {
            p.setSourceRGB(0.97, 0.97, 0.98);
        } else {
            p.setSourceRGB(0.93, 0.93, 0.95);
        }
        p.setFillStyle();
        p.rectangle(Rect{0, y, s.width, kRowH});
        p.fill();

        // Property name.
        p.setSourceRGB(0.2, 0.2, 0.25);
        p.setFontSize(10.5);
        p.save();
        p.translate(kPadLeft, y + kRowH / 2 + 3);
        p.showText(row.name.c_str());
        p.restore();

        // Property value.
        if (isEditing) {
            double valX = kLabelW + 2;
            double valW = s.width - kLabelW - 4;
            p.setSourceRGB(1.0, 1.0, 1.0);
            p.setFillStyle();
            p.rectangle(Rect{valX, y + 2, valW, kRowH - 4});
            p.fill();
            p.setSourceRGB(0.0, 0.47, 0.85);
            p.setLineWidth(1.0);
            p.rectangle(Rect{valX, y + 2, valW, kRowH - 4});
            p.stroke();

            p.setSourceRGB(0.1, 0.1, 0.15);
            p.setFontSize(10.5);
            p.save();
            p.translate(valX + 4, y + kRowH / 2 + 3);
            std::string display = editingValue_ + "_";
            p.showText(display.c_str());
            p.restore();
        } else if (row.kind == "Bool") {
            double cbX = kLabelW + 6;
            double cbY = y + 5;
            double cbS = 13;
            bool checked = (row.value == "true" || row.value == "1");
            p.setSourceRGB(1.0, 1.0, 1.0);
            p.setFillStyle();
            p.rectangle(Rect{cbX, cbY, cbS, cbS});
            p.fill();
            p.setSourceRGB(0.5, 0.5, 0.55);
            p.setLineWidth(1.0);
            p.rectangle(Rect{cbX, cbY, cbS, cbS});
            p.stroke();
            if (checked) {
                p.setSourceRGB(0.0, 0.47, 0.85);
                p.setLineWidth(1.5);
                p.moveTo(cbX + 3, cbY + cbS / 2);
                p.lineTo(cbX + cbS / 2 - 1, cbY + cbS - 3);
                p.lineTo(cbX + cbS - 3, cbY + 3);
                p.stroke();
            }
        } else if (row.kind == "Color") {
            double swX = kLabelW + 6;
            double swY = y + 5;
            double swS = 13;
            double cr = 0.5, cg = 0.5, cb = 0.5;
            if (row.value.size() >= 7 && row.value[0] == '#') {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                    return 0;
                };
                cr = (hex(row.value[1]) * 16 + hex(row.value[2])) / 255.0;
                cg = (hex(row.value[3]) * 16 + hex(row.value[4])) / 255.0;
                cb = (hex(row.value[5]) * 16 + hex(row.value[6])) / 255.0;
            }
            p.setSourceRGB(cr, cg, cb);
            p.setFillStyle();
            p.rectangle(Rect{swX, swY, swS, swS});
            p.fill();
            p.setSourceRGB(0.5, 0.5, 0.55);
            p.setLineWidth(0.5);
            p.rectangle(Rect{swX, swY, swS, swS});
            p.stroke();
            p.setSourceRGB(0.15, 0.15, 0.2);
            p.setFontSize(10.0);
            p.save();
            p.translate(swX + swS + 4, y + kRowH / 2 + 3);
            p.showText(row.value.c_str());
            p.restore();
        } else {
            p.setSourceRGB(0.15, 0.15, 0.2);
            p.setFontSize(10.5);
            p.save();
            p.translate(kLabelW + 6, y + kRowH / 2 + 3);
            p.showText(row.value.c_str());
            p.restore();
        }

        y += kRowH;
    }
}

// ── 指针事件 ───────────────────────────────────────────────────────────────

void RenderDesignProperties::handleEvent(const PointerEvent& event,
                                         const HitTestEntry& /*entry*/) {
    if (event.type != PointerEventType::Down) return;
    if (!model()) return;

    int row = hitTestRow(event.position.dy);
    if (row < 0) {
        editingRow_ = -1;
        markNeedsPaint();
        return;
    }

    DesignNode* sel = model()->selected();
    if (!sel) return;

    auto& r = rows_[row];

    // Bool toggle: click anywhere on the row.
    if (r.kind == "Bool") {
        std::string newVal = (r.value == "true" || r.value == "1") ? "false" : "true";
        model()->setProperty(sel, r.name, newVal);
        if (controller_) controller_->syncAll();
        markNeedsPaint();
        return;
    }

    // Start editing other types.
    editingRow_ = row;
    editingValue_ = r.value;
    markNeedsPaint();
}

// ── 键盘编辑（由 IdeController::handleKey 路由）───────────────────────────

void RenderDesignProperties::onKey(const KeyEvent& ev) {
    if (editingRow_ < 0) return;

    const auto code = static_cast<KeyCode>(ev.key);

    if (code == KeyCode::Return) {  // Enter → commit.
        DesignNode* sel = model() ? model()->selected() : nullptr;
        if (sel && editingRow_ < static_cast<int>(rows_.size()))
            model()->setProperty(sel, rows_[editingRow_].name, editingValue_);
        editingRow_ = -1;
        if (controller_) controller_->syncAll();
        markNeedsPaint();
        return;
    }
    if (code == KeyCode::Escape) {  // Escape → cancel.
        editingRow_ = -1;
        markNeedsPaint();
        return;
    }
    if (code == KeyCode::Backspace || code == KeyCode::Delete) {
        if (!editingValue_.empty())
            editingValue_.pop_back();
        markNeedsPaint();
        return;
    }

    // Printable character → append.
    if (!ev.text.empty() && static_cast<unsigned char>(ev.text[0]) >= 32) {
        editingValue_ += ev.text;
        markNeedsPaint();
    }
}

}  // namespace designer
}  // namespace cui_core
