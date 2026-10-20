#pragma once

#include <array>
#include "display/lcd_display.h"
#include "xiaole_frame.h"
#include "xiaole_imu.h"

class XiaoleDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    ~XiaoleDisplay();
    void SetImu(XiaoleImu* imu) { imu_ = imu; }
    void SetupUI() override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetTheme(Theme* theme) override;
    void SetPowerSaveMode(bool on) override;

private:
    void ApplyLayout();
    void Animate();
    XiaoleImu* imu_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    lv_image_dsc_t sprite_ = {};
    std::array<uint32_t, xiaole::kSpriteWidth * xiaole::kSpriteHeight> pixels_{};
    int mood_ = xiaole::kNeutralMood;
    bool sleeping_ = false;
};
