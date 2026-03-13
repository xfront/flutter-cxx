#pragma once
/// @file observable.h
/// @brief Reactive event stream with chainable operators.
///
/// Analogous to:
///   RxJS:     Observable + filter/map/merge
///   :  Flow + operators
///
/// 控件的鼠标事件以 Observable 形式暴露，便于
/// declarative event composition:
///
///   widget.mousePress()
///       .take_until(widget.mouseRelease())
///       .subscribe([](Point p) { /* drag */ });

#include <algorithm>
#include <any>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include "cui/foundation/shared_ptr.h"

namespace cui {

template <typename T>
class Observable {
public:
    using Observer = std::function<void(T)>;
    using Unsubscribe = std::function<void()>;

    Observable() = default;

    // Non-copyable, movable
    Observable(const Observable&) = delete;
    Observable& operator=(const Observable&) = delete;
    Observable(Observable&&) = default;
    Observable& operator=(Observable&&) = default;

    /// Subscribe an observer. Returns an unsubscribe callable.
    Unsubscribe subscribe(Observer observer) {
        auto shared = state_;
        int id = shared->nextId++;
        shared->observers.push_back({id, std::move(observer)});
        return [shared, id]() {
            auto& obs = shared->observers;
            obs.erase(std::remove_if(obs.begin(), obs.end(), [id](const Entry& e) { return e.id == id; }), obs.end());
        };
    }

    /// Push a value to all subscribers (snapshot iteration).
    void next(T value) {
        typename State::ObserverList snapshot;
        {
            snapshot = state_->observers;
        }
        for (auto& entry : snapshot) {
            entry.observer(value);
        }
    }

    // ── Operators ──────────────────────────────────────────────────────────

    /// Filter: only forward values satisfying the predicate.
    Observable<T> filter(std::function<bool(const T&)> predicate) {
        Observable<T> result;
        auto resState = result.state_;
        state_->observers.push_back({state_->nextId++, [resState, pred = std::move(predicate)](T val) {
                                         if (pred(val)) {
                                             Observable<T> tmp;
                                             tmp.state_ = resState;
                                             tmp.next(std::move(val));
                                         }
                                     }});
        return result;
    }

    /// Map: transform each T into a U.
    template <typename U>
    Observable<U> map(std::function<U(const T&)> transform) {
        Observable<U> result;
        auto resState = result.state_;
        state_->observers.push_back({state_->nextId++, [resState, tr = std::move(transform)](T val) {
                                         U mapped = tr(val);
                                         Observable<U> tmp;
                                         tmp.state_ = resState;
                                         tmp.next(std::move(mapped));
                                     }});
        return result;
    }

    /// take_until: forward values until `until` emits, then stop.
    Observable<T> take_until(Observable<std::any>& until) {
        Observable<T> result;
        auto resState = result.state_;
        auto completed = cui::make_shared<bool>(false);

        state_->observers.push_back({state_->nextId++, [resState, completed](T val) {
                                         if (!*completed) {
                                             Observable<T> tmp;
                                             tmp.state_ = resState;
                                             tmp.next(std::move(val));
                                         }
                                     }});

        until.state_->observers.push_back({until.state_->nextId++, [completed](std::any) { *completed = true; }});

        return result;
    }

    /// combine_latest: whenever either stream emits, emit pair of latest.
    template <typename U>
    Observable<std::pair<T, U>> combine_latest(Observable<U>& other) {
        Observable<std::pair<T, U>> result;
        auto resState = result.state_;

        auto latestT = cui::make_shared<std::optional<T>>();
        auto latestU = cui::make_shared<std::optional<U>>();

        state_->observers.push_back({state_->nextId++, [resState, latestT, latestU](T val) {
                                         *latestT = std::move(val);
                                         if (latestU->has_value()) {
                                             Observable<std::pair<T, U>> tmp;
                                             tmp.state_ = resState;
                                             tmp.next({latestT->value(), latestU->value()});
                                         }
                                     }});

        other.state_->observers.push_back({other.state_->nextId++, [resState, latestT, latestU](U val) {
                                               *latestU = std::move(val);
                                               if (latestT->has_value()) {
                                                   Observable<std::pair<T, U>> tmp;
                                                   tmp.state_ = resState;
                                                   tmp.next({latestT->value(), latestU->value()});
                                               }
                                           }});

        return result;
    }

private:
    struct Entry {
        int id;
        Observer observer;
    };

    struct State {
        using ObserverList = std::vector<Entry>;
        ObserverList observers;
        int nextId = 0;
    };

    SharedPtr<State> state_ = cui::make_shared<State>();
};

}  // namespace cui_core
