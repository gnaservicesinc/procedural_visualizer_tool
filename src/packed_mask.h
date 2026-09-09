#ifndef PVT_PACKED_MASK_H
#define PVT_PACKED_MASK_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pvt::detail {

// Frame-local coverage, with one bit per pixel. Writers must own the whole
// mask (or disjoint words); setting adjacent bits is a read-modify-write.
class PackedMask {
public:
    static constexpr std::size_t storage_bytes(std::size_t bits) noexcept {
        return (bits / 64U + (bits % 64U != 0U ? 1U : 0U))
               * sizeof(std::uint64_t);
    }

    explicit PackedMask(std::size_t bits)
        : words_(bits / 64U + (bits % 64U != 0U ? 1U : 0U), 0U) {}

    bool test(std::size_t bit) const noexcept {
        return (words_[bit / 64U] & (std::uint64_t{1} << (bit % 64U))) != 0U;
    }

    void set(std::size_t bit) noexcept {
        words_[bit / 64U] |= std::uint64_t{1} << (bit % 64U);
    }

private:
    std::vector<std::uint64_t> words_;
};

} // namespace pvt::detail

#endif
