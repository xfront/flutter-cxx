/// @file ohos_channel.cpp
/// @brief NAPI transport for the platform channel on HarmonyOS (see ohos_bridge.h).
///
/// ArkTS contract (see
/// src/platform/ohos/entry/src/main/ets/cui_core/CuiBridge.ets):
///   import cui_core from 'libcui.so'
///   cui_core.install((channel, method, args) => string)   // one-time, JS thread
///   cui_core.methodCall(channel, method, args): string    // ArkTS → C++ (sync)
///   cui_core.notify(channel, method, args)                // ArkTS → C++ (async)
///   cui_core.event(channel, event)                        // EventChannel push
///
/// Unlike JNI, NAPI is thread-bound: a `napi_env` may only be used on the JS
/// thread that created it. `invoke()` therefore takes two paths:
///   - called on the JS thread (the normal case: the app loop runs there) →
///     call the registered dispatcher directly;
///   - called from another thread → hand the call to a threadsafe function
///     and block until the JS thread answers (5 s safety timeout, then the
///     call degrades to "no host").
/// Async host work (permission dialogs, camera capture, BLE scan) never
/// blocks this path: the dispatcher replies immediately (often "pending") and
/// the result arrives later through CuiActivity-style `notify`/`event` calls.

#include "ohos_bridge.h"

#if defined(__OHOS__) && __has_include(<napi/native_api.h>)
#define CUI_HAVE_NAPI 1
#endif

#ifdef CUI_HAVE_NAPI

#include <napi/native_api.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "channel_registry.h"
#include "cui/platform/services/event_channel.h"
#include "cui/platform/services/method_channel.h"

namespace cui {
namespace services {
namespace ohos {

namespace {

std::mutex g_mutex;
napi_env g_env = nullptr;
napi_ref g_dispatch = nullptr;  ///< JS (channel, method, args) => string
napi_threadsafe_function g_tsfn = nullptr;
std::thread::id g_jsThread;
bool g_installed = false;

constexpr auto kCrossThreadTimeout = std::chrono::seconds(5);

bool readString(napi_env env, napi_value value, std::string& out) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok)
        return false;
    out.resize(len);
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, value, out.data(), len + 1, &copied) != napi_ok)
        return false;
    out.resize(copied);
    return true;
}

napi_value makeString(napi_env env, const std::string& text) {
    napi_value value = nullptr;
    napi_create_string_utf8(env, text.c_str(), text.size(), &value);
    return value;
}

void clearException(napi_env env) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        napi_value ignored = nullptr;
        napi_get_and_clear_last_exception(env, &ignored);
    }
}

std::string callDispatcher(napi_env env, napi_value fn, const std::string& channel,
                           const std::string& method, const std::string& arguments) {
    napi_value global = nullptr;
    if (!fn || napi_get_global(env, &global) != napi_ok)
        return {};
    napi_value argv[3] = {makeString(env, channel), makeString(env, method),
                          makeString(env, arguments)};
    napi_value result = nullptr;
    if (napi_call_function(env, global, fn, 3, argv, &result) != napi_ok) {
        clearException(env);
        return {};
    }
    std::string out;
    if (result)
        readString(env, result, out);
    return out;
}

/// Cross-thread call payload. Shared with the JS-thread callback; a timed-out
/// caller leaves its shared_ptr behind so a late callback still cleans up.
struct InvokeCtx {
    std::string channel;
    std::string method;
    std::string arguments;
    std::string result;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

void callJs(napi_env env, napi_value js_cb, void* /*hint*/, void* data) {
    auto* holder = static_cast<std::shared_ptr<InvokeCtx>*>(data);
    if (!holder)
        return;
    std::shared_ptr<InvokeCtx> ctx = *holder;
    delete holder;

    if (env && js_cb && ctx)
        ctx->result = callDispatcher(env, js_cb, ctx->channel, ctx->method, ctx->arguments);
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->done = true;
    }
    ctx->cv.notify_all();
}

napi_value Install(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    napi_valuetype type = napi_undefined;
    if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok || type != napi_function) {
        napi_throw_type_error(env, nullptr, "install(dispatch: (channel, method, args) => string)");
        return nullptr;
    }

    napi_ref dispatch = nullptr;
    if (napi_create_reference(env, argv[0], 1, &dispatch) != napi_ok)
        return nullptr;

    napi_value name = nullptr;
    napi_create_string_utf8(env, "cuiHostInvoke", NAPI_AUTO_LENGTH, &name);
    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[0], nullptr, name, 0, 1, nullptr,
                                        nullptr, nullptr, callJs, &tsfn) != napi_ok) {
        napi_delete_reference(env, dispatch);
        return nullptr;
    }

    napi_ref oldDispatch = nullptr;
    napi_threadsafe_function oldTsfn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        oldDispatch = g_dispatch;
        oldTsfn = g_tsfn;
        g_env = env;
        g_dispatch = dispatch;
        g_tsfn = tsfn;
        g_jsThread = std::this_thread::get_id();
        g_installed = true;
    }
    if (oldDispatch)
        napi_delete_reference(env, oldDispatch);
    if (oldTsfn)
        napi_release_threadsafe_function(oldTsfn, napi_tsfn_release);

    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value Uninstall(napi_env env, napi_callback_info) {
    napi_ref dispatch = nullptr;
    napi_threadsafe_function tsfn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        dispatch = g_dispatch;
        tsfn = g_tsfn;
        g_dispatch = nullptr;
        g_tsfn = nullptr;
        g_env = nullptr;
        g_installed = false;
    }
    if (dispatch)
        napi_delete_reference(env, dispatch);
    if (tsfn)
        napi_release_threadsafe_function(tsfn, napi_tsfn_release);
    napi_value out = nullptr;
    napi_get_boolean(env, false, &out);
    return out;
}

