#pragma once
/// @file task.h
/// @brief Task<T> — C++20 coroutine return type for async GUI operations.
///
/// Analogous to:
///   C#:       Task<T> + async/await
///   Kotlin:   Deferred<T> + suspend fun
///   JS:       Promise<T> + async/await
///
/// Key design choices:
///   - Lazy start: the body does not run until start() (or until the task is
///     awaited) — so the body always executes on the GUI thread
///   - Continuation chaining: final_suspend resumes the caller
///   - GUI-thread resumption: start() schedules via scheduleTaskResume()
///
/// Usage:
///   Task<std::string> loadFile() {
///       auto content = co_await readFileAsync("data.txt");
///       co_return content;
///   }
///
///   auto task = loadFile();
///   task.start();          // body runs on the GUI thread
///
/// Usage:
///   Task<std::string> loadFile() {
///       auto content = co_await readFileAsync("data.txt");
///       co_return content;
///   }

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace cui {

class Application;

/// Schedule a coroutine handle for resumption on the GUI event loop.
/// Implemented in application.cpp — avoids circular include dependency.
void scheduleTaskResume(std::coroutine_handle<> handle);

// ── Task<T> for non-void types ─────────────────────────────────────────────

template <typename T>
class Task {
public:
    struct promise_type {
        Task get_return_object() { return Task{Handle::from_promise(*this)}; }

        /// 惰性启动：协程体在 start()（或被 await）之前不执行。
        std::suspend_always initial_suspend() { return {}; }

        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
                    auto& prom = Handle::from_address(h.address()).promise();
                    if (prom.continuation_) {
                        return prom.continuation_;
                    }
                    return std::noop_coroutine();
                }

                void await_resume() noexcept {}
            };
            return FinalAwaiter{};
        }

        void return_value(T value) { result_ = std::move(value); }

        void unhandled_exception() { exception_ = std::current_exception(); }

        // Members
        std::optional<T> result_;
        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_;
    };

    using Handle = std::coroutine_handle<promise_type>;

    // ── Constructors / Destructor ──────────────────────────────────────────

    explicit Task(Handle h) : handle_(h) {}

    Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_)
                handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_)
            handle_.destroy();
    }

    // ── Awaitable interface ────────────────────────────────────────────────

    bool await_ready() const noexcept { return handle_.done(); }

    void await_suspend(std::coroutine_handle<> caller) { handle_.promise().continuation_ = caller; }

    T await_resume() {
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
        return std::move(*handle_.promise().result_);
    }

    // ── Fire-and-forget start (schedules on GUI thread) ────────────────────

    void start() {
        if (!handle_ || handle_.done() || started_)
            return;
        started_ = true;
        scheduleTaskResume(std::coroutine_handle<>(handle_));
    }

    // ── Access ─────────────────────────────────────────────────────────────

    bool isDone() const noexcept { return handle_.done(); }
    std::coroutine_handle<> coroHandle() const { return handle_; }

private:
    Handle handle_;
    bool started_ = false;
};

// ── Task<void> specialization ──────────────────────────────────────────────

template <>
class Task<void> {
public:
    struct promise_type {
        Task get_return_object() { return Task{Handle::from_promise(*this)}; }

        /// 惰性启动：协程体在 start()（或被 await）之前不执行。
        std::suspend_always initial_suspend() { return {}; }

        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() noexcept { return false; }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
                    auto& prom = Handle::from_address(h.address()).promise();
                    if (prom.continuation_) {
                        return prom.continuation_;
                    }
                    return std::noop_coroutine();
                }

                void await_resume() noexcept {}
            };
            return FinalAwaiter{};
        }

        void return_void() {}

        void unhandled_exception() { exception_ = std::current_exception(); }

        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_;
    };

    using Handle = std::coroutine_handle<promise_type>;

    explicit Task(Handle h) : handle_(h) {}

    Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_)
                handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_)
            handle_.destroy();
    }

    bool await_ready() const noexcept { return handle_.done(); }

    void await_suspend(std::coroutine_handle<> caller) {
        handle_.promise().continuation_ = caller;
        // 被 await 的任务自动启动（惰性启动的语义）。
        start();
    }

    void await_resume() {
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
    }

    /// 启动协程体（调度到 GUI 线程）；重复调用是安全的空操作。
    void start() {
        if (!handle_ || handle_.done() || started_)
            return;
        started_ = true;
        scheduleTaskResume(std::coroutine_handle<>(handle_));
    }

    bool isDone() const noexcept { return handle_.done(); }
    std::coroutine_handle<> coroHandle() const { return handle_; }

private:
    Handle handle_;
    bool started_ = false;
};

}  // namespace cui_core
