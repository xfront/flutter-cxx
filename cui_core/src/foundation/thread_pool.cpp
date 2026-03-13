/// @file thread_pool.cpp
#include "cui/utils/thread_pool.h"

namespace cui {

ThreadPool::ThreadPool(size_t numThreads) {
#ifdef __EMSCRIPTEN__
    // Single-threaded wasm build: ThreadPool::submit() executes inline.
    (void)numThreads;
#else
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
#endif
}

ThreadPool::~ThreadPool() {
    shutdown();
    // jthread joins in its destructor; plain std::thread does not.
    for (auto& worker : workers_) {
        if (worker.joinable())
            worker.join();
    }
}

void ThreadPool::shutdown() {
    {
        std::unique_lock lock(mutex_);
        if (stop_)
            return;
        stop_ = true;
    }
    cv_.notify_all();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            // Wait on the mutex-protected stop_ flag — a stop_token change
            // alone never wakes a condition_variable.
            cv_.wait(lock, [&] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty())
                return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

}  // namespace cui_core
