#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace argus {

struct CameraFrame {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t format = 0;
};

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool Capture() = 0;
    virtual bool GetFrame(CameraFrame& frame) const = 0;
};

struct AttendanceEvent {
    char event_id[40] = {};
    char person_id[32] = {};
    char device_id[32] = {};
    char timestamp[25] = {};
    float similarity = 0.0F;
    bool local_only = false;
};

class FaceAttendance {
public:
    using EventCallback = std::function<void(const AttendanceEvent&)>;

    FaceAttendance(FrameSource* camera, EventCallback callback);
    ~FaceAttendance();

    bool Start();
    void Stop();
    bool EnrollNextFace(const std::string& person_id);
    bool ClearAll();
    int GetEnrolledCount();
    void SetBridgeInfo(const std::string& base_url, const std::string& token, const std::string& device_id);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace argus
