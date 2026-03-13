/**
 * 蓝牙服务的 OHOS 实现（对应 Android 的 CuiBluetoothService.java）。
 *
 * 通道协议（与 C++ bluetooth.cpp / Android 完全一致）：
 *   listen / cancel        → "ok"
 *   available              → "1" | "0"
 *   state                  → off|on|turningOn|turningOff|unauthorized|unsupported|unknown
 *   requestEnable          → "ok" | "error:..."（OHOS 普通应用无权开关蓝牙）
 *   bondedDevices          → "id\tname\trssi\tpaired" 每行一台
 *   startScan              → "ok" | "error:..."
 *   stopScan               → "ok"
 *   connect <id>           → "ok" | "error:..."
 *   disconnect <id>        → "ok"
 *
 * 事件（经 emitCuiEvent 推）：
 *   device\t<id>\t<name>\t<rssi>\t0     扫描发现
 *   state\t<adapter state>               适配器状态变化
 *   connection\t<id>\t<state>            连接状态变化
 *   error\t<message>                     扫描/连接失败
 *
 * ⚠️ 参考实现：@ohos.bluetooth.* 各子模块的导入路径与 startBLEScan /
 *    GattClientDevice 的签名随 SDK 版本变化（新 SDK 走 @kit.ConnectivityKit）；
 *    上真机前按实际 SDK 校准。扫描/连接需要 ohos.permission.ACCESS_BLUETOOTH，
 *    配对仍由系统设置页负责。
 */

import access from '@ohos.bluetooth.access';
import ble from '@ohos.bluetooth.ble';
// API 26 口径：连接状态**值**的枚举在 @ohos.bluetooth.constant 里
// （constant.ProfileConnectionState：DISCONNECTED=0 / CONNECTING=1 / CONNECTED=2 /
// DISCONNECTING=3）。ble 命名空间只给了类型别名 `ble.ProfileConnectionState`
// （= constant.ProfileConnectionState），别名拿不到枚举成员，所以取值必须走 constant。
import constant from '@ohos.bluetooth.constant';
import connection from '@ohos.bluetooth.connection';
import abilityAccessCtrl, { Permissions } from '@ohos.abilityAccessCtrl';
import bundleManager from '@ohos.bundle.bundleManager';
import { BusinessError } from '@ohos.base';
import { emitCuiEvent } from './CuiBridge';

const CHANNEL = 'cui/bluetooth';
const PERMISSION: Permissions = 'ohos.permission.ACCESS_BLUETOOTH' as Permissions;

export class CuiBluetoothService {
  private static scanning: boolean = false;
  private static clients: Map<string, ble.GattClientDevice> = new Map();
  // GattClientDevice.on('BLEConnectionStateChange') 的回调参数是**事件对象**
  // ble.BLEConnectionChangeState（{ deviceId, state, reason? }），不是状态枚举本身。
  private static listeners: Map<string, (change: ble.BLEConnectionChangeState) => void> = new Map();
  private static watchingState: boolean = false;
  private static scanListener: ((results: Array<ble.ScanResult>) => void) | undefined = undefined;

  /** Ability.onCreate 里由 CuiPlatformServices 调用一次。 */
  static init(): void {
    if (CuiBluetoothService.watchingState) {
      return;
    }
    try {
      // 未授权时注册会抛 201；等用户授权后由 startScan 再尝试一次。
      access.on('stateChange', (state: access.BluetoothState) => {
        emitCuiEvent(CHANNEL, `state\t${CuiBluetoothService.stateName(state)}`);
      });
      CuiBluetoothService.watchingState = true;
    } catch (err) {
      CuiBluetoothService.watchingState = false;
    }
  }

