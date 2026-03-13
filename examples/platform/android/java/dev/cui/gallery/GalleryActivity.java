package dev.cui.gallery;

/**
 * gallery 的 Activity：直接继承 framework 里的 {@code CuiActivity}
 * （NativeActivity 子类），平台通道、生命周期、返回键都由它接线。
 *
 * <p>native 入口在 {@code flutter/src/platform/android/cpp/main.cpp} 的
 * {@code android_main()}，通过 {@code android.app.lib_name = cui_gallery}
 * 由 NativeActivity 加载。
 */
public class GalleryActivity extends dev.cui.framework.CuiActivity {
}
