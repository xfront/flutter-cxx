#ifndef CUI_IOS_BRIDGE_H
#define CUI_IOS_BRIDGE_H

/// cui_core 引擎导出的 C 桥（实现见 src/platform/services/ios_channel.cpp）。
///
/// Swift 侧通过 bridging header 引入本文件；符号名与 Android 的 JNI 入口、
/// OHOS 的 NAPI 导出一一对应，参数均为 UTF-8 C 字符串。
///
///   cui_ios_register_host_dispatch(dispatch)   // C++ → 宿主（每 App 一次）
///   cui_ios_unregister_host_dispatch()
///   cui_ios_method_call(channel, method, args) // 宿主 → C++（同步）
///   cui_ios_notify(channel, method, args)      // 宿主 → C++（下一帧派发）
///   cui_ios_emit_event(channel, event)         // EventChannel 事件
///
/// 所有权：
///   - dispatch 返回的 char* 由**引擎** free（宿主用 strdup 分配）。
///   - cui_ios_method_call 返回的 char* 由**宿主** free。

#ifdef __cplusplus
extern "C" {
#endif

typedef char *(*CuiIosHostDispatch)(const char *channel, const char *method,
                                    const char *args);

void cui_ios_register_host_dispatch(CuiIosHostDispatch dispatch);
void cui_ios_unregister_host_dispatch(void);
char *cui_ios_method_call(const char *channel, const char *method,
                          const char *args);
void cui_ios_notify(const char *channel, const char *method, const char *args);
void cui_ios_emit_event(const char *channel, const char *event);

#ifdef __cplusplus
}
#endif

#endif /* CUI_IOS_BRIDGE_H */
