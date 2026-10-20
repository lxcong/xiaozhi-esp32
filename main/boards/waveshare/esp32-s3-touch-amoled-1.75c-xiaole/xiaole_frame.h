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
// Layout presets. kLayoutLarge = 1: 5x sprite (360x320), icons and status on one line at
// the top (y 24..62), single-line subtitle at the bottom (y 366..402, 260 px wide).
// kLayoutLarge = 0: 4x sprite, status line at y 46..84, two-line subtitle at y 326..398.
#ifndef XIAOLE_LAYOUT_LARGE
#define XIAOLE_LAYOUT_LARGE 1
#endif
inline constexpr int kLayoutLarge = XIAOLE_LAYOUT_LARGE;
inline constexpr int kSpriteScale = kLayoutLarge ? 5 : 4;     // 72x64 -> 360x320 or 288x256
inline constexpr int kEarRows = 24;        // Rows above this follow the ear offset.
inline constexpr int kMoodCount = static_cast<int>(std::size(kFrames));
inline constexpr int kNeutralMood = 0;
inline constexpr int kSurprisedMood = 9;
inline constexpr int kThinkingMood = 11;
inline constexpr int kWinkingMood = 12;
inline constexpr int kSleepyMood = 18;
inline constexpr int kTopLineY = kLayoutLarge ? 24 : 46;   // icons (and status text) row
inline constexpr int kTopLineH = 38;
inline constexpr int kBoxOffsetY = kLayoutLarge ? -14 : -24;  // content rows 3..58 between the top line and the subtitle
inline constexpr int kContentTop = 3;       // first sprite row with pixels (ear tips)
inline constexpr int kContentBottom = 58;   // last sprite row with pixels (feet)

// Pupil gaze: moods drawn with the plain "open" eye can look around by moving the
// 3x6 pupil (and its highlight) inside an 8x8 eye box (one cream column added on each
// side of the sprite's 6 px eye); bit 1 = left eye, 2 = right. Horizontal range is
// +-2 pixels (8 on screen), vertical +-1.
inline constexpr int kLeftEyeX = 22;
inline constexpr int kRightEyeX = 38;
inline constexpr int kEyeTop = 32;
inline constexpr int kEyeRows = 8;
inline constexpr int kPupilRangeX = 2;
inline constexpr int kPupilRangeY = 1;
constexpr uint8_t EyePupils(int mood) {
    switch (mood) {
        case 0: case 4: case 5: case 11: case 20: return 3;  // neutral, sad, angry, thinking, confused
        case 12: case 16: case 17: case 19: return 1;        // winking, kissy, confident, silly: right eye closed
        default: return 0;                                   // smiling, wide, heart, closed, glasses, tears
    }
}
inline constexpr int kAngryMood = 5;
inline constexpr int kSadMood = 4;
inline constexpr int kConfusedMood = 20;
inline constexpr int kBrowRow = 30;  // procedural brows use rows 30..31 above the eye box

