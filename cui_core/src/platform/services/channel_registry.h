#pragma once
/// @file channel_registry.h
/// @brief Private glue between method_channel.cpp and event_channel.cpp.
///
/// Both channel kinds share one inbound queue and one UI-thread pump, so the
/// event sink registry lives here instead of in a public header.

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace cui {
namespace services {
namespace detail {

struct EventSink {
    std::function<void(const std::string& event)> listener;
};

/// Register / unregister the sink listening on `channel`. Only one sink per
/// channel is supported (Flutter's EventChannel also has a single listener).
void registerEventSink(const std::string& channel, std::shared_ptr<EventSink> sink);
void unregisterEventSink(const std::string& channel, const EventSink* sink);

/// Queue an event for delivery by the next pumpPlatformMessages().
void pushEvent(std::string channel, std::string event);

/// Call the handler registered for `channel` right now (native → C++ call
/// that expects a synchronous reply). Returns an empty string when there is
/// no handler or it throws.
std::string dispatchHandlerNow(const std::string& channel,
                               const std::string& method,
                               const std::string& arguments);

}  // namespace detail
}  // namespace services
}  // namespace cui_core
