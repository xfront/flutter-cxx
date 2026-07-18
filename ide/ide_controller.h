#pragma once
/// @file ide/ide_controller.h
/// @brief ide/ 切 上游 流水线后的共享控制器。
///
/// 替代 legacy ide_main.cpp 里的全局裸指针（g_model/g_canvas/...），
/// 统一承载：
///   - DesignModel（框架无关，复用 design_model.h）；
///   - 工具拖拽状态（toolbox → canvas 跨控件拖放）；
///   - 预览开关（edit / preview 模式）；
///   - canvas 重绘钩子 + 模型变更钩子（解耦各 上游控件）。
///
/// 该头只依赖 foundation + designer 逻辑层，绝不引入 legacy 控件头，
/// 以免与 cui_widgets 内的同名控件触发 ODR。

#include <functional>
#include <memory>
#include <string>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/framework/focus.h"

namespace cui {
namespace designer {

class DesignModel;
class RenderDesignCanvas;
class RenderDesignProperties;

class IdeController {
public:
    explicit IdeController(SharedPtr<DesignModel> model)
        : model_(std::move(model)) {}

    SharedPtr<DesignModel> model() const { return model_; }

    // ── canvas 注册（用于 global→local 转换 + 重绘钩子）─────────────────
    void setCanvas(RenderDesignCanvas* c) { canvas_ = c; }
    RenderDesignCanvas* canvas() const { return canvas_; }

    // ── Properties 面板注册（键盘编辑时由 shell 路由 key 到此）──────────
    void setProperties(RenderDesignProperties* p) { properties_ = p; }
    RenderDesignProperties* properties() const { return properties_; }

    // ── 键盘入口（IdeShell 的 Focus onKeyEvent 转发）────────────────────
    // - 属性面板正在编辑 → 把 key 喂给属性面板；
    // - 否则 Delete/Backspace → 删除选中节点。
    void handleKey(const KeyEvent& ev);

    void setOnCanvasDirty(std::function<void()> f) { onCanvasDirty_ = std::move(f); }
    void setOnModelChanged(std::function<void()> f) { onModelChanged_ = std::move(f); }

    /// 选中集变化钩子（画布点击 / 框选后）：外壳据此刷新状态栏 + 依赖选中的面板。
    void setOnSelectionChanged(std::function<void()> f) { onSelectionChanged_ = std::move(f); }
    void syncSelectionChanged() {
        if (onSelectionChanged_) onSelectionChanged_();
    }

    /// 仅重绘画布（拖拽过程中高频调用，不重建整树）。
    void touchCanvas() { if (onCanvasDirty_) onCanvasDirty_(); }
    /// 画布重绘 + 外壳重建（选择/删除/属性变更后）。
    void syncAll() {
        if (onCanvasDirty_) onCanvasDirty_();
        if (onModelChanged_) onModelChanged_();
    }

    // ── 预览模式 ────────────────────────────────────────────────────────
    bool previewMode() const { return previewMode_; }
    void setPreviewMode(bool on) { previewMode_ = on; syncAll(); }
    void togglePreview() { setPreviewMode(!previewMode_); }

    // ── 工具拖拽（toolbox → canvas）──────────────────────────────────────
    bool toolDragging() const { return toolDragging_; }
    const std::string& dragType() const { return dragType_; }
    /// 当前拖放位置（画布盒内本地坐标；绘制时再减去 canvasOff）。
    Offset dragPos() const { return dragPos_; }

    // 以下三个方法需要 RenderDesignCanvas 的完整定义，实现见 ide_controller.cpp。
    void beginToolDrag(const std::string& type, Offset globalPos);
    void updateToolDrag(Offset globalPos);
    void endToolDrag(Offset globalPos);

    void cancelToolDrag() {
        dragType_.clear();
        toolDragging_ = false;
        touchCanvas();
    }

    // ── 键盘：删除选中（需要 DesignModel 完整定义，见 ide_controller.cpp）──
    void deleteSelected();

private:
    SharedPtr<DesignModel> model_;
    RenderDesignCanvas* canvas_ = nullptr;
    RenderDesignProperties* properties_ = nullptr;
    std::function<void()> onCanvasDirty_;
    std::function<void()> onModelChanged_;
    std::function<void()> onSelectionChanged_;

    bool previewMode_ = false;

    bool toolDragging_ = false;
    std::string dragType_;
    Offset dragPos_{};  // 画布盒内本地坐标
};

}  // namespace designer
}  // namespace cui
