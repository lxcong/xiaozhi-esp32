#pragma once

#include <array>
#include <atomic>
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
    void SetLoudness(float level) { loudness_.store(level); }  // manual 0..1 override, < 0 = use playback level
    // Called from the audio output path with each chunk's loudness in dBFS.
    void SetPlaybackLevelDb(float dbfs, uint64_t at_ms) {
        playback_dbfs_.store(dbfs);
        playback_at_ms_.store(at_ms);
    }

private:
    void ApplyLayout();
    void Animate();
    xiaole::VoiceState CurrentVoiceState() const;
    XiaoleImu* imu_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    lv_image_dsc_t sprite_ = {};
    std::array<uint32_t, xiaole::kSpriteWidth * xiaole::kSpriteHeight> pixels_{};
    xiaole::ExpressionController expression_;
    xiaole::FaceAnimator animator_;
    xiaole::VoiceState last_voice_ = xiaole::VoiceState::kIdle;
    std::atomic<float> loudness_{-1.f};  // manual override (simulator parity); < 0 = envelope from playback
    std::atomic<float> playback_dbfs_{-100.f};
    std::atomic<uint64_t> playback_at_ms_{0};
    xiaole::LoudnessEnvelope envelope_;
    bool sleeping_ = false;
};
