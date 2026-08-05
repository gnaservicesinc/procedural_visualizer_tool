#include <procedural_visualizer_tool.h>

#include <cstdlib>
#include <string>

int main() {
    pvt::RenderConfig config = pvt::default_config();
    config.width = 32;
    config.height = 24;
    config.block_size = 4;
    pvt::Image image;
    std::string error;
    return pvt::validate(config).ok && pvt::render_frame(config, 0, image, &error)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

