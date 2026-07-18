#pragma once
/// @file widgets/hero.h
/// @brief 上游 widgets/heroes.dart 的移植：Hero / HeroController / HeroMode。
///
/// 对应 上游源码：
///   上游 widgets/heroes.dart
///
/// 一次 hero 飞行 = 「两条 PageRoute 各有一个同 tag 的 Hero」→ 在 Navigator
/// 的 Overlay 顶层插一条 shuttle 条目，用路由自己的转场动画把矩形从
/// fromHero 的位置补到 toHero 的位置；从/到两个 Hero 在飞行期间换成占位。
///
/// ── 与上游的有意偏离（勿当 bug 排查）────────────────────────────────────
///   1. **tag 是 `std::string`**（上游 `Object`）。C++ 侧不做 `Object` 装箱，
///      与 `ValueKey<T>` 的口径一致：需要别类型的 tag 时用 string 表达。
///   2. **`CreateRectTween` 返回 `SharedPtr<Tween<Rect>>`**（上游返回
///      `Tween<Rect?>`），`std::optional<Rect>` 两端对应上游可空的 begin/end。
///   3. **飞行结束的兜底走 `TransitionRoute::whenTransitionCompleted`**：
///      pop 方向的路由在退场动画走完时会被 dispose，而 `dispose()` 会先
///      `clearStatusListeners()` ⇒ 驱动的 curve/proxy 收不到最后一拍。
///      因此 pop 飞行额外在「被销毁的那条路由」上挂一个完成回调（用共享
///      token 防悬挂，见 hero.cpp 的 `HeroFlightToken`）。上游靠 Dart GC 里
///      `Future.then` 不会因为对象被丢弃而丢回调。
///   4. **`MediaQuery` padding 插值未接**（本仓无 MediaQuery）：默认 shuttle
///      直接返回 toHero 的 child（= 上游「取不到 MediaQuery」那条分支）。
///   5. **abort 时的淡出未接**：toHero 在飞行途中消失时，本仓停在最后一帧
///      矩形上（上游用 `_heroOpacity` 把它淡掉）；`divert` 的换车路径保留。
///   6. **曲线用 `Easing`**：上游默认 `Curves.fastOutSlowIn`，本仓取最接近的
///      `Easing::EaseInOutCubic`（enm 里没有 fastOutSlowIn，同 routes.h 偏离 15）。
///   7. **`HeroController` 必须挂在 `Navigator.observers` 上，且存活期不短于
///      它观察的 Navigator**（与上游 MaterialApp 的约束一致）。它的飞行表里
///      持有 HeroState 指针，Navigator 必须先于控制器结束。
///   8. `transitionOnUserGestures` 已透传；本仓没有 iOS 返回手势，该分支
///      只在测试里用手动 `didStartUserGesture` 触发。
///   9. 嵌套 Navigator 的 hero 收集已按上游实现（最近 Navigator 不是当前
///      控制器时，只收「顶层 PageRoute」里的 hero）；`HeroMode(false)` 跳过
///      整棵子树。

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/key.h"
#include "cui/framework/framework.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/navigator.h"
#include "cui/widgets/overlay.h"
#include "cui/widgets/pages.h"
#include "cui/widgets/routes.h"

