#include "argus_voice_service.hpp"

#include "sdkconfig.h"
#include "volc_conv_ai.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include "esp_ae_rate_cvt.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace argus {
namespace {

static_assert(CONFIG_VOLC_WS_MODE && CONFIG_VOLC_AUDIO_PCM16,
              "Argus voice currently supports only the official low-load WebSocket PCM16 path");
constexpr const char* kTag = "argus-voice";
constexpr int kPcmSampleRate = 16000;
constexpr int kFrameSamples = 1600;         // 100 ms, the officially demonstrated low-load cadence.
constexpr int kPlaybackInputSamples = 960;  // 60 ms at 16 kHz, matching Xiaozhi playback.
constexpr size_t kPlaybackBufferSize = 64 * 1024;
constexpr int kResumeQuietPolls = 5;

bool IsValidCredential(std::string_view value) {
    return !value.empty() && value.size() < 128 &&
           std::all_of(
               value.begin(), value.end(),
               [](char ch) { return ch >= 0x21 && ch <= 0x7E && ch != '"' && ch != '\\'; });
}

void Resample(const std::vector<int16_t>& input, int input_rate, int output_rate,
              std::vector<int16_t>& output) {
    if (input.empty() || input_rate <= 0 || output_rate <= 0 || input_rate == output_rate) {
        output = input;
        return;
    }
    const double step = static_cast<double>(input_rate) / static_cast<double>(output_rate);
    const size_t output_size = std::max<size_t>(1, input.size() * output_rate / input_rate);
    output.clear();
    output.reserve(output_size);
    for (size_t i = 0; i < output_size; ++i) {
        const double position = i * step;
        const size_t left = std::min(static_cast<size_t>(position), input.size() - 1);
        const size_t right = std::min(left + 1, input.size() - 1);
        const double fraction = position - static_cast<double>(left);
        const double value = input[left] * (1.0 - fraction) + input[right] * fraction;
        output.push_back(static_cast<int16_t>(std::lround(value)));
    }
}

}  // namespace

struct VoiceService::Impl {
    AudioSource* source = nullptr;
    AudioSink* sink = nullptr;
    volc_engine_t engine = nullptr;
    TaskHandle_t input_task = nullptr;
    TaskHandle_t playback_task = nullptr;
    StreamBufferHandle_t playback_stream = nullptr;
    StaticStreamBuffer_t playback_stream_storage{};
    uint8_t* playback_buffer = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler = nullptr;
    std::atomic<bool> connected{false};
    std::atomic<bool> input_allowed{true};
    std::atomic<bool> answer_finished{false};
    std::atomic<int> quiet_input_polls{0};
    std::atomic<uint32_t> received_audio_frames{0};
    std::atomic<uint32_t> dropped_audio_chunks{0};
    volc_event_handler_t handler{};

    void CloseOutputResampler() {
        if (output_resampler == nullptr) {
            return;
        }
        esp_ae_rate_cvt_close(output_resampler);
        output_resampler = nullptr;
    }

    bool WritePlaybackChunk(std::vector<int16_t>& input, std::vector<int16_t>& output) {
        if (input.empty() || sink == nullptr) {
            return false;
        }
        if (output_resampler == nullptr) {
            sink->Write(input);
            return true;
        }
        uint32_t output_capacity = 0;
        if (esp_ae_rate_cvt_get_max_out_sample_num(output_resampler,
                                                   static_cast<uint32_t>(input.size()),
                                                   &output_capacity) != ESP_AE_ERR_OK ||
            output_capacity == 0) {
            return false;
        }
        output.resize(output_capacity);
        uint32_t output_count = output_capacity;
        if (esp_ae_rate_cvt_process(output_resampler, const_cast<int16_t*>(input.data()),
                                    static_cast<uint32_t>(input.size()), output.data(),
                                    &output_count) != ESP_AE_ERR_OK) {
            return false;
        }
        output.resize(output_count);
        sink->Write(output);
        return true;
    }

