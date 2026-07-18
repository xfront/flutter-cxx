#pragma once
/// @file widgets/auto_hide_toolbar.h
/// @brief 自动隐藏工具栏（对齐 Microsoft Remote Desktop / TeamViewer 的连接栏行为）。
///
/// 来自 rui 的 `widgets/advanced/auto_hide_toolbar.rs`，但重建成 cui_core 的三层架构：
/// 本控件是**纯复合控件**（`ImplicitlyAnimatedWidget` 上的 build），全部行为由既有
/// 原语拼出，没有自定义 RenderObject：
///   - 展开/收缩补间：`ImplicitTweenDriver`（`tween("progress", …)`，逐帧重建，
///     与 `widgets/transition.h` 的口径一致）
///   - 悬停进入/离开：`MouseRegion`（enter/exit 是集合差分，见「鼠标悬停区域」）
///   - 离开后延迟收回：一条自驱动的 `Ticker`（= rui 的 hide_ticker）
///   - 逐帧显露：上游 `SizeTransition` 的同款树
///     `ClipRect → Align(width/heightFactor) → 内容`，内容始终按展开尺寸布局，
///     `Align` 自身按因子收缩，`ClipRect` 裁掉溢出。
///
/// 命中区跟着条带走靠的是**盒子尺寸闸门**（`RenderBox::hitTest` 第一步
/// `size.contains(position)`，与上游一致）：溢出到 `Align` 之外的按钮既看不见也
/// 点不到。本仓以前没有这道闸门（于是「裁剪不裁命中」），本控件那批改动补齐。
///
/// 收缩态另有 `kCollapsedTriggerMargin` 的隐形悬停容差：只有几像素的 peek 线很难
/// 精确命中。容差由外层 Stack 里一条**交叉轴 0 的占位子**撑出来（容器尺寸 = 热区，
/// 条带贴吸附边），并用 `MouseRegion(opaque=false)` ⇒ **不吃**身后页面的点击；
/// 热区恒 ≥ 容差 ⇒ 展开途中不会因盒子变小而误触 Exit。

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/framework.h"
#include "cui/platform/event.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/icons.h"
#include "cui/widgets/implicit_animation.h"
#include "cui/widgets/paint.h"

