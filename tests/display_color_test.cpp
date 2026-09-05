#include "../gui/display_color.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

unsigned char editor_reference(float value) {
    if (!std::isfinite(value)) return 0;
    value = std::clamp(value, 0.0F, 1.0F);
    const float encoded = value <= 0.0031308F
        ? value * 12.92F
        : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
    return static_cast<unsigned char>(std::lround(encoded * 255.0F));
}

unsigned char live_reference(float value) {
    if (!std::isfinite(value)) return 0;
    const float encoded = value <= 0.0031308F
        ? 12.92F * value
        : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
    return static_cast<unsigned char>(std::lround(
        std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
}

void reference_row(const float* input, unsigned char* output, std::size_t pixels) {
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const std::size_t offset = pixel * 4U;
        output[offset] = editor_reference(input[offset]);
        output[offset + 1U] = editor_reference(input[offset + 1U]);
        output[offset + 2U] = editor_reference(input[offset + 2U]);
        output[offset + 3U] = static_cast<unsigned char>(std::lround(
            std::clamp(input[offset + 3U], 0.0F, 1.0F) * 255.0F));
    }
}

std::uint32_t float_bits(float value) {
    std::uint32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

float from_bits(std::uint32_t bits) {
    float result = 0.0F;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool check_sample(float value) {
    const auto actual = pvt::display::srgb8_converter()(value);
    if (actual == editor_reference(value) && actual == live_reference(value)) {
        return true;
    }
    std::cerr << "Display conversion differs for float bits 0x" << std::hex
              << float_bits(value) << std::dec << ": actual "
              << static_cast<int>(actual) << ", expected "
              << static_cast<int>(editor_reference(value)) << '\n';
    return false;
}

bool test_conversion() {
    const std::array<float, 13> special = {{
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::max(), -1.0F, -0.0F, 0.0F,
        std::numeric_limits<float>::denorm_min(), 0.0031308F, 1.0F, 2.0F,
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),
        -std::numeric_limits<float>::quiet_NaN()
    }};
    for (float value : special) if (!check_sample(value)) return false;

    // Independently invert every mathematical quantization threshold, then
    // cover nearby representable floats. This catches a one-code rounding
    // error even when it affects only one out of millions of input values.
    for (int code = 1; code < 256; ++code) {
        const double encoded = (static_cast<double>(code) - 0.5) / 255.0;
        const float linear = static_cast<float>(encoded <= 0.04045
            ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4));
        const std::uint32_t center = float_bits(linear);
        for (std::uint32_t bits = center - 512U; bits <= center + 512U; ++bits) {
            if (!check_sample(from_bits(bits))) return false;
        }
    }
    for (std::uint32_t sample = 0; sample <= 1048576U; ++sample) {
        if (!check_sample(static_cast<float>(sample) / 1048576.0F)) return false;
    }
    std::uint32_t state = 0x50565431U;
    for (std::size_t sample = 0; sample < 1048576U; ++sample) {
        state = state * 1664525U + 1013904223U;
        if (!check_sample(from_bits(state))) return false;
    }

    // Exercise RGB order, straight alpha, clamping, an odd row length and
    // untouched row padding. The converter must not premultiply transparent RGB.
    const std::array<float, 20> input = {{
        0.01F, 0.1F, 0.9F, 0.0F,
        1.0F, 0.0031308F, 0.5F, 0.5F,
        -1.0F, 10.0F, 0.0F, 1.0F,
        0.9F, 0.2F, 0.3F, -0.1F,
        0.4F, 0.5F, 0.6F, 2.0F
    }};
    std::array<unsigned char, 27> actual{};
    actual.fill(0xAD);
    auto expected = actual;
    reference_row(input.data(), expected.data(), 5U);
    pvt::display::convert_rgba_row(input.data(), actual.data(), 5U);
    pvt::display::convert_rgba_row(nullptr, nullptr, 0U);
    if (actual != expected) {
        std::cerr << "RGBA row order, alpha, or padding changed.\n";
        return false;
    }
    return true;
}

bool exhaustive_conversion() {
    for (std::uint32_t bits = 0; bits <= 0x3F800000U; ++bits) {
        if (!check_sample(from_bits(bits))) return false;
    }
    std::cout << "All 1,065,353,217 positive float32 inputs in [0,1] match.\n";
    return true;
}

bool benchmark(std::size_t width, std::size_t height) {
    const std::size_t pixels = width * height;
    std::vector<float> input(pixels * 4U);
    std::uint32_t state = 0x50565431U;
    for (std::size_t component = 0; component < input.size(); ++component) {
        state = state * 1664525U + 1013904223U;
        input[component] = static_cast<float>(state >> 8U) / 16777215.0F;
    }
    std::vector<unsigned char> expected(input.size());
    std::vector<unsigned char> actual(input.size());
    reference_row(input.data(), expected.data(), pixels);
    pvt::display::convert_rgba_row(input.data(), actual.data(), pixels);
    if (actual != expected) return false;
    std::array<double, 9> before{};
    std::array<double, 9> after{};
    for (std::size_t iteration = 0; iteration < before.size(); ++iteration) {
        const auto begin = std::chrono::steady_clock::now();
        reference_row(input.data(), expected.data(), pixels);
        const auto middle = std::chrono::steady_clock::now();
        pvt::display::convert_rgba_row(input.data(), actual.data(), pixels);
        const auto end = std::chrono::steady_clock::now();
        before[iteration] = std::chrono::duration<double, std::milli>(middle - begin).count();
        after[iteration] = std::chrono::duration<double, std::milli>(end - middle).count();
        if (actual != expected) return false;
    }
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    std::cout << width << 'x' << height << " RGBA display conversion, median of 9: "
              << before[4] << " ms -> " << after[4] << " ms ("
              << before[4] / after[4] << "x), identical bytes.\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    static_assert(sizeof(float) == sizeof(std::uint32_t)
                      && std::numeric_limits<float>::is_iec559,
                  "Display boundary tests require IEEE float32.");
    if (!test_conversion()) return 1;
    if (argc > 1 && std::string(argv[1]) == "--exhaustive") {
        if (!exhaustive_conversion()) return 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--benchmark") {
        if (!benchmark(1280U, 720U) || !benchmark(1920U, 1080U)
            || !benchmark(3840U, 2160U)) return 1;
    }
    std::cout << "Display conversion tests passed.\n";
    return 0;
}
