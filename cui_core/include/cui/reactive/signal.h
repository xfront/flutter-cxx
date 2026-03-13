#pragma once
/// @file signal.h
/// @brief Type-safe signal/slot mechanism (the reactive primitive).
///
/// Signal<Args...> is the foundation of the reactive system.
/// Property<T> wraps a Signal to notify on value changes.
/// Observable<T> extends the pattern with stream operators.

#include <algorithm>
#include <concepts>
#include <functional>
#include <vector>

namespace cui {

template <typename F, typename... Args>
concept SlotCallable = std::invocable<F, Args...>;

/// A type-safe signal that can be connected to any compatible callable.
/// emit() uses snapshot iteration — safe for re-entrant connect/disconnect.
template <typename... Args>
class Signal {
public:
    using SlotType = std::function<void(Args...)>;
    using ConnectionId = int;

    Signal() = default;

    // Non-copyable, movable
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&) = default;
    Signal& operator=(Signal&&) = default;

    /// Connect a callable. Returns a connection ID for disconnect.
    template <SlotCallable<Args...> F>
    ConnectionId connect(F&& slot) {
        int id = nextId_++;
        slots_.push_back({id, std::forward<F>(slot)});
        return id;
    }

    /// Disconnect by connection ID.
    void disconnect(ConnectionId id) {
        slots_.erase(std::remove_if(slots_.begin(), slots_.end(), [id](const SlotEntry& e) { return e.id == id; }),
                     slots_.end());
    }

    /// Emit the signal to all connected slots (snapshot copy).
    void emit(Args... args) const {
        auto snapshot = slots_;
        for (auto& entry : snapshot) {
            entry.slot(args...);
        }
    }

    /// Convenience: call operator.
    void operator()(Args... args) const { emit(std::forward<Args>(args)...); }

    /// Remove all connections.
    void clear() { slots_.clear(); }

    bool empty() const { return slots_.empty(); }

private:
    struct SlotEntry {
        ConnectionId id;
        SlotType slot;
    };

    std::vector<SlotEntry> slots_;
    int nextId_ = 0;
};

}  // namespace cui_core