namespace cui {

/// 工具栏吸附方向。
enum class AutoHideDirection {
    Top,
    Bottom,
    Left,
    Right,
};

namespace auto_hide_detail {

/// 收缩时额外保留的可悬停触发容差（像素）。
inline constexpr double kCollapsedTriggerMargin = 16.0;
/// pin 按钮边长与距边内缩。边长要容得下一枚能认出来的图钉（16 像素画针脚会糊成一团）。
inline constexpr double kPinSize = 22.0;
inline constexpr double kPinInset = 4.0;
/// 图钉字形在按钮里的边长（四周留 3 像素呼吸）。
inline constexpr double kPinGlyphSize = 16.0;

/// 主轴（条带变短的那条轴）是否为竖直方向。
inline bool isVertical(AutoHideDirection d) {
    return d == AutoHideDirection::Top || d == AutoHideDirection::Bottom;
}

/// `Align` 的两个轴因子：主轴给逐帧显露的因子，交叉轴不给（撑满）。
inline std::optional<double> mainFactor(AutoHideDirection d, double factor) {
    return isVertical(d) ? std::optional<double>{factor} : std::nullopt;
}
inline std::optional<double> crossFactor(AutoHideDirection d, double factor) {
    return isVertical(d) ? std::nullopt : std::optional<double>{factor};
}

/// 贴吸附边的锚点（= 上游 SizeTransition 的 alignment）。
///
/// 同一个值用两处：条带内「内容哪一端保持可见」，以及可见条带在「条带 + 悬停
/// 容差」盒子里贴哪条边。
inline void edgeAlignment(AutoHideDirection d, double& x, double& y) {
    switch (d) {
        case AutoHideDirection::Top:
            x = -1.0; y = -1.0; break;
        case AutoHideDirection::Bottom:
            x = -1.0; y = 1.0; break;
        case AutoHideDirection::Left:
            x = -1.0; y = -1.0; break;
        case AutoHideDirection::Right:
            x = 1.0; y = -1.0; break;
    }
}

/// 可见条带在「条带 + 悬停容差」容器里的锚点。
inline RenderStack::Alignment stackAlignment(AutoHideDirection d) {
    switch (d) {
        case AutoHideDirection::Top:
            return RenderStack::Alignment::TopStart;
        case AutoHideDirection::Bottom:
            return RenderStack::Alignment::BottomStart;
        case AutoHideDirection::Left:
            return RenderStack::Alignment::TopStart;
        case AutoHideDirection::Right:
            return RenderStack::Alignment::TopEnd;
    }
    return RenderStack::Alignment::TopStart;
}

/// 内容始终按展开尺寸布局用的约束。
///
/// 注意是**紧**约束（min==max）：`RenderConstrainedBox::enforce` 会把紧值夹到父
/// 约束的 max 上，所以条带外面不能再套一层「主轴紧 = 热区尺寸」的盒子（那会把
/// max 压成十几个像素，内容被逐帧尺寸二次缩放 —— 本控件早期的双重收缩就是这么
/// 来的）。热区尺寸改由 `marginSpacer` 的占位子撑。
inline BoxConstraints expandedConstraints(AutoHideDirection d, double expanded) {
    return isVertical(d)
               ? BoxConstraints(0.0, BoxConstraints::kInfinity, expanded, expanded)
               : BoxConstraints(expanded, expanded, 0.0, BoxConstraints::kInfinity);
}

/// 把容器撑到「热区尺寸」的占位子（交叉轴 0，不吃命中）。
inline SharedPtr<Widget> marginSpacer(AutoHideDirection d, double hot) {
    return isVertical(d) ? cui::make_shared<SizedBox>(0.0, hot)
                         : cui::make_shared<SizedBox>(hot, 0.0);
}

/// pin 按钮的位置：主轴贴吸附边（保证收缩过程中仍可见），交叉轴贴起始边。
inline void applyPinPosition(Positioned& pos, AutoHideDirection d) {
    pos.hasTop = true;
    pos.top = kPinInset;
    pos.hasRight = true;
    pos.right = kPinInset;
    switch (d) {
        case AutoHideDirection::Bottom:
            pos.hasTop = false;
            pos.hasBottom = true;
            pos.bottom = kPinInset;
            break;
        case AutoHideDirection::Left:
            pos.hasRight = false;
            pos.hasLeft = true;
            pos.left = kPinInset;
            break;
        case AutoHideDirection::Top:
        case AutoHideDirection::Right:
            break;
    }
}

}  // namespace auto_hide_detail

/// 自动隐藏工具栏。
///
/// 参数（顺序即构造顺序，`key` 固定尾置）：
///   - `child`：条带内容（通常是一行按钮）。
///   - `direction`：吸附边。
///   - `expandedSize`：展开时主轴尺寸（rui 默认 40，这里对齐 story 页用的 56）。
///   - `peekSize`：收缩时保留的可见厚度。
///   - `hideDelayMs`：鼠标离开后多久自动收回。
///   - `pinnable`：是否画 pin 按钮（点击可钉住/取消钉住）。
///   - `background`：条带底色（本仓没有运行时 theme，由调用方给）。
///   - `onPinChanged`：钉住状态变化回调。
///   - `durationMs` / `curve`：展开/收缩补间。
class AutoHideToolbar : public ImplicitlyAnimatedWidget {
public:
    using PinCallback = std::function<void(bool)>;

    explicit AutoHideToolbar(SharedPtr<Widget> child = nullptr,
                             AutoHideDirection direction = AutoHideDirection::Top,
                             double expandedSize = 56.0, double peekSize = 2.0,
                             double hideDelayMs = 2000.0, bool pinnable = true,
                             Color background = theme().surface,
                             PinCallback onPinChanged = {}, double durationMs = 300.0,
                             Easing curve = Easing::EaseOutCubic, KeyPtr key = nullptr)
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          child_(std::move(child)), direction_(direction), expandedSize_(expandedSize),
          peekSize_(peekSize), hideDelayMs_(hideDelayMs), pinnable_(pinnable),
          background_(background), onPinChanged_(std::move(onPinChanged)) {}

