/// @file widgets/hero.cpp
/// @brief 上游 widgets/heroes.dart 的实现：Hero / HeroState / HeroController /
///        飞行（_HeroFlight / _HeroFlightManifest）/ HeroMode。
///
/// 与上游的偏离逐条列在 hero.h；这里只标注各段对应的上游行号。

#include "cui/widgets/hero.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "cui/framework/build_owner.h"
#include "cui/rendering/render_box.h"
#include "cui/rendering/render_object.h"
#include "cui/widgets/animated_builder.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/ticker_mode.h"

namespace cui {

namespace {

bool rectIsFinite(const Rect& rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
           std::isfinite(rect.height);
}

bool sizeIsFinite(const Size& size) {
    return std::isfinite(size.width) && std::isfinite(size.height);
}

/// 把一个轴对齐矩形过一遍矩阵。Hero 要求英雄本身轴对齐（上游文档同），
/// 所以取左上/右下两个角点再归一化就够（本仓没有 MatrixUtils.transformRect）。
Rect transformRect(const Matrix4& transform, const Rect& rect) {
    const Offset topLeft = transform.transformOffset({rect.x, rect.y});
    const Offset bottomRight =
            transform.transformOffset({rect.x + rect.width, rect.y + rect.height});
    const double left = std::min(topLeft.dx, bottomRight.dx);
    const double top = std::min(topLeft.dy, bottomRight.dy);
    const double right = std::max(topLeft.dx, bottomRight.dx);
    const double bottom = std::max(topLeft.dy, bottomRight.dy);
    return Rect(left, top, right - left, bottom - top);
}

RenderBox* boxOf(BuildContext* context) {
    if (!context || !context->element())
        return nullptr;
    return dynamic_cast<RenderBox*>(context->element()->findRenderObject());
}

Rect invalidRect() {
    const double nan = std::nan("");
    return Rect(nan, nan, nan, nan);
}

/// `hero` 到 `route` 子树坐标系的包围盒。上游 `_HeroFlightManifest.
/// _boundingBoxFor`（heroes.dart:504-513）。
Rect boundingBoxFor(HeroState* hero, PageRoute* route) {
    if (!hero || !route)
        return invalidRect();
    RenderBox* box = boxOf(hero->context());
    BuildContext* routeContext = route->subtreeContext();
    RenderObject* ancestor = routeContext && routeContext->element()
                                     ? routeContext->element()->findRenderObject()
                                     : nullptr;
    if (!box || !ancestor)
        return invalidRect();
    return transformRect(box->getTransformTo(ancestor), Rect(0, 0, box->size().width,
                                                              box->size().height));
}

}  // namespace

// ── HeroState ───────────────────────────────────────────────────────────────

void HeroState::startFlight(bool shouldIncludeChildInPlaceholder) {
    // 上游 `_HeroState.startFlight`（heroes.dart:386-395）。
    if (!mounted())
        return;
    RenderBox* box = boxOf(context());
    if (!box)
        return;
    shouldIncludeChildInPlaceholder_ = shouldIncludeChildInPlaceholder;
    const Size size = box->size();
    setState([this, size] { placeholderSize_ = size; });
}

void HeroState::endFlight(bool keepPlaceholder) {
    // 上游 `_HeroState.endFlight`（heroes.dart:401-411）。
    if (keepPlaceholder || !placeholderSize_)
        return;
    placeholderSize_.reset();
    if (mounted())
        setState();
}

SharedPtr<Widget> HeroState::build(BuildContext&) {
    // 上游 `_HeroState.build`（heroes.dart:413-439）。
    Hero* hero = heroWidget();
    const bool showPlaceholder = placeholderSize_.has_value();

    if (showPlaceholder && hero->placeholderBuilder()) {
        return hero->placeholderBuilder()(*context(), *placeholderSize_, hero->child());
    }

    if (showPlaceholder && !shouldIncludeChildInPlaceholder_) {
        return make_shared<SizedBox>(placeholderSize_->width, placeholderSize_->height);
    }

    SharedPtr<Widget> inner = make_shared<KeyedSubtree>(key_, hero->child());
    inner = make_shared<TickerMode>(inner, !showPlaceholder);
    inner = make_shared<Offstage>(showPlaceholder, inner);
    if (showPlaceholder) {
        // 上游始终外包一个宽度/高度可空的 SizedBox；本仓 SizedBox 只收具值，
        // 无占位时尺寸本来就不该被约束，故只在占位期补这一层。
        inner = make_shared<SizedBox>(placeholderSize_->width, placeholderSize_->height, inner);
    }
    return inner;
}

std::unique_ptr<State> Hero::createState() {
    return std::make_unique<HeroState>();
}

// ── 飞行清单与飞行（internal）────────────────────────────────────────────────

namespace {

/// 飞行结束兜底用的共享 token：路由的完成回调捕的是它，飞行析构时把
/// `flight` 置空 ⇒ 回调晚到也不会打到已释放的飞行（本仓没有 weak_ptr）。
struct HeroFlightToken {
    class HeroFlight* flight = nullptr;
};

}  // namespace

/// 一次飞行的全部已知信息。上游 `_HeroFlightManifest`（heroes.dart:442）。
class HeroFlightManifest {
public:
    HeroFlightDirection type = HeroFlightDirection::push;
    OverlayState* overlay = nullptr;
    NavigatorState* navigator = nullptr;
    Size navigatorSize;
    PageRoute* fromRoute = nullptr;
    PageRoute* toRoute = nullptr;
    HeroState* fromHero = nullptr;
    HeroState* toHero = nullptr;
    CreateRectTween createRectTween;
    HeroFlightShuttleBuilder shuttleBuilder;
    bool isUserGestureTransition = false;
    bool isDiverted = false;
    std::string tag;

