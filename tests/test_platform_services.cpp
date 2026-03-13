/// @file test_platform_services.cpp
/// @brief 平台服务层单元测试（通道 / 事件通道 / 权限 / 相机 / 蓝牙）。
///
/// 覆盖：
///   1. MethodChannel：无 backend 返回空；invoke 透传 channel/method/args；
///      处理器注册/注销；原生入站调用排队后由 pumpPlatformMessages 派发；
///      唤醒钩子只在有原生消息时触发。
///   2. EventChannel：listen 发 "listen"、cancel 发 "cancel"；事件入队后
///      下一帧派发给监听者；监听者注销后不再收到。
///   3. 权限：状态字符串解析；桌面无 backend 视为已授权；立即回复的状态
///      同步回调；"pending" 等原生 result 通知再回调；checkPermission 查询。
///   4. 相机：availableCameras 行格式解析（朝向/角度）；open 状态机
///      （Opening → 原生 state 事件 → Open）；未打开时拍照被拒；saved/error
///      事件映射；闪模式参数；打开失败回报 Error。
///   5. 蓝牙：桌面 unavailable/Unsupported/扫描失败；行格式解析（已配对
///      设备、扫描结果）；state/connection 事件；stopScan 下发。

#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cui/framework/app_lifecycle.h"
#include "cui/platform/services/app_lifecycle.h"
#include "cui/platform/services/bluetooth.h"
#include "cui/platform/services/camera.h"
#include "cui/platform/services/event_channel.h"
#include "cui/platform/services/haptic_feedback.h"
#include "cui/platform/services/method_channel.h"
#include "cui/platform/services/permission.h"
#include "cui/platform/services/system_chrome.h"
#include "cui/platform/services/system_navigator.h"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

struct Call {
    std::string channel;
    std::string method;
    std::string arguments;
};

struct FakeChannel {
    std::vector<Call> calls;
    std::function<std::optional<std::string>(const std::string& channel,
                                             const std::string& method,
                                             const std::string& args)>
        responder = [](const std::string&, const std::string&, const std::string&) {
            return std::optional<std::string>("ok");
        };

    void install() {
        cui::services::setChannelBackend(
            [this](const std::string& channel, const std::string& method,
                   const std::string& args) -> std::optional<std::string> {
                calls.push_back({channel, method, args});
                return responder(channel, method, args);
            });
    }

    static void clear() { cui::services::setChannelBackend({}); }

    const Call* find(const std::string& channel, const std::string& method) const {
        for (const auto& call : calls)
            if (call.channel == channel && call.method == method)
                return &call;
        return nullptr;
    }
};

void testMethodChannel() {
    std::printf("[MethodChannel]\n");
    using namespace cui::services;

    setChannelBackend({});
    check(!hasChannelBackend(), "无 backend 时 hasChannelBackend == false");
    check(!invokeMethod("cui_core/test", "ping").has_value(),
          "无 backend 时 invokeMethod 返回空");

    FakeChannel fake;
    fake.install();
    check(hasChannelBackend(), "安装 fake backend 后 hasChannelBackend == true");

    MethodChannel channel("cui_core/demo");
    auto reply = channel.invokeMethod("echo", "hello");
    check(reply && *reply == "ok", "MethodChannel::invokeMethod 返回后端回复");
    check(fake.calls.size() == 1 && fake.calls[0].channel == "cui_core/demo" &&
              fake.calls[0].method == "echo" && fake.calls[0].arguments == "hello",
          "channel / method / arguments 原样透传");

    bool handlerCalled = false;
    std::string seenMethod;
    setMethodCallHandler("cui_core/demo", [&](const std::string& method,
                                         const std::string& args) {
        handlerCalled = true;
        seenMethod = method + "|" + args;
        return std::string("reply");
    });
    check(hasMethodCallHandler("cui_core/demo"), "处理器注册后 hasMethodCallHandler");

    int wakes = 0;
    setWakeUpHandler([&] { ++wakes; });
    pushInboundCall("cui_core/demo", "nativeCall", "payload");
    check(!handlerCalled, "入站调用在 pump 之前不派发");
    check(wakes == 1, "pushInboundCall 触发一次唤醒");
    pumpPlatformMessages();
    check(handlerCalled && seenMethod == "nativeCall|payload",
          "pumpPlatformMessages 派发 method/args");
    pumpPlatformMessages();
    check(wakes == 1, "空 pump 不再唤醒事件循环");

    setMethodCallHandler("cui_core/demo", nullptr);
    check(!hasMethodCallHandler("cui_core/demo"), "空处理器注销注册");
    setWakeUpHandler(nullptr);
    FakeChannel::clear();
}

