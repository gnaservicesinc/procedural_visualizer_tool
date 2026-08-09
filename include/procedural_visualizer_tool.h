#ifndef PROCEDURAL_VISUALIZER_TOOL_H
#define PROCEDURAL_VISUALIZER_TOOL_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(PVT_SHARED)
#  if defined(PVT_BUILDING_LIBRARY)
#    define PVT_API __declspec(dllexport)
#  else
#    define PVT_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(PVT_SHARED)
#  define PVT_API __attribute__((visibility("default")))
#else
#  define PVT_API
#endif

namespace pvt {

constexpr std::uint32_t kSetupFormatVersion = 4;
constexpr std::size_t kMaximumWaves = 256;
constexpr std::size_t kMaximumEffects = 256;
constexpr std::size_t kMaximumSwings = 64;
constexpr std::size_t kMaximumLayers = 64;
constexpr std::size_t kMaximumPaletteColors = 256;
constexpr std::size_t kBuiltInPaletteCount = 6;
constexpr std::size_t kMaximumSetupBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumSequenceWorkers = 256;
constexpr std::size_t kDefaultSequenceMemoryBudgetBytes =
    std::size_t{2} << 30U;

enum class EdgeMode : std::uint8_t {
    Alpha = 0,
    Black,
    White,
    Reflect
};

enum class EffectType : std::uint8_t {
    EndlessZoom = 0,
    Ripple,
    Shake,
    FlagWave,
    Glow,
    BlockScale
};

// Texture-space effects run before surface wrapping. Surface-space effects run
// after wrapping, so coordinate effects move/deform the rendered primitive and
// its silhouette instead of merely changing the texture painted on it.
enum class EffectSpace : std::uint8_t {
    Texture = 0,
    Surface
};

enum class DitherMethod : std::uint8_t {
    BlueNoise = 0,
    OrderedBayer,
    FloydSteinberg
};

enum class SurfaceMapping : std::uint8_t {
    Plane = 0,
    Cylinder,
    Sphere,
    Cube,
    CustomObj
};

enum class Waveform : std::uint8_t {
    Sine = 0,
    Triangle,
    SmoothPulse,
    Bounce
};

enum class QuantizationMode : std::uint8_t {
    Rgb = 0,
    Luminance,
    Hue
};

enum class MirrorMode : std::uint8_t {
    None = 0,
    LeftToRight,
    RightToLeft,
    TopToBottom,
    BottomToTop,
    FourWay
};

enum class BlendMode : std::uint8_t {
    // Normal is the requested "none" mode: ordinary straight-alpha
    // source-over compositing with no special RGB blend function.
    Normal = 0,
    SoftLight,
    GrainMerge,
    Overlay,
    ColorDodge,
    LinearBurn,
    ColorBurn,
    Difference,
    Subtract,
    Multiply,
    Add
};

struct WaveConfig {
    std::uint64_t id = 0;
    std::string name;
    bool enabled = true;
    bool synchronized = true;
    double x_percent = 50.0;
    double y_percent = 50.0;
    double amplitude = 0.5;
    double spatial_frequency = 4.0;
    int cycles_per_loop = 1;
    double phase_degrees = 0.0;

    // 0.0 is horizontal propagation, 0.5 is radial/all-directions, and
    // 1.0 is vertical propagation. Intermediate values blend continuously.
    double direction = 0.5;
};

struct SwingConfig {
    std::uint64_t id = 0;
    std::string name;
    bool enabled = true;
    Waveform waveform = Waveform::Sine;
    double amount = 0.15;
    int cycles_per_loop = 4;
    double phase_degrees = 0.0;
    double shape = 0.5;

