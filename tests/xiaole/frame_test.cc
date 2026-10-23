// Pins xiaole_frame.h (shared by the firmware display and tools/xiaole-sim) to the
// compositing rules it replaced: mood selection, 180 ms blink at the end of each
// 4.2 s period, ear rows following the ear offset, and the box position with breathing.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include "xiaole_frame.h"

namespace {

// The builtin rabbit serialised as an avatar pack (the format assets/xiaole/generate.py
// writes), so the pack path can be checked against the compiled-in tables.
std::vector<uint8_t> BuiltinAsPack() {
    using namespace xiaole;
    const SpriteSet& b = BuiltinSprites();
    std::vector<uint8_t> out(kAvatarPackHeaderSize, 0);
    std::memcpy(out.data(), "XLAV", 4);
    out[4] = kAvatarPackVersion;
    out[6] = kAvatarPackHeaderSize;
    out[8] = out[9] = kFrameSize;
    out[10] = kMoodCount;
    out[11] = static_cast<uint8_t>(b.palette_count);
    const AvatarLayout& L = b.layout;
    const int layout[] = {L.left_eye_x, L.right_eye_x, L.eye_top, L.eye_rows, L.brow_row, L.mouth_x, L.mouth_y,
                          L.mouth_w, L.mouth_h, kMouthLevels, L.ear_rows, L.content_top, L.content_bottom,
                          kBuiltinFaceIndex, kBuiltinInkIndex, kBuiltinHighlightIndex, L.pupil_style};
    for (size_t i = 0; i < std::size(layout); ++i)
        out[12 + i] = static_cast<uint8_t>(layout[i]);
    std::memcpy(out.data() + 32, b.id, std::strlen(b.id));
    std::memcpy(out.data() + 48, b.name, std::strlen(b.name));
    for (int i = 0; i < b.palette_count; ++i)
        for (int k = 0; k < 4; ++k)
            out.push_back(static_cast<uint8_t>(b.palette[i] >> (8 * k)));
    for (int i = 0; i < kMoodCount; ++i)
        out.insert(out.end(), b.frames[i], b.frames[i] + kFrameSize * kFrameSize);
    for (int i = 0; i < kMouthLevels; ++i)
        out.insert(out.end(), b.mouth[i], b.mouth[i] + L.mouth_w * L.mouth_h);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
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
    static_assert(kSubtitleX + kSubtitleWidth == (kLayoutLarge ? 363 : 383) && kSubtitleY + kSubtitleHeight == (kLayoutLarge ? 402 : 398));
    static_assert(kSubtitleLinePitch * kSubtitleLines == kSubtitleHeight);
    constexpr int kFontLineHeight = 46;  // font_noto_sans_common_30_4 as loaded from assets
    static_assert(SubtitleLineSpace(kFontLineHeight) == -10);
    auto height = [](int lines) { return lines * kFontLineHeight + (lines - 1) * SubtitleLineSpace(kFontLineHeight); };
    for (int lines = 1; lines <= 6; ++lines)
        assert(SubtitleLineCount(height(lines), kFontLineHeight) == lines);
    assert(SubtitleLineCount(0, kFontLineHeight) == 1);
    assert(SubtitleOverflow(1) == 0 && SubtitleOverflow(kSubtitleLines) == 0);
    assert(SubtitleOverflow(kSubtitleLines + 1) == kSubtitleLinePitch && SubtitleOverflow(kSubtitleLines + 3) == 3 * kSubtitleLinePitch);
    // A smaller fallback font (builtin basic 30 px, line height 32) is handled the same way.
    assert(SubtitleLineCount(32 * 3 + 2 * SubtitleLineSpace(32), 32) == 3);
    // Five lines: three line steps, each hold + slide; offsets rest on line boundaries.
    {
        const int overflow = SubtitleOverflow(kSubtitleLines + 3);
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
        scene.eye_open = 0.f;
        f = ComposeFrame(scene, 0);
        assert(f.blink && f.mood == kNeutralMood);
        scene.eye_open = 1.f;
        scene.gaze_x = 1; scene.gaze_y = -1;
        f = ComposeFrame(scene, 0);
        assert(f.box_x == 0 && f.box_y == kBoxOffsetY);           // the fidget no longer moves the body
        assert(f.pupil_x == 1 && f.pupil_y == -1);                 // it moves the pupils
        scene.pose.x = 20.f; scene.pose.y = -10.f;                 // a tilt wins over the fidget
        f = ComposeFrame(scene, 0);
        assert(f.box_x == 20 && f.pupil_x == 2 && f.pupil_y == -1);  // 20/8 -> 2 (max), -10/6 -> -1 (max)
        scene.pose.x = 9.f; scene.pose.y = 0.f;
        assert(ComposeFrame(scene, 0).pupil_x == 1);
        scene.pose.x = -3.f; scene.pose.y = 2.f;                   // below one pupil step: fidget shows
        f = ComposeFrame(scene, 0);
        assert(f.box_x == -3 && f.pupil_x == 1 && f.pupil_y == -1);
        scene.pose = {};
        scene.gaze_x = scene.gaze_y = 0;
        scene.voice = VoiceState::kListening;
        assert(ComposeFrame(scene, 0).box_y == kBoxOffsetY - kListenLift);
        scene.voice = VoiceState::kThinking;
        f = ComposeFrame(scene, 0);
        assert(f.box_x == 0 && f.pupil_x == kThinkLookX && f.pupil_y == kThinkLookY);
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

    {
        // Pupils: neutral eyes move with pupil_x/y and keep the highlight; happy (smile eyes)
        // never changes; winking moves only the left eye; nothing moves mid-transition.
        Frame look;
        look.pupil_x = 1; look.pupil_y = 0;
        RenderFrame(look, pixels);
        static uint32_t plain[kSpriteWidth * kSpriteHeight];
        RenderFrame(Frame{}, plain);
        assert(pixels[33 * kSpriteWidth + 4 + 24] == 0xffffffff);  // highlight moved right by one
        assert(pixels[35 * kSpriteWidth + 4 + 26] == 0xff343f35);  // pupil column x 24..26
        assert(pixels[35 * kSpriteWidth + 4 + 23] == 0xfffff0d5);  // old pupil column erased
        assert(plain[35 * kSpriteWidth + 4 + 23] == 0xff343f35);
        assert(pixels[35 * kSpriteWidth + 4 + 42] == 0xff343f35);  // right eye moved too
        for (int y = 0; y < kSpriteHeight; ++y)
            for (int x = 0; x < kSpriteWidth; ++x)
                if (y < kEyeTop || y >= kEyeTop + kEyeRows)
                    assert(pixels[y * kSpriteWidth + x] == plain[y * kSpriteWidth + x]);
        Frame far;
        far.pupil_x = 2;  // pupil reaches the added column at x0+6 = 28 (left eye)
        RenderFrame(far, pixels);
        assert(pixels[35 * kSpriteWidth + 4 + 27] == 0xff343f35 && pixels[35 * kSpriteWidth + 4 + 25] == 0xff343f35);
        assert(pixels[35 * kSpriteWidth + 4 + 22] == 0xfffff0d5);
        assert(pixels[39 * kSpriteWidth + 4 + 21] == plain[39 * kSpriteWidth + 4 + 21]);  // blush row untouched
        far.pupil_x = -2;
        RenderFrame(far, pixels);
        assert(pixels[35 * kSpriteWidth + 4 + 21] == 0xff343f35);  // reaches the added column at x0-1
        Frame happy;
        happy.mood = 1; happy.mood_from = 1; happy.pupil_x = 1;
        RenderFrame(happy, pixels);
        Frame happy_still;
        happy_still.mood = 1; happy_still.mood_from = 1;
        RenderFrame(happy_still, plain);
        assert(std::memcmp(pixels, plain, sizeof(uint32_t) * kSpriteWidth * kSpriteHeight) == 0);
        Frame wink;
        wink.mood = kWinkingMood; wink.mood_from = kWinkingMood; wink.pupil_x = -1;
        RenderFrame(wink, pixels);
        Frame wink_still = wink;
        wink_still.pupil_x = 0;
        RenderFrame(wink_still, plain);
        assert(pixels[35 * kSpriteWidth + 4 + 22] == 0xff343f35 && plain[35 * kSpriteWidth + 4 + 22] != 0xff343f35);
        for (int y = kEyeTop; y < kEyeTop + kEyeRows; ++y)
            for (int x = kRightEyeX; x < kRightEyeX + 6; ++x)
                assert(pixels[y * kSpriteWidth + 4 + x] == plain[y * kSpriteWidth + 4 + x]);
        Frame mid;
        mid.mood = 4; mid.mood_from = 0; mid.blend = 0.5f; mid.pupil_x = 1;
        Frame mid_still = mid;
        mid_still.pupil_x = 0;
        RenderFrame(mid, pixels);
        RenderFrame(mid_still, plain);
        assert(std::memcmp(pixels, plain, sizeof(uint32_t) * kSpriteWidth * kSpriteHeight) == 0);
        assert(EyePupils(0) == 3 && EyePupils(1) == 0 && EyePupils(kWinkingMood) == 1 && EyePupils(kSleepyMood) == 0);
        static_assert(kSpriteScale == (kLayoutLarge ? 5 : 4), "sprite scale per layout");
    }

    {
        // Eye expression: lids by attention and boredom, pupil size, brows, squint, glance habits.
        Scene scene;
        scene.voice = VoiceState::kListening;
        scene.pose.x = 20.f;  // tilted, but a listener looks at the person
        Frame f = ComposeFrame(scene, 0);
        assert(f.pupil_size == 2 && f.pupil_x == 0 && f.pupil_y == 0 && f.eye_open == 1.f);
        scene.voice = VoiceState::kIdle;
        scene.pose = {};
        scene.idle_ms = kBoredAfterMs;
        assert(std::fabs(ComposeFrame(scene, 0).eye_open - 0.67f) < 1e-5f);
        scene.idle_ms = kDrowsyAfterMs;
        assert(std::fabs(ComposeFrame(scene, 0).eye_open - 0.34f) < 1e-5f);
        scene.eye_open = 0.f;  // a blink still closes fully while bored
        assert(ComposeFrame(scene, 0).eye_open == 0.f && ComposeFrame(scene, 0).blink);
        scene.eye_open = 1.f;
        scene.idle_ms = 0;
        scene.mix = {kSadMood, kSadMood, 0.f, kSadMood, 1.f};
        f = ComposeFrame(scene, 0);
        assert(f.pupil_size == 0 && f.brows == 2 && !f.highlight);
        scene.mix = {kAngryMood, kAngryMood, 0.f, kAngryMood, 1.f};
        f = ComposeFrame(scene, 0);
        assert(f.brows == 1 && f.pupil_size == 1 && f.highlight);
        scene.mix = {kConfusedMood, kConfusedMood, 0.f, kConfusedMood, 1.f};
        f = ComposeFrame(scene, 0);
        assert(f.brows == 3 && f.right_lid_extra == 3);
        scene.mix = {};
        scene.voice = VoiceState::kThinking;
        assert(ComposeFrame(scene, 0).pupil_x == kThinkLookX && ComposeFrame(scene, kThinkGlanceMs).pupil_x == -kThinkLookX);
        assert(ComposeFrame(scene, 0).pupil_y == kThinkLookY);
        // happy keeps its sprite eyes: no procedural parameters apply
        scene.voice = VoiceState::kListening;
        scene.mix = {1, 1, 0.f, 1, 1.f};
        assert(ComposeFrame(scene, 0).pupil_size == 1 && EyePupils(1) == 0);
    }
    {
        // Eye rendering: brows, lids, pupil sizes, closed line on every pupil mood.
        static uint32_t plain[kSpriteWidth * kSpriteHeight];
        Frame base;
        RenderFrame(base, plain);
        Frame angry;
        angry.mood = angry.mood_from = angry.mood_hold = kAngryMood;
        angry.brows = 1;
        RenderFrame(angry, pixels);
        // angry: left brow outer end (x 22..24) on row 30, inner end (x 25..27) on row 31
        assert(pixels[30 * kSpriteWidth + 4 + 22] == 0xff343f35 && pixels[31 * kSpriteWidth + 4 + 22] == 0xfffff0d5);
        assert(pixels[31 * kSpriteWidth + 4 + 27] == 0xff343f35 && pixels[30 * kSpriteWidth + 4 + 27] == 0xfffff0d5);
        // right brow mirrors: inner end (x 38..40) on row 31, outer (41..43) on row 30
        assert(pixels[31 * kSpriteWidth + 4 + 38] == 0xff343f35 && pixels[30 * kSpriteWidth + 4 + 43] == 0xff343f35);
        Frame sad;
        sad.mood = sad.mood_from = sad.mood_hold = kSadMood;
        sad.brows = 2; sad.pupil_size = 0; sad.highlight = false;
        RenderFrame(sad, pixels);
        assert(pixels[30 * kSpriteWidth + 4 + 27] == 0xff343f35 && pixels[31 * kSpriteWidth + 4 + 22] == 0xff343f35);
        int dark = 0;
        for (int y = kEyeTop; y < kEyeTop + kEyeRows; ++y)
            for (int x = kLeftEyeX - 1; x < kLeftEyeX + 7; ++x)
                dark += pixels[y * kSpriteWidth + 4 + x] == 0xff343f35;
        assert(dark == 8);  // small pupil: 2x4, no highlight
        Frame half;
        half.eye_open = 0.5f;
        RenderFrame(half, pixels);
        assert(pixels[35 * kSpriteWidth + 4 + 22] == 0xff343f35);  // lid edge line across the eye
        assert(pixels[34 * kSpriteWidth + 4 + 24] == 0xfffff0d5);  // pupil rows above the lid hidden
        assert(pixels[37 * kSpriteWidth + 4 + 24] == 0xff343f35);  // pupil visible below
        Frame big;
        big.pupil_size = 2;
        RenderFrame(big, pixels);
        dark = 0;
        int white = 0;
        for (int y = kEyeTop; y < kEyeTop + kEyeRows; ++y)
            for (int x = kLeftEyeX - 1; x < kLeftEyeX + 7; ++x) {
                dark += pixels[y * kSpriteWidth + 4 + x] == 0xff343f35;
                white += pixels[y * kSpriteWidth + 4 + x] == 0xffffffff;
            }
        assert(dark + white == 24 && white == 4);  // 4x6 pupil with a 2x2 highlight
        Frame closed_angry = angry;
        closed_angry.eye_open = 0.f;
        RenderFrame(closed_angry, pixels);
        assert(pixels[36 * kSpriteWidth + 4 + 24] == 0xff343f35 && pixels[34 * kSpriteWidth + 4 + 24] == 0xfffff0d5);
        Frame squint;
        squint.mood = squint.mood_from = squint.mood_hold = kConfusedMood;
        squint.brows = 3; squint.right_lid_extra = 3;
        RenderFrame(squint, pixels);
        assert(pixels[30 * kSpriteWidth + 4 + 24] == 0xff343f35 && pixels[30 * kSpriteWidth + 4 + 40] != 0xff343f35);  // left brow raised only
        assert(pixels[34 * kSpriteWidth + 4 + 40] == 0xfffff0d5 && pixels[34 * kSpriteWidth + 4 + 24] == 0xff343f35);  // right eye half shut
        (void)plain;
    }
    {
        // Blink styles: slow blinks pass through half-closed frames; double blinks close twice.
        FaceAnimator slow(7);
        slow.SetBlinkStyle(BlinkStyle::kSlow);
        bool saw_half = false;
        int closes = 0; float prev = 1.f;
        for (uint64_t ms = 0; ms < 30000; ms += 10) {
            slow.Tick(ms, false);
            if (slow.eye_open() == 0.5f) saw_half = true;
            if (slow.eye_open() == 0.f && prev > 0.f) ++closes;
            prev = slow.eye_open();
        }
        assert(saw_half && closes >= 4);
        FaceAnimator dbl(7);
        dbl.SetBlinkStyle(BlinkStyle::kDouble);
        int pairs = 0; uint64_t last_close = 0; prev = 1.f;
        for (uint64_t ms = 0; ms < 30000; ms += 10) {
            dbl.Tick(ms, false);
            if (dbl.eye_open() == 0.f && prev > 0.f) {
                if (ms - last_close < 400) ++pairs;
                last_close = ms;
            }
            prev = dbl.eye_open();
        }
        assert(pairs >= 4);
        ExpressionController c;
        c.Step(1000);
        assert(c.IdleMs(31000) == 30000);
        c.SetVoice(VoiceState::kListening);
        c.Step(40000);
        assert(c.IdleMs(41000) == 1000);
        FaceAnimator a;
        ApplyAnimatorPreset(a, kNeutralMood, VoiceState::kIdle, kBoredAfterMs);
        assert(a.params.gaze_amplitude == 2);
    }

    {
        // Avatar pack: the builtin rabbit serialised as a pack loads back and renders the
        // same pixels through every drawing path (blend, blink, pupils, brows, lids, mouth,
        // squash, ears); corrupt packs are refused and leave the set untouched.
        const std::vector<uint8_t> pack = BuiltinAsPack();
        SpriteSet loaded;
        const char* error = nullptr;
        assert(LoadAvatarPack(pack.data(), pack.size(), loaded, &error));
        assert(!loaded.builtin && std::strcmp(loaded.id, kBuiltinAvatarId) == 0 && std::strcmp(loaded.name, kBuiltinAvatarName) == 0);
        assert(loaded.palette_count == BuiltinSprites().palette_count && loaded.face == 0xfffff0d5 && loaded.ink == 0xff343f35);
        assert(loaded.layout.left_eye_x == kLeftEyeX && loaded.layout.mouth_w == kMouthW && loaded.layout.ear_rows == kEarRows);
        static uint32_t from_pack[kSpriteWidth * kSpriteHeight];
        Frame cases[8];
        cases[1].mood = 1; cases[1].mood_from = 0; cases[1].blend = 0.4f;
        cases[2].blink = true;
        cases[3].pupil_x = 2; cases[3].pupil_y = -1; cases[3].pupil_size = 2;
        cases[4].mood = cases[4].mood_from = cases[4].mood_hold = kAngryMood; cases[4].brows = 1; cases[4].eye_open = 0.5f;
        cases[5].mouth = 2; cases[5].squash = 0.7f;
        cases[6].ear_shift = 2; cases[6].mood = cases[6].mood_from = kConfusedMood; cases[6].brows = 3; cases[6].right_lid_extra = 3;
        cases[7].mood = cases[7].mood_from = cases[7].mood_hold = kSadMood; cases[7].pupil_size = 0; cases[7].highlight = false; cases[7].eye_open = 0.f;
        for (const Frame& f : cases) {
            RenderFrame(f, pixels);
            RenderFrame(f, from_pack, loaded);
            assert(std::memcmp(pixels, from_pack, sizeof(pixels)) == 0);
        }
        for (int mood = 0; mood < kMoodCount; ++mood) {
            Frame f;
            f.mood = f.mood_from = f.mood_hold = mood;
            RenderFrame(f, pixels);
            RenderFrame(f, from_pack, loaded);
            assert(std::memcmp(pixels, from_pack, sizeof(pixels)) == 0);
        }
        // The pupil style shifts the engine's pupil size: big style draws the 4x6 pupil at size 1.
        SpriteSet big = loaded;
        big.layout.pupil_style = 2;
        Frame plain;
        RenderFrame(plain, from_pack, big);
        Frame big_pupils;
        big_pupils.pupil_size = 2;
        RenderFrame(big_pupils, pixels);
        assert(std::memcmp(pixels, from_pack, sizeof(pixels)) == 0);
        // Refusals: short, bad magic, wrong version, wrong mood count, index past the palette,
        // eye box outside the frame, content rows outside the checked band.
        SpriteSet untouched = BuiltinSprites();
        auto refuse = [&](std::vector<uint8_t> bad, const char* what) {
            const char* why = nullptr;
            const bool ok = LoadAvatarPack(bad.data(), bad.size(), untouched, &why);
            assert(!ok && why != nullptr && untouched.builtin);
            (void)what;
        };
        refuse(std::vector<uint8_t>(pack.begin(), pack.begin() + 40), "short");
        refuse(std::vector<uint8_t>(pack.begin(), pack.end() - 1), "truncated");
        { auto bad = pack; bad[0] = 'Y'; refuse(bad, "magic"); }
        { auto bad = pack; bad[4] = 2; refuse(bad, "version"); }
        { auto bad = pack; bad[10] = 20; refuse(bad, "moods"); }
        { auto bad = pack; bad[kAvatarPackHeaderSize + 4 * bad[11] + 100] = 200; refuse(bad, "frame index"); }
        { auto bad = pack; bad.back() = 250; refuse(bad, "mouth index"); }
        { auto bad = pack; bad[13] = 60; refuse(bad, "right eye past the frame"); }
        { auto bad = pack; bad[23] = 0; refuse(bad, "content above the checked band"); }
        { auto bad = pack; bad[25] = bad[11]; refuse(bad, "face colour index"); }
        assert(LoadAvatarPack(nullptr, 0, untouched, &error) == false);
        // An external pack (argv[1], e.g. from assets/xiaole/export_pack.py) must load and
        // keep every mood inside the frame contract the layout was checked against.
        if (argc > 1) {
            std::ifstream in(argv[1], std::ios::binary);
            std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            SpriteSet external;
            const char* why = nullptr;
            if (!LoadAvatarPack(file.data(), file.size(), external, &why)) {
                std::cerr << argv[1] << ": " << (why ? why : "?") << "\n";
                return 1;
            }
            for (int mood = 0; mood < kMoodCount; ++mood) {
                Frame f;
                f.mood = f.mood_from = f.mood_hold = mood;
                f.mouth = mood % 3;
                f.blink = mood % 4 == 0;
                RenderFrame(f, pixels, external);
                for (int y = 0; y < external.layout.content_top; ++y)
                    for (int x = 0; x < kSpriteWidth; ++x)
                        assert(pixels[y * kSpriteWidth + x] == 0);
                for (int y = external.layout.content_bottom + 1; y < kSpriteHeight; ++y)
                    for (int x = 0; x < kSpriteWidth; ++x)
                        assert(pixels[y * kSpriteWidth + x] == 0);
            }
            std::cout << "external pack " << external.id << " (" << external.name << "): " << external.palette_count
                      << " colours, 21 moods render inside rows " << external.layout.content_top << ".."
                      << external.layout.content_bottom << "\n";
        }
    }

    std::cout << "frame mood selection, blink window, ear rows, box offset, subtitle geometry, "
                 "expression engine, animator, blending, squash, loudness envelope, mouth overlay, pupils, "
                 "eye expression, blink styles, avatar pack: PASS\n";
}
