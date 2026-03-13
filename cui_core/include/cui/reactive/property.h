#pragma once
/// @file property.h
/// @brief Reactive property — assignment triggers Signal.
///
/// Analogous to:
///   Vue 3:  ref(0)
///   : mutableStateOf(0)
///
/// Usage:
///   Property<int> count = 0;
///   count.changed().connect([](const int& v) { ... });
///   count = 42;  // triggers signal

#include <utility>

#include "cui/reactive/signal.h"

namespace cui {

template <typename T>
class Property {
public:
    Property() = default;
    Property(T value) : value_(std::move(value)) {}

    /// Get the current value.
    const T& get() const { return value_; }

    /// Set a new value. Triggers changed() only if the value differs.
    void set(const T& value) {
        if (value_ != value) {
            value_ = value;
            changed_.emit(value_);
        }
    }

    /// Assignment triggers changed (if different).
    Property& operator=(const T& value) {
        set(value);
        return *this;
    }

    /// Move assignment — avoids a copy for temporary values.
    Property& operator=(T&& value) {
        if (value_ != value) {
            value_ = std::move(value);
            changed_.emit(value_);
        }
        return *this;
    }

    /// Implicit conversion to const T&.
    operator const T&() const { return value_; }

    /// Signal emitted on value change.
    Signal<const T&>& changed() { return changed_; }

    /// Modify the value in-place and emit changed().
    /// Use this for efficient in-place modifications (e.g. string insert/erase)
    /// without creating a temporary copy.
    template <typename F>
    void modify(F&& f) {
        f(value_);
        changed_.emit(value_);
    }

private:
    T value_{};
    Signal<const T&> changed_;
};

}  // namespace cui_core
