#ifndef PVT_PALETTE_IO_H
#define PVT_PALETTE_IO_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pvt::palette_io {

// Palette interchange intentionally lives outside the renderer ABI.  The GUI
// retains the encoding and exact finite values here when mapping entries into
// the renderer, so linear/HDR sources are not silently forced through sRGB.
enum class ColorEncoding {
    SRGB,
    Linear
};

enum class PaletteFormat {
    Auto,
    GimpGpl,
    KritaKpl,
    CssStylesheet,
    PythonDictionary,
    PhpDictionary,
    JavaMap,
    TextHex,
    PngImage,
    FloatExrImage
};

struct PaletteEntry {
    std::string name;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;
    ColorEncoding source_encoding = ColorEncoding::SRGB;
    std::size_t source_order = 0U;
};

struct PaletteDocument {
    std::string name = "Imported Palette";
    std::vector<PaletteEntry> entries;
    std::optional<std::size_t> columns;
};

struct PaletteIoSummary {
    std::size_t scanned = 0U;
    std::size_t transparent_ignored = 0U;
    std::size_t duplicates_ignored = 0U;
    std::size_t accepted = 0U;
    std::size_t skipped = 0U;
    std::size_t unsupported = 0U;

    bool names_lost = false;
    bool alpha_lost = false;
    bool precision_lost = false;
    bool encoding_converted = false;
    std::vector<std::string> warnings;
};

// Central hostile-input limits.  They are deliberately much smaller than the
// project-bundle signed-int ceiling because a usable palette is small.
inline constexpr std::size_t kMaximumPaletteFileBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaximumPaletteDecodedBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaximumPaletteEntries = 1U * 1024U * 1024U;
inline constexpr std::size_t kMaximumPaletteTextLineBytes = 1U * 1024U * 1024U;
inline constexpr std::size_t kMaximumKplArchiveEntries = 64U;
inline constexpr std::size_t kMaximumKplExpandedBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaximumKplXmlBytes = 16U * 1024U * 1024U;

PaletteFormat format_from_path(const std::string& path);
const char* format_name(PaletteFormat format);

// Imports are transactional: destination and summary change only after the
// complete source has passed structural and numeric validation.  Code formats
// are parsed as data; no interpreter, compiler, shell, or macro engine is ever
// invoked.
bool import_palette(const std::string& path,
                    PaletteFormat format,
                    PaletteDocument& destination,
                    PaletteIoSummary& summary,
                    std::string* error = nullptr);

// Exports use a sibling temporary and an atomic/no-clobber install.  Set
// overwrite only after the UI has obtained an explicit replacement decision.
bool export_palette(const std::string& path,
                    PaletteFormat format,
                    const PaletteDocument& source,
                    bool overwrite,
                    PaletteIoSummary& summary,
                    std::string* error = nullptr);

} // namespace pvt::palette_io

#endif