void testEventChannel() {
    std::printf("[EventChannel]\n");
    using namespace cui::services;

    FakeChannel fake;
    fake.install();

    EventChannel events("cui_core/events");
    check(!events.isListening(), "初始未订阅");
    check(events.listen(), "listen() 经通道成功");
    check(events.isListening(), "listen 后 isListening");
    check(fake.find("cui_core/events", "listen") != nullptr, "下发 listen 方法");

    std::vector<std::string> received;
    events.setListener([&](const std::string& event) { received.push_back(event); });

    pushInboundEvent("cui_core/events", "alpha");
    check(received.empty(), "事件在 pump 之前不派发");
    pumpPlatformMessages();
    check(received.size() == 1 && received[0] == "alpha", "pump 派发事件");

    events.setListener(nullptr);
    pushInboundEvent("cui_core/events", "beta");
    pumpPlatformMessages();
    check(received.size() == 1, "注销监听者后不再收到事件");

    events.setListener([&](const std::string& event) { received.push_back(event); });
    events.cancel();
    check(!events.isListening(), "cancel 后 isListening == false");
    check(fake.find("cui_core/events", "cancel") != nullptr, "下发 cancel 方法");

    FakeChannel::clear();
}

void testPermission() {
    std::printf("[Permission]\n");
    using namespace cui::services;

    check(std::string(permissionCode(Permission::Camera)) == "camera", "camera 权限码");
    check(std::string(permissionCode(Permission::BluetoothScan)) == "bluetoothScan",
          "bluetoothScan 权限码");
    check(parsePermissionStatus("granted") == PermissionStatus::Granted,
          "解析 granted");
    check(parsePermissionStatus("limited") == PermissionStatus::Limited, "解析 limited");
    check(parsePermissionStatus("permanently_denied") ==
              PermissionStatus::PermanentlyDenied,
          "解析 permanently_denied 别名");
    check(parsePermissionStatus("weird") == PermissionStatus::Unknown, "未知状态");

    FakeChannel::clear();
    check(checkPermission(Permission::Camera) == PermissionStatus::Granted,
          "桌面无后端时 checkPermission 视为已授权");
    bool granted = false;
    requestPermission(Permission::Camera, [&](PermissionStatus status) {
        granted = status == PermissionStatus::Granted;
    });
    check(granted, "桌面无后端时 requestPermission 同步授予");

    FakeChannel fake;
    fake.install();
    fake.responder = [](const std::string& channel, const std::string& method,
                        const std::string& args) -> std::optional<std::string> {
        if (channel == "cui_core/permission" && method == "request" && args == "camera")
            return std::string("denied");
        return std::string("ok");
    };
    PermissionStatus got = PermissionStatus::Unknown;
    requestPermission(Permission::Camera,
                      [&](PermissionStatus status) { got = status; });
    check(got == PermissionStatus::Denied, "后端立即回复 denied 时同步回调");

    fake.responder = [](const std::string& channel, const std::string& method,
                        const std::string&) -> std::optional<std::string> {
        if (channel == "cui_core/permission" && method == "request")
            return std::string("pending");
        return std::string("ok");
    };
    got = PermissionStatus::Unknown;
    requestPermission(Permission::Microphone,
                      [&](PermissionStatus status) { got = status; });
    check(got == PermissionStatus::Unknown, "pending 时不同步回调");
    pushInboundCall("cui_core/permission", "result", "microphone\tgranted");
    pumpPlatformMessages();
    check(got == PermissionStatus::Granted, "原生 result 通知后回调 granted");

    fake.responder = [](const std::string& channel, const std::string& method,
                        const std::string&) -> std::optional<std::string> {
        if (channel == "cui_core/permission" && method == "check")
            return std::string("limited");
        return std::string("ok");
    };
    check(checkPermission(Permission::Photos) == PermissionStatus::Limited,
          "checkPermission 解析后端状态");

    FakeChannel::clear();
}

