#pragma once
/// @file ide/design_properties.h
/// @brief 上游 流水线版属性面板（RenderBox + 内联编辑）。
///
/// 替代 legacy DesignPropertyEditor。绘制与交互逻辑与 legacy design_properties.cpp
/// 完全一致，仅入口从控件的 paint 换成 RenderBox::performPaint，并且
/// 键盘编辑经 IdeController::handleKey 转发到本面板的 onKey（由 IdeShell 的
/// Focus 捕获 Delete/字符键后路由）。

#include <memory>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/framework/focus.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_box.h"
#include "ide_controller.h"

namespace cui {
namespace designer {

struct DesignNode;  // design_model.h（头文件只用到指针，无需完整定义）

class RenderDesignProperties : public RenderBox {
public:
    void setController(SharedPtr<IdeController> c) {
        controller_ = std::move(c);
        controller_->setProperties(this);
    }
    SharedPtr<IdeController> controller() const { return controller_; }

    void performLayout() override { setSize(constraints().biggest()); }
    void performPaint(PaintingContext& context, const Offset& offset) override;
    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 &&
               position.dx < size_.width && position.dy < size_.height;
    }
    void handleEvent(const PointerEvent& event, const HitTestEntry& entry) override;

    // ── 供 IdeController 键盘路由 ────────────────────────────────────────
    bool isEditing() const { return editingRow_ >= 0; }
    void onKey(const KeyEvent& ev);

    void markDirty() { markNeedsPaint(); }

private:
    SharedPtr<IdeController> controller_;
    DesignModel* model() const {
        return controller_ ? controller_->model().get() : nullptr;
    }

    struct Row {
        std::string group;
        std::string name;
        std::string kind;    // "String" | "Int" | "Bool" | "Color" | "Enum" | "Double"
        std::string value;
    };
    std::vector<Row> rows_;

    int editingRow_ = -1;     // 正在编辑的行（-1 = 无）
    std::string editingValue_;
    DesignNode* editingNode_ = nullptr;  // 上次的选中项（变化则结束编辑）
    int hoverRow_ = -1;

    static constexpr double kRowH = 24.0;
    static constexpr double kHeaderH = 28.0;
    static constexpr double kLabelW = 90.0;
    static constexpr double kPadLeft = 6.0;

    void rebuildRows();
    int hitTestRow(double localY) const;
};

class DesignProperties : public LeafRenderObjectWidget {
public:
    explicit DesignProperties(SharedPtr<IdeController> controller,
                                     KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)),
          controller_(std::move(controller)) {}

    RenderObject* createRenderObject() override {
        return new RenderDesignProperties();
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderDesignProperties*>(ro);
        if (r) r->setController(controller_);
    }

private:
    SharedPtr<IdeController> controller_;
};

}  // namespace designer
}  // namespace cui
