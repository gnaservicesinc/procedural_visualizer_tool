#include "palette_io.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace palette = pvt::palette_io;

int failures = 0;

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #expression << '\n';               \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

bool close(double left, double right, double tolerance = 1e-6) {
    return std::abs(left - right) <= tolerance;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    CHECK(static_cast<bool>(output));
}

palette::PaletteDocument sample_document() {
    palette::PaletteDocument result;
    result.name = "Interchange Test";
    result.columns = 2U;
    result.entries = {
        {"Duplicate Name", 1.0, 0.25, 0.0, 1.0,
         palette::ColorEncoding::SRGB, 0U},
        {"Duplicate Name", 0.0, 0.5, 1.0, 0.5,
         palette::ColorEncoding::SRGB, 1U},
        {"HDR Linear", 3.5, -0.25, 0.125, 0.75,
         palette::ColorEncoding::Linear, 2U}
    };
    return result;
}

void test_format_detection() {
    CHECK(palette::format_from_path("palette.GPL") == palette::PaletteFormat::GimpGpl);
    CHECK(palette::format_from_path("palette.kpl") == palette::PaletteFormat::KritaKpl);
    CHECK(palette::format_from_path("palette.css")
          == palette::PaletteFormat::CssStylesheet);
    CHECK(palette::format_from_path("palette.py")
          == palette::PaletteFormat::PythonDictionary);
    CHECK(palette::format_from_path("palette.php")
          == palette::PaletteFormat::PhpDictionary);
    CHECK(palette::format_from_path("palette.java") == palette::PaletteFormat::JavaMap);
    CHECK(palette::format_from_path("palette.txt") == palette::PaletteFormat::TextHex);
    CHECK(palette::format_from_path("palette.png") == palette::PaletteFormat::PngImage);
    CHECK(palette::format_from_path("palette.exr")
          == palette::PaletteFormat::FloatExrImage);
}

void test_gimp_text_imports(const fs::path& directory) {
    const fs::path gpl = directory / "sample.gpl";
    write_text(gpl,
               "GIMP Palette\nName: Example\nColumns: 2\n# comment\n"
               "255 115 0 Fire\n0 0 0 Black\nnot a color\n");
    palette::PaletteDocument imported;
    palette::PaletteIoSummary summary;
    std::string error;
    CHECK(palette::import_palette(gpl.string(), palette::PaletteFormat::Auto,
                                  imported, summary, &error));
    CHECK(error.empty());
    CHECK(imported.name == "Example");
    CHECK(imported.columns == 2U);
    CHECK(imported.entries.size() == 2U);
    CHECK(imported.entries[0U].name == "Fire");
    CHECK(imported.entries[0U].source_encoding == palette::ColorEncoding::SRGB);
    CHECK(summary.scanned == 3U);
    CHECK(summary.skipped == 1U);

    const fs::path text = directory / "sample.txt";
    write_text(text, "#ff7300\n#000000ff Black\n#bad\n");
    imported = {};
    summary = {};
    CHECK(palette::import_palette(text.string(), palette::PaletteFormat::Auto,
                                  imported, summary, &error));
    CHECK(imported.entries.size() == 2U);
    CHECK(imported.entries[1U].name == "Black");
    CHECK(summary.skipped == 1U);
}

void test_code_import_never_executes(const fs::path& directory) {
    const fs::path sentinel = directory / "must-not-exist";
    const fs::path python = directory / "hostile.py";
    write_text(python,
               "import os\n"
               "os.system('touch must-not-exist')\n"
               "colors = {\n"
               "'One': '#010203',\n"
               "'One': '#04050680',\n"
               "}\n");
    palette::PaletteDocument imported;
    palette::PaletteIoSummary summary;
    std::string error;
    CHECK(palette::import_palette(python.string(), palette::PaletteFormat::Auto,
                                  imported, summary, &error));
    CHECK(imported.entries.size() == 2U);
    CHECK(imported.entries[0U].name == "One");
    CHECK(imported.entries[1U].name == "One");
    CHECK(!fs::exists(sentinel));
}

void test_code_round_trips(const fs::path& directory) {
    const palette::PaletteDocument source = sample_document();
    const std::vector<std::pair<std::string, palette::PaletteFormat>> formats = {
        {"palette.css", palette::PaletteFormat::CssStylesheet},
        {"palette.py", palette::PaletteFormat::PythonDictionary},
        {"palette.php", palette::PaletteFormat::PhpDictionary},
        {"Palette.java", palette::PaletteFormat::JavaMap}
    };
    for (const auto& format : formats) {
        const fs::path path = directory / format.first;
        palette::PaletteIoSummary exported;
        std::string error;
        CHECK(palette::export_palette(path.string(), format.second, source, false,
                                      exported, &error));
        CHECK(error.empty());
        CHECK(exported.accepted == source.entries.size());
        CHECK(exported.precision_lost);
        CHECK(exported.encoding_converted);
        palette::PaletteDocument imported;
        palette::PaletteIoSummary import_summary;
        CHECK(palette::import_palette(path.string(), format.second, imported,
                                      import_summary, &error));
        CHECK(imported.entries.size() == source.entries.size());
        if (imported.entries.size() == source.entries.size()) {
            CHECK(imported.entries[0U].name == "Duplicate Name");
            CHECK(imported.entries[1U].name == "Duplicate Name");
            CHECK(imported.entries[2U].name == "HDR Linear");
        }
    }
}

