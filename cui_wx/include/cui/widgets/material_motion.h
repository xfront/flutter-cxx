#pragma once
/// @file widgets/material_motion.h
/// @brief Material Motion 转场族 —— 对齐 package:animations，并补上 rui story
///        「Material Motion」页（11–15 节）用的行内 OpenContainer。
///
/// 上游对应关系（flutter/packages/animations/lib/src）：
///   FadeScaleTransition        fade_scale_transition.dart（147 行）
///   FadeThroughTransition      fade_through_transition.dart（285 行）
///   SharedAxisTransition       shared_axis_transition.dart（453 行）
///   PageTransitionSwitcher     page_transition_switcher.dart（416 行）
///   OpenContainer              open_container.dart（946 行，**仅行内形态**）
///
/// 前四个逐行对齐上游；它们共同依赖 Flutter core 的
/// `DualTransitionBuilder`（widgets/dual_transition_builder.dart，已补进
/// transition.h）。
///
/// 与上游的差异：
///   1. **没有 ThemeData**：上游 `fillColor` 默认取 `ThemeData.canvasColor`，
///      本仓取全局 `theme().surface`（浅色近白、深色为暗灰）。
///   2. **OpenContainer 只移植行内形态**：上游它是一条自定义 `ModalRoute`
///      （`_OpenContainerRoute` + 测量 closed 矩形 + 容器 morph），本仓库
///      没有 showModal/ModalConfiguration 这条路，故按 rui story 的语义实现
///      「原地交叉淡入 + 背景色/圆角补间 + toggle()」；不 push 路由、不接管
///      Navigator。尺寸取 closed/open 两者的较大者（本仓无「先测再插值尺寸」
///      的布局路径），story 的两态尺寸接近，视觉等价。
///   3. PageTransitionSwitcher 不做 `TickerMode` 静音联动（条目 ticker 由
///      本 State 直接持有，与 AnimatedSwitcher 的既有口径一致）。

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/key.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/framework.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/transition.h"

namespace cui {
namespace material_motion {

// ── 曲线 ────────────────────────────────────────────────────────────────────

/// 上游 `Cubic` 的最小实现：给定 x ∈ [0,1]，先解 x(t) 的 Bezier 参数，
/// 再求 y(t)。二分法 60 轮，误差远小于 1e-9。
inline double cubicTransform(double x1, double y1, double x2, double y2, double x) {
    if (x <= 0.0)
        return 0.0;
    if (x >= 1.0)
        return 1.0;
    auto eval = [](double a1, double a2, double t) {
        const double u = 1.0 - t;
        return 3.0 * u * u * t * a1 + 3.0 * u * t * t * a2 + t * t * t;
    };
    double lo = 0.0;
    double hi = 1.0;
    double t = x;
    for (int i = 0; i < 60; ++i) {
        t = 0.5 * (lo + hi);
        const double estimate = eval(x1, x2, t);
        if (std::abs(estimate - x) < 1e-9)
            break;
        if (estimate < x)
            lo = t;
        else
            hi = t;
    }
    return eval(y1, y2, t);
}

/// 上游 `Easing.legacy`（`Cubic(0.4, 0.0, 0.2, 1.0)`，Material 标准曲线）。
inline CurveFn legacy() {
    return [](double t) { return cubicTransform(0.4, 0.0, 0.2, 1.0, t); };
}

/// 上游 `Easing.legacyDecelerate`（`Cubic(0.0, 0.0, 0.2, 1.0)`）。
inline CurveFn legacyDecelerate() {
    return [](double t) { return cubicTransform(0.0, 0.0, 0.2, 1.0, t); };
}

/// 上游 `Easing.legacyAccelerate`（`Cubic(0.4, 0.0, 1.0, 1.0)`）。
inline CurveFn legacyAccelerate() {
    return [](double t) { return cubicTransform(0.4, 0.0, 1.0, 1.0, t); };
}

/// 上游 `Interval(begin, end).transform(t)`（无内层曲线）。
inline double intervalValue(double begin, double end, double t) {
    if (end <= begin)
        return 0.0;
    if (t <= begin)
        return 0.0;
    if (t >= end)
        return 1.0;
    return (t - begin) / (end - begin);
}

/// 把 `CurveFn` 包成随父动画逐帧求值的 `Animation<double>`。
inline SharedPtr<Animation<double>> curved(const SharedPtr<Animation<double>>& parent,
                                           CurveFn curve) {
    return cui::make_shared<CurvedAnimation>(parent, std::move(curve));
}

inline Color lerpColor(const Color& a, const Color& b, double t) {
    return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                 a.a + (b.a - a.a) * t};
}

// ── FadeThrough 的帧函数 ────────────────────────────────────────────────────
//
// 上游用两条 TweenSequence（权重 6/20 + 14/20）：前 30% 保持不变，后 70%
// 沿 decelerate / accelerate 过渡。这里直接写成帧函数，避免 TweenSequence
// 只接受 Easing 枚举、装不下 exact cubic。

inline double fadeThroughInOpacity(double t) {
    if (t <= 0.3)
        return 0.0;
    return cubicTransform(0.0, 0.0, 0.2, 1.0, (t - 0.3) / 0.7);
}

inline double fadeThroughInScale(double t) {
    if (t <= 0.3)
        return 0.92;
    return 0.92 + 0.08 * cubicTransform(0.0, 0.0, 0.2, 1.0, (t - 0.3) / 0.7);
}

inline double fadeThroughOutOpacity(double t) {
    if (t >= 0.3)
        return 0.0;
    return 1.0 - cubicTransform(0.4, 0.0, 1.0, 1.0, t / 0.3);
}

/// `fadeThroughInScale` 的 [0,1] 形态。上游那条 TweenSequence 里缩放是
/// `Tween(0.92, 1.00).chain(CurveTween(decelerate))`——曲线先求值、Tween 后
/// 映射，所以这里把 0.92 的常段剥掉，交给 `AnimatedWidget::range`。
inline double fadeThroughInScaleCurve(double t) {
    if (t <= 0.3)
        return 0.0;
    return cubicTransform(0.0, 0.0, 0.2, 1.0, (t - 0.3) / 0.7);
}

/// `fadeThroughOutOpacity` 的 [0,1] 形态：上游是
/// `Tween(1.0, 0.0).chain(CurveTween(accelerate))`，同理由 range 取反。
inline double fadeThroughOutCurve(double t) {
    return cubicTransform(0.4, 0.0, 1.0, 1.0, intervalValue(0.0, 0.3, t));
}

}  // namespace material_motion

