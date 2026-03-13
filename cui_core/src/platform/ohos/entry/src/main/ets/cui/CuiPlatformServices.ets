/**
 * 平台通道的 ArkTS 分发器（对应 Android 的 CuiPlatformServices.java）。
 *
 * 通道协议与 Android 完全一致：
 *   cui/platform   ping                      → "ok"
 *                  SystemChrome.* / HapticFeedback.vibrate / SystemSound.play /
 *                  SystemNavigator.*        → "ok"（方法名/参数编码与
 *                                              system_chrome.h 一致）
 *   cui/clipboard  set/get/has               → "ok" / 文本 / "1"|"0"
 *   cui/permission check/request/openSettings→ "granted|denied|unavailable" /
 *                                              "pending"（结果经 notify 回推）
 *   cui/camera     见 CuiCameraService.ets
 *   cui/bluetooth  见 CuiBluetoothService.ets
 *
 * 生命周期 / 返回键由 Ability 主动推送（引擎侧的通道在 pump 时自动安装）：
 *   onForeground() → notifyCuiLifecycle('resumed')
 *   onBackground() → notifyCuiLifecycle('hidden')   // 如需 inactive 自行补发
 *   onDestroy()    → notifyCuiLifecycle('detached')
 *   onBackPressed()→ notifyCuiBack()
 * 见 CuiBridge.ets 顶部的接线说明。
 *
 * ⚠️ 参考实现：OHOS SDK 版本间 API 签名有差异（pasteboard 的 *Sync 系列、
 *    verifyAccessTokenSync 的可用版本、设置页的 bundleName），上真机前按
 *    实际 SDK 校准。
 */

import pasteboard from '@ohos.pasteboard';
import abilityAccessCtrl, { Permissions } from '@ohos.abilityAccessCtrl';
import bundleManager from '@ohos.bundle.bundleManager';
import common from '@ohos.app.ability.common';
import window from '@ohos.window';
import vibrator from '@ohos.vibrator';
import { notifyCui } from './CuiBridge';
import { CuiCameraService } from './CuiCameraService';
import { CuiBluetoothService } from './CuiBluetoothService';

let context: common.UIAbilityContext | undefined = undefined;

/** Ability.onCreate 里调用一次：注入上下文，初始化各子服务。 */
export function initCuiPlatformServices(ctx: common.UIAbilityContext): void {
  context = ctx;
  CuiCameraService.init(ctx);
  CuiBluetoothService.init();
}

export class CuiPlatformServices {
  static handle(channel: string, method: string, args: string): string {
    switch (channel) {
      case 'cui/platform':
        return method === 'ping' ? 'ok' : platform(method, args);
      case 'cui/clipboard':
        return clipboard(method, args);
      case 'cui/permission':
        return permission(method, args);
      case 'cui/camera':
        return CuiCameraService.handle(method, args);
      case 'cui/bluetooth':
        return CuiBluetoothService.handle(method, args);
      default:
        return '';
    }
  }
}

// ── 剪贴板 ──────────────────────────────────────────────────────────────────

function clipboard(method: string, args: string): string {
  const board = pasteboard.getSystemPasteboard();
  try {
    if (method === 'set') {
      board.setDataSync(pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, args));
      return 'ok';
    }
    if (method === 'get') {
      return board.getDataSync().getPrimaryText() ?? '';
    }
    if (method === 'has') {
      const text = board.getDataSync().getPrimaryText();
      return text !== undefined && text.length > 0 ? '1' : '0';
    }
  } catch (err) {
    // 空剪贴板 / 无读取权限时 getDataSync 会抛异常：按"没有内容"处理。
    return method === 'has' ? '0' : '';
  }
  return '';
}

// ── 权限 ────────────────────────────────────────────────────────────────────

function permissionFor(code: string): string {
  switch (code) {
    case 'camera':
      return 'ohos.permission.CAMERA';
    case 'microphone':
      return 'ohos.permission.MICROPHONE';
    case 'storage':
    case 'photos':
      return 'ohos.permission.READ_IMAGEVIDEO';
    case 'location':
      return 'ohos.permission.APPROXIMATELY_LOCATION';
    case 'bluetooth':
    case 'bluetoothScan':
      return 'ohos.permission.ACCESS_BLUETOOTH';
    case 'contacts':
      return 'ohos.permission.READ_CONTACTS';
    case 'notifications':
      // 普通应用无需权限即可发通知；这里返回空串由 checkPermission 视为 granted。
      return '';
    default:
      return code;
  }
}

function accessTokenId(): number {
  const info = bundleManager.getBundleInfoForSelfSync(
    bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION
  );
  return info.appInfo.accessTokenId;
}

export function checkCuiPermission(code: string): string {
  const name = permissionFor(code);
  if (name.length === 0) {
    return 'granted';
  }
  try {
    const status = abilityAccessCtrl
      .createAtManager()
      .verifyAccessTokenSync(accessTokenId(), name as Permissions);
    return status === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED ? 'granted' : 'denied';
  } catch (err) {
    return 'unavailable';
  }
}

