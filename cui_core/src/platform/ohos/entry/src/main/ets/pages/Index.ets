/*
 * cui 的 HarmonyOS 渲染面 + 输入转发（与 native 的接缝）。
 *
 * 职责：一块全屏 XComponent（SURFACE 类型，libraryname "cui_core"）承担
 *   ① surface 生命周期 → cui.startSurface / resizeSurface / stopSurface
 *   ② 触摸 / 鼠标 / 键盘 / 滚轮 → cui.dispatchXxx（坐标一律换算成**物理像素**）
 *   ③ 屏幕密度 → cui.setDensity
 * 顺带在页面存活期挂上平台方法通道（cui.install / uninstall）。
 *
 * ── 事件名与单位的出处（本机 SDK：DevEco sdk/default/openharmony，apiVersion 26；
 *    与 E:\dev-cache\ohos-sdk\26.0.0 的声明逐字一致）─────────────────────
 *  · surface 回调**不是** XComponent 的属性方法，而是 `XComponentController`
 *    的成员，靠子类覆写拿到（ets/component/xcomponent.d.ts:263/277/289，@since 12）：
 *        onSurfaceCreated(surfaceId: string): void
 *        onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void
 *        onSurfaceDestroyed(surfaceId: string): void
 *    SurfaceRect.surfaceWidth / surfaceHeight 的单位已经是 **px**（同文件 42-91 行），
 *    所以下面不做换算。
 *  · 输入四类都在 `CommonMethod`（ets/component/common.d.ts）：
 *        onMouse(Callback<MouseEvent>)     :18618   onTouch(...) :18632
 *        onKeyEvent(...)                   :18643/18656
 *        onAxisEvent(Callback<AxisEvent>)  :18727（@since 17）
 *  · 滚轮这条路选 `onAxisEvent`，不是 onMouseEvent：ArkUI 里鼠标滚轮/触控板滚动
 *    走 axis 通道（`AxisEvent` 定义在 common.d.ts:9722，@since 17），增量字段
 *    `axisHorizontal`/`axisVertical` 挂在 **BaseEvent** 上（common.d.ts:8039/8057，
 *    @since 12，单位 vp），`AxisEvent.scrollStep`（:9840 附近）才是「格数」。
 *    本仓把 vp 换算成 px 后按**像素**下传（native 侧对应 PlatformEvent::deltaY
 *    「正值向下」+ wheelInNotches=false 的口径，见 include/cui/platform/event.h:77-81）。
 *    旧的 `PanGesture` 那条路不需要：触摸滚动已经由 onTouch 的 Move 序列表达。
 *  · 密度：`UIContext` 在本 SDK 里**没有** getDensityPixels()（只有 vp2px/px2vp 与
 *    'densityUpdate' 监听，api/@ohos.arkui.UIContext.d.ts:5140/5152/1651），所以取
 *    `display.getDefaultDisplaySync().densityPixels`（api/@ohos.display.d.ts:1330，
 *    物理像素/逻辑像素系数）。vp→px 的换算统一用 `this.getUIContext().vp2px(v)`
 *    （全局函数 vp2px 在 @since 18 已废弃，common.d.ts:5988 那条注释里写明改用
 *    UIContext#vp2px）。
 *
 * ── 尚未接（留给后续批次）───────────────────────────────────────────────
 *  · 软键盘 / InputConnection：这里只转发物理键盘（keyText），IME 的 preedit/
 *    commit 需要 TextInputHandle 一套（Android 侧同样未接，见 agents.md）。
 *  · 多指之外的 hover/pointer 细节、触控板惯性（fling）事件。
 *  · 转屏/分屏导致的 densityUpdate 重报：只在 surface 创建与尺寸变化时上报一次
 *    setDensity，真机上如出现密度跳变需补 UIContext.on('densityUpdate')。
 */

import cui from 'libcui_core.so';
import display from '@ohos.display';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { installCuiPlatform, uninstallCuiPlatform } from '../cui/CuiBridge';
import { CuiPlatformServices } from '../cui/CuiPlatformServices';

const TAG: string = 'cui';

/** 物理像素 = 逻辑像素(vp) × densityPixels；两者在 ArkUI 里由 vp2px 同一系数换算。 */
function currentDensity(): number {
  try {
    const info = display.getDefaultDisplaySync();
    return info.densityPixels;
  } catch (e) {
    hilog.warn(0x0000, TAG, '%{public}s', 'densityPixels unavailable, fallback 1.0');
    return 1.0;
  }
}

/**
 * surface 生命周期：把 XComponent 的 surfaceId 交给 native（libcui_core.so 里
 * 通过 OH_NativeWindow_CreateNativeWindowFromSurfaceId 接管，见窗口/呈现那一批），
 * 尺寸变化直接把 SurfaceRect 的 px 值下传。
 */
class CuiSurfaceController extends XComponentController {
  onSurfaceCreated(surfaceId: string): void {
    hilog.info(0x0000, TAG, 'surface created %{public}s', surfaceId);
    // 密度先给一次：startSurface 之后 native 会按窗口尺寸建图，setDensity 影响
    // dpiScale（布局仍用物理像素，本仓 Android 侧同口径）。
    cui.setDensity(currentDensity());
    cui.startSurface(surfaceId);
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    // rect.surfaceWidth / rect.surfaceHeight 单位已是 px（xcomponent.d.ts:60-88）。
    cui.setDensity(currentDensity());
    cui.resizeSurface(surfaceId, rect.surfaceWidth, rect.surfaceHeight);
  }

