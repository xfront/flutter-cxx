#pragma once
/// @file framework/render_object_widget.h
/// @brief 上游的 RenderObjectWidget 体系移植。
///
/// 对应 上游源码：
///   上游 widgets/render_object_widget.dart
///
/// 这是 Widget 层与 Render 层的桥。三种基类（与 上游 完全一致）：
///   RenderObjectWidget          — 抽象基类
///   LeafRenderObjectWidget      — 叶子（自己就是 RenderObject）
///   SingleChildRenderObjectWidget — 单子节点
///   MultiChildRenderObjectWidget  — 多子节点列表

#include <memory>
#include <vector>

#include "../utils/shared_ptr.h"
#include "cui/rendering/render_object.h"
#include "element.h"

namespace cui {

/// 渲染对象型 Widget 的基类。
class RenderObjectWidget : public Widget {
public:
    using Widget::Widget;

    /// 创建对应的 RenderObject。上游的 `createRenderObject()`。
    virtual RenderObject* createRenderObject() = 0;

    /// 更新已存在的 RenderObject 属性。上游的 `updateRenderObject()`。
    virtual void updateRenderObject(RenderObject* renderObject) { (void)renderObject; }

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override;
};

/// 叶子渲染对象 Widget：自身即 RenderObject。
class LeafRenderObjectWidget : public RenderObjectWidget {
public:
    using RenderObjectWidget::RenderObjectWidget;

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override;
};

/// 单子节点渲染对象 Widget。上游的 `SingleChildRenderObjectWidget`。
class SingleChildRenderObjectWidget : public RenderObjectWidget {
public:
    explicit SingleChildRenderObjectWidget(KeyPtr key = nullptr,
                                           SharedPtr<Widget> child = nullptr)
        : RenderObjectWidget(std::move(key)), child_(std::move(child)) {}

    /// 子节点 Widget。
    const SharedPtr<Widget>& child() const { return child_; }
    void setChild(SharedPtr<Widget> child) { child_ = std::move(child); }

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override;

protected:
    SharedPtr<Widget> child_;
};

/// 多子节点渲染对象 Widget。上游的 `MultiChildRenderObjectWidget`。
///
/// 子节点列表由 Widget 持有；对应的 Element 走 上游的
/// `updateChildren` reconcile 规则（有 Key 按 Key 匹配，无 Key 按位置）。
class MultiChildRenderObjectWidget : public RenderObjectWidget {
public:
    explicit MultiChildRenderObjectWidget(KeyPtr key = nullptr,
                                          std::vector<SharedPtr<Widget>> children = {})
        : RenderObjectWidget(std::move(key)), children_(std::move(children)) {}

    /// 子节点 Widget 列表。
    const std::vector<SharedPtr<Widget>>& children() const { return children_; }
    void setChildren(std::vector<SharedPtr<Widget>> children) {
        children_ = std::move(children);
    }

    std::unique_ptr<Element> createElement(SharedPtr<Widget> self) override;

protected:
    std::vector<SharedPtr<Widget>> children_;
};

}
