#ifndef PVT_PHOTO_DEPTH_H
#define PVT_PHOTO_DEPTH_H
#include "source_image.h"
namespace pvt::detail {
PVT_API SurfaceConfig photo_surface(const StartingImageConfig& photo, const SurfaceConfig& surface);
PVT_API double fitted_photo_height(const HeightImage& image, StartingImageFit fit,
                                   int width, int height, double u, double v,
                                   int source_width = 0, int source_height = 0);
PVT_API bool apply_photo_depth(const StartingImageConfig& photo, const SurfaceConfig& surface,
                               Image& image, const std::atomic_bool* cancel, std::string* error);
}
#endif
