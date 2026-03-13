/// @file event_channel.cpp
/// @brief EventChannel implementation (see event_channel.h).

#include "cui/platform/services/event_channel.h"

#include <utility>

#include "channel_registry.h"

namespace cui {
namespace services {

struct EventChannel::Sink : detail::EventSink {};

void pushInboundEvent(std::string channel, std::string event) {
    detail::pushEvent(std::move(channel), std::move(event));
}

EventChannel::EventChannel(std::string name)
    : name_(std::move(name)), sink_(std::make_shared<Sink>()) {}

EventChannel::~EventChannel() {
    if (sink_)
        detail::unregisterEventSink(name_, sink_.get());
}

void EventChannel::setListener(EventListener listener) {
    if (!sink_)
        sink_ = std::make_shared<Sink>();
    sink_->listener = std::move(listener);
    if (sink_->listener)
        detail::registerEventSink(name_, sink_);
    else
        detail::unregisterEventSink(name_, sink_.get());
}

bool EventChannel::listen() {
    if (listening_)
        return true;
    auto reply = invokeMethod(name_, "listen");
    if (!reply)
        return false;
    listening_ = true;
    return true;
}

void EventChannel::cancel() {
    if (!listening_)
        return;
    invokeMethod(name_, "cancel");
    listening_ = false;
}

}  // namespace services
}  // namespace cui_core
