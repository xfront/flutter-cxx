import UIKit
import AVFoundation
import AudioToolbox
import Photos
import CoreLocation
import CoreBluetooth
import Contacts
import UserNotifications

/// 平台通道的 Swift 分发器（对应 Android 的 CuiPlatformServices.java /
/// OHOS 的 CuiPlatformServices.ets）。
///
/// 通道协议与其它平台完全一致：
///   cui/platform   ping                       → "ok"
///                  SystemChrome.* / HapticFeedback.vibrate / SystemSound.play /
///                  SystemNavigator.*         → "ok"（方法名/参数编码与
///                                               system_chrome.h 一致）
///   cui/clipboard  set/get/has                → "ok" / 文本 / "1"|"0"
///   cui/permission check/request/openSettings → "granted|limited|denied|restricted|
///                                               permanentlyDenied|unavailable" /
///                                               "pending"（结果经 notify 回推）
///   cui/camera     见 CuiCameraService.swift
///   cui/bluetooth  见 CuiBluetoothService.swift
///
/// 生命周期 / 返回键由 App 主动推送（引擎侧的通道在 pump 时自动安装）：
///   applicationDidBecomeActive      → CuiAppLifecycle.notify(.resumed)
///   applicationWillResignActive     → CuiAppLifecycle.notify(.inactive)
///   applicationDidEnterBackground   → CuiAppLifecycle.notify(.hidden)
///   applicationWillTerminate        → CuiAppLifecycle.notify(.detached)
///   （SceneDelegate 的 sceneDidBecomeActive / sceneWillResignActive 同理）
///   iOS 没有系统返回键；需要时由宿主调用 CuiAppLifecycle.notifyBack()。
///
/// 所有入口都由引擎在 UI 线程调用；耗时工作（权限弹窗、拍照、BLE）在宿主侧
/// 异步完成，之后用 `CuiBridge.notify` / `emitEvent` 回帧泵。
enum CuiPlatformServices {

    static func handle(channel: String, method: String, args: String) -> String {
        switch channel {
        case "cui/platform":
            return method == "ping" ? "ok" : CuiSystemChrome.handle(method: method, args: args)
        case "cui/clipboard":
            return clipboard(method, args)
        case "cui/permission":
            return permission(method, args)
        case "cui/camera":
            return CuiCameraService.shared.handle(method, args)
        case "cui/bluetooth":
            return CuiBluetoothService.shared.handle(method, args)
        default:
            return ""
        }
    }

    // MARK: - 剪贴板

    private static func clipboard(_ method: String, _ args: String) -> String {
        switch method {
        case "set":
            UIPasteboard.general.string = args
            return "ok"
        case "get":
            return UIPasteboard.general.string ?? ""
        case "has":
            return UIPasteboard.general.hasStrings ? "1" : "0"
        default:
            return ""
        }
    }

    // MARK: - 权限

    private static let notificationLock = NSLock()
    private static var notificationGranted = false
    private static var bluetoothPending = false

    private static func permission(_ method: String, _ code: String) -> String {
        switch method {
        case "check":
            return check(code)
        case "request":
            return request(code)
        case "openSettings":
            return openSettings()
        default:
            return ""
        }
    }

    /// 同步查询。iOS 的 `.notDetermined` 对 C++ 而言等同「未授权」。
    private static func check(_ code: String) -> String {
        switch code {
        case "camera":
            return cameraStatus(AVCaptureDevice.authorizationStatus(for: .video))
        case "microphone":
            switch AVAudioSession.sharedInstance().recordPermission {
            case .granted: return "granted"
            case .denied: return "denied"
            default: return "denied"
            }
        case "photos":
            return photoStatus(photoAuthorization())
        case "storage":
            // App 沙箱内没有运行时存储权限。
            return "granted"
        case "location":
            return locationStatus(locationAuthorization())
        case "contacts":
            return contactsStatus(CNContactStore.authorizationStatus(for: .contacts))
        case "bluetooth", "bluetoothScan":
            return bluetoothStatus(CBManager.authorization)
        case "notifications":
            notificationLock.lock()
            defer { notificationLock.unlock() }
            return notificationGranted ? "granted" : "denied"
        default:
            // 平台没有对应权限概念的代码按已授权处理（与 Android null 分支一致）。
            return "granted"
        }
    }

