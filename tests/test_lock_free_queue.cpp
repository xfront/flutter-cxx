/// @file lock_free_queue_test.cpp
/// @brief Tests for LockFreeQueue: single-thread FIFO, multi-producer
///        correctness, and stress under contention.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <cui/utils/lock_free_queue.h>

using namespace cui;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok)
        std::printf("  [PASS] %s\n", what.c_str());
    else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

// ── Basic FIFO ──────────────────────────────────────────────────────────────

void testBasicFIFO() {
    std::printf("== LockFreeQueue: basic FIFO ==\n");
    LockFreeQueue<int> q;

    check(q.empty(), "empty after construction");
    check(q.size() == 0, "size 0 after construction");

    q.push(1);
    q.push(2);
    q.push(3);

    check(!q.empty(), "not empty after push");
    check(q.size() == 3, "size 3 after 3 pushes");

    auto items = q.drain();
    check(items.size() == 3, "drain returns 3 items");
    check(items[0] == 1 && items[1] == 2 && items[2] == 3,
          "drain returns items in FIFO order (1,2,3)");
    check(q.empty(), "empty after drain");
    check(q.size() == 0, "size 0 after drain");
}

// ── Empty drain ─────────────────────────────────────────────────────────────

void testEmptyDrain() {
    std::printf("== LockFreeQueue: empty drain ==\n");
    LockFreeQueue<int> q;

    auto items = q.drain();
    check(items.empty(), "drain on empty queue returns empty vector");

    q.push(42);
    auto first = q.drain();
    check(first.size() == 1 && first[0] == 42, "single push → single drain");

    auto second = q.drain();
    check(second.empty(), "second drain after first returns empty");
}

// ── Interleaved push / drain ────────────────────────────────────────────────

void testInterleaved() {
    std::printf("== LockFreeQueue: interleaved push/drain ==\n");
    LockFreeQueue<int> q;

    q.push(1);
    q.push(2);
    auto a = q.drain();   // [1, 2]
    check(a.size() == 2 && a[0] == 1 && a[1] == 2, "first drain [1,2]");

    q.push(3);
    q.push(4);
    q.push(5);
    auto b = q.drain();   // [3, 4, 5]
    check(b.size() == 3 && b[0] == 3 && b[1] == 4 && b[2] == 5,
          "second drain [3,4,5]");
}

// ── Move-only types ─────────────────────────────────────────────────────────

void testMoveOnly() {
    std::printf("== LockFreeQueue: move-only types (std::function) ==\n");
    LockFreeQueue<std::function<void()>> q;

    int counter = 0;
    q.push([&counter]() { counter += 1; });
    q.push([&counter]() { counter += 10; });
    q.push([&counter]() { counter += 100; });

    auto tasks = q.drain();
    check(tasks.size() == 3, "3 tasks drained");
    for (auto& t : tasks) t();
    check(counter == 111, "tasks executed in FIFO order (1+10+100=111)");
}

// ── Reset ───────────────────────────────────────────────────────────────────

void testReset() {
    std::printf("== LockFreeQueue: reset ==\n");
    LockFreeQueue<int> q;
    q.push(1);
    q.push(2);
    q.push(3);
    q.reset();
    check(q.empty(), "empty after reset");
    auto items = q.drain();
    check(items.empty(), "drain after reset returns empty");
}

// ── Multi-producer (4 threads × 1000 items) ─────────────────────────────────

void testMultiProducer() {
    std::printf("== LockFreeQueue: multi-producer (4 threads) ==\n");
    LockFreeQueue<int> q;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 1000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&q, t]() {
            int base = t * kPerThread;
            for (int i = 0; i < kPerThread; ++i)
                q.push(base + i);
        });
    }
    for (auto& th : threads) th.join();

    check(q.size() == static_cast<size_t>(kThreads * kPerThread),
          "size == 4000 after all producers finish");

    auto items = q.drain();
    check(items.size() == kThreads * kPerThread,
          "drain returns all 4000 items");

    // All values present (order may vary across threads).
    std::set<int> seen(items.begin(), items.end());
    check(seen.size() == kThreads * kPerThread,
          "all 4000 unique values present");
}

// ── Stress: 100k items, 8 producers ─────────────────────────────────────────

void testStress() {
    std::printf("== LockFreeQueue: stress (8 threads × 100k) ==\n");
    LockFreeQueue<int> q;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100'000;

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&q, t]() {
            int base = t * kPerThread;
            for (int i = 0; i < kPerThread; ++i)
                q.push(base + i);
        });
    }
    for (auto& th : threads) th.join();

    auto items = q.drain();

    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    check(items.size() == static_cast<size_t>(kThreads * kPerThread),
          "all 800k items drained");

    std::set<int> seen(items.begin(), items.end());
    check(seen.size() == kThreads * kPerThread,
          "all 800k unique values present");

    std::printf("  [INFO] 800k push+drain in %ld µs (%.1f ns/op)\n",
                static_cast<long>(us),
                items.empty() ? 0.0 : (us * 1000.0 / items.size()));
}

// ── Multi-producer with interleaved drains ──────────────────────────────────

void testProducerConsumerInterleave() {
    std::printf("== LockFreeQueue: producer/consumer interleaved ==\n");
    LockFreeQueue<int> q;
    std::atomic<int> totalDrained{0};
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 5000;
    constexpr int kTotal = kProducers * kPerProducer;

    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&q, t]() {
            int base = t * kPerProducer;
            for (int i = 0; i < kPerProducer; ++i)
                q.push(base + i);
        });
    }

    // Consumer thread drains repeatedly until all items collected.
    std::thread consumer([&]() {
        while (totalDrained.load() < kTotal) {
            auto items = q.drain();
            totalDrained.fetch_add(static_cast<int>(items.size()));
            if (items.empty())
                std::this_thread::yield();
        }
    });

    for (auto& th : producers) th.join();
    consumer.join();

    check(totalDrained.load() == kTotal,
          "consumer drained exactly 20000 items across multiple drain calls");
}

}  // namespace

int main() {
    std::printf("LockFreeQueue tests\n");
    std::printf("===================\n");

    testBasicFIFO();
    testEmptyDrain();
    testInterleaved();
    testMoveOnly();
    testReset();
    testMultiProducer();
    testStress();
    testProducerConsumerInterleave();

    std::printf("\n%s — %d failure(s)\n",
                g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
