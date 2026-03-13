/// @file method_channel.cpp
/// @brief Platform channel registry + UI-thread pump (see method_channel.h).

#include "cui/platform/services/method_channel.h"

#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "android_bridge.h"
#include "builtin_channels.h"
#include "channel_registry.h"
#include "ios_bridge.h"
#include "ohos_bridge.h"

namespace cui {
namespace services {

namespace {

struct InboundCall {
    std::string channel;
    std::string method;
    std::string arguments;
};

std::mutex g_mutex;
std::unordered_map<std::string, MethodCallHandler> g_handlers;
std::unordered_map<std::string, std::shared_ptr<detail::EventSink>> g_eventSinks;
std::deque<InboundCall> g_inbound;
std::deque<std::pair<std::string, std::string>> g_events;
ChannelBackend g_backend;
std::function<void()> g_wakeUp;

void wakeIfNeeded() {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        wake = g_wakeUp;
    }
    if (wake)
        wake();
}

}  // namespace

void setChannelBackend(ChannelBackend backend) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backend = std::move(backend);
}

bool hasChannelBackend() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<bool>(g_backend);
}

std::optional<std::string> invokeMethod(const std::string& channel,
                                        const std::string& method,
                                        const std::string& arguments) {
    ChannelBackend backend;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        backend = g_backend;
    }
    if (backend)
        return backend(channel, method, arguments);
    // Exactly one native transport is linked per platform; the others are
    // stubs that answer nullopt, so trying each keeps this file
    // platform-agnostic.
    if (auto reply = android::invoke(channel, method, arguments))
        return reply;
    if (auto reply = ohos::invoke(channel, method, arguments))
        return reply;
    return ios::invoke(channel, method, arguments);
}

void setMethodCallHandler(const std::string& channel, MethodCallHandler handler) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (handler)
        g_handlers[channel] = std::move(handler);
    else
        g_handlers.erase(channel);
}

bool hasMethodCallHandler(const std::string& channel) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_handlers.find(channel) != g_handlers.end();
}

void pushInboundCall(std::string channel, std::string method,
                     std::string arguments) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_inbound.push_back(
            InboundCall{std::move(channel), std::move(method), std::move(arguments)});
    }
    wakeIfNeeded();
}

void pumpPlatformMessages() {
    // Lifecycle and back-button events arrive without the app registering a
    // handler first; install the engine-owned channels before draining.
    detail::installLifecycleChannel();
    detail::installSystemNavigatorChannel();

    std::deque<InboundCall> calls;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        calls.swap(g_inbound);
    }
    for (const auto& call : calls) {
        MethodCallHandler handler;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_handlers.find(call.channel);
            if (it != g_handlers.end())
                handler = it->second;
        }
        if (!handler)
            continue;
        try {
            handler(call.method, call.arguments);
        } catch (...) {
            // A throwing handler must not take down the frame pump.
        }
    }

    std::deque<std::pair<std::string, std::string>> events;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        events.swap(g_events);
    }
    for (const auto& event : events) {
        std::shared_ptr<detail::EventSink> sink;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_eventSinks.find(event.first);
            if (it != g_eventSinks.end())
                sink = it->second;
        }
        if (!sink || !sink->listener)
            continue;
        try {
            sink->listener(event.second);
        } catch (...) {
        }
    }
}

void setWakeUpHandler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_wakeUp = std::move(handler);
}

// ── detail::event sinks ──────────────────────────────────────────────────────

namespace detail {

void registerEventSink(const std::string& channel, std::shared_ptr<EventSink> sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_eventSinks[channel] = std::move(sink);
}

void unregisterEventSink(const std::string& channel, const EventSink* sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_eventSinks.find(channel);
    if (it != g_eventSinks.end() && it->second.get() == sink)
        g_eventSinks.erase(it);
}

void pushEvent(std::string channel, std::string event) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_events.emplace_back(std::move(channel), std::move(event));
    }
    wakeIfNeeded();
}

std::string dispatchHandlerNow(const std::string& channel,
                               const std::string& method,
                               const std::string& arguments) {
    MethodCallHandler handler;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handlers.find(channel);
        if (it != g_handlers.end())
            handler = it->second;
    }
    if (!handler)
        return {};
    try {
        return handler(method, arguments);
    } catch (...) {
        return {};
    }
}

}  // namespace detail

// ── MethodChannel ───────────────────────────────────────────────────────────

MethodChannel::MethodChannel(std::string name) : name_(std::move(name)) {}

std::optional<std::string> MethodChannel::invokeMethod(
    const std::string& method, const std::string& arguments) const {
    return services::invokeMethod(name_, method, arguments);
}

void MethodChannel::setMethodCallHandler(MethodCallHandler handler) const {
    services::setMethodCallHandler(name_, std::move(handler));
}

bool MethodChannel::hasHandler() const {
    return services::hasMethodCallHandler(name_);
}

}  // namespace services
}  // namespace cui_core