void test_css_comment_name_is_escaped(const fs::path& directory) {
    palette::PaletteDocument source;
    source.name = "CSS Comment Safety";
    source.entries = {{"*/ body { color: red; } /*", 1.0, 0.0, 0.0, 1.0,
                       palette::ColorEncoding::SRGB, 0U}};
    const fs::path path = directory / "hostile-name.css";
    palette::PaletteIoSummary summary;
    std::string error;
    CHECK(palette::export_palette(path.string(), palette::PaletteFormat::Auto,
                                  source, false, summary, &error));
    std::ifstream input(path, std::ios::binary);
    const std::string css((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    CHECK(css.find("*/ body { color: red; }") == std::string::npos);
    CHECK(css.find("*\\/ body { color: red; }") != std::string::npos);

    palette::PaletteDocument imported;
    summary = {};
    CHECK(palette::import_palette(path.string(), palette::PaletteFormat::Auto,
                                  imported, summary, &error));
    CHECK(imported.entries.size() == 1U);
    if (imported.entries.size() == 1U) {
        CHECK(imported.entries[0U].name == source.entries[0U].name);
    }
}

void test_loss_summaries_and_no_clobber(const fs::path& directory) {
    const palette::PaletteDocument source = sample_document();
    palette::PaletteIoSummary summary;
    std::string error;
    const fs::path gpl = directory / "export.gpl";
    CHECK(palette::export_palette(gpl.string(), palette::PaletteFormat::Auto,
                                  source, false, summary, &error));
    CHECK(summary.alpha_lost);
    CHECK(summary.precision_lost);
    palette::PaletteIoSummary unchanged = summary;
    CHECK(!palette::export_palette(gpl.string(), palette::PaletteFormat::Auto,
                                   source, false, unchanged, &error));
    CHECK(!error.empty());
    CHECK(palette::export_palette(gpl.string(), palette::PaletteFormat::Auto,
                                  source, true, summary, &error));

    const fs::path text = directory / "export.txt";
    CHECK(palette::export_palette(text.string(), palette::PaletteFormat::Auto,
                                  source, false, summary, &error));
    CHECK(summary.names_lost);
    CHECK(!summary.alpha_lost);
}

void test_krita_round_trip(const fs::path& directory) {
    const palette::PaletteDocument source = sample_document();
    const fs::path path = directory / "palette.kpl";
    palette::PaletteIoSummary summary;
    std::string error;
    CHECK(palette::export_palette(path.string(), palette::PaletteFormat::Auto,
                                  source, false, summary, &error));
    CHECK(!summary.precision_lost);
    CHECK(!summary.names_lost);
    CHECK(!summary.alpha_lost);
    palette::PaletteDocument imported;
    palette::PaletteIoSummary imported_summary;
    CHECK(palette::import_palette(path.string(), palette::PaletteFormat::Auto,
                                  imported, imported_summary, &error));
    CHECK(error.empty());
    CHECK(imported.name == source.name);
    CHECK(imported.columns == source.columns);
    CHECK(imported.entries.size() == source.entries.size());
    if (imported.entries.size() == source.entries.size()) {
        CHECK(imported.entries[0U].name == source.entries[0U].name);
        CHECK(imported.entries[0U].source_encoding == palette::ColorEncoding::SRGB);
        CHECK(imported.entries[2U].source_encoding == palette::ColorEncoding::Linear);
        CHECK(imported.entries[2U].red == source.entries[2U].red);
        CHECK(imported.entries[2U].green == source.entries[2U].green);
        CHECK(imported.entries[2U].alpha == source.entries[2U].alpha);
    }
}

void test_png_image_semantics(const fs::path& directory) {
    palette::PaletteDocument source;
    source.name = "Image";
    source.columns = 2U;
    source.entries = {
        {"Red", 1.0, 0.0, 0.0, 1.0, palette::ColorEncoding::SRGB, 0U},
        {"Transparent", 0.0, 1.0, 0.0, 0.0, palette::ColorEncoding::SRGB, 1U},
        {"Red again", 1.0, 0.0, 0.0, 1.0, palette::ColorEncoding::SRGB, 2U},
        {"Blue", 0.0, 0.0, 1.0, 0.5, palette::ColorEncoding::SRGB, 3U}
    };
    const fs::path path = directory / "palette.png";
    palette::PaletteIoSummary exported;
    std::string error;
    CHECK(palette::export_palette(path.string(), palette::PaletteFormat::Auto,
                                  source, false, exported, &error));
    CHECK(exported.names_lost);
    palette::PaletteDocument imported;
    palette::PaletteIoSummary summary;
    CHECK(palette::import_palette(path.string(), palette::PaletteFormat::Auto,
                                  imported, summary, &error));
    CHECK(summary.scanned == 4U);
    CHECK(summary.transparent_ignored == 1U);
    CHECK(summary.duplicates_ignored == 1U);
    CHECK(summary.accepted == 2U);
    CHECK(imported.entries.size() == 2U);
    if (imported.entries.size() == 2U) {
        CHECK(imported.entries[0U].source_order == 0U);
        CHECK(imported.entries[1U].source_order == 3U);
        CHECK(imported.entries[0U].source_encoding == palette::ColorEncoding::Linear);
        CHECK(close(imported.entries[1U].alpha, 128.0 / 255.0, 2.0 / 65535.0));
    }
}

void test_exr_image_semantics(const fs::path& directory) {
    palette::PaletteDocument source;
    source.name = "HDR Image";
    source.columns = 2U;
    source.entries = {
        {"HDR", 4.25, -0.5, 0.125, 0.75, palette::ColorEncoding::Linear, 0U},
        {"Transparent", 9.0, 8.0, 7.0, 0.0, palette::ColorEncoding::Linear, 1U},
        {"HDR duplicate", 4.25, -0.5, 0.125, 0.75,
         palette::ColorEncoding::Linear, 2U},
        {"Encoded", 0.5, 0.25, 1.0, 1.0, palette::ColorEncoding::SRGB, 3U}
    };
    const fs::path path = directory / "palette.exr";
    palette::PaletteIoSummary exported;
    std::string error;
    CHECK(palette::export_palette(path.string(), palette::PaletteFormat::Auto,
                                  source, false, exported, &error));
    CHECK(exported.names_lost);
    CHECK(exported.encoding_converted);
    palette::PaletteDocument imported;
    palette::PaletteIoSummary summary;
    CHECK(palette::import_palette(path.string(), palette::PaletteFormat::Auto,
                                  imported, summary, &error));
    CHECK(summary.scanned == 4U);
    CHECK(summary.transparent_ignored == 1U);
    CHECK(summary.duplicates_ignored == 1U);
    CHECK(imported.entries.size() == 2U);
    if (imported.entries.size() == 2U) {
        CHECK(imported.entries[0U].source_encoding == palette::ColorEncoding::Linear);
        CHECK(imported.entries[0U].red == 4.25);
        CHECK(imported.entries[0U].green == -0.5);
        CHECK(imported.entries[0U].blue == 0.125);
        CHECK(imported.entries[0U].alpha == 0.75);
        CHECK(imported.entries[1U].source_order == 3U);
    }
}

void test_non_finite_rejected(const fs::path& directory) {
    palette::PaletteDocument invalid = sample_document();
    invalid.entries[0U].red = std::numeric_limits<double>::quiet_NaN();
    palette::PaletteIoSummary summary;
    std::string error;
    CHECK(!palette::export_palette((directory / "nan.kpl").string(),
                                   palette::PaletteFormat::Auto, invalid, false,
                                   summary, &error));
    CHECK(error.find("NaN") != std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        for (int index = 1; index < argc; ++index) {
            palette::PaletteDocument document;
            palette::PaletteIoSummary summary;
            std::string error;
            if (!palette::import_palette(argv[index], palette::PaletteFormat::Auto,
                                         document, summary, &error)) {
                std::cerr << argv[index] << ": " << error << '\n';
                return 1;
            }
            std::cout << argv[index] << ": " << summary.accepted << " accepted, "
                      << summary.transparent_ignored << " transparent ignored, "
                      << summary.duplicates_ignored << " duplicates ignored, "
                      << summary.unsupported << " unsupported ignored\n";
        }
        return 0;
    }
    test_format_detection();
    const fs::path directory = fs::temp_directory_path()
                               / "pvt-palette-io-focused-tests";
    std::error_code code;
    fs::remove_all(directory, code);
    code.clear();
    fs::create_directories(directory, code);
    if (code) {
        std::cerr << "Could not create palette test directory: " << code.message() << '\n';
        return 1;
    }
    test_gimp_text_imports(directory);
    test_code_import_never_executes(directory);
    test_code_round_trips(directory);
    test_css_comment_name_is_escaped(directory);
    test_loss_summaries_and_no_clobber(directory);
    test_krita_round_trip(directory);
    test_png_image_semantics(directory);
    test_exr_image_semantics(directory);
    test_non_finite_rejected(directory);
    fs::remove_all(directory, code);
    if (failures != 0) {
        std::cerr << failures << " palette I/O test(s) failed\n";
        return 1;
    }
    std::cout << "palette I/O tests passed\n";
    return 0;
}
