#pragma once

#include <array>
#include <atomic>
#include <string>
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
    // Avatar pack from the assets partition (xiaole_avatar.h); nullptr = builtin rabbit.
    // A pack with a new id gets a short happy overlay so the change is visible at boot.
    bool SetAvatarPack(const void* data, size_t size) override;
    const xiaole::SpriteSet& sprites() const { return sprites_; }
    const xiaole::SpriteSet& live_sprites() const { return live_sprites_; }
    // Live preview of a candidate pack (self.avatar.preview): `data` must come from
    // heap_caps_malloc and is owned by the display from here on (freed on failure too).
    // The preview is drawn until EndAvatarPreview(), until the conversation has been
    // idle for kPreviewIdleRevertMs, or after kPreviewMaxMs; then the live pack returns.
    bool PreviewAvatarPack(uint8_t* data, size_t size, std::string* error);
    void EndAvatarPreview();
    bool previewing() const { return preview_data_ != nullptr; }
    static constexpr uint64_t kPreviewIdleRevertMs = 3000;
    static constexpr uint64_t kPreviewMaxMs = 10 * 60 * 1000;
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
    xiaole::SpriteSet sprites_ = xiaole::BuiltinSprites();       // what Animate draws: live or preview
    xiaole::SpriteSet live_sprites_ = xiaole::BuiltinSprites();  // assets partition pack or builtin
    uint8_t* preview_data_ = nullptr;                             // owned pack bytes behind a preview
    uint64_t preview_started_ms_ = 0, preview_idle_since_ms_ = 0;
    void EndAvatarPreviewLocked();
    xiaole::ExpressionController expression_;
    xiaole::FaceAnimator animator_;
    xiaole::VoiceState last_voice_ = xiaole::VoiceState::kIdle;
    std::atomic<float> loudness_{-1.f};  // manual override (simulator parity); < 0 = envelope from playback
    std::atomic<float> playback_dbfs_{-100.f};
    std::atomic<uint64_t> playback_at_ms_{0};
    xiaole::LoudnessEnvelope envelope_;
    bool sleeping_ = false;
};