    // A zero radius preserves the original whole-layer clock modulation.
    // Positive values localize the swing around this normalized center; the
    // radius is expressed as a fraction of the shorter canvas edge.
    double center_x = 0.5;
    double center_y = 0.5;
    double radius = 0.0;
};

// Effects share a compact parameter block so clients can edit and reorder a
// heterogeneous stack without unsafe unions. Parameters that do not apply to a
// given type are ignored. Coordinate-effect `magnitude` is normalized to the
// shorter image edge; Glow and BlockScale use type-specific fields instead.
//
// EndlessZoom: intensity, magnitude, frequency, center, edge mode.
// Ripple:      intensity, magnitude, frequency, secondary (falloff), center,
//              edge mode.
// Shake:       intensity, magnitude, frequency, secondary (second-axis ratio),
//              angle, edge mode.
// FlagWave:    intensity, magnitude, frequency, secondary (harmonic mix),
//              center, angle, edge mode.
// Glow:        intensity, secondary (pulse depth), radius_pixels, threshold,
//              soft_knee. Glow expands alpha coverage using straight-alpha
//              compositing.
// BlockScale:  intensity (mix), magnitude (minimum block-size multiplier),
//              frequency (maximum multiplier), secondary (whole quantization
//              steps, where 0 is smooth). The animated multiplier is applied
//              to RenderConfig::block_size at this position in the stack.
//
// All types use enabled, synchronized, cycles_per_loop, and phase_degrees.
// Synchronized effects use the swung master clock; otherwise they use their
// own linear periodic clock. Both modes close at the loop boundary.
struct EffectConfig {
    std::uint64_t id = 0;
    std::string name;
    EffectType type = EffectType::Ripple;
    EffectSpace space = EffectSpace::Texture;
    bool enabled = false;
    bool synchronized = true;
    int cycles_per_loop = 1;
    double phase_degrees = 0.0;
    EdgeMode edge_mode = EdgeMode::Reflect;

    double intensity = 0.5;
    double magnitude = 0.03;
    double frequency = 4.0;
    double secondary = 1.0;
    double center_x = 0.5;
    double center_y = 0.5;
    double angle_degrees = 0.0;
    double radius_pixels = 12.0;
    double threshold = 0.65;
    double soft_knee = 0.25;

    // A zero area radius applies the effect to the full layer, preserving the
    // legacy behavior. Positive values use a smoothly feathered circular area
    // around center_x/center_y, measured against the shorter canvas edge.
    double area_radius = 0.0;
};

// Palette component values are authored in display/sRGB space. Rendering
// converts them to linear light before choosing the nearest color. Alpha is
// deliberately independent so palette changes never rewrite layer opacity.
struct PaletteColor {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

struct PaletteConfig {
    bool enabled = false;
    std::string name = "Custom";
    std::vector<PaletteColor> colors;
};

struct LayerTransformConfig {
    bool flip_horizontal = false;
    bool flip_vertical = false;
    MirrorMode mirror = MirrorMode::None;
};

struct AlphaConfig {
    // Enables procedural alpha modulation for this render/layer. Legacy
    // RenderConfig exports also treat this flag as an RGBA request; projects
    // select their final RGB/RGBA output independently with
    // ExportConfig::write_alpha.
    bool enabled = false;
    // Opaque defaults make enabling RGBA output neutral until modulation is
    // requested explicitly by lowering minimum or maximum.
    double minimum = 1.0;
    double maximum = 1.0;
    double spatial_frequency = 2.0;
    int cycles_per_loop = 1;
    double phase_degrees = 0.0;
};

struct QuantizationConfig {
    bool enabled = false;
    int levels = 16;
    double mix = 1.0;
    QuantizationMode mode = QuantizationMode::Rgb;
};

struct SurfaceConfig {
    bool enabled = false;
    SurfaceMapping mapping = SurfaceMapping::Plane;
    int rotations_per_loop = 0;
    double phase_degrees = 0.0;
    // For Cylinder/Sphere/Cube/CustomObj, values continuously interpolate from
    // the planar source at 0.0 to the full mapped, lit, and masked surface at
    // 1.0. Plane uses phase/rotation but not curvature.
    double curvature = 1.0;
    double lighting = 0.35;
    // Used when mapping is CustomObj. Relative paths are resolved against the
    // process working directory; GUI launches anchor that directory explicitly.
    std::string obj_path;
};

struct ExportConfig {
    int bit_depth = 8; // 8/16 write PNG; 32 writes full-float EXR.
    // libpng/zlib compression level: 0 stores without deflate compression and
    // 9 spends the most CPU for the smallest output. Ignored for EXR.
    int png_compression_level = 5;
    bool dither_enabled = true;
    DitherMethod dither_method = DitherMethod::BlueNoise;
    // Project-global final image channel selection. Legacy RenderConfig APIs
    // continue to honor AlphaConfig::enabled as well.
    bool write_alpha = false;
    std::string output_directory = ".";
    std::string filename_prefix = "frame_";
    int first_frame_number = 0;
    int filename_digits = 4;
    bool overwrite_existing = false;
};

struct CanvasLoopConfig {
    int width = 1920;
    int height = 1080;
    int block_size = 16;
    int total_frames = 480;
    double fps = 60.0;
};

// Per-layer render data. Canvas/loop and export settings deliberately live
// outside this type so switching layers cannot overwrite project-global data.
struct RenderData {
    std::vector<WaveConfig> waves;
    std::vector<SwingConfig> swings;
    std::vector<EffectConfig> effects;

