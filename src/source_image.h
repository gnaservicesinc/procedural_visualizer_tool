#ifndef PVT_SOURCE_IMAGE_H
#define PVT_SOURCE_IMAGE_H

#include "procedural_visualizer_tool.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace pvt::detail {

PVT_API bool validate_starting_image_source(const std::string& path,
                                            std::string* error);

PVT_API bool load_starting_image_source(const std::string& path,
                                        std::shared_ptr<const Image>& image,
                                        const std::atomic_bool* cancel,
                                        std::string* error);

// Data images bypass display-transfer conversion. PNG sample codes are
// normalized directly and HALF/FLOAT OpenEXR samples stay linear float32.
// This is the required path for height fields, masks, and future data maps.
PVT_API bool validate_data_image_source(const std::string& path,
                                        std::string* error);

PVT_API bool load_data_image_source(const std::string& path,
                                    std::shared_ptr<const Image>& image,
                                    const std::atomic_bool* cancel,
                                    std::string* error);

// Height maps retain the same double-precision luminance used by the mesh
// sampler, without retaining four float channels per pixel. In particular an
// 8192 x 8192 PNG fits the bounded 512 MiB decoded-source cache.
struct HeightImage {
    int width = 0;
    int height = 0;
    std::vector<double> samples;
};

PVT_API bool validate_height_image_source(const std::string& path,
                                          std::string* error);

PVT_API bool load_height_image_source(const std::string& path,
                                      std::shared_ptr<const HeightImage>& image,
                                      const std::atomic_bool* cancel,
                                      std::string* error);

// Environment maps share the bounded image cache while retaining their
// authored transfer interpretation. Auto uses PNG color metadata and linear
// OpenEXR; Srgb forces the standard sRGB transfer; Linear preserves sample
// values exactly.
PVT_API bool validate_environment_map_source(
    const std::string& path, EnvironmentMapEncoding encoding,
    std::string* error);

PVT_API bool load_environment_map_source(
    const std::string& path, EnvironmentMapEncoding encoding,
    std::shared_ptr<const Image>& image, const std::atomic_bool* cancel,
    std::string* error);

bool render_starting_image(const StartingImageConfig& source,
                           int destination_width,
                           int destination_height,
                           Image& destination,
                           const std::atomic_bool* cancel,
                           std::string* error);

} // namespace pvt::detail

#endif
