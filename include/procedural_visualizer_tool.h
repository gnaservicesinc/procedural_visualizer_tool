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

constexpr std::uint32_t kSetupFormatVersion = 1;
constexpr std::size_t kMaximumWaves = 256;
constexpr std::size_t kMaximumEffects = 256;
constexpr std::size_t kMaximumSwings = 64;
constexpr std::size_t kMaximumSetupBytes = 4U * 1024U * 1024U;

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
    Glow
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
    Cube
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
};

// Effects share a compact parameter block so clients can edit and reorder a
// heterogeneous stack without unsafe unions. Parameters that do not apply to a
// given type are ignored. Coordinate-effect `magnitude` is normalized to the
// shorter image edge; Glow uses `radius_pixels` instead.
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
//
// All types use enabled, synchronized, cycles_per_loop, and phase_degrees.
// Synchronized effects use the swung master clock; otherwise they use their
// own linear periodic clock. Both modes close at the loop boundary.
struct EffectConfig {
    std::uint64_t id = 0;
    std::string name;
    EffectType type = EffectType::Ripple;
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
};

struct AlphaConfig {
    // Enables RGBA export and procedural alpha modulation. It must be enabled
    // when active effects or surface mappings can create transparent pixels.
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
    // For Cylinder/Sphere/Cube, values continuously interpolate from the planar
    // source at 0.0 to the full mapped, lit, and masked primitive at 1.0. Plane
    // uses phase/rotation but not curvature.
    double curvature = 1.0;
    double lighting = 0.35;
};

struct ExportConfig {
    int bit_depth = 8; // 8/16 write PNG; 32 writes full-float EXR.
    bool dither_enabled = true;
    DitherMethod dither_method = DitherMethod::BlueNoise;
    std::string output_directory = ".";
    std::string filename_prefix = "frame_";
    int first_frame_number = 0;
    int filename_digits = 4;
    bool overwrite_existing = false;
};

struct RenderConfig {
    int width = 1920;
    int height = 1080;
    int block_size = 16;
    int total_frames = 480;
    double fps = 60.0;

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
    ExportConfig output;
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

PVT_API RenderConfig default_config();
// Item factories intentionally return id == 0 because they cannot see the
// destination configuration. Before inserting an item, assign
// `item.id = allocate_id(config)`; default_config() assigns all built-in IDs.
PVT_API WaveConfig default_wave(std::size_t index = 0);
PVT_API SwingConfig default_swing(std::size_t index = 0);
PVT_API EffectConfig default_effect(EffectType type);
PVT_API std::uint64_t allocate_id(const RenderConfig& config);

PVT_API ValidationResult validate(const RenderConfig& config);
PVT_API bool render_frame_at_phase(const RenderConfig& config,
                                   double normalized_phase,
                                   Image& destination,
                                   std::string* error = nullptr);
PVT_API bool render_frame(const RenderConfig& config,
                          int frame_index,
                          Image& destination,
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

// Save is atomic within the destination directory. Load is transactional: on
// any parse or validation error, `destination` is unchanged.
PVT_API bool save_setup(const RenderConfig& config,
                        const std::string& path,
                        std::string* error = nullptr);
PVT_API bool load_setup(const std::string& path,
                        RenderConfig& destination,
                        std::string* error = nullptr);

PVT_API const char* effect_type_name(EffectType value);
PVT_API const char* edge_mode_name(EdgeMode value);
PVT_API const char* dither_method_name(DitherMethod value);
PVT_API const char* surface_mapping_name(SurfaceMapping value);
PVT_API const char* waveform_name(Waveform value);
PVT_API const char* quantization_mode_name(QuantizationMode value);

} // namespace pvt

#endif
