#include "xiaole_display.h"
#include "application.h"
#include "esp_timer.h"
#include "xiaole_lvgl.h"

XiaoleDisplay::~XiaoleDisplay() {
    DisplayLockGuard lock(this);
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
    xiaole::RenderFrame(frame, pixels_.data());
    xiaole::PresentFrame(frame, sprite_, emoji_image_, emoji_box_);
}
