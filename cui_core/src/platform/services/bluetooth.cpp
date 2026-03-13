/// @file bluetooth.cpp
/// @brief Bluetooth LE service (see bluetooth.h).

#include "cui/platform/services/bluetooth.h"

#include <cstdlib>
#include <utility>

#include "cui/platform/services/method_channel.h"

namespace cui {
namespace services {

namespace {

constexpr const char* kChannel = "cui_core/bluetooth";

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r')
                current.pop_back();
            if (!current.empty())
                lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        lines.push_back(current);
    return lines;
}

std::vector<std::string> splitTabs(const std::string& text) {
    std::vector<std::string> fields;
    std::string current;
    for (char c : text) {
        if (c == '\t') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
}

bool replyIsError(const std::string& reply) {
    return reply.rfind("error", 0) == 0;
}

bool replyIsTrue(const std::string& reply) {
    return reply == "1" || reply == "true" || reply == "yes";
}

BluetoothDevice parseDevice(const std::vector<std::string>& fields) {
    BluetoothDevice device;
    if (fields.empty())
        return device;
    device.id = fields[0];
    if (fields.size() > 1)
        device.name = fields[1];
    if (fields.size() > 2)
        device.rssi = std::atoi(fields[2].c_str());
    if (fields.size() > 3)
        device.paired = replyIsTrue(fields[3]);
    return device;
}

}  // namespace

const char* bluetoothAdapterStateName(BluetoothAdapterState state) {
    switch (state) {
        case BluetoothAdapterState::Off: return "off";
        case BluetoothAdapterState::On: return "on";
        case BluetoothAdapterState::TurningOn: return "turningOn";
        case BluetoothAdapterState::TurningOff: return "turningOff";
        case BluetoothAdapterState::Unauthorized: return "unauthorized";
        case BluetoothAdapterState::Unsupported: return "unsupported";
        case BluetoothAdapterState::Unknown: break;
    }
    return "unknown";
}

BluetoothAdapterState parseBluetoothAdapterState(const std::string& value) {
    if (value == "off")
        return BluetoothAdapterState::Off;
    if (value == "on")
        return BluetoothAdapterState::On;
    if (value == "turningOn")
        return BluetoothAdapterState::TurningOn;
    if (value == "turningOff")
        return BluetoothAdapterState::TurningOff;
    if (value == "unauthorized")
        return BluetoothAdapterState::Unauthorized;
    if (value == "unsupported")
        return BluetoothAdapterState::Unsupported;
    return BluetoothAdapterState::Unknown;
}

BluetoothConnectionState parseBluetoothConnectionState(const std::string& value) {
    if (value == "connecting")
        return BluetoothConnectionState::Connecting;
    if (value == "connected")
        return BluetoothConnectionState::Connected;
    if (value == "disconnecting")
        return BluetoothConnectionState::Disconnecting;
    return BluetoothConnectionState::Disconnected;
}

BluetoothService::BluetoothService() {
    events_.setListener([this](const std::string& event) { handleEvent(event); });
}

BluetoothService::~BluetoothService() {
    if (scanning_)
        stopScan();
    events_.cancel();
}

void BluetoothService::ensureListening() {
    if (!events_.isListening())
        events_.listen();
}

bool BluetoothService::isAvailable() const {
    auto reply = invokeMethod(kChannel, "available");
    return reply && replyIsTrue(*reply);
}

BluetoothAdapterState BluetoothService::adapterState() const {
    auto reply = invokeMethod(kChannel, "state");
    if (!reply)
        return BluetoothAdapterState::Unsupported;
    return parseBluetoothAdapterState(*reply);
}

bool BluetoothService::requestEnable() const {
    auto reply = invokeMethod(kChannel, "requestEnable");
    return reply && !replyIsError(*reply);
}

std::vector<BluetoothDevice> BluetoothService::bondedDevices() const {
    std::vector<BluetoothDevice> devices;
    auto reply = invokeMethod(kChannel, "bondedDevices");
    if (!reply)
        return devices;
    for (const auto& line : splitLines(*reply))
        devices.push_back(parseDevice(splitTabs(line)));
    return devices;
}

bool BluetoothService::startScan(DeviceFoundCallback onDevice,
                                 AdapterStateCallback onState) {
    onDevice_ = std::move(onDevice);
    onState_ = std::move(onState);
    ensureListening();
    auto reply = invokeMethod(kChannel, "startScan");
    if (!reply || replyIsError(*reply)) {
        scanning_ = false;
        return false;
    }
    scanning_ = true;
    return true;
}

void BluetoothService::stopScan() {
    if (scanning_)
        invokeMethod(kChannel, "stopScan");
    scanning_ = false;
}

bool BluetoothService::connect(const std::string& deviceId,
                               ConnectionCallback onState) {
    ensureListening();
    if (onState)
        connectionCallbacks_[deviceId] = std::move(onState);
    auto reply = invokeMethod(kChannel, "connect", deviceId);
    return reply && !replyIsError(*reply);
}

void BluetoothService::disconnect(const std::string& deviceId) {
    invokeMethod(kChannel, "disconnect", deviceId);
    connectionCallbacks_.erase(deviceId);
}

void BluetoothService::handleEvent(const std::string& event) {
    const auto tab = event.find('\t');
    const std::string kind = tab == std::string::npos ? event : event.substr(0, tab);
    const std::string rest = tab == std::string::npos ? std::string{} : event.substr(tab + 1);

    if (kind == "device") {
        if (onDevice_)
            onDevice_(parseDevice(splitTabs(rest)));
    } else if (kind == "state") {
        if (onState_)
            onState_(parseBluetoothAdapterState(rest));
    } else if (kind == "connection") {
        const auto fields = splitTabs(rest);
        if (fields.size() < 2)
            return;
        auto it = connectionCallbacks_.find(fields[0]);
        if (it != connectionCallbacks_.end() && it->second)
            it->second(fields[0], parseBluetoothConnectionState(fields[1]));
    } else if (kind == "error") {
        scanning_ = false;
    }
}

}  // namespace services
}  // namespace cui_core
