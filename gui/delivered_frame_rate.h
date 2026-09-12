#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace pvt::display {

// Missed presentation deadlines include ticks the event loop never scheduled,
// not merely work replaced in a render queue. Supply only playing time and
// reset the epoch when the target FPS changes or playback resumes.
inline std::uint64_t missed_frame_deadlines(std::int64_t playing_nanoseconds,
                                           double fps, std::uint64_t delivered) noexcept {
    if (playing_nanoseconds <= 0 || !std::isfinite(fps) || fps <= 0) return 0;
    const long double expected = std::floor(
        static_cast<long double>(playing_nanoseconds) * fps / 1.0e9L);
    const auto count = expected >= static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(expected);
    return count > delivered ? count - delivered : 0;
}

// Count completed-frame intervals over a quarter second. Averaging rates for
// individual (millisecond-rounded) intervals exaggerates jitter and throughput.
class DeliveredFrameRate {
public:
    void reset() { start_.reset(); intervals_ = 0U; }

    std::optional<double> record(std::int64_t now_nanoseconds) {
        if (!start_ || now_nanoseconds < *start_) {
            start_ = now_nanoseconds;
            intervals_ = 0U;
            return std::nullopt;
        }
        ++intervals_;
        const std::int64_t elapsed = now_nanoseconds - *start_;
        if (elapsed < 250000000) return std::nullopt;
        const double rate = static_cast<double>(intervals_) * 1.0e9
                            / static_cast<double>(elapsed);
        start_ = now_nanoseconds;
        intervals_ = 0U;
        return rate;
    }

private:
    std::optional<std::int64_t> start_;
    std::uint64_t intervals_ = 0U;
};

} // namespace pvt::display