    /// 已授权直接返回；否则弹系统授权框并回 "pending"，完成后经 notify 回推。
    private static func request(_ code: String) -> String {
        let current = check(code)
        if current == "granted" || current == "limited" {
            return current
        }
        switch code {
        case "camera":
            AVCaptureDevice.requestAccess(for: .video) { granted in
                complete(code, granted ? "granted" : "denied")
            }
            return "pending"
        case "microphone":
            AVAudioSession.sharedInstance().requestRecordPermission { granted in
                complete(code, granted ? "granted" : "denied")
            }
            return "pending"
        case "photos":
            requestPhotoAuthorization { status in
                complete(code, photoStatus(status))
            }
            return "pending"
        case "location":
            CuiLocationPermission.shared.request { status in
                complete(code, status)
            }
            return "pending"
        case "contacts":
            CNContactStore().requestAccess(for: .contacts) { granted, _ in
                complete(code, granted ? "granted" : "denied")
            }
            return "pending"
        case "bluetooth", "bluetoothScan":
            // 首次创建 CBCentralManager 会弹出系统授权；结果在状态回调里回推。
            CuiBluetoothService.shared.ensureCentral()
            if CBManager.authorization == .notDetermined {
                bluetoothPending = true
                return "pending"
            }
            return bluetoothStatus(CBManager.authorization)
        case "notifications":
            UNUserNotificationCenter.current().requestAuthorization(
                options: [.alert, .badge, .sound]
            ) { granted, _ in
                notificationLock.lock()
                notificationGranted = granted
                notificationLock.unlock()
                complete(code, granted ? "granted" : "denied")
            }
            return "pending"
        default:
            return "granted"
        }
    }

    private static func openSettings() -> String {
        guard let url = URL(string: UIApplication.openSettingsURLString) else {
            return "error:no settings url"
        }
        UIApplication.shared.open(url)
        return "ok"
    }

    /// 异步结果回 C++：permission.cpp 在 pumpPlatformMessages() 里执行回调。
    static func complete(_ code: String, _ status: String) {
        DispatchQueue.main.async {
            CuiBridge.notify("cui/permission", "result", "\(code)\t\(status)")
        }
    }

    /// CBCentralManager 状态变化时由 CuiBluetoothService 调用：
    /// 若此前 request 因 .notDetermined 挂了账，这里补发蓝牙权限结果。
    static func bluetoothAuthorizationChanged() {
        guard bluetoothPending, CBManager.authorization != .notDetermined else {
            return
        }
        bluetoothPending = false
        let status = bluetoothStatus(CBManager.authorization)
        complete("bluetooth", status)
        complete("bluetoothScan", status)
    }

    // MARK: - 版本兼容入口

    private static func photoAuthorization() -> PHAuthorizationStatus {
        if #available(iOS 14.0, *) {
            return PHPhotoLibrary.authorizationStatus(for: .readWrite)
        }
        return PHPhotoLibrary.authorizationStatus()
    }

    private static func requestPhotoAuthorization(
        _ handler: @escaping (PHAuthorizationStatus) -> Void
    ) {
        if #available(iOS 14.0, *) {
            PHPhotoLibrary.requestAuthorization(for: .readWrite, handler: handler)
        } else {
            PHPhotoLibrary.requestAuthorization(handler)
        }
    }

    private static func locationAuthorization() -> CLAuthorizationStatus {
        if #available(iOS 14.0, *) {
            return CLLocationManager().authorizationStatus
        }
        return CLLocationManager.authorizationStatus()
    }

    // MARK: - 状态映射

    private static func cameraStatus(_ status: AVAuthorizationStatus) -> String {
        switch status {
        case .authorized: return "granted"
        case .denied: return "denied"
        case .restricted: return "restricted"
        default: return "denied"
        }
    }

    private static func photoStatus(_ status: PHAuthorizationStatus) -> String {
        switch status {
        case .authorized: return "granted"
        case .limited: return "limited"
        case .denied: return "denied"
        case .restricted: return "restricted"
        default: return "denied"
        }
    }

    private static func locationStatus(_ status: CLAuthorizationStatus) -> String {
        switch status {
        case .authorizedAlways, .authorizedWhenInUse: return "granted"
        case .denied: return "denied"
        case .restricted: return "restricted"
        default: return "denied"
        }
    }

    private static func contactsStatus(_ status: CNAuthorizationStatus) -> String {
        switch status {
        case .authorized: return "granted"
        case .denied: return "denied"
        case .restricted: return "restricted"
        @unknown default: return "denied"
        }
    }

    private static func bluetoothStatus(_ status: CBManagerAuthorization) -> String {
        switch status {
        case .allowedAlways: return "granted"
        case .denied: return "denied"
        case .restricted: return "restricted"
        default: return "denied"
        }
    }
}

/// CLLocationManager 的授权结果是一次性回调，用单例持有 manager 与 delegate。
final class CuiLocationPermission: NSObject, CLLocationManagerDelegate {
    static let shared = CuiLocationPermission()

    private let manager = CLLocationManager()
    private var completion: ((String) -> Void)?

