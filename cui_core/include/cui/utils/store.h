#pragma once
/// @file store.h
/// @brief MVI Store — the single source of truth for application state.
///
/// Architecture (Unidirectional Data Flow):
///
///     ┌──────────┐    State     ┌──────┐
///     │  Store   │─────────────▶│ View │
///     │(Reducer) │              │      │
///     └────▲─────┘              └──┬───┘
///          │                       │
///     Action/Intent          User interaction
///          │                       │
///     ┌────┴───────────────────────▼───┐
///     │         dispatch()             │
///     └────────────────────────────────┘
///
/// Analogous to:
///   :  ViewModel + MutableStateFlow + reducer
///   Redux:    createStore(reducer, initialState)
///   Vuex:     store({ state, mutations, actions })
///
/// Usage:
///   struct State { int count = 0; };
///   struct Increment {};
///   struct Decrement {};
///   using Action = std::variant<Increment, Decrement>;
///
///   Store<State, Action> store(
///       State{},
///       [](State s, Action a) -> State {
///           return std::visit(overload{
///               [&](Increment) { return State{ s.count + 1 }; },
///               [&](Decrement) { return State{ s.count - 1 }; },
///           }, a);
///       });
///
///   store.dispatch(Increment{});
///   store.state().changed().connect([](const State& s) { /* re-render */ });

#include <functional>
#include <variant>

#include "cui/reactive/property.h"
#include "cui/utils/task.h"

namespace cui {

/// The MVI Store. State must be copyable (value semantics).
template <typename State, typename Action>
class Store {
public:
    using Reducer = std::function<State(const State&, const Action&)>;

    Store(State initialState, Reducer reducer) : state_(std::move(initialState)), reducer_(std::move(reducer)) {}

    /// Dispatch an action — runs the reducer synchronously.
    void dispatch(const Action& action) {
        State newState = reducer_(state_.get(), action);
        state_ = std::move(newState);
    }

    /// Get the reactive state property.
    Property<State>& state() { return state_; }
    const Property<State>& state() const { return state_; }

    /// Subscribe to state changes (convenience).
    auto subscribe(std::function<void(const State&)> observer) { return state_.changed().connect(std::move(observer)); }

    /// Get current state snapshot.
    const State& currentState() const { return state_.get(); }

private:
    Property<State> state_;
    Reducer reducer_;
};

}  // namespace cui_core
