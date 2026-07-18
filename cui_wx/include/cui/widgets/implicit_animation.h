#pragma once
/// @file widgets/implicit_animation.h
/// @brief 隐式动画控件族 —— 对齐上游的 ImplicitlyAnimatedWidget 体系。
///
/// 上游的隐式动画只有一个机制：目标值变化时，从「当前插值结果」开始一段
/// 新动画（duration + curve），逐帧产出新值。本头文件提供同一套机制：
///
///   ImplicitTweenDriver   单值插值器（当前值 → 目标值）
///   ImplicitlyAnimatedWidget
///                        隐式动画基类。子类在 build() 里通过
///                        `tween(key, target)` 取得插值结果；State 自动驱动
///                        Ticker，动画期间按帧重建子树。
///   TweenAnimationBuilder
///                        通用隐式动画构建器（数值型）。
///   AnimatedOpacity / AnimatedScale / AnimatedRotation / AnimatedSlide
///   AnimatedPadding / AnimatedAlign / AnimatedPositioned
///   AnimatedContainer / AnimatedCrossFade
///   AnimatedSwitcher     例外：它是上游那个 StatefulWidget（每子一份
///                        controller 的 `_ChildEntry` 家族），不走本头的
///                        `tween(key, target)` 机制，只是同族控件放在这里。
///
/// 用法：
///   auto op = cui_core::make_shared<AnimatedOpacity>(0.2, child);
///   // 之后把 op 的 opacity 改成 1.0 → 自动淡入。

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "../../../../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/framework.h"
#include "cui/widgets/animated_size.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/container.h"
#include "cui/widgets/ticker_mode.h"

namespace cui {

// ── 插值工具 ────────────────────────────────────────────────────────────────

/// 线性插值（不做缓动；缓动由调用方施加）。
double lerpDouble(double a, double b, double t);

/// 颜色插值（逐通道，含 alpha）。
Color lerpColor(const Color& a, const Color& b, double t);

/// 四边内边距插值。
EdgeInsets lerpEdgeInsets(const EdgeInsets& a, const EdgeInsets& b, double t);

/// 尺寸插值。
Size lerpSize(const Size& a, const Size& b, double t);

/// 偏移插值。
Offset lerpOffset(const Offset& a, const Offset& b, double t);

/// 对齐插值（x/y 各自线性）。
Alignment lerpAlignment(const Alignment& a, const Alignment& b, double t);

/// 盒装饰插值（背景色 / 圆角 / 边框宽 / 边框色）。
BoxDecoration lerpBoxDecoration(const BoxDecoration& a, const BoxDecoration& b, double t);

// ── ImplicitTweenDriver ─────────────────────────────────────────────────────

/// 单值隐式插值器：从「当前值」补间到「目标值」。
///
/// 目标变化时从当前插值结果重新开始一段动画（duration + curve）；动画结束
/// 后 current_ 精确落在 target_ 上。
class ImplicitTweenDriver {
public:
    ImplicitTweenDriver(double initial = 0.0, double durationMs = 200.0,
                         Easing curve = Easing::EaseInOutCubic);

    double duration() const { return durationMs_; }
    void setDuration(double ms) { durationMs_ = ms; }

    Easing curve() const { return curve_; }
    void setCurve(Easing e) { curve_ = e; }

    /// 设定目标值。发生变化时重启动画并返回 true。
    bool setTarget(double target);

    /// 当前插值结果；无动画在跑时等于目标值。
    double value() const { return current_; }
    double target() const { return target_; }
    double begin() const { return begin_; }

    bool isRunning() const { return running_; }

    /// 推进 dtMs 毫秒；返回 true 表示动画仍在运行。
    bool advance(double dtMs);

    /// 立即跳到目标值并结束动画。
    void jumpToTarget();

    /// 强制把插值器复位到指定值（不产生动画），下一次 setTarget 会重新补间。
    void resetTo(double value);

private:
    double durationMs_ = 200.0;
    Easing curve_ = Easing::EaseInOutCubic;
    double begin_ = 0.0;
    double target_ = 0.0;
    double current_ = 0.0;
    double elapsed_ = 0.0;
    bool running_ = false;
};

// ── ImplicitlyAnimatedWidget ────────────────────────────────────────────────

/// 隐式动画基类。
///
/// 提供 duration / curve 配置；真正的插值状态在 ImplicitlyAnimatedWidgetState。
class ImplicitlyAnimatedWidget : public StatefulWidget {
public:
    ImplicitlyAnimatedWidget(double durationMs = 200.0,
                             Easing curve = Easing::EaseInOutCubic, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), durationMs_(durationMs), curve_(curve) {}

