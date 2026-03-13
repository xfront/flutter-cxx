import Foundation
import CoreBluetooth

/// 蓝牙服务的 iOS 实现（对应 Android 的 CuiBluetoothService.java 与
/// OHOS 的 CuiBluetoothService.ets）。
///
/// 通道协议（与 C++ bluetooth.cpp 一致）：
///   listen / cancel        → "ok"
///   available              → "1" | "0"
///   state                  → off|on|turningOn|turningOff|unauthorized|unsupported|unknown
///   requestEnable          → "error:not supported"（iOS 不允许应用开关蓝牙）
///   bondedDevices          → ""（iOS 无公开的已配对设备列表）
///   startScan              → "ok" | "error:..."
///   stopScan               → "ok"
///   connect <id>           → "ok" | "error:..."（id = 外设 UUID 字符串）
///   disconnect <id>        → "ok"
///
/// 事件：
///   device\t<id>\t<name>\t<rssi>\t0
///   state\t<adapter state>
///   connection\t<id>\t<connecting|connected|disconnecting|disconnected>
///   error\t<message>
///
/// ⚠️ 参考实现：CBCentralManager 必须在有 run loop 的队列上创建（这里用主
///    队列）；`NSBluetoothAlwaysUsageDescription` 必填，否则连接时系统会直接
///    拒绝。
final class CuiBluetoothService: NSObject, CBCentralManagerDelegate {

    static let shared = CuiBluetoothService()

    private var central: CBCentralManager?
    private var discovered: [String: CBPeripheral] = [:]
    private var pendingScan = false
    private var scanning = false

    private override init() {
        super.init()
    }

    /// 首次调用会创建 CBCentralManager（触发系统蓝牙授权弹窗）。
    func ensureCentral() {
        if central == nil {
            central = CBCentralManager(
                delegate: self,
                queue: .main,
                options: [CBCentralManagerOptionShowPowerAlertKey: false])
        }
    }

    // MARK: - 通道入口

    func handle(_ method: String, _ args: String) -> String {
        ensureCentral()
        switch method {
        case "listen", "cancel":
            return "ok"
        case "available":
            return central?.state == .unsupported ? "0" : "1"
        case "state":
            return stateName(central?.state ?? .unknown)
        case "requestEnable":
            return "error:not supported"
        case "bondedDevices":
            // iOS 不提供已配对设备枚举；配对由系统设置负责。
            return ""
        case "startScan":
            return startScan()
        case "stopScan":
            stopScan()
            return "ok"
        case "connect":
            return connect(args)
        case "disconnect":
            disconnect(args)
            return "ok"
        default:
            return ""
        }
    }

    // MARK: - 适配器状态

    private func stateName(_ state: CBManagerState) -> String {
        switch state {
        case .poweredOn: return "on"
        case .poweredOff: return "off"
        case .resetting: return "turningOn"
        case .unauthorized: return "unauthorized"
        case .unsupported: return "unsupported"
        default: return "unknown"
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        emit("state\t\(stateName(central.state))")
        CuiPlatformServices.bluetoothAuthorizationChanged()
        if central.state == .poweredOn && pendingScan {
            pendingScan = false
            _ = startScan()
        }
    }

    // MARK: - 扫描

    private func startScan() -> String {
        stopScan()
        switch central?.state ?? .unknown {
        case .poweredOn:
            central?.scanForPeripherals(
                withServices: nil,
                options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
            scanning = true
            return "ok"
        case .poweredOff:
            return "error:bluetooth is off"
        case .unauthorized:
            return "error:scan permission denied"
        case .unsupported:
            return "error:unsupported"
        default:
            // 状态未就绪（首次授权弹窗中）：等 poweredOn 后自动开始。
            pendingScan = true
            return "ok"
        }
    }

    private func stopScan() {
        pendingScan = false
        if scanning {
            central?.stopScan()
            scanning = false
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        let id = peripheral.identifier.uuidString
        discovered[id] = peripheral
        let advertised = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let name = peripheral.name ?? advertised ?? ""
        emit("device\t\(id)\t\(clean(name))\t\(RSSI.intValue)\t0")
    }

    // MARK: - 连接

    private func connect(_ id: String) -> String {
        guard !id.isEmpty else {
            return "error:no device id"
        }
        guard let uuid = UUID(uuidString: id) else {
            return "error:bad device id"
        }
        let peripheral = discovered[id]
            ?? central?.retrievePeripherals(withIdentifiers: [uuid]).first
        guard let peripheral else {
            return "error:device not found"
        }
        discovered[id] = peripheral
        central?.connect(peripheral, options: nil)
        emit("connection\t\(id)\tconnecting")
        return "ok"
    }

    private func disconnect(_ id: String) {
        var peripheral = discovered[id]
        if peripheral == nil, let uuid = UUID(uuidString: id) {
            peripheral = central?.retrievePeripherals(withIdentifiers: [uuid]).first
        }
        guard let peripheral else {
            return
        }
        central?.cancelPeripheralConnection(peripheral)
        discovered.removeValue(forKey: peripheral.identifier.uuidString)
        emit("connection\t\(id)\tdisconnected")
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        emit("connection\t\(peripheral.identifier.uuidString)\tconnected")
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        let id = peripheral.identifier.uuidString
        discovered.removeValue(forKey: id)
        emit("connection\t\(id)\tdisconnected")
        if let error {
            emit("error\t\(error.localizedDescription)")
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        let id = peripheral.identifier.uuidString
        discovered.removeValue(forKey: id)
        emit("connection\t\(id)\tdisconnected")
    }

    // MARK: - 工具

    private func emit(_ event: String) {
        CuiBridge.emitEvent("cui/bluetooth", event)
    }

    private func clean(_ value: String) -> String {
        value.replacingOccurrences(of: "\t", with: " ")
            .replacingOccurrences(of: "\n", with: " ")
    }
}