void testCamera() {
    std::printf("[Camera]\n");
    using namespace cui::services;

    FakeChannel::clear();
    check(CameraController::availableCameras().empty(), "桌面无后端时相机列表为空");
    {
        CameraController camera;
        check(camera.open(0) == false, "桌面无后端时 open 失败");
        check(camera.state() == CameraState::Error, "打开失败状态为 Error");
    }

    FakeChannel fake;
    fake.install();
    fake.responder = [](const std::string& channel, const std::string& method,
                        const std::string&) -> std::optional<std::string> {
        if (channel == "cui_core/camera" && method == "availableCameras")
            return std::string("0\tBack Camera\t1\t90\n1\tFront Camera\t2\t270\n");
        return std::string("ok");
    };
    auto cameras = CameraController::availableCameras();
    check(cameras.size() == 2, "解析两台相机");
    check(cameras[0].id == 0 && cameras[0].name == "Back Camera" &&
              cameras[0].facing == CameraLensFacing::Back &&
              cameras[0].sensorOrientation == 90,
          "第一台：id / 名称 / 后置 / 90°");
    check(cameras[1].facing == CameraLensFacing::Front &&
              cameras[1].sensorOrientation == 270,
          "第二台：前置 / 270°");

    CameraController camera;
    std::vector<CameraEvent> events;
    camera.setEventListener([&](const CameraEvent& event) { events.push_back(event); });

    check(camera.open(1), "open 下发成功");
    check(camera.state() == CameraState::Opening, "open 后状态 Opening");
    check(camera.cameraId() == 1, "记录相机 id");
    check(fake.find("cui_core/camera", "listen") != nullptr, "open 前订阅相机事件");
    const Call* openCall = fake.find("cui_core/camera", "open");
    check(openCall && openCall->arguments == "1", "open 参数为相机 id");
    check(!events.empty() && events.back().type == CameraEventType::StateChanged &&
              events.back().state == CameraState::Opening,
          "状态变化回调 Opening");

    check(!camera.takePicture("shot.jpg"), "Opening 状态拍照被拒");
    check(events.back().type == CameraEventType::Error, "未打开拍照回报 Error");

    camera.handleEvent("state\topen");
    check(camera.isOpen(), "原生 state=open 后 isOpen");
    check(events.back().state == CameraState::Open, "状态回调 Open");

    check(camera.setFlashMode(FlashMode::Torch), "设置闪模式成功");
    check(fake.find("cui_core/camera", "setFlashMode")->arguments == "torch",
          "闪模式参数 torch");

    check(camera.takePicture("shot.jpg"), "打开后拍照下发成功");
    check(fake.find("cui_core/camera", "takePicture")->arguments == "shot.jpg",
          "拍照参数为路径");
    camera.handleEvent("saved\tshot.jpg");
    check(events.back().type == CameraEventType::PictureSaved &&
              events.back().message == "shot.jpg",
          "saved 事件携带路径");

    camera.handleEvent("error\tsensor timeout");
    check(camera.state() == CameraState::Error && events.back().type ==
              CameraEventType::Error && events.back().message == "sensor timeout",
          "原生 error 事件映射");

    camera.close();
    check(camera.state() == CameraState::Closed, "close 后状态 Closed");
    check(fake.find("cui_core/camera", "close") != nullptr, "下发 close");

    FakeChannel::clear();
}

