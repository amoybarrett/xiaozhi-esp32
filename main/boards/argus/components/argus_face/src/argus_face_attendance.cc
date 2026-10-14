#include "argus_face_attendance.hpp"

#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"

#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_spiffs.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <linux/videodev2.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <unordered_map>
#include <string>

namespace argus {
namespace {

constexpr const char* kTag = "argus-face";
constexpr const char* kStoragePartition = "argus";
constexpr const char* kDatabasePath = "/spiffs/face.db";
constexpr const char* kNamespace = "argus_face";
constexpr uint32_t kCooldownMs = 180000;
constexpr TickType_t kPollPeriod = pdMS_TO_TICKS(1000);

bool IsValidPersonId(const std::string& person_id) {
    if (person_id.empty() || person_id.size() >= 32) {
        return false;
    }
    return std::all_of(person_id.begin(), person_id.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_';
    });
}

std::string MakeEventId() {
    uint8_t random[8] = {};
    esp_fill_random(random, sizeof(random));
    char value[sizeof(random) * 2 + 1] = {};
    for (size_t i = 0; i < sizeof(random); ++i) {
        std::snprintf(value + i * 2, 3, "%02x", random[i]);
    }
    return value;
}

std::string MakeTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm_value{};
    gmtime_r(&now, &tm_value);
    char value[25] = {};
    std::strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &tm_value);
    return value;
}

}  // namespace

struct FaceAttendance::Impl {
    FrameSource* camera = nullptr;
    EventCallback callback;
    SemaphoreHandle_t mutex = nullptr;
    QueueHandle_t event_queue = nullptr;
    TaskHandle_t worker = nullptr;
    TaskHandle_t uploader = nullptr;
    bool running = false;
    std::string pending_person;
    std::string bridge_url;
    std::string bridge_token;
    std::string device_id = "k10";
    std::unique_ptr<HumanFaceDetect> detector;
    std::unique_ptr<HumanFaceRecognizer> recognizer;
    std::unordered_map<uint16_t, int64_t> last_seen;

    void RunWorker();
    void RunUploader();
    bool IsRunning();
};

FaceAttendance::FaceAttendance(FrameSource* camera, EventCallback callback) : impl_(new Impl()) {
    impl_->camera = camera;
    impl_->callback = std::move(callback);
    impl_->mutex = xSemaphoreCreateMutex();
    impl_->event_queue = xQueueCreate(8, sizeof(AttendanceEvent));
}

FaceAttendance::~FaceAttendance() {
    Stop();
    if (impl_->event_queue != nullptr) {
        vQueueDelete(impl_->event_queue);
    }
    if (impl_->mutex != nullptr) {
        vSemaphoreDelete(impl_->mutex);
    }
    delete impl_;
}

bool FaceAttendance::Start() {
    if (impl_->camera == nullptr || impl_->event_queue == nullptr || impl_->mutex == nullptr) {
        return false;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_FALSE(nvs_flash_erase() == ESP_OK, false, kTag, "Cannot erase NVS");
        ESP_RETURN_ON_FALSE(nvs_flash_init() == ESP_OK, false, kTag, "Cannot initialize NVS");
    } else if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Cannot initialize NVS: %s", esp_err_to_name(err));
        return false;
    }

    const esp_vfs_spiffs_conf_t config = {
        .base_path = "/spiffs",
        .partition_label = kStoragePartition,
        .max_files = 2,
        .format_if_mount_failed = true,
    };
    err = esp_vfs_spiffs_register(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Cannot mount Argus storage: %s", esp_err_to_name(err));
        return false;
    }

    xSemaphoreTake(impl_->mutex, portMAX_DELAY);
    if (impl_->detector == nullptr) {
        impl_->detector = std::make_unique<HumanFaceDetect>();
    }
    if (impl_->recognizer == nullptr) {
        impl_->recognizer = std::make_unique<HumanFaceRecognizer>(kDatabasePath);
    }
    const bool already_running = impl_->running;
    impl_->running = true;
    xSemaphoreGive(impl_->mutex);
    if (already_running) {
        return true;
    }

    if (xTaskCreate([](void* arg) { static_cast<Impl*>(arg)->RunWorker(); }, "argus_face", 12288, impl_, 4,
                    &impl_->worker) != pdPASS) {
        impl_->running = false;
        return false;
    }
    if (xTaskCreate([](void* arg) { static_cast<Impl*>(arg)->RunUploader(); }, "argus_upload", 6144, impl_, 3,
                    &impl_->uploader) != pdPASS) {
        Stop();
        return false;
    }
    ESP_LOGI(kTag, "Local face attendance started");
    return true;
}