function requestPermission(code: string): string {
  if (context === undefined) {
    return 'unavailable';
  }
  const name = permissionFor(code);
  if (name.length === 0) {
    return 'granted';
  }
  try {
    abilityAccessCtrl
      .createAtManager()
      .requestPermissionsFromUser(context, [name as Permissions])
      // 回调参数不写显式类型：abilityAccessCtrl.PermissionRequestResult 虽然在
      // d.ts 里 `export type`（:807），但把它当**命名空间成员**引用
      // （abilityAccessCtrl.PermissionRequestResult）在本 SDK 的通道路径下解析不到，
      // 而 Promise 的泛型已经把 result 推成同样的结构 ⇒ 交给上下文推断。
      .then((result) => {
        const granted = result.authResults.length > 0 && result.authResults[0] === 0;
        // 结果异步回 C++：permission.cpp 在 pumpPlatformMessages() 里执行回调。
        notifyCui('cui/permission', 'result', `${code}\t${granted ? 'granted' : 'denied'}`);
      })
      .catch(() => {
        notifyCui('cui/permission', 'result', `${code}\tdenied`);
      });
    return 'pending';
  } catch (err) {
    return 'unavailable';
  }
}

function openSettings(): string {
  if (context === undefined) {
    return 'error:no context';
  }
  context
    .startAbility({
      bundleName: 'com.huawei.hmos.settings',
      abilityName: 'com.huawei.hmos.settings.MainAbility',
      uri: 'application_info_entry',
      parameters: { pushParams: context.abilityInfo.bundleName }
    })
    .catch(() => {});
  return 'ok';
}

function permission(method: string, code: string): string {
  if (method === 'check') {
    return checkCuiPermission(code);
  }
  if (method === 'request') {
    return requestPermission(code);
  }
  if (method === 'openSettings') {
    return openSettings();
  }
  return '';
}

// ── 系统 UI / 触感 / 导航 ───────────────────────────────────────────────────

/** Ability 生命周期推送（引擎在 pump 时已装好 cui/lifecycle 处理器）。 */
export function notifyCuiLifecycle(state: string): void {
  notifyCui('cui/lifecycle', 'AppLifecycleState.changed', `AppLifecycleState.${state}`);
}

/** Ability.onBackPressed 里调用：结果由 C++ 侧处理器消费并决定是否退出。 */
export function notifyCuiBack(): void {
  notifyCui('cui/system_navigator', 'back', '');
}

async function mainWindow(): Promise<window.Window | undefined> {
  if (context === undefined) {
    return undefined;
  }
  try {
    return await window.getLastWindow(context);
  } catch (err) {
    return undefined;
  }
}

function orientationFor(name: string): window.Orientation | undefined {
  switch (name) {
    case 'DeviceOrientation.portraitUp':
      return window.Orientation.PORTRAIT;
    case 'DeviceOrientation.portraitDown':
      return window.Orientation.PORTRAIT_INVERTED;
    case 'DeviceOrientation.landscapeLeft':
      return window.Orientation.LANDSCAPE;
    case 'DeviceOrientation.landscapeRight':
      return window.Orientation.LANDSCAPE_INVERTED;
    default:
      return undefined;
  }
}

function setPreferredOrientations(args: string): string {
  if (context === undefined) {
    return 'unavailable';
  }
  const names = args.length === 0 ? [] : args.split(',');
  // 单个方向锁死；多方向用 AUTO_ROTATION（由系统按传感器选）。
  const orientation = names.length === 1
    ? orientationFor(names[0])
    : window.Orientation.AUTO_ROTATION;
  mainWindow().then((win) => {
    if (win === undefined) {
      return;
    }
    win.setPreferredOrientation(orientation ?? window.Orientation.UNSPECIFIED).catch(() => {});
  });
  return 'ok';
}

function setApplicationSwitcherDescription(args: string): string {
  // OHOS 的最近任务卡片由 module.json5 的 ability label/icon 决定，运行时
  // 只能改应用名（bundleManager.setApplicationLabel 需要系统权限）。
  return context === undefined ? 'unavailable' : 'ok';
}

let lastUiMode = 'SystemUiMode.edgeToEdge';
let lastOverlays = 'SystemUiOverlay.top,SystemUiOverlay.bottom';

function setEnabledSystemUIMode(mode: string): string {
  lastUiMode = mode;
  if (context === undefined) {
    return 'unavailable';
  }
  mainWindow().then((win) => {
    if (win === undefined) {
      return;
    }
    win.setWindowLayoutFullScreen(mode === 'SystemUiMode.edgeToEdge').catch(() => {});
    if (mode === 'SystemUiMode.leanBack') {
      win.setWindowSystemBarEnable(['status', 'navigation']).catch(() => {});
    } else if (mode === 'SystemUiMode.immersive'
      || mode === 'SystemUiMode.immersiveSticky') {
      win.setWindowSystemBarEnable([]).catch(() => {});
    }
  });
  return 'ok';
}