    double duration() const { return durationMs_; }
    void setDuration(double ms) { durationMs_ = ms; }

    Easing curve() const { return curve_; }
    void setCurve(Easing e) { curve_ = e; }

    std::unique_ptr<State> createState() override;

protected:
    double durationMs_;
    Easing curve_;
};

/// 隐式动画 State：持有各属性的插值器，并用 Ticker 逐帧推进。
class ImplicitlyAnimatedWidgetState : public State {
public:
    void initState() override;
    void dispose() override;

    /// 上游的 `TickerProviderStateMixin.didChangeDependencies`：取（或换）
    /// ambient TickerMode 的 notifier，接到本 State 自己造的 ticker 上。
    void didChangeDependencies() override;

    /// 基类不产出子树；子类必须覆写。
    SharedPtr<Widget> build(BuildContext& context) override { return nullptr; }

protected:
    /// 取得（或创建）指定 key 的插值器，设定目标值并返回当前插值结果。
    ///
    /// 首次调用该 key 时插值器直接落在目标值上（无入场动画）；之后目标变化
    /// 会从当前插值结果开始新的补间。
    double tween(const std::string& key, double target);

    /// 取当前插值结果（不设定目标）。
    double tweenValue(const std::string& key) const;

    /// 让指定 key 的插值器从 from 重新补间到 to。
    ///
    /// 用于「上一次值 → 当前值」两端式插值（如 AnimatedContainer 的各属性）：
    /// 调用方把上一次的属性值记下来，属性变化时用本方法重启插值器，
    /// 再用 lerpDouble / lerpColor 等按进度插值出中间值。
    void restartTween(const std::string& key, double from, double to);

    /// 是否还有动画在跑。
    bool hasRunningTween() const;

    /// 立即把所有插值器跳到各自的目标值。
    void jumpToTargets();

    /// 一段动画收尾时回调一次（ticker 发现「全部不再运行」而停表那一刻）。
    ///
    /// 对齐上游 `AnimationController` 的 status 监听：`completed` / `dismissed`
    /// 各触发一次，且发生在 build 之外 —— 上游 `AnimatedCrossFade.onEnd` 就挂
    /// 在那里，所以本仓也必须在 ticker 回调里发，不能塞进 build()。
    virtual void onTweenSettled() {}

    double duration() const;
    Easing curve() const;

private:
    struct Entry {
        SharedPtr<ImplicitTweenDriver> driver;
    };

    std::map<std::string, Entry> tweens_;
    std::unique_ptr<Ticker> ticker_;
    double lastElapsedMs_ = 0.0;
    bool rebuildPending_ = false;
    /// 上游 TickerProviderStateMixin 的那段接线：ambient TickerMode 一变
    /// 就给 ticker_ 设 muted（静音时时钟照走、回调不打）。
    TickerModeBinding tickerMode_;
};

// ── TweenAnimationBuilder ───────────────────────────────────────────────────

/// 通用隐式动画构建器（数值型）。
///
/// 对齐上游 `TweenAnimationBuilder`：目标值变化时，builder 以插值结果重建
/// 子树。
///
///   auto b = cui_core::make_shared<TweenAnimationBuilder>(
///       0.0, 1.0,
///       [](BuildContext& ctx, double v) {
///           return cui_core::make_shared<Opacity>(v, child);
///       });
class TweenAnimationBuilder : public ImplicitlyAnimatedWidget {
public:
    using Builder = std::function<SharedPtr<Widget>(BuildContext&, double)>;

    TweenAnimationBuilder(double target, Builder builder,
                          double durationMs = 200.0,
                          Easing curve = Easing::EaseInOutCubic, KeyPtr key = nullptr)
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          target_(target), builder_(std::move(builder)) {}

    double target() const { return target_; }
    void setTarget(double t) { target_ = t; }

    const Builder& builder() const { return builder_; }

    std::unique_ptr<State> createState() override;

private:
    double target_;
    Builder builder_;
};

class TweenAnimationBuilderState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── AnimatedOpacity ─────────────────────────────────────────────────────────

/// 透明度隐式动画。
class AnimatedOpacity : public ImplicitlyAnimatedWidget {
public:
    AnimatedOpacity(double opacity, SharedPtr<Widget> child = nullptr,
                    double durationMs = 200.0, Easing curve = Easing::EaseInOutCubic,
                    KeyPtr key = nullptr)
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          opacity_(opacity), child_(std::move(child)) {}

