#pragma once
/// @file android_channel.h
/// @brief Android JNI side of the platform channel (optional for the host).
///
/// Hosts that use `dev.cui_core.framework.CuiActivity`
/// (src/platform/android/java/) install the bridge from Java; this header is
/// only needed when the host owns its own Activity and wants to wire the
/// bridge from native code.
///
/// Requirements:
///   - The Activity class declares the four static natives bound by
///     `dev.cui_core.framework.CuiActivity` (see the Java reference) or the
///     `nativeCuiMethodCall` / `nativeCuiNotify` / `nativeCuiEvent` symbols
///     resolve through name-based JNI lookup.
///   - The engine .so is loaded before install() runs.

#ifdef __ANDROID__

#include <jni.h>

namespace cui {
namespace services {
namespace android {

/// Bind the bridge to `activity` (a global ref is taken). Idempotent; returns
/// false when the activity does not expose
/// `String onCuiMethodCall(String channel, String method, String args)`.
bool install(JNIEnv* env, jobject activity);

/// Drop the global refs. Call before the VM goes away.
void uninstall(JNIEnv* env);

/// True while a host activity is installed.
bool installed();

}  // namespace android
}  // namespace services
}  // namespace cui_core

#endif  // __ANDROID__