void testBluetooth() {
    std::printf("[Bluetooth]\n");
    using namespace cui::services;

    check(std::string(bluetoothAdapterStateName(BluetoothAdapterState::TurningOn)) ==
              "turningOn",
          "适配器状态名");
    check(parseBluetoothAdapterState("unauthorized") ==
              BluetoothAdapterState::Unauthorized,
          "解析 unauthorized");
    check(parseBluetoothConnectionState("connected") ==
              BluetoothConnectionState::Connected,
          "解析连接状态 connected");

    FakeChannel::clear();
    {
        BluetoothService bt;
        check(!bt.isAvailable(), "桌面无后端时不可用");
        check(bt.adapterState() == BluetoothAdapterState::Unsupported,
              "桌面适配器状态 Unsupported");
        check(!bt.startScan([](const BluetoothDevice&) {}), "桌面扫描启动失败");
        check(!bt.isScanning(), "桌面不处于扫描态");
    }

    FakeChannel fake;
    fake.install();
    fake.responder = [](const std::string& channel, const std::string& method,
                        const std::string&) -> std::optional<std::string> {
        if (channel != "cui_core/bluetooth")
            return std::string("ok");
        if (method == "available")
            return std::string("1");
        if (method == "state")
            return std::string("on");
        if (method == "bondedDevices")
            return std::string("AA:BB\tHeadphones\t0\t1\nCC:DD\tWatch\t0\t0\n");
        return std::string("ok");
    };

    BluetoothService bt;
    check(bt.isAvailable(), "后端 available=1 时可用");
    check(bt.adapterState() == BluetoothAdapterState::On, "适配器状态 On");
    check(bt.isEnabled(), "isEnabled 派生自 On");

    auto bonded = bt.bondedDevices();
    check(bonded.size() == 2, "解析两台已配对设备");
    check(bonded[0].id == "AA:BB" && bonded[0].name == "Headphones" &&
              bonded[0].paired,
          "已配对设备字段解析");
    check(!bonded[1].paired, "未配对标记");

    std::vector<BluetoothDevice> found;
    std::vector<BluetoothAdapterState> states;
    std::vector<std::pair<std::string, BluetoothConnectionState>> connections;
    check(bt.startScan(
              [&](const BluetoothDevice& device) { found.push_back(device); },
              [&](BluetoothAdapterState state) { states.push_back(state); }),
          "startScan 下发成功");
    check(bt.isScanning(), "扫描中");
    check(fake.find("cui_core/bluetooth", "listen") != nullptr, "扫描前订阅事件");

    bt.handleEvent("device\tEE:FF\tSensor\t-42\t0");
    check(found.size() == 1 && found[0].id == "EE:FF" &&
              found[0].name == "Sensor" && found[0].rssi == -42 && !found[0].paired,
          "扫描结果解析（rssi / paired）");

    bt.handleEvent("state\toff");
    check(states.size() == 1 && states[0] == BluetoothAdapterState::Off,
          "适配器状态事件");

    check(bt.connect("EE:FF", [&](const std::string& id,
                                  BluetoothConnectionState state) {
              connections.emplace_back(id, state);
          }),
          "connect 下发成功");
    check(fake.find("cui_core/bluetooth", "connect")->arguments == "EE:FF",
          "connect 参数为设备 id");
    bt.handleEvent("connection\tEE:FF\tconnected");
    check(connections.size() == 1 && connections[0].first == "EE:FF" &&
              connections[0].second == BluetoothConnectionState::Connected,
          "连接状态事件");

    bt.disconnect("EE:FF");
    check(fake.find("cui_core/bluetooth", "disconnect") != nullptr, "disconnect 下发");

    bt.stopScan();
    check(!bt.isScanning(), "stopScan 后退出扫描态");
    check(fake.find("cui_core/bluetooth", "stopScan") != nullptr, "stopScan 下发");

    FakeChannel::clear();
}