// ── FadeScaleTransition ─────────────────────────────────────────────────────

/// Material Fade Scale 转场。上游 animations 包 `fade_scale_transition.dart`。
///
/// 入场：快速淡入 + 从 80% 缩放回 100%；退场：只淡出（缩放只强调新内容）。
class FadeScaleTransition : public StatelessWidget {
public:
    FadeScaleTransition(SharedPtr<Animation<double>> animation,
                        SharedPtr<Widget> child = nullptr, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), animation_(std::move(animation)),
          child_(std::move(child)) {}

    const SharedPtr<Animation<double>>& animation() const { return animation_; }
    const SharedPtr<Widget>& child() const { return child_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        return cui::make_shared<DualTransitionBuilder>(
                animation_,
                [](BuildContext&, SharedPtr<Animation<double>> anim,
                   SharedPtr<Widget> child) -> SharedPtr<Widget> {
                    // 上游：CurveTween(Interval(0.0, 0.3)) 与
                    // Tween(0.80, 1.00).chain(CurveTween(legacyDecelerate))。
                    // 曲线必须是 0→1 形态，端点保护才不会把 0.80 抹成 0。
                    auto scaleAnim = material_motion::curved(anim, material_motion::legacyDecelerate());
                    auto scale = cui::make_shared<ScaleTransition>(scaleAnim, std::move(child));
                    scale->setRange(Tween<double>(0.80, 1.00));
                    auto fadeAnim = material_motion::curved(anim, [](double t) {
                        return material_motion::intervalValue(0.0, 0.3, t);
                    });
                    return cui::make_shared<FadeTransition>(fadeAnim, scale);
                },
                [](BuildContext&, SharedPtr<Animation<double>> anim,
                   SharedPtr<Widget> child) -> SharedPtr<Widget> {
                    // 上游：Tween(1.0, 0.0)（线性映射，不套曲线）。
                    auto fade = cui::make_shared<FadeTransition>(anim, std::move(child));
                    fade->setRange(Tween<double>(1.0, 0.0));
                    return fade;
                },
                child_);
    }

    std::string toStringShort() const override { return "FadeScaleTransition"; }

private:
    SharedPtr<Animation<double>> animation_;
    SharedPtr<Widget> child_;
};

// ── FadeThroughTransition ───────────────────────────────────────────────────

/// Material Fade Through 转场。上游 `fade_through_transition.dart`。
///
/// 旧内容先淡出、再淡入新内容（两段在中点交叉）。`animation` 驱动新内容，
/// `secondaryAnimation` 驱动被新内容压住的旧内容（反向包一层
/// `ReverseAnimation`，与 PageTransitionSwitcher 的契约一致）。
class FadeThroughTransition : public StatelessWidget {
public:
    FadeThroughTransition(SharedPtr<Animation<double>> animation,
                          SharedPtr<Animation<double>> secondaryAnimation,
                          SharedPtr<Widget> child = nullptr,
                          std::optional<Color> fillColor = std::nullopt,
                          KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), animation_(std::move(animation)),
          secondaryAnimation_(std::move(secondaryAnimation)),
          child_(std::move(child)), fillColor_(fillColor) {}