bool FaceAttendance::Impl::IsRunning() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool value = running;
    xSemaphoreGive(mutex);
    return value;
}

void FaceAttendance::Impl::RunWorker() {
    while (IsRunning()) {
        std::string pending;
        xSemaphoreTake(mutex, portMAX_DELAY);
        pending.swap(pending_person);
        std::string upload_url = bridge_url;
        std::string upload_token = bridge_token;
        std::string upload_device = device_id;
        xSemaphoreGive(mutex);

        CameraFrame frame{};
        if (camera == nullptr || !camera->Capture() || !camera->GetFrame(frame) ||
            frame.format != V4L2_PIX_FMT_RGB565) {
            vTaskDelay(kPollPeriod);
            continue;
        }

        dl::image::img_t image = {
            .data = const_cast<uint8_t*>(frame.data),
            .width = frame.width,
            .height = frame.height,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        };
        auto detect_results = detector->run(image);

        AttendanceEvent event{};
        std::string event_id = MakeEventId();
        std::string timestamp = MakeTimestamp();
        std::snprintf(event.event_id, sizeof(event.event_id), "%s", event_id.c_str());
        std::snprintf(event.timestamp, sizeof(event.timestamp), "%s", timestamp.c_str());
        std::snprintf(event.device_id, sizeof(event.device_id), "%s", upload_device.c_str());

        if (!pending.empty()) {
            if (detect_results.empty()) {
                ESP_LOGW(kTag, "Enrollment waiting for a visible face: %s", pending.c_str());
                xSemaphoreTake(mutex, portMAX_DELAY);
                pending_person = pending;
                xSemaphoreGive(mutex);
            } else {
                xSemaphoreTake(mutex, portMAX_DELAY);
                const esp_err_t enrolled = recognizer->enroll(image, detect_results);
                const int count = recognizer->get_num_feats();
                xSemaphoreGive(mutex);
                if (enrolled == ESP_OK && count > 0) {
                    const uint16_t record_id = static_cast<uint16_t>(count);
                    nvs_handle_t handle = 0;
                    if (nvs_open(kNamespace, NVS_READWRITE, &handle) == ESP_OK) {
                        char key[12] = {};
                        std::snprintf(key, sizeof(key), "p%u", record_id);
                        nvs_set_str(handle, key, pending.c_str());
                        nvs_commit(handle);
                        nvs_close(handle);
                    }
                    std::snprintf(event.person_id, sizeof(event.person_id), "%s", pending.c_str());
                    event.similarity = 1.0F;
                    ESP_LOGI(kTag, "Enrolled %s as record %u", pending.c_str(), record_id);
                } else {
                    ESP_LOGE(kTag, "Face enrollment failed: %s", esp_err_to_name(enrolled));
                }
            }
        } else if (!detect_results.empty()) {
            xSemaphoreTake(mutex, portMAX_DELAY);
            auto results = recognizer->recognize(image, detect_results);
            xSemaphoreGive(mutex);
            if (!results.empty()) {
                const uint16_t record_id = results.front().id;
                const int64_t now = esp_timer_get_time() / 1000;
                auto last = last_seen.find(record_id);
                if (last == last_seen.end() || now - last->second >= kCooldownMs) {
                    last_seen[record_id] = now;
                    nvs_handle_t handle = 0;
                    char key[12] = {};
                    std::snprintf(key, sizeof(key), "p%u", record_id);
                    if (nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK) {
                        size_t size = sizeof(event.person_id);
                        nvs_get_str(handle, key, event.person_id, &size);
                        nvs_close(handle);
                    }
                    if (event.person_id[0] != '\0') {
                        event.similarity = results.front().similarity;
                        if (event_queue != nullptr) {
                            xQueueSend(event_queue, &event, 0);
                        }
                        if (callback) {
                            callback(event);
                        }
                    }
                }
            }
        }

        vTaskDelay(kPollPeriod);
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (worker == xTaskGetCurrentTaskHandle()) {
        worker = nullptr;
    }
    xSemaphoreGive(mutex);
    vTaskDelete(nullptr);
}

void FaceAttendance::Impl::RunUploader() {
    while (IsRunning()) {
        AttendanceEvent event{};
        if (xQueueReceive(event_queue, &event, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }
        if (event.event_id[0] == '\0') {
            continue;
        }

        xSemaphoreTake(mutex, portMAX_DELAY);
        std::string url = bridge_url;
        std::string token = bridge_token;
        xSemaphoreGive(mutex);
        if (url.empty() || token.empty()) {
            continue;
        }
        while (!url.empty() && url.back() == '/') {
            url.pop_back();
        }
        url += "/attendance/event";

        char payload[512] = {};
        std::snprintf(payload, sizeof(payload),
                      "{\"event_id\":\"%s\",\"person_id\":\"%s\",\"device_id\":\"%s\",\"timestamp\":\"%s\",\"kind\":\"check_in\",\"confidence\":%.3f}",
                      event.event_id, event.person_id, event.device_id, event.timestamp, event.similarity);
        char auth[160] = {};
        std::snprintf(auth, sizeof(auth), "Bearer %s", token.c_str());

        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = 4000;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            ESP_LOGE(kTag, "Cannot create attendance HTTP client");
            continue;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_open(client, static_cast<int>(std::strlen(payload)));
        esp_http_client_write(client, payload, static_cast<int>(std::strlen(payload)));
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(kTag, "Attendance upload failed with HTTP %d", status);
        }
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (uploader == xTaskGetCurrentTaskHandle()) {
        uploader = nullptr;
    }
    xSemaphoreGive(mutex);
    vTaskDelete(nullptr);
}

void FaceAttendance::Stop() {
    if (impl_ == nullptr) {
        return;
    }
    xSemaphoreTake(impl_->mutex, portMAX_DELAY);
    impl_->running = false;
    xSemaphoreGive(impl_->mutex);

    for (int i = 0; i < 100 && (impl_->worker != nullptr || impl_->uploader != nullptr); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_vfs_spiffs_unregister(kStoragePartition);
}

bool FaceAttendance::EnrollNextFace(const std::string& person_id) {
    if (!IsValidPersonId(person_id)) {
        return false;
    }
    xSemaphoreTake(impl_->mutex, portMAX_DELAY);
    const bool accepted = impl_->running;
    if (accepted) {
        impl_->pending_person = person_id;
    }
    xSemaphoreGive(impl_->mutex);
    return accepted;
}

bool FaceAttendance::ClearAll() {
    xSemaphoreTake(impl_->mutex, portMAX_DELAY);
    const bool success = impl_->recognizer != nullptr && impl_->recognizer->clear_all_feats() == ESP_OK;
    xSemaphoreGive(impl_->mutex);
    if (!success) {
        return false;
    }
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    return true;
}

int FaceAttendance::GetEnrolledCount() {
    xSemaphoreTake(impl_->mutex, portMAX_DELAY);
    const int count = impl_->recognizer == nullptr ? 0 : impl_->recognizer->get_num_feats();
    xSemaphoreGive(impl_->mutex);
    return count;
}

void FaceAttendance::SetBridgeInfo(const std::string& base_url, const std::string& token,
                                   const std::string& device_id) {
    if (device_id.empty() || !IsValidPersonId(device_id) ||
        device_id.size() >= sizeof(AttendanceEvent::device_id)) {
        return;
    }
    xSemaphoreTake(impl_->mutex, portMAX_DELAY);
    impl_->bridge_url = base_url;
    impl_->bridge_token = token;
    impl_->device_id = device_id;
    xSemaphoreGive(impl_->mutex);
}

}  // namespace argus
