#pragma once
/// @file rendering/render_inkwell.h
/// @brief RenderInkWell — Material 水波纹（ink splash）渲染对象。
///
/// 对应 上游源码：material/ink_well.dart 的 `_RenderInkWell` +
/// `InkSplash`（简化单水波）。
///
/// 约定：
///   - 继承 RenderProxyBox：先按默认路径画子节点，再在内容之上叠加
///     hover 高亮与水波纹圆形（clip 在自身矩形内）；
///   - 事件自处理（与 RenderSlider 同模式）：Down 记录水波圆心并启动
///     扩散动画，Up 触发 onTap，移动出界取消；Hover（鼠标）点亮高亮；
///   - 动画用 detail::RenderTransition（foundation Ticker 真实时间）：
///     水波扩散 400ms（EaseOutCubic，alpha 随 t 线性衰减），hover 150ms；
///   - 已知简化：事件模型无 pointer exit，hover 高亮只点亮不熄灭
///     （与 Tooltip 的隐藏语义一致——靠 Up 复位 pressed 状态）。

#include <algorithm>
#include <functional>

#include "cui/foundation/events.h"
#include "cui/foundation/geometry.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_interactive.h"  // detail::RenderTransition

namespace cui {

class RenderInkWell : public RenderProxyBox {
public:
    using VoidCallback = std::function<void()>;
    using DownCallback = std::function<void(const PointerEvent&)>;

    RenderInkWell()
        : splash_([this] { markNeedsPaint(); }),
          hoverT_([this] { markNeedsPaint(); }) {
        splash_.setDuration(400.0);
        hoverT_.setDuration(150.0);
    }

    bool hitTestSelf(const Offset& position) const override {
        return position.dx >= 0 && position.dy >= 0 &&
               position.dx < size_.width && position.dy < size_.height;
    }

    void setOnTap(VoidCallback cb) { onTap_ = std::move(cb); }
    void setOnTapDown(DownCallback cb) { onTapDown_ = std::move(cb); }
    void setOnTapCancel(VoidCallback cb) { onTapCancel_ = std::move(cb); }

    // ── 测试/headless 观测接口 ────────────────────────────────────────────
    bool hovered() const { return hovered_; }
    double hoverT() const { return hoverT_.t(); }
    /// 水波动画进行中（已开始且未到 1）。
    bool splashActive() const {
        double t = splash_.t();
        return splash_.animating() || (t > 0.0 && t < 1.0);
    }
    /// 水波扩散进度 t ∈ [0,1]（已含缓动）。
    double splashT() const { return splash_.t(); }
    /// 水波圆心（本地坐标）。
    const Offset& splashCenter() const { return splashCenter_; }
    /// 当前水波半径（本地坐标；t=0 时为 0）。
    double splashRadius() const {
        double maxDim = std::max(size_.width, size_.height);
        return maxDim * 1.2 * splash_.t();
    }

    void handleEvent(const PointerEvent& event, const HitTestEntry& entry) override;

protected:
    void performPaint(PaintingContext& context, const Offset& offset) override;

private:
    bool pressed_ = false;
    int64_t pressedId_ = -1;
    bool hovered_ = false;
    Offset splashCenter_{};
    detail::RenderTransition splash_;
    detail::RenderTransition hoverT_;
    VoidCallback onTap_;
    DownCallback onTapDown_;
    VoidCallback onTapCancel_;
};

}  // namespace cui_core