    SharedPtr<CurvedAnimation> animation;

    Rect fromHeroLocation = invalidRect();
    Rect toHeroLocation = invalidRect();

    bool isValid() const {
        return rectIsFinite(toHeroLocation) && (isDiverted || rectIsFinite(fromHeroLocation));
    }

    /// 两端的矩形补间。上游 `createHeroRectTween`（heroes.dart:492-497）。
    SharedPtr<Tween<Rect>> createHeroRectTween(std::optional<Rect> begin,
                                               std::optional<Rect> end) const {
        if (toHero && toHero->heroWidget()->createRectTween())
            return toHero->heroWidget()->createRectTween()(begin, end);
        if (createRectTween)
            return createRectTween(begin, end);
        return cui::make_shared<RectTween>(begin, end);
    }
};

/// 在飞的 hero。上游 `_HeroFlight`（heroes.dart:544）。
class HeroFlight {
public:
    using OnFlightEnded = std::function<void(HeroFlight*)>;

    explicit HeroFlight(OnFlightEnded onFlightEnded)
        : onFlightEnded_(std::move(onFlightEnded)) {
        token_ = make_shared<HeroFlightToken>();
        token_->flight = this;
        proxyAnimation_ = make_shared<ProxyAnimation>();
        statusHandle_ = proxyAnimation_->addStatusListener(
                [this](AnimationStatus status) { handleAnimationUpdate(status); });
    }

    ~HeroFlight() {
        token_->flight = nullptr;
        dispose();
    }

    HeroFlight(const HeroFlight&) = delete;
    HeroFlight& operator=(const HeroFlight&) = delete;

    const HeroFlightManifest& manifest() const { return *manifest_; }
    const SharedPtr<HeroFlightToken>& token() const { return token_; }
    bool finished() const { return finished_; }
    /// 上游 `flight._proxyAnimation.isDismissed`：手势没真推动时的判定。
    bool proxyDismissed() const { return proxyAnimation_ && proxyAnimation_->value() == 0.0; }
    /// 是否已经在「被驱动的那条路由」上挂了 dispose 兜底（见 hero.h 偏离 3）。
    /// 驱动路由随方向变化：push 看 toRoute，pop 看 fromRoute。
    PageRoute* hookedRoute() const { return hookedRoute_; }
    void setHookedRoute(PageRoute* route) { hookedRoute_ = route; }
    PageRoute* drivingRoute() const {
        if (!manifest_)
            return nullptr;
        return manifest_->type == HeroFlightDirection::pop ? manifest_->fromRoute
                                                           : manifest_->toRoute;
    }

