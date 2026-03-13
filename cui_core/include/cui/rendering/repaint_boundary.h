#pragma once
/// @file rendering/repaint_boundary.h
/// @brief RenderRepaintBoundary — 把子树录进独立 PictureLayer 的重绘边界。
///
/// 对应 上游 rendering/proxy_box.dart 的 `RenderRepaintBoundary`，它本身就是
/// 一个 `RenderProxyBox`：布局/绘制/命中/intrinsics 全部沿用代理盒语义，
/// 本类只多一条 `isRepaintBoundary()`。
///
/// 约定：
///   - 「建层、何时重录、复用到哪一帧」由**父级**的
///     `PaintingContext::paintChild` → `pushRepaintBoundary` 决定（上游的
///     `context.pushPaintingBoundary(this, layer, offset)`），本类的
///     performPaint 不感知自己的边界身份；
///   - 图层对象存在基类的 `layer_`（`setLayer`/`layer()`），本类不要再声明
///     同名成员——非虚的基类访问器会被遮蔽，边界自己的 `layer_` 永远是空的；
///   - **不是** relayout boundary（上游同口径）：代理盒把子节点尺寸透传给
///     父节点，子节点长高时父节点必须重排。
///
/// 上游的 `alwaysNeedsCompositing`、`isComplex`/`willChange`（Skia 光栅提示）
/// 与 `showOnScreen()` 未移植。

#include "cui/rendering/render_box.h"

namespace cui {

/// 单代理盒子节点，但绘制输出被缓存进自己的 PictureLayer。
class RenderRepaintBoundary : public RenderProxyBox {
public:
    RenderRepaintBoundary() = default;
    ~RenderRepaintBoundary() override = default;

    /// 上游的 `RenderRepaintBoundary.isRepaintBoundary`。
    bool isRepaintBoundary() const override { return true; }
};

}  // namespace cui_core
