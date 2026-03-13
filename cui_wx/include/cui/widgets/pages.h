#pragma once
/// @file widgets/pages.h
/// @brief 上游的「整屏路由」层移植：PageRoute / PageRouteBuilder。
///
/// 对应 上游源码：
///   上游 widgets/pages.dart
///
/// 这一层薄得几乎只是「一组默认值」：`PageRoute` 坐在 `ModalRoute` 上，把
/// 整屏路由的三条公共口径钉死（永远 opaque、barrier 默认不可点消、只有同为
/// PageRoute 的前后脚才联动 secondaryAnimation），其余留给具体页面自己覆写。
/// `PageRouteBuilder` 则是「不加子类、改用回调」的那条快捷通道。
///
/// ── 与上游的有意偏离（勿当 bug 排查）────────────────────────────────────
///   1. **泛型 T（返回值类型）压成 `SharedPtr<void>`**，口径与 routes.h 一致。
///   2. **`allowSnapshotting` / `requestFocus` / 遍历边行为（traversalEdge
///      Behavior）未接**：前者本仓没有快照通道（同 routes.h 偏离 12），后两者
///      属 Focus/Intent 体系。
///   3. **`popGestureEnabled` 未接**（routes.h 偏离 5，PredictiveBack 整条
///      手势通道不在本层），故上游构造函数文档里那句「`barrierDismissible`
///      为真时按 Esc 弹掉」也没翻过来——本仓的 barrier 只响应点击。
///   4. **`transitionDuration` 用 double 毫秒**，不引入 `Duration` 值类型
///      参数（同 routes.h）。

#include <utility>

#include "cui/widgets/routes.h"

namespace cui {

// ── PageRoute ───────────────────────────────────────────────────────────────

/// 占满整屏的模态路由。上游的 `PageRoute<T>`（pages.dart:23）。
///
/// 它自己仍是抽象类（`buildPage`、`transitionDuration`、`maintainState` 都没
/// 给）；一次性页面请直接派生 `PageRouteBuilder`。
class PageRoute : public ModalRoute {
public:
    explicit PageRoute(bool fullscreenDialog = false, bool barrierDismissible = false,
                       SharedPtr<const RouteSettings> settings = nullptr)
        : ModalRoute(std::move(settings)), fullscreenDialog_(fullscreenDialog),
          barrierDismissible_(barrierDismissible) {}

    bool fullscreenDialog() const override { return fullscreenDialog_; }

    /// 上游的 `opaque => true`：整屏路由转场结束后一定把下层挡严实。
    bool opaque() const override { return true; }

    /// 上游的 `barrierDismissible`：默认 false（整屏页面不靠点遮罩关闭）。
    bool barrierDismissible() const override { return barrierDismissible_; }

    /// 上游的 `canTransitionTo` / `canTransitionFrom`：只有同为 PageRoute 才
    /// 联动次级动画（弹窗压在页面上时，页面不该跟着弹窗的淡入淡出走）。
    bool canTransitionTo(const TransitionRoute& nextRoute) const override {
        return dynamic_cast<const PageRoute*>(&nextRoute) != nullptr;
    }
    bool canTransitionFrom(const TransitionRoute& previousRoute) const override {
        return dynamic_cast<const PageRoute*>(&previousRoute) != nullptr;
    }

private:
    bool fullscreenDialog_ = false;
    bool barrierDismissible_ = false;
};

// ── PageRouteBuilder ────────────────────────────────────────────────────────

/// 用回调拼装整屏路由。上游的 `PageRouteBuilder<T>`（pages.dart:89）。
///
/// `pageBuilder` 必填；`transitionsBuilder` 缺省给「硬切」（直接返回 child）。
/// 两条时长各自独立（上游 也是两个字段，退场可以更快）。
class PageRouteBuilder : public PageRoute {
public:
    PageRouteBuilder(RoutePageBuilder pageBuilder,
                     RouteTransitionsBuilder transitionsBuilder = nullptr,
                     double transitionDurationMs = 300.0,
                     std::optional<double> reverseTransitionDurationMs = std::nullopt,
                     bool opaque = true, bool maintainState = true,
                     std::optional<Color> barrierColor = std::nullopt,
                     bool barrierDismissible = false, bool fullscreenDialog = false,
                     SharedPtr<const RouteSettings> settings = nullptr)
        : PageRoute(fullscreenDialog, barrierDismissible, std::move(settings)),
          pageBuilder_(std::move(pageBuilder)),
          transitionsBuilder_(std::move(transitionsBuilder)),
          transitionDurationMs_(transitionDurationMs),
          reverseTransitionDurationMs_(reverseTransitionDurationMs),
          opaque_(opaque), maintainState_(maintainState),
          barrierColor_(barrierColor) {}

    bool opaque() const override { return opaque_; }
    bool maintainState() const override { return maintainState_; }
    std::optional<Color> barrierColor() const override { return barrierColor_; }

    double transitionDuration() const override { return transitionDurationMs_; }
    double reverseTransitionDuration() const override {
        return reverseTransitionDurationMs_.value_or(transitionDurationMs_);
    }

    SharedPtr<Widget> buildPage(BuildContext& context,
                                 SharedPtr<Animation<double>> animation,
                                 SharedPtr<Animation<double>> secondaryAnimation) override {
        return pageBuilder_ ? pageBuilder_(context, animation, secondaryAnimation)
                            : nullptr;
    }

    SharedPtr<Widget> buildTransitions(BuildContext& context,
                                       SharedPtr<Animation<double>> animation,
                                       SharedPtr<Animation<double>> secondaryAnimation,
                                       SharedPtr<Widget> child) override {
        return transitionsBuilder_
                       ? transitionsBuilder_(context, animation, secondaryAnimation,
                                             std::move(child))
                       : child;
    }

private:
    RoutePageBuilder pageBuilder_;
    RouteTransitionsBuilder transitionsBuilder_;
    double transitionDurationMs_;
    std::optional<double> reverseTransitionDurationMs_;
    bool opaque_;
    bool maintainState_;
    std::optional<Color> barrierColor_;
};

} // namespace cui_core
