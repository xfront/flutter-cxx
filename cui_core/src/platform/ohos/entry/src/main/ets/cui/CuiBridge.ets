/**
 * cui 平台通道的 ArkTS 入口（HarmonyOS）。
 *
 * 与 Android 侧 CuiActivity.java 对位：
 *   Android: nativeInstallCuiPlatform(activity) / nativeCuiMethodCall(...) / notifyCui(...)
 *   OHOS:    cui.install(dispatch)            / cui.methodCall(...)         / cui.notify(...)
 *
 * 接线（Ability 里各调一次）：
 *   import { installCuiPlatform, uninstallCuiPlatform } from '../cui/CuiBridge'
 *   import { CuiPlatformServices, initCuiPlatformServices,
 *            notifyCuiLifecycle, notifyCuiBack } from '../cui/CuiPlatformServices'
 *
 *   onCreate():             initCuiPlatformServices(this.context)
 *   onWindowStageCreate():  installCuiPlatform((c, m, a) => CuiPlatformServices.handle(c, m, a))
 *   onWindowStageDestroy(): uninstallCuiPlatform()
 *   onForeground():         notifyCuiLifecycle('resumed')   // 可选先发 'inactive'
 *   onBackground():         notifyCuiLifecycle('hidden')
 *   onDestroy():            notifyCuiLifecycle('detached')
 *   onBackPressed():        notifyCuiBack()   // C++ 侧处理器消费，未消费时引擎退出应用
 *
 * module.json5 需要的权限（按页面实际使用裁剪）：
 *   "ohos.permission.CAMERA"
 *   "ohos.permission.MICROPHONE"
 *   "ohos.permission.APPROXIMATELY_LOCATION"
 *   "ohos.permission.ACCESS_BLUETOOTH"
 *   "ohos.permission.READ_PASTEBOARD"        // 读取剪贴板
 *   "ohos.permission.READ_IMAGEVIDEO"        // 相册/媒体读取
 *   "ohos.permission.READ_CONTACTS"
 *
 * 注意：NativeActivity 时代不复存在；cui 引擎需要被编成 libcui_core.so 并由本模块
 * 载入。OHOS 的窗口/输入后端（PlatformWindow）尚未实现，当前只接平台服务。
 */

import cui from 'libcui_core.so'

export type CuiMethodDispatcher = (channel: string, method: string, args: string) => string;

/** 注册 C++ → ArkTS 的同步分发函数（一次即可，重复调用会替换旧的）。 */
export function installCuiPlatform(dispatch: CuiMethodDispatcher): boolean {
  return cui.install(dispatch) === true;
}

/** 撤销注册（Ability 销毁时调用；之后 C++ 侧 invokeMethod 返回空）。 */
export function uninstallCuiPlatform(): void {
  cui.uninstall();
}

/** ArkTS → C++：同步方法调用（对应 Android 的 nativeCuiMethodCall）。 */
export function cuiMethodCall(channel: string, method: string, args: string): string {
  return cui.methodCall(channel, method, args);
}

/** ArkTS → C++：异步结果/状态通知（对应 Android 的 notifyCui，下一帧派发）。 */
export function notifyCui(channel: string, method: string, args: string): void {
  cui.notify(channel, method, args);
}

/** ArkTS → C++：EventChannel 事件（对应 Android 的 emitCuiEvent）。 */
export function emitCuiEvent(channel: string, event: string): void {
  cui.emitEvent(channel, event);
}