namespace cui {

class HeroController;
class HeroFlight;

/// 飞行的方向。上游的 `HeroFlightDirection`（heroes.dart:57）。
enum class HeroFlightDirection {
    /// 路由 push 触发：动画 0 → 1，默认展示目标路由的 Hero child。
    push,
    /// 路由 pop 触发：动画 1 → 0，默认展示源路由的 Hero child。
    pop,
};

/// `Hero.createRectTween` / `HeroController.createRectTween`。
/// 上游的 `CreateRectTween`（heroes.dart:35）。
using CreateRectTween =
        std::function<SharedPtr<Tween<Rect>>(std::optional<Rect> begin, std::optional<Rect> end)>;

/// 给占位控件用的构造器。上游的 `HeroPlaceholderBuilder`（heroes.dart:44）。
using HeroPlaceholderBuilder = std::function<SharedPtr<Widget>(
        BuildContext& context, Size heroSize, SharedPtr<Widget> child)>;

/// 飞行中 shuttle 的构造器。上游的 `HeroFlightShuttleBuilder`（heroes.dart:49）。
using HeroFlightShuttleBuilder = std::function<SharedPtr<Widget>(
        BuildContext& flightContext, const SharedPtr<Animation<double>>& animation,
        HeroFlightDirection flightDirection, BuildContext& fromHeroContext,
        BuildContext& toHeroContext)>;

class HeroState;

// ── Hero ────────────────────────────────────────────────────────────────────

/// 把 child 标成「可做 hero 动画」的候选。上游的 `Hero`（heroes.dart:170）。
///
/// 参数顺序照上游（child 在最后），`key` 仍按本仓口径尾置 —— 本类是新增
/// 控件，没有既有调用点要迁就。
class Hero : public StatefulWidget {
public:
    Hero(std::string tag, SharedPtr<Widget> child, CreateRectTween createRectTween = nullptr,
         HeroFlightShuttleBuilder flightShuttleBuilder = nullptr,
         HeroPlaceholderBuilder placeholderBuilder = nullptr,
         bool transitionOnUserGestures = false, Easing curve = Easing::EaseInOutCubic,
         std::optional<Easing> reverseCurve = std::nullopt, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), tag_(std::move(tag)), child_(std::move(child)),
          createRectTween_(std::move(createRectTween)),
          flightShuttleBuilder_(std::move(flightShuttleBuilder)),
          placeholderBuilder_(std::move(placeholderBuilder)),
          transitionOnUserGestures_(transitionOnUserGestures), curve_(curve),
          reverseCurve_(reverseCurve) {}

    const std::string& tag() const { return tag_; }
    const SharedPtr<Widget>& child() const { return child_; }
    const CreateRectTween& createRectTween() const { return createRectTween_; }
    const HeroFlightShuttleBuilder& flightShuttleBuilder() const { return flightShuttleBuilder_; }
    const HeroPlaceholderBuilder& placeholderBuilder() const { return placeholderBuilder_; }
    bool transitionOnUserGestures() const { return transitionOnUserGestures_; }
    Easing curve() const { return curve_; }
    std::optional<Easing> reverseCurve() const { return reverseCurve_; }

    std::unique_ptr<State> createState() override;

private:
    std::string tag_;
    SharedPtr<Widget> child_;
    CreateRectTween createRectTween_;
    HeroFlightShuttleBuilder flightShuttleBuilder_;
    HeroPlaceholderBuilder placeholderBuilder_;
    bool transitionOnUserGestures_ = false;
    Easing curve_ = Easing::EaseInOutCubic;
    std::optional<Easing> reverseCurve_;
};

// ── HeroState ───────────────────────────────────────────────────────────────

/// Hero 的状态。上游的 `_HeroState`（heroes.dart:362）。
///
/// 公开可见只是为了 `HeroController` 能在头文件里持有它的指针；业务代码
/// 不应直接使用（上游是库私有类）。
class HeroState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override;

    Hero* heroWidget() const { return static_cast<Hero*>(widget()); }
    const std::string& tag() const { return heroWidget()->tag(); }

    /// 起飞：记下当前尺寸，把自己替换成占位。
    /// 上游 `startFlight({shouldIncludedChildInPlaceholder})`。
    void startFlight(bool shouldIncludeChildInPlaceholder = false);

    /// 落地：恢复真实 child。`keepPlaceholder` 为真时继续留着占位（上游
    /// 用它把「已经飞完但仍在场」的 hero 藏住）。
    void endFlight(bool keepPlaceholder = false);

    bool placeholderVisible() const { return placeholderSize_.has_value(); }

private:
    std::optional<Size> placeholderSize_;
    bool shouldIncludeChildInPlaceholder_ = true;
    SharedPtr<GlobalKey> key_ = make_shared<GlobalKey>("hero");
};

// ── HeroMode ────────────────────────────────────────────────────────────────

