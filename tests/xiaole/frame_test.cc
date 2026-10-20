// Pins xiaole_frame.h (shared by the firmware display and tools/xiaole-sim) to the
// compositing rules it replaced: mood selection, 180 ms blink at the end of each
// 4.2 s period, ear rows following the ear offset, and the box position with breathing.
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include "xiaole_frame.h"

int main() {
    using namespace xiaole;
    static uint32_t pixels[kSpriteWidth * kSpriteHeight];
    static_assert(kMoodCount == 21, "21 moods are exported by assets/xiaole/export_firmware.py");
    assert(MoodIndex("surprised") == kSurprisedMood && MoodIndex("sleepy") == kSleepyMood);
    assert(MoodIndex("no-such-mood") == kNeutralMood && MoodIndex(nullptr) == kNeutralMood);

    // First boot: no sensor data, neutral mood, eyes open, box 9 px above center.
    Frame idle = ComposeFrame(Pose{}, kNeutralMood, false, 0);
    assert(idle.mood == kNeutralMood && !idle.blink && idle.ear_shift == 0);
    assert(idle.box_x == 0 && idle.box_y == kBoxOffsetY);
    RenderFrame(idle, pixels);
    for (int y = 0; y < kSpriteHeight; ++y) {
        for (int x = 0; x < 4; ++x)
            assert(pixels[y * kSpriteWidth + x] == 0 && pixels[y * kSpriteWidth + 68 + x] == 0);
        for (int x = 0; x < kFrameSize; ++x)
            assert(pixels[y * kSpriteWidth + 4 + x] == kPalette[kFrames[0][y * kFrameSize + x]]);
    }

    // Blink window: closed for ms % 4200 in [4020, 4200), neutral only.
    assert(ComposeFrame(Pose{}, kNeutralMood, false, 4019).blink == false);
    assert(ComposeFrame(Pose{}, kNeutralMood, false, 4020).blink == true);
    assert(ComposeFrame(Pose{}, kNeutralMood, false, 8399).blink == true);
    assert(ComposeFrame(Pose{}, 1, false, 4020).blink == false);
    Frame blink = ComposeFrame(Pose{}, kNeutralMood, false, 4100);
    RenderFrame(blink, pixels);
    assert(pixels[36 * kSpriteWidth + 4 + 24] == 0xff343f35 && pixels[34 * kSpriteWidth + 4 + 40] == 0xfffff0d5);

    // Surprise overrides the conversation mood; sleep overrides everything and stops breathing.
    Pose shaken;
    shaken.surprised = true;
    assert(ComposeFrame(shaken, 3, false, 0).mood == kSurprisedMood);
    assert(ComposeFrame(shaken, 3, true, 0).mood == kSleepyMood);
    for (uint64_t ms = 0; ms < 5000; ms += 37)
        assert(ComposeFrame(Pose{}, 0, true, ms).box_y == kBoxOffsetY);

    // Ears shift only the top 24 rows and stay inside the 72 px sprite at +-2 px.
    for (float ears : {-2.f, 2.f}) {
        Pose tilted;
        tilted.ears = ears;
        tilted.x = ears > 0 ? 24.f : -24.f;
        tilted.y = 12.f;
        Frame f = ComposeFrame(tilted, 0, false, 0);
        assert(f.ear_shift == (ears > 0 ? 2 : -2) && f.box_x == (ears > 0 ? 24 : -24));
        assert(f.box_y >= kBoxOffsetY + 12 - 2 && f.box_y <= kBoxOffsetY + 12 + 2);
        RenderFrame(f, pixels);
        for (int y = 0; y < kSpriteHeight; ++y)
            for (int x = 0; x < kFrameSize; ++x) {
                const int shift = y < kEarRows ? f.ear_shift : 0;
                assert(pixels[y * kSpriteWidth + 4 + shift + x] == kPalette[kFrames[0][y * kFrameSize + x]]);
            }
    }

    // Subtitle region: two lines of the 46 px device font land on the 36 px pitch,
    // fit inside the 72 px window and never scroll; a third line scrolls one pitch.
    static_assert(kSubtitleX + kSubtitleWidth == 383 && kSubtitleY + kSubtitleHeight == 398);
    static_assert(kSubtitleLinePitch * kSubtitleLines == kSubtitleHeight);
    constexpr int kFontLineHeight = 46;  // font_noto_sans_common_30_4 as loaded from assets
    static_assert(SubtitleLineSpace(kFontLineHeight) == -10);
    auto height = [](int lines) { return lines * kFontLineHeight + (lines - 1) * SubtitleLineSpace(kFontLineHeight); };
    for (int lines = 1; lines <= 6; ++lines)
        assert(SubtitleLineCount(height(lines), kFontLineHeight) == lines);
    assert(SubtitleLineCount(0, kFontLineHeight) == 1);
    assert(SubtitleOverflow(1) == 0 && SubtitleOverflow(2) == 0);
    assert(SubtitleOverflow(3) == kSubtitleLinePitch && SubtitleOverflow(5) == 3 * kSubtitleLinePitch);
    // A smaller fallback font (builtin basic 30 px, line height 32) is handled the same way.
    assert(SubtitleLineCount(32 * 3 + 2 * SubtitleLineSpace(32), 32) == 3);
    // Five lines: three line steps, each hold + slide; offsets rest on line boundaries.
    {
        const int overflow = SubtitleOverflow(5);
        const int step = kSubtitleHoldMs + kSubtitleSlideMs;
        assert(SubtitleScrollDurationMs(overflow) == 3 * step);
        assert(SubtitleScrollOffset(overflow, 0) == 0);
        assert(SubtitleScrollOffset(overflow, kSubtitleHoldMs) == 0);
        assert(SubtitleScrollOffset(overflow, kSubtitleHoldMs + kSubtitleSlideMs / 2) == kSubtitleLinePitch / 2);
        assert(SubtitleScrollOffset(overflow, step) == kSubtitleLinePitch);
        assert(SubtitleScrollOffset(overflow, step + kSubtitleHoldMs - 1) == kSubtitleLinePitch);
        assert(SubtitleScrollOffset(overflow, 2 * step) == 2 * kSubtitleLinePitch);
        assert(SubtitleScrollOffset(overflow, 3 * step) == overflow);
        assert(SubtitleScrollOffset(overflow, 3 * step + 5000) == overflow);
        int prev = 0;
        for (int t = 0; t <= 3 * step; t += 7) {  // monotonic, never past the end
            const int v = SubtitleScrollOffset(overflow, t);
            assert(v >= prev && v <= overflow);
            prev = v;
        }
        assert(SubtitleScrollOffset(0, 1000) == 0);
    }

    std::cout << "frame mood selection, blink window, ear rows, box offset, subtitle geometry: PASS\n";
}
