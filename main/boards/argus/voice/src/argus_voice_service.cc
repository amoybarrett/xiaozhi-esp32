#include "argus_voice_service.hpp"

#include "sdkconfig.h"
#include "volc_conv_ai.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include "decoder/esp_audio_dec.h"
#include "decoder/impl/esp_g711_dec.h"
#include "encoder/esp_audio_enc.h"
#include "encoder/impl/esp_g711_enc.h"
#include "esp_ae_rate_cvt.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace argus {
namespace {

#if CONFIG_VOLC_RTC_MODE
static_assert(CONFIG_VOLC_AUDIO_G711A,
              "Argus RTC voice requires the official G.711A high-quality path");
constexpr int kCloudSampleRate = 8000;
constexpr int kFrameMs = 60;
constexpr const char* kTransportName = "high-quality RTC";
#else
static_assert(CONFIG_VOLC_WS_MODE && CONFIG_VOLC_AUDIO_PCM16,
              "Argus voice requires the official low-load WebSocket PCM16 path");
constexpr int kCloudSampleRate = 16000;
constexpr int kFrameMs = 100;
constexpr const char* kTransportName = "low-load WebSocket";
#endif
constexpr const char* kTag = "argus-voice";
constexpr int kFrameSamples = kCloudSampleRate * kFrameMs / 1000;
constexpr int kPlaybackInputSamples = kCloudSampleRate * kFrameMs / 1000;
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
    void* g711_encoder = nullptr;
    void* g711_decoder = nullptr;
    std::vector<uint8_t> encoded;
    std::vector<int16_t> decoded;
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

    void CloseCloudCodec() {
#if CONFIG_VOLC_RTC_MODE
        if (g711_encoder != nullptr) {
            esp_g711_enc_close(g711_encoder);
            g711_encoder = nullptr;
        }
        if (g711_decoder != nullptr) {
            esp_g711_dec_close(g711_decoder);
            g711_decoder = nullptr;
        }
#endif
    }

    bool OpenCloudCodec() {
#if CONFIG_VOLC_RTC_MODE
        esp_g711_enc_config_t encoder_config = {
            .sample_rate = kCloudSampleRate,
            .channel = 1,
            .bits_per_sample = 16,
            .frame_duration = kFrameMs,
        };
        if (esp_g711a_enc_open(&encoder_config, sizeof(encoder_config), &g711_encoder) !=
            ESP_AUDIO_ERR_OK) {
            CloseCloudCodec();
            return false;
        }
        esp_g711_dec_cfg_t decoder_config = {.channel = 1};
        if (esp_g711_dec_open(&decoder_config, sizeof(decoder_config), &g711_decoder) !=
            ESP_AUDIO_ERR_OK) {
            CloseCloudCodec();
            return false;
        }
#endif
        return true;
    }

