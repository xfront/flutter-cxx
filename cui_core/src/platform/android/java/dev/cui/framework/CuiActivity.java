package dev.cui.framework;

import android.app.Activity;
import android.app.NativeActivity;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;

/**
 * FlutterActivity equivalent for cui: a {@link NativeActivity} that installs
 * the platform channel between the engine (C++) and Java.
 *
 * <p>Host setup:
 * <pre>
 *   &lt;activity android:name=".MyActivity"
 *             android:hasCode="true" ...&gt;
 *     &lt;meta-data android:name="android.app.lib_name" android:value="myapp"/&gt;
 *   &lt;/activity&gt;
 * </pre>
 * with {@code MyActivity extends dev.cui.framework.CuiActivity}. The engine
 * library (libmyapp.so) must export the four JNI symbols in
 * {@code src/platform/services/android_channel.cpp}; they are found through
 * name-based lookup once NativeActivity loads the library.
 *
 * <p>Runtime permissions used by the built-in services:
 * {@code CAMERA}, {@code RECORD_AUDIO}, {@code ACCESS_FINE_LOCATION},
 * {@code BLUETOOTH_SCAN}, {@code BLUETOOTH_CONNECT},
 * {@code POST_NOTIFICATIONS}, {@code READ_MEDIA_IMAGES},
 * {@code READ_CONTACTS}. Declare the ones the app needs in the manifest, then
 * call {@code services::requestPermission(...)} from C++.
 */
public class CuiActivity extends NativeActivity {
    private static final String TAG = "cui-platform";
    private static final String CHANNEL_LIFECYCLE = "cui/lifecycle";
    private static final String CHANNEL_NAVIGATOR = "cui/system_navigator";
    /** Same key NativeActivity reads to pick its engine library. */
    private static final String LIB_NAME_META = "android.app.lib_name";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Load the engine here too: NativeActivity calls System.loadLibrary from
        // its own class, so the handle ends up registered against the *bootstrap*
        // class loader, while ART resolves a native method only from libraries
        // loaded by the declaring class's loader (dev.cui.* live in the app's
        // PathClassLoader). Without this the symbols exist in .dynsym but every
        // call below fails with "No implementation found".
        loadEngineLibrary();
        // super.onCreate() then loads the same lib (dlopen returns the cached
        // handle) and starts native activity / android_main.
        super.onCreate(savedInstanceState);
        try {
            boolean ok = nativeInstallCuiPlatform(this);
            Log.i(TAG, "platform channel installed=" + ok);
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "engine library does not export the cui platform bridge: " + e);
        }
    }

    /** {@code <meta-data android:name="android.app.lib_name">} of this activity. */
    private String engineLibName() {
        try {
            ActivityInfo info = getPackageManager().getActivityInfo(getComponentName(),
                    PackageManager.GET_META_DATA);
            if (info != null && info.metaData != null) {
                String name = info.metaData.getString(LIB_NAME_META);
                if (name != null && !name.isEmpty()) {
                    return name;
                }
            }
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            Log.w(TAG, "cannot read " + LIB_NAME_META + ": " + e);
        }
        return "main"; // NativeActivity's own default
    }

    private void loadEngineLibrary() {
        final String name = engineLibName();
        try {
            System.loadLibrary(name);
            Log.i(TAG, "loaded lib" + name + ".so in app class loader scope");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "System.loadLibrary(\"" + name + "\") failed: " + e);
        }
    }

    // Lifecycle: pushed on the queued channel so AppLifecycleListener sees the
    // same states as Flutter (inactive while paused, hidden while stopped).
    @Override
    protected void onResume() {
        super.onResume();
        notifyLifecycle("resumed");
    }

    @Override
    protected void onPause() {
        super.onPause();
        notifyLifecycle("inactive");
    }

    @Override
    protected void onStop() {
        super.onStop();
        notifyLifecycle("hidden");
    }

    @Override
    protected void onDestroy() {
        notifyLifecycle("detached");
        try {
            nativeUninstallCuiPlatform();
        } catch (UnsatisfiedLinkError ignored) {
            // Channel never installed.
        }
        super.onDestroy();
    }

    private static void notifyLifecycle(String state) {
        notifyCui(CHANNEL_LIFECYCLE, "AppLifecycleState.changed",
                "AppLifecycleState." + state);
    }

    /**
     * Back goes through the engine first (see services::SystemNavigator): the
     * queued {@code back} call runs the registered handlers, and pops the app
     * itself when nobody consumed it. Disabling predictive back lets us keep
     * the activity alive until that round-trip finishes.
     */
    @Override
    public void onBackPressed() {
        if (!CuiPlatformServices.frameworkHandlesBack()) {
            super.onBackPressed();
            return;
        }
        notifyCui(CHANNEL_NAVIGATOR, "back", "");
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        CuiPlatformServices.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }

    /**
     * C++ → Java dispatch (Flutter: MethodChannel handler). Hosts may override
     * to add their own channels; the default routes to
     * {@link CuiPlatformServices} (clipboard, permissions, camera, Bluetooth).
     */
    public String onCuiMethodCall(String channel, String method, String args) {
        return CuiPlatformServices.handle(this, channel, method, args);
    }

    /** Java → C++ synchronous call (Flutter: invokeMethod). */
    public static String invokeCuiMethod(String channel, String method, String args) {
        try {
            return nativeCuiMethodCall(channel, method, args);
        } catch (UnsatisfiedLinkError e) {
            return "";
        }
    }

    /** Java → C++ queued call: delivered on the UI thread by the frame pump. */
    public static void notifyCui(String channel, String method, String args) {
        try {
            nativeCuiNotify(channel, method, args);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    /** Java → C++ event-channel push (Flutter: EventSink.add). */
    public static void emitCuiEvent(String channel, String event) {
        try {
            nativeCuiEvent(channel, event);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private static native boolean nativeInstallCuiPlatform(Activity activity);
    private static native void nativeUninstallCuiPlatform();
    private static native String nativeCuiMethodCall(String channel, String method, String args);
    private static native void nativeCuiNotify(String channel, String method, String args);
    private static native void nativeCuiEvent(String channel, String event);
}
