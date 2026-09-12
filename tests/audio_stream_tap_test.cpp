#include "audio_stream_tap.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>

int main() {
    pvt::audio::AudioStreamTap tap;
    std::array<float, 1920> source;
    source.fill(0.5F);
    std::array<std::uint8_t,3840> result{};
    tap.write(source.data(),960);
    if (tap.read(result.data())) return 1; // opt-in required
    tap.enable(true);
    tap.write(source.data(),960);
    if (!tap.read(result.data()) || result[0]!=0 || result[1]!=64) return 2;
    source[0] = std::nanf(""); source[1] = 5.0F;
    tap.write(source.data(),960);
    if (!tap.read(result.data()) || result[0]!=0 || result[1]!=0 || result[2]!=255 || result[3]!=127) return 3;
    for(int i=0;i<100;++i) tap.write(source.data(),960);
    int reads=0; while(tap.read(result.data())) ++reads;
    if (reads!=5) return 4; // drop stale data after stalls
    tap.enable(false); tap.discard();
    tap.write(source.data(),960);
    if(tap.read(result.data())) return 5;
    std::cout << "Audio stream tap checks passed\n";
}
