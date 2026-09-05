#include "../gui/delivered_frame_rate.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

int main() {
    pvt::display::DeliveredFrameRate rate;
    int failures = 0;
    const auto check = [&](bool ok) { if (!ok) ++failures; };
    check(!rate.record(0));
    // A 2.5 ms cadence must report 400, rather than jumping between 500/333.
    for (int frame = 1; frame < 100; ++frame) {
        check(!rate.record(frame * 2500000LL));
    }
    const auto steady = rate.record(250000000);
    check(steady && std::fabs(*steady - 400.0) < 1.0e-9);
    // Count intervals, not the arithmetic mean of instantaneous FPS.
    for (int frame = 1; frame < 100; ++frame) {
        check(!rate.record(250000000LL + (frame / 2) * 5000000LL
                           + (frame % 2) * 2000000LL));
    }
    const auto jitter = rate.record(500000000);
    check(jitter && std::fabs(*jitter - 400.0) < 1.0e-9);
    rate.reset();
    check(!rate.record(0));
    for (int frame = 1; frame < 500; ++frame) {
        check(!rate.record(frame * 500000LL));
    }
    const auto submillisecond = rate.record(250000000);
    check(submillisecond && std::fabs(*submillisecond - 2000.0) < 1.0e-9);
    // A new clock epoch starts a fresh window; a long stall counts as elapsed.
    check(!rate.record(0));
    const auto stalled = rate.record(1000000000);
    check(stalled && *stalled == 1.0);
    if (failures) {
        std::cerr << failures << " delivered frame rate checks failed.\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
