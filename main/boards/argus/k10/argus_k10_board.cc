#include "application.h"
#include "argus_face_attendance.hpp"
#include "argus_k10_audio_codec.h"
#include "argus_voice_service.hpp"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp_lcd_ili9341.h"
#include "esp_video.h"
#include "led_control.h"
#include "mcp_server.h"
#include "settings.h"
#include "wifi_board.h"

#include "assets/lang_config.h"
#include "led/circular_strip.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <atomic>

#include "esp_io_expander_tca95xx_16bit.h"

#define TAG "ARGUS-K10"

class ArgusK10FrameSource : public argus::FrameSource {
private:
    EspVideo* camera_;

public:
    explicit ArgusK10FrameSource(EspVideo* camera) : camera_(camera) {}

    bool Capture() override { return camera_ != nullptr && camera_->Capture(); }

    bool GetFrame(argus::CameraFrame& output) const override {
        if (camera_ == nullptr) {
            return false;
        }
        CameraFrame frame;
        if (!camera_->GetFrame(frame)) {
            return false;
        }
        output.data = frame.data;
        output.size = frame.size;
        output.width = frame.width;
        output.height = frame.height;
        output.format = frame.format;
        return true;
    }
};

class ArgusK10AudioSource : public argus::AudioSource {
private:
    AudioCodec* codec_;

public:
    explicit ArgusK10AudioSource(AudioCodec* codec) : codec_(codec) {}

    int sample_rate() const override {
        return codec_ == nullptr ? 16000 : codec_->input_sample_rate();
    }

    int channels() const override { return codec_ == nullptr ? 1 : codec_->input_channels(); }

    bool Read(std::vector<int16_t>& samples) override {
        return codec_ != nullptr && codec_->InputData(samples);
    }
};

class ArgusK10AudioSink : public argus::AudioSink {
private:
    AudioCodec* codec_;
    std::vector<int16_t> output_;

public:
    explicit ArgusK10AudioSink(AudioCodec* codec) : codec_(codec) {}

    int sample_rate() const override {
        return codec_ == nullptr ? 16000 : codec_->output_sample_rate();
    }

    void Write(const std::vector<int16_t>& samples) override {
        if (codec_ == nullptr || samples.empty()) {
            return;
        }
        output_.assign(samples.begin(), samples.end());
        codec_->OutputData(output_);
    }
};