  static handle(method: string, args: string): string {
    switch (method) {
      case 'listen':
      case 'cancel':
        return 'ok';
      case 'available':
        return CuiBluetoothService.available() ? '1' : '0';
      case 'state':
        return CuiBluetoothService.state();
      case 'requestEnable':
        // OHOS 只允许系统应用调用 access.enableBluetooth()，普通应用无权限。
        return 'error:not permitted';
      case 'bondedDevices':
        return CuiBluetoothService.bondedDevices();
      case 'startScan':
        return CuiBluetoothService.startScan();
      case 'stopScan':
        CuiBluetoothService.stopScan();
        return 'ok';
      case 'connect':
        return CuiBluetoothService.connect(args);
      case 'disconnect':
        CuiBluetoothService.disconnect(args);
        return 'ok';
      default:
        return '';
    }
  }

  // ── 适配器 ───────────────────────────────────────────────────────────────

  private static available(): boolean {
    try {
      access.getState();
      return true;
    } catch (err) {
      return false;
    }
  }

  private static state(): string {
    if (!CuiBluetoothService.hasPermission()) {
      return 'unauthorized';
    }
    try {
      return CuiBluetoothService.stateName(access.getState());
    } catch (err) {
      // 201: 权限不足；未初始化蓝牙服务时同样走到这里。
      return 'unsupported';
    }
  }

  private static stateName(state: access.BluetoothState): string {
    switch (state) {
      case access.BluetoothState.STATE_ON:
      case access.BluetoothState.STATE_BLE_ON:
        // BLE_ON 表示经典蓝牙关闭但 BLE 栈可用；对只做 GATT 的调用方等价于 on。
        return 'on';
      case access.BluetoothState.STATE_OFF:
        return 'off';
      case access.BluetoothState.STATE_TURNING_ON:
      case access.BluetoothState.STATE_BLE_TURNING_ON:
        return 'turningOn';
      case access.BluetoothState.STATE_TURNING_OFF:
      case access.BluetoothState.STATE_BLE_TURNING_OFF:
        return 'turningOff';
      default:
        return 'unknown';
    }
  }

  private static hasPermission(): boolean {
    try {
      const info = bundleManager.getBundleInfoForSelfSync(
        bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION
      );
      const status = abilityAccessCtrl
        .createAtManager()
        .verifyAccessTokenSync(info.appInfo.accessTokenId, PERMISSION);
      return status === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED;
    } catch (err) {
      return false;
    }
  }

  // ── 已配对设备 ───────────────────────────────────────────────────────────

  private static bondedDevices(): string {
    if (!CuiBluetoothService.hasPermission()) {
      return '';
    }
    const lines: string[] = [];
    try {
      const addresses: Array<string> = connection.getPairedDevices();
      for (const address of addresses) {
        let name = '';
        try {
          // 部分设备名受隐私保护不可读，按空名展示。
          name = connection.getRemoteDeviceName(address);
        } catch (err) {
          name = '';
        }
        lines.push(`${address}\t${clean(name)}\t0\t1`);
      }
    } catch (err) {
      return '';
    }
    return lines.join('\n');
  }

  // ── 扫描 ─────────────────────────────────────────────────────────────────

  private static startScan(): string {
    if (!CuiBluetoothService.hasPermission()) {
      return 'error:scan permission denied';
    }
    let state: access.BluetoothState;
    try {
      state = access.getState();
    } catch (err) {
      return 'error:unsupported';
    }
    if (state !== access.BluetoothState.STATE_ON && state !== access.BluetoothState.STATE_BLE_ON) {
      return 'error:bluetooth is off';
    }
    CuiBluetoothService.stopScan();
    CuiBluetoothService.init();

    CuiBluetoothService.scanListener = (results: Array<ble.ScanResult>) => {
      for (const result of results) {
        emitCuiEvent(
          CHANNEL,
          `device\t${result.deviceId}\t${clean(result.deviceName ?? '')}\t${result.rssi}\t0`
        );
      }
    };
    try {
      ble.on('BLEDeviceFind', CuiBluetoothService.scanListener);
      ble.startBLEScan([], {
        interval: 0,
        dutyMode: ble.ScanDuty.SCAN_MODE_LOW_LATENCY,
        matchMode: ble.MatchMode.MATCH_MODE_AGGRESSIVE
      });
      CuiBluetoothService.scanning = true;
      return 'ok';
    } catch (err) {
      CuiBluetoothService.removeScanListener();
      return `error:${(err as BusinessError).message}`;
    }
  }

