package dev.cui.framework;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.view.Window;

import java.lang.ref.WeakReference;
import java.util.HashMap;
import java.util.Map;

/**
 * Built-in Java side of the cui platform channels.
 *
 * <p>Channels (method names mirror the C++ services):
 * <ul>
 *   <li>{@code cui/clipboard}: {@code set}, {@code get}, {@code has}</li>
 *   <li>{@code cui/permission}: {@code request}, {@code check},
 *       {@code openSettings} — results are pushed back asynchronously on
 *       {@code cui/permission} method {@code result} with
 *       {@code "<code>\t<status>"}</li>
 *   <li>{@code cui/camera}: delegated to {@link CuiCameraService}</li>
 *   <li>{@code cui/bluetooth}: delegated to {@link CuiBluetoothService}</li>
 *   <li>{@code cui/platform}: {@code SystemChrome.*} (orientations, UI mode,
 *       overlay style, task description), {@code HapticFeedback.vibrate},
 *       {@code SystemSound.play} and {@code SystemNavigator.*} (see the
 *       method names in system_chrome.h / system_navigator.cpp)</li>
 * </ul>
 *
 * <p>All calls arrive on the Activity (UI) thread; never block here.
 */
final class CuiPlatformServices {
    private static final String CHANNEL_PERMISSION = "cui/permission";

    private static WeakReference<Activity> sActivity = new WeakReference<>(null);
    private static final Map<Integer, String> REQUEST_TO_CODE = new HashMap<>();
    private static int sNextRequestCode = 4100;

    /** Flutter's predictive-back switch; see {@code SystemNavigator.setFrameworkHandlesBack}. */
    private static boolean sFrameworkHandlesBack = true;

    /** Last UI-mode request, re-applied by {@code restoreSystemUIOverlays}. */
    private static String sLastUiMode = "SystemUiMode.edgeToEdge";
    private static String sLastOverlays = "SystemUiOverlay.top,SystemUiOverlay.bottom";

    private CuiPlatformServices() {
    }

    static String handle(Activity activity, String channel, String method, String args) {
        sActivity = new WeakReference<>(activity);
        if ("cui/platform".equals(channel)) {
            if ("ping".equals(method)) {
                return "ok";
            }
            return platform(activity, method, args);
        }
        if ("cui/clipboard".equals(channel)) {
            return clipboard(activity, method, args);
        }
        if (CHANNEL_PERMISSION.equals(channel)) {
            return permission(activity, method, args);
        }
        if ("cui/camera".equals(channel)) {
            return CuiCameraService.handle(activity, method, args);
        }
        if ("cui/bluetooth".equals(channel)) {
            return CuiBluetoothService.handle(activity, method, args);
        }
        return "";
    }

    // ── Clipboard ─────────────────────────────────────────────────────────

