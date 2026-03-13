/// @file app_lifecycle.cpp
/// @brief AppLifecycleState registry + `cui_core/lifecycle` inbound channel.

#include "cui/platform/services/app_lifecycle.h"

#include <mutex>
#include <utility>
#include <vector>

#include "cui/platform/services/method_channel.h"

namespace cui {
namespace services {

namespace {

constexpr const char* kChannel = "cui_core/lifecycle";

std::mutex g_mutex;
AppLifecycleState g_state = AppLifecycleState::Detached;
std::vector<std::pair<int, AppLifecycleObserver>> g_observers;
int g_nextHandle = 1;

/// Notify the snapshot taken while `g_mutex` was held. Callbacks run outside
/// the lock so they may add/remove observers (a State disposing itself, …).
void notifyObservers(const std::vector<AppLifecycleObserver>& observers,
                     AppLifecycleState state) {
    for (const auto& observer : observers) {
        if (!observer)
            continue;
        try {
            observer(state);
        } catch (...) {
            // One observer must not take down the frame pump.
        }
    }
}

}  // namespace

const char* appLifecycleStateName(AppLifecycleState state) {
    switch (state) {
        case AppLifecycleState::Detached: return "detached";
        case AppLifecycleState::Resumed: return "resumed";
        case AppLifecycleState::Inactive: return "inactive";
        case AppLifecycleState::Hidden: return "hidden";
        case AppLifecycleState::Paused: return "paused";
    }
    return "detached";
}

std::optional<AppLifecycleState> parseAppLifecycleState(const std::string& value) {
    const std::string prefix = "AppLifecycleState.";
    std::string name = value;
    if (name.rfind(prefix, 0) == 0)
        name = name.substr(prefix.size());
    if (name == "detached") return AppLifecycleState::Detached;
    if (name == "resumed") return AppLifecycleState::Resumed;
    if (name == "inactive") return AppLifecycleState::Inactive;
    if (name == "hidden") return AppLifecycleState::Hidden;
    if (name == "paused") return AppLifecycleState::Paused;
    return std::nullopt;
}

AppLifecycleState appLifecycleState() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

int addAppLifecycleObserver(AppLifecycleObserver observer) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int handle = g_nextHandle++;
    g_observers.emplace_back(handle, std::move(observer));
    return handle;
}

void removeAppLifecycleObserver(int handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_observers.begin(); it != g_observers.end(); ++it) {
        if (it->first == handle) {
            g_observers.erase(it);
            return;
        }
    }
}

void setAppLifecycleState(AppLifecycleState state) {
    std::vector<AppLifecycleObserver> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state == state)
            return;
        g_state = state;
        snapshot.reserve(g_observers.size());
        for (const auto& entry : g_observers)
            snapshot.push_back(entry.second);
    }
    notifyObservers(snapshot, state);
}

namespace detail {

/// Installed by pumpPlatformMessages() (see builtin_channels.h). Idempotent.
void installLifecycleChannel() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        setMethodCallHandler(kChannel, [](const std::string& method,
                                         const std::string& arguments) {
            if (method != "AppLifecycleState.changed" && method != "changed")
                return std::string{};
            if (auto state = parseAppLifecycleState(arguments))
                setAppLifecycleState(*state);
            return std::string{};
        });
    });
}

}  // namespace detail

}  // namespace services
}  // namespace cui_core