/// 开关子树里的 hero 动画。上游的 `HeroMode`（heroes.dart:1129）。
class HeroMode : public StatelessWidget {
public:
    explicit HeroMode(SharedPtr<Widget> child, bool enabled = true, KeyPtr key = nullptr)
        : StatelessWidget(std::move(key)), child_(std::move(child)), enabled_(enabled) {}

    bool enabled() const { return enabled_; }
    const SharedPtr<Widget>& child() const { return child_; }

    SharedPtr<Widget> build(BuildContext&) override { return child_; }

private:
    SharedPtr<Widget> child_;
    bool enabled_ = true;
};

// ── HeroController ──────────────────────────────────────────────────────────

/// 管理 hero 飞行的 NavigatorObserver。上游的 `HeroController`
/// （heroes.dart:835）。
///
/// 用法（与上游 `MaterialApp` 同形）：
///   auto hero = make_shared<HeroController>();
///   navigator->setObservers({hero.get()});
/// 控制器必须比 Navigator 活得久（见文件头偏离 7）。
class HeroController : public NavigatorObserver {
public:
    explicit HeroController(CreateRectTween createRectTween = nullptr);
    ~HeroController() override;

    HeroController(const HeroController&) = delete;
    HeroController& operator=(const HeroController&) = delete;

    const CreateRectTween& createRectTween() const { return createRectTween_; }

    // NavigatorObserver
    void didChangeTop(Route* topRoute, Route* previousTopRoute) override;
    void didStartUserGesture(Route* route, Route* previousRoute) override;
    void didStopUserGesture() override;

    /// 释放全部飞行与占位。上游的 `dispose()`。
    void dispose();

    /// 当前在飞的 tag 数（测试用）。
    size_t flightCount() const { return flights_.size(); }
    /// 某个 tag 是否在飞（测试用）。
    bool hasFlight(const std::string& tag) const {
        return flights_.find(tag) != flights_.end();
    }

private:
    friend class HeroFlight;

    /// 上游 `_maybeStartHeroTransition`：判方向、把 toRoute 先摆到 offstage
    /// 量一次最终位置，帧末再真正起飞。
    void maybeStartHeroTransition(NavigatorState* navigator, Route* fromRoute, Route* toRoute,
                                  bool isUserGestureTransition);

    /// 上游 `_startHeroTransition`：收集两侧 hero，起新飞行或换车。
    void startHeroTransition(NavigatorState* navigator, PageRoute* from, PageRoute* to,
                             std::optional<HeroFlightDirection> flightType,
                             bool isUserGestureTransition);

    /// 上游 `_handleFlightEnded`：从表里摘下来（延迟销毁，见 hero.cpp）。
    void handleFlightEnded(HeroFlight* flight);

    /// 路由销毁兜底：该 tag 的飞行若还在，按 pop 收尾。
    void finishFlightFromRouteDispose(const std::string& tag);

    /// 上游 `Hero._allHeroesFor`：收集 `context` 子树里参与本次转场的 hero。
    static std::map<std::string, HeroState*> allHeroesFor(BuildContext& context,
                                                         bool isUserGestureTransition,
                                                         NavigatorState* navigator);

    /// 默认 shuttle：本仓无 MediaQuery，直接给 toHero 的 child
    /// （见文件头偏离 4）。
    static SharedPtr<Widget> defaultFlightShuttleBuilder(
            BuildContext& flightContext, const SharedPtr<Animation<double>>& animation,
            HeroFlightDirection flightDirection, BuildContext& fromHeroContext,
            BuildContext& toHeroContext);

    CreateRectTween createRectTween_;

    /// 在飞的飞行（按 tag）。上游 `_flights`。
    std::map<std::string, std::unique_ptr<HeroFlight>> flights_;

    /// 已经结束、等下一拍回收的飞行。析构发生在自己的动画回调栈上是不安全
    /// 的（见 hero.cpp 的 `handleFlightEnded`）。
    std::vector<std::unique_ptr<HeroFlight>> trash_;

    bool disposed_ = false;
};

}  // namespace cui_core
