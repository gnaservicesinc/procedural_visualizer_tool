#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pvt::display {

// The transport samples elapsed time, not the number of timer callbacks. A
// delayed callback skips expired frames without slowing the authored timeline.
class PlaybackTimeline {
public:
    struct Sample {
        int frame = 0;
        bool looped = false;
    };

    void reset(int frame, double fps, int frame_count) {
        frame_count_ = std::max(1, frame_count);
        start_frame_ = std::clamp(frame, 0, frame_count_ - 1);
        fps_ = std::isfinite(fps) && fps > 0.0 ? fps : 60.0;
        previous_cycle_ = 0.0L;
    }

    Sample sample(std::int64_t elapsed_nanoseconds) {
        const long double position = start_frame_
            + static_cast<long double>(std::max<std::int64_t>(0, elapsed_nanoseconds))
                  * fps_ / 1000000000.0L;
        const long double cycle = std::floor(position / frame_count_);
        const int frame = static_cast<int>(std::fmod(position, frame_count_));
        const bool looped = cycle > previous_cycle_;
        previous_cycle_ = cycle;
        return {frame, looped};
    }

private:
    int frame_count_ = 1;
    int start_frame_ = 0;
    long double fps_ = 60.0L;
    long double previous_cycle_ = 0.0L;
};

} // namespace pvt::display
