/// @file main.cpp — OpenHarmony 宿主入口（libcui.so 的 napi 侧）。
///
/// 与 android/cpp/main.cpp 同层，但启动方式相反：Android 是 NativeActivity 直接
/// 加载 .so 并调 android_main()；这里 .so 由 ArkTS `import cui_core from 'libcui.so'`
/// 加载，napi 模块「cui_core」的注册已经由引擎自己做掉（services/ohos_channel.cpp 的
/// constructor + `napi_module.g_module`，模块名必须是 "cui_core"，与 XComponent 的
/// `libraryname: 'cui_core'` 同一条）。所以本文件**不再注册第二个模块**，只通过那条弱
/// 符号钩子把 surface/输入这 8 个函数追加到同一个 exports 上 —— 用弱符号而不是硬
/// 引用：demo_gallery / demo_ide 这类没有宿主的 ohos 目标也链得动同一套引擎。
///
/// 线程模型（这是本文件存在的主要理由）：
///   · JS 线程：所有 napi 函数在这里被调用。ArkTS 的平台通道回调（cui_core.install 那个
///     dispatcher）也必须由 JS 线程的事件循环执行，所以 GUI 循环**不能**占住这根
///     线程 —— 否则 C++ → ArkTS 的同步调用只能等到 kCrossThreadTimeout 超时。
///   · GUI 线程：`Application::beginEventLoop()` / `runIteration()` / `endEventLoop()`
///     自持一条循环（与 android_main 同构，agents.md 的铁律：自带主循环的宿主必须
///     begin/end，否则 runIteration 第一拍返回 false，一帧不出）。
///   ⇒ OHNativeWindow 的 attach/resize/detach 一律**编组到 GUI 线程**执行：
///     native_window/native_buffer 不是线程安全接口，而 presentPixels 天然只在 GUI
///     线程跑。dispatch* 四类只往 OhosWindow 的互斥事件队列里投事件，可以留在 JS
///     线程（那根线程也是这些数据的唯一写者）。
///   · .so 卸载（应用被杀）不做优雅停机：stopHost() 供将来/测试用，置标志 + join。

#include <node_api.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <hilog/log.h>
#include <native_window/external_window.h>

#include "cui/foundation/application.h"
#include "cui/foundation/translator.h"
#include "cui/foundation/window.h"
#include "cui/framework/app_root.h"  // cui_core::createAppRoot()：由应用层实现的注入点
#include "cui/platform/ohos_window.h"

#undef LOG_TAG
#define LOG_TAG "cui_core"
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0201
#define LOGI(...) ((void)OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__))
#define LOGW(...) ((void)OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, __VA_ARGS__))

namespace {

/// JS 线程投给 GUI 线程的 surface 操作；除 Density 外都带 surfaceId。
struct SurfaceOp {
    enum class Kind : int32_t { Attach, Resize, Detach, Density };
    Kind kind = Kind::Attach;
    uint64_t surfaceId = 0;
    int32_t width = 0;
    int32_t height = 0;
    float density = 0.0f;
};

struct Host {
    /// GUI 线程自己创建；JS 线程不碰。
    std::unique_ptr<cui::Application> app;
    std::thread guiThread;

    std::mutex mutex;
    std::deque<SurfaceOp> ops;
    float density = 1.0f;  ///< 最近一次上报的密度（首帧之前就要有值）

    std::atomic<cui::OhosWindow*> window{nullptr};
    std::atomic<bool> guiStarted{false};
    std::mutex startMutex;  ///< 保证 GUI 线程只起一次
};

Host& host() {
    static Host h;
    return h;
}

void postOp(SurfaceOp op) {
    auto& h = host();
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        // 同 kind + 同 surfaceId 的 resize/density 只需保留最后一份；attach 与
        // detach 不合并（重建 surface 的次序有意义）。
        if (op.kind == SurfaceOp::Kind::Resize || op.kind == SurfaceOp::Kind::Density) {
            for (auto& pending : h.ops) {
                if (pending.kind == op.kind && pending.surfaceId == op.surfaceId) {
                    pending = op;
                    return;
                }
            }
        }
        h.ops.push_back(std::move(op));
    }
    // 叫醒 GUI 线程：它可能正阻塞在 waitForEvent 的帧周期里。
    if (cui::OhosWindow* w = h.window.load())
        w->wakeUp();
}

