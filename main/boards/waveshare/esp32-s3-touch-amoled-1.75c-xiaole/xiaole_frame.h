#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include "xiaole_motion.h"
#include "xiaole_sprites.h"

// Pixel compositing shared by the firmware (xiaole_display.cc) and the host
// simulator (tools/xiaole-sim). No LVGL or ESP-IDF dependency: everything here is
// pure arithmetic over the palette sprites, so both targets render identical frames.
namespace xiaole {

inline constexpr int kFrameSize = 64;      // Palette frames are 64x64.
inline constexpr int kSpriteWidth = 72;    // 4 px of ear travel on each side.
inline constexpr int kSpriteHeight = 64;
inline constexpr int kSpriteScale = 3;     // 72x64 -> 216x192 on the 466x466 panel.
inline constexpr int kEarRows = 24;        // Rows above this follow the ear offset.
inline constexpr int kMoodCount = static_cast<int>(std::size(kFrames));
inline constexpr int kNeutralMood = 0;
inline constexpr int kSurprisedMood = 9;
inline constexpr int kSleepyMood = 18;
inline constexpr int kBoxOffsetY = -9;     // Rabbit sits above center to leave room for the subtitle.

// Subtitle region: x=83..383, y=326..398 (300x72). Every corner stays inside the
// 223 px radius the round panel leaves usable. Text wraps to two lines inside it;
// anything longer scrolls vertically so the whole message can be read.
inline constexpr int kSubtitleX = 83;
inline constexpr int kSubtitleY = 326;
inline constexpr int kSubtitleWidth = 300;
inline constexpr int kSubtitleHeight = 72;
inline constexpr int kSubtitleInset = 4;   // horizontal text inset inside the region
inline constexpr int kSubtitleLines = 2;
inline constexpr int kSubtitleLinePitch = kSubtitleHeight / kSubtitleLines;  // 36 px per line
inline constexpr int kSubtitleHoldMs = 1500;   // pause before, between and after line steps
inline constexpr int kSubtitleSlideMs = 300;    // one-line slide duration

// Extra line spacing (usually negative) that makes a font's lines land on the pitch.
constexpr int SubtitleLineSpace(int font_line_height) {
    return kSubtitleLinePitch - font_line_height;
}
// Number of wrapped lines LVGL laid out, from the label height it reports:
// height = lines * line_height + (lines - 1) * line_space.
constexpr int SubtitleLineCount(int label_height, int font_line_height) {
    const int pitch = font_line_height + SubtitleLineSpace(font_line_height);
    if (label_height <= font_line_height || pitch <= 0)
        return 1;
    return 1 + (label_height - font_line_height + pitch / 2) / pitch;
}
// Pixels the region must scroll to reveal the last line (0 when the text fits).
constexpr int SubtitleOverflow(int line_count) {
    return line_count <= kSubtitleLines ? 0 : (line_count - kSubtitleLines) * kSubtitleLinePitch;
}
// Scroll offset at time t (ms since the animation began) for a text that overflows by
// `overflow` pixels: hold, slide one line pitch, hold, ... until the last line shows.
// Total duration is SubtitleScrollDurationMs(overflow); the caller restarts after a hold.
constexpr int SubtitleScrollDurationMs(int overflow) {
    return (overflow / kSubtitleLinePitch) * (kSubtitleHoldMs + kSubtitleSlideMs);
}
constexpr int SubtitleScrollOffset(int overflow, int t_ms) {
    const int step_ms = kSubtitleHoldMs + kSubtitleSlideMs;
    const int steps = overflow / kSubtitleLinePitch;
    if (t_ms <= 0 || steps <= 0)
        return 0;
    const int done = t_ms / step_ms;
    if (done >= steps)
        return overflow;
    const int in_step = t_ms - done * step_ms - kSubtitleHoldMs;
    if (in_step <= 0)
        return done * kSubtitleLinePitch;
    // ease-in-out over the slide so the line settles instead of snapping
    const int p = in_step >= kSubtitleSlideMs ? 1024 : (in_step * 1024) / kSubtitleSlideMs;
    const int eased = p < 512 ? (2 * p * p) / 1024 : 1024 - (2 * (1024 - p) * (1024 - p)) / 1024;
    return done * kSubtitleLinePitch + (kSubtitleLinePitch * eased) / 1024;
}
inline constexpr uint32_t kBlinkPeriodMs = 4200;
inline constexpr uint32_t kBlinkOpenMs = 4020;  // Eyes are closed for the last 180 ms of the period.

static_assert(kMoodCount == static_cast<int>(std::size(kMoods)), "mood table mismatch");

// Index into kMoods/kFrames for an emotion name; unknown names map to neutral.
inline int MoodIndex(const char* emotion) {
    if (emotion) {
        for (int i = 0; i < kMoodCount; ++i) {
            if (std::strcmp(emotion, kMoods[i]) == 0)
                return i;
        }
    }
    return kNeutralMood;
}

// Everything the display needs to draw one frame at time `ms`.
struct Frame {
    int mood = kNeutralMood;
    int ear_shift = 0;
    bool blink = false;
    int box_x = 0;            // emoji_box_ offset from screen center
    int box_y = kBoxOffsetY;  // includes tilt and breathing
};

inline Frame ComposeFrame(const Pose& pose, int mood, bool sleeping, uint64_t ms) {
    Frame frame;
    frame.mood = sleeping ? kSleepyMood : (pose.surprised ? kSurprisedMood : mood);
    frame.ear_shift = static_cast<int>(std::lround(pose.ears));
    frame.blink = frame.mood == kNeutralMood && ms % kBlinkPeriodMs >= kBlinkOpenMs;
    const int breath = sleeping ? 0 : static_cast<int>(std::lround(2 * std::sin(ms / 650.0)));
    frame.box_x = static_cast<int>(std::lround(pose.x));
    frame.box_y = kBoxOffsetY + static_cast<int>(std::lround(pose.y)) + breath;
    return frame;
}

// Fills `pixels` (kSpriteWidth * kSpriteHeight ARGB8888, row-major) from the palette frame.
inline void RenderFrame(const Frame& frame, uint32_t* pixels) {
    std::memset(pixels, 0, sizeof(uint32_t) * kSpriteWidth * kSpriteHeight);
    const uint8_t* indices = kFrames[frame.mood];
    for (int y = 0; y < kFrameSize; ++y) {
        for (int x = 0; x < kFrameSize; ++x) {
            uint32_t color = kPalette[indices[y * kFrameSize + x]];
            if (frame.blink && y >= 32 && y <= 38 && ((x >= 22 && x < 28) || (x >= 38 && x < 44))) {
                color = (y == 36 || y == 37) ? 0xff343f35 : 0xfffff0d5;
            }
            const int dest_x = x + (kSpriteWidth - kFrameSize) / 2 + (y < kEarRows ? frame.ear_shift : 0);
            pixels[y * kSpriteWidth + dest_x] = color;
        }
    }
}

}  // namespace xiaole
