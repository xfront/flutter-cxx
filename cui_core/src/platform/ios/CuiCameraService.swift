import AVFoundation
import Foundation

/// 相机服务的 iOS 实现（对应 Android 的 CuiCameraService.java 与
/// OHOS 的 CuiCameraService.ets）。
///
/// 通道协议（与 C++ camera.cpp 一致）：
///   listen / cancel        → "ok"（事件由 CuiBridge.emitEvent 主动推）
///   availableCameras       → "id\tname\tfacing\tsensorOrientation" 每行一台
///                            facing: 1=后置 2=前置 3=外接
///   open <id>              → "ok" | "error:..."
///   close                  → "ok"
///   setFlashMode off|auto|on|torch → "ok"
///   takePicture <path>     → "ok" | "error:..."，完成时推 "saved\t<path>"
///   startPreview/stopPreview → "ok" | "error:..."
///   state                  → closed|opening|open|closing|error
///
/// 事件：state / error / saved / frame。
///
/// 预览：宿主把 App 里的 `AVCaptureVideoPreviewLayer` 通过
/// `attachPreview(layer:)` 交给本服务，`startPreview` 才有意义（引擎没有
/// AVFoundation 的 Surface 概念）。
///
/// ⚠️ 参考实现：`AVCapturePhotoSettings.flashMode` / `videoOrientation` 与
///    iOS 17 起的 rotation angle API 有版本差异，上真机前按 Xcode 校准。
final class CuiCameraService: NSObject, AVCapturePhotoCaptureDelegate {

    static let shared = CuiCameraService()

    private let sessionQueue = DispatchQueue(label: "cui.camera.session")
    private let session = AVCaptureSession()
    private var devices: [AVCaptureDevice] = []
    private var photoOutput: AVCapturePhotoOutput?
    private var previewLayer: AVCaptureVideoPreviewLayer?
    private var state = "closed"
    private var flash = "off"
    private var pendingPath = ""

    private override init() {
        super.init()
    }

    // MARK: - 通道入口

    func handle(_ method: String, _ args: String) -> String {
        switch method {
        case "listen", "cancel":
            return "ok"
        case "availableCameras":
            return listCameras()
        case "open":
            return open(args)
        case "close":
            close()
            return "ok"
        case "setFlashMode":
            flash = args.isEmpty ? "off" : args
            return "ok"
        case "takePicture":
            return takePicture(args)
        case "startPreview":
            return previewLayer == nil ? "error:preview not attached" : "ok"
        case "stopPreview":
            return "ok"
        case "state":
            return state
        default:
            return ""
        }
    }

    /// App 视图层调用：把预览图层挂到当前/下次打开的会话上。
    func attachPreview(layer: AVCaptureVideoPreviewLayer) {
        previewLayer = layer
        layer.session = session
    }

    // MARK: - 枚举

