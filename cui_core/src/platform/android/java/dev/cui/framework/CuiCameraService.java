package dev.cui.framework;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Size;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;

/**
 * Camera2 backend for {@code cui/camera}.
 *
 * <p>Supports enumeration, open/close, flash mode and still capture to a JPEG
 * path. Preview frames need a Surface from the native window; attach one in
 * {@link #attachPreviewSurface} if the host provides it (the C++ service's
 * {@code startPreview()} returns false until then).
 *
 * <p>Events pushed on {@code cui/camera}:
 * {@code state\t<opening|open|closed>}, {@code saved\t<path>},
 * {@code error\t<message>}, {@code frame\t<...>}.
 *
 * <p>Requires {@code android.permission.CAMERA} and a prior
 * {@code services::requestPermission(Permission::Camera, ...)}.
 */
final class CuiCameraService {
    private static final String TAG = "cui-camera";
    private static final String CHANNEL = "cui/camera";

    private static CameraDevice sDevice;
    private static CameraCaptureSession sSession;
    private static ImageReader sJpegReader;
    private static android.view.Surface sPreviewSurface;
    private static HandlerThread sThread;
    private static Handler sHandler;
    private static String sState = "closed";
    private static String sPendingPath;
    private static String sFlashMode = "off";
    private static int sSensorOrientation = 0;
    private static boolean sFrontFacing = false;

    private CuiCameraService() {
    }

    /**
     * Host hook: hand the native window's Surface (ANativeWindow → Surface)
     * to the camera so {@code startPreview()} can create a preview session.
     */
    static void attachPreviewSurface(android.view.Surface surface) {
        sPreviewSurface = surface;
    }

    static String handle(Activity activity, String method, String args) {
        switch (method) {
            case "listen":
            case "cancel":
                return "ok";
            case "availableCameras":
                return listCameras(activity);
            case "open":
                return open(activity, args);
            case "close":
                close();
                return "ok";
            case "setFlashMode":
                sFlashMode = args == null ? "off" : args;
                return "ok";
            case "takePicture":
                return takePicture(activity, args);
            case "startPreview":
                return sPreviewSurface == null ? "error:preview surface not attached" : "ok";
            case "stopPreview":
                return "ok";
            case "state":
                return sState;
            default:
                return "";
        }
    }

    private static Handler handler() {
        if (sHandler == null) {
            sThread = new HandlerThread("cui-camera");
            sThread.start();
            sHandler = new Handler(sThread.getLooper());
        }
        return sHandler;
    }

    private static void emit(String event) {
        CuiActivity.emitCuiEvent(CHANNEL, event);
    }

    private static boolean hasCameraPermission(Activity activity) {
        return Build.VERSION.SDK_INT < 23
                || activity.checkSelfPermission(Manifest.permission.CAMERA)
                        == PackageManager.PERMISSION_GRANTED;
    }

