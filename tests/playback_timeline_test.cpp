#include "../gui/playback_timeline.h"

#include <cstdlib>
#include <iostream>

int main() {
    int failures = 0;
    const auto check = [&](bool ok) { if (!ok) ++failures; };
    pvt::display::PlaybackTimeline timeline;
    timeline.reset(0, 6000.0, 300);
    check(timeline.sample(20000000).frame == 120); // One late callback, 120 frames.
    check(timeline.sample(49999999).frame == 299);
    const auto seam = timeline.sample(50000000);
    check(seam.frame == 0 && seam.looped);
    const auto multiple_loops = timeline.sample(175000000);
    check(multiple_loops.frame == 150 && multiple_loops.looped);
    check(!timeline.sample(175000000).looped);
    timeline.reset(42, 6000.0, 300); // Resume/scrub establishes a new origin.
    check(timeline.sample(0).frame == 42);
    check(timeline.sample(1000000).frame == 48);
    timeline.reset(0, 29.97, 3000);
    check(timeline.sample(10000000000LL).frame == 299);
    check(timeline.sample(10010010011LL).frame == 300);
    timeline.reset(299, 60.0, 300);
    check(timeline.sample(16666667).frame == 0);
    timeline.reset(0, 60.0, 1);
    check(timeline.sample(1000000000).frame == 0);
    if (failures) {
        std::cerr << failures << " elapsed-time playback checks failed.\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
