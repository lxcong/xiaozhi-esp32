#include "xiaole_imu.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {
constexpr const char* kTag = "XiaoleImu";
constexpr int kTimeoutMs = 10;
constexpr int kBusWaitMs = 50;
int16_t Decode(const uint8_t* bytes) {
    return static_cast<int16_t>(static_cast<uint16_t>(bytes[0]) |
                                (static_cast<uint16_t>(bytes[1]) << 8));
}
}  // namespace

void XiaoleImu::Start(i2c_master_bus_handle_t bus, SemaphoreHandle_t bus_mutex) {
    if (task_)
        return;
    bus_ = bus;
    bus_mutex_ = bus_mutex;
    if (xTaskCreate([](void* arg) { static_cast<XiaoleImu*>(arg)->Run(); }, "xiaole_imu", 4096,
                    this, 1, &task_) != pdPASS) {
        task_ = nullptr;
        ESP_LOGW(kTag, "No sensor task; idle animation remains available");
    }
}

esp_err_t XiaoleImu::Read(uint8_t reg, uint8_t* data, size_t size) {
    if (bus_mutex_ && xSemaphoreTake(bus_mutex_, pdMS_TO_TICKS(kBusWaitMs)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    const esp_err_t result = i2c_master_transmit_receive(device_, &reg, 1, data, size, kTimeoutMs);
    if (bus_mutex_)
        xSemaphoreGive(bus_mutex_);
    return result;
}
esp_err_t XiaoleImu::Write(uint8_t reg, uint8_t value) {
    const uint8_t bytes[] = {reg, value};
    if (bus_mutex_ && xSemaphoreTake(bus_mutex_, pdMS_TO_TICKS(kBusWaitMs)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    const esp_err_t result = i2c_master_transmit(device_, bytes, sizeof(bytes), kTimeoutMs);
    if (bus_mutex_)
        xSemaphoreGive(bus_mutex_);
    return result;
}

bool XiaoleImu::Initialize() {
    // Register values and scaling verified against Waveshare's sensor/qmi8658 driver.
    // Probe the 1.75C's high address first; low supports the alternate SA0 strap.
    for (uint16_t address : {0x6b, 0x6a}) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = address;
        cfg.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(bus_, &cfg, &device_) != ESP_OK)
            continue;
        uint8_t id = 0;
        bool ok = Read(0x00, &id, 1) == ESP_OK && id == 0x05;
        ok = ok && Write(0x08, 0x00) == ESP_OK;  // Sensors off while configuring.
        ok = ok && Write(0x02, 0x60) == ESP_OK;  // Auto-increment, big-endian disabled.
        ok = ok && Write(0x03, 0x26) == ESP_OK;  // Accelerometer: 8g, 125 Hz.
        ok = ok && Write(0x04, 0x56) == ESP_OK;  // Gyroscope: 512 degrees/s, 125 Hz.
        ok = ok && Write(0x08, 0x03) == ESP_OK;
        if (ok) {
            ESP_LOGI(kTag, "QMI8658 at 0x%02x; hold still briefly to center Xiaole", address);
            return true;
        }
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
    return false;
}

xiaole::Pose XiaoleImu::Snapshot(uint64_t now_ms) {
    portENTER_CRITICAL(&mux_);
    auto pose = pose_;
    const auto sample_ms = sample_ms_;
    portEXIT_CRITICAL(&mux_);
    // A dead sensor must not leave a permanent tilt or surprised face.
    if (!sample_ms || now_ms - sample_ms > 500)
        return {};
    return pose;
}

void XiaoleImu::Run() {
    xiaole::Motion motion;
    bool suspended = false;
    unsigned failures = 0;
    while (true) {
        if (!device_) {
            if (!Initialize()) {
                ESP_LOGW(kTag, "QMI8658 unavailable; retry in 5 seconds");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            motion = {};
            failures = 0;
            suspended = false;
        }
        const bool sleeping = sleeping_.load();
        if (sleeping != suspended) {
            if (Write(0x08, sleeping ? 0x00 : 0x03) != ESP_OK) {
                i2c_master_bus_rm_device(device_);
                device_ = nullptr;
                continue;
            }
            suspended = sleeping;
            motion = {};
            portENTER_CRITICAL(&mux_);
            sample_ms_ = 0;
            portEXIT_CRITICAL(&mux_);
        }
        if (!sleeping) {
            uint8_t status = 0, data[12];
            esp_err_t result = Read(0x2e, &status, 1);
            if (result == ESP_OK && (status & 3) == 3) {
                result = Read(0x35, data, sizeof(data));
                if (result == ESP_OK) {
                    const uint64_t ms = esp_timer_get_time() / 1000;
                    auto pose =
                        motion.Update(Decode(data) / 4096.f, Decode(data + 2) / 4096.f,
                                      Decode(data + 4) / 4096.f, Decode(data + 6) / 64.f,
                                      Decode(data + 8) / 64.f, Decode(data + 10) / 64.f, ms);
                    portENTER_CRITICAL(&mux_);
                    pose_ = pose;
                    sample_ms_ = ms;
                    portEXIT_CRITICAL(&mux_);
                    failures = 0;
                }
            } else if (result == ESP_OK) {
                result = ESP_ERR_TIMEOUT;  // Treat a stuck data-ready bit as a stale sensor too.
            }
            if (result != ESP_OK && ++failures >= 25) {
                ESP_LOGW(kTag, "Sensor stalled; reinitializing");
                i2c_master_bus_rm_device(device_);
                device_ = nullptr;
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(sleeping ? 200 : 20));
    }
}
