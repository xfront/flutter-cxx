/// @file camera.cpp
/// @brief Camera service (see camera.h).

#include "cui/platform/services/camera.h"

#include <cstdlib>
#include <sstream>
#include <utility>
#include <vector>

#include "cui/platform/services/method_channel.h"

namespace cui {
namespace services {

namespace {

constexpr const char* kChannel = "cui_core/camera";

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r')
                current.pop_back();
            if (!current.empty())
                lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        lines.push_back(current);
    return lines;
}

std::vector<std::string> splitTabs(const std::string& text) {
    std::vector<std::string> fields;
    std::string current;
    for (char c : text) {
        if (c == '\t') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
}

bool replyIsError(const std::string& reply) {
    return reply.rfind("error", 0) == 0;
}

const char* flashModeName(FlashMode mode) {
    switch (mode) {
        case FlashMode::Off: return "off";
        case FlashMode::Auto: return "auto";
        case FlashMode::On: return "on";
        case FlashMode::Torch: return "torch";
    }
    return "off";
}

}  // namespace

const char* cameraStateName(CameraState state) {
    switch (state) {
        case CameraState::Closed: return "closed";
        case CameraState::Opening: return "opening";
        case CameraState::Open: return "open";
        case CameraState::Closing: return "closing";
        case CameraState::Error: return "error";
    }
    return "closed";
}

CameraState parseCameraState(const std::string& value) {
    if (value == "opening")
        return CameraState::Opening;
    if (value == "open")
        return CameraState::Open;
    if (value == "closing")
        return CameraState::Closing;
    if (value == "error")
        return CameraState::Error;
    return CameraState::Closed;
}

CameraController::CameraController() {
    events_.setListener([this](const std::string& event) { handleEvent(event); });
}

CameraController::~CameraController() {
    if (cameraId_ >= 0 && state_ != CameraState::Closed)
        close();
    events_.cancel();
}

std::vector<CameraInfo> CameraController::availableCameras() {
    std::vector<CameraInfo> cameras;
    auto reply = invokeMethod(kChannel, "availableCameras");
    if (!reply)
        return cameras;
    for (const auto& line : splitLines(*reply)) {
        const auto fields = splitTabs(line);
        if (fields.empty())
            continue;
        CameraInfo info;
        info.id = std::atoi(fields[0].c_str());
        if (fields.size() > 1)
            info.name = fields[1];
        if (fields.size() > 2) {
            const int facing = std::atoi(fields[2].c_str());
            switch (facing) {
                case 1: info.facing = CameraLensFacing::Back; break;
                case 2: info.facing = CameraLensFacing::Front; break;
                case 3: info.facing = CameraLensFacing::External; break;
                default: info.facing = CameraLensFacing::Unknown; break;
            }
        }
        if (fields.size() > 3)
            info.sensorOrientation = std::atoi(fields[3].c_str());
        cameras.push_back(std::move(info));
    }
    return cameras;
}

void CameraController::ensureListening() {
    if (!events_.isListening())
        events_.listen();
}

bool CameraController::open(int cameraId) {
    ensureListening();
    auto reply = invokeMethod(kChannel, "open", std::to_string(cameraId));
    if (!reply || replyIsError(*reply)) {
        state_ = CameraState::Error;
        emit(CameraEventType::Error, reply ? *reply : "camera host unavailable");
        return false;
    }
    cameraId_ = cameraId;
    state_ = CameraState::Opening;
    emit(CameraEventType::StateChanged, {});
    return true;
}

void CameraController::close() {
    if (state_ != CameraState::Closed) {
        state_ = CameraState::Closing;
        emit(CameraEventType::StateChanged, {});
        invokeMethod(kChannel, "close");
    }
    state_ = CameraState::Closed;
    previewing_ = false;
    emit(CameraEventType::StateChanged, {});
}

bool CameraController::startPreview() {
    auto reply = invokeMethod(kChannel, "startPreview");
    if (!reply || replyIsError(*reply))
        return false;
    previewing_ = true;
    return true;
}

bool CameraController::stopPreview() {
    invokeMethod(kChannel, "stopPreview");
    previewing_ = false;
    return true;
}

bool CameraController::setFlashMode(FlashMode mode) {
    auto reply = invokeMethod(kChannel, "setFlashMode", flashModeName(mode));
    return reply && !replyIsError(*reply);
}

bool CameraController::takePicture(const std::string& path) {
    if (state_ != CameraState::Open) {
        emit(CameraEventType::Error, "camera is not open");
        return false;
    }
    auto reply = invokeMethod(kChannel, "takePicture", path);
    if (!reply || replyIsError(*reply)) {
        emit(CameraEventType::Error,
             reply ? *reply : "camera host unavailable");
        return false;
    }
    return true;
}

void CameraController::setEventListener(EventListener listener) {
    listener_ = std::move(listener);
    if (listener_)
        ensureListening();
}

void CameraController::emit(CameraEventType type, const std::string& message) {
    if (!listener_)
        return;
    CameraEvent event;
    event.type = type;
    event.state = state_;
    event.message = message;
    listener_(event);
}

void CameraController::handleEvent(const std::string& event) {
    const auto tab = event.find('\t');
    const std::string kind = tab == std::string::npos ? event : event.substr(0, tab);
    const std::string rest = tab == std::string::npos ? std::string{} : event.substr(tab + 1);

    if (kind == "state") {
        state_ = parseCameraState(rest);
        if (state_ == CameraState::Closed || state_ == CameraState::Error)
            previewing_ = false;
        emit(CameraEventType::StateChanged, {});
    } else if (kind == "error") {
        state_ = CameraState::Error;
        emit(CameraEventType::Error, rest);
    } else if (kind == "saved") {
        emit(CameraEventType::PictureSaved, rest);
    } else if (kind == "frame") {
        emit(CameraEventType::PreviewFrame, rest);
    }
}

}  // namespace services
}  // namespace cui_core
