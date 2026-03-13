#pragma once
/// @file event_channel.h
/// @brief Flutter-style event channels (streaming native → C++ events).
///
/// Flutter:
///   final events = EventChannel('cui_core/bluetooth').receiveBroadcastStream();
///   events.listen((event) => ...);
///
/// cui_core:
///   services::EventChannel channel("cui_core/bluetooth");
///   channel.setListener([](const std::string& event) { ... });
///   channel.listen();     // sends method "listen" to the native side
///   channel.cancel();     // sends method "cancel"
///
/// The native side pushes events with pushInboundEvent() (from JNI:
/// `nativeCuiEvent`). Events are queued and delivered on the UI thread by
/// pumpPlatformMessages(); a single listener per channel is supported, the
/// same as Flutter's EventChannel.

#include <functional>
#include <memory>
#include <string>

#include "cui/platform/services/method_channel.h"

namespace cui {
namespace services {

/// Native → C++ event callback. `event` is an opaque service-defined string.
using EventListener = std::function<void(const std::string& event)>;

/// Queue a native-originated event (any thread). Delivered by the next
/// pumpPlatformMessages() to the listener registered for `channel`.
void pushInboundEvent(std::string channel, std::string event);

class EventChannel {
public:
    explicit EventChannel(std::string name);
    ~EventChannel();

    EventChannel(const EventChannel&) = delete;
    EventChannel& operator=(const EventChannel&) = delete;

    const std::string& name() const { return name_; }

    /// Replace the listener (nullptr clears it). Setting a listener while
    /// listening keeps the native subscription running.
    void setListener(EventListener listener);

    /// Subscribe on the native side (Flutter: listen()). Returns false when
    /// the native side is unreachable.
    bool listen();

    /// Unsubscribe on the native side (Flutter: cancel()).
    void cancel();

    bool isListening() const { return listening_; }

private:
    struct Sink;
    std::string name_;
    std::shared_ptr<Sink> sink_;
    bool listening_ = false;
};

}  // namespace services
}  // namespace cui_core