class ArgusK10Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    esp_io_expander_handle_t io_expander;
    LcdDisplay* display_;
    button_handle_t btn_a;
    button_handle_t btn_b;
    EspVideo* camera_;
    argus::FaceAttendance* face_attendance_ = nullptr;
    ArgusK10FrameSource* frame_source_ = nullptr;
    ArgusK10AudioSource* audio_source_ = nullptr;
    ArgusK10AudioSink* audio_sink_ = nullptr;
    argus::VoiceService* voice_service_ = nullptr;
    std::atomic<bool> voice_control_busy_{false};

    button_driver_t* btn_a_driver_ = nullptr;
    button_driver_t* btn_b_driver_ = nullptr;

    CircularStrip* led_strip_;

    static ArgusK10Board* instance_;

    static void WaitForXiaozhiAudioShutdown(AudioCodec* codec) {
        if (codec == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            return;
        }
        for (int i = 0; i < 100 && codec->input_enabled(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (codec->input_enabled()) {
            ESP_LOGW(TAG, "Xiaozhi audio input did not stop cleanly");
        }
        // The input task clears the enabled flag before exiting. Keep a short
        // grace period so it cannot disable the codec after Volcano reopens it.
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_21;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_12;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    esp_err_t IoExpanderSetLevel(uint16_t pin_mask, uint8_t level) {
        return esp_io_expander_set_level(io_expander, pin_mask, level);
    }

    uint8_t IoExpanderGetLevel(uint16_t pin_mask) {
        uint32_t pin_val = 0;
        esp_io_expander_get_level(io_expander, DRV_IO_EXP_INPUT_MASK, &pin_val);
        pin_mask &= DRV_IO_EXP_INPUT_MASK;
        return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
    }

    void InitializeIoExpander() {
        esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000,
                                              &io_expander);

        esp_err_t ret;
        ret = esp_io_expander_print_state(io_expander);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Print state failed: %s", esp_err_to_name(ret));
        }

        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1,
                                      IO_EXPANDER_OUTPUT);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Set direction failed: %s", esp_err_to_name(ret));
        }
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1,
                                        0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Set level failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1,
                                        1);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Set level failed: %s", esp_err_to_name(ret));
        }
        ret = esp_io_expander_set_dir(io_expander, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Set direction failed: %s", esp_err_to_name(ret));
        }
    }

    void InitializeButtons() {
        instance_ = this;

        // Button A
        button_config_t btn_a_config = {.long_press_time = 1000, .short_press_time = 0};
        btn_a_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        btn_a_driver_->enable_power_save = false;
        btn_a_driver_->get_key_level = [](button_driver_t* button_driver) -> uint8_t {
            return !instance_->IoExpanderGetLevel(IO_EXPANDER_PIN_NUM_2);
        };
        ESP_ERROR_CHECK(iot_button_create(&btn_a_config, btn_a_driver_, &btn_a));
        iot_button_register_cb(
            btn_a, BUTTON_SINGLE_CLICK, nullptr,
            [](void* button_handle, void* usr_data) {
                auto self = static_cast<ArgusK10Board*>(usr_data);
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    self->EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            },
            this);
        iot_button_register_cb(
            btn_a, BUTTON_LONG_PRESS_START, nullptr,
            [](void* button_handle, void* usr_data) {
                auto self = static_cast<ArgusK10Board*>(usr_data);
                auto codec = self->GetAudioCodec();
                auto volume = codec->output_volume() - 10;
                if (volume < 0) {
                    volume = 0;
                }
                codec->SetOutputVolume(volume);
                self->GetDisplay()->ShowNotification(Lang::Strings::VOLUME +
                                                     std::to_string(volume));
            },
            this);
        iot_button_register_cb(
            btn_a, BUTTON_DOUBLE_CLICK, nullptr,
            [](void* button_handle, void* usr_data) {
                auto self = static_cast<ArgusK10Board*>(usr_data);
                self->RequestVoiceIntercom(!self->voice_service_->IsRunning());
            },
            this);

        // Button B
        button_config_t btn_b_config = {.long_press_time = 1000, .short_press_time = 0};
        btn_b_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        btn_b_driver_->enable_power_save = false;
        btn_b_driver_->get_key_level = [](button_driver_t* button_driver) -> uint8_t {
            return !instance_->IoExpanderGetLevel(IO_EXPANDER_PIN_NUM_12);
        };
        ESP_ERROR_CHECK(iot_button_create(&btn_b_config, btn_b_driver_, &btn_b));
        iot_button_register_cb(
            btn_b, BUTTON_SINGLE_CLICK, nullptr,
            [](void* button_handle, void* usr_data) {
                auto self = static_cast<ArgusK10Board*>(usr_data);
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    self->EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            },
            this);
        iot_button_register_cb(
            btn_b, BUTTON_LONG_PRESS_START, nullptr,
            [](void* button_handle, void* usr_data) {
                auto self = static_cast<ArgusK10Board*>(usr_data);
                auto codec = self->GetAudioCodec();
                auto volume = codec->output_volume() + 10;
                if (volume > 100) {
                    volume = 100;
                }
                codec->SetOutputVolume(volume);
                self->GetDisplay()->ShowNotification(Lang::Strings::VOLUME +
                                                     std::to_string(volume));
            },
            this);
        iot_button_register_cb(
            btn_b, BUTTON_DOUBLE_CLICK, nullptr,
            [](void* button_handle, void* usr_data) {
                auto self = static_cast<ArgusK10Board*>(usr_data);
                self->RequestVoiceIntercom(!self->voice_service_->IsRunning());
            },
            this);
    }

    void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io =
                {
                    [0] = CAMERA_PIN_D2,
                    [1] = CAMERA_PIN_D3,
                    [2] = CAMERA_PIN_D4,
                    [3] = CAMERA_PIN_D5,
                    [4] = CAMERA_PIN_D6,
                    [5] = CAMERA_PIN_D7,
                    [6] = CAMERA_PIN_D8,
                    [7] = CAMERA_PIN_D9,
                },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
    }

    void InitializeIli9341Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_14;
        io_config.dc_gpio_num = GPIO_NUM_13;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 16;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
#else
        panel_config.color_space = ESP_LCD_COLOR_SPACE_BGR;
