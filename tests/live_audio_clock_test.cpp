#include "live_audio_capture.h"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

bool near(double left, double right, double tolerance = 1.0e-12) {
    return std::fabs(left - right) <= tolerance;
}

void test_continuous_beat_position() {
    const auto absent = pvt::audio::live_beat_timing(
        0U, 96000U, 48000U, 48000U, 120.0);
    CHECK(near(absent.anchor_seconds, 0.0));
    CHECK(near(absent.position, 0.0));
    CHECK(near(absent.phase, 0.0));

    const auto first = pvt::audio::live_beat_timing(
        1U, 60000U, 48000U, 48000U, 120.0);
    CHECK(near(first.anchor_seconds, 1.0));
    CHECK(near(first.position, 0.5));
    CHECK(near(first.phase, 0.5));

    // No second onset was accepted, but the unwrapped position continues for
    // two complete predicted beats instead of repeating beat zero forever.
    const auto missed = pvt::audio::live_beat_timing(
        1U, 96000U, 48000U, 48000U, 120.0);
    CHECK(near(missed.position, 2.0));
    CHECK(near(missed.phase, 0.0));

    const auto later = pvt::audio::live_beat_timing(
        5U, 54000U, 48000U, 48000U, 120.0);
    CHECK(near(later.position, 4.25));
    CHECK(near(later.phase, 0.25));
}

void test_reference_meter_divisor_does_not_cancel_detected_tempo() {
    constexpr double reference_beats_per_loop = 8.0;
    const auto slow = pvt::audio::live_beat_timing(
        1U, 72000U, 48000U, 48000U, 60.0);
    const auto fast = pvt::audio::live_beat_timing(
        1U, 72000U, 48000U, 48000U, 120.0);
    const auto slow_phase = pvt::audio::live_beat_route_phase(
        1U, slow.position, reference_beats_per_loop);
    const auto fast_phase = pvt::audio::live_beat_route_phase(
        1U, fast.position, reference_beats_per_loop);
    CHECK(slow_phase.has_value());
    CHECK(fast_phase.has_value());
    CHECK(near(*slow_phase, 0.0625));
    CHECK(near(*fast_phase, 0.125));
    CHECK(!near(*slow_phase, *fast_phase));

    const auto latency = pvt::audio::live_beat_route_phase(
        5U, 4.25, reference_beats_per_loop, 0.5);
    CHECK(latency.has_value());
    CHECK(near(*latency, 0.59375));

    const auto wrapped_negative = pvt::audio::live_beat_route_phase(
        1U, 0.25, 4.0, -0.75);
    CHECK(wrapped_negative.has_value());
    CHECK(near(*wrapped_negative, 0.875));

    // A physical beat at t=0 that reaches the callback 100 ms late must be
    // advanced by those 0.2 beats at 120 BPM, not delayed a second time.
    const auto capture_delay = pvt::audio::live_beat_route_phase(
        1U, 0.0, 4.0, 0.2);
    CHECK(capture_delay.has_value());
    CHECK(near(*capture_delay, 0.05));
}

void test_invalid_beat_routes_stay_unlocked() {
    CHECK(!pvt::audio::live_beat_route_phase(0U, 0.0, 8.0).has_value());
    CHECK(!pvt::audio::live_beat_route_phase(1U, 0.0, 0.0).has_value());
    CHECK(!pvt::audio::live_beat_route_phase(
        1U, (std::numeric_limits<double>::quiet_NaN)(), 8.0).has_value());
    CHECK(!pvt::audio::live_beat_route_phase(
        1U, 0.0, 8.0,
        (std::numeric_limits<double>::infinity)()).has_value());
}

void test_null_callbacks_do_not_double_advance_holdover() {
    CHECK(pvt::audio::live_audio_frame_clock_increment(24000U, true)
          == 24000U);
    CHECK(pvt::audio::live_audio_frame_clock_increment(24000U, false) == 0U);

    // At 120 BPM, advancing the valid sample clock by a sampleless half-second
    // would add one beat before Live adds its wall-clock holdover. Freezing the
    // frame clock leaves that extrapolation to exactly one source of time.
    constexpr std::uint64_t last_valid_frame = 48000U;
    const auto frozen = pvt::audio::live_beat_timing(
        1U, last_valid_frame
                + pvt::audio::live_audio_frame_clock_increment(24000U, false),
        last_valid_frame, 48000U, 120.0);
    CHECK(near(frozen.position, 0.0));

    const double before_threshold =
        pvt::audio::live_audio_extrapolated_beat_position(
            frozen.position, 120.0, 0.499);
    const double at_threshold =
        pvt::audio::live_audio_extrapolated_beat_position(
            frozen.position, 120.0, 0.500);
    const double after_threshold =
        pvt::audio::live_audio_extrapolated_beat_position(
            frozen.position, 120.0, 0.501);
    CHECK(near(before_threshold, 0.998));
    CHECK(near(at_threshold, 1.0));
    CHECK(near(after_threshold, 1.002));
    CHECK(near(at_threshold - before_threshold,
               after_threshold - at_threshold));

    // Routing uses the callback age directly. The coarse 500 ms `receiving`
    // lamp must not hide an advanced route's shorter holdover; only a small
    // normal-callback scheduling floor is applied.
    CHECK(pvt::audio::live_audio_callback_within_holdover(0.099, 20));
    CHECK(!pvt::audio::live_audio_callback_within_holdover(0.101, 20));
    CHECK(pvt::audio::live_audio_callback_within_holdover(0.499, 500));
    CHECK(pvt::audio::live_audio_callback_within_holdover(0.500, 500));
    CHECK(!pvt::audio::live_audio_callback_within_holdover(0.501, 500));
    CHECK(!pvt::audio::live_audio_callback_within_holdover(-1.0, 500));
}

void test_duplicate_device_names_require_opaque_runtime_identity() {
    const std::vector<pvt::audio::LiveAudioDevice> devices {
        {"USB Microphone", false, "runtime-a", "USB Microphone (1 of 2)"},
        {"USB Microphone", true, "runtime-b", "USB Microphone (2 of 2)"},
        {"Built-in Microphone", false, "runtime-c", "Built-in Microphone"}};
    std::string error;
    const auto exact = pvt::audio::find_live_audio_device(
        devices, "runtime-b", &error);
    CHECK(exact.has_value());
    CHECK(*exact == 1U);
    CHECK(error.empty());

    const auto ambiguous = pvt::audio::find_live_audio_device(
        devices, "USB Microphone", &error);
    CHECK(!ambiguous.has_value());
    CHECK(error.find("Multiple") != std::string::npos);

    const auto legacy_unique = pvt::audio::find_live_audio_device(
        devices, "Built-in Microphone", &error);
    CHECK(legacy_unique.has_value());
    CHECK(*legacy_unique == 2U);
    CHECK(error.empty());

    const auto missing = pvt::audio::find_live_audio_device(
        devices, "runtime-gone", &error);
    CHECK(!missing.has_value());
    CHECK(error.find("no longer available") != std::string::npos);
}

} // namespace

int main() {
    test_continuous_beat_position();
    test_reference_meter_divisor_does_not_cancel_detected_tempo();
    test_invalid_beat_routes_stay_unlocked();
    test_null_callbacks_do_not_double_advance_holdover();
    test_duplicate_device_names_require_opaque_runtime_identity();
    if (failures != 0) {
        std::cerr << failures << " live audio clock check(s) failed\n";
        return 1;
    }
    std::cout << "live audio clock tests passed\n";
    return 0;
}