  private static stopScan(): void {
    if (!CuiBluetoothService.scanning) {
      CuiBluetoothService.removeScanListener();
      return;
    }
    try {
      ble.stopBLEScan();
    } catch (err) {
      // 适配器已关闭时 stop 会抛，忽略并继续清状态。
    }
    CuiBluetoothService.removeScanListener();
    CuiBluetoothService.scanning = false;
  }

  private static removeScanListener(): void {
    if (CuiBluetoothService.scanListener === undefined) {
      return;
    }
    try {
      ble.off('BLEDeviceFind', CuiBluetoothService.scanListener);
    } catch (err) {
      // 监听器可能随适配器关闭自动失效。
    }
    CuiBluetoothService.scanListener = undefined;
  }

  // ── GATT 连接 ────────────────────────────────────────────────────────────

  private static connect(deviceId: string): string {
    if (deviceId.length === 0) {
      return 'error:no device id';
    }
    if (!CuiBluetoothService.hasPermission()) {
      return 'error:connect permission denied';
    }
    CuiBluetoothService.disconnect(deviceId);

    let client: ble.GattClientDevice;
    try {
      client = ble.createGattClientDevice(deviceId);
    } catch (err) {
      return `error:${(err as BusinessError).message}`;
    }
    const listener = (change: ble.BLEConnectionChangeState) => {
      emitCuiEvent(
        CHANNEL,
        `connection\t${deviceId}\t${CuiBluetoothService.connectionName(change.state)}`
      );
      if (change.state === constant.ProfileConnectionState.STATE_DISCONNECTED) {
        CuiBluetoothService.dropClient(deviceId);
      }
    };
    try {
      client.on('BLEConnectionStateChange', listener);
      CuiBluetoothService.clients.set(deviceId, client);
      CuiBluetoothService.listeners.set(deviceId, listener);
      client.connect();
    } catch (err) {
      CuiBluetoothService.dropClient(deviceId);
      return `error:${(err as BusinessError).message}`;
    }
    emitCuiEvent(CHANNEL, `connection\t${deviceId}\tconnecting`);
    return 'ok';
  }

  private static disconnect(deviceId: string): void {
    const client = CuiBluetoothService.clients.get(deviceId);
    if (client === undefined) {
      return;
    }
    try {
      client.disconnect();
    } catch (err) {
      // 已断开时 disconnect 会抛，直接走清理。
    }
    CuiBluetoothService.dropClient(deviceId);
    emitCuiEvent(CHANNEL, `connection\t${deviceId}\tdisconnected`);
  }

  private static dropClient(deviceId: string): void {
    const client = CuiBluetoothService.clients.get(deviceId);
    if (client === undefined) {
      return;
    }
    const listener = CuiBluetoothService.listeners.get(deviceId);
    if (listener !== undefined) {
      try {
        client.off('BLEConnectionStateChange', listener);
      } catch (err) {
        // 监听器可能已随连接销毁。
      }
    }
    try {
      client.close();
    } catch (err) {
      // 同上。
    }
    CuiBluetoothService.clients.delete(deviceId);
    CuiBluetoothService.listeners.delete(deviceId);
  }

  private static connectionName(state: ble.ProfileConnectionState): string {
    switch (state) {
      case constant.ProfileConnectionState.STATE_CONNECTED:
        return 'connected';
      case constant.ProfileConnectionState.STATE_CONNECTING:
        return 'connecting';
      case constant.ProfileConnectionState.STATE_DISCONNECTING:
        return 'disconnecting';
      default:
        return 'disconnected';
    }
  }
}

function clean(value: string): string {
  return value.replace(/\t/g, ' ').replace(/\n/g, ' ');
}
