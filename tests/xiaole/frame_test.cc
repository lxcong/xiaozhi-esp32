// Pins xiaole_frame.h (shared by the firmware display and tools/xiaole-sim) to the
// compositing rules it replaced: mood selection, 180 ms blink at the end of each
// 4.2 s period, ear rows following the ear offset, and the box position with breathing.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
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


    // ---- Expression engine (after groki-bot) ----
    {
        ExpressionController c;
        auto m = c.Step(1000);
        assert(m.to == kNeutralMood && m.from == kNeutralMood && m.blend == 1.f && m.hold_blend == 0.f);
        // Base mood eases in over 300 ms with smoothstep: 0 at start, 0.5 at 150 ms, 1 at 300 ms.
        c.SetBase(1);
        m = c.Step(2000);
        assert(m.from == kNeutralMood && m.to == 1 && m.blend == 0.f);
        m = c.Step(2150);
        assert(std::fabs(m.blend - 0.5f) < 1e-5f);
        m = c.Step(2300);
        assert(m.blend == 1.f && m.from == 1 && m.hold_blend == 0.f);
        // Interrupt mid-ease: the on-screen mix (neutral->1 at 0.5) becomes the source.
        c.SetBase(4);
        m = c.Step(2400);
        assert(m.to == 4 && m.from == 1 && m.blend == 0.f);
        c.SetBase(5);
        m = c.Step(2550);
        assert(m.to == 5 && m.from == 1 && m.hold_to == 4 && std::fabs(m.hold_blend - 0.5f) < 1e-5f && m.blend == 0.f);
        // A third interrupt folds the older pair into its heavier side (hold 0.5 -> hold_to).
        m = c.Step(2600);  // blend ~ smoothstep(1/6)
        c.SetBase(6);
        m = c.Step(2601);
        assert(m.to == 6 && m.from == 4 && m.hold_to == 5 && m.blend < 0.05f);
        m = c.Step(3000);
        assert(m.blend == 1.f && m.from == 6 && m.hold_blend == 0.f);
        // Overlay outranks base and expires after its hold; sticky overlay (0) stays.
        c.SetOverlay(kWinkingMood, 500);
        assert(c.Resolve(5000) == kWinkingMood && c.Resolve(5499) == kWinkingMood);
        assert(c.Resolve(5500) == 6 && !c.overlay_active());
        c.SetOverlay(kWinkingMood, 0);
        assert(c.Resolve(9000) == kWinkingMood && c.Resolve(99000) == kWinkingMood);
        c.ClearOverlay();
        assert(c.Resolve(99001) == 6);
        // Thinking shows the thinking face regardless of base; listening keeps the base.
        c.SetVoice(VoiceState::kThinking);
        assert(c.Resolve(100000) == kThinkingMood);
        c.SetVoice(VoiceState::kListening);
        assert(c.Resolve(100001) == 6);
        c.SetVoice(VoiceState::kIdle);
    }
    {
        // Animator: deterministic, blink closes for 180..260 ms every 2.5..4.5 s, gaze within +-1.
        FaceAnimator a(42);
        uint64_t closed_ms = 0, open_ms = 0, last_change = 0;
        bool prev = true;
        int gaze_max = 0;
        int blinks = 0;
        for (uint64_t ms = 0; ms < 60000; ms += 10) {
            a.Tick(ms, false);
            gaze_max = std::max({gaze_max, std::abs(a.gaze_x()), std::abs(a.gaze_y())});
            if (a.eyes_open() != prev) {
                const uint64_t span = ms - last_change;
                if (prev)  // was open, now closed
                    open_ms = span;
                else {
                    closed_ms = span;
                    ++blinks;
                    assert(closed_ms >= 180 && closed_ms <= 270);
                }
                assert(prev == false || open_ms <= 4510);
                last_change = ms;
                prev = a.eyes_open();
            }
        }
        assert(blinks >= 10 && blinks <= 25 && gaze_max <= 1);
        FaceAnimator b(42);
        for (uint64_t ms = 0; ms < 5000; ms += 10) {
            a.Tick(ms + 60000, false);
        }
        (void)b;
        // Sleeping: eyes open, no gaze.
        a.Tick(70000, true);
        assert(a.eyes_open() && a.gaze_x() == 0 && a.gaze_y() == 0);
    }
    {
        // Scene composition: blend fields pass through, blink only on a settled neutral face,
        // posture per voice state, squash while speaking, reflexes override the mix.
        Scene scene;
        scene.mix = {kNeutralMood, kNeutralMood, 0.f, 1, 0.25f};
        Frame f = ComposeFrame(scene, 0);
        assert(f.mood == 1 && f.mood_from == kNeutralMood && f.blend == 0.25f && !f.blink);
        scene.mix = {};
        scene.eyes_open = false;
        f = ComposeFrame(scene, 0);
        assert(f.blink && f.mood == kNeutralMood);
        scene.eyes_open = true;
        scene.gaze_x = 1; scene.gaze_y = -1;
        f = ComposeFrame(scene, 0);
        assert(f.box_x == 1 && f.box_y == kBoxOffsetY - 1);
        scene.gaze_x = scene.gaze_y = 0;
        scene.voice = VoiceState::kListening;
        assert(ComposeFrame(scene, 0).box_y == kBoxOffsetY - kListenLift);
        scene.voice = VoiceState::kThinking;
        f = ComposeFrame(scene, 0);
        assert(f.box_x == kThinkLookX && f.box_y == kBoxOffsetY - kThinkLookY);
        scene.voice = VoiceState::kSpeaking;
        scene.loudness = 0.75f;
        assert(ComposeFrame(scene, 0).squash == 0.75f);
        scene.loudness = -1.f;
        f = ComposeFrame(scene, 50);  // 5 Hz bounce at 50 ms = peak
        assert(f.squash > 0.99f);
        scene.voice = VoiceState::kIdle;
        scene.pose.surprised = true;
        scene.mix = {1, 1, 0.f, 2, 0.5f};
        f = ComposeFrame(scene, 0);
        assert(f.mood == kSurprisedMood && f.blend == 1.f);
        scene.pose.surprised = false;
        scene.sleeping = true;
        f = ComposeFrame(scene, 0);
        assert(f.mood == kSleepyMood && f.blend == 1.f && f.squash == 0.f);
    }
    {
        // Rendering: blend 0 equals the source frame, blend 1 the target, 0.5 is channel-wise midway;
        // squash keeps the bottom row and leaves the top rows transparent.
        Frame f;
        f.mood = 1; f.mood_from = kNeutralMood; f.blend = 0.f;
        RenderFrame(f, pixels);
        for (int i = 0; i < kFrameSize * kFrameSize; ++i)
            assert(pixels[(i / kFrameSize) * kSpriteWidth + 4 + i % kFrameSize] == kPalette[kFrames[0][i]]);
        f.blend = 1.f;
        RenderFrame(f, pixels);
        for (int i = 0; i < kFrameSize * kFrameSize; ++i)
            assert(pixels[(i / kFrameSize) * kSpriteWidth + 4 + i % kFrameSize] == kPalette[kFrames[1][i]]);
        f.blend = 0.5f;
        RenderFrame(f, pixels);
        for (int i = 0; i < kFrameSize * kFrameSize; ++i) {
            const uint32_t a = kPalette[kFrames[0][i]], b = kPalette[kFrames[1][i]];
            const uint32_t p = pixels[(i / kFrameSize) * kSpriteWidth + 4 + i % kFrameSize];
            for (int shift = 0; shift < 32; shift += 8) {
                const int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff, cp = (p >> shift) & 0xff;
                assert(std::abs(cp * 2 - (ca + cb)) <= 1);
            }
        }
        assert(LerpColor(0x00000000, 0xffffffff, 0.5f) == 0x80808080);
        Frame squashed;
        squashed.squash = 1.f;  // 12 %: top ~8 rows empty, bottom row intact
        RenderFrame(squashed, pixels);
        for (int x = 0; x < kSpriteWidth; ++x)
            assert(pixels[x] == 0 && pixels[4 * kSpriteWidth + x] == 0);
        for (int x = 0; x < kFrameSize; ++x)
            assert(pixels[(kFrameSize - 1) * kSpriteWidth + 4 + x] == kPalette[kFrames[0][(kFrameSize - 1) * kFrameSize + x]]);
    }

    {
        // Loudness envelope: silence stays closed; a sustained tone opens then settles toward
        // half as the baseline catches up; attack is faster than release; reset clears.
        LoudnessEnvelope env;
        for (int i = 0; i < 10; ++i)
            assert(env.Update(-80.f) == 0.f);
        float first = env.Update(-20.f);
        assert(first > 0.2f && first <= 0.6f);
        float peak = first;
        for (int i = 0; i < 5; ++i)
            peak = std::max(peak, env.Update(-20.f));
        assert(peak >= first);
        for (int i = 0; i < 200; ++i)
            env.Update(-20.f);
        const float settled = env.level();
        assert(settled > 0.3f && settled < 0.7f);  // steady sound sits around half open
        float louder = env.Update(-12.f);
        assert(louder > settled);  // a syllable above the running average opens wider
        float after_one_quiet = env.Update(-80.f);
        assert(after_one_quiet < louder && after_one_quiet > 0.f);  // release is gradual
        for (int i = 0; i < 30; ++i)
            env.Update(-80.f);
        assert(env.level() == 0.f);
        env.Update(-20.f);
        env.Reset();
        assert(env.level() == 0.f);
        assert(MouthLevel(0.f) == 0 && MouthLevel(0.19f) == 0 && MouthLevel(0.2f) == 1 && MouthLevel(0.54f) == 1 && MouthLevel(0.55f) == 2);
    }
    {
        // Mouth overlay: only while speaking, only inside the 10x8 mouth box, index 0 leaves the face.
        Scene scene;
        scene.voice = VoiceState::kSpeaking;
        scene.loudness = 0.9f;
        Frame f = ComposeFrame(scene, 0);
        assert(f.mouth == 2 && f.squash == 0.9f);
        scene.loudness = 0.3f;
        assert(ComposeFrame(scene, 0).mouth == 1);
        scene.voice = VoiceState::kListening;
        assert(ComposeFrame(scene, 0).mouth == 0);
        f = Frame{};
        f.mouth = 2;  // overlay alone, no squash, so rows line up with the plain render
        RenderFrame(f, pixels);
        static uint32_t plain[kSpriteWidth * kSpriteHeight];
        Frame base;
        RenderFrame(base, plain);
        int changed = 0;
        for (int y = 0; y < kSpriteHeight; ++y)
            for (int x = 0; x < kSpriteWidth; ++x) {
                const bool inside = y >= kMouthY && y < kMouthY + kMouthH && x - 4 >= kMouthX && x - 4 < kMouthX + kMouthW;
                const uint32_t a = pixels[y * kSpriteWidth + x], b = plain[y * kSpriteWidth + x];
                if (!inside)
                    assert(a == b);
                else if (a != b)
                    ++changed;
            }
        assert(changed > 10);  // the open mouth is visibly different from the neutral mouth
        assert(pixels[41 * kSpriteWidth + 4 + 30] == 0xff343f35);  // dark mouth interior
        assert(pixels[44 * kSpriteWidth + 4 + 31] == 0xffedaaa7);  // pink tongue
    }

    std::cout << "frame mood selection, blink window, ear rows, box offset, subtitle geometry, "
                 "expression engine, animator, blending, squash, loudness envelope, mouth overlay: PASS\n";
}
