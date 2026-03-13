#pragma once
/// @file bluetooth.h
/// @brief Bluetooth LE scanning / connection service.
///
/// Flutter uses flutter_blue_plus / flutter_reactive_ble for this; cui_core exposes
/// the same shape as a small service driven by the platform channel:
///
///   services::BluetoothService bt;
///   bt.startScan([](const services::BluetoothDevice& d) { ... });
///   bt.connect(d.id, [](const std::string& id, services::BluetoothConnectionState s) { ... });
///   bt.stopScan();
///
/// On Android the host (CuiBluetoothService) uses BluetoothLeScanner and
/// GATT; Android 12+ needs Permission::BluetoothScan / Permission::Bluetooth
/// granted first. Desktop reports Unsupported and every operation is a no-op.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cui/platform/services/event_channel.h"

namespace cui {
namespace services {

enum class BluetoothAdapterState {
    Unknown,
    Off,
    On,
    TurningOn,
    TurningOff,
    Unauthorized,
    Unsupported,
};

enum class BluetoothConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Disconnecting,
};

struct BluetoothDevice {
    std::string id;      ///< MAC address (Android) / opaque id
    std::string name;    ///< advertised or bonded name (may be empty)
    int rssi = 0;
    bool paired = false;
};

const char* bluetoothAdapterStateName(BluetoothAdapterState state);
BluetoothAdapterState parseBluetoothAdapterState(const std::string& value);
BluetoothConnectionState parseBluetoothConnectionState(const std::string& value);

class BluetoothService {
public:
    using DeviceFoundCallback = std::function<void(const BluetoothDevice& device)>;
    using AdapterStateCallback = std::function<void(BluetoothAdapterState state)>;
    using ConnectionCallback = std::function<void(const std::string& deviceId,
                                                  BluetoothConnectionState state)>;

    BluetoothService();
    ~BluetoothService();

    BluetoothService(const BluetoothService&) = delete;
    BluetoothService& operator=(const BluetoothService&) = delete;

    /// Device has a Bluetooth LE adapter.
    bool isAvailable() const;

    /// Current adapter state (Off/On/Unauthorized/…).
    BluetoothAdapterState adapterState() const;
    bool isEnabled() const { return adapterState() == BluetoothAdapterState::On; }

    /// Show the system "turn on Bluetooth" dialog (Android).
    bool requestEnable() const;

    /// Devices already paired with the system.
    std::vector<BluetoothDevice> bondedDevices() const;

    /// Begin a BLE scan. `onDevice` runs on the UI thread. Returns false when
    /// the host is unavailable or permissions are missing.
    bool startScan(DeviceFoundCallback onDevice, AdapterStateCallback onState = {});
    void stopScan();
    bool isScanning() const { return scanning_; }

    /// Connect to a device id (from a scan result or bondedDevices()).
    bool connect(const std::string& deviceId, ConnectionCallback onState = {});
    void disconnect(const std::string& deviceId);

    /// Exposed for tests / custom transports: feed one raw event line.
    void handleEvent(const std::string& event);

private:
    void ensureListening();

    EventChannel events_{"cui_core/bluetooth"};
    DeviceFoundCallback onDevice_;
    AdapterStateCallback onState_;
    std::map<std::string, ConnectionCallback> connectionCallbacks_;
    bool scanning_ = false;
};

}  // namespace services
}  // namespace cui_core
