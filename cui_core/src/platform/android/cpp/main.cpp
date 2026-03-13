/// @file main.cpp — Android 宿主入口。
///
/// NativeActivity（Java 侧 dev.cui_core.framework.CuiActivity）加载 libcui_gallery.so
/// 后，android_native_app_glue 调用 android_main()；本文件负责：
///   1. 把 android_app 的 surface / focus / input 回调转给 cui_core::AndroidWindow；
///   2. 用 Application 的单窗口流水线挂上 cui_core::createAppRoot() 注入的应用根；
///   3. 每轮吃 android 事件 + runIteration()（事件分发、ticker、渲染、上屏）。
///
/// 上屏走 AppWindow::blitToNativeWindow() 的 Android 分支（CPU 光栅 +
/// ANativeWindow_lock 整帧拷贝），所以不需要 Vulkan/EGL 上下文。

#include <android/log.h>
#include <android_native_app_glue.h>

#include <chrono>
#include <thread>

#include "cui/foundation/application.h"
#include "cui/foundation/translator.h"
#include "cui/foundation/window.h"
#include "cui/framework/app_root.h"  // cui_core::createAppRoot()：由应用层实现的注入点
#include "cui/platform/android_window.h"

namespace {

constexpr const char* kTag = "cui_core-host";

struct Host {
    cui::Application app;
    cui::AndroidWindow* window = nullptr;  // 归 AppWindow 所有，这里只借用
    bool started = false;
};

void startUi(Host& host, android_app* state) {
    if (host.started || !state->window)
        return;

    const int width = ANativeWindow_getWidth(state->window);
    const int height = ANativeWindow_getHeight(state->window);

    cui::AppWindow* appWindow = host.app.createWindow("CUI", width, height);
    if (!appWindow) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "createWindow failed");
        return;
    }
    host.window = static_cast<cui::AndroidWindow*>(appWindow->platformWindow());
    if (!host.window) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "platform window is not AndroidWindow");
        return;
    }

    // 配置密度决定 PlatformWindow::dpiScale()（160 DPI = 1.0）。当前配置挂在
    // android_app 上（ANativeActivity 自 NDK r9 起已没有 config 字段）。
    if (state->config) {
        const int dpi = AConfiguration_getDensity(state->config);
        if (dpi > 0)
            host.window->setDensity(dpi);
    }

    // surface 已经就绪（APP_CMD_INIT_WINDOW 里调用），把它接上再出第一帧。
    host.window->onSurfaceCreated(state->window);

    cui::Translator::instance().setLanguage("zh");
    host.app.setRoot(cui::createAppRoot());
    host.app.renderFrame(/*forceFull=*/true);

    host.started = true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "ui started (%dx%d)", width, height);
}

void handleAppCmd(android_app* state, int32_t cmd) {
    auto* host = static_cast<Host*>(state->userData);
    if (!host)
        return;

    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (!host->started) {
                startUi(*host, state);
            } else if (host->window && state->window) {
                // 切回前台 / 旋转后 surface 重建：重新接管并全帧重绘。
                host->window->onSurfaceCreated(state->window);
                host->window->onSurfaceChanged(ANativeWindow_getWidth(state->window),
                                               ANativeWindow_getHeight(state->window));
            }
            break;

        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if (host->window && state->window) {
                host->window->onSurfaceChanged(ANativeWindow_getWidth(state->window),
                                               ANativeWindow_getHeight(state->window));
            }
            break;

        case APP_CMD_TERM_WINDOW:
            // surface 被回收（切后台 / 旋转中间态）。窗口本身仍然活着：
            // AndroidWindow::onSurfaceDestroyed() 只释放 ANativeWindow。
            if (host->window)
                host->window->onSurfaceDestroyed();
            break;

        case APP_CMD_GAINED_FOCUS:
            if (host->window)
                host->window->onFocusChanged(true);
            break;

        case APP_CMD_LOST_FOCUS:
            if (host->window)
                host->window->onFocusChanged(false);
            break;

        case APP_CMD_DESTROY:
            host->app.quit();
            break;

        default:
            break;
    }
}

int32_t handleInput(android_app* state, AInputEvent* event) {
    auto* host = static_cast<Host*>(state->userData);
    if (!host || !host->window)
        return 0;
    // AndroidWindow 把 touch 翻译成 Touch* + Mouse*（指针事件兼容层）。
    return host->window->onInputEvent(event) ? 1 : 0;
}

}  // namespace

void android_main(android_app* state) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "android_main enter");

    Host host;
    state->userData = &host;
    state->onAppCmd = handleAppCmd;
    state->onInputEvent = handleInput;

    // 本线程就是 GUI 线程：beginEventLoop() 置 running_ 并接好平台通道的
    // wakeUp 钩子（不调的话 runIteration() 直接返回 false，循环空转一次就退）。
    host.app.beginEventLoop();

    while (!state->destroyRequested) {
        // 先非阻塞吃完排队的命令/输入：surface 未就绪时这里是唯一的进展来源。
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(0, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source)
                source->process(state, source);
            if (state->destroyRequested)
                break;
        }
        if (state->destroyRequested) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "loop exit: destroyRequested");
            break;
        }

        if (!host.started) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }

        // runIteration()：drain 事件 → 平台消息 → ticker → 任务（含渲染）；
        // 没事件时它在帧周期内阻塞等待，AndroidWindow::waitForEvent 内部
        // 会用 ALooper_pollOnce 顺带驱动上面的 glue 回调。
        if (!host.app.runIteration()) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "loop exit: runIteration false");
            break;
        }
    }

    host.app.endEventLoop();
    __android_log_print(ANDROID_LOG_INFO, kTag, "android_main exit");
}