    double opacity() const { return opacity_; }
    void setOpacity(double o) { opacity_ = o; }
    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    std::unique_ptr<State> createState() override;

private:
    double opacity_;
    SharedPtr<Widget> child_;
};

class AnimatedOpacityState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── AnimatedScale ───────────────────────────────────────────────────────────

/// 缩放隐式动画。上游 `AnimatedScale`：默认绕盒子中心缩放，缩放枢轴可用
/// alignment 指定（对齐上游同名参数，透传给 ScaleTransition）。
class AnimatedScale : public ImplicitlyAnimatedWidget {
public:
    AnimatedScale(double scale, SharedPtr<Widget> child = nullptr,
                  double durationMs = 200.0, Easing curve = Easing::EaseInOutCubic,
                  KeyPtr key = nullptr, Alignment alignment = Alignment::center())
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          scale_(scale), child_(std::move(child)), alignment_(alignment) {}

    double scale() const { return scale_; }
    void setScale(double s) { scale_ = s; }
    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }
    Alignment alignment() const { return alignment_; }

    std::unique_ptr<State> createState() override;

private:
    double scale_;
    SharedPtr<Widget> child_;
    Alignment alignment_;
};

class AnimatedScaleState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── AnimatedRotation ────────────────────────────────────────────────────────

/// 旋转隐式动画。上游 `AnimatedRotation`：turns 为圈数，默认绕盒子中心旋转，
/// 枢轴可用 alignment 指定。
class AnimatedRotation : public ImplicitlyAnimatedWidget {
public:
    AnimatedRotation(double turns, SharedPtr<Widget> child = nullptr,
                     double durationMs = 200.0, Easing curve = Easing::EaseInOutCubic,
                     KeyPtr key = nullptr, Alignment alignment = Alignment::center())
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          turns_(turns), child_(std::move(child)), alignment_(alignment) {}

    /// 圈数（1.0 = 一整圈）。
    double turns() const { return turns_; }
    void setTurns(double t) { turns_ = t; }
    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }
    Alignment alignment() const { return alignment_; }

    std::unique_ptr<State> createState() override;

private:
    double turns_;
    SharedPtr<Widget> child_;
    Alignment alignment_;
};

class AnimatedRotationState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── AnimatedSlide ───────────────────────────────────────────────────────────

/// 相对位移隐式动画（按自身尺寸的比例）。
class AnimatedSlide : public ImplicitlyAnimatedWidget {
public:
    AnimatedSlide(Offset offset, SharedPtr<Widget> child = nullptr,
                  double durationMs = 200.0, Easing curve = Easing::EaseInOutCubic,
                  KeyPtr key = nullptr)
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          offset_(offset), child_(std::move(child)) {}

    Offset offset() const { return offset_; }
    void setOffset(Offset o) { offset_ = o; }
    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    std::unique_ptr<State> createState() override;

private:
    Offset offset_;
    SharedPtr<Widget> child_;
};

class AnimatedSlideState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── AnimatedPadding ─────────────────────────────────────────────────────────

/// 内边距隐式动画。
class AnimatedPadding : public ImplicitlyAnimatedWidget {
public:
    AnimatedPadding(EdgeInsets padding, SharedPtr<Widget> child = nullptr,
                    double durationMs = 200.0, Easing curve = Easing::EaseInOutCubic,
                    KeyPtr key = nullptr)
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          padding_(padding), child_(std::move(child)) {}

    EdgeInsets padding() const { return padding_; }
    void setPadding(EdgeInsets p) { padding_ = p; }
    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    std::unique_ptr<State> createState() override;

private:
    EdgeInsets padding_;
    SharedPtr<Widget> child_;
};

class AnimatedPaddingState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── AnimatedAlign ───────────────────────────────────────────────────────────

/// 对齐隐式动画。
class AnimatedAlign : public ImplicitlyAnimatedWidget {
public:
    AnimatedAlign(Alignment alignment, SharedPtr<Widget> child = nullptr,
                  double durationMs = 200.0, Easing curve = Easing::EaseInOutCubic,
                  KeyPtr key = nullptr)
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          alignment_(alignment), child_(std::move(child)) {}