    bool EncodeG711A(const std::vector<int16_t>& input) {
#if CONFIG_VOLC_RTC_MODE
        if (input.empty() || g711_encoder == nullptr) {
            return false;
        }
        encoded.resize(input.size());
        esp_audio_enc_in_frame_t in_frame = {
            .buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(input.data())),
            .len = static_cast<uint32_t>(input.size() * sizeof(int16_t)),
        };
        esp_audio_enc_out_frame_t out_frame = {
            .buffer = encoded.data(),
            .len = static_cast<uint32_t>(encoded.size()),
        };
        if (esp_g711_enc_process(g711_encoder, &in_frame, &out_frame) != ESP_AUDIO_ERR_OK ||
            out_frame.encoded_bytes == 0 || out_frame.encoded_bytes > encoded.size()) {
            return false;
        }
        encoded.resize(out_frame.encoded_bytes);
        return true;
#else
        (void)input;
        return false;
#endif
    }

    bool DecodeG711A(const uint8_t* input, size_t size) {
#if CONFIG_VOLC_RTC_MODE
        if (input == nullptr || size == 0 || g711_decoder == nullptr) {
            return false;
        }
        decoded.resize(size);
        esp_audio_dec_in_raw_t raw = {
            .buffer = const_cast<uint8_t*>(input),
            .len = static_cast<uint32_t>(size),
        };
        size_t decoded_bytes = 0;
        while (raw.len > 0 && decoded_bytes < decoded.size() * sizeof(int16_t)) {
            uint8_t* output = reinterpret_cast<uint8_t*>(decoded.data()) + decoded_bytes;
            esp_audio_dec_out_frame_t frame = {
                .buffer = output,
                .len = static_cast<uint32_t>(decoded.size() * sizeof(int16_t) - decoded_bytes),
            };
            esp_audio_dec_info_t info = {};
            if (esp_g711a_dec_decode(g711_decoder, &raw, &frame, &info) != ESP_AUDIO_ERR_OK ||
                raw.consumed == 0) {
                return false;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            decoded_bytes += frame.decoded_size;
        }
        decoded.resize(decoded_bytes / sizeof(int16_t));
        return !decoded.empty();
#else
        (void)input;
        (void)size;
        return false;
#endif
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
    if (!impl_->OpenCloudCodec()) {
        ESP_LOGE(kTag, "Failed to create the Volcano cloud audio codec");
        return false;
    }
    if (impl_->sink->sample_rate() != kCloudSampleRate) {
        esp_ae_rate_cvt_cfg_t config = {
            .src_rate = static_cast<uint32_t>(kCloudSampleRate),
            .dest_rate = static_cast<uint32_t>(impl_->sink->sample_rate()),
            .channel = 1,
            .bits_per_sample = 16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        if (esp_ae_rate_cvt_open(&config, &impl_->output_resampler) != ESP_AE_ERR_OK) {
            ESP_LOGE(kTag, "Failed to create Volcano output resampler");
            impl_->CloseOutputResampler();
            impl_->CloseCloudCodec();
            return false;
        }
    }
    if (!impl_->OpenPlaybackStream()) {
        ESP_LOGE(kTag, "Failed to create the PSRAM Volcano playback stream");
        impl_->CloseCloudCodec();
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
                    // The SDK does not emit an initial LISTENING status for a
                    // new session; its initial state is listening.
                    impl->answer_finished.store(false);
                    impl->quiet_input_polls.store(0);
                    impl->input_allowed.store(true);
                    ESP_LOGI(kTag, "Volcano %s connected", kTransportName);
                } else if (event->code == VOLC_EV_DISCONNECTED) {
                    impl->connected.store(false);
                    impl->input_allowed.store(false);
                    impl->answer_finished.store(false);
                    impl->quiet_input_polls.store(0);
                    ESP_LOGW(kTag, "Volcano %s disconnected", kTransportName);
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
            [](volc_engine_t, const void* data, size_t size, volc_audio_frame_info_t* info,
               void* user_data) {
                auto* impl = static_cast<Impl*>(user_data);
                if (impl == nullptr || impl->sink == nullptr || data == nullptr ||
                    impl->playback_stream == nullptr) {
                    return;
                }
#if CONFIG_VOLC_RTC_MODE
                if (info == nullptr || info->data_type != VOLC_AUDIO_DATA_TYPE_G711A ||
                    !impl->DecodeG711A(static_cast<const uint8_t*>(data), size)) {
                    return;
                }
                const void* playback_data = impl->decoded.data();
                const size_t playback_size = impl->decoded.size() * sizeof(int16_t);
#else
                if (info == nullptr || info->data_type != VOLC_AUDIO_DATA_TYPE_PCM ||
                    (size & 1U) != 0U) {
                    return;
                }
                const void* playback_data = data;
                const size_t playback_size = size;
#endif
                const size_t sent =
                    xStreamBufferSend(impl->playback_stream, playback_data, playback_size, 0);
                if (sent != playback_size) {
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

    const std::string iot_config = std::string("{\"instance_id\":\"") + CONFIG_VOLC_INSTANCE_ID +
                                   "\",\"product_key\":\"" + CONFIG_VOLC_PRODUCT_KEY +
                                   "\",\"product_secret\":\"" + CONFIG_VOLC_PRODUCT_SECRET +
                                   "\",\"device_name\":\"" + CONFIG_VOLC_DEVICE_NAME + "\"}";
#if CONFIG_VOLC_RTC_MODE
    const std::string config =
        std::string("{\"ver\":1,\"iot\":") + iot_config +
        ",\"rtc\":{\"log_level\":2,\"audio\":{\"publish\":true,\"subscribe\":true,"
        "\"codec\":3},\"video\":{\"publish\":false,\"subscribe\":false,\"codec\":1},"
        "\"params\":["
        "\"{\\\"rtc\\\":{\\\"access\\\":{\\\"concurrent_requests\\\":1}}}\","
        "\"{\\\"rtc\\\":{\\\"ice\\\":{\\\"concurrent_agents\\\":1}}}\","
        "\"{\\\"rtc\\\":{\\\"thread\\\":{\\\"stack_in_ext\\\":1}}}\","
        "\"{\\\"rtc\\\":{\\\"network\\\":{\\\"enable_audio_jitter2\\\":0}}}\","
        "\"{\\\"rtc\\\":{\\\"report\\\":{\\\"enable\\\":0}}}\","
        "\"{\\\"audio\\\":{\\\"codec\\\":{\\\"pcma\\\":{\\\"s_samples_per_frame\\\":" +
        std::to_string(kFrameSamples) + "}}}}}\"]}}";
#else
    const std::string config =
        std::string("{\"ver\":1,\"iot\":") + iot_config + ",\"ws\":{\"audio\":{\"codec\":4}}}";
#endif

    ESP_LOGI(kTag, "Creating Volcano engine; internal free=%u largest=%u, PSRAM free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    if (volc_create(&impl_->engine, config.c_str(), &impl_->handler, impl_) != 0) {
        ESP_LOGE(kTag, "Failed to create the Volcano %s engine", kTransportName);
        impl_->engine = nullptr;
        impl_->CloseCloudCodec();
        impl_->CloseOutputResampler();
        impl_->ClosePlaybackStream();
        return false;
    }
    ESP_LOGI(kTag, "Volcano engine created; internal free=%u largest=%u, PSRAM free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    volc_opt_t option = {
#if CONFIG_VOLC_RTC_MODE
        .mode = VOLC_MODE_RTC,
#else
        .mode = VOLC_MODE_WS,
#endif
        .bot_id = const_cast<char*>(CONFIG_VOLC_BOT_ID),
        .params = nullptr,
    };
    if (volc_start(impl_->engine, &option) != 0) {
        ESP_LOGE(kTag, "Failed to start the Volcano %s conversation", kTransportName);
        volc_destroy(impl_->engine);
        impl_->engine = nullptr;
        impl_->CloseCloudCodec();
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
    ESP_LOGI(kTag, "Volcano %s intercom started (%s)", kTransportName, volc_get_version());
    return true;
}

void VoiceService::InputTask() {
    const int channels = std::max(1, impl_->source->channels());
    const size_t input_samples = static_cast<size_t>(kFrameSamples * impl_->source->sample_rate() /
                                                     kCloudSampleRate * channels);
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
        Resample(mono, impl_->source->sample_rate(), kCloudSampleRate, pcm);
#if CONFIG_VOLC_RTC_MODE
        if (!impl_->EncodeG711A(pcm)) {
            continue;
        }
        volc_audio_frame_info_t frame = {
            .data_type = VOLC_AUDIO_DATA_TYPE_G711A,
            .commit = false,
        };
        volc_send_audio_data(impl_->engine, impl_->encoded.data(), impl_->encoded.size(), &frame);
#else
        const size_t count = std::min(pcm.size(), static_cast<size_t>(kFrameSamples));
        volc_audio_frame_info_t frame = {
            .data_type = VOLC_AUDIO_DATA_TYPE_PCM,
            .commit = false,
        };
        volc_send_audio_data(impl_->engine, pcm.data(), count * sizeof(int16_t), &frame);
#endif
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
    impl_->CloseCloudCodec();
    impl_->ClosePlaybackStream();
    ESP_LOGI(kTag, "Volcano %s intercom stopped", kTransportName);
}

}  // namespace argus
