#pragma once

#include <cstdint>
#include <optional>

namespace pvt::display {

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
