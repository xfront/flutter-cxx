#pragma once
/// @file utils/future.h
/// @brief 协程友好的 Future / Stream 原语。
///
/// 与仓库既有的 `Task<T>`（utils/task.h）配套，构成完整的协程异步三件套：
///
///   - `Task<T>`   eager-start 的协程任务：调用即开始执行，直到第一个挂起点。
///   - `Future<T>` 可 `co_await` 的「将来值」：可在任意线程完成，等待方在
///                 GUI 线程恢复。
///   - `Stream<T>` 可 `co_await stream.next()` 逐项消费的流。
///
/// 用法：
///   Future<std::string> loadText() {
///       return Future<std::string>::fromAsync([] { return readFile(); });
///   }
///
///   Task<void> show() {
///       auto text = co_await loadText();
///       // ……
///   }
///
///   Task<void> consume(Stream<int>& s) {
///       while (auto v = co_await s.next())
///           handle(*v);
///   }

#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "cui/foundation/application.h"
#include "cui/utils/task.h"
#include "shared_ptr.h"

namespace cui {

/// 在 GUI 线程恢复一个挂起的协程句柄。
///
/// 已在 GUI 线程时立即恢复；否则投递到事件循环（避免后台线程直接操作 UI）。
inline void resumeOnGuiThread(std::coroutine_handle<> handle) {
    if (!handle || handle.done())
        return;
    if (auto* app = Application::instance()) {
        // 已在 GUI 线程，或事件循环没在跑（离屏 / 测试）→ 直接恢复。
        if (app->isGuiThread() || !app->isRunning()) {
            handle.resume();
        } else {
            app->postTask([handle] {
                if (handle && !handle.done())
                    handle.resume();
            });
        }
    } else {
        handle.resume();
    }
}

// ── Future<T> ───────────────────────────────────────────────────────────────

/// 可 `co_await` 的将来值。
///
/// 完成（setValue / setError）可在任意线程调用；等待方在 GUI 线程恢复，
/// 因此协程里可以直接触碰界面。
///
/// 内部状态放在堆上（shared_ptr），因此 Future 本身可拷贝可移动 —— 拷贝后
/// 双方共享同一个「将来值」。
template <typename T>
class Future {
public:
    Future() : state_(std::make_shared<State>()) {}

    /// 已经完成的 Future（立即可 await）。
    static Future<T> completed(T value) {
        Future<T> f;
        f.state_->value = std::move(value);
        f.state_->done = true;
        return f;
    }

    /// 已经失败的 Future。
    static Future<T> failed(std::exception_ptr e) {
        Future<T> f;
        f.state_->error = std::move(e);
        f.state_->done = true;
        return f;
    }

    /// 在线程池执行 work，完成后在 GUI 线程恢复等待方。
    static Future<T> fromAsync(std::function<T()> work) {
        Future<T> f;
        if (auto* app = Application::instance()) {
            app->runAsync(
                [work = std::move(work)]() { return work(); },
                [&f](T v) { f.setValue(std::move(v)); });
        }
        return f;
    }

    // ── Awaitable ──────────────────────────────────────────────────────────

    bool await_ready() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->done;
    }

    bool await_suspend(std::coroutine_handle<> caller) {
        bool resumeNow = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->done) {
                resumeNow = true;
            } else {
                state_->continuation = caller;
            }
        }
        // false = 不挂起，协程立即继续；true = 挂起，等 setValue 恢复。
        return !resumeNow;
    }

    T await_resume() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->error)
            std::rethrow_exception(state_->error);
        return std::move(state_->value.value_or(T{}));
    }

    // ── Completion ─────────────────────────────────────────────────────────

    /// 完成（任意线程可调用）。返回 true 表示本次调用真正完成了它。
    bool setValue(T value) {
        std::coroutine_handle<> cont;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->done)
                return false;
            state_->value = std::move(value);
            state_->done = true;
            cont = state_->continuation;
            state_->continuation = nullptr;
        }
        if (cont)
            resumeOnGuiThread(cont);
        return true;
    }

    /// 以异常完成（任意线程可调用）。
    bool setError(std::exception_ptr e) {
        std::coroutine_handle<> cont;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->done)
                return false;
            state_->error = std::move(e);
            state_->done = true;
            cont = state_->continuation;
            state_->continuation = nullptr;
        }
        if (cont)
            resumeOnGuiThread(cont);
        return true;
    }

    bool isDone() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->done;
    }

    std::optional<T> peek() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->value;
    }

