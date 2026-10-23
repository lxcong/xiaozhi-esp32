#include "xiaole_display.h"
#include "application.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"
#include "xiaole_lvgl.h"

#define TAG "XiaoleDisplay"

XiaoleDisplay::~XiaoleDisplay() {
    DisplayLockGuard lock(this);
    EndAvatarPreviewLocked();
    if (timer_)
        lv_timer_delete(timer_);
    // Base destructor destroys the screen after our members. Remove the image first.
    if (emoji_image_) {
        lv_obj_delete(emoji_image_);
        emoji_image_ = nullptr;
    }
}

void XiaoleDisplay::SetupUI() {
    if (setup_ui_called_)
        return;
    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);
    lv_display_add_event_cb(
        display_,
        [](lv_event_t* event) {
            auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
            area->x1 &= ~1;
            area->y1 &= ~1;
            area->x2 |= 1;
            area->y2 |= 1;
        },
        LV_EVENT_INVALIDATE_AREA, nullptr);

    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    xiaole::AttachSprite(sprite_, pixels_.data(), emoji_image_, emoji_box_);
    ApplyLayout();
    Animate();
    timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<XiaoleDisplay*>(lv_timer_get_user_data(timer))->Animate();
        },
        50, this);
}

void XiaoleDisplay::ApplyLayout() {
    xiaole::ScreenWidgets widgets;
    widgets.top_bar = top_bar_;
    widgets.status_bar = status_bar_;
    widgets.status_label = status_label_;
    widgets.notification_label = notification_label_;
    widgets.bottom_bar = bottom_bar_;
    widgets.chat_message_label = chat_message_label_;
    widgets.low_battery_popup = low_battery_popup_;
    widgets.low_battery_label = low_battery_label_;
    xiaole::ApplyLayout(widgets);
}

void XiaoleDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    expression_.SetBase(xiaole::MoodIndex(emotion));
    if (timer_)
        Animate();
}

xiaole::VoiceState XiaoleDisplay::CurrentVoiceState() const {
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateListening: return xiaole::VoiceState::kListening;
        case kDeviceStateConnecting: return xiaole::VoiceState::kThinking;
        case kDeviceStateSpeaking: return xiaole::VoiceState::kSpeaking;
        default: return xiaole::VoiceState::kIdle;
    }
}

void XiaoleDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content ? content : "");
    DisplayLockGuard lock(this);
    if (bottom_bar_)
        ApplyLayout();
}

void XiaoleDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    if (bottom_bar_)
        ApplyLayout();
}

bool XiaoleDisplay::SetAvatarPack(const void* data, size_t size) {
    xiaole::SpriteSet loaded;
    const char* error = nullptr;
    if (data != nullptr && !xiaole::LoadAvatarPack(data, size, loaded, &error)) {
        ESP_LOGW(TAG, "avatar pack rejected: %s", error ? error : "?");
        return false;
    }
    DisplayLockGuard lock(this);
    if (data == nullptr) {
        EndAvatarPreviewLocked();  // the partition is about to be rewritten (or we are shutting down)
        live_sprites_ = xiaole::BuiltinSprites();
        ESP_LOGI(TAG, "avatar: builtin %s", live_sprites_.name);
    } else {
        live_sprites_ = loaded;
        ESP_LOGI(TAG, "avatar: pack %s (%s, %d colours, pupil style %d)", live_sprites_.id, live_sprites_.name,
                 live_sprites_.palette_count, live_sprites_.layout.pupil_style);
        Settings settings("persona", true);
        if (settings.GetString("avatar_id") != live_sprites_.id) {
            settings.SetString("avatar_id", live_sprites_.id);
            expression_.SetOverlay(1 /* happy */, 3000);  // a new look: say hello with it
        }
    }
    if (!previewing())
        sprites_ = live_sprites_;
    if (timer_)
        Animate();
    return true;
}