    void ClosePlaybackStream() {
        if (playback_stream != nullptr) {
            vStreamBufferDelete(playback_stream);
            playback_stream = nullptr;
        }
        if (playback_buffer != nullptr) {
            heap_caps_free(playback_buffer);
            playback_buffer = nullptr;
        }
    }

    bool OpenPlaybackStream() {
        playback_buffer = static_cast<uint8_t*>(
            heap_caps_malloc(kPlaybackBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (playback_buffer == nullptr) {
            return false;
        }
        playback_stream = xStreamBufferCreateStatic(kPlaybackBufferSize, 1, playback_buffer,
                                                    &playback_stream_storage);
        if (playback_stream == nullptr) {
            ClosePlaybackStream();
            return false;
        }
        return true;
    }
};

VoiceService::VoiceService() : impl_(new Impl()) {}

VoiceService::~VoiceService() {
    Stop();
    delete impl_;
}

bool VoiceService::Start(AudioSource* source, AudioSink* sink) {
    if (running_.load() || source == nullptr || sink == nullptr) {
        return false;
    }
    if (!IsValidCredential(CONFIG_VOLC_INSTANCE_ID) ||
        !IsValidCredential(CONFIG_VOLC_PRODUCT_KEY) ||
        !IsValidCredential(CONFIG_VOLC_PRODUCT_SECRET) || !IsValidCredential(CONFIG_VOLC_BOT_ID) ||
        !IsValidCredential(CONFIG_VOLC_DEVICE_NAME)) {
        ESP_LOGW(kTag, "Volcano InstanceID/ProductKey/ProductSecret/BotID is not configured");
        return false;
    }

    impl_->source = source;
    impl_->sink = sink;
    impl_->connected.store(false);
    // A newly created low-load session starts in listening mode. The socket
    // callback below enables upload once the connection is ready.
    impl_->input_allowed.store(false);
    impl_->answer_finished.store(false);
    impl_->quiet_input_polls.store(0);
    impl_->received_audio_frames.store(0);
    impl_->dropped_audio_chunks.store(0);
    impl_->CloseOutputResampler();
    if (impl_->sink->sample_rate() != kPcmSampleRate) {
        esp_ae_rate_cvt_cfg_t config = {
            .src_rate = static_cast<uint32_t>(kPcmSampleRate),
            .dest_rate = static_cast<uint32_t>(impl_->sink->sample_rate()),
            .channel = 1,
            .bits_per_sample = 16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        if (esp_ae_rate_cvt_open(&config, &impl_->output_resampler) != ESP_AE_ERR_OK) {
            ESP_LOGE(kTag, "Failed to create Volcano output resampler");
            impl_->CloseOutputResampler();
            return false;
        }
    }
    if (!impl_->OpenPlaybackStream()) {
        ESP_LOGE(kTag, "Failed to create the PSRAM Volcano playback stream");
        impl_->CloseOutputResampler();
        return false;
    }
    impl_->handler = {
        .on_volc_event =
            [](volc_engine_t, volc_event_t* event, void* user_data) {
                auto* impl = static_cast<Impl*>(user_data);
                if (impl == nullptr || event == nullptr) {
                    return;
                }
                if (event->code == VOLC_EV_CONNECTED) {
                    impl->connected.store(true);
                    // The low-load SDK does not emit an initial LISTENING
                    // status for a new session; its initial state is listening.
                    impl->answer_finished.store(false);
                    impl->quiet_input_polls.store(0);
                    impl->input_allowed.store(true);
                    ESP_LOGI(kTag, "Volcano low-load WebSocket connected");
                } else if (event->code == VOLC_EV_DISCONNECTED) {
                    impl->connected.store(false);
                    impl->input_allowed.store(false);
                    impl->answer_finished.store(false);
                    impl->quiet_input_polls.store(0);
                    ESP_LOGW(kTag, "Volcano low-load WebSocket disconnected");
                }
            },
        .on_volc_conversation_status =
            [](volc_engine_t, volc_conv_status_e status, void* user_data) {
                auto* impl = static_cast<Impl*>(user_data);
                const char* name = "unknown";
                switch (status) {
                    case VOLC_CONV_STATUS_LISTENING:
                        name = "listening";
                        if (impl != nullptr) {
                            impl->answer_finished.store(false);
                            impl->quiet_input_polls.store(0);
                            impl->input_allowed.store(true);
                        }
                        break;
                    case VOLC_CONV_STATUS_THINKING:
                        name = "thinking";
                        if (impl != nullptr) {
                            impl->input_allowed.store(false);
                        }
                        break;
                    case VOLC_CONV_STATUS_ANSWERING:
                        name = "answering";
                        if (impl != nullptr) {
                            impl->input_allowed.store(false);
                        }
                        break;
                    case VOLC_CONV_STATUS_INTERRUPTED:
                        name = "interrupted";
                        if (impl != nullptr) {
                            impl->input_allowed.store(false);
                        }
                        break;
                    case VOLC_CONV_STATUS_ANSWER_FINISH:
                        name = "answer_finish";
                        if (impl != nullptr) {
                            impl->input_allowed.store(false);
                            impl->answer_finished.store(true);
                            impl->quiet_input_polls.store(0);
                        }
                        break;
                    default:
                        break;
                }
                ESP_LOGI(kTag, "Volcano conversation status: %s (%d)", name,
                         static_cast<int>(status));
            },
        .on_volc_audio_data =
            [](volc_engine_t, const void* data, size_t size, volc_audio_frame_info_t*,
               void* user_data) {
                auto* impl = static_cast<Impl*>(user_data);
                if (impl == nullptr || impl->sink == nullptr || data == nullptr ||
                    impl->playback_stream == nullptr || (size & 1U) != 0U) {
                    return;
                }
                const size_t sent = xStreamBufferSend(impl->playback_stream, data, size, 0);
                if (sent != size) {
                    const uint32_t drops = ++impl->dropped_audio_chunks;
                    if (drops == 1) {
                        ESP_LOGW(kTag, "Volcano playback stream is full");
                    }
                    return;
                }
                // A late audio delta can arrive around the response-done event.
                // Keep the response active so microphone upload does not resume
                // while its chunks are still entering the playback stream.
                impl->answer_finished.store(false);
                const uint32_t frames = ++impl->received_audio_frames;
                if (frames == 1 || frames % 100 == 0) {
                    ESP_LOGI(kTag, "Received Volcano audio frame count: %u",
                             static_cast<unsigned>(frames));
                }
            },
        .on_volc_video_data = [](volc_engine_t, const void*, size_t, volc_video_frame_info_t*,
                                 void*) {},
        .on_volc_message_data = [](volc_engine_t, const void*, size_t, volc_message_info_t*,
                                   void*) {},
    };

    const std::string config = std::string("{\"ver\":1,\"iot\":{\"instance_id\":\"") +
                               CONFIG_VOLC_INSTANCE_ID + "\",\"product_key\":\"" +
                               CONFIG_VOLC_PRODUCT_KEY + "\",\"product_secret\":\"" +
                               CONFIG_VOLC_PRODUCT_SECRET + "\",\"device_name\":\"" +
                               CONFIG_VOLC_DEVICE_NAME + "\"},\"ws\":{\"audio\":{\"codec\":4}}}";

    if (volc_create(&impl_->engine, config.c_str(), &impl_->handler, impl_) != 0) {
        ESP_LOGE(kTag, "Failed to create Volcano low-load engine");
        impl_->engine = nullptr;
        impl_->CloseOutputResampler();
        impl_->ClosePlaybackStream();
        return false;
    }
    volc_opt_t option = {
        .mode = VOLC_MODE_WS,
        .bot_id = const_cast<char*>(CONFIG_VOLC_BOT_ID),
        .params = nullptr,
    };
    if (volc_start(impl_->engine, &option) != 0) {
        ESP_LOGE(kTag, "Failed to start Volcano low-load conversation");
        volc_destroy(impl_->engine);
        impl_->engine = nullptr;
        impl_->CloseOutputResampler();
        impl_->ClosePlaybackStream();
        return false;
    }

    running_.store(true);
    if (xTaskCreate([](void* arg) { static_cast<VoiceService*>(arg)->PlaybackTask(); },
                    "argus_voice_out", 16384, this, 4, &impl_->playback_task) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create audio playback task");
        Stop();
        return false;
    }
    if (xTaskCreate([](void* arg) { static_cast<VoiceService*>(arg)->InputTask(); },
                    "argus_voice_in", 6144, this, 5, &impl_->input_task) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create audio input task");
        Stop();
        return false;
    }
    ESP_LOGI(kTag, "Volcano low-load intercom started (%s)", volc_get_version());
    return true;
}

void VoiceService::InputTask() {
    const int channels = std::max(1, impl_->source->channels());
    const size_t input_samples = static_cast<size_t>(kFrameSamples * impl_->source->sample_rate() /
                                                     kPcmSampleRate * channels);
    std::vector<int16_t> input;
    std::vector<int16_t> mono;
    std::vector<int16_t> pcm;

    while (running_.load()) {
        while (running_.load() && !impl_->connected.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!running_.load()) {
            break;
        }

        input.assign(input_samples, 0);
        if (!impl_->source->Read(input)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!running_.load()) {
            break;
        }
        if (!impl_->input_allowed.load()) {
            // Half-duplex mode: keep draining the microphone to discard stale
            // samples, but do not upload the device's own speaker output.
            if (impl_->answer_finished.load()) {
                if (xStreamBufferBytesAvailable(impl_->playback_stream) == 0) {
                    if (++impl_->quiet_input_polls >= kResumeQuietPolls) {
                        impl_->answer_finished.store(false);
                        impl_->quiet_input_polls.store(0);
                        impl_->input_allowed.store(true);
                        ESP_LOGI(kTag, "Volcano playback drained; microphone upload resumed");
                    }
                } else {
                    impl_->quiet_input_polls.store(0);
                }
            }
            continue;
        }
        mono.clear();
        mono.reserve(input.size() / channels);
        for (size_t i = 0; i + channels <= input.size(); i += channels) {
            mono.push_back(input[i]);
        }
        Resample(mono, impl_->source->sample_rate(), kPcmSampleRate, pcm);
        const size_t count = std::min(pcm.size(), static_cast<size_t>(kFrameSamples));
        volc_audio_frame_info_t frame = {
            .data_type = VOLC_AUDIO_DATA_TYPE_PCM,
            .commit = false,
        };
        volc_send_audio_data(impl_->engine, pcm.data(), count * sizeof(int16_t), &frame);
    }

    impl_->input_task = nullptr;
    vTaskDelete(nullptr);
}

void VoiceService::PlaybackTask() {
    std::vector<int16_t> pending(kPlaybackInputSamples);
    std::vector<int16_t> output;
    size_t pending_bytes = 0;
    while (true) {
        const size_t received_bytes = xStreamBufferReceive(
            impl_->playback_stream, pending.data() + pending_bytes / sizeof(int16_t),
            pending.size() * sizeof(int16_t) - pending_bytes, pdMS_TO_TICKS(100));
        pending_bytes += received_bytes;
        if (pending_bytes == pending.size() * sizeof(int16_t)) {
            impl_->WritePlaybackChunk(pending, output);
            pending_bytes = 0;
        }
        if (received_bytes == 0 && !running_.load()) {
            break;
        }
    }
    pending.clear();
    impl_->playback_task = nullptr;
    vTaskDelete(nullptr);
}

void VoiceService::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    int input_waits = 0;
    while (impl_->input_task != nullptr) {
        if (++input_waits == 100) {
            ESP_LOGW(kTag, "Waiting for the audio input task to stop");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (impl_->engine != nullptr) {
        volc_stop(impl_->engine);
        volc_destroy(impl_->engine);
        impl_->engine = nullptr;
    }
    int playback_waits = 0;
    while (impl_->playback_task != nullptr) {
        if (++playback_waits == 100) {
            ESP_LOGW(kTag, "Waiting for the audio playback task to stop");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    impl_->CloseOutputResampler();
    impl_->ClosePlaybackStream();
    ESP_LOGI(kTag, "Volcano low-load intercom stopped");
}

}  // namespace argus