/// ArkTS → C++ synchronous call (permission result, lifecycle hooks). Mirrors
/// Android's nativeCuiMethodCall: handlers registered with
/// setMethodCallHandler() reply immediately.
napi_value MethodCall(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string channel;
    std::string method;
    std::string arguments;
    if (argc > 0)
        readString(env, argv[0], channel);
    if (argc > 1)
        readString(env, argv[1], method);
    if (argc > 2)
        readString(env, argv[2], arguments);

    return makeString(env, detail::dispatchHandlerNow(channel, method, arguments));
}

napi_value Notify(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string channel;
    std::string method;
    std::string arguments;
    if (argc > 0)
        readString(env, argv[0], channel);
    if (argc > 1)
        readString(env, argv[1], method);
    if (argc > 2)
        readString(env, argv[2], arguments);
    pushInboundCall(std::move(channel), std::move(method), std::move(arguments));
    return nullptr;
}

napi_value EmitEvent(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string channel;
    std::string event;
    if (argc > 0)
        readString(env, argv[0], channel);
    if (argc > 1)
        readString(env, argv[1], event);
    pushInboundEvent(std::move(channel), std::move(event));
    return nullptr;
}

/// 宿主扩展点：ArkTS 侧 `import cui_core from 'libcui.so'` 只能拿到**一个**名为 "cui_core"
/// 的模块，而模块注册在引擎这一侧（见下面 g_module），所以 surface/输入那批 napi
/// 函数必须由宿主追加进同一份 exports。
///
/// 这里是**弱定义**（空实现），platform/ohos/cpp/main.cpp 给强定义覆盖它。用弱符号
/// 而不是硬 extern 引用：story / demo_ide 这类没有宿主的 arm64-ohos 目标也
/// 链得动同一套引擎库（硬引用会在链接期就报 undefined symbol）。
extern "C" __attribute__((weak)) void CuiOhosExtendHostExports(napi_env env, napi_value exports) {
    (void)env;
    (void)exports;
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"install", nullptr, Install, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uninstall", nullptr, Uninstall, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"methodCall", nullptr, MethodCall, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"notify", nullptr, Notify, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"emitEvent", nullptr, EmitEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    CuiOhosExtendHostExports(env, exports);
    return exports;
}

napi_module g_module = {
    // node_api.h 给的是宏 NAPI_MODULE_VERSION（nm_version 是 int），没有
    // napi_module_version_1 这个标识符 —— 自写 stub 头时代抄错的字段名只有在真
    // NDK 上才暴露。
    NAPI_MODULE_VERSION,
    0,
    __FILE__,
    Init,
    "cui_core",
    nullptr,
    {},
};

}  // namespace

bool available() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_installed;
}

std::optional<std::string> invoke(const std::string& channel,
                                  const std::string& method,
                                  const std::string& arguments) {
    napi_env env = nullptr;
    napi_ref dispatchRef = nullptr;
    napi_threadsafe_function tsfn = nullptr;
    bool onJsThread = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed)
            return std::nullopt;
        env = g_env;
        dispatchRef = g_dispatch;
        tsfn = g_tsfn;
        onJsThread = std::this_thread::get_id() == g_jsThread;
    }

    if (onJsThread) {
        napi_value fn = nullptr;
        if (napi_get_reference_value(env, dispatchRef, &fn) != napi_ok || !fn)
            return std::nullopt;
        return callDispatcher(env, fn, channel, method, arguments);
    }

    if (!tsfn)
        return std::nullopt;
    auto ctx = std::make_shared<InvokeCtx>();
    ctx->channel = channel;
    ctx->method = method;
    ctx->arguments = arguments;
    auto* holder = new std::shared_ptr<InvokeCtx>(ctx);
    if (napi_call_threadsafe_function(tsfn, holder, napi_tsfn_blocking) != napi_ok) {
        delete holder;
        return std::nullopt;
    }
    std::unique_lock<std::mutex> lock(ctx->mutex);
    if (!ctx->cv.wait_for(lock, kCrossThreadTimeout, [&] { return ctx->done; }))
        return std::nullopt;
    return ctx->result;
}

}  // namespace ohos
}  // namespace services
}  // namespace cui_core

// ── NAPI module registration ────────────────────────────────────────────────
// The engine library must be built as libcui.so so ArkTS can `import cui_core from
// 'libcui.so'`; the registered module name must match the library name.

extern "C" __attribute__((constructor)) void CuiRegisterPlatformModule() {
    napi_module_register(&cui::services::ohos::g_module);
}

#else  // !OHOS / no NAPI headers

namespace cui {
namespace services {
namespace ohos {

bool available() {
    return false;
}

std::optional<std::string> invoke(const std::string&, const std::string&,
                                  const std::string&) {
    return std::nullopt;
}

}  // namespace ohos
}  // namespace services
}  // namespace cui_core

#endif  // CUI_HAVE_NAPI