#endif

        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        led_strip_ = new CircularStrip(BUILTIN_LED_GPIO, 3);
        new LedStripControl(led_strip_);
    }

    void InitializeArgus() {
        Settings settings("argus");
        const std::string bridge_url = settings.GetString("bridge_url");
        const std::string bridge_token = settings.GetString("bridge_token");
        const std::string device_id = settings.GetString("device_id", CONFIG_VOLC_DEVICE_NAME);

        frame_source_ = new ArgusK10FrameSource(camera_);
        face_attendance_ =
            new argus::FaceAttendance(frame_source_, [this](const argus::AttendanceEvent& event) {
                std::string message = "考勤：";
                message += event.person_id;
                Application::GetInstance().Schedule([message]() {
                    Board::GetInstance().GetDisplay()->ShowNotification(message.c_str(), 3000);
                });
            });
        face_attendance_->SetBridgeInfo(bridge_url, bridge_token, device_id);
        if (!face_attendance_->Start()) {
            ESP_LOGE(TAG, "Failed to start face attendance");
        }

        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("argus.attendance.enroll",
                           "Enroll the next visible face as the specified person. person_id "
                           "supports letters, digits, '-' and '_'.",
                           PropertyList({Property("person_id", kPropertyTypeString)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               return face_attendance_->EnrollNextFace(
                                   properties["person_id"].value<std::string>());
                           });
        mcp_server.AddTool("argus.attendance.status",
                           "Get the number of enrolled local face records.", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               return face_attendance_->GetEnrolledCount();
                           });
        mcp_server.AddTool("argus.attendance.clear",
                           "Delete every enrolled face record. This cannot be undone.",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               return face_attendance_->ClearAll();
                           });
        mcp_server.AddUserOnlyTool(
            "argus.bridge.set_config",
            "Set the Argus bridge endpoint used for attendance upload. The token stays on this "
            "device only.",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("token", kPropertyTypeString),
                          Property("device_id", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ReturnValue {
                const std::string url = properties["url"].value<std::string>();
                const std::string token = properties["token"].value<std::string>();
                const std::string device_id = properties["device_id"].value<std::string>();
                face_attendance_->SetBridgeInfo(url, token, device_id);
                Settings settings("argus", true);
                settings.SetString("bridge_url", url);
                settings.SetString("bridge_token", token);
                settings.SetString("device_id", device_id);
                return true;
            });
        mcp_server.AddTool("argus.voice.start",
                           "Start the Volcano low-load WebSocket intercom. Requires Volcano "
                           "credentials in menuconfig.",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               return RequestVoiceIntercom(true);
                           });
        mcp_server.AddTool("argus.voice.stop",
                           "Stop the Volcano low-load intercom and resume Xiaozhi audio.",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               return RequestVoiceIntercom(false);
                           });
    }

    void RunVoiceControl(bool start) {
        auto* codec = GetAudioCodec();
        if (start) {
#if CONFIG_VOLC_RTC_MODE
            // RTC Lite needs additional internal memory. Temporarily release
            // Xiaozhi's MQTT/TLS protocol before creating the RTC engine.
            Application::GetInstance().ResetProtocol();
            vTaskDelay(pdMS_TO_TICKS(1000));
#endif
            if (face_attendance_ != nullptr) {
                face_attendance_->Stop();
            }
            Application::GetInstance().GetAudioService().Stop();
            WaitForXiaozhiAudioShutdown(codec);
            codec->EnableInput(true);
            codec->EnableOutput(true);
            if (!voice_service_->Start(audio_source_, audio_sink_)) {
                codec->EnableInput(false);
                codec->EnableOutput(false);
                Application::GetInstance().GetAudioService().Start();
                if (face_attendance_ != nullptr) {
                    face_attendance_->Start();
                }
                Application::GetInstance().Schedule([]() {
                    Board::GetInstance().GetDisplay()->ShowNotification("语音服务启动失败", 3000);
                });
            } else {
                Application::GetInstance().Schedule([]() {
                    Board::GetInstance().GetDisplay()->ShowNotification("火山语音已启动", 3000);
                });
            }
            return;
        }

        voice_service_->Stop();
        codec->EnableInput(false);
        codec->EnableOutput(false);
        Application::GetInstance().GetAudioService().Start();
        if (face_attendance_ != nullptr && !face_attendance_->Start()) {
            ESP_LOGE(TAG, "Failed to restart face attendance");
        }
        Application::GetInstance().Schedule(
            []() { Board::GetInstance().GetDisplay()->ShowNotification("火山语音已停止", 3000); });
    }

    bool RequestVoiceIntercom(bool start) {
        if (voice_service_ == nullptr) {
            return false;
        }
        bool expected = false;
        if (!voice_control_busy_.compare_exchange_strong(expected, true)) {
            return false;
        }
        struct Command {
            ArgusK10Board* board;
            bool start;
        };
        auto* command = new Command{this, start};
        if (xTaskCreateWithCaps(
                [](void* arg) {
                    auto* item = static_cast<Command*>(arg);
                    item->board->RunVoiceControl(item->start);
                    item->board->voice_control_busy_.store(false);
                    delete item;
                    vTaskDeleteWithCaps(nullptr);
                },
                "argus_voice_ctl", 8192, command, 4, nullptr,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create the voice control task");
            voice_control_busy_.store(false);
            delete command;
            return false;
        }
        return true;
    }

public:
    ArgusK10Board() {
        InitializeI2c();
        InitializeIoExpander();
        InitializeSpi();
        InitializeIli9341Display();
        InitializeButtons();
        InitializeIot();
        InitializeCamera();
        InitializeArgus();
        audio_source_ = new ArgusK10AudioSource(GetAudioCodec());
        audio_sink_ = new ArgusK10AudioSink(GetAudioCodec());
        voice_service_ = new argus::VoiceService();
    }

    virtual Led* GetLed() override { return led_strip_; }

    virtual AudioCodec* GetAudioCodec() override {
        static ArgusK10AudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Camera* GetCamera() override { return camera_; }

    virtual Display* GetDisplay() override { return display_; }
};

DECLARE_BOARD(ArgusK10Board);

ArgusK10Board* ArgusK10Board::instance_ = nullptr;