    const SharedPtr<Widget>& child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    AutoHideDirection direction() const { return direction_; }
    void setDirection(AutoHideDirection d) { direction_ = d; }

    double expandedSize() const { return expandedSize_; }
    double peekSize() const { return peekSize_; }
    double hideDelayMs() const { return hideDelayMs_; }
    bool pinnable() const { return pinnable_; }
    const Color& background() const { return background_; }
    const PinCallback& onPinChanged() const { return onPinChanged_; }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "AutoHideToolbar"; }

private:
    SharedPtr<Widget> child_;
    AutoHideDirection direction_ = AutoHideDirection::Top;
    double expandedSize_ = 56.0;
    double peekSize_ = 2.0;
    double hideDelayMs_ = 2000.0;
    bool pinnable_ = true;
    Color background_ = theme().surface;
    PinCallback onPinChanged_;
};

/// 工具栏状态：`hovering_`（鼠标在热区）/ `holding_`（离开后延迟未到期）/
/// `pinned_` 三个布尔压成一条 progress 补间。
class AutoHideToolbarState : public ImplicitlyAnimatedWidgetState {
public:
    void dispose() override {
        stopHideTimer();
        ImplicitlyAnimatedWidgetState::dispose();
    }

    bool isPinned() const { return pinned_; }
    bool isHovering() const { return hovering_; }
    bool isHolding() const { return holding_; }

    /// 当前补间进度（0 = 完全收缩，1 = 完全展开）；测试与页面用它判定状态。
    double progress() const { return tweenValue("progress"); }

    void setPinned(bool pinned) {
        if (pinned_ == pinned)
            return;
        pinned_ = pinned;
        if (pinned) {
            // 钉住：取消未到期的隐藏定时，立即展开。
            holding_ = false;
            stopHideTimer();
        } else if (!hovering_) {
            // 取消钉住且鼠标已不在条带上：先进入按住态，重新计时收回。
            holding_ = true;
            armHideTimer();
        }
        if (auto* w = toolbar(); w && w->onPinChanged())
            w->onPinChanged()(pinned);
        if (mounted())
            setState();
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        using namespace auto_hide_detail;
        auto* w = toolbar();
        const AutoHideDirection dir = w->direction();

        // 悬停、钉住、或「离开后延迟未到期」都算展开态。tween() 只在目标真的变化
        // 时才重启补间，所以已展开时鼠标在热区边缘抖动不会把进度清零（rui 的同名
        // 回归用例 reenter_does_not_reset_expanded_animation）。
        const bool open = pinned_ || hovering_ || holding_;
        const double p = tween("progress", open ? 1.0 : 0.0);

        const double expanded = std::max(0.0, w->expandedSize());
        const double peek = std::max(0.0, w->peekSize());
        const double visible = peek + (expanded - peek) * p;
        const double factor = expanded > 0.0 ? visible / expanded : 0.0;
        // 热区恒 ≥ 容差：收缩过程中盒子变小不会把仍停在边缘的鼠标「挤出去」。
        const double hot = std::max(visible, kCollapsedTriggerMargin);

        double ax = 0.0, ay = 0.0;
        edgeAlignment(dir, ax, ay);

        // 内容始终按展开尺寸布局 ⇒ 条带逐帧变高不引起按钮重排；Align 按因子收缩，
        // 溢出部分既不绘制（ClipRect）也不命中（Align 的尺寸闸门）。
        auto content = cui::make_shared<ConstrainedBox>(
            expandedConstraints(dir, expanded), w->child());
        auto reveal = cui::make_shared<Align>(ax, ay, std::move(content), nullptr,
                                             crossFactor(dir, factor),
                                             mainFactor(dir, factor));

        std::vector<SharedPtr<Widget>> layers;
        layers.push_back(cui::make_shared<DecoratedBox>(BoxDecoration{w->background()},
                                                       std::move(reveal)));
        if (w->pinnable()) {
            // 图钉字形（= rui 用的 Material `push_pin`）：钉住 = 白针插在一块 primary
            // 底上，未钉 = 无底、用正文色 ⇒ 一眼看得出当前是哪一档。
            // 这里**不能**用 CustomPaint：自绘盒是 repaint boundary，绘制录进独立图层，
            // 会躲过条带外面那圈 ClipRect（收缩态只剩 2 像素，图钉却整个浮在页面上）。
            auto glyph = cui::make_shared<Icon>(
                IconCode::PushPin, kPinGlyphSize,
                pinned_ ? theme().textOnPrimary : theme().textPrimary);
            auto plate = cui::make_shared<Container>(std::move(glyph));
            plate->width(kPinSize).height(kPinSize)
                  .alignment(0.0, 0.0)
                  .decoration(BoxDecoration{pinned_ ? theme().primary
                                                    : Color(0.0, 0.0, 0.0, 0.0),
                                              3.0});
            auto pos = cui::make_shared<Positioned>(
                cui::make_shared<GestureDetector>(std::move(plate),
                                                  [this] { setPinned(!pinned_); }));
            applyPinPosition(*pos, dir);
            layers.push_back(std::move(pos));
        }

        // 条带：底色 + pin，整体裁到逐帧可见的那一条。
        auto band = cui::make_shared<Stack>(std::move(layers));
        auto clip = cui::make_shared<ClipRect>(std::move(band));
        // 热区：条带 + 零宽的容差占位；占位只把容器撑大，交叉轴 0 ⇒ 不吃命中。
        std::vector<SharedPtr<Widget>> area;
        area.push_back(std::move(clip));
        area.push_back(marginSpacer(dir, hot));
        auto region = cui::make_shared<Stack>(std::move(area), nullptr,
                                             stackAlignment(dir));
        // opaque=false：容差那十几个像素不吃掉身后页面的点击。
        return cui::make_shared<MouseRegion>(
            std::move(region),
            [this](const PointerEvent&) { onEnter(); },
            [this](const PointerEvent&) { onExit(); }, nullptr, false);
    }

private:
    AutoHideToolbar* toolbar() { return static_cast<AutoHideToolbar*>(widget()); }

