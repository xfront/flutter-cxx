/// @file ide/ide_controller.cpp
/// @brief IdeController 的键盘路由实现。
///
/// 键盘由 IdeShell 的 Focus(FocusNode) 捕获后转发到这里：
///   - 属性面板处于编辑态 → 交给面板（字符输入 / 回车提交 / Esc 取消）；
///   - 否则 Delete / Backspace → 删除当前选中的节点。

#include "ide_controller.h"

#include "cui/platform/event.h"

#include "design_canvas.h"
#include "design_properties.h"
#include "design_model.h"

namespace cui {
namespace designer {

// ── 工具拖拽（需要 RenderDesignCanvas 的完整定义）────────────────────────

void IdeController::beginToolDrag(const std::string& type, Offset globalPos) {
    dragType_ = type;
    toolDragging_ = true;
    if (canvas_) {
        dragPos_ = canvas_->globalToLocal(globalPos);
        canvas_->markDirty();
    }
    touchCanvas();
}

void IdeController::updateToolDrag(Offset globalPos) {
    if (canvas_) {
        dragPos_ = canvas_->globalToLocal(globalPos);
        canvas_->markDirty();
    }
    touchCanvas();
}

void IdeController::endToolDrag(Offset globalPos) {
    if (canvas_) {
        Offset lp = canvas_->toDesign(canvas_->globalToLocal(globalPos));
        canvas_->completeDrop(dragType_, lp);
    }
    dragType_.clear();
    toolDragging_ = false;
    syncAll();
}

void IdeController::deleteSelected() {
    if (!model_) return;
    auto sel = model_->multiSelection();
    for (auto* node : sel)
        model_->removeNode(node);
    syncAll();
}

// ── 键盘路由 ────────────────────────────────────────────────────────────

void IdeController::handleKey(const KeyEvent& ev) {
    if (properties_ && properties_->isEditing()) {
        properties_->onKey(ev);
        return;
    }
    const auto code = static_cast<KeyCode>(ev.key);
    if (code == KeyCode::Delete || code == KeyCode::Backspace)
        deleteSelected();
}

}  // namespace designer
}  // namespace cui_core
