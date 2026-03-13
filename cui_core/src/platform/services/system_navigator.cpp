/// @file system_navigator.cpp
/// @brief SystemNavigator channel + back-button registry.

#include "cui/platform/services/system_navigator.h"

#include <mutex>
#include <utility>
#include <vector>

#include "cui/platform/services/method_channel.h"

namespace cui {
namespace services {

namespace {

constexpr const char* kChannel = "cui_core/platform";
constexpr const char* kBackChannel = "cui_core/system_navigator";

std::mutex g_mutex;
std::vector<std::pair<int, SystemNavigator::BackHandler>> g_handlers;
int g_nextHandle = 1;

}  // namespace

bool SystemNavigator::pop(bool animated) {
    return invokeMethod(kChannel, "SystemNavigator.pop", animated ? "1" : "0")
            .has_value();
}

bool SystemNavigator::setFrameworkHandlesBack(bool frameworkHandlesBack) {
    return invokeMethod(kChannel, "SystemNavigator.setFrameworkHandlesBack",
                        frameworkHandlesBack ? "1" : "0")
            .has_value();
}

int SystemNavigator::addBackHandler(BackHandler handler) {
    int handle = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        handle = g_nextHandle++;
        g_handlers.emplace_back(handle, std::move(handler));
    }
    // Predictive back stays disabled only while someone can consume it.
    setFrameworkHandlesBack(true);
    return handle;
}

void SystemNavigator::removeBackHandler(int handle) {
    bool empty = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_handlers.begin(); it != g_handlers.end(); ++it) {
            if (it->first == handle) {
                g_handlers.erase(it);
                break;
            }
        }
        empty = g_handlers.empty();
    }
    if (empty)
        setFrameworkHandlesBack(false);
}

int SystemNavigator::backHandlerCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_handlers.size());
}

bool SystemNavigator::handleBack() {
    std::vector<BackHandler> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snapshot.reserve(g_handlers.size());
        for (auto it = g_handlers.rbegin(); it != g_handlers.rend(); ++it)
            snapshot.push_back(it->second);
    }
    for (const auto& handler : snapshot) {
        if (!handler)
            continue;
        try {
            if (handler())
                return true;
        } catch (...) {
            // A throwing handler must not take down the frame pump.
        }
    }
    // Nobody consumed it: leave the app (root route of Flutter does this in
    // the engine, not in Dart).
    pop();
    return false;
}

namespace detail {

/// Installed by pumpPlatformMessages() (see builtin_channels.h). Idempotent.
void installSystemNavigatorChannel() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        setMethodCallHandler(kBackChannel, [](const std::string& method,
                                             const std::string&) {
            if (method != "back")
                return std::string{};
            return SystemNavigator::handleBack() ? std::string{"1"} : std::string{"0"};
        });
    });
}

}  // namespace detail

}  // namespace services
}  // namespace cui_core