// Subtitle region. Large layout: x=103..363, y=366..402 (260x36), one line. Compact
// layout: x=83..383, y=326..398 (300x72), two lines. Every corner stays inside the
// 223 px radius the round panel leaves usable. Longer text scrolls line by line.
inline constexpr int kSubtitleX = kLayoutLarge ? 103 : 83;
inline constexpr int kSubtitleY = kLayoutLarge ? 366 : 326;
inline constexpr int kSubtitleWidth = kLayoutLarge ? 260 : 300;
inline constexpr int kSubtitleHeight = kLayoutLarge ? 36 : 72;
inline constexpr int kSubtitleInset = 4;   // horizontal text inset inside the region
inline constexpr int kSubtitleLines = kLayoutLarge ? 1 : 2;
static_assert(233 + kBoxOffsetY - (kSpriteHeight / 2 - kContentTop) * kSpriteScale >= kTopLineY + kTopLineH, "ears clear the top line");
static_assert(233 + kBoxOffsetY + (kContentBottom + 1 - kSpriteHeight / 2) * kSpriteScale <= kSubtitleY, "feet clear the subtitle");
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
    int mood = kNeutralMood;        // target (visible when blend == 1)
    int mood_from = kNeutralMood;   // transition source
    int mood_hold = kNeutralMood;   // frozen interrupted transition: from -> hold at hold_blend
    float hold_blend = 0.f;         // 0 = hold inactive
    float blend = 1.f;              // eased progress from the (held) source to mood
    int ear_shift = 0;
    bool blink = false;
    int pupil_x = 0, pupil_y = 0;   // pupil offset in sprite pixels (moods per EyePupils)
    // Procedural eyes (moods per EyePupils): lid openness 0..1 (0 = closed line), pupil
    // size 0 small / 1 normal / 2 big, brows 0 none / 1 angry / 2 sad / 3 left raised,
    // extra lid rows on the right eye (confused squint), and the highlight.
    float eye_open = 1.f;
    int pupil_size = 1;
    int brows = 0;
    int right_lid_extra = 0;
    bool highlight = true;
    float squash = 0.f;             // 0..1 vertical squash while speaking (bottom anchored)
    int mouth = 0;                  // 0 = the mood's own mouth, 1 = half open, 2 = open (lip sync)
    int box_x = 0;            // emoji_box_ offset from screen center
    int box_y = kBoxOffsetY;  // includes tilt, breathing, gaze and posture
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

inline constexpr float kSpeakSquash = 0.12f;  // full loudness compresses the body by 12 %

inline uint32_t LerpColor(uint32_t a, uint32_t b, float t) {
    if (t <= 0.f)
        return a;
    if (t >= 1.f)
        return b;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xff);
        const float cb = static_cast<float>((b >> shift) & 0xff);
        out |= static_cast<uint32_t>(std::lround(ca + (cb - ca) * t)) << shift;
    }
    return out;
}