    const SharedPtr<Animation<double>>& animation() const { return animation_; }
    const SharedPtr<Animation<double>>& secondaryAnimation() const {
        return secondaryAnimation_;
    }
    const SharedPtr<Widget>& child() const { return child_; }
    const std::optional<Color>& fillColor() const { return fillColor_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        // 上游：_ZoomedFadeInFadeOut(animation: animation, child: ColoredBox(
        //   color: fillColor ?? canvasColor,
        //   child: _ZoomedFadeInFadeOut(animation: ReverseAnimation(secondaryAnimation), child)))
        // 本仓 ColoredBox 是叶子（无 child），带子的纯色底用 DecoratedBox。
        SharedPtr<Widget> inner = zoomedFadeInFadeOut(
                secondaryAnimation_ ? cui::make_shared<ReverseAnimation>(secondaryAnimation_)
                                    : secondaryAnimation_,
                child_);
        if (fillColor_)
            inner = cui::make_shared<DecoratedBox>(BoxDecoration{*fillColor_}, inner);
        return zoomedFadeInFadeOut(animation_, inner);
    }

    std::string toStringShort() const override { return "FadeThroughTransition"; }

private:
    /// 上游的 `_ZoomedFadeInFadeOut`：正向走 `_ZoomedFadeIn`，反向走 `_FadeOut`。
    static SharedPtr<Widget> zoomedFadeInFadeOut(SharedPtr<Animation<double>> animation,
                                                 SharedPtr<Widget> child) {
        return cui::make_shared<DualTransitionBuilder>(
                animation,
                [](BuildContext&, SharedPtr<Animation<double>> anim,
                   SharedPtr<Widget> ch) -> SharedPtr<Widget> {
                    // 上游 `_scaleIn`：0.92 常段 + Tween(0.92,1.00) 链 decelerate。
                    auto scaleAnim = material_motion::curved(
                            anim, [](double t) { return material_motion::fadeThroughInScaleCurve(t); });
                    auto scale = cui::make_shared<ScaleTransition>(scaleAnim, std::move(ch));
                    scale->setRange(Tween<double>(0.92, 1.00));
                    auto fadeAnim = material_motion::curved(
                            anim, [](double t) { return material_motion::fadeThroughInOpacity(t); });
                    return cui::make_shared<FadeTransition>(fadeAnim, scale);
                },
                [](BuildContext&, SharedPtr<Animation<double>> anim,
                   SharedPtr<Widget> ch) -> SharedPtr<Widget> {
                    // 上游 `_fadeOutOpacity`：Tween(1.0,0.0) 链 accelerate（前 30%）。
                    auto fadeAnim = material_motion::curved(
                            anim, [](double t) { return material_motion::fadeThroughOutCurve(t); });
                    auto fade = cui::make_shared<FadeTransition>(fadeAnim, std::move(ch));
                    fade->setRange(Tween<double>(1.0, 0.0));
                    return fade;
                },
                child);
    }

    SharedPtr<Animation<double>> animation_;
    SharedPtr<Animation<double>> secondaryAnimation_;
    SharedPtr<Widget> child_;
    std::optional<Color> fillColor_;
};

// ── SharedAxisTransition ────────────────────────────────────────────────────

/// 共享轴类型。上游 `SharedAxisTransitionType`。
enum class SharedAxisTransitionType {
    Vertical,    ///< 垂直滑动
    Horizontal,  ///< 水平滑动
    Scaled,      ///< 缩放 + 淡入淡出（父子导航）
};

/// Material Shared Axis 转场。上游 `shared_axis_transition.dart`。
class SharedAxisTransition : public StatelessWidget {
public:
    SharedAxisTransition(SharedPtr<Animation<double>> animation,
                         SharedPtr<Animation<double>> secondaryAnimation,
                         SharedAxisTransitionType transitionType,
                         SharedPtr<Widget> child = nullptr,
                         std::optional<Color> fillColor = std::nullopt,
                         KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), animation_(std::move(animation)),
          secondaryAnimation_(std::move(secondaryAnimation)),
          transitionType_(transitionType), child_(std::move(child)), fillColor_(fillColor) {}