    private func refreshDevices() -> [AVCaptureDevice] {
        var types: [AVCaptureDevice.DeviceType] = [
            .builtInWideAngleCamera, .builtInTelephotoCamera
        ]
        if #available(iOS 13.0, *) {
            types.append(.builtInUltraWideCamera)
        }
        if #available(iOS 17.0, *) {
            types.append(.external)
        }
        let discovery = AVCaptureDevice.DiscoverySession(
            deviceTypes: types, mediaType: .video, position: .unspecified)
        devices = discovery.devices
        return devices
    }

    private func listCameras() -> String {
        let found = refreshDevices()
        var lines: [String] = []
        for (index, device) in found.enumerated() {
            let facing: Int
            switch device.position {
            case .back: facing = 1
            case .front: facing = 2
            default: facing = 3
            }
            // iOS 不暴露传感器方向；竖屏相机惯用 90°。
            lines.append("\(index)\t\(clean(device.localizedName))\t\(facing)\t90")
        }
        return lines.joined(separator: "\n")
    }

    // MARK: - 打开 / 关闭

    private func open(_ args: String) -> String {
        let index = Int(args.trimmingCharacters(in: .whitespaces)) ?? -1
        if devices.isEmpty {
            _ = refreshDevices()
        }
        guard index >= 0 && index < devices.count else {
            return "error:no such camera"
        }
        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else {
            return "error:camera permission not granted"
        }

        let device = devices[index]
        state = "opening"
        emit("state\topening")
        sessionQueue.async { [weak self] in
            guard let self else { return }
            self.closeInternal(notify: false)
            self.startSession(device)
        }
        return "ok"
    }

    private func startSession(_ device: AVCaptureDevice) {
        do {
            let input = try AVCaptureDeviceInput(device: device)
            session.beginConfiguration()
            if session.canSetSessionPreset(.photo) {
                session.sessionPreset = .photo
            }
            if session.canAddInput(input) {
                session.addInput(input)
            }
            let output = AVCapturePhotoOutput()
            if session.canAddOutput(output) {
                session.addOutput(output)
                photoOutput = output
            }
            session.commitConfiguration()
            session.startRunning()
            output.connection(with: .video)?.videoOrientation = .portrait
            applyTorch(device)
            state = "open"
            emit("state\topen")
        } catch {
            state = "error"
            emit("error\t\(error.localizedDescription)")
        }
    }

    private func close() {
        sessionQueue.async { [weak self] in
            self?.closeInternal(notify: true)
        }
    }

    private func closeInternal(notify: Bool) {
        let wasOpen = state != "closed"
        if session.isRunning {
            session.stopRunning()
        }
        session.beginConfiguration()
        for input in session.inputs {
            session.removeInput(input)
        }
        for output in session.outputs {
            session.removeOutput(output)
        }
        session.commitConfiguration()
        photoOutput = nil
        state = "closed"
        if notify && wasOpen {
            emit("state\tclosed")
        }
    }

    // MARK: - 拍照

    private func takePicture(_ path: String) -> String {
        guard state == "open" else {
            return "error:camera is not open"
        }
        guard !path.isEmpty else {
            return "error:no output path"
        }
        pendingPath = path
        sessionQueue.async { [weak self] in
            guard let self, let output = self.photoOutput else {
                self?.emit("error\tcamera is not open")
                return
            }
            let settings = AVCapturePhotoSettings()
            let desired = self.flashMode()
            if output.supportedFlashModes.contains(desired) {
                settings.flashMode = desired
            }
            output.capturePhoto(with: settings, delegate: self)
        }
        return "ok"
    }

    func photoOutput(_ output: AVCapturePhotoOutput,
                     didFinishProcessingPhoto photo: AVCapturePhoto,
                     error: Error?) {
        if let error {
            emit("error\t\(error.localizedDescription)")
            return
        }
        guard let data = photo.fileDataRepresentation() else {
            emit("error\tno image data")
            return
        }
        let path = pendingPath
        do {
            try data.write(to: URL(fileURLWithPath: path), options: .atomic)
            emit("saved\t\(path)")
        } catch {
            emit("error\t\(error.localizedDescription)")
        }
    }

    // MARK: - 闪光灯

    private func flashMode() -> AVCaptureDevice.FlashMode {
        switch flash {
        case "auto": return .auto
        case "on", "torch": return .on
        default: return .off
        }
    }

    private func applyTorch(_ device: AVCaptureDevice) {
        guard device.hasTorch else {
            return
        }
        do {
            try device.lockForConfiguration()
            device.torchMode = flash == "torch" ? .on : .off
            device.unlockForConfiguration()
        } catch {
            // 部分镜头不支持手电筒，不阻断打开流程。
        }
    }

    // MARK: - 工具

    private func emit(_ event: String) {
        CuiBridge.emitEvent("cui/camera", event)
    }

    private func clean(_ value: String) -> String {
        value.replacingOccurrences(of: "\t", with: " ")
            .replacingOccurrences(of: "\n", with: " ")
    }
}