// Fills `pixels` (kSpriteWidth * kSpriteHeight ARGB8888, row-major) from the palette
// frames. Transition: source = hold active ? lerp(from, hold, hold_blend) : from, then
// lerp(source, mood, blend). Squash resamples rows toward the bottom edge.
inline void RenderFrame(const Frame& frame, uint32_t* pixels) {
    std::memset(pixels, 0, sizeof(uint32_t) * kSpriteWidth * kSpriteHeight);
    const uint8_t* to = kFrames[frame.mood];
    const uint8_t* from = kFrames[frame.mood_from];
    const uint8_t* hold = kFrames[frame.mood_hold];
    const bool blending = frame.blend < 1.f;
    const bool holding = blending && frame.hold_blend > 0.f;
    const float scale = 1.f - kSpeakSquash * (frame.squash < 0.f ? 0.f : frame.squash > 1.f ? 1.f : frame.squash);
    for (int y = 0; y < kFrameSize; ++y) {
        // Bottom-anchored vertical squash: output row y samples source row sy.
        int sy = y;
        if (scale < 1.f) {
            const float src = static_cast<float>(kFrameSize) - (static_cast<float>(kFrameSize - y)) / scale;
            if (src < 0.f)
                continue;
            sy = static_cast<int>(src);
        }
        for (int x = 0; x < kFrameSize; ++x) {
            const int i = sy * kFrameSize + x;
            uint32_t color = kPalette[to[i]];
            if (blending) {
                uint32_t source = kPalette[from[i]];
                if (holding)
                    source = LerpColor(source, kPalette[hold[i]], frame.hold_blend);
                color = LerpColor(source, color, frame.blend);
            }
            if (frame.blink && sy >= 32 && sy <= 38 && ((x >= 22 && x < 28) || (x >= 38 && x < 44))) {
                color = (sy == 36 || sy == 37) ? 0xff343f35 : 0xfffff0d5;
            } else if (!blending && EyePupils(frame.mood) && sy >= kBrowRow && sy < kEyeTop + kEyeRows) {
                const uint8_t eyes = EyePupils(frame.mood);
                for (int eye = 0; eye < 2; ++eye) {
                    if (!(eyes & (1 << eye)))
                        continue;
                    const int x0 = eye == 0 ? kLeftEyeX : kRightEyeX;
                    if (x < x0 - 1 || x >= x0 + 7)
                        continue;
                    // Row 39 is shared with the blush (x 16..21 and 43..48): only the five
                    // inner columns of the eye box may be repainted there.
                    if (sy == kEyeTop + kEyeRows - 1 && (x < x0 || x > x0 + 4))
                        continue;
                    if (sy < kEyeTop && (x == x0 - 1 || x == x0 + 6))
                        continue;  // brow rows stay 6 wide
                    color = 0xfffff0d5;  // cream: erase the eye and brow drawn in the sprite
                    // --- brows (rows 30..31). "Inner" is the side toward the nose. ---
                    if (sy < kEyeTop) {
                        const int row = sy - kBrowRow;  // 0 or 1
                        const bool inner_half = eye == 0 ? x >= x0 + 3 : x < x0 + 3;
                        bool dark = false;
                        if (frame.brows == 1)       // angry: inner end low, outer end high
                            dark = inner_half ? row == 1 : row == 0;
                        else if (frame.brows == 2)  // sad: inner end high, outer end low
                            dark = inner_half ? row == 0 : row == 1;
                        else if (frame.brows == 3 && eye == 0)  // confused: left brow raised flat
                            dark = row == 0;
                        if (dark)
                            color = 0xff343f35;
                        continue;
                    }
                    // --- lids: rows hidden from the top; 6 = closed line ---
                    float open = frame.eye_open;
                    int hidden = static_cast<int>(std::lround((1.f - open) * 6.f));
                    if (eye == 1)
                        hidden += frame.right_lid_extra;
                    if (hidden > 6)
                        hidden = 6;
                    if (hidden >= 6) {
                        if ((sy == 36 || sy == 37) && x >= x0 && x < x0 + 6)
                            color = 0xff343f35;  // closed eye: the sprite's blink line
                        continue;
                    }
                    const int pw = frame.pupil_size == 0 ? 2 : frame.pupil_size == 2 ? 4 : 3;
                    const int ph = frame.pupil_size == 0 ? 4 : 6;
                    const int px = x0 + 1 + frame.pupil_x - (pw == 4 ? 1 : 0) + (pw == 2 ? 0 : 0);
                    const int py = kEyeTop + 1 + frame.pupil_y + (ph == 4 ? 1 : 0);
                    const int lid_row = kEyeTop + 1 + frame.pupil_y + hidden;  // first visible pupil row
                    if (hidden > 0 && sy == lid_row - 1 && x >= x0 && x < x0 + 6) {
                        color = 0xff343f35;  // lid edge
                        continue;
                    }
                    if (x >= px && x < px + pw && sy >= py && sy < py + ph && sy >= lid_row) {
                        color = 0xff343f35;
                        if (frame.highlight && x == px && sy < py + 2)
                            color = 0xffffffff;
                        if (frame.highlight && pw == 4 && x == px + 1 && sy < py + 2)
                            color = 0xffffffff;
                    }
                }
            }
            if (frame.mouth > 0 && sy >= kMouthY && sy < kMouthY + kMouthH && x >= kMouthX && x < kMouthX + kMouthW) {
                const uint8_t index = kMouthOverlays[frame.mouth > 1 ? 1 : 0][(sy - kMouthY) * kMouthW + (x - kMouthX)];
                if (index != 0)
                    color = kPalette[index];
            }
            const int dest_x = x + (kSpriteWidth - kFrameSize) / 2 + (sy < kEarRows ? frame.ear_shift : 0);
            pixels[y * kSpriteWidth + dest_x] = color;
        }
    }
}

// ---------------------------------------------------------------------------
// Expression engine, after groki-bot's avatar (ExpressionController + FaceAnimator):
// three sources decide the face each frame, transitions ease over 300 ms and an
// interrupted transition is frozen as the new source instead of snapping.
// ---------------------------------------------------------------------------

enum class VoiceState { kIdle, kListening, kThinking, kSpeaking };

struct XorShift32 {
    uint32_t state;
    explicit XorShift32(uint32_t seed) : state(seed ? seed : 0x12345678u) {}
    uint32_t Next() {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return state = x;
    }
    uint32_t Range(uint32_t lo, uint32_t hi) { return lo + Next() % (hi - lo + 1); }  // inclusive
    int SignedRange(int amp) { return static_cast<int>(Next() % (2 * amp + 1)) - amp; }
};

