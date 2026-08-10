#ifndef PVT_CONFIG_CODEC_H
#define PVT_CONFIG_CODEC_H

#include "procedural_visualizer_tool.h"

#include <string>

namespace pvt::detail {

inline constexpr std::uint32_t kLayerConfigFormatVersion = 3U;
inline constexpr std::uint32_t kRenderOutputConfigFormatVersion = 2U;

// In-memory access to the installed legacy codec. These are exported only so
// the non-installed bundle helper can reuse the exact parser when the main
// library is shared; this header itself is private and is not installed.
PVT_API bool serialize_setup_config(const RenderConfig& config,
                                    std::string& serialized,
                                    std::string* error = nullptr);
PVT_API bool deserialize_setup_config(const std::string& serialized,
                                      RenderConfig& destination,
                                      std::string* error = nullptr);

// Internal, deterministic text codecs used by project bundles. They share the
// strict record grammar and bounds of legacy PVT_SETUP files, but deliberately
// keep per-layer render/audio-response data separate from project-global
// canvas/clock/export data. Readers retain every earlier codec version.
bool serialize_layer_config(const RenderData& render,
                            std::string& serialized,
                            std::string* error = nullptr);
bool deserialize_layer_config(const std::string& serialized,
                              RenderData& destination,
                              std::string* error = nullptr);

bool serialize_render_output_config(const CanvasLoopConfig& canvas,
                                    const ExportConfig& output,
                                    std::string& serialized,
                                    std::string* error = nullptr);
bool deserialize_render_output_config(const std::string& serialized,
                                      CanvasLoopConfig& canvas,
                                      ExportConfig& output,
                                      std::string* error = nullptr);

} // namespace pvt::detail

#endif
