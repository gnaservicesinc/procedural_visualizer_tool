#include "../gui/palette_remix.h"
#include "../src/config_codec.h"

#include <iostream>

namespace {
bool same(const pvt::PaletteConfig& a, const pvt::PaletteConfig& b) {
    if (a.enabled != b.enabled || a.name != b.name || a.columns != b.columns
        || a.colors.size() != b.colors.size()) return false;
    for (std::size_t i = 0; i < a.colors.size(); ++i) {
        const auto& x = a.colors[i];
        const auto& y = b.colors[i];
        if (x.red != y.red || x.green != y.green || x.blue != y.blue
            || x.alpha != y.alpha || x.name != y.name || x.encoding != y.encoding) return false;
    }
    return true;
}
}

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << message << '\n'; }
    };
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-12; };
    using pvt::palette_remix::Settings;
    using pvt::palette_remix::apply;
    pvt::PaletteConfig palette;
    palette.name = "Remix fixture";
    palette.columns = 3;
    palette.colors = {{1.0, 0.0, 0.0, 0.25, "Red"},
                      {0.0, 1.0, 0.0, 0.5, "Green"},
                      {0.0, 0.0, 1.0, 0.75, "Blue"},
                      {4.0, -0.1, 0.25, 0.123456789012345, "HDR",
                       pvt::PaletteColorEncoding::Linear}};
    const auto original = palette;
    check(same(apply(palette, {}), palette), "Neutral remix must be exact, including HDR and metadata.");
    Settings settings;
    settings.hue_degrees = 360;
    check(same(apply(palette, settings), palette), "A full hue turn must be an exact no-op.");
    settings.hue_degrees = 120;
    const auto shifted = apply(palette, settings);
    check(close(shifted.colors[0].green, 1) && close(shifted.colors[0].red, 0)
          && close(shifted.colors[1].blue, 1) && close(shifted.colors[2].red, 1),
          "Hue rotation must move red to green, green to blue, and blue to red.");
    settings.hue_degrees = -120;
    const auto backwards = apply(palette, settings);
    check(close(backwards.colors[0].blue, 1), "Negative hue rotation must go in the opposite direction.");
    check(shifted.colors[3].green > 1 && shifted.colors[3].blue < 0
          && shifted.colors[3].encoding == pvt::PaletteColorEncoding::Linear,
          "HDR hue edits must retain extended positive and negative values and linear encoding.");
    settings = {};
    settings.exposure_stops = 1;
    const auto brighter = apply(palette, settings);
    check(brighter.colors[3].red == 8 && brighter.colors[3].green == -0.2,
          "One stop must double linear light, including signed HDR components.");
    auto midtone = palette;
    midtone.colors = {{0.25, 0.25, 0.25, 0.333, "Gray"}};
    const auto light = apply(midtone, settings);
    check(close(pvt::palette_remix::decode(light.colors[0].red),
                2 * pvt::palette_remix::decode(0.25)),
          "Exposure of sRGB values must happen in linear light.");
    settings = {};
    settings.saturation = 0;
    for (const auto& color : apply(palette, settings).colors)
        check(close(color.red, color.green) && close(color.red, color.blue),
              "Zero saturation must give neutral grays in both encodings.");
    settings = {};
    settings.reverse = true;
    settings.offset = 1;
    const auto reordered = apply(palette, settings);
    check(reordered.colors[0].name == "Blue" && reordered.colors[0].alpha == 0.75
          && reordered.colors.back().name == "HDR"
          && reordered.colors.back().red == 4.0,
          "Reordering must move whole entries with exact alpha, names, and encoding.");
    settings.offset += palette.colors.size();
    check(same(reordered, apply(palette, settings)), "Order offsets must wrap by palette length.");
    check(apply(pvt::PaletteConfig{}, settings).colors.empty(), "Empty palettes must be safe.");
    settings = {};
    settings.hue_degrees = std::numeric_limits<double>::quiet_NaN();
    check(same(apply(palette, settings), palette), "Non-finite controls must leave the palette alone.");
    settings.hue_degrees = 95;
    settings.exposure_stops = 2;
    settings.saturation = 2;
    auto limits = palette;
    const double limit = (std::numeric_limits<float>::max)();
    limits.colors = {{limit, -limit, limit, 1, "Extreme", pvt::PaletteColorEncoding::Linear}};
    for (double value : {apply(limits, settings).colors[0].red,
                         apply(limits, settings).colors[0].green,
                         apply(limits, settings).colors[0].blue})
        check(std::isfinite(value) && std::abs(value) <= limit,
              "Extended colors must stay finite and within the renderer's float range.");
    for (std::size_t i = 0; i < palette.colors.size(); ++i)
        check(shifted.colors[i].alpha == palette.colors[i].alpha
              && shifted.colors[i].name == palette.colors[i].name,
              "Color edits must preserve exact alpha and entry names.");
    check(same(palette, original), "Remixing must never modify the source palette.");

    // Existing persistence and CPU rendering must consume the resulting ordinary
    // palette directly; the creative tool introduces no runtime or format feature.
    auto config = pvt::default_config();
    config.width = 32;
    config.height = 32;
    config.palette = pvt::default_palette(0);
    config.palette.enabled = true;
    config.output.write_alpha = true;
    pvt::Image before_image, after_image;
    std::string error, serialized;
    check(pvt::render_frame(config, 0, before_image, &error), "Original palette must render.");
    settings = {};
    settings.hue_degrees = 120;
    config.palette = apply(config.palette, settings);
    check(pvt::render_frame(config, 0, after_image, &error), "Remixed palette must render.");
    check(before_image.pixels != after_image.pixels, "Remixing a warm palette must visibly change its rendered artwork.");
    pvt::RenderConfig loaded;
    check(pvt::detail::serialize_setup_config(config, serialized, &error)
          && pvt::detail::deserialize_setup_config(serialized, loaded, &error)
          && same(loaded.palette, config.palette), "Applied colors must round-trip through the existing codec.");
    return failures == 0 ? 0 : 1;
}