function setEnabledSystemUIOverlays(overlays: string): string {
  lastUiMode = 'SystemUiMode.manual';
  lastOverlays = overlays;
  if (context === undefined) {
    return 'unavailable';
  }
  const bars: Array<'status' | 'navigation'> = [];
  if (overlays.indexOf('SystemUiOverlay.top') >= 0) {
    bars.push('status');
  }
  if (overlays.indexOf('SystemUiOverlay.bottom') >= 0) {
    bars.push('navigation');
  }
  mainWindow().then((win) => {
    win?.setWindowSystemBarEnable(bars).catch(() => {});
  });
  return 'ok';
}

/** `key=value` 列表，见 SystemUiOverlayStyle::encode()。 */
function setSystemUIOverlayStyle(args: string): string {
  if (context === undefined) {
    return 'unavailable';
  }
  const fields: Map<string, string> = new Map();
  for (const pair of args.split('\t')) {
    const eq = pair.indexOf('=');
    if (eq > 0) {
      fields.set(pair.substring(0, eq), pair.substring(eq + 1));
    }
  }
  const properties: window.SystemBarProperties = {};
  const navColor = fields.get('systemNavigationBarColor');
  if (navColor !== undefined) {
    properties.navigationBarColor = navColor;
  }
  const statusColor = fields.get('statusBarColor');
  if (statusColor !== undefined) {
    properties.statusBarColor = statusColor;
  }
  const statusIcons = fields.get('statusBarIconBrightness');
  if (statusIcons !== undefined) {
    properties.statusBarContentColor = statusIcons === 'Brightness.dark' ? '#000000' : '#FFFFFF';
  }
  const navIcons = fields.get('systemNavigationBarIconBrightness');
  if (navIcons !== undefined) {
    properties.navigationBarContentColor =
      navIcons === 'Brightness.dark' ? '#000000' : '#FFFFFF';
  }
  // statusBarBrightness 只对 iOS 有意义。
  mainWindow().then((win) => {
    win?.setWindowSystemBarProperties(properties).catch(() => {});
  });
  return 'ok';
}

function hapticFeedback(type: string): string {
  if (type === '' || type === 'HapticFeedbackType.vibrate') {
    vibrator.startVibration({ type: 'time', duration: 50 }, { id: 0, usage: 'unknown' })
      .catch(() => {});
    return 'ok';
  }
  // HarmonyOS 只有一组时长档位，按强度近似映射。
  let duration = 20;
  if (type === 'HapticFeedbackType.mediumImpact'
    || type === 'HapticFeedbackType.selectionClick') {
    duration = 30;
  } else if (type === 'HapticFeedbackType.heavyImpact'
    || type === 'HapticFeedbackType.errorNotification') {
    duration = 60;
  } else if (type === 'HapticFeedbackType.successNotification'
    || type === 'HapticFeedbackType.warningNotification') {
    duration = 40;
  }
  vibrator.startVibration({ type: 'time', duration }, { id: 0, usage: 'unknown' })
    .catch(() => {});
  return 'ok';
}

function systemSound(type: string): string {
  // HarmonyOS 没有公开的系统提示音播放接口；宿主可自行接 AVPlayer。
  return type === 'SystemSoundType.click' ? 'unavailable' : '';
}

function popNavigator(animated: string): string {
  if (context === undefined) {
    return 'unavailable';
  }
  context.terminateSelf().catch(() => {});
  return 'ok';
}

function platform(method: string, args: string): string {
  switch (method) {
    case 'SystemChrome.setPreferredOrientations':
      return setPreferredOrientations(args);
    case 'SystemChrome.setApplicationSwitcherDescription':
      return setApplicationSwitcherDescription(args);
    case 'SystemChrome.setEnabledSystemUIMode':
      return setEnabledSystemUIMode(args);
    case 'SystemChrome.setEnabledSystemUIOverlays':
      return setEnabledSystemUIOverlays(args);
    case 'SystemChrome.setSystemUIOverlayStyle':
      return setSystemUIOverlayStyle(args);
    case 'SystemChrome.restoreSystemUIOverlays':
      return lastUiMode === 'SystemUiMode.manual'
        ? setEnabledSystemUIOverlays(lastOverlays)
        : setEnabledSystemUIMode(lastUiMode);
    case 'HapticFeedback.vibrate':
      return hapticFeedback(args);
    case 'SystemSound.play':
      return systemSound(args);
    case 'SystemNavigator.pop':
      return popNavigator(args);
    case 'SystemNavigator.setFrameworkHandlesBack':
      // OHOS 的返回键总是先交给 onBackPressed，无需额外开关。
      return 'ok';
    default:
      return '';
  }
}