    Alignment alignment() const { return alignment_; }
    void setAlignment(Alignment a) { alignment_ = a; }
    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    std::unique_ptr<State> createState() override;

private:
    Alignment alignment_;
    SharedPtr<Widget> child_;
};

class AnimatedAlignState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── AnimatedPositioned ──────────────────────────────────────────────────────

/// Stack 内定位的隐式动画（left/top/right/bottom/width/height）。
///
/// 用法：作为 Stack 的子节点使用。
class AnimatedPositioned : public ImplicitlyAnimatedWidget {
public:
    AnimatedPositioned(SharedPtr<Widget> child = nullptr,
                       double durationMs = 200.0, Easing curve = Easing::EaseInOutCubic,
                       KeyPtr key = nullptr)
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          child_(std::move(child)) {}

    /// 定位参数（nullopt = 不约束该边）。
    std::optional<double> left, top, right, bottom, width, height;

    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Widget> child_;
};

class AnimatedPositionedState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;
};

// ── AnimatedContainer ───────────────────────────────────────────────────────

/// 容器隐式动画：尺寸 / 内边距 / 外边距 / 对齐 / 装饰 / 约束 逐项插值。
///
/// 对齐上游 `AnimatedContainer` 的做法（各属性分别 Tween，再组合）。
class AnimatedContainer : public ImplicitlyAnimatedWidget {
public:
    AnimatedContainer(SharedPtr<Widget> child = nullptr,
                     double durationMs = 200.0, Easing curve = Easing::EaseInOutCubic,
                     KeyPtr key = nullptr)
        : ImplicitlyAnimatedWidget(durationMs, curve, std::move(key)),
          child_(std::move(child)) {}

    std::optional<double> width, height;
    EdgeInsets padding;
    EdgeInsets margin;
    std::optional<Alignment> alignment;
    BoxDecoration decoration;
    std::optional<BoxConstraints> constraints;

    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Widget> child_;
};

class AnimatedContainerState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    // prev* = 当前的目标值；begin* = 本次补间的起点（属性变化时的旧值）。
    std::optional<double> prevWidth_, prevHeight_;
    std::optional<double> beginWidth_, beginHeight_;
    EdgeInsets prevPadding_, beginPadding_;
    EdgeInsets prevMargin_, beginMargin_;
    std::optional<Alignment> prevAlignment_, beginAlignment_;
    BoxDecoration prevDecoration_, beginDecoration_;
    bool initialized_ = false;
};

// ── AnimatedSwitcher ────────────────────────────────────────────────────────

/// 上游的 `AnimatedSwitcherTransitionBuilder` / `AnimatedSwitcherLayoutBuilder`。
///
/// 偏离：上游把 `Animation<double>` 交给 builder（builder 造一个订阅它的
/// `FadeTransition`，渲染对象自己逐帧取值）；本仓的 RenderObject 不订阅响应流，
/// 走「每帧重建」路径（同 widgets/transition.h 的既有口径），所以这里直接给
/// 当帧的数值。
using AnimatedSwitcherTransitionBuilder = std::function<SharedPtr<Widget>(
    SharedPtr<Widget> child, double animationValue)>;
using AnimatedSwitcherLayoutBuilder = std::function<SharedPtr<Widget>(
    SharedPtr<Widget> currentChild, std::vector<SharedPtr<Widget>> previousChildren)>;