    void onEnter() {
        hovering_ = true;
        holding_ = false;
        stopHideTimer();
        if (mounted())
            setState();
    }

    void onExit() {
        // 收缩途中热区变小导致的 Exit（此时 hovering_ 已是 false）忽略：目标已经
        // 是 0，再挂一次延迟反而会把条带重新撑开。
        if (!hovering_)
            return;
        hovering_ = false;
        if (!pinned_) {
            holding_ = true;
            armHideTimer();
        }
        if (mounted())
            setState();
    }

    /// 武装自动隐藏定时：离开后 hideDelayMs 到期且仍未悬停/钉住才收回。
    ///
    /// 自驱动（每帧检查 elapsed），不依赖 hover 事件转发 —— rui 那边同款：框架的
    /// hover 兜底会被窗口级控件抢占，依赖转发的自动隐藏在含菜单栏的窗口里永不触发。
    void armHideTimer() {
        if (!hideTicker_) {
            hideTicker_ = std::make_unique<Ticker>([this](Duration elapsed) {
                const auto* w = toolbar();
                const double delay = w ? w->hideDelayMs() : 0.0;
                if (elapsed.count() < delay)
                    return;
                stopHideTimer();
                if (hovering_ || pinned_)
                    return;
                holding_ = false;
                if (mounted())
                    setState();
            });
        }
        hideTicker_->start();
    }

    void stopHideTimer() {
        if (hideTicker_ && hideTicker_->isActive())
            hideTicker_->stop();
    }

    bool hovering_ = false;
    bool holding_ = false;
    bool pinned_ = false;
    std::unique_ptr<Ticker> hideTicker_;
};

inline std::unique_ptr<State> AutoHideToolbar::createState() {
    return std::make_unique<AutoHideToolbarState>();
}

}  // namespace cui_core
