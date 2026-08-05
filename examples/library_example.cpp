#include "procedural_visualizer_tool.h"

#include <cstdlib>
#include <iostream>

int main() {
    pvt::RenderConfig config = pvt::default_config();
    config.width = 640;
    config.height = 360;
    config.block_size = 4;
    config.alpha.enabled = true;
    config.output.bit_depth = 16;

    pvt::EffectConfig ripple = pvt::default_effect(pvt::EffectType::Ripple);
    ripple.id = pvt::allocate_id(config);
    ripple.enabled = true;
    ripple.synchronized = false;
    ripple.cycles_per_loop = 2;
    ripple.edge_mode = pvt::EdgeMode::Reflect;
    config.effects.push_back(ripple);

    const pvt::ValidationResult validation = pvt::validate(config);
    if (!validation.ok) {
        std::cerr << validation.message << '\n';
        return EXIT_FAILURE;
    }

    pvt::Image image;
    std::string error;
    if (!pvt::render_frame(config, 0, image, &error)
        || !pvt::write_image("library_example.png", image, config, 0U, &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Wrote a 16-bit RGBA frame to library_example.png\n";
    return EXIT_SUCCESS;
}

