#pragma once

#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "xiaole_frame.h"

// LVGL glue shared by the firmware and the host simulator: sprite descriptor,
// frame presentation and the round-screen layout. Pure LVGL 9 API, no ESP-IDF.
namespace xiaole {

// Points `image` at `pixels` and sizes `box` exactly like the device.
inline void AttachSprite(lv_image_dsc_t& dsc, const uint32_t* pixels, lv_obj_t* image, lv_obj_t* box) {
    dsc = {};
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc.header.w = kSpriteWidth;
    dsc.header.h = kSpriteHeight;
    dsc.header.stride = kSpriteWidth * sizeof(uint32_t);
    dsc.data_size = kSpriteWidth * kSpriteHeight * sizeof(uint32_t);
    dsc.data = reinterpret_cast<const uint8_t*>(pixels);
    lv_image_set_src(image, &dsc);
    lv_image_set_scale(image, kSpriteScale * 256);
    lv_image_set_antialias(image, false);
    lv_obj_center(image);
    lv_obj_set_size(box, kSpriteWidth * kSpriteScale, kSpriteHeight * kSpriteScale);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
}

// Shows a frame already rendered into the descriptor's pixels.
inline void PresentFrame(const Frame& frame, const lv_image_dsc_t& dsc, lv_obj_t* image, lv_obj_t* box) {
    // Dynamic descriptor: discard cached decoding before invalidating its pixels.
    lv_image_cache_drop(&dsc);
    lv_obj_invalidate(image);
    lv_obj_align(box, LV_ALIGN_CENTER, frame.box_x, frame.box_y);
}

// Widgets positioned by ApplyLayout. Null entries are skipped.
struct ScreenWidgets {
    lv_obj_t* top_bar = nullptr;
    lv_obj_t* status_bar = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* notification_label = nullptr;
    lv_obj_t* bottom_bar = nullptr;
    lv_obj_t* chat_message_label = nullptr;
    lv_obj_t* low_battery_popup = nullptr;
    lv_obj_t* low_battery_label = nullptr;
};

// Subtitle: the bar is the fixed 300x72 window, the label wraps inside it at
// kSubtitleLinePitch per line. Two lines fit; more lines make the bar scroll
// vertically (pause, scroll to the end, pause, jump back) so the whole message
// can be read. Call after every text or font change: it re-measures the label.
inline void LayoutSubtitle(lv_obj_t* bar, lv_obj_t* label) {
    lv_obj_set_size(bar, kSubtitleWidth, kSubtitleHeight);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, kSubtitleY);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(bar, LV_DIR_VER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLL_CHAIN);

    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int line_height = font ? lv_font_get_line_height(font) : kSubtitleLinePitch;
    lv_obj_set_width(label, kSubtitleWidth - 2 * kSubtitleInset);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, SubtitleLineSpace(line_height), LV_PART_MAIN);
    lv_obj_set_style_anim_duration(label, 0, LV_PART_MAIN);  // the label itself never scrolls

    lv_anim_delete(bar, nullptr);
    lv_obj_scroll_to_y(bar, 0, LV_ANIM_OFF);
    lv_obj_update_layout(label);
    const int lines = SubtitleLineCount(lv_obj_get_height(label), line_height);
    const int overflow = SubtitleOverflow(lines);
    if (overflow == 0) {
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
    // The animation value is elapsed time; the step/hold profile lives in xiaole_frame.h
    // so the host test pins it. After the last line, hold, then start over from the top.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_user_data(&a, reinterpret_cast<void*>(static_cast<intptr_t>(overflow)));
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t t_ms) {
        auto* bar = static_cast<lv_obj_t*>(obj);
        lv_anim_t* running = lv_anim_get(bar, nullptr);
        const int overflow = running ? static_cast<int>(reinterpret_cast<intptr_t>(lv_anim_get_user_data(running))) : 0;
        lv_obj_scroll_to_y(bar, SubtitleScrollOffset(overflow, t_ms), LV_ANIM_OFF);
    });
    const int duration = SubtitleScrollDurationMs(overflow);
    lv_anim_set_values(&a, 0, duration);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_repeat_delay(&a, kSubtitleHoldMs);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

// Every corner of these rectangles stays within a radius of 223 pixels.
// Keep 10 px clearance from the physical 233 px round display edge.
inline void ApplyLayout(const ScreenWidgets& w) {
    if (w.top_bar) {
        lv_obj_set_size(w.top_bar, 240, 38);
        lv_obj_align(w.top_bar, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_set_style_pad_all(w.top_bar, 0, 0);
    }
    if (w.status_bar) {
        lv_obj_set_size(w.status_bar, 290, 40);
        lv_obj_align(w.status_bar, LV_ALIGN_TOP_MID, 0, 82);
        lv_obj_set_style_pad_all(w.status_bar, 0, 0);
    }
    if (w.status_label)
        lv_obj_set_width(w.status_label, 280);
    if (w.notification_label)
        lv_obj_set_width(w.notification_label, 280);
    if (w.bottom_bar && w.chat_message_label)
        LayoutSubtitle(w.bottom_bar, w.chat_message_label);
    if (w.low_battery_popup) {
        lv_obj_set_size(w.low_battery_popup, 300, 72);
        lv_obj_align(w.low_battery_popup, LV_ALIGN_TOP_MID, 0, 326);
    }
    if (w.low_battery_label) {
        lv_obj_set_width(w.low_battery_label, 280);
        lv_label_set_long_mode(w.low_battery_label, LV_LABEL_LONG_WRAP);
        lv_obj_center(w.low_battery_label);
    }
}

}  // namespace xiaole
