#ifndef CUI_IOS_BRIDGING_H
#define CUI_IOS_BRIDGING_H

/// Swift 侧（CuiApp.swift）通过本 bridging header 调用引擎的 C / ObjC++ 入口。
/// 把它设为 Xcode 工程的 SWIFT_OBJC_BRIDGING_HEADER（或 XcodeGen 的
/// bridgingHeader 字段）。

#import "cui_ios_bridge.h"  // C 平台通道桥（cui_ios_method_call 等）

#ifdef __cplusplus
extern "C" {
#endif

/// 起引擎（建窗口、挂应用根、启动帧泵）。由 CuiHost.mm 实现。
void cuiHostBoot(void);

/// 停引擎（停止帧泵、结束事件循环）。由 CuiHost.mm 实现。
void cuiHostShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* CUI_IOS_BRIDGING_H */
