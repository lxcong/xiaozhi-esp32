#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xiaole {
struct Pose {
    float x = 0;
    float y = 0;
    float ears = 0;
    bool surprised = false;
    bool calibrated = false;
};

// Pure motion processing. Input: acceleration in g, angular speed in degrees/s.
// Owned by the sensor task; the display receives only a copy of Pose.
class Motion {
public:
    Pose Update(float ax, float ay, float az, float gx, float gy, float gz, uint64_t ms) {
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az) || !std::isfinite(gx) ||
            !std::isfinite(gy) || !std::isfinite(gz)) {
            return pose_;
        }
        const float dt = last_ms_ == 0 ? .02f : std::clamp((ms - last_ms_) / 1000.f, .001f, .1f);
        last_ms_ = ms;
        const float norm = std::sqrt(ax * ax + ay * ay + az * az);
        const float spin = std::sqrt(gx * gx + gy * gy + gz * gz);
        if (!pose_.calibrated) {
            const bool stable = norm > .8f && norm < 1.2f && spin < 15 &&
                                (!samples_ || (std::fabs(ax - sum_x_ / samples_) < .08f &&
                                               std::fabs(ay - sum_y_ / samples_) < .08f));
            if (!stable) {
                samples_ = 0;
                sum_x_ = sum_y_ = 0;
                stable_since_ = ms;
                return pose_;
            }
            if (!samples_)
                stable_since_ = ms;
            sum_x_ += ax;
            sum_y_ += ay;
            ++samples_;
            if (ms - stable_since_ >= 600 && samples_ >= 20) {
                bias_x_ = sum_x_ / samples_;
                bias_y_ = sum_y_ / samples_;
                pose_.calibrated = true;
            }
            return pose_;
        }
        // The vendor's 1.75C example maps sensor Y to screen X and sensor X to screen Y.
        // Gyro adds a short response to rotation; gravity prevents accumulated drift.
        const float tx = std::clamp(Deadzone(ay - bias_y_) * 48.f - gx * .025f, -24.f, 24.f);
        const float ty = std::clamp(Deadzone(ax - bias_x_) * 24.f + gy * .012f, -12.f, 12.f);
        const float alpha = dt / (.16f + dt);
        pose_.x += alpha * (tx - pose_.x);
        pose_.y += alpha * (ty - pose_.y);
        pose_.ears += dt / (.32f + dt) * (pose_.x / 12.f - pose_.ears);

        // Two impulses within 200 ms, followed by a 2.5 s cooldown. Slow tilt cannot trigger it.
        if (std::fabs(norm - 1.f) > .65f && ms >= cooldown_until_) {
            if (impulse_ms_ != 0 && ms - impulse_ms_ >= 20 && ms - impulse_ms_ <= 200) {
                surprise_until_ = ms + 850;
                cooldown_until_ = ms + 2500;
                impulse_ms_ = 0;
            } else {
                impulse_ms_ = ms;
            }
        }
        pose_.surprised = ms < surprise_until_;
        return pose_;
    }

private:
    static float Deadzone(float value) { return std::fabs(value) < .025f ? 0.f : value; }
    Pose pose_;
    uint64_t last_ms_ = 0, stable_since_ = 0, impulse_ms_ = 0;
    uint64_t surprise_until_ = 0, cooldown_until_ = 0;
    unsigned samples_ = 0;
    float sum_x_ = 0, sum_y_ = 0, bias_x_ = 0, bias_y_ = 0;
};
}  // namespace xiaole
