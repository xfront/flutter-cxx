/// @file foundation/ticker.cpp
/// @brief Ticker — per-frame callback implementation with global active list.

#include "cui/foundation/ticker.h"

#include <algorithm>

#include "cui/foundation/application.h"

namespace cui {

// ── Global active ticker registry ───────────────────────────────────────────

std::vector<Ticker*>& Ticker::activeTickers() {
    static std::vector<Ticker*> sActive;
    return sActive;
}

void Ticker::registerSelf() {
    auto& list = activeTickers();
    // Idempotent: don't add twice.
    if (std::find(list.begin(), list.end(), this) == list.end())
        list.push_back(this);}

void Ticker::unregisterSelf() {
    auto& list = activeTickers();
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
}

// ── Ticker ──────────────────────────────────────────────────────────────────

Ticker::Ticker(Callback onTick) : onTick_(std::move(onTick)) {}

Ticker::~Ticker() {
    if (active_)
        unregisterSelf();
}

void Ticker::start() {    if (active_)
        return;
    active_ = true;
    startTime_ = std::chrono::steady_clock::now();
    lastElapsed_ = Duration{0};
    registerSelf();
    // 启动即请求一帧：动画的第一步不该等输入事件才发生
    // （否则「点一下才动一下」）。
    if (auto* app = Application::instance())
        app->scheduleRender();
}

void Ticker::stop() {    if (!active_)
        return;
    active_ = false;
    unregisterSelf();
}

Duration Ticker::elapsed() const {
    if (!active_)
        return lastElapsed_;
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<Duration>(now - startTime_);
}

void Ticker::setMuted(bool value) {
    if (value == muted_)
        return;
    muted_ = value;
    if (value)
        return;
    // 解除静音要补一帧：静音期间时间过去了，但不补帧的话动画就停在静音前
    // 那一格，要等到下一个输入事件才继续（上游 靠 shouldScheduleTick 重新投帧）。
    if (active_) {
        if (auto* app = Application::instance())
            app->scheduleRender();
    }
}

void Ticker::tick(Duration frameTime) {
    // 静音的 Ticker 不打回调，也不更新 lastElapsed_（上游 的 _tick 根本不会被调度，
    // 时钟本身仍在走，见 elapsed() 用 steady_clock 现算）。
    if (!active_ || muted_ || !onTick_)
        return;
    lastElapsed_ = elapsed();
    onTick_(lastElapsed_);
    // 注意：这里**不**主动 scheduleRender()。下一帧由事件循环统一驱动：
    // Application::run() 的 Phase 244 在有活动 Ticker 时每轮 scheduleRender()，
    // Phase 3 以剩余帧周期（kMinFrameIntervalMs）限速等待，从而每个事件循环
    // 迭代恰好渲染一次。若在此重投渲染任务，Phase 2 已渲染一次、Phase 3
    // 超时后又把它再渲染一次 → 每轮渲染两次，CPU 翻倍（动画页 86% 的根因）。
}

void Ticker::tickAllActive(Duration frameTime) {
    // Copy the list — tick callbacks may start/stop other tickers.
    auto copy = activeTickers();
    for (auto* t : copy) {
        if (t && t->isActive())
            t->tick(frameTime);
    }
}

bool Ticker::hasActiveTickers() {
    // 只看「真的在打回调」的 Ticker：被 TickerMode 静音的那条不再拉帧
    // （上游 的 shouldScheduleTick 同样带 !muted）。
    for (auto* t : activeTickers()) {
        if (t && t->isTicking())
            return true;
    }
    return false;
}

}  // namespace cui_core