    const SharedPtr<Animation<double>>& animation() const { return animation_; }
    const SharedPtr<Animation<double>>& secondaryAnimation() const {
        return secondaryAnimation_;
    }
    SharedAxisTransitionType transitionType() const { return transitionType_; }
    const SharedPtr<Widget>& child() const { return child_; }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        const Color color = fillColor_.value_or(theme().surface);
        const SharedAxisTransitionType type = transitionType_;
        // 内层：旧内容由 secondaryAnimation 驱动（取反后当正向动画用）。
        SharedPtr<Widget> inner = cui::make_shared<DualTransitionBuilder>(
                secondaryAnimation_
                        ? cui::make_shared<ReverseAnimation>(secondaryAnimation_)
                        : secondaryAnimation_,
                [type](BuildContext&, SharedPtr<Animation<double>> anim,
                       SharedPtr<Widget> ch) -> SharedPtr<Widget> {
                    return enter(anim, type, true, std::move(ch));
                },
                [type, color](BuildContext&, SharedPtr<Animation<double>> anim,
                              SharedPtr<Widget> ch) -> SharedPtr<Widget> {
                    return exit(anim, type, false, color, std::move(ch));
                },
                child_);
        // 外层：新内容由 animation 驱动。
        return cui::make_shared<DualTransitionBuilder>(
                animation_,
                [type](BuildContext&, SharedPtr<Animation<double>> anim,
                       SharedPtr<Widget> ch) -> SharedPtr<Widget> {
                    return enter(anim, type, false, std::move(ch));
                },
                [type, color](BuildContext&, SharedPtr<Animation<double>> anim,
                              SharedPtr<Widget> ch) -> SharedPtr<Widget> {
                    return exit(anim, type, true, color, std::move(ch));
                },
                inner);
    }

    std::string toStringShort() const override { return "SharedAxisTransition"; }

private:
    /// 上游 `_EnterTransition`。
    static SharedPtr<Widget> enter(SharedPtr<Animation<double>> anim,
                                   SharedAxisTransitionType type, bool reverse,
                                   SharedPtr<Widget> child) {
        // 上游 _fadeInTransition = CurveTween(decelerate).chain(Interval(0.3, 1.0))
        auto fadeAnim = material_motion::curved(anim, [](double t) {
            return material_motion::legacyDecelerate()(
                    material_motion::intervalValue(0.3, 1.0, t));
        });
        switch (type) {
            case SharedAxisTransitionType::Horizontal:
            case SharedAxisTransitionType::Vertical: {
                const bool horizontal = type == SharedAxisTransitionType::Horizontal;
                const double begin = reverse ? -30.0 : 30.0;
                auto slideAnim = material_motion::curved(anim, material_motion::legacy());
                auto slide = cui::make_shared<MatrixTransition>(
                        slideAnim,
                        [horizontal, begin](double v) {
                            const double d = begin * (1.0 - v);
                            return horizontal ? Matrix4::translation(d, 0.0)
                                              : Matrix4::translation(0.0, d);
                        });
                auto fade = cui::make_shared<FadeTransition>(fadeAnim, slide);
                slide->setChild(std::move(child));
                return fade;
            }
            case SharedAxisTransitionType::Scaled: {
                // 上游：reverse ? Tween(1.10→1.00) : Tween(0.80→1.00)，链 legacy
                const double begin = reverse ? 1.10 : 0.80;
                auto scaleAnim = material_motion::curved(anim, material_motion::legacy());
                auto scale = cui::make_shared<ScaleTransition>(scaleAnim, std::move(child));
                scale->setRange(Tween<double>(begin, 1.00));
                return cui::make_shared<FadeTransition>(fadeAnim, scale);
            }
        }
        return child;
    }

    /// 上游 `_ExitTransition`（`_FlippedCurveTween(accelerate).chain(Interval(0, 0.3))`）。
    static SharedPtr<Widget> exit(SharedPtr<Animation<double>> anim,
                                  SharedAxisTransitionType type, bool reverse, Color fillColor,
                                  SharedPtr<Widget> child) {
        // 上游 `_FlippedCurveTween(accelerate)`：曲线 0→1，再由 range 取反。
        auto makeFade = [&anim](SharedPtr<Widget> c) {
            auto fadeAnim = material_motion::curved(anim, [](double t) {
                return material_motion::legacyAccelerate()(
                        material_motion::intervalValue(0.0, 0.3, t));
            });
            auto fade = cui::make_shared<FadeTransition>(fadeAnim, std::move(c));
            fade->setRange(Tween<double>(1.0, 0.0));
            return fade;
        };
        switch (type) {
            case SharedAxisTransitionType::Horizontal:
            case SharedAxisTransitionType::Vertical: {
                const bool horizontal = type == SharedAxisTransitionType::Horizontal;
                const double end = reverse ? 30.0 : -30.0;
                auto slideAnim = material_motion::curved(anim, material_motion::legacy());
                auto slide = cui::make_shared<MatrixTransition>(
                        slideAnim,
                        [horizontal, end](double v) {
                            const double d = end * v;
                            return horizontal ? Matrix4::translation(d, 0.0)
                                              : Matrix4::translation(0.0, d);
                        });
                slide->setChild(std::move(child));
                auto box = cui::make_shared<DecoratedBox>(BoxDecoration{fillColor}, slide);
                return makeFade(box);
            }
            case SharedAxisTransitionType::Scaled: {
                // 上游：reverse ? Tween(1.00→0.80) : Tween(1.00→1.10)
                const double end = reverse ? 0.80 : 1.10;
                auto scaleAnim = material_motion::curved(anim, material_motion::legacy());
                auto scale = cui::make_shared<ScaleTransition>(scaleAnim, std::move(child));
                scale->setRange(Tween<double>(1.00, end));
                auto box = cui::make_shared<DecoratedBox>(BoxDecoration{fillColor}, scale);
                return makeFade(box);
            }
        }
        return child;
    }

    SharedPtr<Animation<double>> animation_;
    SharedPtr<Animation<double>> secondaryAnimation_;
    SharedAxisTransitionType transitionType_;
    SharedPtr<Widget> child_;
    std::optional<Color> fillColor_;
};

