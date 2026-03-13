import Foundation

/// cui 平台通道的 Swift 入口（对应 Android 的 CuiActivity / OHOS 的 CuiBridge.ets）。
///
/// 接线（每个 App 一次）：
///   AppDelegate.application(_:didFinishLaunchingWithOptions:) 或 SceneDelegate 中
///   `CuiBridge.install()`；退出前 `CuiBridge.uninstall()`。
///   引擎帧泵在 UI 线程运行，`(channel, method, args) => String` 分发即
///   `CuiPlatformServices.handle(...)`。
///
/// Info.plist 需要的用途说明（按页面实际使用裁剪）：
///   NSCameraUsageDescription
///   NSMicrophoneUsageDescription
///   NSBluetoothAlwaysUsageDescription
///   NSPhotoLibraryUsageDescription
///   NSLocationWhenInUseUsageDescription
///   NSContactsUsageDescription
/// 后台 BLE（如需）在 UIBackgroundModes 里加 "bluetooth-central"。
///
/// ⚠️ 参考实现：引擎需以静态库 / framework 形式链接进 App；本文件未在本机
///    iOS 工具链编译，接入时请用 Xcode 校准签名与权限弹窗文案。
enum CuiBridge {

    /// 注册 C++ → Swift 的分发函数。
    static func install() {
        cui_ios_register_host_dispatch(cuiHostDispatch)
    }

    /// 撤销注册（之后 C++ 侧 invokeMethod 返回空）。
    static func uninstall() {
        cui_ios_unregister_host_dispatch()
    }

    /// Swift → C++ 同步调用（对应 Android 的 nativeCuiMethodCall）。
    static func methodCall(_ channel: String, _ method: String, _ args: String) -> String {
        guard let raw = cui_ios_method_call(channel, method, args) else {
            return ""
        }
        defer { free(raw) }
        return String(cString: raw)
    }

    /// Swift → C++ 异步通知（对应 Android 的 notifyCui，下一帧派发）。
    static func notify(_ channel: String, _ method: String, _ args: String) {
        cui_ios_notify(channel, method, args)
    }

    /// EventChannel 事件推送。
    static func emitEvent(_ channel: String, _ event: String) {
        cui_ios_emit_event(channel, event)
    }
}

/// C 函数指针落点：无捕获，可直接作为 `CuiIosHostDispatch`。
private func cuiHostDispatch(_ channel: UnsafePointer<CChar>?,
                             _ method: UnsafePointer<CChar>?,
                             _ args: UnsafePointer<CChar>?) -> UnsafeMutablePointer<CChar>? {
    let channelStr = channel.map { String(cString: $0) } ?? ""
    let methodStr = method.map { String(cString: $0) } ?? ""
    let argsStr = args.map { String(cString: $0) } ?? ""
    let reply = CuiPlatformServices.handle(channel: channelStr, method: methodStr, args: argsStr)
    return strdup(reply)
}
