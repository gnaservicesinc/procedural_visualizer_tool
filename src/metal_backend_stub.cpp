#include "frame_renderer_internal.h"

namespace pvt::detail {

bool metal_backend_compiled() {
    return false;
}

bool metal_backend_available(std::string* device_name,
                             std::string* status) {
    if (device_name != nullptr) {
        device_name->clear();
    }
    if (status != nullptr) {
        *status = "Metal rendering is not compiled for this platform.";
    }
    return false;
}

bool metal_backend_supports(const RenderConfig&, std::string* reason) {
    if (reason != nullptr) {
        *reason = "Metal rendering is not compiled for this platform.";
    }
    return false;
}

bool render_prepared_frame_metal(const RenderConfig&,
                                 const PreparedFrame&,
                                 const FrameRenderOptions&,
                                 Image&,
                                 const std::atomic_bool*,
                                 std::string* error) {
    if (error != nullptr) {
        *error = "Metal rendering is not compiled for this platform.";
    }
    return false;
}

} // namespace pvt::detail
