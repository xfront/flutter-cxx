#pragma once
/// @file ide/design_toolbox.h
/// @brief 上游 流水线版控件面板（分类 + 拖源），替代 legacy DesignToolbox。

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

class RenderDesignToolbox : public RenderBox {
public:
    void setController(SharedPtr<IdeController> c) { controller_ = std::move(c); }
    SharedPtr<IdeController> controller() const { return controller_; }

    void performLayout() override { setSize(constraints().biggest()); }
    void performPaint(PaintingContext& context, const Offset& offset) override;
    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 &&
               position.dx < size_.width && position.dy < size_.height;
    }
    void handleEvent(const PointerEvent& event, const HitTestEntry& entry) override;

private:
    SharedPtr<IdeController> controller_;

    struct Category {
        std::string name;
        bool expanded = true;
        std::vector<std::string> types;
    };
    std::vector<Category> categories_;

    bool pendingDrag_ = false;
    std::string dragType_;
    int pressCat_ = -1;
    int pressType_ = -1;
    Offset dragStart_{};
    int hoverCat_ = -1;
    int hoverType_ = -1;

    static constexpr double kItemH = 30.0;
    static constexpr double kCatH = 26.0;
    static constexpr double kPadTop = 4.0;
    static constexpr double kPadBottom = 4.0;
    static constexpr double kPadLeft = 8.0;
    static constexpr double kIconSize = 16.0;
    static constexpr double kDragThreshold = 5.0;

    struct HitResult { int catIndex; int typeIndex; };
    HitResult hitTestItem(double localY) const;
    double totalHeight() const;

    static void drawIcon(Painter& p, const std::string& type,
                         double x, double y, double s, const Color& accent);

    void ensureCategories();
    bool categoriesBuilt_ = false;
};

class DesignToolbox : public LeafRenderObjectWidget {
public:
    explicit DesignToolbox(SharedPtr<IdeController> controller,
                                  KeyPtr key = nullptr)
        : LeafRenderObjectWidget(std::move(key)),
          controller_(std::move(controller)) {}

    RenderObject* createRenderObject() override {
        return new RenderDesignToolbox();
    }
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderDesignToolbox*>(ro);
        if (r) r->setController(controller_);
    }

private:
    SharedPtr<IdeController> controller_;
};

}  // namespace designer
}  // namespace cui