/// 子控件切换时的过渡：新子渐显、**所有**旧子并行渐隐。
///
/// 对应 上游 `widgets/animated_switcher.dart`。骨架照抄：
///   * 每个子节点一份 `_ChildEntry`（自带 controller + 曲线 + 稳定 key），
///     所以同时可以有任意多个子节点在淡出（旧实现只有一个 `outgoing_`）。
///   * 「换子」的判定是 `Widget::canUpdate`（同类型 + 同 key），不是指针比较。
///   * entry 在**创建时**抓一份 duration/曲线 —— 上游注释明说「改 duration 不
///     影响进行中的过渡」。
///   * 淡出到 0（上游 `AnimationStatus.dismissed`）时把 entry 从列表里摘掉。
///   * 每个 transition 外面补一层 `KeyedSubtree(key: childNumber)`，等价上游
///     的 `KeyedSubtree.wrap(builder(child, animation), _childNumber)`：children
///     列表增删位移时 Element 靠它认人。
///
/// 有意未移植：`reverseDuration`（本仓 `ImplicitTweenDriver` 不区分正反）。
class AnimatedSwitcher : public StatefulWidget {
public:
    explicit AnimatedSwitcher(SharedPtr<Widget> child = nullptr, double durationMs = 200.0,
                              Easing switchInCurve = Easing::Linear,
                              Easing switchOutCurve = Easing::Linear,
                              AnimatedSwitcherTransitionBuilder transitionBuilder = nullptr,
                              AnimatedSwitcherLayoutBuilder layoutBuilder = nullptr,
                              KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)), durationMs_(durationMs),
          switchInCurve_(switchInCurve), switchOutCurve_(switchOutCurve),
          transitionBuilder_(std::move(transitionBuilder)),
          layoutBuilder_(std::move(layoutBuilder)) {}

    SharedPtr<Widget> child() const { return child_; }
    void setChild(SharedPtr<Widget> c) { child_ = std::move(c); }

    double duration() const { return durationMs_; }
    void setDuration(double ms) { durationMs_ = ms; }

    Easing switchInCurve() const { return switchInCurve_; }
    Easing switchOutCurve() const { return switchOutCurve_; }
    const AnimatedSwitcherTransitionBuilder& transitionBuilder() const { return transitionBuilder_; }
    const AnimatedSwitcherLayoutBuilder& layoutBuilder() const { return layoutBuilder_; }

    /// 上游的 `AnimatedSwitcher.defaultTransitionBuilder`：`FadeTransition`。
    /// 上游给它挂 `ValueKey(child.key)` ⇒ 同一个子控件换进来时 Element 复用。
    static SharedPtr<Widget> defaultTransitionBuilder(SharedPtr<Widget> child,
                                                            double animationValue);

    /// 上游的 `AnimatedSwitcher.defaultLayoutBuilder`：`Stack` 居中，
    /// 先铺 previousChildren、再叠 currentChild（所以新子画在上面）。
    static SharedPtr<Widget> defaultLayoutBuilder(
        SharedPtr<Widget> currentChild, std::vector<SharedPtr<Widget>> previousChildren);

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Widget> child_;
    double durationMs_ = 200.0;
    Easing switchInCurve_ = Easing::Linear;
    Easing switchOutCurve_ = Easing::Linear;
    AnimatedSwitcherTransitionBuilder transitionBuilder_;
    AnimatedSwitcherLayoutBuilder layoutBuilder_;
};

class AnimatedSwitcherState : public State {
public:
    void initState() override;
    void dispose() override;
    void didChangeDependencies() override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    /// 上游的 `_ChildEntry`。
    struct Entry {
        SharedPtr<Widget> widgetChild;
        SharedPtr<ImplicitTweenDriver> driver;  // 0..1 == controller.value
        KeyPtr transitionKey;                         // == KeyedSubtree(childNumber)
    };

    /// 上游的 `_addEntryForNewChild`：把 current 转成 outgoing 并（可选）起新动画。
    void addEntryForNewChild(bool animate);

    /// 上游的 `_newEntry` 里那个 status 监听：dismissed 即摘除。
    void dropSettledOutgoing();

    SharedPtr<Widget> transitionFor(const SharedPtr<Entry>& entry, double value);

    SharedPtr<Entry> current_;
    std::vector<SharedPtr<Entry>> outgoing_;
    int childNumber_ = 0;
    std::unique_ptr<Ticker> ticker_;
    double lastElapsedMs_ = 0.0;
    TickerModeBinding tickerMode_;
};

// ── AnimatedCrossFade ───────────────────────────────────────────────────────

/// 显示哪个子节点。上游的 `CrossFadeState`（animated_cross_fade.dart）。
enum class CrossFadeState {
    ShowFirst,
    ShowSecond,
};

/// 上游的 `AnimatedCrossFadeBuilder`：把 top/bottom 两个子节点摆进同一容器。
using AnimatedCrossFadeBuilder = std::function<SharedPtr<Widget>(
    SharedPtr<Widget> topChild, KeyPtr topChildKey, SharedPtr<Widget> bottomChild,
    KeyPtr bottomChildKey)>;

