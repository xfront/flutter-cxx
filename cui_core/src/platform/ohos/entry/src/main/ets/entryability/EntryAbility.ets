/*
 * cui Gallery 的 HarmonyOS 宿主 Ability（entry/src/main/ets/entryability）。
 *
 * 与 Android 侧 dev.cui.framework.CuiActivity 对位，职责只有三件：
 *   1. 注入 UIAbilityContext 给平台服务（相机/蓝牙/剪贴板/权限都靠它）；
 *   2. 把生命周期与返回键推给 C++（通道协议见 cui/CuiBridge.ets 顶部注释）；
 *   3. 装载 pages/Index —— 真正的内容是一块全屏 XComponent，surface 与输入
 *      转发都在那里接（见 pages/Index.ets）。
 *
 * 方法通道（cui.install）刻意放在 **页面** 里而不是这里：页面的
 * aboutToAppear/aboutToDisappear 与 XComponent 的存活期一一对应，Ability 的
 * onWindowStageCreate/Destroy 粒度更粗（且 loadContent 是异步回调，先后顺序
 * 不如页面可控）。
 *
 * 窗口全屏/沉浸（setWindowLayoutFullScreen）与 Android 的 edge-to-edge 同理，
 * 本批**未接**：XComponent 会拿到去掉系统栏后的窗口区域，cui 侧的布局尺寸
 * 由 resizeSurface 上报的物理像素决定，因此不做 safe area 处理。
 */

import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { initCuiPlatformServices, notifyCuiLifecycle, notifyCuiBack } from '../cui/CuiPlatformServices';

const DOMAIN: number = 0x0000;
const TAG: string = 'cui';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // 平台服务的上下文（CuiCameraService/CuiBluetoothService 都从这里取）。
    initCuiPlatformServices(this.context);
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onCreate');
  }

  onDestroy(): void {
    // 'detached' = 引擎可以收尾了（对齐 Android 的 onDestroy 通知）。
    notifyCuiLifecycle('detached');
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onWindowStageCreate');
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'Failed to load the content. Cause: %{public}s',
          JSON.stringify(err) ?? '');
        return;
      }
      hilog.info(DOMAIN, TAG, '%{public}s', 'content loaded');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    notifyCuiLifecycle('resumed');
  }

  onBackground(): void {
    notifyCuiLifecycle('hidden');
  }

  // 系统返回键 / 手势返回：交给 C++ 侧的处理器（未被消费时引擎退出应用）。
  onBackPressed(): boolean {
    notifyCuiBack();
    // 返回 true = 本 Ability 已消费，系统不再做默认「退出」。cui 侧没消费时
    // 会自己调用 context.terminateSelf()（见 services 的 back 通道实现）。
    return true;
  }
}