struct AnimatorParams {
    uint32_t blink_open_min_ms = 2500, blink_open_max_ms = 4500;
    uint32_t blink_closed_min_ms = 180, blink_closed_max_ms = 260;
    uint32_t saccade_min_ms = 600, saccade_max_ms = 2400;
    int gaze_amplitude = 1;  // sprite pixels; 0 disables the fidget
};

enum class BlinkStyle { kNormal, kSlow, kDouble };

// Blink and gaze fidget scheduled in absolute milliseconds; seeded so tests and the
// simulator replay the same sequence. Blink styles: normal (quick), slow (half-closed
// frames around a longer close: relaxed), double (two quick closes: curious).
class FaceAnimator {
public:
    explicit FaceAnimator(uint32_t seed = 0x9e3779b9u) : rng_(seed) {}
    AnimatorParams params;
    void SetBlinkStyle(BlinkStyle style) { style_ = style; }
    void Tick(uint64_t now_ms, bool sleeping) {
        if (sleeping) {
            eye_open_ = 1.f;
            phase_ = 0;
            gaze_x_ = gaze_y_ = 0;
            blink_next_ms_ = saccade_next_ms_ = now_ms;  // reschedule on wake
            return;
        }
        if (now_ms >= blink_next_ms_)
            AdvanceBlink(now_ms);
        if (now_ms >= saccade_next_ms_) {
            gaze_x_ = rng_.SignedRange(params.gaze_amplitude);
            gaze_y_ = rng_.SignedRange(params.gaze_amplitude > 1 ? 1 : params.gaze_amplitude);
            saccade_next_ms_ = now_ms + rng_.Range(params.saccade_min_ms, params.saccade_max_ms);
        }
    }
    float eye_open() const { return eye_open_; }
    bool eyes_open() const { return eye_open_ >= 1.f; }
    int gaze_x() const { return gaze_x_; }
    int gaze_y() const { return gaze_y_; }

private:
    // phase 0 = open (waiting), then a per-style sequence of (eye_open, duration) steps.
    void AdvanceBlink(uint64_t now_ms) {
        struct Step { float open; uint32_t ms; };
        static constexpr Step kSlow[] = {{0.5f, 70}, {0.f, 420}, {0.5f, 90}};
        static constexpr Step kDouble[] = {{0.f, 110}, {1.f, 120}, {0.f, 110}};
        const Step* steps = nullptr;
        int count = 0;
        if (style_ == BlinkStyle::kSlow) { steps = kSlow; count = 3; }
        else if (style_ == BlinkStyle::kDouble) { steps = kDouble; count = 3; }
        if (phase_ == 0) {
            if (steps) {
                phase_ = 1;
                eye_open_ = steps[0].open;
                blink_next_ms_ = now_ms + steps[0].ms;
            } else {
                phase_ = 1;
                eye_open_ = 0.f;
                blink_next_ms_ = now_ms + rng_.Range(params.blink_closed_min_ms, params.blink_closed_max_ms);
            }
            return;
        }
        if (steps && phase_ < count) {
            eye_open_ = steps[phase_].open;
            blink_next_ms_ = now_ms + steps[phase_].ms;
            ++phase_;
            return;
        }
        phase_ = 0;
        eye_open_ = 1.f;
        blink_next_ms_ = now_ms + rng_.Range(params.blink_open_min_ms, params.blink_open_max_ms);
    }
    XorShift32 rng_;
    BlinkStyle style_ = BlinkStyle::kNormal;
    float eye_open_ = 1.f;
    int phase_ = 0;
    int gaze_x_ = 0, gaze_y_ = 0;
    uint64_t blink_next_ms_ = 0, saccade_next_ms_ = 0;
};

class ExpressionController {
public:
    static constexpr uint32_t kBlendMs = 300;
    static constexpr uint32_t kDefaultOverlayHoldMs = 3000;

    struct Mix {
        int from = kNeutralMood;
        int hold_to = kNeutralMood;
        float hold_blend = 0.f;
        int to = kNeutralMood;
        float blend = 1.f;
    };

