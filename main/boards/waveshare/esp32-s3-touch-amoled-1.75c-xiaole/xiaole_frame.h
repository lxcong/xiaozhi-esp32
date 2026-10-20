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
inline constexpr int kThinkingMood = 11;
inline constexpr int kWinkingMood = 12;
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
    int mood = kNeutralMood;        // target (visible when blend == 1)
    int mood_from = kNeutralMood;   // transition source
    int mood_hold = kNeutralMood;   // frozen interrupted transition: from -> hold at hold_blend
    float hold_blend = 0.f;         // 0 = hold inactive
    float blend = 1.f;              // eased progress from the (held) source to mood
    int ear_shift = 0;
    bool blink = false;
    float squash = 0.f;             // 0..1 vertical squash while speaking (bottom anchored)
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
    int gaze_amplitude = 1;  // sprite pixels (x3 on screen); 0 disables the fidget
};

// Blink and gaze fidget scheduled in absolute milliseconds; seeded so tests and the
// simulator replay the same sequence.
class FaceAnimator {
public:
    explicit FaceAnimator(uint32_t seed = 0x9e3779b9u) : rng_(seed) {}
    AnimatorParams params;
    void Tick(uint64_t now_ms, bool sleeping) {
        if (sleeping) {
            eyes_open_ = true;
            gaze_x_ = gaze_y_ = 0;
            blink_next_ms_ = saccade_next_ms_ = now_ms;  // reschedule on wake
            return;
        }
        if (now_ms >= blink_next_ms_) {
            eyes_open_ = !eyes_open_;
            blink_next_ms_ = now_ms + (eyes_open_ ? rng_.Range(params.blink_open_min_ms, params.blink_open_max_ms)
                                                  : rng_.Range(params.blink_closed_min_ms, params.blink_closed_max_ms));
        }
        if (now_ms >= saccade_next_ms_) {
            gaze_x_ = rng_.SignedRange(params.gaze_amplitude);
            gaze_y_ = rng_.SignedRange(params.gaze_amplitude);
            saccade_next_ms_ = now_ms + rng_.Range(params.saccade_min_ms, params.saccade_max_ms);
        }
    }
    bool eyes_open() const { return eyes_open_; }
    int gaze_x() const { return gaze_x_; }
    int gaze_y() const { return gaze_y_; }

private:
    XorShift32 rng_;
    bool eyes_open_ = true;
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

    void SetBase(int mood) { base_ = mood; }
    void SetVoice(VoiceState voice) { voice_ = voice; }
    // hold_ms == 0 keeps the overlay until ClearOverlay().
    void SetOverlay(int mood, uint32_t hold_ms) {
        overlay_ = mood;
        overlay_hold_ms_ = hold_ms;
        overlay_started_ = false;
    }
    void ClearOverlay() { overlay_ = -1; }
    int base() const { return base_; }
    VoiceState voice() const { return voice_; }
    bool overlay_active() const { return overlay_ >= 0; }

    // Which face should show at `now`: overlay, then thinking, then the base mood.
    int Resolve(uint64_t now_ms) {
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
};

// Everything that feeds one frame besides the clock.
struct Scene {
    Pose pose;
    ExpressionController::Mix mix;
    bool eyes_open = true;
    int gaze_x = 0, gaze_y = 0;        // sprite pixels from FaceAnimator
    VoiceState voice = VoiceState::kIdle;
    float loudness = -1.f;             // 0..1 playback level; < 0 = unknown, use a synthetic talk bounce
    bool sleeping = false;
};

// Posture per voice state, in sprite pixels: listening perks up, thinking looks up and aside.
inline constexpr int kListenLift = 2;
inline constexpr int kThinkLookX = 3;
inline constexpr int kThinkLookY = 2;

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
    frame.blink = frame.mood == kNeutralMood && frame.blend >= 1.f && !scene.sleeping && !scene.eyes_open;
    const int breath = scene.sleeping ? 0 : static_cast<int>(std::lround(2 * std::sin(ms / 650.0)));
    int gaze_x = scene.gaze_x, gaze_y = scene.gaze_y, lift = 0;
    if (!scene.sleeping) {
        switch (scene.voice) {
            case VoiceState::kListening: lift = -kListenLift; break;
            case VoiceState::kThinking: gaze_x = kThinkLookX; gaze_y = -kThinkLookY; break;
            case VoiceState::kSpeaking:
                frame.squash = scene.loudness < 0.f ? TalkBounce(ms) : scene.loudness;
                break;
            case VoiceState::kIdle: break;
        }
    }
    frame.box_x = static_cast<int>(std::lround(pose.x)) + gaze_x;
    frame.box_y = kBoxOffsetY + static_cast<int>(std::lround(pose.y)) + breath + gaze_y + lift;
    return frame;
}

}  // namespace xiaole
