#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

namespace argus {

class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual int sample_rate() const = 0;
    virtual int channels() const = 0;
    virtual bool Read(std::vector<int16_t>& samples) = 0;
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual int sample_rate() const = 0;
    virtual void Write(const std::vector<int16_t>& samples) = 0;
};

class VoiceService {
public:
    VoiceService();
    ~VoiceService();

    bool Start(AudioSource* source, AudioSink* sink);
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    void InputTask();

    struct Impl;
    Impl* impl_;
    std::atomic<bool> running_{false};
};

}  // namespace argus
