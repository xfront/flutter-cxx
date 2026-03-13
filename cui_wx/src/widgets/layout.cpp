/// @file widgets/layout.cpp
/// @brief LayoutBuilderElement 实现（属于 cui_wx）。
///
/// LayoutBuilderElement 的声明在 framework/element.h（core），但四个方法的实现
/// 要用 widgets/layout.h 里的 LayoutBuilder 控件，因此放在控件层编译；core 一侧
/// 只保留声明，不再 include 任何 cui_core/widgets/ 头文件。

#include "cui/widgets/layout.h"

#include <memory>
#include <utility>

#include "cui/framework/element.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/render_box_widgets.h"

namespace cui {

// ── LayoutBuilderElement ─────────────────────────────────────────────────────

void LayoutBuilderElement::mount(Element* parent, BuildOwner* owner) {
    // 先走基类挂载：didMount 会用 widget->child()（LayoutBuilder 为 null）
    // 建立初始（空）子节点；真正的子节点在首次布局时由 layoutCallback 重建。
    SingleChildRenderObjectElement::mount(parent, owner);
    if (auto* rl = dynamic_cast<RenderLayoutBuilder*>(renderObject()))
        rl->layoutCallback = [this](const BoxConstraints& c) { rebuild(c); };
}

void LayoutBuilderElement::update(SharedPtr<Widget> newWidget) {
    // 只走 RenderObjectElement::update（更新 widget 引用 + updateRenderObject
    // + performRebuild）。SingleChildRenderObjectElement::update 会用
    // widget->child()（LayoutBuilder 恒为 null）把 layoutCallback 挂载的
    // 动态子树清掉——约束未变时 layoutCallback 不再触发，子树永久丢失、
    // RenderLayoutBuilder 退化为 biggest() 尺寸（在无界约束下即 1e18）。
    RenderObjectElement::update(std::move(newWidget));
}

void LayoutBuilderElement::rebuild(const BoxConstraints& constraints) {
    // 仅在约束变化时同步重建，避免 performLayout 自调用死循环。
    if (constraints == constraints_)
        return;
    constraints_ = constraints;
    performRebuild();
}

void LayoutBuilderElement::performRebuild() {
    // 同步 RenderObject 配置（上游的 updateRenderObject）。
    if (renderObject_) {
        if (auto* row = dynamic_cast<RenderObjectWidget*>(widget_.get()))
            row->updateRenderObject(renderObject_.get());
    }
    // 用当前约束重建子节点并 reconcile（对应 上游的 _LayoutBuilderElement）。
    auto* lb = dynamic_cast<LayoutBuilder*>(widget_.get());
    SharedPtr<Widget> built = lb ? lb->build(*this, constraints_) : nullptr;
    child_ = updateChild(std::move(child_), built);
    dirty_ = false;
}

}  // namespace cui_core
