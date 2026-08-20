#include "frame_renderer_internal.h"

namespace pvt::detail {

bool opengl_surface_backend_compiled() {
    return false;
}

bool opengl_surface_backend_available(std::string* device_name,
                                      std::string* status) {
    if (device_name != nullptr) device_name->clear();
    if (status != nullptr) {
        *status = "The Qt-hosted OpenGL surface backend is not compiled for "
                  "this build.";
    }
    return false;
}

bool opengl_backend_supports(const RenderConfig&, std::string* reason) {
    if (reason != nullptr) {
        *reason = "The Qt-hosted OpenGL surface backend is not compiled for "
                  "this build.";
    }
    return false;
}

bool opengl_generated_base_supported(const RenderConfig&) {
    return false;
}

bool opengl_surface_backend_supports(const SurfaceConfig&) {
    return false;
}

bool opengl_surface_acceleration_active() {
    return false;
}

bool set_opengl_surface_acceleration_active(bool) {
    return false;
}

const PreparedFrame* opengl_prepared_frame() {
    return nullptr;
}

const PreparedFrame* set_opengl_prepared_frame(const PreparedFrame*) {
    return nullptr;
}

bool render_generated_base_opengl(const RenderConfig&,
                                  const PreparedFrame&, Image&,
                                  const std::atomic_bool*,
                                  std::string* error) {
    if (error != nullptr) {
        *error = "The OpenGL generated-source backend is not compiled.";
    }
    return false;
}

bool apply_surface_mapping_opengl(const Image&, Image&,
                                  const SurfaceConfig&, double,
                                  const std::atomic_bool*, std::string* error) {
    if (error != nullptr) {
        *error = "The Qt-hosted OpenGL surface backend is not compiled for "
                 "this build.";
    }
    return false;
}

} // namespace pvt::detail