    double phrase_warp = 0.05;
    double ghost_mix = 0.25;
    double ghost_lag_degrees = 5.7296;

    bool displacement_enabled = true;
    double displacement = 32.0;
    bool lighting_enabled = true;
    double wave_depth = 0.85;
    bool spiral_enabled = true;
    double spiral_frequency = 3.4377;
    int spiral_arms = 4;
    bool wall_reflection_enabled = true;
    double wall_frequency = 6.0161;
    double wall_mix = 0.45;
    int hue_cycles = 2;
    double saturation = 1.0;

    AlphaConfig alpha;
    QuantizationConfig quantization;
    SurfaceConfig surface;
    PaletteConfig palette;
    LayerTransformConfig transform;
};

// Backward-compatible single-render configuration. Public field access such
// as config.waves remains source-compatible through the public RenderData base.
struct RenderConfig : RenderData {
    int width = 1920;
    int height = 1080;
    int block_size = 16;
    int total_frames = 480;
    double fps = 60.0;

    ExportConfig output;
};

struct LayerConfig {
    // Canonical lower-case RFC 4122 UUID text. UUIDs identify layers across
    // renames and reordering; they must be unique within a project.
    std::string uuid;
    // Stable bundle filename identity (for example 0 -> 0.pvt). Reordering
    // never changes it and deletion may deliberately leave gaps.
    std::uint64_t file_id = 0;
    std::string name = "Layer 1";
    bool enabled = true;
    BlendMode blend_mode = BlendMode::Normal;
    double opacity = 1.0;
    RenderData render;
};

struct ProjectConfig {
    std::string uuid;
    std::string name = "Untitled Fire";
    CanvasLoopConfig canvas;
    ExportConfig output;
    // Paint order is back-to-front: index 0 is the bottom layer and the last
    // enabled entry is composited on top.
    std::vector<LayerConfig> layers;
};

struct PVT_API Image {
    int width = 0;
    int height = 0;
    // Linear-light, straight-alpha RGBA in row-major order. RGB may exceed 1.0
    // (notably after Glow); alpha is constrained to [0, 1].
    std::vector<float> pixels;