/// 两个子控件交叉淡化，同时把自身尺寸动画到正在显示的那一个。
///
/// 对应 上游 `widgets/animated_cross_fade.dart`。上游的骨架是「一个
/// AnimationController（0→1 对时间线性）驱动三条曲线」：
///   firstCurve  作用在 controller 上再整体取反（1-v）⇒ 淡出者
///   secondCurve 作用在 controller 上 ⇒ 淡入者
///   sizeCurve   交给下层 AnimatedSize ⇒ 尺寸补间
/// 本仓 `ImplicitTweenDriver` 的曲线在创建时取自 widget，所以
/// `AnimatedCrossFade` 把基类的 curve 钉成 `Linear`：`tween("progress", …)`
/// 拿到的就是 `controller.value`，三条曲线各自在上面施加。
///
/// 有意未移植（上游有、这里没有）：
///   * `reverseDuration`（本仓驱动不区分正反，见 ImplicitTweenDriver）
///   * `excludeBottomFocus` + `ExcludeFocus`/`ExcludeSemantics`/`TickerMode`
///     三件套包装（本仓没有对应控件），淡出者只保留 `IgnorePointer`
///   * 上游 AnimationController 的 `forward()` 按「剩余距离」缩放时长，
///     本仓固定走整段 duration —— 这是 ImplicitTweenDriver 的全局口径
class AnimatedCrossFade : public ImplicitlyAnimatedWidget {
public:
    explicit AnimatedCrossFade(SharedPtr<Widget> first, SharedPtr<Widget> second,
                               CrossFadeState state = CrossFadeState::ShowFirst,
                               double durationMs = 100.0, Easing firstCurve = Easing::Linear,
                               Easing secondCurve = Easing::Linear,
                               Easing sizeCurve = Easing::Linear,
                               Alignment alignment = Alignment::topCenter(),
                               Clip clipBehavior = Clip::HardEdge,
                               AnimatedCrossFadeBuilder layoutBuilder = nullptr,
                               std::function<void()> onEnd = nullptr, KeyPtr key = nullptr)
        : ImplicitlyAnimatedWidget(durationMs, Easing::Linear, std::move(key)),
          first_(std::move(first)), second_(std::move(second)), state_(state),
          firstCurve_(firstCurve), secondCurve_(secondCurve), sizeCurve_(sizeCurve),
          alignment_(alignment), clipBehavior_(clipBehavior),
          layoutBuilder_(std::move(layoutBuilder)), onEnd_(std::move(onEnd)) {}

    SharedPtr<Widget> first() const { return first_; }
    SharedPtr<Widget> second() const { return second_; }
    void setFirst(SharedPtr<Widget> w) { first_ = std::move(w); }
    void setSecond(SharedPtr<Widget> w) { second_ = std::move(w); }

    CrossFadeState crossFadeState() const { return state_; }
    void setCrossFadeState(CrossFadeState s) { state_ = s; }

    Easing firstCurve() const { return firstCurve_; }
    Easing secondCurve() const { return secondCurve_; }
    Easing sizeCurve() const { return sizeCurve_; }
    Alignment alignment() const { return alignment_; }
    Clip clipBehavior() const { return clipBehavior_; }
    const AnimatedCrossFadeBuilder& layoutBuilder() const { return layoutBuilder_; }
    const std::function<void()>& onEnd() const { return onEnd_; }

    /// 上游的 `AnimatedCrossFade.defaultLayoutBuilder`：`Stack` 里 bottom 用
    /// `Positioned(left/top/right = 0)` 横向拉满、不锁高度，top 是**非定位**
    /// 子（按 Stack 的 fit 摆放）。两个 key 挂在 `Positioned` 上，于是交换
    /// top/bottom 时 Element 能按 key 复用，两棵子树不会被重建。
    static SharedPtr<Widget> defaultLayoutBuilder(SharedPtr<Widget> topChild,
                                                        KeyPtr topChildKey,
                                                        SharedPtr<Widget> bottomChild,
                                                        KeyPtr bottomChildKey);

    std::unique_ptr<State> createState() override;

private:
    SharedPtr<Widget> first_;
    SharedPtr<Widget> second_;
    CrossFadeState state_;
    Easing firstCurve_;
    Easing secondCurve_;
    Easing sizeCurve_;
    Alignment alignment_;
    Clip clipBehavior_;
    AnimatedCrossFadeBuilder layoutBuilder_;
    std::function<void()> onEnd_;
};

class AnimatedCrossFadeState : public ImplicitlyAnimatedWidgetState {
public:
    SharedPtr<Widget> build(BuildContext& context) override;

protected:
    /// 上游把 `onEnd` 挂在 controller 的 status 监听上（completed/dismissed）。
    void onTweenSettled() override;
};

}  // namespace cui_core
