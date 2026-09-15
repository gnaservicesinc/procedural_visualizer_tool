#include "photo_depth.h"
#include "environment_map.h"
#include <algorithm>
#include <cmath>
namespace pvt::detail {
SurfaceConfig photo_surface(const StartingImageConfig& photo, const SurfaceConfig& surface) {
    auto result = surface;
    if (photo.enabled && photo.depth_enabled && surface.mapping == SurfaceMapping::Plane) {
        for (const auto& image : photo.derived_images) if (image.kind == "depth") {
            result.plane_displacement.photo_depth_path = image.path;
            result.plane_displacement.photo_depth_source_path = photo.path;
            result.plane_displacement.photo_depth_amount = photo.depth_amount;
            result.plane_displacement.photo_depth_fit = photo.fit;
            break;
        }
    }
    return result;
}
double fitted_photo_height(const HeightImage& image, StartingImageFit fit, int width, int height,
                           double u, double v, int source_width, int source_height) {
    if (source_width <= 0) source_width = image.width;
    if (source_height <= 0) source_height = image.height;
    // Match render_starting_image's pixel centers, using the color image's
    // dimensions even when the auxiliary map has a lower resolution.
    double x, y;
    if (fit == StartingImageFit::Stretch) {
        x = u * image.width - .5; y = v * image.height - .5;
    } else if (fit == StartingImageFit::Tile) {
        x = std::fmod(u * width, source_width) * image.width / source_width - .5;
        y = std::fmod(v * height, source_height) * image.height / source_height - .5;
        if (x < -.5) x += image.width;
        if (y < -.5) y += image.height;
    } else {
        double scale_x = double(width) / source_width, scale_y = double(height) / source_height;
        double scale = fit == StartingImageFit::Contain ? std::min(scale_x, scale_y) : std::max(scale_x, scale_y);
        x = (((u - .5) * width - .5) / scale + source_width * .5 + .5) * image.width / source_width - .5;
        y = (((v - .5) * height - .5) / scale + source_height * .5 + .5) * image.height / source_height - .5;
        if (fit == StartingImageFit::Contain && (x < -.5 || y < -.5 || x >= image.width-.5 || y >= image.height-.5)) return .5;
    }
    x = std::clamp(x, 0., double(image.width - 1)); y = std::clamp(y, 0., double(image.height - 1));
    int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
    auto at = [&](int px, int py) { return image.samples[std::size_t(py) * std::size_t(image.width) + std::size_t(px)]; };
    double top = at(x0,y0) * (1 - (x-x0)) + at(x1,y0) * (x-x0);
    double bottom = at(x0,y1) * (1 - (x-x0)) + at(x1,y1) * (x-x0);
    return std::clamp(top * (1 - (y-y0)) + bottom * (y-y0), 0., 1.);
}
bool apply_photo_depth(const StartingImageConfig& photo, const SurfaceConfig& surface, Image& image,
                       const std::atomic_bool* cancel, std::string* error) {
    if (!photo.enabled || !photo.depth_enabled || photo.depth_amount == 0) return true;
    if (!photo.depth_lighting && photo.depth_tilt_x == 0 && photo.depth_tilt_y == 0) return true;
    const auto found = std::find_if(photo.derived_images.begin(), photo.derived_images.end(), [](const auto& a) { return a.kind == "depth"; });
    if (found == photo.derived_images.end()) { if (error) *error = "Photo depth is missing."; return false; }
    std::shared_ptr<const HeightImage> depth;
    if (!load_height_image_source(found->path, depth, cancel, error)) return false;
    std::shared_ptr<const Image> source;
    if (!load_starting_image_source(photo.path, source, cancel, error)) return false;
    const auto height = [&](double u, double v) { return (fitted_photo_height(*depth, photo.fit, image.width, image.height, u, v, source->width, source->height) - .5) * photo.depth_amount; };
    const double dx = 1. / image.width, dy = 1. / image.height;
    const double tilt_x = std::tan(photo.depth_tilt_y * 3.141592653589793 / 180.);
    const double tilt_y = std::tan(photo.depth_tilt_x * 3.141592653589793 / 180.);
    PreparedEnvironmentMap environment;
    if (photo.depth_lighting && surface.environment_map.enabled
        && !prepare_environment_map(surface.environment_map, environment, cancel, error)) return false;
    double lx = surface.light_direction_x, ly = surface.light_direction_y, lz = surface.light_direction_z;
    // Dominant environment direction approximates the shadow-casting source;
    // diffuse color still uses the shared environment sampler below.
    if (environment) {
        double brightest = -1;
        for (int y = 0; y < environment.image->height; y += std::max(1, environment.image->height / 32))
            for (int x = 0; x < environment.image->width; x += std::max(1, environment.image->width / 64)) {
                std::size_t i = (std::size_t(y) * std::size_t(environment.image->width) + std::size_t(x)) * 4;
                const auto& pixels = environment.image->pixels;
                double value = pixels[i] * .2126 + pixels[i+1] * .7152 + pixels[i+2] * .0722;
                if (value > brightest) {
                    brightest = value;
                    double longitude = ((double(x) / environment.image->width) - .5 - environment.rotation_turns) * 6.283185307179586;
                    double latitude = (double(y) / environment.image->height) * 3.141592653589793;
                    lx = std::sin(longitude) * std::sin(latitude); ly = std::cos(latitude); lz = std::cos(longitude) * std::sin(latitude);
                }
            }
    }
    double length = std::sqrt(lx*lx + ly*ly + lz*lz); if (length > 1e-12) { lx /= length; ly /= length; lz /= length; }
    Image result; result.width = image.width; result.height = image.height; result.pixels.resize(image.pixels.size());
    for (int y = 0; y < image.height; ++y) {
        if (cancel && cancel->load()) { if (error) *error = "Photo rendering cancelled."; return false; }
        for (int x = 0; x < image.width; ++x) {
            double u = (x+.5)*dx, v = (y+.5)*dy, su = u, sv = v;
            for (int iteration = 0; iteration < 3; ++iteration) { double h = height(su,sv); su = u - tilt_x*h; sv = v + tilt_y*h; }
            double light[3]{1,1,1};
            if (photo.depth_lighting) {
                double nx = -(height(su+dx,sv)-height(su-dx,sv))/(2*dx);
                double ny = (height(su,sv+dy)-height(su,sv-dy))/(2*dy);
                double inv = 1/std::sqrt(nx*nx+ny*ny+1); nx *= inv; ny *= inv;
                double visible = 1;
                if (lz > 0) for (int step = 1; step <= 24; ++step) {
                    double distance = step / 96.;
                    double tu = su + lx*distance, tv = sv - ly*distance;
                    if (tu < 0 || tu > 1 || tv < 0 || tv > 1) break;
                    if (height(tu,tv) > height(su,sv) + lz*distance + .002) { visible = 0; break; }
                }
                double diffuse = surface.light_ambient + surface.light_diffuse * std::max(0., nx*lx + ny*ly + inv*lz) * visible;
                light[0] = light[1] = light[2] = diffuse;
                if (environment) {
                    auto color = sample_environment_map_diffuse(environment,nx,ny,inv);
                    double channels[]{color.red,color.green,color.blue};
                    for (int c = 0; c < 3; ++c) light[c] = diffuse*(1-environment.mix) + channels[c]*(.28+.72*visible)*environment.mix;
                }
            }
            double px = su*image.width-.5, py = sv*image.height-.5;
            std::size_t target = (std::size_t(y)*std::size_t(image.width)+std::size_t(x))*4;
            if (px < 0 || py < 0 || px > image.width-1 || py > image.height-1) {
                for (int c=0;c<4;++c) result.pixels[target+std::size_t(c)] = 0;
                continue;
            }
            int x0=int(px), y0=int(py), x1=std::min(x0+1,image.width-1), y1=std::min(y0+1,image.height-1);
            double sums[4]{};
            for (int yy=0; yy<2; ++yy) for (int xx=0; xx<2; ++xx) {
                int sx = xx ? x1 : x0, sy = yy ? y1 : y0;
                double weight = (xx ? px-x0 : 1-(px-x0))*(yy ? py-y0 : 1-(py-y0));
                std::size_t index = (std::size_t(sy)*std::size_t(image.width)+std::size_t(sx))*4;
                double alpha = image.pixels[index+3]; sums[3] += alpha*weight;
                for (int c=0;c<3;++c) sums[c] += image.pixels[index+std::size_t(c)]*alpha*weight;
            }
            for (int c=0;c<3;++c) result.pixels[target+std::size_t(c)] = float(sums[3] > 1e-12 ? sums[c]/sums[3]*light[c] : 0);
            result.pixels[target+3] = float(sums[3]);
        }
    }
    image = std::move(result); return true;
}
}