    // Returns nullptr for out-of-bounds coordinates or inconsistent metadata.
    float* pixel(int x, int y);
    const float* pixel(int x, int y) const;
};

struct ValidationResult {
    bool ok = false;
    std::string message;
    std::size_t estimated_peak_bytes = 0;
};

using ProgressCallback = std::function<bool(int completed_frames, int total_frames)>;

// Sequence rendering execution policy. `worker_count == 0` selects the host's
// reported hardware concurrency; a positive value is an upper bound rather
// than a promise to oversubscribe memory. The renderer also limits workers by
// the frame count, kMaximumSequenceWorkers, and the validated per-frame peak
// estimate. `memory_budget_bytes == 0` selects the conservative 2 GiB default.
// A valid render always receives at least one worker even when its single-frame
// estimate exceeds the aggregate budget.
//
// Workers render and encode independently. Final output names are installed
// atomically in ascending frame order, and progress callbacks are serialized on
// the calling thread after each installation. This preserves legacy callback
// cancellation and visible output-order semantics.
struct SequenceRenderOptions {
    std::size_t worker_count = 0;
    std::size_t memory_budget_bytes = 0;
};

PVT_API RenderConfig default_config();
PVT_API LayerConfig default_layer(std::size_t index = 0);
PVT_API ProjectConfig default_project();
PVT_API std::string generate_uuid();
// Item factories intentionally return id == 0 because they cannot see the
// destination configuration. Before inserting an item, assign
// `item.id = allocate_id(config)`; default_config() assigns all built-in IDs.
PVT_API WaveConfig default_wave(std::size_t index = 0);
PVT_API SwingConfig default_swing(std::size_t index = 0);
PVT_API EffectConfig default_effect(EffectType type);
// Built-in palette indexes beyond the final preset wrap.
PVT_API PaletteConfig default_palette(std::size_t index = 0);
PVT_API std::uint64_t allocate_id(const RenderData& render);
PVT_API std::uint64_t allocate_id(const RenderConfig& config);
PVT_API std::uint64_t allocate_layer_file_id(const ProjectConfig& project);

// Materializes a legacy RenderConfig for one layer without retaining stale
// per-layer copies of project-global canvas or export settings.
PVT_API RenderConfig apply_global_config(const CanvasLoopConfig& canvas,
                                         const ExportConfig& output,
                                         const RenderData& render);

PVT_API ValidationResult validate(const RenderConfig& config);
PVT_API ValidationResult validate(const ProjectConfig& project);
PVT_API bool render_frame_at_phase(const RenderConfig& config,
                                   double normalized_phase,
                                   Image& destination,
                                   std::string* error = nullptr);
PVT_API bool render_frame(const RenderConfig& config,
                          int frame_index,
                          Image& destination,
                          std::string* error = nullptr);

// Cancellable counterparts for a single layer. The legacy entry points above
// remain source-compatible and behave as if `cancel` were null. Cancellation
// is cooperative, checked throughout the expensive render passes, and
// transactional: `destination` is unchanged when cancellation is observed.
PVT_API bool render_frame_at_phase_cancellable(
    const RenderConfig& config,
    double normalized_phase,
    Image& destination,
    const std::atomic_bool* cancel,
    std::string* error = nullptr);
PVT_API bool render_frame_cancellable(const RenderConfig& config,
                                      int frame_index,
                                      Image& destination,
                                      const std::atomic_bool* cancel,
                                      std::string* error = nullptr);

// Composites source over destination in linear-light, straight-alpha space.
// On failure destination is unchanged. Images must have matching positive
// dimensions and exactly four finite float components per pixel.
PVT_API bool composite_over(const Image& source,
                            Image& destination,
                            BlendMode mode = BlendMode::Normal,
                            double opacity = 1.0,
                            std::string* error = nullptr);
PVT_API bool render_project_frame_at_phase(
    const ProjectConfig& project,
    double normalized_phase,
    Image& destination,
    const std::atomic_bool* cancel = nullptr,
    std::string* error = nullptr);
PVT_API bool render_project_frame(const ProjectConfig& project,
                                  int frame_index,
                                  Image& destination,
                                  const std::atomic_bool* cancel = nullptr,
                                  std::string* error = nullptr);

PVT_API bool write_image(const std::string& path,
                         const Image& image,
                         const RenderConfig& config,
                         std::uint32_t deterministic_seed,
                         std::string* error = nullptr);
PVT_API bool render_sequence(const RenderConfig& config,
                             const ProgressCallback& progress = {},
                             const std::atomic_bool* cancel = nullptr,
                             std::string* error = nullptr);
PVT_API bool render_sequence(const RenderConfig& config,
                             const SequenceRenderOptions& options,
                             const ProgressCallback& progress,
                             const std::atomic_bool* cancel,
                             std::string* error = nullptr);
PVT_API bool render_project_sequence(const ProjectConfig& project,
                                     const ProgressCallback& progress = {},
                                     const std::atomic_bool* cancel = nullptr,
                                     std::string* error = nullptr);
PVT_API bool render_project_sequence(const ProjectConfig& project,
                                     const SequenceRenderOptions& options,
                                     const ProgressCallback& progress,
                                     const std::atomic_bool* cancel,
                                     std::string* error = nullptr);

// Save is atomic within the destination directory. Load is transactional: on
// any parse or validation error, `destination` is unchanged.
PVT_API bool save_setup(const RenderConfig& config,
                        const std::string& path,
                        std::string* error = nullptr);
PVT_API bool load_setup(const std::string& path,
                        RenderConfig& destination,
                        std::string* error = nullptr);

PVT_API const char* effect_type_name(EffectType value);
PVT_API const char* effect_space_name(EffectSpace value);
PVT_API const char* edge_mode_name(EdgeMode value);
PVT_API const char* dither_method_name(DitherMethod value);
PVT_API const char* surface_mapping_name(SurfaceMapping value);
PVT_API const char* waveform_name(Waveform value);
PVT_API const char* quantization_mode_name(QuantizationMode value);
PVT_API const char* mirror_mode_name(MirrorMode value);
PVT_API const char* blend_mode_name(BlendMode value);

} // namespace pvt

#endif
