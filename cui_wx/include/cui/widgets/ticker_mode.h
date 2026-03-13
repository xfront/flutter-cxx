#pragma once
/// @file widgets/ticker_mode.h
/// @brief 上游的 TickerMode 移植（含 TickerModeData 与 Ticker 静音接线）。
///
/// 对应 上游源码：
///   上游 widgets/ticker_provider.dart：`TickerMode` / `_TickerModeState` /
///     `_EffectiveTickerMode` / `TickerModeData`
///   上游 scheduler/ticker.dart：`Ticker.muted`
///
/// 语义（与上游一致）：
///   - TickerMode 是「这一段子树里的 Ticker 要不要打 tick」的开关。
///     生效值 = 自身与全部祖先 `enabled` 之**与**、`forceFrames` 之**或**。
///   - 静音只影响回调，**时钟照走**（`Ticker::elapsed` 仍累加），
///     所以解除静音后动画会跳到「真实时间该在的位置」，而不是接着放。
///   - 树里没有 TickerMode 时按 fallback = { enabled: true, forceFrames: false }。
///
/// 与上游的结构性差异（有意偏离，勿当 bug 排查）：
///   1. 上游把接线收在 `TickerProviderStateMixin` / `SingleTickerProviderStateMixin`
///      里 —— 因为它的 Ticker 一律由 provider 创建。本仓的 State 直接
///      `make_unique<Ticker>(...)`（还没有 TickerProvider 抽象），所以这段职责
///      放在下面的 `TickerModeBinding`，由持有 Ticker 的一方显式 bind / unbind。
///      **已接线**：`ImplicitlyAnimatedWidgetState`、`TransitionRoute`（转场
///      Ticker 由每条路由自持，绑定的是宿主 Navigator 的 context，见
///      widgets/routes.h 偏离 11）。
///      渲染层自持 Ticker 的那几处（RenderAnimatedSize、RenderInteractive 的
///      惯性滚动）拿不到 BuildContext，暂未接。
///   2. 上游的 `TickerMode.of` / `getNotifier`（只回一个 bool）在 3.47 已
///      deprecated，这里保留语义等价的 `enabledOf`，不再补一套同名废接口。
///   3. `forceFrames` 只记录、不参与调度：本仓出帧由
///      `Ticker::hasActiveTickers()` 统一决定，没有「屏幕熄灭仍可强制出帧」这层。

#include <optional>
#include <utility>

#include "cui/framework/framework.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"

namespace cui {

/// 上游的 `TickerModeData`（ticker_provider.dart:615）。
struct TickerModeData {
    bool enabled = true;
    bool forceFrames = false;

    /// 上游的 `TickerModeData.fallback`：祖先里没有 TickerMode 时的默认值。
    static TickerModeData fallback() { return TickerModeData{true, false}; }

    bool operator==(const TickerModeData& other) const {
        return enabled == other.enabled && forceFrames == other.forceFrames;
    }
    bool operator!=(const TickerModeData& other) const { return !(*this == other); }
};

/// 上游私有的 `_EffectiveTickerMode`：把「已与祖先和过的」生效值发给子树。
///
/// 只有它是 InheritedWidget —— `TickerMode` 本身是 StatefulWidget，
/// 这样「祖先的值变了」能沿依赖链自然重算（见 TickerModeState）。
class EffectiveTickerMode : public InheritedWidget<TickerModeData> {
public:
    EffectiveTickerMode(SharedPtr<Widget> child, SharedPtr<const TickerModeData> values,
                        SharedPtr<ValueNotifier<TickerModeData>> valuesNotifier,
                        KeyPtr key = nullptr)
        : InheritedWidget<TickerModeData>(std::move(key), std::move(child)),
          values_(std::move(values)), valuesNotifier_(std::move(valuesNotifier)) {}

    SharedPtr<const TickerModeData> data() const override { return values_; }