    /// 上游 `_HeroFlight.start`（heroes.dart:697-731）。
    void start(std::unique_ptr<HeroFlightManifest> initialManifest) {
        manifest_ = std::move(initialManifest);
        if (manifest_->type == HeroFlightDirection::pop) {
            proxyAnimation_->setParent(make_shared<ReverseAnimation>(manifest_->animation));
        } else {
            proxyAnimation_->setParent(manifest_->animation);
        }

        rectBegin_ = manifest_->fromHeroLocation;
        rectEnd_ = manifest_->toHeroLocation;
        heroRectTween_ = manifest_->createHeroRectTween(rectBegin_, rectEnd_);

        manifest_->fromHero->startFlight(manifest_->type == HeroFlightDirection::push);
        manifest_->toHero->startFlight(false);

        overlayEntry_ = make_shared<OverlayEntry>(
                [this](BuildContext& context) { return buildOverlay(context); });
        manifest_->overlay->insert(overlayEntry_);
        tickHandle_ = proxyAnimation_->addListenerWithHandle([this] { onTick(); });
    }

    /// 上游 `_HeroFlight.divert`（heroes.dart:733-800）：飞行途中又发生了一次
    /// 同 tag 的 push/pop ⇒ 把飞行改挂到新的目标上，而不是重开一条。
    void divert(std::unique_ptr<HeroFlightManifest> newManifest) {
        const HeroFlightDirection oldType = manifest_->type;
        const HeroFlightDirection newType = newManifest->type;

        if (oldType == HeroFlightDirection::push && newType == HeroFlightDirection::pop) {
            // push 被 pop 打断：同一条矩形补间反着走（上游同款注释）。
            proxyAnimation_->setParent(make_shared<ReverseAnimation>(newManifest->animation));
            Rect begin = rectEnd_;
            Rect end = rectBegin_;
            rectBegin_ = begin;
            rectEnd_ = end;
            heroRectTween_ = manifest_->createHeroRectTween(rectBegin_, rectEnd_);
        } else if (oldType == HeroFlightDirection::pop && newType == HeroFlightDirection::push) {
            Tween<double> remaining(manifest_->animation ? manifest_->animation->value() : 0.0, 1.0);
            proxyAnimation_->setParent(remaining.animate(newManifest->animation));
            if (manifest_->toHero != newManifest->fromHero) {
                manifest_->fromHero->endFlight(true);
                newManifest->toHero->startFlight(false);
                rectBegin_ = rectEnd_;
                rectEnd_ = newManifest->toHeroLocation;
                heroRectTween_ = manifest_->createHeroRectTween(rectBegin_, rectEnd_);
            } else {
                std::swap(rectBegin_, rectEnd_);
                heroRectTween_ = manifest_->createHeroRectTween(rectBegin_, rectEnd_);
            }
        } else {
            // 同向换目标：从当前补间值接着走到新目标。
            rectBegin_ = heroRectTween_ ? heroRectTween_->evaluate(*proxyAnimation_) : rectEnd_;
            rectEnd_ = newManifest->toHeroLocation;
            heroRectTween_ = manifest_->createHeroRectTween(rectBegin_, rectEnd_);
            shuttle_.reset();

            if (newType == HeroFlightDirection::pop) {
                proxyAnimation_->setParent(make_shared<ReverseAnimation>(newManifest->animation));
            } else {
                proxyAnimation_->setParent(newManifest->animation);
            }

            manifest_->fromHero->endFlight(true);
            manifest_->toHero->endFlight(true);
            newManifest->fromHero->startFlight(newType == HeroFlightDirection::push);
            newManifest->toHero->startFlight(false);
            if (overlayEntry_)
                overlayEntry_->markNeedsBuild();
        }

        manifest_ = std::move(newManifest);
    }

