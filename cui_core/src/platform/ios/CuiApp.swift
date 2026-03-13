/// @file CuiApp.swift
/// @brief iOS 宿主 App（对应 Android 的 CuiActivity / OHOS 的 EntryAbility）。
///
/// 进程入口：@UIApplicationMain 让 UIKit 用本 delegate 启动。didFinishLaunching
/// 里先 CuiBridge.install()（注册 C++ → Swift 的平台通道分发），再 cuiHostBoot()
/// 起引擎；退出前 uninstall + cuiHostShutdown。
///
/// 窗口由引擎内部创建（IOSWindow::create 建全屏 UIWindow 并 makeKeyAndVisible），
/// 本文件不持有 UIWindow。

import UIKit

@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
    ) -> Bool {
        // 注册 C++ → Swift 的平台通道分发（相机/蓝牙/平台服务等）。
        CuiBridge.install()
        // 起引擎：建窗口 → 挂 createAppRoot() 注入的应用根 → 启动 CADisplayLink 帧泵。
        cuiHostBoot()
        return true
    }

    func applicationWillTerminate(_ application: UIApplication) {
        cuiHostShutdown()
        CuiBridge.uninstall()
    }
}