    /// 上游的 `_EffectiveTickerMode.valuesNotifier`：订阅用，**不建立依赖**。
    const SharedPtr<ValueNotifier<TickerModeData>>& valuesNotifier() const {
        return valuesNotifier_;
    }

private:
    SharedPtr<const TickerModeData> values_;
    SharedPtr<ValueNotifier<TickerModeData>> valuesNotifier_;
};

/// 控制子树内 Ticker 是否出 tick。上游的 `TickerMode`。
class TickerMode : public StatefulWidget {
public:
    explicit TickerMode(SharedPtr<Widget> child, bool enabled = true,
                        bool forceFrames = false, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), child_(std::move(child)), enabled_(enabled),
          forceFrames_(forceFrames) {}

    bool enabled() const { return enabled_; }
    bool forceFrames() const { return forceFrames_; }
    const SharedPtr<Widget>& child() const { return child_; }

    /// 上游的 `TickerMode.valuesOf`：读生效值 **并建立依赖**
    /// （祖先 TickerMode 变化时本 context 会重建）。
    static TickerModeData valuesOf(BuildContext& context) {
        auto* provider = context.dependOnInheritedWidgetOfExactType<EffectiveTickerMode>();
        if (!provider)
            return TickerModeData::fallback();
        auto data = provider->data();
        return data ? *data : TickerModeData::fallback();
    }

    /// 上游 deprecated 的 `TickerMode.of`（只取 enabled 这一项）。
    static bool enabledOf(BuildContext& context) { return valuesOf(context).enabled; }

    /// 上游的 `TickerMode.getValuesNotifier`：返回可订阅源，**不建立依赖**。
    /// 没有祖先 TickerMode 时给一个恒为 fallback 的常量源（上游的
    /// `_ConstantTickerModeDataListenable`）。
    static SharedPtr<ValueNotifier<TickerModeData>> valuesNotifierOf(BuildContext& context) {
        auto* provider = context.getInheritedWidgetOfExactType<EffectiveTickerMode>();
        if (!provider || !provider->valuesNotifier())
            return constantNotifier();
        return provider->valuesNotifier();
    }

    /// 上游的 `TickerMode.merge`：给了值的项覆盖环境值，没给的沿用环境值。
    /// （上游 用 `bool?`，本仓用 `std::optional<bool>`；两者都不给就等于
    /// 透传环境值，仍要建一层是因为子树需要自己的 notifier。）
    static SharedPtr<Widget> merge(std::optional<bool> enabled,
                                   std::optional<bool> forceFrames, SharedPtr<Widget> child,
                                   KeyPtr key = nullptr) {
        // 上游借 Builder 拿到「自己位置上」的父值，再构造 TickerMode —— 同形。
        return cui::make_shared<Builder>(
                [enabled, forceFrames, child](BuildContext& context) -> SharedPtr<Widget> {
                    const TickerModeData parent = valuesOf(context);
                    return cui::make_shared<TickerMode>(
                            child, enabled.value_or(parent.enabled),
                            forceFrames.value_or(parent.forceFrames));
                },
                std::move(key));
    }

    std::unique_ptr<State> createState() override;

private:
    /// 常量源：无 TickerMode 祖先时用，永不通知。
    static const SharedPtr<ValueNotifier<TickerModeData>>& constantNotifier() {
        static const SharedPtr<ValueNotifier<TickerModeData>> sNotifier =
                cui::make_shared<ValueNotifier<TickerModeData>>(TickerModeData::fallback());
        return sNotifier;
    }

    SharedPtr<Widget> child_;
    bool enabled_ = true;
    bool forceFrames_ = false;
};

/// TickerMode 的状态：与祖先的值做「与 / 或」，再发给子树。
/// 上游的 `_TickerModeState`。
class TickerModeState : public State {
public:
    void initState() override;
    void didChangeDependencies() override;
    void didUpdateWidget(cui::Widget* oldWidget) override;
    SharedPtr<Widget> build(BuildContext& context) override;

private:
    void updateEffectiveMode();