    void SetBase(int mood) { base_ = mood; activity_pending_ = true; }
    void SetVoice(VoiceState voice) { voice_ = voice; activity_pending_ = true; }
    void NoteActivity() { activity_pending_ = true; }
    // Milliseconds since the last mood/voice/overlay change seen by Step/Resolve.
    uint64_t IdleMs(uint64_t now_ms) const { return now_ms > last_activity_ms_ ? now_ms - last_activity_ms_ : 0; }
    // hold_ms == 0 keeps the overlay until ClearOverlay().
    void SetOverlay(int mood, uint32_t hold_ms) {
        overlay_ = mood;
        overlay_hold_ms_ = hold_ms;
        overlay_started_ = false;
        activity_pending_ = true;
    }
    void ClearOverlay() { overlay_ = -1; }
    int base() const { return base_; }
    VoiceState voice() const { return voice_; }
    bool overlay_active() const { return overlay_ >= 0; }

    // Which face should show at `now`: overlay, then thinking, then the base mood.
    int Resolve(uint64_t now_ms) {
        if (activity_pending_) {
            last_activity_ms_ = now_ms;
            activity_pending_ = false;
        }
        if (overlay_ >= 0) {
            if (!overlay_started_) {
                overlay_start_ms_ = now_ms;
                overlay_started_ = true;
            }
            if (overlay_hold_ms_ == 0 || now_ms - overlay_start_ms_ < overlay_hold_ms_)
                return overlay_;
            overlay_ = -1;
        }
        if (voice_ == VoiceState::kThinking)
            return kThinkingMood;
        return base_;
    }

    // Advances the transition to `now`, then retargets toward Resolve(now) if it changed,
    // and returns what to draw.
    Mix Step(uint64_t now_ms) {
        Advance(now_ms);
        const int target = Resolve(now_ms);
        if (target != to_) {
            if (blend_ < 1.f) {
                // Freeze the on-screen mix as the new source. A second interrupt folds the
                // older pair into whichever side carries more weight (bounded error).
                if (hold_blend_ > 0.f)
                    from_ = hold_blend_ >= 0.5f ? hold_to_ : from_;
                hold_to_ = to_;
                hold_blend_ = blend_;
            } else {
                from_ = to_;
                hold_to_ = to_;
                hold_blend_ = 0.f;
            }
            to_ = target;
            start_ms_ = now_ms;
            blend_ = 0.f;
        }
        return Mix{from_, hold_to_, hold_blend_, to_, blend_};
    }

private:
    void Advance(uint64_t now_ms) {
        if (blend_ >= 1.f)
            return;
        const uint64_t elapsed = now_ms - start_ms_;
        const float t = elapsed >= kBlendMs ? 1.f : static_cast<float>(elapsed) / kBlendMs;
        blend_ = t * t * (3.f - 2.f * t);  // smoothstep
        if (blend_ >= 1.f) {
            from_ = to_;
            hold_to_ = to_;
            hold_blend_ = 0.f;
            blend_ = 1.f;
        }
    }

private:
    int base_ = kNeutralMood;
    VoiceState voice_ = VoiceState::kIdle;
    int overlay_ = -1;
    uint32_t overlay_hold_ms_ = 0;
    uint64_t overlay_start_ms_ = 0;
    bool overlay_started_ = false;
    int from_ = kNeutralMood, hold_to_ = kNeutralMood, to_ = kNeutralMood;
    float hold_blend_ = 0.f, blend_ = 1.f;
    uint64_t start_ms_ = 0;
    uint64_t last_activity_ms_ = 0;
    bool activity_pending_ = true;
};

// Playback loudness -> 0..1 mouth/squash level, after groki-bot's update_mouth:
// the level is how far a chunk sits above the clip's running average loudness,
// so mastered music does not pin the mouth open, with a fast attack and slow
// release so consonants open the mouth at once and it closes gracefully.
inline constexpr float kLoudSilenceDb = -55.f;     // below this the mouth is closed
inline constexpr float kLoudRangeDb = 12.f;        // dB above baseline that reaches level 1
inline constexpr float kLoudBaselineRate = 0.05f;  // per update, ~1 s to settle at 20 fps
inline constexpr float kLoudAttack = 0.55f;
inline constexpr float kLoudRelease = 0.20f;
inline constexpr float kMouthHalfLevel = 0.20f;
inline constexpr float kMouthOpenLevel = 0.55f;