void testAppLifecycle() {
    std::printf("[AppLifecycle]\n");
    using namespace cui::services;

    check(parseAppLifecycleState("AppLifecycleState.resumed") ==
              AppLifecycleState::Resumed,
          "解析主机前缀形式 AppLifecycleState.resumed");
    check(parseAppLifecycleState("hidden") == AppLifecycleState::Hidden,
          "解析裸名 hidden");
    check(!parseAppLifecycleState("bogus").has_value(), "未知状态返回空");
    check(appLifecycleState() == AppLifecycleState::Detached, "初始为 detached");

    std::vector<AppLifecycleState> seen;
    int handle = addAppLifecycleObserver([&](AppLifecycleState state) {
        seen.push_back(state);
    });
    setAppLifecycleState(AppLifecycleState::Resumed);
    check(seen.size() == 1 && seen[0] == AppLifecycleState::Resumed,
          "状态变化通知观察者");
    setAppLifecycleState(AppLifecycleState::Resumed);
    check(seen.size() == 1, "同一状态不重复通知");
    removeAppLifecycleObserver(handle);
    setAppLifecycleState(AppLifecycleState::Inactive);
    check(seen.size() == 1, "注销后不再通知");
    check(appLifecycleState() == AppLifecycleState::Inactive,
          "无观察者时状态仍被缓存");

    // 主机入站：cui_core/lifecycle + AppLifecycleState.changed（引擎同名协议）。
    std::vector<AppLifecycleState> inbound;
    int handle2 = addAppLifecycleObserver([&](AppLifecycleState state) {
        inbound.push_back(state);
    });
    pushInboundCall("cui_core/lifecycle", "AppLifecycleState.changed",
                    "AppLifecycleState.hidden");
    check(inbound.empty(), "入站事件在 pump 之前不派发");
    pumpPlatformMessages();
    check(inbound.size() == 1 && inbound[0] == AppLifecycleState::Hidden,
          "pump 派发主机生命周期事件");
    removeAppLifecycleObserver(handle2);

    // AppLifecycleListener 的状态机（逐条对齐上游过渡表）。
    setAppLifecycleState(AppLifecycleState::Detached);
    std::vector<std::string> calls;
    cui::AppLifecycleCallbacks callbacks;
    callbacks.onStateChange = [&](AppLifecycleState s) {
        calls.push_back(std::string("state:") + appLifecycleStateName(s));
    };
    callbacks.onResume = [&] { calls.push_back("resume"); };
    callbacks.onInactive = [&] { calls.push_back("inactive"); };
    callbacks.onHide = [&] { calls.push_back("hide"); };
    callbacks.onShow = [&] { calls.push_back("show"); };
    callbacks.onPause = [&] { calls.push_back("pause"); };
    callbacks.onRestart = [&] { calls.push_back("restart"); };
    callbacks.onDetach = [&] { calls.push_back("detach"); };
    {
        cui::AppLifecycleListener listener(callbacks);
        check(listener.lifecycleState() == AppLifecycleState::Detached,
              "监听器构造时读取当前状态");
        setAppLifecycleState(AppLifecycleState::Resumed);
        setAppLifecycleState(AppLifecycleState::Inactive);
        setAppLifecycleState(AppLifecycleState::Hidden);
        setAppLifecycleState(AppLifecycleState::Paused);
        setAppLifecycleState(AppLifecycleState::Hidden);
        setAppLifecycleState(AppLifecycleState::Inactive);
        setAppLifecycleState(AppLifecycleState::Resumed);
        setAppLifecycleState(AppLifecycleState::Detached);

        const std::vector<std::string> expected = {
            "resume", "state:resumed",       "inactive", "state:inactive",
            "hide",   "state:hidden",        "pause",    "state:paused",
            "restart", "state:hidden",       "show",     "state:inactive",
            "resume", "state:resumed",       "detach",   "state:detached",
        };
        check(calls == expected,
              "onResume/onInactive/onHide/onPause/onRestart/onShow/onDetach 顺序");
    }
    check(appLifecycleState() == AppLifecycleState::Detached, "收尾状态 detached");
}