    private static String clipboard(Activity activity, String method, String args) {
        ClipboardManager manager =
                (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
        if (manager == null) {
            return "error:no clipboard service";
        }
        switch (method) {
            case "set":
                manager.setPrimaryClip(ClipData.newPlainText("cui", args));
                return "ok";
            case "get": {
                if (!manager.hasPrimaryClip()) {
                    return "";
                }
                ClipData clip = manager.getPrimaryClip();
                if (clip == null || clip.getItemCount() == 0) {
                    return "";
                }
                CharSequence text = clip.getItemAt(0).coerceToText(activity);
                return text == null ? "" : text.toString();
            }
            case "has":
                return manager.hasPrimaryClip() ? "1" : "0";
            default:
                return "";
        }
    }

    // ── System chrome / haptics / navigator ───────────────────────────────

    /**
     * {@code cui/platform} handler. Method names and argument encodings match
     * the C++ services line for line (see system_chrome.h).
     */
    private static String platform(Activity activity, String method, String args) {
        switch (method) {
            case "SystemChrome.setPreferredOrientations":
                return setPreferredOrientations(activity, args);
            case "SystemChrome.setApplicationSwitcherDescription":
                return setApplicationSwitcherDescription(activity, args);
            case "SystemChrome.setEnabledSystemUIMode":
                return setEnabledSystemUIMode(activity, args);
            case "SystemChrome.setEnabledSystemUIOverlays":
                return setEnabledSystemUIOverlays(activity, args);
            case "SystemChrome.setSystemUIOverlayStyle":
                return setSystemUIOverlayStyle(activity, args);
            case "SystemChrome.restoreSystemUIOverlays":
                // Flutter's engine re-applies the last configuration it sent.
                if ("SystemUiMode.manual".equals(sLastUiMode)) {
                    return setEnabledSystemUIOverlays(activity, sLastOverlays);
                }
                return setEnabledSystemUIMode(activity, sLastUiMode);
            case "HapticFeedback.vibrate":
                return hapticFeedback(activity, args);
            case "SystemSound.play":
                return systemSound(activity, args);
            case "SystemNavigator.pop":
                return pop(activity, args);
            case "SystemNavigator.setFrameworkHandlesBack":
                sFrameworkHandlesBack = !"0".equals(args);
                return "ok";
            default:
                return "";
        }
    }

    private static String setPreferredOrientations(Activity activity, String args) {
        if (args == null || args.isEmpty()) {
            activity.setRequestedOrientation(
                    ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
            return "ok";
        }
        String[] names = args.split(",");
        // Android's setRequestedOrientation accepts a single value; when the
        // app allows several, ask for the sensor-driven equivalent instead.
        String first = names[0];
        if (names.length > 1) {
            activity.setRequestedOrientation(
                    ActivityInfo.SCREEN_ORIENTATION_FULL_USER);
        } else if ("DeviceOrientation.portraitUp".equals(first)) {
            activity.setRequestedOrientation(
                    ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
        } else if ("DeviceOrientation.portraitDown".equals(first)) {
            activity.setRequestedOrientation(
                    ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT);
        } else if ("DeviceOrientation.landscapeLeft".equals(first)) {
            activity.setRequestedOrientation(
                    ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        } else if ("DeviceOrientation.landscapeRight".equals(first)) {
            activity.setRequestedOrientation(
                    ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE);
        }
        return "ok";
    }

    private static String setApplicationSwitcherDescription(Activity activity, String args) {
        String label = args;
        String colorHex = "";
        int tab = args == null ? -1 : args.indexOf('\t');
        if (tab >= 0) {
            label = args.substring(0, tab);
            colorHex = args.substring(tab + 1);
        }
        int color = colorHex.isEmpty() ? 0 : Color.parseColor(colorHex);
        activity.setTaskDescription(new ActivityManager.TaskDescription(label, null, color));
        return "ok";
    }

    private static String setEnabledSystemUIMode(Activity activity, String mode) {
        sLastUiMode = mode;
        Window window = activity.getWindow();
        View decor = window.getDecorView();
        if ("SystemUiMode.edgeToEdge".equals(mode)) {
            if (Build.VERSION.SDK_INT >= 30) {
                window.setDecorFitsSystemWindows(false);
            } else {
                decor.setSystemUiVisibility(
                        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            }
            return "ok";
        }
        int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
        if ("SystemUiMode.leanBack".equals(mode)) {
            flags |= View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION;
        } else if ("SystemUiMode.immersive".equals(mode)) {
            flags |= View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE;
        } else if ("SystemUiMode.immersiveSticky".equals(mode)) {
            flags |= View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        } else {
            return "";
        }
        decor.setSystemUiVisibility(flags);
        return "ok";
    }

    private static String setEnabledSystemUIOverlays(Activity activity, String overlays) {
        sLastUiMode = "SystemUiMode.manual";
        sLastOverlays = overlays == null ? "" : overlays;
        boolean top = overlays != null && overlays.contains("SystemUiOverlay.top");
        boolean bottom = overlays != null && overlays.contains("SystemUiOverlay.bottom");
        Window window = activity.getWindow();
        View decor = window.getDecorView();
        int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
        if (!top) {
            flags |= View.SYSTEM_UI_FLAG_FULLSCREEN;
        }
        if (!bottom) {
            flags |= View.SYSTEM_UI_FLAG_HIDE_NAVIGATION;
        }
        decor.setSystemUiVisibility(flags);
        return "ok";
    }

    /**
     * Parses the tab separated {@code key=value} list produced by
     * {@code SystemUiOverlayStyle::encode()}.
     */
    private static String setSystemUIOverlayStyle(Activity activity, String args) {
        Window window = activity.getWindow();
        View decor = window.getDecorView();
        int flags = decor.getSystemUiVisibility();
        Map<String, String> fields = new HashMap<>();
        if (args != null && !args.isEmpty()) {
            for (String pair : args.split("\t")) {
                int eq = pair.indexOf('=');
                if (eq > 0) {
                    fields.put(pair.substring(0, eq), pair.substring(eq + 1));
                }
            }
        }
        String navColor = fields.get("systemNavigationBarColor");
        if (navColor != null) {
            window.setNavigationBarColor(Color.parseColor(navColor));
        }
        String statusColor = fields.get("statusBarColor");
        if (statusColor != null) {
            window.setStatusBarColor(Color.parseColor(statusColor));
        }
        String statusIcons = fields.get("statusBarIconBrightness");
        if (statusIcons != null) {
            flags = setFlag(flags, View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR,
                    "Brightness.dark".equals(statusIcons));
        }
        String navIcons = fields.get("systemNavigationBarIconBrightness");
        if (navIcons != null) {
            flags = setFlag(flags, View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR,
                    "Brightness.dark".equals(navIcons));
        }
        decor.setSystemUiVisibility(flags);
        if (Build.VERSION.SDK_INT >= 29) {
            String statusContrast = fields.get("systemStatusBarContrastEnforced");
            if (statusContrast != null) {
                window.setStatusBarContrastEnforced("1".equals(statusContrast));
            }
            String navContrast = fields.get("systemNavigationBarContrastEnforced");
            if (navContrast != null) {
                window.setNavigationBarContrastEnforced("1".equals(navContrast));
            }
        }
        // statusBarBrightness only has meaning on iOS (inverted status bar).
        return "ok";
    }

    private static int setFlag(int flags, int flag, boolean on) {
        return on ? (flags | flag) : (flags & ~flag);
    }

    private static String hapticFeedback(Activity activity, String type) {
        View view = activity.getWindow().getDecorView();
        int constant;
        if (type == null || type.isEmpty() || "HapticFeedbackType.vibrate".equals(type)) {
            constant = HapticFeedbackConstants.LONG_PRESS;
        } else {
            switch (type) {
                case "HapticFeedbackType.lightImpact":
                    constant = HapticFeedbackConstants.CLOCK_TICK;
                    break;
                case "HapticFeedbackType.mediumImpact":
                    constant = HapticFeedbackConstants.CONTEXT_CLICK;
                    break;
                case "HapticFeedbackType.heavyImpact":
                    constant = HapticFeedbackConstants.KEYBOARD_TAP;
                    break;
                case "HapticFeedbackType.selectionClick":
                    constant = HapticFeedbackConstants.TEXT_HANDLE_MOVE;
                    break;
                case "HapticFeedbackType.successNotification":
                    constant = Build.VERSION.SDK_INT >= 30
                            ? HapticFeedbackConstants.CONFIRM
                            : HapticFeedbackConstants.CONTEXT_CLICK;
                    break;
                case "HapticFeedbackType.warningNotification":
                case "HapticFeedbackType.errorNotification":
                    constant = Build.VERSION.SDK_INT >= 30
                            ? HapticFeedbackConstants.REJECT
                            : HapticFeedbackConstants.LONG_PRESS;
                    break;
                default:
                    constant = HapticFeedbackConstants.LONG_PRESS;
                    break;
            }
        }
        view.performHapticFeedback(constant);
        return "ok";
    }

    private static String systemSound(Activity activity, String type) {
        if (!"SystemSoundType.click".equals(type)) {
            return "";
        }
        AudioManager audio =
                (AudioManager) activity.getSystemService(Context.AUDIO_SERVICE);
        if (audio == null) {
            return "error:no audio service";
        }
        audio.playSoundEffect(AudioManager.FX_KEY_CLICK, 1.0f);
        return "ok";
    }

    private static String pop(Activity activity, String animated) {
        if ("0".equals(animated) && Build.VERSION.SDK_INT >= 21) {
            activity.finishAndRemoveTask();
        } else {
            activity.finish();
        }
        return "ok";
    }

    /** True when the engine asked us to keep delivering back presses. */
    static boolean frameworkHandlesBack() {
        return sFrameworkHandlesBack;
    }

    // ── Permissions ───────────────────────────────────────────────────────

    private static String permission(Activity activity, String method, String args) {
        switch (method) {
            case "check": {
                String permission = permissionFor(args);
                return permission == null || granted(activity, permission)
                        ? "granted" : "denied";
            }
            case "request": {
                String permission = permissionFor(args);
                if (permission == null || granted(activity, permission)) {
                    return "granted";
                }
                if (Build.VERSION.SDK_INT < 23) {
                    return "granted";
                }
                int requestCode = sNextRequestCode++;
                REQUEST_TO_CODE.put(requestCode, args);
                activity.requestPermissions(new String[]{permission}, requestCode);
                return "pending";
            }
            case "openSettings": {
                Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
                intent.setData(Uri.fromParts("package", activity.getPackageName(), null));
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                activity.startActivity(intent);
                return "ok";
            }
            default:
                return "";
        }
    }

    static void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        String code = REQUEST_TO_CODE.remove(requestCode);
        if (code == null || permissions == null || grantResults == null
                || grantResults.length == 0) {
            return;
        }
        Activity activity = sActivity.get();
        boolean granted = grantResults[0] == PackageManager.PERMISSION_GRANTED;
        String status;
        if (granted) {
            status = "granted";
        } else if (activity != null && Build.VERSION.SDK_INT >= 23
                && activity.shouldShowRequestPermissionRationale(permissions[0])) {
            // 用户只是点了拒绝（还能再次弹窗）。
            status = "denied";
        } else {
            // 用户选了「不再询问」/ 系统策略禁止。
            status = "permanentlyDenied";
        }
        CuiActivity.notifyCui(CHANNEL_PERMISSION, "result", code + "\t" + status);
    }

    private static boolean granted(Activity activity, String permission) {
        if (Build.VERSION.SDK_INT < 23) {
            return true;
        }
        return activity.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Maps a service permission code to a platform permission string, or null
     * when the platform has nothing to ask (normal permission / older API).
     */
    private static String permissionFor(String code) {
        if (code == null) {
            return null;
        }
        switch (code) {
            case "camera":
                return Manifest.permission.CAMERA;
            case "microphone":
                return Manifest.permission.RECORD_AUDIO;
            case "location":
                return Manifest.permission.ACCESS_FINE_LOCATION;
            case "contacts":
                return Manifest.permission.READ_CONTACTS;
            case "photos":
                return Build.VERSION.SDK_INT >= 33
                        ? Manifest.permission.READ_MEDIA_IMAGES
                        : Manifest.permission.READ_EXTERNAL_STORAGE;
            case "storage":
                return Manifest.permission.READ_EXTERNAL_STORAGE;
            case "notifications":
                return Build.VERSION.SDK_INT >= 33
                        ? Manifest.permission.POST_NOTIFICATIONS : null;
            case "bluetooth":
                return Build.VERSION.SDK_INT >= 31
                        ? Manifest.permission.BLUETOOTH_CONNECT : null;
            case "bluetoothScan":
                return Build.VERSION.SDK_INT >= 31
                        ? Manifest.permission.BLUETOOTH_SCAN : null;
            default:
                return null;
        }
    }
}
