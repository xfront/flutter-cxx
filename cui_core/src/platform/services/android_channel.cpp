/// @file android_channel.cpp
/// @brief JNI transport for the platform channel (see android_bridge.h).
///
/// Java contract (dev.cui_core.framework.CuiActivity):
///   static native boolean nativeInstallCuiPlatform(Activity activity);
///   static native String  nativeCuiMethodCall(String channel, String method, String args);
///   static native void    nativeCuiNotify(String channel, String method, String args);
///   static native void    nativeCuiEvent(String channel, String event);
///   public String onCuiMethodCall(String channel, String method, String args);
///
/// The symbols below resolve through name-based JNI lookup, so the host only
/// has to load the engine library before touching the bridge.

#include "android_bridge.h"

#ifdef __ANDROID__

#include <android/log.h>
#include <jni.h>

#include <mutex>
#include <string>

#include "cui/platform/services/android_channel.h"
#include "cui/platform/services/event_channel.h"
#include "cui/platform/services/method_channel.h"
#include "channel_registry.h"

#define CUI_LOG_TAG "cui_core-channel"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, CUI_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, CUI_LOG_TAG, __VA_ARGS__)

namespace cui {
namespace services {
namespace android {

namespace {

std::mutex g_mutex;
JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;     // global ref
jclass g_activityClass = nullptr; // global ref
jmethodID g_onMethodCall = nullptr;

std::string jstringToUtf8(JNIEnv* env, jstring value) {
    if (!value)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars)
        return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

jstring utf8ToJstring(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

/// Attach the calling thread for the duration of one native → Java call.
class ScopedEnv {
public:
    ScopedEnv() {
        std::lock_guard<std::mutex> lock(g_mutex);
        vm_ = g_vm;
        if (!vm_)
            return;
        jint rc = vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
        if (rc == JNI_EDETACHED) {
            if (vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK)
                attached_ = true;
            else
                env_ = nullptr;
        } else if (rc != JNI_OK) {
            env_ = nullptr;
        }
    }

    ~ScopedEnv() {
        if (attached_ && vm_)
            vm_->DetachCurrentThread();
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    JNIEnv* env() const { return env_; }

private:
    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

void clearPendingException(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

}  // namespace

bool install(JNIEnv* env, jobject activity) {
    if (!env || !activity)
        return false;

    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || !vm) {
        LOGE("install: GetJavaVM failed");
        return false;
    }

    jclass cls = env->GetObjectClass(activity);
    if (!cls)
        return false;
    jmethodID onMethodCall = env->GetMethodID(
        cls, "onCuiMethodCall",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!onMethodCall) {
        clearPendingException(env);
        LOGE("install: activity lacks String onCuiMethodCall(String,String,String)");
        env->DeleteLocalRef(cls);
        return false;
    }

    jobject activityRef = env->NewGlobalRef(activity);
    jclass classRef = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);
    if (!activityRef || !classRef) {
        if (activityRef)
            env->DeleteGlobalRef(activityRef);
        if (classRef)
            env->DeleteGlobalRef(classRef);
        return false;
    }

    jobject oldActivity = nullptr;
    jclass oldClass = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        oldActivity = g_activity;
        oldClass = g_activityClass;
        g_vm = vm;
        g_activity = activityRef;
        g_activityClass = classRef;
        g_onMethodCall = onMethodCall;
    }
    if (oldActivity)
        env->DeleteGlobalRef(oldActivity);
    if (oldClass)
        env->DeleteGlobalRef(oldClass);

    LOGI("platform channel installed");
    return true;
}

void uninstall(JNIEnv* env) {
    if (!env)
        return;
    jobject activity = nullptr;
    jclass cls = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        activity = g_activity;
        cls = g_activityClass;
        g_activity = nullptr;
        g_activityClass = nullptr;
        g_onMethodCall = nullptr;
        g_vm = nullptr;
    }
    if (activity)
        env->DeleteGlobalRef(activity);
    if (cls)
        env->DeleteGlobalRef(cls);
}

bool installed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_activity != nullptr;
}

bool available() {
    return installed();
}

std::optional<std::string> invoke(const std::string& channel,
                                  const std::string& method,
                                  const std::string& arguments) {
    if (!available())
        return std::nullopt;

    ScopedEnv scoped;
    JNIEnv* env = scoped.env();
    if (!env)
        return std::nullopt;

    jobject activity = nullptr;
    jmethodID mid = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        activity = g_activity;
        mid = g_onMethodCall;
    }
    if (!activity || !mid)
        return std::nullopt;

    jstring jchannel = utf8ToJstring(env, channel);
    jstring jmethod = utf8ToJstring(env, method);
    jstring jargs = utf8ToJstring(env, arguments);
    auto reply = env->CallObjectMethod(activity, mid, jchannel, jmethod, jargs);
    if (env->ExceptionCheck()) {
        clearPendingException(env);
        env->DeleteLocalRef(jchannel);
        env->DeleteLocalRef(jmethod);
        env->DeleteLocalRef(jargs);
        return std::nullopt;
    }
    std::optional<std::string> out;
    if (reply) {
        out = jstringToUtf8(env, static_cast<jstring>(reply));
        env->DeleteLocalRef(reply);
    }
    env->DeleteLocalRef(jchannel);
    env->DeleteLocalRef(jmethod);
    env->DeleteLocalRef(jargs);
    return out;
}

}  // namespace android
}  // namespace services
}  // namespace cui_core

// ── Exported JNI entry points ───────────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_cui_framework_CuiActivity_nativeInstallCuiPlatform(JNIEnv* env, jclass,
                                                            jobject activity) {
    return cui::services::android::install(env, activity) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_cui_framework_CuiActivity_nativeUninstallCuiPlatform(JNIEnv* env, jclass) {
    cui::services::android::uninstall(env);
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_cui_framework_CuiActivity_nativeCuiMethodCall(JNIEnv* env, jclass,
                                                       jstring channel,
                                                       jstring method,
                                                       jstring args) {
    using namespace cui::services;
    const std::string channelStr = android::jstringToUtf8(env, channel);
    const std::string methodStr = android::jstringToUtf8(env, method);
    const std::string argsStr = android::jstringToUtf8(env, args);
    const std::string reply =
        detail::dispatchHandlerNow(channelStr, methodStr, argsStr);
    return env->NewStringUTF(reply.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_dev_cui_framework_CuiActivity_nativeCuiNotify(JNIEnv* env, jclass,
                                                   jstring channel,
                                                   jstring method,
                                                   jstring args) {
    using namespace cui::services;
    pushInboundCall(android::jstringToUtf8(env, channel),
                    android::jstringToUtf8(env, method),
                    android::jstringToUtf8(env, args));
}

extern "C" JNIEXPORT void JNICALL
Java_dev_cui_framework_CuiActivity_nativeCuiEvent(JNIEnv* env, jclass,
                                                  jstring channel,
                                                  jstring event) {
    using namespace cui::services;
    pushInboundEvent(android::jstringToUtf8(env, channel),
                     android::jstringToUtf8(env, event));
}

#else  // !__ANDROID__

namespace cui {
namespace services {
namespace android {

bool available() {
    return false;
}

std::optional<std::string> invoke(const std::string&, const std::string&,
                                  const std::string&) {
    return std::nullopt;
}

}  // namespace android
}  // namespace services
}  // namespace cui_core

#endif  // __ANDROID__