    override init() {
        super.init()
        manager.delegate = self
    }

    func request(_ completion: @escaping (String) -> Void) {
        self.completion = completion
        manager.requestWhenInUseAuthorization()
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        finish(manager.authorizationStatus)
    }

    /// iOS 14 以下走旧的 delegate 方法。
    func locationManager(_ manager: CLLocationManager,
                         didChangeAuthorization status: CLAuthorizationStatus) {
        finish(status)
    }

    private func finish(_ status: CLAuthorizationStatus) {
        guard let completion else {
            return
        }
        let result: String
        switch status {
        case .authorizedAlways, .authorizedWhenInUse:
            result = "granted"
        case .denied:
            result = "denied"
        case .restricted:
            result = "restricted"
        default:
            return // .notDetermined：等用户操作
        }
        self.completion = nil
        completion(result)
    }
}

// MARK: - 系统 UI / 触感 / 导航

/// 系统 UI 控制（对应 Flutter 的 SystemChrome / HapticFeedback / SystemSound /
/// SystemNavigator）。宿主在根 ViewController 里转发这几个查询：
///   override var prefersStatusBarHidden: Bool { CuiSystemChrome.statusBarHidden }
///   override var preferredStatusBarStyle: UIStatusBarStyle { CuiSystemChrome.statusBarStyle }
///   override var prefersHomeIndicatorAutoHidden: Bool { CuiSystemChrome.homeIndicatorHidden }
///   override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
///       CuiSystemChrome.orientations
///   }
enum CuiSystemChrome {

    private static let lock = NSLock()
    private(set) static var statusBarHidden = false
    private(set) static var homeIndicatorHidden = false
    private(set) static var statusBarStyle: UIStatusBarStyle = .default
    private(set) static var orientations: UIInterfaceOrientationMask = .allButUpsideDown

    static func handle(method: String, args: String) -> String {
        switch method {
        case "SystemChrome.setPreferredOrientations":
            return setPreferredOrientations(args)
        case "SystemChrome.setApplicationSwitcherDescription":
            // iOS 的最近任务卡片由 CFBundleDisplayName / 应用图标决定，运行时
            // 没有对应 API。
            return "ok"
        case "SystemChrome.setEnabledSystemUIMode":
            return setEnabledSystemUIMode(args)
        case "SystemChrome.setEnabledSystemUIOverlays":
            return setEnabledSystemUIOverlays(args)
        case "SystemChrome.setSystemUIOverlayStyle":
            return setSystemUIOverlayStyle(args)
        case "SystemChrome.restoreSystemUIOverlays":
            updateSystemBars(hidden: false, homeIndicator: false)
            return "ok"
        case "HapticFeedback.vibrate":
            return hapticFeedback(args)
        case "SystemSound.play":
            return systemSound(args)
        case "SystemNavigator.pop":
            return popNavigator()
        case "SystemNavigator.setFrameworkHandlesBack":
            // iOS 没有系统返回键；开关只影响宿主自行转发的返回手势。
            return "ok"
        default:
            return ""
        }
    }

    // MARK: 方向

    private static func orientationMask(_ name: String) -> UIInterfaceOrientationMask? {
        switch name {
        case "DeviceOrientation.portraitUp": return .portrait
        case "DeviceOrientation.portraitDown": return .portraitUpsideDown
        case "DeviceOrientation.landscapeLeft": return .landscapeLeft
        case "DeviceOrientation.landscapeRight": return .landscapeRight
        default: return nil
        }
    }

