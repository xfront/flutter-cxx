#pragma once
/// @file foundation/ticker.h
/// @brief Ticker — a per-frame callback driven by SchedulerBinding.
///
/// A Ticker is the structural replacement for the old "advance animation in
/// paint()" pattern.  Instead of pushing time forward during rendering (which
/// caused animation stalls when a frame was dropped), Tickers are advanced
/// by the SchedulerBinding at the start of each frame (handleBeginFrame),
/// before any layout or paint happens.
///
/// Tickers self-register with a global active-ticker list when started and
/// unregister when stopped.  SchedulerBinding::handleBeginFrame iterates
/// this list and calls tick() on each active ticker.

#include <chrono>
#include <functional>
#include <vector>

namespace cui {

/// Duration type used by Ticker callbacks (milliseconds since ticker start).
using Duration = std::chrono::duration<double, std::milli>;

/// A per-frame callback.  The SchedulerBinding calls `tick()` once per frame
/// while the ticker is active.
class Ticker {
public:
    using Callback = std::function<void(Duration elapsed)>;

    explicit Ticker(Callback onTick);
    ~Ticker();

    /// Start ticking.  Resets the elapsed counter.  Registers with the
    /// global active-ticker list.
    void start();

    /// Stop ticking.  Unregisters from the global list.
    void stop();

    /// True while the ticker is active (between start() and stop()).
    bool isActive() const { return active_; }

    /// 上游的 `Ticker.muted`。
    ///
    /// 静音时**时钟照走**（`elapsed()` 继续累加），只是不再打回调。
    /// 按上游约定，这个开关由创建 Ticker 的一方（TickerProvider，如
    /// NavigatorState / TickerMode）控制，而不是监听 ticks 的一方。
    bool muted() const { return muted_; }
    void setMuted(bool value);

    /// 上游的 `Ticker.isTicking`：active 且未静音，也就是「真的在出 tick」。
    /// `muted` 的 Ticker 是 active 但不 ticking。
    bool isTicking() const { return active_ && !muted_; }

    /// The elapsed time since the last start() call.
    Duration elapsed() const;

    /// Called by SchedulerBinding once per frame.
    void tick(Duration frameTime);

    /// The last elapsed time passed to the callback.
    Duration lastElapsed() const { return lastElapsed_; }

    /// Tick all active tickers.  Called by SchedulerBinding::handleBeginFrame.
    static void tickAllActive(Duration frameTime);

    /// True while at least one ticker is **ticking** (i.e. an animation is in
    /// flight).  事件循环据此决定是否继续出帧 —— 否则动画会停在原地，
    /// 只有输入事件才会让它再走一步。
    /// 上游口径：`Ticker.shouldScheduleTick => !muted && isActive && !scheduled`
    /// ⇒ 被 `TickerMode` 静音的 Ticker 不拉帧（时间仍在走，见 `muted`）。
    static bool hasActiveTickers();

private:
    void registerSelf();
    void unregisterSelf();

    Callback onTick_;
    bool active_ = false;
    bool muted_ = false;
    std::chrono::steady_clock::time_point startTime_;
    Duration lastElapsed_{0};

    /// Global list of active tickers (raw pointers; tickers own themselves).
    static std::vector<Ticker*>& activeTickers();
};

}  // namespace cui_core
