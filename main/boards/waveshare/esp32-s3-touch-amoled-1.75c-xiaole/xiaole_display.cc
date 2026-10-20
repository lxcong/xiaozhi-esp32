#include "xiaole_display.h"
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
    mood_ = xiaole::MoodIndex(emotion);
    if (timer_)
        Animate();
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
    const auto pose = !sleeping_ && imu_ ? imu_->Snapshot(ms) : xiaole::Pose{};
    const auto frame = xiaole::ComposeFrame(pose, mood_, sleeping_, ms);
    xiaole::RenderFrame(frame, pixels_.data());
    xiaole::PresentFrame(frame, sprite_, emoji_image_, emoji_box_);
}