    private static func setPreferredOrientations(_ args: String) -> String {
        let names = args.isEmpty ? [] : args.split(separator: ",").map(String.init)
        var mask: UIInterfaceOrientationMask = .allButUpsideDown
        if names.count == 1 {
            mask = orientationMask(names[0]) ?? .allButUpsideDown
        } else if names.count > 1 {
            mask = names.reduce(into: UIInterfaceOrientationMask()) { result, name in
                if let one = orientationMask(name) {
                    result.formUnion(one)
                }
            }
        }
        lock.lock()
        orientations = mask
        lock.unlock()
        DispatchQueue.main.async {
            if #available(iOS 16.0, *) {
                UIApplication.shared.connectedScenes
                    .compactMap { $0 as? UIWindowScene }
                    .first?
                    .requestGeometryUpdate(.iOS(interfaceOrientations: mask))
            } else if let value = singleOrientation(mask) {
                // iOS 15 及以下只能一次性转到某个方向。
                UIDevice.current.setValue(value.rawValue, forKey: "orientation")
            }
        }
        return "ok"
    }

    private static func singleOrientation(_ mask: UIInterfaceOrientationMask)
        -> UIInterfaceOrientation? {
        switch mask {
        case .portrait: return .portrait
        case .portraitUpsideDown: return .portraitUpsideDown
        case .landscapeLeft: return .landscapeLeft
        case .landscapeRight: return .landscapeRight
        default: return nil
        }
    }

    // MARK: 系统栏

    private static func setEnabledSystemUIMode(_ mode: String) -> String {
        let immersive = mode == "SystemUiMode.immersive"
            || mode == "SystemUiMode.immersiveSticky"
        updateSystemBars(hidden: immersive || mode == "SystemUiMode.leanBack",
                         homeIndicator: immersive)
        return "ok"
    }

    private static func setEnabledSystemUIOverlays(_ overlays: String) -> String {
        updateSystemBars(hidden: !overlays.contains("SystemUiOverlay.top"),
                         homeIndicator: !overlays.contains("SystemUiOverlay.bottom"))
        return "ok"
    }

    private static func updateSystemBars(hidden: Bool, homeIndicator: Bool) {
        lock.lock()
        statusBarHidden = hidden
        homeIndicatorHidden = homeIndicator
        lock.unlock()
        DispatchQueue.main.async {
            rootViewController()?.setNeedsStatusBarAppearanceUpdate()
            rootViewController()?.setNeedsUpdateOfHomeIndicatorAutoHidden()
        }
    }

    /// `key=value` 列表，见 C++ SystemUiOverlayStyle::encode()。
    private static func setSystemUIOverlayStyle(_ args: String) -> String {
        var fields: [String: String] = [:]
        for pair in args.split(separator: "\t") {
            guard let eq = pair.firstIndex(of: "=") else { continue }
            fields[String(pair[..<eq])] = String(pair[pair.index(after: eq)...])
        }
        var style = statusBarStyle
        if let icons = fields["statusBarIconBrightness"] {
            style = icons == "Brightness.dark" ? .default : .lightContent
        } else if let background = fields["statusBarBrightness"] {
            // iOS 声明的是「状态栏背景明暗」（与图标相反）；上游 light 表示
            // 深色背景 ⇒ 白色文字。
            style = background == "Brightness.light" ? .lightContent : .default
        }
        lock.lock()
        statusBarStyle = style
        lock.unlock()
        DispatchQueue.main.async {
            rootViewController()?.setNeedsStatusBarAppearanceUpdate()
        }
        // statusBarColor / systemNavigationBarColor 在 iOS 上不可设置。
        return "ok"
    }

    private static func rootViewController() -> UIViewController? {
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap { $0.windows }
            .first { $0.isKeyWindow }?
            .rootViewController
    }

    // MARK: 触感 / 提示音

    private static func hapticFeedback(_ type: String) -> String {
        switch type {
        case "", "HapticFeedbackType.vibrate":
            AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
        case "HapticFeedbackType.lightImpact":
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        case "HapticFeedbackType.mediumImpact":
            UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        case "HapticFeedbackType.heavyImpact":
            UIImpactFeedbackGenerator(style: .heavy).impactOccurred()
        case "HapticFeedbackType.selectionClick":
            UISelectionFeedbackGenerator().selectionChanged()
        case "HapticFeedbackType.successNotification":
            UINotificationFeedbackGenerator().notificationOccurred(.success)
        case "HapticFeedbackType.warningNotification":
            UINotificationFeedbackGenerator().notificationOccurred(.warning)
        case "HapticFeedbackType.errorNotification":
            UINotificationFeedbackGenerator().notificationOccurred(.error)
        default:
            AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
        }
        return "ok"
    }

    private static func systemSound(_ type: String) -> String {
        guard type == "SystemSoundType.click" else {
            return ""
        }
        // 1104 = Tock，系统常用的轻点声。
        AudioServicesPlaySystemSound(1104)
        return "ok"
    }

    // MARK: 导航

    private static func popNavigator() -> String {
        // iOS 不鼓励应用自行退出：只收起模态页；根页面由宿主决定是否关闭。
        DispatchQueue.main.async {
            rootViewController()?.dismiss(animated: true)
        }
        return "ok"
    }
}

/// 生命周期 / 返回键推送（AppDelegate / SceneDelegate 调用）。
enum CuiAppLifecycle {
    enum State: String {
        case resumed, inactive, hidden, paused, detached
    }

    static func notify(_ state: State) {
        CuiBridge.notify("cui/lifecycle", "AppLifecycleState.changed",
                         "AppLifecycleState.\(state.rawValue)")
    }

    /// 需要把某个手势/按键当作系统返回键时调用；结果由 C++ 侧处理器消费。
    static func notifyBack() {
        CuiBridge.notify("cui/system_navigator", "back", "")
    }
}