    bool ancestorEnabled_ = TickerModeData::fallback().enabled;
    bool ancestorForceFrames_ = TickerModeData::fallback().forceFrames;
    SharedPtr<ValueNotifier<TickerModeData>> effectiveValues_;
};

/// 把 ambient TickerMode 接到一条 Ticker 上。
///
/// 上游的对应职责在 `TickerProviderStateMixin._updateTicker`：订阅
/// `TickerMode.getValuesNotifier(context)`，值一变就设 `ticker.muted`。
/// 本仓的 State 自己造 Ticker，所以显式提供这个绑定器：
///   - `bind(context, ticker)`：按当前生效值立刻设一次 muted，并订阅后续变化；
///   - `unbind()`：摘订阅（dispose 里必须调 —— 捕获 `this` 的 lambda 只能
///     走句柄通道摘除）。
///
/// 子树被移动到另一个 TickerMode 之下时，上游要求在 `activate` 里重新绑定；
/// 本仓调用方暂未接（见文件头偏离 1）。
class TickerModeBinding {
public:
    TickerModeBinding() = default;
    ~TickerModeBinding() { unbind(); }

    TickerModeBinding(const TickerModeBinding&) = delete;
    TickerModeBinding& operator=(const TickerModeBinding&) = delete;

    void bind(BuildContext& context, Ticker& ticker) {
        unbind();
        ticker_ = &ticker;
        notifier_ = TickerMode::valuesNotifierOf(context);
        applyValues();
        if (notifier_)
            handle_ = notifier_->addListenerWithHandle([this] { applyValues(); });
    }

    void unbind() {
        if (notifier_ && handle_ != 0)
            notifier_->removeListenerByHandle(handle_);
        handle_ = 0;
        notifier_.reset();
        ticker_ = nullptr;
    }

    /// 最近一次由 TickerMode 推出的静音状态。
    bool muted() const { return muted_; }

private:
    void applyValues() {
        if (!ticker_ || !notifier_)
            return;
        muted_ = !notifier_->value().enabled;
        ticker_->setMuted(muted_);
    }

    Ticker* ticker_ = nullptr;
    SharedPtr<ValueNotifier<TickerModeData>> notifier_;
    int handle_ = 0;
    bool muted_ = false;
};

// ── 内联实现 ────────────────────────────────────────────────────────────────

inline std::unique_ptr<State> TickerMode::createState() {
    return std::make_unique<TickerModeState>();
}

inline void TickerModeState::initState() {
    effectiveValues_ =
            cui::make_shared<ValueNotifier<TickerModeData>>(TickerModeData::fallback());
}

inline void TickerModeState::didChangeDependencies() {
    if (!context())
        return;
    // dependOn（不是 get）：这条依赖正是「祖先 TickerMode 变了要我重算」的通道。
    auto* provider = context()->dependOnInheritedWidgetOfExactType<EffectiveTickerMode>();
    if (provider) {
        auto data = provider->data();
        if (data) {
            ancestorEnabled_ = data->enabled;
            ancestorForceFrames_ = data->forceFrames;
        }
    } else {
        ancestorEnabled_ = TickerModeData::fallback().enabled;
        ancestorForceFrames_ = TickerModeData::fallback().forceFrames;
    }
    updateEffectiveMode();
}

inline void TickerModeState::didUpdateWidget(cui::Widget* oldWidget) {
    (void)oldWidget;
    updateEffectiveMode();
}

inline void TickerModeState::updateEffectiveMode() {
    const auto* w = static_cast<const TickerMode*>(widget());
    if (!w || !effectiveValues_)
        return;
    // enabled 取「与」、forceFrames 取「或」（上游 _updateEffectiveMode 同款）。
    effectiveValues_->setValue(
            TickerModeData{ancestorEnabled_ && w->enabled(),
                           ancestorForceFrames_ || w->forceFrames()});
}

inline SharedPtr<Widget> TickerModeState::build(BuildContext& context) {
    (void)context;
    const auto* w = static_cast<const TickerMode*>(widget());
    const SharedPtr<const TickerModeData> values = effectiveValues_
            ? cui::make_shared<const TickerModeData>(effectiveValues_->value())
            : cui::make_shared<const TickerModeData>(TickerModeData::fallback());
    return cui::make_shared<EffectiveTickerMode>(w ? w->child() : nullptr, values,
                                                 effectiveValues_);
}

}
