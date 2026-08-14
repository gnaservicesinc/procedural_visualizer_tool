#ifndef PVT_SOURCE_IMAGE_H
#define PVT_SOURCE_IMAGE_H

#include "procedural_visualizer_tool.h"

#include <atomic>
#include <memory>
#include <string>

namespace pvt::detail {

bool validate_starting_image_source(const std::string& path,
                                    std::string* error);

bool load_starting_image_source(const std::string& path,
                                std::shared_ptr<const Image>& image,
                                const std::atomic_bool* cancel,
                                std::string* error);

bool render_starting_image(const StartingImageConfig& source,
                           int destination_width,
                           int destination_height,
                           Image& destination,
                           const std::atomic_bool* cancel,
                           std::string* error);

} // namespace pvt::detail

#endif