/// onSurfaceCreated 只给 surfaceId，尺寸要自己问（onSurfaceChanged 紧随其后，
/// 但首帧不该等它）。这里临时换一个 OHNativeWindow 只为读几何；真正接管它的是
/// GUI 线程上的 OhosWindow::attachSurface。
bool querySurfaceGeometry(uint64_t surfaceId, int32_t& width, int32_t& height) {
    OHNativeWindow* window = nullptr;
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window) != 0 || !window)
        return false;
    width = height = 0;
    // GET_BUFFER_GEOMETRY 的可变参数顺序是「先 height 再 width」（头里明写）。
    OH_NativeWindow_NativeWindowHandleOpt(window, GET_BUFFER_GEOMETRY, &height, &width);
    OH_NativeWindow_DestroyNativeWindow(window);
    return width > 0 && height > 0;
}

/// 起 UI：建窗口 → 接管 surface → 挂 createAppRoot() 注入的应用根 → 出第一帧。
/// 只在 GUI 线程调用，且只成功一次（AppWindow 建出来就不回收）。
/// 返回 false 只有一种情况：窗口还没建出来（尺寸未知或 createWindow 失败），
/// 调用方下一轮重试；attach 失败不算失败，控件树照挂，等下一次 Attach。
bool startUi(cui::Application& app, Host& h, uint64_t surfaceId, int32_t width, int32_t height) {
    cui::AppWindow* appWindow = app.createWindow("CUI", width, height);
    if (!appWindow) {
        LOGE("createWindow(%{public}dx%{public}d) failed", width, height);
        return false;
    }
    auto* window = static_cast<cui::OhosWindow*>(appWindow->platformWindow());
    if (!window) {
        LOGE("platform window is not OhosWindow");
        return false;
    }
    window->setDensity(h.density);
    if (!window->attachSurface(surfaceId))
        LOGW("attachSurface(%{public}llu) failed，等下一次 Attach",
             static_cast<unsigned long long>(surfaceId));

    cui::Translator::instance().setLanguage("zh");
    app.setRoot(cui::createAppRoot());
    app.renderFrame(/*forceFull=*/true);

    h.window.store(window);
    LOGI("ui started (%{public}dx%{public}d)", window->width(), window->height());
    return true;
}

void drainOps(cui::Application& app, Host& h) {
    std::deque<SurfaceOp> ops;
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        ops.swap(h.ops);
    }

    for (const SurfaceOp& op : ops) {
        cui::OhosWindow* window = h.window.load();
        switch (op.kind) {
            case SurfaceOp::Kind::Density:
                h.density = op.density;
                if (window)
                    window->setDensity(op.density);
                break;

            case SurfaceOp::Kind::Resize:
                if (window) {
                    window->resizeSurface(op.surfaceId, op.width, op.height);
                    // attach 曾经失败过（surface 当时还没备好）：这里补一次接管。
                    if (!window->hasSurface()) {
                        window->attachSurface(op.surfaceId);
                        app.renderFrame(/*forceFull=*/true);
                    }
                }
                break;

            case SurfaceOp::Kind::Detach:
                if (window) {
                    // 只放开 buffer 队列，窗口与控件树留着（XComponent 隐藏/转屏
                    // 都会走这条，随后还有一次 Attach）。
                    window->detachSurface(op.surfaceId);
                }
                break;

            case SurfaceOp::Kind::Attach:
                if (window) {
                    // surface 重建（转屏 / 切回前台）：换 id 才重新接管并全帧重绘。
                    if (window->surfaceId() != op.surfaceId && window->attachSurface(op.surfaceId))
                        app.renderFrame(/*forceFull=*/true);
                    break;
                }
                int32_t width = op.width;
                int32_t height = op.height;
                if ((width <= 0 || height <= 0) &&
                    !querySurfaceGeometry(op.surfaceId, width, height)) {
                    LOGW("attach(%{public}llu): 几何未就绪，下一轮重试",
                         static_cast<unsigned long long>(op.surfaceId));
                    std::lock_guard<std::mutex> lock(h.mutex);
                    h.ops.push_front(op);  // surface 就绪前这里是唯一的进展来源
                    break;
                }
                startUi(app, h, op.surfaceId, width, height);
                break;
        }
    }
}

