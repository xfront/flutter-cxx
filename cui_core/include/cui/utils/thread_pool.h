#pragma once
/// @file thread_pool.h
/// @brief Fixed-size thread pool built on std::thread.
///
/// Workers are only ever stopped through the mutex-protected `stop_` flag, so
/// `std::jthread`'s stop_token adds nothing here. It is used when the standard
/// library provides it (desktop toolchains) purely for the automatic join; the
/// NDK's libc++ (18, r27) does not ship <stop_token>/std::jthread, so Android
/// falls back to plain std::thread + an explicit join in the destructor.

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

#include "shared_ptr.h"

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
#define CUI_HAS_JTHREAD 1
#else
#define CUI_HAS_JTHREAD 0
#endif

namespace cui {

class ThreadPool {
public:
    /// Create a pool with `numThreads` worker threads.
    explicit ThreadPool(size_t numThreads = std::thread::hardware_concurrency());

    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Submit a callable. Returns a future for the result.
    template <typename F>
    auto submit(F&& func) -> std::future<std::invoke_result_t<F>>;

    /// Graceful shutdown.
    void shutdown();

private:
    void workerLoop();

#if CUI_HAS_JTHREAD
    std::vector<std::jthread> workers_;
#else
    std::vector<std::thread> workers_;
#endif
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

// ── Template implementation ────────────────────────────────────────────────

template <typename F>
auto ThreadPool::submit(F&& func) -> std::future<std::invoke_result_t<F>> {
    using ReturnType = std::invoke_result_t<F>;

#ifdef __EMSCRIPTEN__
    // The browser build has no worker threads unless -pthread (and
    // SharedArrayBuffer / COOP+COEP hosting) is enabled, so work runs inline
    // on the calling thread and the future is already satisfied. runAsync's
    // onDone still hops back through Application::postTask().
    std::packaged_task<ReturnType()> task(std::forward<F>(func));
    std::future<ReturnType> result = task.get_future();
    task();
    return result;
#else
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(std::forward<F>(func));

    std::future<ReturnType> result = task->get_future();

    {
        std::unique_lock lock(mutex_);
        if (stop_) {
            throw std::runtime_error("ThreadPool: submit on stopped pool");
        }
        tasks_.emplace([task]() { (*task)(); });
    }

    cv_.notify_one();
    return result;
#endif
}

}  // namespace cui_core