// ── PageTransitionSwitcher ──────────────────────────────────────────────────

/// 换子时的转场构建器。上游 `PageTransitionSwitcherTransitionBuilder`。
using PageTransitionSwitcherTransitionBuilder = std::function<SharedPtr<Widget>(
    SharedPtr<Widget> child, SharedPtr<Animation<double>> primaryAnimation,
    SharedPtr<Animation<double>> secondaryAnimation)>;

/// 摆放所有条目的布局构建器。上游 `PageTransitionSwitcherLayoutBuilder`。
using PageTransitionSwitcherLayoutBuilder =
        std::function<SharedPtr<Widget>(const std::vector<SharedPtr<Widget>>& entries)>;

class PageTransitionSwitcherState;

/// 按 PageRoute 的「入场 / 退场」双动画切换子控件。上游
/// `page_transition_switcher.dart`。
///
/// 每个子一份 `_ChildEntry`（primary / secondary 两个 AnimationController +
/// 各自的 Ticker）。新子 primary 正放、旧子 secondary 正放（reverse=true 时
/// 新子 secondary 反放、旧子 primary 反放），转场完成后条目自动摘除。
class PageTransitionSwitcher : public StatefulWidget {
public:
    PageTransitionSwitcher(SharedPtr<Widget> child,
                           PageTransitionSwitcherTransitionBuilder transitionBuilder,
                           double durationMs = 300.0, bool reverse = false,
                           PageTransitionSwitcherLayoutBuilder layoutBuilder = nullptr,
                           KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)),
          transitionBuilder_(std::move(transitionBuilder)), durationMs_(durationMs),
          reverse_(reverse), layoutBuilder_(std::move(layoutBuilder)) {}

    const SharedPtr<Widget>& child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    double duration() const { return durationMs_; }
    void setDuration(double ms) { durationMs_ = ms; }

    bool reverse() const { return reverse_; }
    void setReverse(bool v) { reverse_ = v; }

    const PageTransitionSwitcherTransitionBuilder& transitionBuilder() const {
        return transitionBuilder_;
    }
    const PageTransitionSwitcherLayoutBuilder& layoutBuilder() const { return layoutBuilder_; }

    /// 上游 `PageTransitionSwitcher.defaultLayoutBuilder`：Stack 居中。
    static SharedPtr<Widget> defaultLayoutBuilder(
            const std::vector<SharedPtr<Widget>>& entries) {
        return cui::make_shared<Stack>(entries, nullptr, RenderStack::Alignment::MiddleCenter,
                                       RenderStack::Fit::Loose);
    }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Widget> child_;
    PageTransitionSwitcherTransitionBuilder transitionBuilder_;
    double durationMs_ = 300.0;
    bool reverse_ = false;
    PageTransitionSwitcherLayoutBuilder layoutBuilder_;
};

class PageTransitionSwitcherState : public State {
public:
    void initState() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    /// 上游的 `_ChildEntry`。
    struct Entry {
        SharedPtr<Widget> widgetChild;
        SharedPtr<Widget> transition;  ///< KeyedSubtree(number)
        SharedPtr<AnimationController> primary;
        SharedPtr<AnimationController> secondary;
        std::unique_ptr<Ticker> primaryTicker;
        std::unique_ptr<Ticker> secondaryTicker;
        int number = 0;
        /// 由 controller 的状态监听置位（primary dismissed / secondary
        /// completed），下一次 build 摘除。**不在回调里直接删**：那时正在
        /// 该条目的 tick 里，销毁会踩到自己的调用栈。
        bool settled = false;
    };