void guiMain(Host& h) {
    h.app = std::make_unique<cui::Application>();
    cui::Application& app = *h.app;

    // 本线程就是 GUI 线程：beginEventLoop() 置 running_、定框线程 id、接好平台
    // 通道的 wakeUp 钩子。
    app.beginEventLoop();
    h.guiStarted.store(true);

    while (app.isRunning()) {
        drainOps(app, h);
        if (!h.window.load()) {
            // surface 还没就绪：吃 8ms 空转，等 ArkTS 的下一次投递。
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        // runIteration()：drain 事件 → 平台消息 → ticker → 任务（含渲染）。
        if (!app.runIteration()) {
            LOGI("loop exit: runIteration false");
            break;
        }
    }

    if (cui::OhosWindow* window = h.window.load())
        window->detachSurface(window->surfaceId());
    h.window.store(nullptr);

    app.endEventLoop();
    LOGI("gui thread exit");
}

bool ensureGuiThread(Host& h) {
    std::lock_guard<std::mutex> lock(h.startMutex);
    if (h.guiStarted.load())
        return true;
    h.guiThread = std::thread(guiMain, std::ref(h));
    return true;
}

// ── napi 参数读取 ───────────────────────────────────────────────────────────

bool readStringArg(napi_env env, napi_value value, std::string& out) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok)
        return false;
    out.assign(len + 1, '\0');
    if (napi_get_value_string_utf8(env, value, out.data(), len + 1, &len) != napi_ok)
        return false;
    out.resize(len);
    return true;
}

bool readIntArg(napi_env env, napi_value value, int32_t& out) {
    return napi_get_value_int32(env, value, &out) == napi_ok;
}

bool readDoubleArg(napi_env env, napi_value value, double& out) {
    return napi_get_value_double(env, value, &out) == napi_ok;
}

/// surfaceId 是 ArkTS 侧的字符串（XComponentController 回调给的就是 string）。
bool readSurfaceId(napi_env env, napi_value value, uint64_t& out) {
    std::string text;
    if (!readStringArg(env, value, text) || text.empty())
        return false;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text.c_str(), nullptr, 10);
    if (errno != 0)
        return false;
    out = static_cast<uint64_t>(parsed);
    return true;
}

/// 取 argc 个参数；不足返回 false（宁可不响应，也不读越界的 argv 槽）。
size_t cbInfo(napi_env env, napi_callback_info info, size_t expect, napi_value* argv) {
    size_t argc = expect;
    for (size_t i = 0; i < expect; ++i)
        argv[i] = nullptr;
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    return argc;
}

