#include "esp_lcd_co5300.h"
#include "wifi_board.h"
#include "xiaole_display.h"
#include "xiaole_imu.h"

#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "i2c_device.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "power_save_timer.h"

#include <cmath>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"
#include <cJSON.h>

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#define TAG "XiaoleWaveshare175C"

// Board-local AXP2101 access. The upstream Axp2101/I2cDevice classes wrap every
// transfer in ESP_ERROR_CHECK, so one NACK on this shared bus (PMIC, touch, IMU,
// two codecs) reboots the device. Here a failed read just leaves the last battery
// reading in place; the bus is reset after a run of consecutive failures.
class Pmic {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : bus_(i2c_bus) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = addr;
        cfg.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &cfg, &device_));
        bool ok = true;
        ok &= WriteReg(0x22, 0b110);  // PWRON > OFFLEVEL as POWEROFF Source enable
        ok &= WriteReg(0x27, 0x10);   // hold 4s to power off
        ok &= WriteReg(0x80, 0x01);   // Disable All DCs but DC1
        ok &= WriteReg(0x90, 0x00);   // Disable All LDOs
        ok &= WriteReg(0x91, 0x00);
        ok &= WriteReg(0x82, (3300 - 1500) / 100);  // DC1 3.3V
        ok &= WriteReg(0x92, (3300 - 500) / 100);   // ALDO1 3.3V
        ok &= WriteReg(0x90, 0x01);   // Enable ALDO1 (MIC)
        ok &= WriteReg(0x64, 0x02);   // CV charger voltage 4.1V
        ok &= WriteReg(0x61, 0x02);   // precharge 50mA
        ok &= WriteReg(0x62, 0x08);   // charge current 200mA
        ok &= WriteReg(0x63, 0x01);   // termination 25mA
        if (!ok)
            ESP_LOGW(TAG, "AXP2101 init had I2C errors; power settings may be partial");
    }

    // Each getter returns false when the PMIC did not answer; outputs are untouched.
    bool ReadDirection(int& direction) {
        uint8_t value;
        if (!ReadReg(0x01, value))
            return false;
        direction = (value & 0b01100000) >> 5;
        return true;
    }
    bool ReadBatteryLevel(int& level) {
        uint8_t value;
        if (!ReadReg(0xA4, value))
            return false;
        level = value;
        return true;
    }
    void PowerOff() {
        uint8_t value = 0;
        ReadReg(0x10, value);
        WriteReg(0x10, value | 0x01);
    }

private:
    bool ReadReg(uint8_t reg, uint8_t& value) {
        return Check(i2c_master_transmit_receive(device_, &reg, 1, &value, 1, 100), "read");
    }
    bool WriteReg(uint8_t reg, uint8_t value) {
        const uint8_t buffer[2] = {reg, value};
        return Check(i2c_master_transmit(device_, buffer, sizeof(buffer), 100), "write");
    }
    bool Check(esp_err_t err, const char* what) {
        if (err == ESP_OK) {
            failures_ = 0;
            return true;
        }
        ++failures_;
        if (failures_ == 1 || failures_ % 10 == 0)
            ESP_LOGW(TAG, "AXP2101 %s failed (%s), %u in a row", what, esp_err_to_name(err), failures_);
        if (failures_ % 10 == 0)
            i2c_master_bus_reset(bus_);
        return false;
    }
    i2c_master_bus_handle_t bus_;
    i2c_master_dev_handle_t device_ = nullptr;
    unsigned failures_ = 0;
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255 * brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

// The CST9217 driver issues the register address and the data read as two I2C
// transactions with a delay in between. The IMU task, the PMIC and the audio codec
// share this bus, so another transfer can land in that gap and the touch chip
// then answers with a non-ACK frame. esp_lvgl_port wraps the read in
// ESP_ERROR_CHECK and aborts on that; this callback reports "released" instead
// and holds the bus mutex so the IMU task never interleaves with a touch read.
// The CST9217 only answers with a valid (ACK) frame after it raised INT, so the
// read is gated on the interrupt like esp_lvgl_port does; polling it between
// events yields ESP_ERR_INVALID_RESPONSE on every frame.
struct XiaoleTouch {
    esp_lcd_touch_handle_t handle = nullptr;
    SemaphoreHandle_t bus_mutex = nullptr;
    std::atomic<bool> pending{false};
    lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;
    lv_point_t last_point = {0, 0};
    unsigned failures = 0;
};

static void XiaoleTouchIsr(esp_lcd_touch_handle_t tp) {
    auto* touch = static_cast<XiaoleTouch*>(tp->config.user_data);
    touch->pending.store(true, std::memory_order_relaxed);
}