void testSystemChrome() {
    std::printf("[SystemChrome]\n");
    using namespace cui::services;

    FakeChannel fake;
    fake.install();

    check(!SystemChrome::latestStyle().has_value(), "初始没有缓存的 overlay 样式");

    check(SystemChrome::setPreferredOrientations(
              {DeviceOrientation::PortraitUp, DeviceOrientation::LandscapeLeft}),
          "setPreferredOrientations 返回成功");
    check(fake.calls.back().channel == "cui_core/platform" &&
              fake.calls.back().method == "SystemChrome.setPreferredOrientations" &&
              fake.calls.back().arguments ==
                      "DeviceOrientation.portraitUp,DeviceOrientation.landscapeLeft",
          "方向参数为 DeviceOrientation.* 逗号列表");

    SystemChrome::setPreferredOrientations({});
    check(fake.calls.back().arguments.empty(), "空列表 = 恢复系统默认");

    SystemChrome::setEnabledSystemUIMode(SystemUiMode::ImmersiveSticky);
    check(fake.calls.back().method == "SystemChrome.setEnabledSystemUIMode" &&
              fake.calls.back().arguments == "SystemUiMode.immersiveSticky",
          "非 manual 模式下发模式名");

    SystemChrome::setEnabledSystemUIMode(SystemUiMode::Manual,
                                         {SystemUiOverlay::Top});
    check(fake.calls.back().method == "SystemChrome.setEnabledSystemUIOverlays" &&
              fake.calls.back().arguments == "SystemUiOverlay.top",
          "manual 模式改发 overlays 列表（上游同款分叉）");

    check(SystemChrome::setSystemUIOverlayStyle(SystemUiOverlayStyle::dark),
          "setSystemUIOverlayStyle 返回成功");
    const std::string encoded = fake.calls.back().arguments;
    check(fake.calls.back().method == "SystemChrome.setSystemUIOverlayStyle" &&
              encoded.find("statusBarIconBrightness=Brightness.dark") !=
                      std::string::npos &&
              encoded.find("statusBarBrightness=Brightness.light") !=
                      std::string::npos &&
              encoded.find("systemNavigationBarColor=#000000") != std::string::npos,
          "overlay 样式编码为 key=value 列表");
    check(SystemChrome::latestStyle() &&
              *SystemChrome::latestStyle() == SystemUiOverlayStyle::dark,
          "latestStyle 缓存最近一次样式");

    const size_t before = fake.calls.size();
    check(SystemChrome::setSystemUIOverlayStyle(SystemUiOverlayStyle::dark),
          "重复样式视为平凡成功");
    check(fake.calls.size() == before, "重复样式不再走通道");

    SystemChrome::setApplicationSwitcherDescription(
        "cui_core", cui::Color::fromRGB(0x11 / 255.0, 0x22 / 255.0, 0x33 / 255.0));
    check(fake.calls.back().method == "SystemChrome.setApplicationSwitcherDescription" &&
              fake.calls.back().arguments == "cui_core\t#112233",
          "应用切换器描述 label\\tcolor");

    check(SystemChrome::restoreSystemUIOverlays(), "restoreSystemUIOverlays 下发");
    check(fake.calls.back().method == "SystemChrome.restoreSystemUIOverlays",
          "restore 方法名");

    FakeChannel::clear();
    check(!SystemChrome::restoreSystemUIOverlays(),
          "无宿主时返回 false（桌面如实报告未连接）");
}

void testHapticFeedback() {
    std::printf("[HapticFeedback]\n");
    using namespace cui::services;

    FakeChannel fake;
    fake.install();

    check(HapticFeedback::lightImpact(), "lightImpact 下发");
    check(fake.calls.back().channel == "cui_core/platform" &&
              fake.calls.back().method == "HapticFeedback.vibrate" &&
              fake.calls.back().arguments == "HapticFeedbackType.lightImpact",
          "轻触参数为 HapticFeedbackType.lightImpact");
    check(HapticFeedback::heavyImpact() &&
              fake.calls.back().arguments == "HapticFeedbackType.heavyImpact",
          "重触参数");
    check(HapticFeedback::selectionClick() &&
              fake.calls.back().arguments == "HapticFeedbackType.selectionClick",
          "选择反馈参数");
    check(HapticFeedback::vibrate() && fake.calls.back().arguments.empty(),
          "vibrate 不带参数（同上游）");
    check(HapticFeedback::successNotification() &&
              fake.calls.back().arguments ==
                      "HapticFeedbackType.successNotification",
          "成功通知参数");
    check(SystemSound::playClick() &&
              fake.calls.back().method == "SystemSound.play" &&
              fake.calls.back().arguments == "SystemSoundType.click",
          "系统提示音方法/参数");

    FakeChannel::clear();
    check(!HapticFeedback::vibrate(), "无宿主时返回 false");
}