  onSurfaceDestroyed(surfaceId: string): void {
    hilog.info(0x0000, TAG, 'surface destroyed %{public}s', surfaceId);
    cui.stopSurface(surfaceId);
  }
}

@Entry
@Component
struct Index {
  // XComponent 的 controller：SURFACE 类型的回调全靠它（见文件头注释）。
  private surfaceCtrl: CuiSurfaceController = new CuiSurfaceController();

  aboutToAppear(): void {
    // 平台方法通道：C++ → ArkTS 的同步分发（协议见 cui/CuiBridge.ets 头注释）。
    installCuiPlatform((channel: string, method: string, args: string): string => {
      return CuiPlatformServices.handle(channel, method, args);
    });
  }

  aboutToDisappear(): void {
    uninstallCuiPlatform();
  }

  // ── vp → 物理像素（native 的输入事件与布局都用物理像素）──────────────────
  private px(valueInVp: number): number {
    return this.getUIContext().vp2px(valueInVp);
  }

  /**
   * 按下键时补齐、松开键时剔除的按钮位掩码：bit = 1 << MouseButton（Left=0、
   * Right=1、Middle=2、Back=3、Forward=4，enums.d.ts:703）。
   * MouseEvent.pressedButtons 是「当前按下的按钮集合」（common.d.ts 内 MouseEvent
   * 声明，@since 18；缺省时退回单个 event.button）。
   */
  private buttonMask(event: MouseEvent): number {
    let mask: number = 0;
    const pressed: MouseButton[] | undefined = event.pressedButtons;
    if (pressed !== undefined) {
      for (let i = 0; i < pressed.length; i++) {
        if (pressed[i] >= 0) {
          mask |= (1 << pressed[i]);
        }
      }
    }
    if (event.action === MouseAction.Press) {
      if (event.button >= 0) {
        mask |= (1 << event.button);
      }
    } else if (event.action === MouseAction.Release) {
      if (event.button >= 0) {
        mask &= ~(1 << event.button);
      }
    }
    return mask;
  }

  build() {
    // @Entry 的 build 只允许**一个根节点，且必须是容器组件**（编译器规则
    // 10905210），所以全屏 XComponent 外面套一层 Stack；Stack 用 TopStart 对齐，
    // 保证子节点左上角与窗口原点重合 —— XComponent 的事件坐标是相对**自己**的，
    // 因此与 surface 像素坐标同原点，换算里没有额外平移。
    Stack() {
      XComponent({
        id: 'cui_surface',
        type: XComponentType.SURFACE,
        libraryname: 'cui_core',
        controller: this.surfaceCtrl
      })
        .width('100%')
        .height('100%')
        // ── 触摸：changedTouches 里每个点一条事件（actionType = TouchType，
        //    Down=0 / Up=1 / Move=2 / Cancel=3 / HOVER_*=9..11，enums.d.ts:574）──
        .onTouch((event: TouchEvent) => {
          const points: TouchObject[] = event.changedTouches;
          for (let i = 0; i < points.length; i++) {
            const p: TouchObject = points[i];
            // p.x / p.y：相对本组件左上角，单位 vp（common.d.ts:9392/9405，@since 7）
            // —— 正好与 surface 的坐标原点一致，只需换成 px。
            cui.dispatchTouch(p.type, p.id, this.px(p.x), this.px(p.y));
          }
        })
        // ── 鼠标：action = MouseAction（Press=0/Release=1/Move=2/Hover=3/
        //    ENTER_WINDOW=4/LEAVE_WINDOW=5，enums.d.ts:768）─────────────────
        .onMouse((event: MouseEvent) => {
          cui.dispatchMouse(event.action, this.px(event.x), this.px(event.y),
            this.buttonMask(event));
        })
        // ── 键盘：action = KeyType（Down=0/Up=1/LongPress=3/Click=4…，
        //    enums.d.ts:1171）；keyCode 是 ArkUI 物理键码；text = keyText（未接 IME）
        .onKeyEvent((event: KeyEvent) => {
          cui.dispatchKey(event.type, event.keyCode, event.keyText);
          // 全键吞掉：cui 自己管焦点与滚动，不能让系统默认行为（Tab 移焦、方向键
          // 滚动页面）再处理一遍。返回 void 即「已消费」，如需系统兜底改成
          // onKeyEvent((e) => boolean)。
        })
        // ── 滚轮 / 触控板：见文件头「滚轮这条路」注释 ────────────────────────
        .onAxisEvent((event: AxisEvent) => {
          // axisHorizontal / axisVertical 在 BaseEvent 上（@since 12，单位 vp）。
          const dxVp: number = event.axisHorizontal ?? 0;
          const dyVp: number = event.axisVertical ?? 0;
          // event.x / event.y：相对本组件，单位 vp。滚轮增量按像素下传
          // （native 侧 wheelInNotches=false 的那一档）。
          cui.dispatchWheel(event.action, this.px(event.x), this.px(event.y),
            this.px(dxVp), this.px(dyVp));
        });
    }
    .width('100%')
    .height('100%')
    .alignContent(Alignment.TopStart)
    .clip(true);
  }
}