    /// 上游的 `_addEntryForNewChild`。
    void addEntryForNewChild(bool shouldAnimate);
    /// 上游的 `_updateTransitionForEntry`：builder/child 变了重建同一 key 的转场。
    void updateTransitionForEntry(const SharedPtr<Entry>& entry);
    SharedPtr<Widget> buildTransition(const SharedPtr<Entry>& entry);
    std::unique_ptr<Ticker> makeEntryTicker(const SharedPtr<Entry>& entry, bool primary);
    void startTickerFor(const SharedPtr<Entry>& entry, bool primary);
    void pruneSettled();

    std::vector<SharedPtr<Entry>> activeEntries_;
    SharedPtr<Entry> currentEntry_;
    int childNumber_ = 0;
};

inline std::unique_ptr<State> PageTransitionSwitcher::createState() {
    return std::make_unique<PageTransitionSwitcherState>();
}

inline std::unique_ptr<Ticker> PageTransitionSwitcherState::makeEntryTicker(
        const SharedPtr<Entry>& entry, bool primary) {
    // 捕获裸 Entry*：entry 拥有 ticker，析构时 ticker 先死，指针不会悬挂。
    Entry* raw = entry.get();
    return std::make_unique<Ticker>([raw, primary](Duration elapsed) {
        AnimationController* c = primary ? raw->primary.get() : raw->secondary.get();
        if (!c)
            return;
        c->tick(elapsed);
        auto& ticker = primary ? raw->primaryTicker : raw->secondaryTicker;
        if (!c->isRunning() && ticker && ticker->isActive())
            ticker->stop();
    });
}

inline void PageTransitionSwitcherState::startTickerFor(const SharedPtr<Entry>& entry,
                                                        bool primary) {
    auto& ticker = primary ? entry->primaryTicker : entry->secondaryTicker;
    if (ticker && !ticker->isActive())
        ticker->start();
}

inline void PageTransitionSwitcherState::initState() {
    addEntryForNewChild(false);
}

inline void PageTransitionSwitcherState::dispose() {
    for (auto& e : activeEntries_) {
        if (e->primaryTicker)
            e->primaryTicker->stop();
        if (e->secondaryTicker)
            e->secondaryTicker->stop();
    }
    activeEntries_.clear();
    currentEntry_.reset();
}

inline SharedPtr<Widget> PageTransitionSwitcherState::buildTransition(
        const SharedPtr<Entry>& entry) {
    auto* w = static_cast<PageTransitionSwitcher*>(widget());
    SharedPtr<Widget> built;
    if (w && w->transitionBuilder())
        built = w->transitionBuilder()(entry->widgetChild, entry->primary, entry->secondary);
    else
        built = entry->widgetChild;
    return cui::make_shared<KeyedSubtree>(
            cui::make_shared<const ValueKey<int>>(entry->number), std::move(built));
}

inline void PageTransitionSwitcherState::updateTransitionForEntry(
        const SharedPtr<Entry>& entry) {
    entry->transition = buildTransition(entry);
}

inline void PageTransitionSwitcherState::addEntryForNewChild(bool shouldAnimate) {
    auto* w = static_cast<PageTransitionSwitcher*>(widget());
    if (!w)
        return;

    if (currentEntry_) {
        // 上游：reverse ? primaryController.reverse() : secondaryController.forward()
        if (w->reverse()) {
            currentEntry_->primary->reverse();
            startTickerFor(currentEntry_, true);
        } else {
            currentEntry_->secondary->forward();
            startTickerFor(currentEntry_, false);
        }
        currentEntry_.reset();
    }
    if (!w->child())
        return;

    auto entry = cui::make_shared<Entry>();
    entry->widgetChild = w->child();
    entry->number = childNumber_;
    entry->primary = cui::make_shared<AnimationController>(w->duration());
    entry->secondary = cui::make_shared<AnimationController>(w->duration());
    entry->primary->setEasing(Easing::Linear);
    entry->secondary->setEasing(Easing::Linear);
    entry->primaryTicker = makeEntryTicker(entry, true);
    entry->secondaryTicker = makeEntryTicker(entry, false);
    if (!shouldAnimate) {
        // 上游 initState 分支：首个子直接到位，不做入场动画。
        entry->primary->setValue(1.0);
    } else if (w->reverse()) {
        // 上游：primary = 1，secondary = 1 后反放。
        entry->primary->setValue(1.0);
        entry->secondary->setValue(1.0);
    }

    if (shouldAnimate) {
        if (w->reverse()) {
            entry->secondary->reverse();
            startTickerFor(entry, false);
        } else {
            entry->primary->forward();
            startTickerFor(entry, true);
        }
    }

    entry->transition = buildTransition(entry);

    // 状态监听必须在初始值/起播之后挂：reverse 分支的 setValue(1.0) 会派发
    // Completed，提前挂上去会把刚建好的条目当场标记为可摘除。
    entry->primary->addStatusListener([this, entry](AnimationStatus status) {
        if (status == AnimationStatus::Dismissed) {
            entry->settled = true;
            if (mounted())
                setState();
        }
    });
    entry->secondary->addStatusListener([this, entry](AnimationStatus status) {
        if (status == AnimationStatus::Completed) {
            entry->settled = true;
            if (mounted())
                setState();
        }
    });

    if (w->reverse() && !activeEntries_.empty())
        activeEntries_.insert(activeEntries_.end() - 1, entry);  // 新子垫在旧子下面
    else
        activeEntries_.push_back(entry);
    currentEntry_ = entry;
}