bool XiaoleDisplay::PreviewAvatarPack(uint8_t* data, size_t size, std::string* error) {
    xiaole::SpriteSet loaded;
    const char* why = nullptr;
    if (data == nullptr || !xiaole::LoadAvatarPack(data, size, loaded, &why)) {
        if (error)
            *error = why ? why : "no data";
        heap_caps_free(data);
        return false;
    }
    DisplayLockGuard lock(this);
    EndAvatarPreviewLocked();
    preview_data_ = data;  // loaded points into it
    sprites_ = loaded;
    preview_started_ms_ = esp_timer_get_time() / 1000;
    preview_idle_since_ms_ = 0;
    expression_.SetOverlay(xiaole::kWinkingMood, 800);  // "ta-da": a short wink in the new look
    ESP_LOGI(TAG, "avatar: previewing pack %s (%s, %d colours)", sprites_.id, sprites_.name, sprites_.palette_count);
    if (timer_)
        Animate();
    return true;
}

void XiaoleDisplay::EndAvatarPreviewLocked() {
    if (!preview_data_)
        return;
    ESP_LOGI(TAG, "avatar: preview of %s ended, back to %s %s", sprites_.id, live_sprites_.builtin ? "builtin" : "pack",
             live_sprites_.id);
    sprites_ = live_sprites_;
    heap_caps_free(preview_data_);
    preview_data_ = nullptr;
    preview_idle_since_ms_ = 0;
}

void XiaoleDisplay::EndAvatarPreview() {
    DisplayLockGuard lock(this);
    EndAvatarPreviewLocked();
    if (timer_)
        Animate();
}

void XiaoleDisplay::SetPowerSaveMode(bool on) {
    SpiLcdDisplay::SetPowerSaveMode(on);
    DisplayLockGuard lock(this);
    sleeping_ = on;
    if (imu_)
        imu_->SetSleeping(on);
    if (timer_) {
        lv_timer_set_period(timer_, on ? 1000 : 50);
        Animate();
    }
}

void XiaoleDisplay::Animate() {
    // Called only with the LVGL lock held (LVGL timer callbacks already own it).
    // The simulator in tools/xiaole-sim runs this same sequence with a fake IMU.
    const uint64_t ms = esp_timer_get_time() / 1000;
    const auto voice = CurrentVoiceState();
    if (previewing()) {
        // A preview belongs to the conversation that asked for it: once the device has been
        // idle (channel closed) for a moment, or after the hard limit, the live look returns.
        if (voice != xiaole::VoiceState::kIdle)
            preview_idle_since_ms_ = 0;
        else if (preview_idle_since_ms_ == 0)
            preview_idle_since_ms_ = ms;
        if ((preview_idle_since_ms_ && ms - preview_idle_since_ms_ >= kPreviewIdleRevertMs) ||
            ms - preview_started_ms_ >= kPreviewMaxMs)
            EndAvatarPreviewLocked();
    }
    if (voice != last_voice_) {
        // Leaving idle: a short wink confirms the wake word (the gateway's
        // WAKE_CONFIRM_EMOTION does the same). Other changes only move the posture.
        if (last_voice_ == xiaole::VoiceState::kIdle)
            expression_.SetOverlay(xiaole::kWinkingMood, 600);
        expression_.SetVoice(voice);
        last_voice_ = voice;
    }
    xiaole::Scene scene;
    scene.mix = expression_.Step(ms);
    scene.idle_ms = expression_.IdleMs(ms);
    xiaole::ApplyAnimatorPreset(animator_, scene.mix.to, voice, scene.idle_ms);
    animator_.Tick(ms, sleeping_);
    scene.pose = !sleeping_ && imu_ ? imu_->Snapshot(ms) : xiaole::Pose{};
    scene.eye_open = animator_.eye_open();
    scene.gaze_x = animator_.gaze_x();
    scene.gaze_y = animator_.gaze_y();
    scene.voice = voice;
    // Lip sync: the codec reports each 60 ms playback chunk's loudness; a chunk older
    // than 250 ms means playback stopped, so the mouth closes instead of sticking.
    float loudness = loudness_.load();
    if (loudness < 0.f) {
        if (voice == xiaole::VoiceState::kSpeaking) {
            const bool fresh = ms - playback_at_ms_.load() <= 250;
            loudness = envelope_.Update(fresh ? playback_dbfs_.load() : -100.f);
        } else {
            envelope_.Reset();
            loudness = 0.f;
        }
    }
    scene.loudness = loudness;
    scene.sleeping = sleeping_;
    const auto frame = xiaole::ComposeFrame(scene, ms);
    xiaole::RenderFrame(frame, pixels_.data(), sprites_);
    xiaole::PresentFrame(frame, sprite_, emoji_image_, emoji_box_);
}