    void abort() { aborted_ = true; }

    /// 收尾（幂等）。`status` 决定两端占位怎么留：
    /// Completed ⇒ 留住 from 的占位（它已经在下面了）；
    /// Dismissed ⇒ 留住 to 的占位（飞行被取消，目标没现身）。
    /// 上游 `_performAnimationUpdate`（heroes.dart:593-613）。
    void finish(AnimationStatus status) {
        if (finished_)
            return;
        finished_ = true;

        if (proxyAnimation_)
            proxyAnimation_->setParent(nullptr);
        if (overlayEntry_) {
            overlayEntry_->remove();
            overlayEntry_.reset();
        }
        if (manifest_) {
            manifest_->fromHero->endFlight(status == AnimationStatus::Completed);
            manifest_->toHero->endFlight(status == AnimationStatus::Dismissed);
        }
        if (proxyAnimation_ && tickHandle_ != 0) {
            proxyAnimation_->removeListenerByHandle(tickHandle_);
            tickHandle_ = 0;
        }
        if (onFlightEnded_)
            onFlightEnded_(this);
    }

    void dispose() {
        if (proxyAnimation_) {
            if (tickHandle_ != 0) {
                proxyAnimation_->removeListenerByHandle(tickHandle_);
                tickHandle_ = 0;
            }
            if (statusHandle_ != 0) {
                proxyAnimation_->removeStatusListener(statusHandle_);
                statusHandle_ = 0;
            }
            proxyAnimation_->setParent(nullptr);
        }
        if (overlayEntry_) {
            overlayEntry_->remove();
            overlayEntry_.reset();
        }
        manifest_.reset();
    }

private:
    // 上游 `_buildOverlay`（heroes.dart:577-590）：shuttle 在飞行开始时构造一次，
    // 之后每拍只更新矩形。
    SharedPtr<Widget> buildOverlay(BuildContext& flightContext) {
        if (!shuttle_) {
            shuttle_ = manifest_->shuttleBuilder(
                    flightContext, manifest_->animation, manifest_->type,
                    *manifest_->fromHero->context(), *manifest_->toHero->context());
        }

        return make_shared<AnimatedBuilder>(
                proxyAnimation_, [this](BuildContext&, const SharedPtr<Animation<double>>&) {
                    const Rect rect = heroRectTween_ ? heroRectTween_->evaluate(*proxyAnimation_)
                                                     : rectEnd_;
                    auto positioned = make_shared<Positioned>(make_shared<IgnorePointer>(true,
                                                                                        shuttle_));
                    positioned->left = rect.x;
                    positioned->top = rect.y;
                    positioned->width = rect.width;
                    positioned->height = rect.height;
                    positioned->hasLeft = true;
                    positioned->hasTop = true;
                    positioned->hasWidth = true;
                    positioned->hasHeight = true;
                    return positioned;
                });
    }

    /// 上游 `_handleAnimationUpdate`（heroes.dart:615-643）：用户手势期间
    /// 状态更新要压到手指抬起之后。
    void handleAnimationUpdate(AnimationStatus status) {
        if (finished_)
            return;
        if (status == AnimationStatus::Forward || status == AnimationStatus::Reverse)
            return;
        NavigatorState* navigator = manifest_ ? manifest_->navigator : nullptr;
        if (!navigator || !navigator->userGestureInProgress()) {
            finish(status);
            return;
        }
        if (deferHandle_ != 0)
            return;
        deferHandle_ = navigator->userGestureInProgressNotifier()->addListenerWithHandle([this] {
            NavigatorState* nav = manifest_ ? manifest_->navigator : nullptr;
            if (!nav || nav->userGestureInProgress())
                return;
            if (deferHandle_ != 0) {
                nav->userGestureInProgressNotifier()->removeListenerByHandle(deferHandle_);
                deferHandle_ = 0;
            }
            finish(proxyAnimation_ ? proxyAnimation_->status() : AnimationStatus::Completed);
        });
    }

