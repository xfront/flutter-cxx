package dev.cui.framework;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Bluetooth LE backend for {@code cui/bluetooth}.
 *
 * <p>Methods: {@code available}, {@code state}, {@code requestEnable},
 * {@code bondedDevices}, {@code startScan}, {@code stopScan},
 * {@code connect}, {@code disconnect} (plus {@code listen}/{@code cancel}).
 *
 * <p>Events on {@code cui/bluetooth}:
 * {@code device\t<id>\t<name>\t<rssi>\t<paired>},
 * {@code state\t<on|off|turningOn|turningOff|unauthorized|unsupported>},
 * {@code connection\t<id>\t<connected|disconnected>},
 * {@code error\t<message>}.
 *
 * <p>Android 12+ requires BLUETOOTH_SCAN / BLUETOOTH_CONNECT (request them
 * from C++ with Permission::BluetoothScan / Permission::Bluetooth). Pairing
 * is left to the system settings app.
 */
final class CuiBluetoothService {
    private static final String TAG = "cui-bluetooth";
    private static final String CHANNEL = "cui/bluetooth";

    private static BluetoothLeScanner sScanner;
    private static ScanCallback sScanCallback;
    private static final Map<String, BluetoothGatt> sConnections = new ConcurrentHashMap<>();

    private CuiBluetoothService() {
    }

    static String handle(Activity activity, String method, String args) {
        switch (method) {
            case "listen":
            case "cancel":
                return "ok";
            case "available":
                return available(activity) ? "1" : "0";
            case "state":
                return state(activity);
            case "requestEnable":
                return requestEnable(activity);
            case "bondedDevices":
                return bondedDevices(activity);
            case "startScan":
                return startScan(activity);
            case "stopScan":
                stopScan();
                return "ok";
            case "connect":
                return connect(activity, args);
            case "disconnect":
                disconnect(args);
                return "ok";
            default:
                return "";
        }
    }

    private static void emit(String event) {
        CuiActivity.emitCuiEvent(CHANNEL, event);
    }

    private static BluetoothAdapter adapter(Activity activity) {
        BluetoothManager manager =
                (BluetoothManager) activity.getSystemService(Context.BLUETOOTH_SERVICE);
        return manager == null ? null : manager.getAdapter();
    }

    private static boolean hasPermission(Activity activity, String permission) {
        if (Build.VERSION.SDK_INT < 23) {
            return true;
        }
        // Pre-31 Bluetooth permissions are normal (manifest-only) permissions.
        if (Build.VERSION.SDK_INT < 31
                && (Manifest.permission.BLUETOOTH_SCAN.equals(permission)
                    || Manifest.permission.BLUETOOTH_CONNECT.equals(permission))) {
            return true;
        }
        return activity.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    private static boolean available(Activity activity) {
        return activity.getPackageManager()
                .hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE);
    }

    private static String state(Activity activity) {
        BluetoothAdapter adapter = adapter(activity);
        if (adapter == null || !available(activity)) {
            return "unsupported";
        }
        if (!hasPermission(activity, Manifest.permission.BLUETOOTH_CONNECT)) {
            return "unauthorized";
        }
        try {
            switch (adapter.getState()) {
                case BluetoothAdapter.STATE_ON:
                    return "on";
                case BluetoothAdapter.STATE_OFF:
                    return "off";
                case BluetoothAdapter.STATE_TURNING_ON:
                    return "turningOn";
                case BluetoothAdapter.STATE_TURNING_OFF:
                    return "turningOff";
                default:
                    return "unknown";
            }
        } catch (SecurityException e) {
            return "unauthorized";
        }
    }

