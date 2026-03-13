#pragma once
/// @file ohos_window.h
/// @brief OpenHarmony (HarmonyOS Next) implementation of PlatformWindow.
///
/// 形状与 Android 端一致：CPU 光栅出帧（Skia N32 → native buffer 逐行 memcpy），
/// 输入由宿主（ArkTS XComponent + napi）转发进来，本类不自建事件循环。
/// surface 由 ArkTS 侧创建，所以 create()/show() 只记录状态，真正「可渲染」是
/// attachSurface() 成功那一刻。
///
/// 呈现走 buffer-queue 路线（RequestBuffer → Map → memcpy → Unmap → FlushBuffer），
/// 不是 LockBuffer：后者是 API 23 才有，本端要对 12+ 生效。
///
/// 参数数值口径 —— **ArkTS 组件枚举**，不是 NDK 的 <ace/xcomponent/*> 枚举：
/// 输入由 ArkTS 的 onTouch/onMouse/onKeyEvent/onAxisEvent 转发进来（这个 SDK 的
/// native XComponent 接口只有 OH_NativeXComponent_GetNativeXComponent，没有
/// ArkUI_NativeXComponentAPI::getInstance，拿不到原生分发回调），所以数值取自
/// SDK 的 ets/component/enums.d.ts。两套编号只在触摸上巧合一致，鼠标差一整位：
///   dispatchTouch(action)    TouchType      0=Down 1=Up 2=Move 3=Cancel
///                            9..12=HOVER_ENTER/MOVE/EXIT/CANCEL（本类忽略，
///                            悬停统一走 dispatchMouse，避免双计）
///   dispatchMouse(action)    MouseAction    0=Press 1=Release 2=Move 3=Hover
///                            4=ENTER_WINDOW 5=LEAVE_WINDOW 13=CANCEL
///                            （NDK 那边是 0=NONE 1=PRESS 2=RELEASE 3=MOVE 4=CANCEL）
///                 buttons    位掩码 bit = MouseButton（Left=0 Right=1 Middle=2
///                            Back=3 Forward=4）⇒ 与 NDK 的 0x01/0x02/0x04/0x08/0x10
///                            恰好逐位一致，宿主两种写法都能喂进来
///   dispatchKey(action)      KeyType        0=Down 1=Up 3=CANCEL（NDK KeyAction 同值）
///                 keyCode    @ohos.multimodalInput.keyCode（KEYCODE_ENTER=2054，
///                            与 NDK OH_NativeXComponent_KeyCode 同一套编号）
///   dispatchWheel(action)    AxisAction     0=NONE 1=BEGIN 2=UPDATE 3=END 4=CANCEL
///                 dx/dy      单位=像素，dy>0 向下（宿主已把 vp 换成 px，见
///                              event.h 的 kDefaultWheelPixelsPerNotch 另一档）
/// 坐标与尺寸一律是 XComponent 的**物理像素**（同 Android 端；setDensity 只影响
/// dpiScale()，不参与坐标换算）。
///
/// 线程口径：事件队列可跨线程投递（wakeUp 会从别的工作线程打来），但
/// OH_NativeWindow_* / OH_NativeBuffer_* 官方声明为非线程安全接口，所以
/// attach/resize/detach/dispatch*/presentPixels 必须都来自事件循环那一根线程
/// （ArkTS 宿主即 JS 线程，天然满足）。
///
/// 要求 OpenHarmony NDK API 12+；非 OHOS 平台整个类被编译掉。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>

#include "platform_window.h"

#ifdef __OHOS__

/// SDK 里 `OHNativeWindow` 是 `typedef struct NativeWindow OHNativeWindow;`，
/// 所以前向声明只能写结构体标签：直接 `struct OHNativeWindow;` 会和那条 typedef
/// 撞成「typedef redefinition with different types」。
struct NativeWindow;

namespace cui {

class OhosWindow : public PlatformWindow {
public:
    OhosWindow();
    ~OhosWindow() override;

    // ── PlatformWindow ────────────────────────────────────────────────────
    void create(const std::string& title, int width, int height) override;
    void destroy() override;
    bool pollEvent(PlatformEvent& event) override;
    bool waitForEvent(PlatformEvent& event, int timeoutMs) override;
    void invalidate() override;
    void show() override;
    int width() const override { return width_.load(); }
    int height() const override { return height_.load(); }
    void wakeUp() override;
    void setTitle(const std::string& title) override;
    void setMinimumSize(int width, int height) override;
    double dpiScale() const override;
    NativeHandles nativeHandles() override;
    bool presentPixels(const void* pixels, int rowBytes, int x, int y, int width,
                       int height) override;

    // 触摸不需要 grab（与 Android 同）。
    void grabPointer() override {}
    void ungrabPointer() override {}

    // ── napi 宿主契约（函数名与语义固定，见文件头）────────────────────────
    /// 取 surfaceId 对应的 OHNativeWindow、钉好 buffer 几何/格式/usage，进入可渲染态。
    bool attachSurface(uint64_t surfaceId);
    /// surface 尺寸变化（XComponentController::onSurfaceChanged 的 surfaceWidth/
    /// surfaceHeight，单位已是 px）。
    void resizeSurface(uint64_t surfaceId, int32_t width, int32_t height);
    /// surface 销毁（XComponent 隐藏 / 组件卸载）：只放开 buffer 队列，不推 Close。
    void detachSurface(uint64_t surfaceId);

    void dispatchTouch(int32_t actionType, int32_t pointerId, float x, float y);
    void dispatchMouse(int32_t action, float x, float y, int32_t buttons);
    void dispatchWheel(int32_t action, float x, float y, float dx, float dy);
    void dispatchKey(int32_t action, int32_t keyCode, const std::string& text);

    /// density ≤ 10 视作倍数（display 的 densityPixels，如 2.0），
    /// > 10 视作 dpi（densityDpi，按 160 归一）；只影响 dpiScale()。
    void setDensity(float density);

    bool hasSurface() const { return window_ != nullptr; }
    uint64_t surfaceId() const { return surfaceId_; }

private:
    /// 让 buffer 队列按 (width, height) 重新分配，并同步记录尺寸。
    void applyGeometry(int32_t width, int32_t height);
    /// 一帧的提交（整帧拷贝：双缓冲后备区不保证保留上一帧，脏区提交会花屏）。
    bool flushFrame(const void* pixels, int rowBytes, int width, int height);
    void pushEvent(const PlatformEvent& event);

    static int keyFromOhosKeyCode(int32_t keyCode);
    static int buttonFromOhosMask(int32_t buttons);

    NativeWindow* window_ = nullptr;  ///< = SDK 的 OHNativeWindow*
    uint64_t surfaceId_ = 0;

    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    float density_ = 1.0f;

    /// 最近一次鼠标按下报告的按钮，CANCEL 时用它补一次 MouseUp。
    int lastMouseButton_ = 0;

    std::string title_;
    std::queue<PlatformEvent> pendingEvents_;
    std::mutex eventMutex_;
    std::condition_variable eventCv_;
};

}  // namespace cui_core

#endif  // __OHOS__
