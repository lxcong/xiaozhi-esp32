#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include "xiaole_motion.h"

int main() {
    xiaole::Motion motion;
    xiaole::Pose pose;
    uint64_t t = 1000;
    for (; t <= 1800; t += 20)
        pose = motion.Update(0, 0, 1, 0, 0, 0, t);
    assert(pose.calibrated && pose.x == 0 && !pose.surprised);
    // A slow tilt maps the vendor's Y axis to screen X without a shake.
    for (int i = 0; i < 100; ++i, t += 20) {
        pose = motion.Update(0, .5f, .866f, 0, 0, 0, t);
        assert(!pose.surprised && std::fabs(pose.x) <= 24 && std::fabs(pose.y) <= 12);
    }
    assert(pose.x > 23 && pose.ears > 1.8f);
    for (int i = 0; i < 100; ++i, t += 20)
        pose = motion.Update(0, 0, 1, 0, 0, 0, t);
    assert(std::fabs(pose.x) < .001f);
    pose = motion.Update(0, 0, 2, 0, 0, 0, t);
    assert(!pose.surprised);
    t += 20;
    pose = motion.Update(0, 0, 2, 0, 0, 0, t);
    assert(pose.surprised);
    const auto first_shake = t;
    // Continuous shaking must expire, then wait out the cooldown.
    for (; t < first_shake + 2400; t += 20) {
        pose = motion.Update(0, 0, 2, 0, 0, 0, t);
        if (t >= first_shake + 850)
            assert(!pose.surprised);
    }
    for (; t < first_shake + 2560; t += 20)
        pose = motion.Update(0, 0, 2, 0, 0, 0, t);
    assert(pose.surprised);
    pose = motion.Update(std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0, 0, t);
    assert(std::isfinite(pose.x));
    xiaole::Motion moving_boot;
    for (int i = 0; i < 100; ++i) {
        pose = moving_boot.Update(0, 0, 1, 100, 0, 0, 1000 + i * 20);
        assert(!pose.calibrated);
    }
    // Calibrate while upright as well as flat; stationary upright does not count as tilt.
    for (int i = 0; i < 100; ++i)
        pose = moving_boot.Update(1, 0, 0, 0, 0, 0, 4000 + i * 20);
    assert(pose.calibrated && pose.x == 0 && pose.y == 0);
    for (int i = 0; i < 100; ++i) {
        pose = moving_boot.Update(8, -8, 8, 512, -512, 512, 7000 + i * 20);
        assert(std::fabs(pose.x) <= 24 && std::fabs(pose.y) <= 12 && std::fabs(pose.ears) <= 2);
    }
    // All subtitle corners must fit inside the physical circular mask with 10 px margin.
    for (int x : {83, 383})
        for (int y : {326, 398}) {
            assert(std::hypot(x - 233, y - 233) < 223);
        }
    std::cout << "motion calibration, tilt, cooldown, invalid samples, bounds, round-screen "
                 "geometry: PASS\n";
}
