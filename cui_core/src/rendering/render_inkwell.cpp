/// @file rendering/render_inkwell.cpp
/// @brief RenderInkWell 实现：hover 高亮 + 水波纹扩散叠加绘制。

#include "cui/rendering/render_inkwell.h"

#include <algorithm>

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRRect.h>

#include "cui/rendering/painting_context.h"

namespace cui {

namespace {

inline void setPaintColor(SkPaint& p, const Color& c) {
    p.setColor4f(SkColor4f{static_cast<float>(c.r), static_cast<float>(c.g),
                           static_cast<float>(c.b), static_cast<float>(c.a)},
                 nullptr);
}

}  // namespace

void RenderInkWell::handleEvent(const PointerEvent& event, const HitTestEntry&) {
    if (event.type == PointerEventType::Down) {
        bool primary = (event.pointerKind != PointerDeviceKind::mouse) ||
                       ((event.buttons & kPrimaryMouseButton) != 0);
        if (!primary)
            return;
        pressed_ = true;
        pressedId_ = event.pointerId;
        splashCenter_ = event.position;
        splash_.jump(0.0);
        splash_.to(1.0);
        if (onTapDown_)
            onTapDown_(event);
    } else if (event.type == PointerEventType::Move) {
        // 按住后拖出边界 → 取消点击（水波继续淡出，不触发 onTap）。
        if (pressed_ && event.pointerId == pressedId_ && !hitTestSelf(event.position)) {
            pressed_ = false;
            pressedId_ = -1;
            if (onTapCancel_)
                onTapCancel_();
        }
    } else if (event.type == PointerEventType::Hover) {
        if (event.pointerKind == PointerDeviceKind::mouse && !hovered_) {
            hovered_ = true;
            hoverT_.to(1.0);
        }
    } else if (event.type == PointerEventType::Up) {
        if (event.pointerId != pressedId_)
            return;
        bool wasPressed = pressed_;
        pressed_ = false;
        pressedId_ = -1;
        if (wasPressed && onTap_)
            onTap_();
    }
}

void RenderInkWell::performPaint(PaintingContext& context, const Offset& offset) {
    // 先按代理盒默认路径画子节点。
    RenderProxyBox::performPaint(context, offset);

    SkCanvas* c = context.canvas();
    if (!c || size_.width <= 0 || size_.height <= 0)
        return;

    double ht = hoverT_.t();
    double st = splash_.t();
    if (ht <= 0.0 && st <= 0.0)
        return;

    double w = size_.width, h = size_.height;
    c->save();
    SkRect rect = SkRect::MakeXYWH(SkScalar(offset.dx), SkScalar(offset.dy),
                                   SkScalar(w), SkScalar(h));
    c->clipRect(rect);

    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kFill_Style);

    // hover 高亮层（半透明灰罩）。
    if (ht > 0.0) {
        setPaintColor(p, Color::fromRGBA(0, 0, 0, 0.06 * ht));
        c->drawRect(rect, p);
    }

    // 水波纹：以按下点为圆心扩散，alpha 随进度衰减。
    if (st > 0.0) {
        double radius = std::max(w, h) * 1.2 * st;
        Color sc = Color::fromRGBA(0, 0, 0, 0.25 * (1.0 - st));
        setPaintColor(p, sc);
        c->drawCircle(SkPoint::Make(SkScalar(offset.dx + splashCenter_.dx),
                                    SkScalar(offset.dy + splashCenter_.dy)),
                      SkScalar(radius), p);
    }

    c->restore();
}

}  // namespace cui_core
