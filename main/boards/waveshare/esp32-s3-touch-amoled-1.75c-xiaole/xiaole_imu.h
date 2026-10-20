#pragma once

#include <atomic>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "xiaole_motion.h"

// Board-lifetime service. No I2C calls or filtering run in LVGL/audio callbacks.
// bus_mutex serializes this task with the touch controller: the CST9217 driver
// splits register address and data into two I2C transactions, and a sensor
// read landing between them makes the touch chip answer with a non-ACK frame.
class XiaoleImu {
public:
    void Start(i2c_master_bus_handle_t bus, SemaphoreHandle_t bus_mutex);
    void SetSleeping(bool sleeping) { sleeping_.store(sleeping); }
    xiaole::Pose Snapshot(uint64_t now_ms);

private:
    void Run();
    bool Initialize();
    esp_err_t Read(uint8_t reg, uint8_t* data, size_t size);
    esp_err_t Write(uint8_t reg, uint8_t value);
    i2c_master_bus_handle_t bus_ = nullptr;
    SemaphoreHandle_t bus_mutex_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> sleeping_{false};
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    xiaole::Pose pose_;
    uint64_t sample_ms_ = 0;
};