    /// 上游 `_HeroFlight.onTick`（heroes.dart:664-693）：目标框每帧重新量一次，
    /// 目标消失时把飞行标记为 aborted（本仓不做淡出，见 hero.h 偏离 5）。
    void onTick() {
        if (!manifest_ || finished_)
            return;
        RenderBox* toBox = !aborted_ && manifest_->toHero->mounted()
                                   ? boxOf(manifest_->toHero->context())
                                   : nullptr;
        Rect newEnd = invalidRect();
        if (toBox) {
            BuildContext* routeContext = manifest_->toRoute->subtreeContext();
            RenderObject* ancestor = routeContext && routeContext->element()
                                             ? routeContext->element()->findRenderObject()
                                             : nullptr;
            if (ancestor) {
                newEnd = transformRect(toBox->getTransformTo(ancestor),
                                       Rect(0, 0, toBox->size().width, toBox->size().height));
            }
        }

        if (rectIsFinite(newEnd)) {
            const double endSize = rectEnd_.width;
            const double endHeight = rectEnd_.height;
            if (newEnd.x != rectEnd_.x || newEnd.y != rectEnd_.y) {
                // 只更新终点坐标，尺寸沿用起飞时量的那一份（上游同款）。
                rectEnd_ = Rect(newEnd.x, newEnd.y, endSize, endHeight);
                heroRectTween_ = manifest_->createHeroRectTween(rectBegin_, rectEnd_);
            }
            aborted_ = false;
        } else {
            aborted_ = true;
        }
    }

