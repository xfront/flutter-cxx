#pragma once
/// @file widgets/tooltip.h
/// @brief Tooltip — 悬停/长按显示提示气泡。
///
/// 对应 上游源码：material/tooltip.dart。
///
/// 移植约定：
///   - 触发：鼠标 hover（PointerEventType::Hover）立即显示；触摸/按压
///     长按 waitMs（默认 500ms，foundation Ticker 真实时间）后显示；
///   - 隐藏：鼠标移开控件（框架派发的 Exit 事件）即隐藏；按下后抬起（Up）
///     也隐藏；长按计时在隐藏时取消；
///   - 气泡不是 Overlay 而是自身 Stack 内的越界 Positioned（本框架
///     RenderStack 不裁剪，气泡可画出父边界——视觉效果与 overlay 一致）。

#include <memory>
#include <string>

#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/painter.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/layout.h"
#include "cui/framework/listener.h"
#include "cui/widgets/paint.h"
#include "cui/widgets/text.h"

namespace cui {

// 对齐 legacy（src/widgets/dialogs/tooltip.cpp）的几何常量。
constexpr double kTooltipBubbleH = 28.0;   // 气泡主体高度
constexpr double kTooltipTail = 6.0;       // callout 小三角长度
constexpr double kTooltipRadius = 4.0;     // 气泡圆角
constexpr double kTooltipFontSize = 12.0;  // 文字尺寸

/// 带 callout 箭头的提示气泡。背景用 CustomPaint 在真实尺寸上画圆角矩形 + 朝下
/// 小三角；文字用 `Text` 由自身宽度驱动气泡宽度（避免手算中文宽度溢出）。
class TooltipBubble : public StatelessWidget {
public:
    explicit TooltipBubble(std::string message, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), message_(std::move(message)) {}

    SharedPtr<Widget> build(BuildContext&) override {
        double h = kTooltipBubbleH, tail = kTooltipTail, r = kTooltipRadius;

        // 背景层：铺满 body（Positioned.fill），在真实 size 上画圆角矩形 + 底部居中三角。
        auto shape = cui::make_shared<CustomPaint>(
            [h, tail, r](Painter& p, const Size& size) {
                double w = size.width;
                p.setSourceColor(Color::fromRGB(0.2, 0.2, 0.2));
                p.skPaint().setAlpha(230);
                p.fillRoundedRect(Rect{0, 0, w, h}, r);
                double cx = w / 2.0;
                p.moveTo(cx - 5.0, h - 1.0);
                p.lineTo(cx, h + tail);
                p.lineTo(cx + 5.0, h - 1.0);
                p.fill();
                p.skPaint().setAlpha(255);
            });
        auto fill = cui::make_shared<Positioned>(shape);
        fill->hasLeft = fill->hasRight = fill->hasTop = fill->hasBottom = true;
        fill->left = fill->right = fill->top = fill->bottom = 0.0;

        // 文字层：白色 12px，水平内边距 10；固定高 28 —— 它决定 Stack 尺寸与气泡宽度。
        auto txt = cui::make_shared<Text>(
            message_, TextStyle().size(kTooltipFontSize).fgColor(Color::fromRGB(1, 1, 1)));
        auto body = cui::make_shared<Container>(cui::make_shared<Center>(txt));
        body->height(h);
        body->padding(EdgeInsets::symmetric(10.0, 0.0));

        std::vector<SharedPtr<Widget>> layers;
        layers.push_back(fill);
        layers.push_back(body);
        return cui::make_shared<Stack>(layers);
    }

private:
    std::string message_;
};

/// 提示气泡组件。上游的 `Tooltip`（tooltip.dart）。
class Tooltip : public StatefulWidget {
public:
    explicit Tooltip(std::string message, SharedPtr<Widget> child = nullptr,
                     double longPressMs = 500.0, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), message_(std::move(message)),
          child_(std::move(child)), longPressMs_(longPressMs) {}

    const std::string& message() const { return message_; }
    const SharedPtr<Widget>& child() const { return child_; }
    double longPressMs() const { return longPressMs_; }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "Tooltip"; }

private:
    std::string message_;
    SharedPtr<Widget> child_;
    double longPressMs_;
};

class TooltipState : public State {
public:
    bool visible() const { return visible_; }

    /// 立即显示气泡（hover / 长按到期时调用）。
    void showNow() {
        if (!visible_) {
            visible_ = true;
            setState();
        }
    }

    /// 隐藏气泡（抬起时调用）。
    void hideNow() {
        cancelPressTimer();
        if (visible_) {
            visible_ = false;
            setState();
        }
    }

    void initState() override {}

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        auto* self = this;
        auto* w = tooltip();

        std::vector<SharedPtr<Widget>> kids;
        kids.push_back(w->child());
        if (visible_) {
            auto bubble = cui::make_shared<TooltipBubble>(w->message());

            // 水平居中于子节点（Align topCenter），并整体推到子节点上方（三角尖端
            // 刚好落在子节点顶边）。RenderStack 不裁剪，三角可越界绘制。
            auto up = Transform::translate(
                Offset{0, -(kTooltipBubbleH + kTooltipTail)}, bubble);
            auto aligned =
                cui::make_shared<Align>(0.0, -1.0, up);
            auto fill = cui::make_shared<Positioned>(aligned);
            fill->hasLeft = fill->hasRight = fill->hasTop = fill->hasBottom = true;
            fill->left = fill->right = fill->top = fill->bottom = 0.0;
            kids.push_back(fill);
        }

        Listener::PointerCallback onDown = [self](const PointerEvent&) {
            self->startPressTimer();
        };
        Listener::PointerCallback onMove = [self](const PointerEvent& e) {
            (void)e;
            self->showNow();  // 触摸拖动同样视为查看提示
        };
        Listener::PointerCallback onHover = [self](const PointerEvent& e) {
            if (e.pointerKind == PointerDeviceKind::mouse)
                self->showNow();  // 鼠标悬停立即显示
        };
        Listener::PointerCallback onUp = [self](const PointerEvent&) {
            self->hideNow();
        };
        Listener::PointerCallback onExit = [self](const PointerEvent&) {
            self->hideNow();  // 鼠标移开控件即隐藏（框架派发的 Exit 事件）
        };

        return cui::make_shared<Listener>(onDown, onMove, onUp, onHover, nullptr,
                                          cui::make_shared<Stack>(std::move(kids)),
                                          nullptr, onExit);
    }

    // ── 长按计时（真实时间 Ticker）─────────────────────────────────────────
    void startPressTimer() {
        auto* self = this;
        double wait = tooltip()->longPressMs();
        if (!timer_)
            timer_ = std::make_unique<Ticker>([self, wait](Duration elapsed) {
                if (elapsed.count() >= wait) {
                    self->timer_->stop();
                    self->showNow();
                }
            });
        timer_->start();
    }

    void cancelPressTimer() {
        if (timer_ && timer_->isActive())
            timer_->stop();
    }

private:
    Tooltip* tooltip() { return static_cast<Tooltip*>(widget()); }

    bool visible_ = false;
    std::unique_ptr<Ticker> timer_;
};

inline std::unique_ptr<State> Tooltip::createState() {
    return std::make_unique<TooltipState>();
}

}  // namespace cui_core