class LoudnessEnvelope {
public:
    void Reset() { level_ = 0.f; baseline_ = kLoudSilenceDb; primed_ = false; }
    // dbfs: chunk loudness in dBFS (<= 0). Call once per animation frame.
    float Update(float dbfs) {
        float target = 0.f;
        if (dbfs > kLoudSilenceDb) {
            if (!primed_) {
                baseline_ = dbfs - kLoudRangeDb * 0.5f;  // first sound: half open
                primed_ = true;
            } else {
                baseline_ += (dbfs - baseline_) * kLoudBaselineRate;
            }
            target = (dbfs - baseline_ + kLoudRangeDb * 0.5f) / kLoudRangeDb;
            target = target < 0.f ? 0.f : target > 1.f ? 1.f : target;
        }
        level_ += (target > level_ ? kLoudAttack : kLoudRelease) * (target - level_);
        if (level_ < 0.01f)
            level_ = 0.f;
        return level_;
    }
    float level() const { return level_; }

private:
    float level_ = 0.f;
    float baseline_ = kLoudSilenceDb;
    bool primed_ = false;
};

inline int MouthLevel(float loudness) {
    return loudness >= kMouthOpenLevel ? 2 : loudness >= kMouthHalfLevel ? 1 : 0;
}

// Everything that feeds one frame besides the clock.
struct Scene {
    Pose pose;
    ExpressionController::Mix mix;
    float eye_open = 1.f;              // from FaceAnimator (blinks), 1 = open
    int gaze_x = 0, gaze_y = 0;        // sprite pixels from FaceAnimator
    uint64_t idle_ms = 0;              // since the last mood/voice change (boredom)
    VoiceState voice = VoiceState::kIdle;
    float loudness = -1.f;             // 0..1 playback level; < 0 = unknown, use a synthetic talk bounce
    bool sleeping = false;
};

// Posture per voice state: listening perks the body up (screen pixels); thinking
// turns the pupils up and aside. Tilt moves the body and turns the pupils toward
// the tilt; the saccade fidget shows in the pupils only while the device is level.
inline constexpr int kListenLift = 2;
inline constexpr int kThinkLookX = 1;
inline constexpr int kThinkLookY = -1;
inline constexpr float kPupilPerTiltX = 8.f;   // sprite pixels of tilt per pupil step
inline constexpr float kPupilPerTiltY = 6.f;

// Idle behaviour: a quiet rabbit gets bored (half lids, bigger glances), then drowsy,
// before the power-save sleep face takes over at 60 s.
inline constexpr uint64_t kBoredAfterMs = 25000;
inline constexpr uint64_t kDrowsyAfterMs = 45000;
inline constexpr uint64_t kRelaxedBlinkAfterMs = 12000;
inline constexpr uint32_t kThinkGlanceMs = 1500;  // thinking alternates its glance this often

// Animator settings for the situation, applied by the display before Tick().
inline void ApplyAnimatorPreset(FaceAnimator& animator, int mood, VoiceState voice, uint64_t idle_ms) {
    AnimatorParams p;
    BlinkStyle style = BlinkStyle::kNormal;
    if (voice == VoiceState::kThinking || mood == kConfusedMood)
        style = BlinkStyle::kDouble;
    else if (voice == VoiceState::kIdle && idle_ms >= kRelaxedBlinkAfterMs)
        style = BlinkStyle::kSlow;
    if (voice == VoiceState::kIdle && idle_ms >= kBoredAfterMs) {
        p.gaze_amplitude = 2;          // looking around
        p.saccade_min_ms = 400;
        p.saccade_max_ms = 1500;
        p.blink_open_min_ms = 3500;    // fewer blinks when drowsy
        p.blink_open_max_ms = 6000;
    } else if (voice == VoiceState::kSpeaking) {
        p.saccade_min_ms = 1500;       // an occasional glance while talking
        p.saccade_max_ms = 4000;
    }
    animator.params = p;
    animator.SetBlinkStyle(style);
}

