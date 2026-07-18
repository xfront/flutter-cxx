/**
 * Type declarations for the cui_core native module (`libcui_core.so`).
 *
 * The library registers a NAPI module named "cui_core" (see
 * src/platform/services/ohos_channel.cpp), so ArkTS imports it as
 * `import cui_core from 'libcui_core.so'`.
 */

export type CuiMethodDispatcher = (channel: string, method: string, args: string) => string;

export function install(dispatch: CuiMethodDispatcher): boolean;
export function uninstall(): void;
export function methodCall(channel: string, method: string, args: string): string;
export function notify(channel: string, method: string, args: string): void;
export function emitEvent(channel: string, event: string): void;

// ── Surface / window（由 pages/Index.ets 的 XComponent 驱动）────────────────
// 约定：**所有**尺寸与坐标都是物理像素（ArkTS 侧用 UIContext#vp2px 换算好的），
// density 是 physical/logical 系数（display.getDefaultDisplaySync().densityPixels）。

/** XComponent 的 surface 已创建：native 侧据此接管 NativeWindow 并起帧循环。 */
export function startSurface(surfaceId: string): void;

/** surface 尺寸变化（含首次给出尺寸）：width/height 为物理像素。 */
export function resizeSurface(surfaceId: string, width: number, height: number): void;

/** surface 即将销毁：native 侧停帧并释放 NativeWindow 引用。 */
export function stopSurface(surfaceId: string): void;

/** 上报屏幕密度（物理像素/逻辑像素系数），影响 dpiScale()。 */
export function setDensity(density: number): void;

// ── 输入转发 ────────────────────────────────────────────────────────────────

/**
 * 触摸：changedTouches 每个点一条。
 * actionType 取 ArkUI 的 TouchType 数值（Down=0、Up=1、Move=2、Cancel=3、
 * HOVER_ENTER=9、HOVER_MOVE=10、HOVER_EXIT=11）；x/y 相对 surface 左上角，物理像素。
 */
export function dispatchTouch(actionType: number, pointerId: number, x: number, y: number): void;

/** 鼠标：action 取 MouseAction（Press=0、Release=1、Move=2、Hover=3、
 *  ENTER_WINDOW=4、LEAVE_WINDOW=5）；buttons 是位掩码 1<<MouseButton。 */
export function dispatchMouse(action: number, x: number, y: number, buttons: number): void;

/** 滚轮/触控板（ArkUI 的 onAxisEvent）：action 取 AxisAction（NONE=0、BEGIN=1、
 *  UPDATE=2、END=3、CANCEL=4）；deltaX/deltaY 为**像素**增量（native 侧
 *  wheelInNotches=false 那一档，dy 正值为向下），x/y 是事件位置的物理像素。 */
export function dispatchWheel(action: number, x: number, y: number,
  deltaX: number, deltaY: number): void;

/** 键盘：action 取 KeyType（Down=0、Up=1、LongPress=3、Click=4…）；keyCode 为
 *  ArkUI 物理键码；text 为 KeyEvent.keyText（软键盘 IME 未接，物理键盘路径）。 */
export function dispatchKey(action: number, keyCode: number, text: string): void;

declare const cui: {
  install: typeof install;
  uninstall: typeof uninstall;
  methodCall: typeof methodCall;
  notify: typeof notify;
  emitEvent: typeof emitEvent;
  startSurface: typeof startSurface;
  resizeSurface: typeof resizeSurface;
  stopSurface: typeof stopSurface;
  setDensity: typeof setDensity;
  dispatchTouch: typeof dispatchTouch;
  dispatchMouse: typeof dispatchMouse;
  dispatchWheel: typeof dispatchWheel;
  dispatchKey: typeof dispatchKey;
};

export default cui;
