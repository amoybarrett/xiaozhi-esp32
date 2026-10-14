#include "argus_voice_service.hpp"

#include "sdkconfig.h"
#include "volc_conv_ai.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace argus {
namespace {

constexpr const char* kTag = "argus-voice";
constexpr int kPcmSampleRate = 16000;
constexpr int kFrameSamples = 1600;  // 100 ms, matching the official low-load demo.

std::vector<int16_t> Resample(const std::vector<int16_t>& input, int input_rate, int output_rate) {
    if (input.empty() || input_rate <= 0 || output_rate <= 0 || input_rate == output_rate) {
        return input;
    }
    const double step = static_cast<double>(input_rate) / static_cast<double>(output_rate);
    const size_t output_size = std::max<size_t>(1, input.size() * output_rate / input_rate);
    std::vector<int16_t> output;
    output.reserve(output_size);
    for (size_t i = 0; i < output_size; ++i) {
        const double position = i * step;
        const size_t left = std::min(static_cast<size_t>(position), input.size() - 1);
        const size_t right = std::min(left + 1, input.size() - 1);
        const double fraction = position - static_cast<double>(left);
        const double value = input[left] * (1.0 - fraction) + input[right] * fraction;
        output.push_back(static_cast<int16_t>(std::lround(value)));
    }
    return output;
}

}  // namespace

struct VoiceService::Impl {
    AudioSource* source = nullptr;
    AudioSink* sink = nullptr;
    volc_engine_t engine = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<bool> connected{false};
    volc_event_handler_t handler{};
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
    if (std::string_view(CONFIG_VOLC_INSTANCE_ID).empty() ||
        std::string_view(CONFIG_VOLC_PRODUCT_KEY).empty() ||
        std::string_view(CONFIG_VOLC_PRODUCT_SECRET).empty() ||
        std::string_view(CONFIG_VOLC_BOT_ID).empty()) {
        ESP_LOGW(kTag, "Volcano InstanceID/ProductKey/ProductSecret/BotID is not configured");
        return false;
    }

    impl_->source = source;
    impl_->sink = sink;
    impl_->connected.store(false);
    impl_->handler = {
        .on_volc_event =
            [](volc_engine_t, volc_event_t* event, void* user_data) {
                auto* impl = static_cast<Impl*>(user_data);
                if (impl == nullptr || event == nullptr) {
                    return;
                }
                if (event->code == VOLC_EV_CONNECTED) {
                    impl->connected.store(true);
                    ESP_LOGI(kTag, "Volcano low-load WebSocket connected");
                } else if (event->code == VOLC_EV_DISCONNECTED) {
                    impl->connected.store(false);
                    ESP_LOGW(kTag, "Volcano low-load WebSocket disconnected");
                }
            },
        .on_volc_conversation_status =
            [](volc_engine_t, volc_conv_status_e status, void*) {
                ESP_LOGI(kTag, "Volcano conversation status: %d", static_cast<int>(status));
            },
        .on_volc_audio_data =
            [](volc_engine_t, const void* data, size_t size, volc_audio_frame_info_t*, void* user_data) {
                auto* impl = static_cast<Impl*>(user_data);
                if (impl == nullptr || impl->sink == nullptr || data == nullptr ||
                    (size & 1U) != 0U) {
                    return;
                }
                const auto* input = static_cast<const int16_t*>(data);
                std::vector<int16_t> decoded(input, input + size / sizeof(int16_t));
                impl->sink->Write(Resample(decoded, kPcmSampleRate, impl->sink->sample_rate()));
            },
        .on_volc_video_data = [](volc_engine_t, const void*, size_t, volc_video_frame_info_t*, void*) {},
        .on_volc_message_data = [](volc_engine_t, const void*, size_t, volc_message_info_t*, void*) {},
    };

    char config[512] = {};
    std::snprintf(config, sizeof(config),
                  "{\"ver\":1,\"iot\":{\"instance_id\":\"%s\",\"product_key\":\"%s\","
                  "\"product_secret\":\"%s\",\"device_name\":\"%s\"},"
                  "\"ws\":{\"audio\":{\"codec\":4}}}",
                  CONFIG_VOLC_INSTANCE_ID, CONFIG_VOLC_PRODUCT_KEY, CONFIG_VOLC_PRODUCT_SECRET,
                  CONFIG_VOLC_DEVICE_NAME);

    if (volc_create(&impl_->engine, config, &impl_->handler, impl_) != 0) {
        ESP_LOGE(kTag, "Failed to create Volcano low-load engine");
        impl_->engine = nullptr;
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
        return false;
    }

    running_.store(true);
    if (xTaskCreate([](void* arg) { static_cast<VoiceService*>(arg)->InputTask(); },
                    "argus_voice_in", 6144, this, 5, &impl_->task) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create audio input task");
        Stop();
        return false;
    }
    ESP_LOGI(kTag, "Volcano low-load intercom started (%s)", volc_get_version());
    return true;
}

void VoiceService::InputTask() {
    const int channels = std::max(1, impl_->source->channels());
    const size_t input_samples = static_cast<size_t>(
        kFrameSamples * impl_->source->sample_rate() / kPcmSampleRate * channels);
    std::vector<int16_t> input;
    std::vector<int16_t> mono;

    while (running_.load()) {
        for (int waited = 0; running_.load() && !impl_->connected.load() && waited < 100; ++waited) {
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
        mono.clear();
        mono.reserve(input.size() / channels);
        for (size_t i = 0; i + channels <= input.size(); i += channels) {
            mono.push_back(input[i]);
        }
        const std::vector<int16_t> pcm = Resample(mono, impl_->source->sample_rate(), kPcmSampleRate);
        const size_t count = std::min(pcm.size(), static_cast<size_t>(kFrameSamples));
        volc_audio_frame_info_t frame = {
            .data_type = VOLC_AUDIO_DATA_TYPE_PCM,
            .commit = false,
        };
        volc_send_audio_data(impl_->engine, pcm.data(), count * sizeof(int16_t), &frame);
    }

    impl_->task = nullptr;
    vTaskDelete(nullptr);
}

void VoiceService::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    for (int i = 0; i < 100 && impl_->task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (impl_->engine != nullptr) {
        volc_stop(impl_->engine);
        volc_destroy(impl_->engine);
        impl_->engine = nullptr;
    }
    ESP_LOGI(kTag, "Volcano low-load intercom stopped");
}

}  // namespace argus
