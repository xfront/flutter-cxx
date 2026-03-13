/// @file widgets/render_object_widget.cpp
/// @brief RenderObjectWidget 家族实现。

#include "cui/framework/render_object_widget.h"
#include "cui/foundation/shared_ptr.h"

namespace cui {

std::unique_ptr<Element> RenderObjectWidget::createElement(SharedPtr<Widget> self) {
    return std::make_unique<RenderObjectElement>(std::move(self));
}

std::unique_ptr<Element> LeafRenderObjectWidget::createElement(SharedPtr<Widget> self) {
    return std::make_unique<LeafRenderObjectElement>(std::move(self));
}

std::unique_ptr<Element> SingleChildRenderObjectWidget::createElement(
        SharedPtr<Widget> self) {
    return std::make_unique<SingleChildRenderObjectElement>(std::move(self));
}

std::unique_ptr<Element> MultiChildRenderObjectWidget::createElement(SharedPtr<Widget> self) {
    return std::make_unique<MultiChildRenderObjectElement>(std::move(self));
}

}