inline void PageTransitionSwitcherState::pruneSettled() {
    activeEntries_.erase(
            std::remove_if(activeEntries_.begin(), activeEntries_.end(),
                           [this](const SharedPtr<Entry>& e) {
                               return e->settled && e.get() != currentEntry_.get();
                           }),
            activeEntries_.end());
}

inline SharedPtr<Widget> PageTransitionSwitcherState::build(BuildContext& context) {
    (void)context;
    auto* w = static_cast<PageTransitionSwitcher*>(widget());
    if (!w)
        return nullptr;

    // 上游把换子判定放在 didUpdateWidget；本仓宿主常原地改同一个 Widget 对象，
    // 那条回调走不到，所以在 build 开头做一次（与 AnimatedSwitcher 同口径）。
    const bool hasNew = (w->child() != nullptr);
    const bool hasOld = (currentEntry_ != nullptr);
    if (hasNew != hasOld ||
        (hasNew && !w->child()->canUpdate(*currentEntry_->widgetChild))) {
        ++childNumber_;
        addEntryForNewChild(true);
    } else if (hasOld) {
        currentEntry_->widgetChild = w->child();
        updateTransitionForEntry(currentEntry_);
    }

    pruneSettled();

    std::vector<SharedPtr<Widget>> entries;
    entries.reserve(activeEntries_.size());
    for (const auto& e : activeEntries_)
        entries.push_back(e->transition);
    if (w->layoutBuilder())
        return w->layoutBuilder()(entries);
    return PageTransitionSwitcher::defaultLayoutBuilder(entries);
}

// ── OpenContainer ───────────────────────────────────────────────────────────

/// 容器变换（Material Container Transform）的**行内版本**。
///
/// rui story 的 `OpenContainer`：一个外部 AnimationController 驱动
/// closed/open 两个子控件的交叉淡入，同时补间容器背景色与圆角；
/// `toggle()` 按当前值决定正放还是反放。上游 animations 包的版本是一条
/// ModalRoute（容器 morph 成整屏），本仓无 showModal 路径，故只移植行内形态
/// （见文件头差异 2）。
///
/// 两态尺寸不同时，容器取较大者（Stack 的松 fit 自然宽度），不做尺寸补间。
class OpenContainer : public StatefulWidget {
public:
    OpenContainer(SharedPtr<AnimationController> animation, SharedPtr<Widget> closedChild,
                  SharedPtr<Widget> openChild,
                  Color closedColor = theme().surface,
                  Color openColor = theme().surface, double borderRadius = 0.0,
                  bool tappable = false, std::function<void()> onClosed = nullptr,
                  KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), animation_(std::move(animation)),
          closedChild_(std::move(closedChild)), openChild_(std::move(openChild)),
          closedColor_(closedColor), openColor_(openColor), borderRadius_(borderRadius),
          tappable_(tappable), onClosed_(std::move(onClosed)) {}

    const SharedPtr<AnimationController>& animation() const { return animation_; }
    const SharedPtr<Widget>& closedChild() const { return closedChild_; }
    const SharedPtr<Widget>& openChild() const { return openChild_; }
    Color closedColor() const { return closedColor_; }
    Color openColor() const { return openColor_; }
    double borderRadius() const { return borderRadius_; }
    bool tappable() const { return tappable_; }

    /// 当前进度（0 = 完全关闭，1 = 完全打开）。
    double progress() const {
        if (!animation_)
            return 0.0;
        return std::clamp(animation_->value(), 0.0, 1.0);
    }

