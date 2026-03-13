#pragma once
/// @file camera.h
/// @brief Camera access (mirrors Flutter's camera plugin surface).
///
/// Flutter:
///   final cameras = await availableCameras();
///   final controller = CameraController(cameras.first, ResolutionPreset.high);
///   await controller.initialize();
///   final file = await controller.takePicture();
///
/// cui_core: CameraController is a lifecycle handle; frames/captures are reported
/// through events so the widget layer never blocks the UI thread.
///
///   services::CameraController camera;
///   camera.setStateListener([](services::CameraState s, const std::string& msg) { ... });
///   camera.open(0);            // async on Android: state → Opening → Open
///   camera.takePicture("shot.jpg");  // "saved" event carries the path
///
/// Request Permission::Camera first (requestPermission() is async on
/// Android); open() reports a permission failure through state Error.
///
/// Desktop: availableCameras() is empty and open() returns false.

#include <functional>
#include <string>
#include <vector>

#include "cui/platform/services/event_channel.h"

namespace cui {
namespace services {

enum class CameraLensFacing {
    Unknown,
    Back,
    Front,
    External,
};

struct CameraInfo {
    int id = 0;
    std::string name;
    CameraLensFacing facing = CameraLensFacing::Unknown;
    int sensorOrientation = 0;
};

enum class CameraState {
    Closed,
    Opening,
    Open,
    Closing,
    Error,
};

enum class FlashMode {
    Off,
    Auto,
    On,
    Torch,
};

const char* cameraStateName(CameraState state);
CameraState parseCameraState(const std::string& value);

enum class CameraEventType {
    StateChanged,
    Error,
    PictureSaved, ///< message = file path
    PreviewFrame, ///< message = host-defined frame descriptor
};

struct CameraEvent {
    CameraEventType type = CameraEventType::StateChanged;
    CameraState state = CameraState::Closed;
    std::string message;
};

class CameraController {
public:
    using EventListener = std::function<void(const CameraEvent& event)>;

    CameraController();
    ~CameraController();

    CameraController(const CameraController&) = delete;
    CameraController& operator=(const CameraController&) = delete;

    /// Enumerate cameras (Flutter: availableCameras()). Empty when the host is
    /// unavailable or the device has no camera.
    static std::vector<CameraInfo> availableCameras();

    /// Start opening a camera. cameraId 0 is normally the back camera.
    bool open(int cameraId = 0);
    void close();

    CameraState state() const { return state_; }
    bool isOpen() const { return state_ == CameraState::Open; }
    int cameraId() const { return cameraId_; }

    /// Preview frames opt-in (host pushes "frame" events; see handleEvent()).
    bool startPreview();
    bool stopPreview();

    bool setFlashMode(FlashMode mode);

    /// Capture a JPEG to `path`. Completion arrives as CameraEventType::
    /// PictureSaved (message = path); failures as Error.
    bool takePicture(const std::string& path);

    void setEventListener(EventListener listener);

    /// Exposed for tests / custom transports: feed one raw event line.
    void handleEvent(const std::string& event);

private:
    void ensureListening();
    void emit(CameraEventType type, const std::string& message);

    EventChannel events_{"cui_core/camera"};
    EventListener listener_;
    CameraState state_ = CameraState::Closed;
    int cameraId_ = 0;
    bool previewing_ = false;
};

}  // namespace services
}  // namespace cui_core