static void XiaoleTouchRead(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* touch = static_cast<XiaoleTouch*>(lv_indev_get_user_data(indev));
    data->state = touch->last_state;
    data->point = touch->last_point;
    if (!touch->pending.exchange(false, std::memory_order_relaxed))
        return;
    if (xSemaphoreTake(touch->bus_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        touch->pending.store(true, std::memory_order_relaxed);  // retry on the next poll
        return;
    }
    const esp_err_t err = esp_lcd_touch_read_data(touch->handle);
    xSemaphoreGive(touch->bus_mutex);
    if (err != ESP_OK) {
        if (touch->failures++ % 200 == 0)
            ESP_LOGW(TAG, "Touch read failed (%s), ignoring frame; %u so far", esp_err_to_name(err),
                     touch->failures);
        return;
    }
    uint16_t x = 0, y = 0;
    uint8_t count = 0;
    if (esp_lcd_touch_get_coordinates(touch->handle, &x, &y, nullptr, &count, 1) && count > 0) {
        touch->last_point.x = x;
        touch->last_point.y = y;
        touch->last_state = LV_INDEV_STATE_PRESSED;
    } else {
        touch->last_state = LV_INDEV_STATE_RELEASED;
    }
    data->state = touch->last_state;
    data->point = touch->last_point;
}

// BoxAudioCodec with a tap on the playback path: each PCM chunk's RMS in dBFS goes
// to the display for lip sync. The math is a few hundred multiply-adds per 60 ms.
class XiaoleAudioCodec : public BoxAudioCodec {
public:
    using BoxAudioCodec::BoxAudioCodec;
    std::function<void(float dbfs, uint64_t at_ms)> on_level;

    void OutputData(std::vector<int16_t>& data) override {
        if (on_level && !data.empty()) {
            double sum = 0;
            for (int16_t s : data)
                sum += static_cast<double>(s) * s;
            const double rms = std::sqrt(sum / data.size()) / 32768.0;
            const float dbfs = rms > 1e-6 ? static_cast<float>(20.0 * std::log10(rms)) : -120.f;
            on_level(dbfs, esp_timer_get_time() / 1000);
        }
        BoxAudioCodec::OutputData(data);
    }
};

class XiaoleWaveshare175C : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    SemaphoreHandle_t i2c_mutex_ = nullptr;
    XiaoleTouch touch_;
    XiaoleImu imu_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    XiaoleDisplay* display_;
    CustomBacklight* backlight_;

    PowerSaveTimer* power_save_timer_;
    bool discharging_ = false;  // last PMIC reading; gates the idle power-off

    void InitializePowerSaveTimer() {
        // No idle power-off: a desk pet on USB must stay on, and the AXP2101
        // current-direction bits proved unreliable for telling USB from battery
        // (the device switched itself off on USB power). Sleep after 60 s idle
        // only; power off with the power key (4 s hold, see the PMIC setup).
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() { pmic_->PowerOff(); });  // never fires: shutdown disabled
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        i2c_mutex_ = xSemaphoreCreateMutex();
        assert(i2c_mutex_);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void*)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new XiaoleDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = PIN_NUM_TOUCH_RST,
            .int_gpio_num = PIN_NUM_TOUCH_INT,
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = 0,
                    .mirror_x = 1,
                    .mirror_y = 1,
                },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));
        touch_.handle = tp;
        touch_.bus_mutex = i2c_mutex_;
        ESP_ERROR_CHECK(esp_lcd_touch_register_interrupt_callback_with_data(tp, XiaoleTouchIsr, &touch_));
        lvgl_port_lock(0);
        lv_indev_t* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, XiaoleTouchRead);
        lv_indev_set_display(indev, lv_display_get_default());
        lv_indev_set_user_data(indev, &touch_);
        lvgl_port_unlock();
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // MultiNet matches the wake word by its pinyin, one syllable per word ("xiao le").
    // Normalizes spacing and case; rejects anything that is not 1..6 latin syllables.
    static bool NormalizeWakePinyin(std::string& pinyin, std::string& error) {
        std::string out;
        int syllables = 0, letters = 0;
        for (char c : pinyin) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c >= 'a' && c <= 'z') {
                if (letters == 0) syllables++;
                if (++letters > 6) { error = "pinyin syllable too long"; return false; }
                out += c;
            } else if (c == ' ' || c == '\t' || c == '\'' || c == '-') {
                if (letters > 0) out += ' ';
                letters = 0;
            } else {
                error = "pinyin must be lowercase latin syllables separated by spaces";
                return false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (syllables < 1 || syllables > 6) { error = "pinyin must have 1 to 6 syllables"; return false; }
        pinyin = out;
        return true;
    }

    // Name and wake word the user gave Xiaole in a conversation. NVS namespace "persona"
    // (name, wake_cmd, wake_text) is also read by CustomWakeWord at boot, so the choice
    // survives reboots and assets reflashes. The gateway keeps the same persona in its
    // persona.json and reconciles on connect.
    void InitializePersonaTools(McpServer& mcp_server) {
        mcp_server.AddTool("self.persona.get",
                           "Get the assistant's current name and the wake word the user says to "
                           "wake the device, with the pinyin MultiNet listens for.",
                           PropertyList(), [](const PropertyList&) -> ReturnValue {
                               auto& audio = Application::GetInstance().GetAudioService();
                               const auto words = audio.GetWakeWords();
                               const std::string wake = words.empty() ? "" : words.front();
                               Settings settings("persona", false);
                               cJSON* json = cJSON_CreateObject();
                               cJSON_AddStringToObject(json, "name", settings.GetString("name", wake).c_str());
                               cJSON_AddStringToObject(json, "wake_word", wake.c_str());
                               cJSON_AddStringToObject(json, "wake_pinyin", audio.GetWakeWordPinyin().c_str());
                               cJSON_AddBoolToObject(json, "customized", !settings.GetString("wake_cmd").empty());
                               return json;
                           });
        mcp_server.AddTool("self.persona.set_name",
                           "Rename the assistant. The device's wake word changes to the new name at once "
                           "(no reboot): `name` is the new Chinese name, `wake_pinyin` its pinyin with one "
                           "syllable per word (小乐 -> \"xiao le\"), `wake_word` optionally a different "
                           "word to wake with (defaults to the name). Returns an error text when the "
                           "speech model cannot take the phrase; the old name stays then.",
                           PropertyList({Property("name", kPropertyTypeString),
                                         Property("wake_pinyin", kPropertyTypeString),
                                         Property("wake_word", kPropertyTypeString, std::string(""))}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               const auto name = properties["name"].value<std::string>();
                               auto pinyin = properties["wake_pinyin"].value<std::string>();
                               auto wake = properties["wake_word"].value<std::string>();
                               if (wake.empty()) wake = name;
                               std::string error;
                               if (name.empty() || name.size() > 32) {
                                   return std::string("failed: name must be 1 to 32 bytes");
                               }
                               if (!NormalizeWakePinyin(pinyin, error)) {
                                   return "failed: " + error;
                               }
                               auto& audio = Application::GetInstance().GetAudioService();
                               if (!audio.SetWakeWord(pinyin, wake, error)) {
                                   return "failed: " + error;
                               }
                               {
                                   Settings settings("persona", true);
                                   settings.SetString("name", name);
                                   settings.SetString("wake_cmd", pinyin);
                                   settings.SetString("wake_text", wake);
                               }
                               ESP_LOGI(TAG, "Persona renamed to %s, wake word %s (%s)", name.c_str(),
                                        wake.c_str(), pinyin.c_str());
                               const std::string message = "以后叫我「" + name + "」吧";
                               display_->SetChatMessage("assistant", message.c_str());
                               cJSON* json = cJSON_CreateObject();
                               cJSON_AddBoolToObject(json, "ok", true);
                               cJSON_AddStringToObject(json, "name", name.c_str());
                               cJSON_AddStringToObject(json, "wake_word", wake.c_str());
                               cJSON_AddStringToObject(json, "wake_pinyin", pinyin.c_str());
                               return json;
                           });
    }

    // 初始化工具
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
                           "End this conversation and enter WiFi configuration mode.\n"
                           "**CAUTION** You must ask the user to confirm this action.",
                           PropertyList(), [this](const PropertyList& properties) {
                               EnterWifiConfigMode();
                               return true;
                           });
        InitializePersonaTools(mcp_server);
    }

public:
    XiaoleWaveshare175C() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        imu_.Start(i2c_bus_, i2c_mutex_);
        display_->SetImu(&imu_);
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static XiaoleAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        if (!audio_codec.on_level && display_) {
            audio_codec.on_level = [this](float dbfs, uint64_t at_ms) {
                display_->SetPlaybackLevelDb(dbfs, at_ms);
            };
        }
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override { return backlight_; }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        // Keep PMIC traffic out of the touch controller's split transaction as well.
        if (xSemaphoreTake(i2c_mutex_, pdMS_TO_TICKS(50)) != pdTRUE)
            return false;
        int direction = 0;
        const bool ok = pmic_->ReadDirection(direction) && pmic_->ReadBatteryLevel(level);
        xSemaphoreGive(i2c_mutex_);
        if (!ok)
            return false;  // caller keeps the previous reading
        charging = direction == 1;
        discharging = direction == 2;
        discharging_ = discharging;
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(XiaoleWaveshare175C);