napi_value Undefined(napi_env env) {
    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

// ── 契约函数（与 entry/src/main/ets/cui_core/libcui.d.ts 一一对应）────────────────

napi_value StartSurface(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    if (cbInfo(env, info, 1, argv) < 1)
        return Undefined(env);
    uint64_t surfaceId = 0;
    if (!readSurfaceId(env, argv[0], surfaceId)) {
        LOGE("startSurface: surfaceId 解析失败");
        return Undefined(env);
    }
    ensureGuiThread(host());
    SurfaceOp op;
    op.kind = SurfaceOp::Kind::Attach;
    op.surfaceId = surfaceId;
    postOp(std::move(op));
    return Undefined(env);
}

napi_value ResizeSurface(napi_env env, napi_callback_info info) {
    napi_value argv[3];
    if (cbInfo(env, info, 3, argv) < 3)
        return Undefined(env);
    uint64_t surfaceId = 0;
    int32_t width = 0;
    int32_t height = 0;
    if (!readSurfaceId(env, argv[0], surfaceId) || !readIntArg(env, argv[1], width) ||
        !readIntArg(env, argv[2], height))
        return Undefined(env);
    SurfaceOp op;
    op.kind = SurfaceOp::Kind::Resize;
    op.surfaceId = surfaceId;
    op.width = width;
    op.height = height;
    postOp(std::move(op));
    return Undefined(env);
}

napi_value StopSurface(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    if (cbInfo(env, info, 1, argv) < 1)
        return Undefined(env);
    uint64_t surfaceId = 0;
    if (!readSurfaceId(env, argv[0], surfaceId))
        return Undefined(env);
    SurfaceOp op;
    op.kind = SurfaceOp::Kind::Detach;
    op.surfaceId = surfaceId;
    postOp(std::move(op));
    return Undefined(env);
}

napi_value SetDensity(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    if (cbInfo(env, info, 1, argv) < 1)
        return Undefined(env);
    double density = 0.0;
    if (!readDoubleArg(env, argv[0], density))
        return Undefined(env);
    SurfaceOp op;
    op.kind = SurfaceOp::Kind::Density;
    op.density = static_cast<float>(density);
    postOp(std::move(op));
    return Undefined(env);
}

/// 事件类调用的公共前置：没起 UI 就丢弃（surface 之前没有可派发的树）。
cui::OhosWindow* eventWindow() {
    return host().window.load();
}

napi_value DispatchTouch(napi_env env, napi_callback_info info) {
    napi_value argv[4];
    if (cbInfo(env, info, 4, argv) < 4)
        return Undefined(env);
    cui::OhosWindow* window = eventWindow();
    if (!window)
        return Undefined(env);
    int32_t action = 0;
    int32_t pointerId = 0;
    double x = 0;
    double y = 0;
    if (!readIntArg(env, argv[0], action) || !readIntArg(env, argv[1], pointerId) ||
        !readDoubleArg(env, argv[2], x) || !readDoubleArg(env, argv[3], y))
        return Undefined(env);
    window->dispatchTouch(action, pointerId, static_cast<float>(x), static_cast<float>(y));
    return Undefined(env);
}

napi_value DispatchMouse(napi_env env, napi_callback_info info) {
    napi_value argv[4];
    if (cbInfo(env, info, 4, argv) < 4)
        return Undefined(env);
    cui::OhosWindow* window = eventWindow();
    if (!window)
        return Undefined(env);
    int32_t action = 0;
    int32_t buttons = 0;
    double x = 0;
    double y = 0;
    if (!readIntArg(env, argv[0], action) || !readDoubleArg(env, argv[1], x) ||
        !readDoubleArg(env, argv[2], y) || !readIntArg(env, argv[3], buttons))
        return Undefined(env);
    window->dispatchMouse(action, static_cast<float>(x), static_cast<float>(y), buttons);
    return Undefined(env);
}

napi_value DispatchWheel(napi_env env, napi_callback_info info) {
    napi_value argv[5];
    if (cbInfo(env, info, 5, argv) < 5)
        return Undefined(env);
    cui::OhosWindow* window = eventWindow();
    if (!window)
        return Undefined(env);
    int32_t action = 0;
    double x = 0;
    double y = 0;
    double dx = 0;
    double dy = 0;
    if (!readIntArg(env, argv[0], action) || !readDoubleArg(env, argv[1], x) ||
        !readDoubleArg(env, argv[2], y) || !readDoubleArg(env, argv[3], dx) ||
        !readDoubleArg(env, argv[4], dy))
        return Undefined(env);
    window->dispatchWheel(action, static_cast<float>(x), static_cast<float>(y),
                          static_cast<float>(dx), static_cast<float>(dy));
    return Undefined(env);
}

napi_value DispatchKey(napi_env env, napi_callback_info info) {
    napi_value argv[3];
    if (cbInfo(env, info, 3, argv) < 3)
        return Undefined(env);
    cui::OhosWindow* window = eventWindow();
    if (!window)
        return Undefined(env);
    int32_t action = 0;
    int32_t keyCode = 0;
    std::string text;
    if (!readIntArg(env, argv[0], action) || !readIntArg(env, argv[1], keyCode) ||
        !readStringArg(env, argv[2], text))
        return Undefined(env);
    window->dispatchKey(action, keyCode, text);
    return Undefined(env);
}

const napi_property_descriptor kHostProps[] = {
    {"startSurface", nullptr, StartSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"resizeSurface", nullptr, ResizeSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"stopSurface", nullptr, StopSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"setDensity", nullptr, SetDensity, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"dispatchTouch", nullptr, DispatchTouch, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"dispatchMouse", nullptr, DispatchMouse, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"dispatchWheel", nullptr, DispatchWheel, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"dispatchKey", nullptr, DispatchKey, nullptr, nullptr, nullptr, napi_default, nullptr},
};

}  // namespace

// 引擎在 ohos_channel.cpp 的 napi Init 里以**弱定义**调用这条钩子；这里给出强定义，
// 把上面的 8 个函数追加进同一个 exports（模块名 "cui_core" 只能有一份）。
extern "C" void CuiOhosExtendHostExports(napi_env env, napi_value exports) {
    if (!env || !exports)
        return;
    if (napi_define_properties(env, exports,
                               sizeof(kHostProps) / sizeof(kHostProps[0]),
                               const_cast<napi_property_descriptor*>(kHostProps)) != napi_ok) {
        LOGE("napi_define_properties(host exports) failed");
        return;
    }
    LOGI("host exports installed (%{public}zu fns)", sizeof(kHostProps) / sizeof(kHostProps[0]));
}
