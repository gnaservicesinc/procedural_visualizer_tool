#ifndef PVT_CONFIG_CODEC_H
#define PVT_CONFIG_CODEC_H

#include "procedural_visualizer_tool.h"

#include <string>

namespace pvt::detail {

inline constexpr std::uint32_t kLayerConfigFormatVersion = 23U;
inline constexpr std::uint32_t kRenderOutputConfigFormatVersion = 10U;
inline constexpr std::uint32_t kMusicAnalysisConfigFormatVersion = 2U;
inline constexpr std::uint32_t kSplitRenderOutputConfigFormatVersion = 7U;

// In-memory access to the installed legacy codec. These are exported only so
// the non-installed bundle helper can reuse the exact parser when the main
// library is shared; this header itself is private and is not installed.
PVT_API bool serialize_setup_config(const RenderConfig& config,
                                    std::string& serialized,
                                    std::string* error = nullptr);
PVT_API bool deserialize_setup_config(const std::string& serialized,
                                      RenderConfig& destination,
                                      std::string* error = nullptr);
// Used only while a portable RenderData block has no final canvas. These
// retain every normal parser/semantic bound and defer the single
// canvas-dependent aggregate particle admission decision.
PVT_API bool serialize_setup_config_without_particle_admission(
    const RenderConfig& config,
    std::string& serialized,
    std::string* error = nullptr);
PVT_API bool deserialize_setup_config_without_particle_admission(
    const std::string& serialized,
    RenderConfig& destination,
    std::string* error = nullptr);

// Appends bounded unknown records and rejected-value recovery envelopes after
// a canonical codec has selected its layer/output subset.
bool append_config_compatibility(
    std::string& serialized,
    const ConfigCompatibility* first,
    const ConfigCompatibility* second = nullptr,
    std::string* error = nullptr);

// Internal, deterministic text codecs used by project bundles. They share the
// strict record grammar and bounds of legacy PVT_SETUP files, but deliberately
// keep per-layer render/audio-response data separate from project-global
// canvas/clock/export data. Readers retain every earlier codec version.
bool serialize_layer_config(const RenderData& render,
                            std::string& serialized,
                            std::string* error = nullptr,
                            const std::vector<CubicMotionPath>* motion_paths = nullptr);
bool deserialize_layer_config(const std::string& serialized,
                              RenderData& destination,
                              std::string* error = nullptr,
                              const std::vector<CubicMotionPath>* motion_paths = nullptr);

bool serialize_render_output_config(const CanvasLoopConfig& canvas,
                                    const ExportConfig& output,
                                    std::string& serialized,
                                    std::string* error = nullptr);
bool deserialize_render_output_config(const std::string& serialized,
                                      CanvasLoopConfig& canvas,
                                      ExportConfig& output,
                                      std::string* error = nullptr);

// Project bundles split the potentially large, deterministic analysis table
// from the small per-version render/output file. Legacy setup and render/output
// readers continue to accept the original embedded representation.
bool serialize_music_analysis_config(const MusicAnalysis& analysis,
                                     std::string& serialized,
                                     std::string* error = nullptr);
bool deserialize_music_analysis_config(const std::string& serialized,
                                       MusicAnalysis& analysis,
                                       std::string* error = nullptr);

// Builds the small, metadata-complete analysis used only while validating a
// split layer/render-output envelope. The independently decoded full analysis
// is installed after that envelope passes; duplicating its large sample tables
// here adds no validation coverage.
MusicAnalysis music_analysis_validation_projection(
    const MusicAnalysis& analysis);

bool serialize_split_render_output_config(const CanvasLoopConfig& canvas,
                                          const ExportConfig& output,
                                          std::string& serialized,
                                          std::string* error = nullptr);
bool deserialize_split_render_output_config(
    const std::string& serialized,
    const std::string& music_analysis,
    CanvasLoopConfig& canvas,
    ExportConfig& output,
    std::string* error = nullptr);

// Headerless native-value dump used by .pvtdat project payloads. It writes the
// complete current RenderConfig field sequence in one buffer, uses native
// scalar representations, and writes each in-memory uint32_t flag bank as the
// same uint32_t. There are no separate Boolean values, field names, type tags,
// magic bytes, or version headers in the file.
// Layouts are append-only: existing slots may become ignored tombstones but
// must never move or change representation. The loader identifies a historical
// layout from the complete byte streams themselves and accepts it only when
// every byte is consumed and the resulting values are sane. No independent
// version field can become detached from the raw data.
inline constexpr std::uint32_t kRawConfigCurrentLayout = 1U;
PVT_API bool serialize_raw_config(const RenderConfig& config,
                          std::string& numeric,
                          std::string& strings,
                          std::string* error = nullptr,
                          bool enforce_particle_workload = true);
PVT_API bool deserialize_raw_config(const std::string& numeric,
                            const std::string& strings,
                            RenderConfig& destination,
                            std::string* error = nullptr,
                            bool enforce_particle_workload = true);

} // namespace pvt::detail

#endif