inline int ClampPupil(float v, int range) {
    const int i = static_cast<int>(std::lround(v));
    return i < -range ? -range : i > range ? range : i;
}

inline float TalkBounce(uint64_t ms) {  // placeholder until real loudness arrives (phase 2)
    return 0.5f + 0.5f * static_cast<float>(std::sin(ms / 1000.0 * 2.0 * 3.14159265 * 5.0));
}

inline Frame ComposeFrame(const Scene& scene, uint64_t ms) {
    Frame frame;
    const Pose& pose = scene.pose;
    if (scene.sleeping) {
        frame.mood = frame.mood_from = frame.mood_hold = kSleepyMood;
    } else if (pose.surprised) {
        frame.mood = frame.mood_from = frame.mood_hold = kSurprisedMood;  // reflex: no ease
    } else {
        frame.mood = scene.mix.to;
        frame.mood_from = scene.mix.from;
        frame.mood_hold = scene.mix.hold_to;
        frame.hold_blend = scene.mix.hold_blend;
        frame.blend = scene.mix.blend;
    }
    frame.ear_shift = static_cast<int>(std::lround(pose.ears));
    frame.eye_open = scene.sleeping ? 1.f : scene.eye_open;
    frame.blink = EyePupils(frame.mood) != 0 && frame.blend >= 1.f && !scene.sleeping && scene.eye_open <= 0.f;
    if (!scene.sleeping && EyePupils(frame.mood)) {
        // Lids: attentive while listening or talking; bored, then drowsy, when ignored.
        if (scene.voice == VoiceState::kIdle && scene.eye_open >= 1.f) {
            if (scene.idle_ms >= kDrowsyAfterMs)
                frame.eye_open = 0.34f;
            else if (scene.idle_ms >= kBoredAfterMs)
                frame.eye_open = 0.67f;
        }
        // Pupils: wide when listening, small when sad, otherwise normal.
        frame.pupil_size = scene.voice == VoiceState::kListening ? 2 : frame.mood == kSadMood ? 0 : 1;
        frame.brows = frame.mood == kAngryMood ? 1 : frame.mood == kSadMood ? 2 : frame.mood == kConfusedMood ? 3 : 0;
        frame.right_lid_extra = frame.mood == kConfusedMood ? 3 : 0;
        frame.highlight = frame.mood != kSadMood;
    }
    const int breath = scene.sleeping ? 0 : static_cast<int>(std::lround(2 * std::sin(ms / 650.0)));
    int lift = 0;
    if (!scene.sleeping) {
        frame.pupil_x = ClampPupil(pose.x / kPupilPerTiltX, kPupilRangeX);
        frame.pupil_y = ClampPupil(pose.y / kPupilPerTiltY, kPupilRangeY);
        if (frame.pupil_x == 0 && frame.pupil_y == 0) {
            frame.pupil_x = ClampPupil(static_cast<float>(scene.gaze_x), kPupilRangeX);
            frame.pupil_y = ClampPupil(static_cast<float>(scene.gaze_y), kPupilRangeY);
        }
        switch (scene.voice) {
            case VoiceState::kListening:
                lift = -kListenLift;
                frame.pupil_x = frame.pupil_y = 0;  // looking at the person, whatever the tilt
                break;
            case VoiceState::kThinking:
                // Up and aside, switching sides every kThinkGlanceMs.
                frame.pupil_x = (ms / kThinkGlanceMs) % 2 == 0 ? kThinkLookX : -kThinkLookX;
                frame.pupil_y = kThinkLookY;
                break;
            case VoiceState::kSpeaking:
                frame.squash = scene.loudness < 0.f ? TalkBounce(ms) : scene.loudness;
                frame.mouth = MouthLevel(frame.squash);
                break;
            case VoiceState::kIdle: break;
        }
    }
    frame.box_x = static_cast<int>(std::lround(pose.x));
    frame.box_y = kBoxOffsetY + static_cast<int>(std::lround(pose.y)) + breath + lift;
    return frame;
}

}  // namespace xiaole