    OnFlightEnded onFlightEnded_;
    SharedPtr<HeroFlightToken> token_;
    SharedPtr<ProxyAnimation> proxyAnimation_;
    std::unique_ptr<HeroFlightManifest> manifest_;
    SharedPtr<OverlayEntry> overlayEntry_;
    SharedPtr<Widget> shuttle_;
    SharedPtr<Tween<Rect>> heroRectTween_;
    Rect rectBegin_;
    Rect rectEnd_;
    bool aborted_ = false;
    bool finished_ = false;
    PageRoute* hookedRoute_ = nullptr;
    int tickHandle_ = 0;
    int statusHandle_ = 0;
    int deferHandle_ = 0;
};

// ── HeroController ──────────────────────────────────────────────────────────

HeroController::HeroController(CreateRectTween createRectTween)
    : createRectTween_(std::move(createRectTween)) {}

HeroController::~HeroController() {
    dispose();
}

void HeroController::dispose() {
    if (disposed_)
        return;
    disposed_ = true;
    trash_.clear();
    for (auto& entry : flights_)
        entry.second->dispose();
    flights_.clear();
}

void HeroController::didChangeTop(Route* topRoute, Route* previousTopRoute) {
    if (disposed_ || !topRoute || !previousTopRoute)
        return;
    // 上一拍的尸体到这时早已卸载完，可以安全回收。
    trash_.clear();

    NavigatorState* navigator = topRoute->navigator();
    if (!navigator)
        navigator = previousTopRoute->navigator();
    if (!navigator || navigator->userGestureInProgress())
        return;

    // 上游 `HeroController.didChangeTop`（heroes.dart:849-864）。
    maybeStartHeroTransition(navigator, previousTopRoute, topRoute, false);
}

void HeroController::didStartUserGesture(Route* route, Route* previousRoute) {
    // 上游 `HeroController.didStartUserGesture`（heroes.dart:866-874）。
    if (disposed_ || !route || !previousRoute)
        return;
    NavigatorState* navigator = route->navigator();
    if (!navigator)
        return;
    maybeStartHeroTransition(navigator, route, previousRoute, true);
}

void HeroController::didStopUserGesture() {
    // 上游 `HeroController.didStopUserGesture`（heroes.dart:876-899）：手势没
    // 真正推动时，飞行永远等不到状态更新，这里手动按 dismissed 收尾。
    if (disposed_)
        return;
    std::vector<HeroFlight*> invalid;
    for (auto& entry : flights_) {
        HeroFlight* flight = entry.second.get();
        if (flight->manifest().isUserGestureTransition &&
            flight->manifest().type == HeroFlightDirection::pop && !flight->finished() &&
            flight->proxyDismissed()) {
            // 只有「还停在起点」的飞行才算无效（上游 `_proxyAnimation.isDismissed`）。
            invalid.push_back(flight);
        }
    }
    for (HeroFlight* flight : invalid)
        flight->finish(AnimationStatus::Dismissed);
    trash_.clear();
}

void HeroController::maybeStartHeroTransition(NavigatorState* navigator, Route* fromRoute,
                                              Route* toRoute, bool isUserGestureTransition) {
    // 上游 `_maybeStartHeroTransition`（heroes.dart:901-978）。
    if (!navigator || toRoute == fromRoute)
        return;

    auto* from = dynamic_cast<PageRoute*>(fromRoute);
    auto* to = dynamic_cast<PageRoute*>(toRoute);
    if (!from || !to)
        return;

    const SharedPtr<Animation<double>> fromAnimation = from->animation();
    const SharedPtr<Animation<double>> toAnimation = to->animation();
    if (!fromAnimation || !toAnimation)
        return;

    std::optional<HeroFlightDirection> flightType;
    if (isUserGestureTransition) {
        flightType = HeroFlightDirection::pop;
    } else if (fromAnimation->status() == AnimationStatus::Reverse) {
        flightType = HeroFlightDirection::pop;
    } else if (toAnimation->status() == AnimationStatus::Forward) {
        flightType = HeroFlightDirection::push;
    }
    if (!flightType)
        return;

    if (*flightType == HeroFlightDirection::pop && fromAnimation->value() == 0.0)
        return;
    if (*flightType == HeroFlightDirection::push && toAnimation->value() == 1.0)
        return;

    // 用户手势驱动的 pop：目标页若 maintainState 且已经布局过，可以立刻量。
    RenderBox* toBox = boxOf(to->subtreeContext());
    const bool hasValidSize = toBox && sizeIsFinite(toBox->size());
    if (isUserGestureTransition && *flightType == HeroFlightDirection::pop &&
        to->maintainState() && hasValidSize) {
        startHeroTransition(navigator, from, to, flightType, true);
        return;
    }

    // 否则先把目标路由摆到 offstage（对外动画值变成 1.0 = 最终位置），
    // 等这一帧布局完再起飞。
    to->setOffstage(toAnimation->value() == 0.0);

    BuildContext* navigatorContext = navigator->context();
    BuildOwner* owner = navigatorContext && navigatorContext->element()
                                ? navigatorContext->element()->owner()
                                : nullptr;
    if (!owner)
        return;

    SharedPtr<Route> fromKeep = from->shared_from_this();
    SharedPtr<Route> toKeep = to->shared_from_this();
    owner->addPostFrameCallback(
            [this, navigator, fromKeep, toKeep, flightType](Duration) {
                if (disposed_)
                    return;
                auto* fromRouteAgain = dynamic_cast<PageRoute*>(fromKeep.get());
                auto* toRouteAgain = dynamic_cast<PageRoute*>(toKeep.get());
                if (!fromRouteAgain || !toRouteAgain)
                    return;
                if (!fromRouteAgain->navigator() || !toRouteAgain->navigator())
                    return;
                startHeroTransition(navigator, fromRouteAgain, toRouteAgain, flightType, false);
            });
}

void HeroController::startHeroTransition(NavigatorState* navigator, PageRoute* from, PageRoute* to,
                                         std::optional<HeroFlightDirection> flightType,
                                         bool isUserGestureTransition) {
    // 上游 `_startHeroTransition`（heroes.dart:980-1082）。
    to->setOffstage(false);

    OverlayState* overlay = navigator ? navigator->overlayState() : nullptr;
    if (!overlay)
        return;

    RenderBox* navigatorBox = boxOf(navigator->context());
    if (!navigatorBox)
        return;
    const Size navigatorSize = navigatorBox->size();

    BuildContext* fromSubtree = from->subtreeContext();
    BuildContext* toSubtree = to->subtreeContext();
    std::map<std::string, HeroState*> fromHeroes =
            fromSubtree ? allHeroesFor(*fromSubtree, isUserGestureTransition, navigator)
                        : std::map<std::string, HeroState*>{};
    std::map<std::string, HeroState*> toHeroes =
            toSubtree ? allHeroesFor(*toSubtree, isUserGestureTransition, navigator)
                      : std::map<std::string, HeroState*>{};

    for (auto& entry : fromHeroes) {
        const std::string& tag = entry.first;
        HeroState* fromHero = entry.second;
        auto toIt = toHeroes.find(tag);
        HeroState* toHero = toIt == toHeroes.end() ? nullptr : toIt->second;
        auto existingIt = flights_.find(tag);
        HeroFlight* existingFlight =
                existingIt == flights_.end() ? nullptr : existingIt->second.get();

        if (!toHero || !flightType) {
            if (existingFlight)
                existingFlight->abort();
            continue;
        }

        auto manifest = std::make_unique<HeroFlightManifest>();
        manifest->type = *flightType;
        manifest->overlay = overlay;
        manifest->navigator = navigator;
        manifest->navigatorSize = navigatorSize;
        manifest->fromRoute = from;
        manifest->toRoute = to;
        manifest->fromHero = fromHero;
        manifest->toHero = toHero;
        manifest->createRectTween = createRectTween_;
        manifest->isUserGestureTransition = isUserGestureTransition;
        manifest->isDiverted = existingFlight != nullptr;
        manifest->tag = tag;

        const Hero* curveSource = (*flightType == HeroFlightDirection::push)
                                          ? toHero->heroWidget()
                                          : fromHero->heroWidget();
        const SharedPtr<Animation<double>> parent =
                (*flightType == HeroFlightDirection::push) ? to->animation() : from->animation();
        if (!parent)
            continue;
        const CurveFn curve = makeCurveFn(curveSource->curve());
        CurveFn reverseCurve = curveSource->reverseCurve()
                                       ? makeCurveFn(*curveSource->reverseCurve())
                                       : flipCurveFn(curve);
        manifest->animation = cui::make_shared<CurvedAnimation>(
                parent, curve,
                manifest->isDiverted ? std::optional<CurveFn>(std::nullopt) : reverseCurve);

        manifest->fromHeroLocation = boundingBoxFor(fromHero, from);
        manifest->toHeroLocation = boundingBoxFor(toHero, to);

        manifest->shuttleBuilder = toHero->heroWidget()->flightShuttleBuilder()
                                           ? toHero->heroWidget()->flightShuttleBuilder()
                                           : (fromHero->heroWidget()->flightShuttleBuilder()
                                                      ? fromHero->heroWidget()->flightShuttleBuilder()
                                                      : defaultFlightShuttleBuilder);

        if (manifest->isValid()) {
            toHeroes.erase(tag);
            HeroFlight* flight = nullptr;
            if (existingFlight) {
                existingFlight->divert(std::move(manifest));
                flight = existingFlight;
            } else {
                auto created = std::make_unique<HeroFlight>(
                        [this](HeroFlight* ended) { handleFlightEnded(ended); });
                flight = created.get();
                flights_[tag] = std::move(created);
                flight->start(std::move(manifest));
            }

            // 被驱动的那条路由若在飞行结束前就 dispose（pop 退场结束 / push 被
            // 打断），dispose 会先清掉动画状态监听 ⇒ 额外挂一条完成回调兜底。
            // 驱动路由随方向变化：push 看 toRoute、pop 看 fromRoute；divert 翻转
            // 方向时按新的驱动路由补挂（同一路由对象则复用已有那条）。
            PageRoute* driven = flight->drivingRoute();
            if (driven && flight->hookedRoute() != driven) {
                flight->setHookedRoute(driven);
                const SharedPtr<HeroFlightToken> token = flight->token();
                driven->whenTransitionCompleted([token](SharedPtr<void>) {
                    if (!token->flight)
                        return;
                    // 回看触发时刻的方向：pop 走完 ⇒ Completed（from 的占位
                    // 留着）；push 被打断 ⇒ Dismissed（收回 to 的占位）。
                    token->flight->finish(
                            token->flight->manifest().type == HeroFlightDirection::pop
                                    ? AnimationStatus::Completed
                                    : AnimationStatus::Dismissed);
                });
            }
        } else if (existingFlight) {
            existingFlight->abort();
        }
    }

    // 没配成对的 toHero 一律把占位收回。
    for (auto& entry : toHeroes)
        entry.second->endFlight(false);
}

void HeroController::handleFlightEnded(HeroFlight* flight) {
    // 不能在这里 delete：调用栈可能还在飞行自己的动画回调里。挪进 trash_，
    // 下一拍（或控制器析构）再回收。
    for (auto it = flights_.begin(); it != flights_.end(); ++it) {
        if (it->second.get() == flight) {
            trash_.push_back(std::move(it->second));
            flights_.erase(it);
            return;
        }
    }
}

void HeroController::finishFlightFromRouteDispose(const std::string& tag) {
    auto it = flights_.find(tag);
    if (it != flights_.end())
        it->second->finish(AnimationStatus::Completed);
}

std::map<std::string, HeroState*> HeroController::allHeroesFor(
        BuildContext& context, bool isUserGestureTransition, NavigatorState* navigator) {
    // 上游 `Hero._allHeroesFor`（heroes.dart:293-359）。
    std::map<std::string, HeroState*> result;

    auto inviteHero = [&](Element* element, const std::string& tag) {
        auto* stateful = dynamic_cast<StatefulElement*>(element);
        auto* heroState = stateful ? dynamic_cast<HeroState*>(stateful->state()) : nullptr;
        if (!heroState)
            return;
        Hero* heroWidget = heroState->heroWidget();
        if (!isUserGestureTransition || heroWidget->transitionOnUserGestures()) {
            result[tag] = heroState;
        } else {
            heroState->endFlight();
        }
    };

    std::function<void(Element*)> visitor = [&](Element* element) {
        Widget* widget = element ? element->widget() : nullptr;
        if (!widget)
            return;
        if (auto* hero = dynamic_cast<Hero*>(widget)) {
            const std::string tag = hero->tag();
            NavigatorState* nearest = element->findAncestorStateOfType<NavigatorState>();
            if (nearest == navigator) {
                inviteHero(element, tag);
            } else {
                // 最近的 Navigator 不是当前转场的那一个 ⇒ 只有「嵌套 Navigator
                // 里顶层 PageRoute」的 hero 才参与。
                ModalRoute* heroRoute = ModalRoute::maybeOf(*element);
                if (heroRoute && dynamic_cast<PageRoute*>(heroRoute) && heroRoute->isCurrent())
                    inviteHero(element, tag);
            }
            return;
        }
        if (auto* mode = dynamic_cast<HeroMode*>(widget); mode && !mode->enabled())
            return;
        element->visitChildren(visitor);
    };

    context.visitChildElements(visitor);
    return result;
}

SharedPtr<Widget> HeroController::defaultFlightShuttleBuilder(
        BuildContext& flightContext, const SharedPtr<Animation<double>>& animation,
        HeroFlightDirection flightDirection, BuildContext& fromHeroContext,
        BuildContext& toHeroContext) {
    // 上游 `_defaultHeroFlightShuttleBuilder`（heroes.dart:1086-1119）在没有
    // MediaQuery 时的分支。
    (void)flightContext;
    (void)animation;
    (void)flightDirection;
    (void)fromHeroContext;
    auto* hero = toHeroContext.element()
                         ? dynamic_cast<Hero*>(toHeroContext.element()->widget())
                         : nullptr;
    return hero ? hero->child() : nullptr;
}

}  // namespace cui_core
