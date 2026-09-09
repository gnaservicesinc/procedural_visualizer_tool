#ifndef PVT_RASTER_OCCLUSION_H
#define PVT_RASTER_OCCLUSION_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace pvt::detail {

// Outward-rounded arithmetic encloses the rasterizer's rounded edge/depth
// expressions, including contracted multiply/add. An uncertain/nonfinite
// bound simply disables rejection; no visual epsilon is introduced.
struct RasterInterval {
    double low, high;
    static double down(double x) {
        return std::nextafter(x, -std::numeric_limits<double>::infinity());
    }
    static double up(double x) {
        return std::nextafter(x, std::numeric_limits<double>::infinity());
    }
    friend RasterInterval operator+(RasterInterval a, RasterInterval b) {
        return {down(a.low + b.low), up(a.high + b.high)};
    }
    friend RasterInterval operator-(RasterInterval a, RasterInterval b) {
        return {down(a.low - b.high), up(a.high - b.low)};
    }
    friend RasterInterval operator*(RasterInterval a, RasterInterval b) {
        const std::array<double, 4U> products = {a.low * b.low, a.low * b.high, a.high * b.low,
                                                 a.high * b.high};
        if (std::any_of(products.begin(), products.end(),
                        [](double x) { return !std::isfinite(x); })) {
            return {-std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity()};
        }
        return {down(*std::min_element(products.begin(), products.end())),
                up(*std::max_element(products.begin(), products.end()))};
    }
    RasterInterval divide_positive(double divisor) const {
        return {down(low / divisor), up(high / divisor)};
    }
};

// Each tile becomes usable only after EVERY pixel has depth coverage. Once
// complete, its maximum is a conservative upper bound forever: nearest-hit
// depth writes can only decrease. No atomics: one render owns this structure.
class RasterOcclusion {
    struct Tile {
        float maximum = std::numeric_limits<float>::infinity();
        unsigned char count = 0U;
    };
    int width_, height_, columns_;
    std::vector<Tile> tiles_;
    bool any_complete_ = false;

  public:
    static constexpr int side = 8;
    static constexpr std::size_t tile_bytes = sizeof(Tile);
    RasterOcclusion(int width, int height, bool enabled)
        : width_(width), height_(height), columns_(width / side + (width % side != 0)),
          tiles_(enabled ? static_cast<std::size_t>(columns_) *
                               static_cast<std::size_t>(height / side + (height % side != 0))
                         : 0U) {}
    bool enabled() const { return !tiles_.empty(); }
    bool has_coverage() const { return any_complete_; }
    void first_hit(std::size_t pixel, const std::vector<float>& depth) {
        if (!enabled()) return;
        const int x = static_cast<int>(pixel % static_cast<std::size_t>(width_));
        const int y = static_cast<int>(pixel / static_cast<std::size_t>(width_));
        auto& tile =
            tiles_[static_cast<std::size_t>(y / side) * static_cast<std::size_t>(columns_) +
                   static_cast<std::size_t>(x / side)];
        const int left = x / side * side, top = y / side * side;
        const int right = left + std::min(side, width_ - left);
        const int bottom = top + std::min(side, height_ - top);
        if (++tile.count != (right - left) * (bottom - top)) return;
        any_complete_ = true;
        tile.maximum = 0.0F;
        for (int row = top; row < bottom; ++row) {
            for (int col = left; col < right; ++col) {
                tile.maximum = std::max(
                    tile.maximum,
                    depth[static_cast<std::size_t>(row) * static_cast<std::size_t>(width_) +
                          static_cast<std::size_t>(col)]);
            }
        }
    }
    bool occludes(int left, int right, int top, int bottom, double minimum_depth) const {
        if (!enabled() || !(minimum_depth > 0.0) || !std::isfinite(minimum_depth)) return false;
        for (int y = top / side; y <= bottom / side; ++y) {
            for (int x = left / side; x <= right / side; ++x) {
                if (!(tiles_[static_cast<std::size_t>(y) * static_cast<std::size_t>(columns_) +
                             static_cast<std::size_t>(x)]
                          .maximum < minimum_depth))
                    return false;
            }
        }
        return true;
    }
};

} // namespace pvt::detail
#endif
