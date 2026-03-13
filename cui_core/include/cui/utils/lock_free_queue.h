#pragma once
/// @file lock_free_queue.h
/// @brief Lock-free FIFO queue built on a Treiber stack.
///
/// Design:
///   Producers push() onto a singly-linked list via atomic CAS (O(1)
///   amortised, no locks).  The single consumer thread calls drain()
///   to atomically take the entire list and reverse it, yielding FIFO
///   order.  No memory-reclamation scheme (hazard pointers / epochs)
///   is needed because nodes are never freed while another thread
///   could still reference them — the swap transfers full ownership.
///
/// Contract:
///   - Any number of threads may call push() concurrently.
///   - Exactly ONE thread calls drain() / tryPopAll() / empty().
///   - std::memory_order_acq_rel on the CAS / swap is sufficient:
///     release in push publishes the node write before the CAS
///     succeeds; acquire in drain makes the full chain visible.

#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace cui {

template <typename T>
class LockFreeQueue {
    struct Node {
        T data;
        Node* next;
        template <typename U>
        Node(U&& v) : data(std::forward<U>(v)), next(nullptr) {}
    };

    std::atomic<Node*> head_{nullptr};
    std::atomic<size_t> size_{0};

public:
    LockFreeQueue() = default;
    ~LockFreeQueue() { reset(); }

    // Non-copyable, non-movable (atomic head)
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    /// Thread-safe: push an item (any thread may call).
    /// O(1) amortised — a CAS retry loop that converges quickly when
    /// contention is low (typical GUI task-posting pattern).
    template <typename U>
    void push(U&& value) {
        Node* node = new Node(std::forward<U>(value));
        node->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(node->next, node,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
            // node->next is refreshed by compare_exchange_weak on failure
        }
        size_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Consumer-only: take ALL items, returned in FIFO order.
    /// Reverses the LIFO stack into a vector so the caller can
    /// iterate front-to-back in push order.
    std::vector<T> drain() {
        Node* list = head_.exchange(nullptr, std::memory_order_acquire);
        if (!list) return {};

        // Count + reverse in one pass (two passes would cost the same
        // asymptotically but need an extra traversal).
        size_t count = 0;
        for (Node* n = list; n; n = n->next) ++count;

        std::vector<T> result;
        result.reserve(count);

        // Reverse the singly-linked list and collect into the vector.
        // After reversal the oldest pushed item sits at the head.
        Node* prev = nullptr;
        Node* curr = list;
        while (curr) {
            Node* next = curr->next;
            curr->next = prev;
            prev = curr;
            curr = next;
        }
        // prev is now the head of the reversed list (oldest item first).
        for (Node* n = prev; n; ) {
            Node* next = n->next;
            result.push_back(std::move(n->data));
            delete n;
            n = next;
        }

        size_.store(0, std::memory_order_relaxed);
        return result;
    }

    /// Consumer-only: true when no items are queued.
    /// Relaxed load — only the consumer reads this; producers never
    /// set it to false (they only increment size_).
    bool empty() const {
        return head_.load(std::memory_order_relaxed) == nullptr;
    }

    /// Approximate count (relaxed — may be stale under contention).
    size_t size() const {
        return size_.load(std::memory_order_relaxed);
    }

    /// Consumer-only: drop all items without running them.
    void reset() {
        Node* curr = head_.exchange(nullptr, std::memory_order_acquire);
        while (curr) {
            Node* next = curr->next;
            delete curr;
            curr = next;
        }
        size_.store(0, std::memory_order_relaxed);
    }
};

}  // namespace cui_core