private:
    struct State {
        mutable std::mutex mutex;
        std::optional<T> value;
        std::exception_ptr error;
        std::coroutine_handle<> continuation;
        bool done = false;
    };

    std::shared_ptr<State> state_;
};

// ── Stream<T> ───────────────────────────────────────────────────────────────

/// 可 `co_await stream.next()` 逐项消费的流。
///
/// 订阅方（生产者）通过 add / addError / close 推送；消费方在协程里循环
/// `co_await stream.next()`，流关闭时返回 nullopt。
template <typename T>
class Stream {
private:
    /// 共享的内部状态（放在堆上，Stream 因此可拷贝可移动）。
    struct State {
        mutable std::mutex mutex;
        std::deque<T> queue;
        std::exception_ptr error;
        std::coroutine_handle<> waiter;
        bool closed = false;
    };

public:
    using ValueCallback = std::function<void(const T&)>;
    using ErrorCallback = std::function<void(std::exception_ptr)>;
    using DoneCallback = std::function<void()>;

    Stream() : state_(std::make_shared<State>()) {}

    // ── 生产端 ─────────────────────────────────────────────────────────────

    /// 推送一个值（任意线程可调用）。
    void add(T value) {
        std::coroutine_handle<> waiter;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->queue.push_back(std::move(value));
            waiter = takeWaiterLocked();
        }
        if (waiter)
            resumeOnGuiThread(waiter);
    }

    /// 推送一个错误（任意线程可调用）。
    void addError(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->error = std::move(e);
    }

    /// 关闭流（消费方的 next() 返回 nullopt）。
    void close() {
        std::coroutine_handle<> waiter;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->closed = true;
            waiter = takeWaiterLocked();
        }
        if (waiter)
            resumeOnGuiThread(waiter);
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->closed;
    }

    // ── 消费端（协程） ─────────────────────────────────────────────────────

    /// 等待下一个值；流结束时返回 nullopt。
    class NextAwaiter {
    public:
        explicit NextAwaiter(std::shared_ptr<State> state) : state_(std::move(state)) {}

        bool await_ready() const {
            std::lock_guard<std::mutex> lock(state_->mutex);
            return !state_->queue.empty() || state_->closed;
        }

        bool await_suspend(std::coroutine_handle<> caller) {
            bool resumeNow = false;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (!state_->queue.empty() || state_->closed) {
                    resumeNow = true;
                } else {
                    state_->waiter = caller;
                }
            }
            // false = 不挂起，协程立即继续；true = 挂起，等 add/close 恢复。
            return !resumeNow;
        }

        std::optional<T> await_resume() {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->queue.empty()) {
                T value = std::move(state_->queue.front());
                state_->queue.pop_front();
                return value;
            }
            if (state_->error)
                std::rethrow_exception(state_->error);
            return std::nullopt;
        }

    private:
        std::shared_ptr<State> state_;
    };

    /// 等待下一个值（协程里 `co_await stream.next()`）。
    NextAwaiter next() { return NextAwaiter(state_); }

private:
    /// 取走当前等待者（调用方须持锁）。
    std::coroutine_handle<> takeWaiterLocked() {
        std::coroutine_handle<> w = state_->waiter;
        state_->waiter = nullptr;
        return w;
    }

    std::shared_ptr<State> state_;
};

}  // namespace cui_core