    private static String listCameras(Activity activity) {
        CameraManager manager =
                (CameraManager) activity.getSystemService(Context.CAMERA_SERVICE);
        if (manager == null) {
            return "";
        }
        StringBuilder out = new StringBuilder();
        try {
            for (String id : manager.getCameraIdList()) {
                CameraCharacteristics characteristics =
                        manager.getCameraCharacteristics(id);
                Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
                Integer orientation =
                        characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION);
                int facingCode = 0;
                if (facing != null) {
                    if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                        facingCode = 1;
                    } else if (facing == CameraCharacteristics.LENS_FACING_FRONT) {
                        facingCode = 2;
                    } else {
                        facingCode = 3;
                    }
                }
                out.append(id).append('\t')
                        .append("Camera ").append(id).append('\t')
                        .append(facingCode).append('\t')
                        .append(orientation == null ? 0 : orientation).append('\n');
            }
        } catch (CameraAccessException | SecurityException e) {
            Log.w(TAG, "listCameras failed: " + e);
        }
        return out.toString();
    }

    private static String open(Activity activity, String id) {
        if (!hasCameraPermission(activity)) {
            return "error:camera permission denied";
        }
        CameraManager manager =
                (CameraManager) activity.getSystemService(Context.CAMERA_SERVICE);
        if (manager == null) {
            return "error:no camera service";
        }
        if (id == null || id.isEmpty()) {
            id = "0";
        }
        try {
            closeInternal(false);
            CameraCharacteristics characteristics =
                    manager.getCameraCharacteristics(id);
            Integer sensor =
                    characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION);
            sSensorOrientation = sensor == null ? 0 : sensor;
            Integer facing = characteristics.get(CameraCharacteristics.LENS_FACING);
            sFrontFacing = facing != null
                    && facing == CameraCharacteristics.LENS_FACING_FRONT;
            sState = "opening";
            emit("state\topening");

            final String cameraId = id;
            manager.openCamera(cameraId, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera) {
                    sDevice = camera;
                    try {
                        createSession(manager, cameraId, camera);
                    } catch (CameraAccessException | SecurityException e) {
                        sState = "error";
                        emit("error\t" + e.getMessage());
                    }
                }

                @Override
                public void onDisconnected(CameraDevice camera) {
                    camera.close();
                    sDevice = null;
                    sState = "closed";
                    emit("state\tclosed");
                }

                @Override
                public void onError(CameraDevice camera, int error) {
                    camera.close();
                    sDevice = null;
                    sState = "error";
                    emit("error\tcamera error " + error);
                }
            }, handler());
            return "ok";
        } catch (CameraAccessException | SecurityException | IllegalArgumentException e) {
            sState = "error";
            return "error:" + e.getMessage();
        }
    }

    private static void createSession(CameraManager manager, String cameraId,
                                      CameraDevice camera) throws CameraAccessException {
        CameraCharacteristics characteristics = manager.getCameraCharacteristics(cameraId);
        StreamConfigurationMap map =
                characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size size = new Size(1280, 720);
        if (map != null) {
            Size[] sizes = map.getOutputSizes(ImageFormat.JPEG);
            if (sizes != null && sizes.length > 0) {
                size = sizes[0];
            }
        }
        sJpegReader = ImageReader.newInstance(size.getWidth(), size.getHeight(),
                ImageFormat.JPEG, 2);
        sJpegReader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
            @Override
            public void onImageAvailable(ImageReader reader) {
                saveJpeg(reader);
            }
        }, handler());

        java.util.List<android.view.Surface> surfaces = new java.util.ArrayList<>();
        surfaces.add(sJpegReader.getSurface());
        if (sPreviewSurface != null) {
            surfaces.add(sPreviewSurface);
        }

        camera.createCaptureSession(surfaces, new CameraCaptureSession.StateCallback() {
            @Override
            public void onConfigured(CameraCaptureSession session) {
                sSession = session;
                sState = "open";
                emit("state\topen");
            }

            @Override
            public void onConfigureFailed(CameraCaptureSession session) {
                sState = "error";
                emit("error\tcapture session configuration failed");
            }
        }, handler());
    }

    private static String takePicture(Activity activity, String path) {
        if (sDevice == null || sSession == null || sJpegReader == null) {
            return "error:camera not open";
        }
        if (path == null || path.isEmpty()) {
            return "error:no output path";
        }
        sPendingPath = path;
        try {
            CaptureRequest.Builder builder =
                    sDevice.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
            builder.addTarget(sJpegReader.getSurface());
            builder.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO);
            applyFlash(builder);
            builder.set(CaptureRequest.JPEG_ORIENTATION, jpegOrientation(activity));
            sSession.capture(builder.build(), null, handler());
            return "ok";
        } catch (CameraAccessException | IllegalArgumentException e) {
            sPendingPath = null;
            return "error:" + e.getMessage();
        }
    }

    private static void applyFlash(CaptureRequest.Builder builder) {
        switch (sFlashMode) {
            case "auto":
                builder.set(CaptureRequest.CONTROL_AE_MODE,
                        CameraMetadata.CONTROL_AE_MODE_ON_AUTO_FLASH);
                break;
            case "on":
                builder.set(CaptureRequest.CONTROL_AE_MODE,
                        CameraMetadata.CONTROL_AE_MODE_ON_ALWAYS_FLASH);
                break;
            case "torch":
                builder.set(CaptureRequest.CONTROL_AE_MODE,
                        CameraMetadata.CONTROL_AE_MODE_ON);
                builder.set(CaptureRequest.FLASH_MODE, CameraMetadata.FLASH_MODE_TORCH);
                break;
            case "off":
            default:
                builder.set(CaptureRequest.CONTROL_AE_MODE,
                        CameraMetadata.CONTROL_AE_MODE_ON);
                builder.set(CaptureRequest.FLASH_MODE, CameraMetadata.FLASH_MODE_OFF);
                break;
        }
    }

    private static int jpegOrientation(Activity activity) {
        int deviceDegrees = 0;
        WindowManager wm = (WindowManager) activity.getSystemService(Context.WINDOW_SERVICE);
        if (wm != null && wm.getDefaultDisplay() != null) {
            deviceDegrees = wm.getDefaultDisplay().getRotation() * 90;
        }
        if (sFrontFacing) {
            return (sSensorOrientation + deviceDegrees) % 360;
        }
        return (sSensorOrientation - deviceDegrees + 360) % 360;
    }

    private static void saveJpeg(ImageReader reader) {
        Image image = null;
        try {
            image = reader.acquireNextImage();
            if (image == null || sPendingPath == null) {
                return;
            }
            ByteBuffer buffer = image.getPlanes()[0].getBuffer();
            byte[] bytes = new byte[buffer.remaining()];
            buffer.get(bytes);
            File file = new File(sPendingPath);
            File parent = file.getParentFile();
            if (parent != null) {
                //noinspection ResultOfMethodCallIgnored
                parent.mkdirs();
            }
            try (FileOutputStream out = new FileOutputStream(file)) {
                out.write(bytes);
            }
            emit("saved\t" + sPendingPath);
        } catch (IOException | IllegalStateException e) {
            emit("error\t" + e.getMessage());
        } finally {
            if (image != null) {
                image.close();
            }
            sPendingPath = null;
        }
    }

    private static void close() {
        closeInternal(true);
    }

    private static void closeInternal(boolean emitEvent) {
        if (sSession != null) {
            sSession.close();
            sSession = null;
        }
        if (sDevice != null) {
            sDevice.close();
            sDevice = null;
        }
        if (sJpegReader != null) {
            sJpegReader.close();
            sJpegReader = null;
        }
        sState = "closed";
        if (emitEvent) {
            emit("state\tclosed");
        }
    }
}