    private static String requestEnable(Activity activity) {
        BluetoothAdapter adapter = adapter(activity);
        if (adapter == null) {
            return "error:unsupported";
        }
        if (adapter.isEnabled()) {
            return "ok";
        }
        try {
            activity.startActivity(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE));
            return "ok";
        } catch (SecurityException e) {
            return "error:" + e.getMessage();
        }
    }

    private static String bondedDevices(Activity activity) {
        BluetoothAdapter adapter = adapter(activity);
        if (adapter == null || !hasPermission(activity, Manifest.permission.BLUETOOTH_CONNECT)) {
            return "";
        }
        StringBuilder out = new StringBuilder();
        try {
            for (BluetoothDevice device : adapter.getBondedDevices()) {
                out.append(device.getAddress()).append('\t')
                        .append(clean(device.getName())).append('\t')
                        .append(0).append('\t')
                        .append(1).append('\n');
            }
        } catch (SecurityException e) {
            Log.w(TAG, "bondedDevices failed: " + e);
        }
        return out.toString();
    }

    private static String startScan(Activity activity) {
        if (!hasPermission(activity, Manifest.permission.BLUETOOTH_SCAN)) {
            return "error:scan permission denied";
        }
        BluetoothAdapter adapter = adapter(activity);
        if (adapter == null) {
            return "error:unsupported";
        }
        if (!adapter.isEnabled()) {
            return "error:bluetooth is off";
        }
        stopScan();
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            return "error:scanner unavailable";
        }
        sScanner = scanner;
        sScanCallback = new ScanCallback() {
            @Override
            public void onScanResult(int callbackType, ScanResult result) {
                BluetoothDevice device = result.getDevice();
                String name = result.getScanRecord() == null
                        ? null : result.getScanRecord().getDeviceName();
                emit("device\t" + device.getAddress() + "\t" + clean(name)
                        + "\t" + result.getRssi() + "\t0");
            }

            @Override
            public void onScanFailed(int errorCode) {
                emit("error\tscan failed " + errorCode);
            }
        };
        try {
            ScanSettings settings = new ScanSettings.Builder()
                    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                    .build();
            scanner.startScan(null, settings, sScanCallback);
            return "ok";
        } catch (SecurityException | IllegalStateException e) {
            sScanner = null;
            sScanCallback = null;
            return "error:" + e.getMessage();
        }
    }

    private static void stopScan() {
        if (sScanner == null || sScanCallback == null) {
            return;
        }
        try {
            sScanner.stopScan(sScanCallback);
        } catch (SecurityException | IllegalStateException e) {
            Log.w(TAG, "stopScan failed: " + e);
        }
        sScanner = null;
        sScanCallback = null;
    }

    private static String connect(Activity activity, String id) {
        if (id == null || id.isEmpty()) {
            return "error:no device id";
        }
        if (!hasPermission(activity, Manifest.permission.BLUETOOTH_CONNECT)) {
            return "error:connect permission denied";
        }
        BluetoothAdapter adapter = adapter(activity);
        if (adapter == null) {
            return "error:unsupported";
        }
        final String address = id;
        try {
            BluetoothDevice device = adapter.getRemoteDevice(address);
            BluetoothGatt gatt = device.connectGatt(activity, false,
                    new BluetoothGattCallback() {
                        @Override
                        public void onConnectionStateChange(BluetoothGatt g, int status,
                                                            int newState) {
                            String state;
                            switch (newState) {
                                case BluetoothGatt.STATE_CONNECTED:
                                    state = "connected";
                                    break;
                                case BluetoothGatt.STATE_CONNECTING:
                                    state = "connecting";
                                    break;
                                case BluetoothGatt.STATE_DISCONNECTING:
                                    state = "disconnecting";
                                    break;
                                case BluetoothGatt.STATE_DISCONNECTED:
                                default:
                                    state = "disconnected";
                                    break;
                            }
                            emit("connection\t" + address + "\t" + state);
                            if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                                sConnections.remove(address);
                                g.close();
                            }
                        }
                    }, BluetoothDevice.TRANSPORT_LE);
            if (gatt == null) {
                return "error:connectGatt failed";
            }
            sConnections.put(address, gatt);
            emit("connection\t" + address + "\tconnecting");
            return "ok";
        } catch (IllegalArgumentException | SecurityException e) {
            return "error:" + e.getMessage();
        }
    }

    private static void disconnect(String id) {
        if (id == null) {
            return;
        }
        BluetoothGatt gatt = sConnections.remove(id);
        if (gatt == null) {
            return;
        }
        try {
            gatt.disconnect();
        } catch (SecurityException e) {
            Log.w(TAG, "disconnect failed: " + e);
        } finally {
            gatt.close();
        }
        emit("connection\t" + id + "\tdisconnected");
    }

    private static String clean(String value) {
        return value == null ? "" : value.replace('\t', ' ').replace('\n', ' ');
    }
}
