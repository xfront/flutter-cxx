#pragma once
/// @file widgets/animated_size.h
/// @brief AnimatedSize —— 子节点尺寸变化时用动画过渡自身尺寸的控件。
///
/// 对应 上游源码：上游 widgets/animated_size.dart（`AnimatedSize` +
/// `_AnimatedSizeState` + 私有的 `_AnimatedSize extends SingleChildRenderObjectWidget`）。
///
/// 层级映射：本仓的 `AnimatedSize` 直接就是上游那个私有 `_AnimatedSize` 的
/// 公开对应物（SingleChildRenderObjectWidget → RenderAnimatedSize），上游
/// 外层的 StatefulWidget 没有移植。原因：State 存在的唯一目的是用
/// `SingleTickerProviderStateMixin` 造/销 `AnimationController`；本仓渲染层
/// 的 `RenderAnimatedSize` 自己持有 Ticker（见 rendering/animated_size.h 的
/// 偏离 1），控制器的生命周期天然跟着渲染对象走，而渲染对象在
/// `updateRenderObject` 路径上是复用的 ⇒ 状态同样跨帧存活，不需要 State。
/// 上游 `didUpdateWidget` 里把 duration 同步给 controller，本仓对应
/// `updateRenderObject` 里的 `setDuration`。
///
/// 参数对照（上游 → 本仓）：
///   alignment        → Alignment（默认 center，即 {0,0}）
///   curve: Curve     → Easing（applyEasing 包成函数交给渲染层，见偏离 2）
///   duration: Duration → durationMs: double
///   reverseDuration  → 未移植（上游只 forward()/stop()，从不 reverse()）
///   clipBehavior     → Clip（默认 hardEdge）
///   onEnd: VoidCallback → std::function<void()>
///   child            → child

#include <functional>
#include <memory>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/framework/framework.h"
#include "cui/framework/render_object_widget.h"
#include "cui/rendering/animated_size.h"
#include "cui/widgets/animation/animation.h"

namespace cui {

/// 上游的 `AnimatedSize`。
class AnimatedSize : public SingleChildRenderObjectWidget {
public:
    explicit AnimatedSize(SharedPtr<Widget> child = nullptr,
                          Alignment alignment = Alignment::center(),
                          double durationMs = 0.0, Easing curve = Easing::Linear,
                          Clip clipBehavior = Clip::HardEdge,
                          RenderAnimatedSize::OnEnd onEnd = nullptr, KeyPtr key = nullptr)
        : SingleChildRenderObjectWidget(std::move(key), std::move(child)),
          alignment_(alignment),
          durationMs_(durationMs),
          curve_(curve),
          clipBehavior_(clipBehavior),
          onEnd_(std::move(onEnd)) {}

    Alignment alignment() const { return alignment_; }
    void setAlignment(Alignment a) { alignment_ = a; }

    double duration() const { return durationMs_; }
    void setDuration(double ms) { durationMs_ = ms; }

    Easing curve() const { return curve_; }
    void setCurve(Easing e) { curve_ = e; }

    Clip clipBehavior() const { return clipBehavior_; }
    void setClipBehavior(Clip c) { clipBehavior_ = c; }

    const RenderAnimatedSize::OnEnd& onEnd() const { return onEnd_; }
    void setOnEnd(RenderAnimatedSize::OnEnd cb) { onEnd_ = std::move(cb); }

    RenderObject* createRenderObject() override {
        return new RenderAnimatedSize(alignment_, durationMs_, makeCurveFn(curve_),
                                       clipBehavior_, onEnd_);
    }

    /// 上游的 `_AnimatedSize.updateRenderObject`：参数逐个推给渲染对象，
    /// 全部走渲染层「等值早退」的 setter ⇒ 参数没变不会白标脏。
    void updateRenderObject(RenderObject* ro) override {
        auto* r = dynamic_cast<RenderAnimatedSize*>(ro);
        if (!r)
            return;
        r->setAlignment(alignment_);
        r->setDuration(durationMs_);
        r->setCurve(makeCurveFn(curve_));
        r->setClipBehavior(clipBehavior_);
        r->setOnEnd(onEnd_);
    }

private:
    /// 把 cui_widgets 的 `Easing` 包成渲染层能用的曲线函数（渲染层不能反向
    /// 依赖本层，见 rendering/animated_size.h 的偏离 2）。
    static RenderAnimatedSize::CurveFn makeCurveFn(Easing e) {
        if (e == Easing::Linear)
            return nullptr;  // 空 = 线性，省一次函数调用
        return [e](double t) { return applyEasing(t, e); };
    }

    Alignment alignment_ = Alignment::center();
    double durationMs_ = 0.0;
    Easing curve_ = Easing::Linear;
    Clip clipBehavior_ = Clip::HardEdge;
    RenderAnimatedSize::OnEnd onEnd_;
};

}  // namespace cui_core