void testSystemNavigator() {
    std::printf("[SystemNavigator]\n");
    using namespace cui::services;

    FakeChannel fake;
    fake.install();

    check(SystemNavigator::pop(true), "pop 下发");
    check(fake.calls.back().channel == "cui_core/platform" &&
              fake.calls.back().method == "SystemNavigator.pop" &&
              fake.calls.back().arguments == "1",
          "pop(animated) 参数为 1");
    check(SystemNavigator::pop(false) && fake.calls.back().arguments == "0",
          "pop(false) 参数为 0");
    check(SystemNavigator::setFrameworkHandlesBack(true) &&
              fake.calls.back().method == "SystemNavigator.setFrameworkHandlesBack" &&
              fake.calls.back().arguments == "1",
          "setFrameworkHandlesBack 下发");

    check(SystemNavigator::backHandlerCount() == 0, "初始无返回键处理器");
    int order = 0;
    int firstCall = 0;
    int secondCall = 0;
    int handle = SystemNavigator::addBackHandler([&] {
        firstCall = ++order;
        return true;
    });
    check(fake.calls.back().method == "SystemNavigator.setFrameworkHandlesBack" &&
              fake.calls.back().arguments == "1",
          "注册处理器后通知宿主 framework 接管返回键");
    check(SystemNavigator::backHandlerCount() == 1, "处理器数量 1");

    int handle2 = SystemNavigator::addBackHandler([&] {
        secondCall = ++order;
        return true;
    });
    check(SystemNavigator::handleBack(), "handleBack 被消费");
    check(secondCall == 1 && firstCall == 0, "后注册的处理器优先（最内层应用）");
    SystemNavigator::removeBackHandler(handle2);
    check(SystemNavigator::handleBack() && firstCall == 2, "移除后轮到外层处理器");
    SystemNavigator::removeBackHandler(handle);
    check(SystemNavigator::backHandlerCount() == 0, "全部移除");
    check(fake.calls.back().method == "SystemNavigator.setFrameworkHandlesBack" &&
              fake.calls.back().arguments == "0",
          "全部移除后恢复 frameworkHandlesBack=false");

    const size_t before = fake.calls.size();
    check(!SystemNavigator::handleBack(), "无人接手时返回 false");
    check(fake.calls.size() == before + 1 &&
              fake.calls.back().method == "SystemNavigator.pop",
          "无人接手时引擎自身 pop()（根路由退出应用）");

    // 主机入站：channel "cui_core/system_navigator"，method "back"。
    bool inboundBack = false;
    int handle3 = SystemNavigator::addBackHandler([&] {
        inboundBack = true;
        return true;
    });
    pushInboundCall("cui_core/system_navigator", "back", "");
    check(!inboundBack, "入站返回键在 pump 之前不派发");
    pumpPlatformMessages();
    check(inboundBack, "pump 派发主机返回键事件");
    SystemNavigator::removeBackHandler(handle3);

    FakeChannel::clear();
    check(!SystemNavigator::pop(true), "无宿主时 pop 返回 false");
}

}  // namespace

int main() {
    std::printf("=== platform services tests ===\n");
    testMethodChannel();
    testEventChannel();
    testPermission();
    testCamera();
    testBluetooth();
    testAppLifecycle();
    testSystemChrome();
    testHapticFeedback();
    testSystemNavigator();

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURES\n", g_failures);
    return 1;
}
