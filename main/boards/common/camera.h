#ifndef CAMERA_H
#define CAMERA_H

#include <expected>
#include <string>
#include <cstddef>
#include <cstdint>

struct CameraFrame {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t format = 0;
};

class Camera {
public:
    virtual ~Camera() = default;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::expected<std::string, std::string> Explain(const std::string& question) = 0;
    virtual bool GetFrame(CameraFrame& frame) const { return false; }
};

#endif  // CAMERA_H