    bool isOpen() const { return progress() > 0.5; }

    /// 小于一半 → 打开；否则 → 关闭。与 rui story 的 toggle 同语义。
    void toggle() const {
        if (!animation_)
            return;
        if (animation_->value() < 0.5)
            animation_->forward();
        else
            animation_->reverse();
    }

    void open() const {
        if (animation_)
            animation_->forward();
    }

    void close() const {
        if (animation_)
            animation_->reverse();
    }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<AnimationController> animation_;
    SharedPtr<Widget> closedChild_;
    SharedPtr<Widget> openChild_;
    Color closedColor_;
    Color openColor_;
    double borderRadius_ = 0.0;
    bool tappable_ = false;
    std::function<void()> onClosed_;
};

class OpenContainerState : public State {
public:
    void initState() override;
    void dispose() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    void onValueChanged();
    void onStatusChanged(AnimationStatus status);

    std::unique_ptr<Ticker> ticker_;
    int listenerHandle_ = 0;
    int statusHandle_ = 0;
};

inline std::unique_ptr<State> OpenContainer::createState() {
    return std::make_unique<OpenContainerState>();
}

inline void OpenContainerState::initState() {
    auto* w = static_cast<OpenContainer*>(widget());
    if (!w || !w->animation())
        return;
    ticker_ = std::make_unique<Ticker>([this](Duration elapsed) {
        auto* self = static_cast<OpenContainer*>(widget());
        if (!self || !self->animation())
            return;
        self->animation()->tick(elapsed);
        if (!self->animation()->isRunning() && ticker_ && ticker_->isActive())
            ticker_->stop();
        if (mounted())
            setState();
    });
    listenerHandle_ = w->animation()->addListenerWithHandle([this] { onValueChanged(); });
    statusHandle_ = w->animation()->addStatusListener(
            [this](AnimationStatus status) { onStatusChanged(status); });
}

inline void OpenContainerState::dispose() {
    auto* w = static_cast<OpenContainer*>(widget());
    if (w && w->animation()) {
        if (listenerHandle_ != 0)
            w->animation()->removeListenerByHandle(listenerHandle_);
        if (statusHandle_ != 0)
            w->animation()->removeStatusListener(statusHandle_);
    }
    listenerHandle_ = 0;
    statusHandle_ = 0;
    if (ticker_ && ticker_->isActive())
        ticker_->stop();
    ticker_.reset();
}

inline void OpenContainerState::onValueChanged() {
    if (mounted())
        setState();
}

inline void OpenContainerState::onStatusChanged(AnimationStatus status) {
    // forward()/reverse() 先派发状态、第一帧才通知监听者；ticker 必须在这一刻
    // 起表，否则动画永远等不到第一次 tick。
    if (status == AnimationStatus::Forward || status == AnimationStatus::Reverse) {
        if (ticker_ && !ticker_->isActive())
            ticker_->start();
    }
    if (mounted())
        setState();
}

inline SharedPtr<Widget> OpenContainerState::build(BuildContext& context) {
    (void)context;
    auto* w = static_cast<OpenContainer*>(widget());
    if (!w || !w->animation())
        return w ? w->closedChild() : nullptr;

    const double t = w->progress();
    // closed 的淡出是 Tween(1.0, 0.0) 直接套在控制器上，别包 CurveTween
    // （本仓 CurvedAnimation 的端点保护会把 1-v 在 0 处抹成 0）。
    auto closed = cui::make_shared<FadeTransition>(w->animation(), w->closedChild());
    closed->setRange(Tween<double>(1.0, 0.0));
    auto opened = cui::make_shared<FadeTransition>(w->animation(), w->openChild());

    std::vector<SharedPtr<Widget>> layers;
    layers.push_back(cui::make_shared<IgnorePointer>(t > 0.5, closed));
    layers.push_back(cui::make_shared<IgnorePointer>(t <= 0.5, opened));
    auto stack = cui::make_shared<Stack>(std::move(layers), nullptr,
                                         RenderStack::Alignment::MiddleCenter,
                                         RenderStack::Fit::Loose);

    auto box = cui::make_shared<Container>(stack);
    box->padding(EdgeInsets::all(16.0));
    box->decoration(BoxDecoration{material_motion::lerpColor(w->closedColor(), w->openColor(), t),
                                  w->borderRadius()});

    if (!w->tappable())
        return box;
    // 捕获 controller 而不是 widget：闭包可能比本次 build 活得更久。
    auto anim = w->animation();
    return cui::make_shared<GestureDetector>(box, [anim] {
        if (!anim)
            return;
        if (anim->value() < 0.5)
            anim->forward();
        else
            anim->reverse();
    });
}

}  // namespace cui_core
