#ifndef PROCEDURAL_VISUALIZER_TOOL_H
#define PROCEDURAL_VISUALIZER_TOOL_H

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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

constexpr std::uint32_t kSetupFormatVersion = 21;
// Author-facing collections are displayed and indexed by Qt APIs whose count
// type is int.  Do not impose smaller policy caps: allocation failure and the
// checked render-memory arithmetic are the real limits below this API bound.
constexpr std::size_t kMaximumUiItems =
    static_cast<std::size_t>((std::numeric_limits<int>::max)());
// Scalar authoring values cross CPU double math and GPU float storage. The
// longest render expression multiplies four independently authored controls
// and one signed-int canvas dimension; derive a shared boundary that keeps
// that product finite instead of imposing small per-control policy ceilings.
inline double maximum_render_parameter_magnitude() noexcept {
    static const double value = std::pow(
        static_cast<double>((std::numeric_limits<float>::max)())
            / static_cast<double>((std::numeric_limits<int>::max)()),
        0.25);
    return value;
}
constexpr std::size_t kMaximumWaves = kMaximumUiItems;
constexpr std::size_t kMaximumEffects = kMaximumUiItems;
constexpr std::size_t kMaximumPostProcessEffects = kMaximumUiItems;
constexpr std::size_t kMaximumSwings = kMaximumUiItems;
constexpr std::size_t kMaximumLayers = kMaximumUiItems;
constexpr std::size_t kMaximumLayerGroups = kMaximumLayers;
constexpr std::size_t kMaximumPaletteColors = kMaximumUiItems;
constexpr std::size_t kMaximumMotionPaths = kMaximumUiItems;
constexpr std::size_t kMaximumMotionPathNodes = kMaximumUiItems;
constexpr std::size_t kMaximumParameterLfos = kMaximumUiItems;
// Fragment transforms are evaluated per triangle during mesh rasterization.
// This is a deliberate workload bound rather than an artistic policy limit.
constexpr std::size_t kMaximumMeshFragments = 65536;
constexpr std::size_t kBuiltInPaletteCount = 6;
// The text codec, CLI, and Qt editors ultimately expose signed-int counts.
// Their bounds replace the former small product-policy ceilings.
constexpr std::size_t kMaximumSetupBytes = kMaximumUiItems;
// Threads and in-flight GPU submissions are operating-system workloads, not
// authored project data. Keep their public controls broadly useful while
// preventing a valid-but-extreme value from attempting millions of threads or
// command buffers before the memory admission limits can intervene.
constexpr std::size_t kMaximumSequenceWorkers = 256;
constexpr std::size_t kMaximumGpuFramesInFlight = 256;
constexpr std::size_t kMaximumMusicFeatureSamples = kMaximumUiItems;
constexpr std::size_t kMaximumMusicBeats = kMaximumUiItems;
constexpr std::size_t kMaximumMusicTempoPoints = kMaximumUiItems;
constexpr std::size_t kMaximumAttachmentBasenameBytes = 255;
// Export basenames use the same broadly portable filesystem-component bound.
constexpr std::size_t kMaximumOutputFilenameBytes = 255;
// Bundle entries are materialized by APIs with signed-int interoperability.
constexpr std::size_t kMaximumEmbeddedAssetBytes = kMaximumUiItems;
// Live routing is visited on latency-sensitive paths. These limits bound
// deterministic per-event and scene-switch work from both authored and
// hostile projects; they are workload/security boundaries rather than scalar
// authoring policy.
constexpr std::size_t kMaximumLiveEndpoints = 256;
constexpr std::size_t kMaximumLiveMappings = 16384;
constexpr std::size_t kMaximumLiveClockInputs = 4096;
constexpr std::size_t kMaximumLiveClockOutputs = 4096;
constexpr std::size_t kMaximumLiveScenes = 2048;
constexpr std::size_t kMaximumLiveSceneValues = 16384;
constexpr std::size_t kMaximumLiveTextBytes = 4096;
// Audio filters run inside real-time callbacks and once per decoded music
// sample. These deliberately small workload bounds keep a hostile project from
// multiplying per-sample DSP without imposing a meaningful artistic limit.
constexpr std::size_t kMaximumAudioEqualizerBands = 32;
constexpr std::size_t kMaximumAudioFrequencyStreams = 64;
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
    BlockScale,
    ParticleField,
    Blur,
    Glitch,
    Starburst,
    LensDistortion,
    EdgeDetect,
    Twirl,
    Water
};

enum class ParticleShape : std::uint8_t {
    Spark = 0,
    SoftOrb,
    Ring,
    Diamond,
    Star
};

// LegacyGlow reproduces setup-format 14 particle falloff exactly when the
// appended particle controls retain their compatibility defaults. Defined
// keeps a crisp, antialiased silhouette under the optional soft halo.
enum class ParticleRenderProfile : std::uint8_t {
    LegacyGlow = 0,
    Defined
};

// Fixed preserves the historical authored travel-angle alignment. FollowMotion
// aligns both trails and silhouettes with instantaneous orbital motion. Random
// follows motion for trails but gives each silhouette a deterministic angle.
enum class ParticleOrientation : std::uint8_t {
    Fixed = 0,
    FollowMotion,
    Random
};

enum class BlurType : std::uint8_t {
    Gaussian = 0,
    Box,
    Directional,
    Radial,
    Zoom
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

enum class SurfaceProjection : std::uint8_t {
    Orthographic = 0,
    Perspective
};

enum class SurfaceSizing : std::uint8_t {
    Contain = 0,
    Cover,
    Stretch,
    ShortSide
};

enum class SurfaceOutside : std::uint8_t {
    Transparent = 0,
    Source,
    Reflect
};

enum class SurfaceRotationOrder : std::uint8_t {
    XYZ = 0,
    XZY,
    YXZ,
    YZX,
    ZXY,
    ZYX
};

enum class MeshConstructionMode : std::uint8_t {
    None = 0,
    Explode,
    Deconstruct,
    Reconstruct
};

enum class MeshFragmentation : std::uint8_t {
    Automatic = 0,
    ConnectedComponents,
    TriangleClusters
};

enum class StartingImageFit : std::uint8_t {
    Stretch = 0,
    Contain,
    Cover,
    Tile
};

// Controls the spatial traversal of generated source colors when no authored
// palette is active. The LegacyHue alias preserves source compatibility while
// the product-facing name remains simply Continuous hue.
enum class StartingColorMode : std::uint8_t {
    ContinuousHue = 0,
    HorizontalRainbow = 1,
    VerticalRainbow = 2,
    DiagonalRainbow = 3,
    // Values 4 and 5 shipped before the radial spiral was introduced and are
    // part of the public shared-library ABI. Keep legacy compiled clients from
    // silently changing their generated-color traversal.
    SquareSpiralRainbow = 4,
    Random = 5,
    SpiralRainbow = 6,
    LegacyHue = ContinuousHue,
    ChannelLoops = HorizontalRainbow,
    Interleaved = VerticalRainbow,
    Additive = DiagonalRainbow,
    Subtractive = SquareSpiralRainbow
};

static_assert(static_cast<std::uint8_t>(StartingColorMode::ContinuousHue) == 0U);
static_assert(static_cast<std::uint8_t>(StartingColorMode::HorizontalRainbow) == 1U);
static_assert(static_cast<std::uint8_t>(StartingColorMode::VerticalRainbow) == 2U);
static_assert(static_cast<std::uint8_t>(StartingColorMode::DiagonalRainbow) == 3U);
static_assert(static_cast<std::uint8_t>(StartingColorMode::SquareSpiralRainbow) == 4U);
static_assert(static_cast<std::uint8_t>(StartingColorMode::Subtractive) == 4U);
static_assert(static_cast<std::uint8_t>(StartingColorMode::Random) == 5U);
static_assert(static_cast<std::uint8_t>(StartingColorMode::SpiralRainbow) == 6U);

enum class PathHandleMode : std::uint8_t {
    Corner = 0,
    AutoSmooth,
    Smooth,
    Symmetric
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

// Channel mapping is evaluated simultaneously: every selected source is read
// from the color entering the stage, never from an earlier mapped output.
enum class ChannelSource : std::uint8_t {
    Red = 0,
    Green,
    Blue,
    Alpha,
    Zero,
    One
};

enum class PostProcessStage : std::uint8_t {
    InvertRgb = 0,
    InvertRed,
    InvertGreen,
    InvertBlue,
    InvertAlpha,
    ChannelMap,
    Antialias,
    Quantization
};

constexpr std::size_t kPostProcessStageCount = 8U;

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
    Add,
    // Destination-out modes use this layer as a mask for the already rendered
    // layers below it. They never paint color and cannot affect later layers
    // above them in the stack.
    Erase,
    ColorEraseTones,
    ColorEraseBrightness
};

// Controls the Porter-Duff ordering used after a layer is rendered. AlphaOver
// preserves the historical behavior: the layer paints over the accumulated
// lower stack. AlphaUnder places the layer beneath that accumulated stack while
// retaining the selected artistic blend function and the layer's own opacity.
enum class AlphaMode : std::uint8_t {
    AlphaOver = 0,
    AlphaUnder
};

// Rendering execution policy is deliberately separate from project/setup
// state. It is a host preference, not authored artwork, so changing devices or
// diagnosing a driver issue never dirties a project or changes its bundle.
enum class RenderBackend : std::uint8_t {
    Cpu = 0,
    CpuAndGpu,
    Gpu
};

// The default clock preserves the original seamless normalized-loop behavior.
// Frame, Time, and Meter clocks create calculated pulse/keyframe positions;
// Music follows cached analysis without decoding audio during rendering.
enum class ClockMode : std::uint8_t {
    Default = 0,
    Frame,
    Time,
    Meter,
    Music
};

enum class ClockInterpolation : std::uint8_t {
    Hold = 0,
    Linear,
    Smoothstep
};

enum class ClockFit : std::uint8_t {
    Exact = 0,
    FitSequence
};

enum class MusicTempoMode : std::uint8_t {
    Half = 0,
    Detected,
    Double
};

// A layer clock remains locked to the project timeline; this policy controls
// how a layer-local Music source is sampled when their durations differ.
// Clock data is remapped, never destructively written back to the source or
// cached analysis.
enum class LayerClockScale : std::uint8_t {
    SmartLoopFit = 0,
    StraightFit,
    PlayOnce,
    PlayOnceThenProject,
    OriginalSpeedLoop
};

// Project and active-layer clocks are independently evaluated and transformed
// before this operation is applied. Replace preserves the behavior of every
// setup written before format 11. SoftXor is the continuous fuzzy-XOR
// P + L - 2*P*L. BitwiseXor converts each wrapped phase to an unsigned 24-bit
// fixed-point value, XORs those integers, then converts the result back to a
// normalized phase. LayerClockScale remains an independent duration-mapping
// policy and is never inferred from this operation.
enum class LayerClockMixMode : std::uint8_t {
    Replace = 0,
    Add,
    Difference,
    SoftXor,
    BitwiseXor
};

// Live endpoints are logical, project-portable roles. A host application binds
// their UUIDs to CoreAudio/ALSA/WASAPI, MIDI, OSC, or controller devices in
// machine-local preferences. Projects deliberately contain no device UID,
// filesystem path, IP address, display identity, or captured stream data.
enum class LiveEndpointProtocol : std::uint8_t {
    Audio = 0,
    Midi,
    Osc,
    FootController
};

enum class LiveEndpointDirection : std::uint8_t {
    Input = 0,
    Output,
    Bidirectional
};

enum class LiveControlInput : std::uint8_t {
    MidiControlChange = 0,
    MidiNote,
    MidiProgramChange,
    MidiPitchBend,
    MidiChannelPressure,
    OscValue,
    Footswitch
};

enum class LiveMappingMode : std::uint8_t {
    Absolute = 0,
    Relative,
    Toggle,
    Momentary,
    Trigger
};

enum class LiveMappingTarget : std::uint8_t {
    Setting = 0,
    Action,
    Scene
};

enum class LiveAction : std::uint8_t {
    Freeze = 0,
    Blackout,
    NextScene,
    PreviousScene,
    RestartScene,
    // A useful foot-controller fallback when no external clock is present.
    TapTempo
};

enum class LiveClockTarget : std::uint8_t {
    Project = 0,
    Layer
};

enum class LiveClockInputSource : std::uint8_t {
    MidiClock = 0,
    AudioStream
};

enum class LiveSceneValueType : std::uint8_t {
    Boolean = 0,
    Integer,
    Real,
    EnumToken,
    String
};

enum class LiveDropoutBehavior : std::uint8_t {
    LastGoodFrame = 0,
    Blackout
};

// Compact, seamless alternatives to the deferred hand-authored Bezier path
// editor. Integer cycle counts close exactly over the half-open project loop.
enum class LayerMotionPath : std::uint8_t {
    None = 0,
    Orbit,
    FigureEight,
    Bounce,
    Lissajous
};

enum class MusicFeature : std::uint8_t {
    Energy = 0,
    Bass,
    Midrange,
    Treble,
    Onset,
    Beat,
    SpectralCentroid,
    SpectralFlatness,
    ChromaHue,
    ChromaStrength
};

// A synchronized wave/effect can inherit its effective profile's category and
// feature, select a feature explicitly (which also opts that item in), force
// that item on with the profile source, or ignore audio. The effective profile's
// master switch remains authoritative. Enabled/Disabled retain the format-8
// force/ignore semantics for source and project compatibility. Missing and
// explicit null persistence values map to Default.
enum class AudioResponseMode : std::uint8_t {
    Default = 0,
    Enabled,
    Disabled,
    Energy,
    Bass,
    Midrange,
    Treble,
    Onset,
    Beat,
    SpectralCentroid,
    SpectralFlatness,
    ChromaHue,
    ChromaStrength
};

// Retained for setup/source compatibility with early format-5 drafts. The
// layer's swings_enabled master is now authoritative in every clock mode.
enum class MusicSwingPolicy : std::uint8_t {
    SuppressAll = 0,
    SuppressGlobal,
    KeepAll
};

struct MusicFeatureSample {
    float energy = 0.0F;
    float bass = 0.0F;
    float midrange = 0.0F;
    float treble = 0.0F;
    float onset = 0.0F;
    float beat = 0.0F;
    // Perceptual frequency brightness and noise-like spectral spread.
    float spectral_centroid = 0.0F;
    float spectral_flatness = 0.0F;
    // ChromaHue is circular over [0, 1): C through B. ChromaStrength is the
    // confidence/tonality of that pitch-class control, so callers can avoid
    // reacting to an arbitrary hue during silence or noise.
    float chroma_hue = 0.0F;
    float chroma_strength = 0.0F;
};

struct MusicTempoPoint {
    double time_seconds = 0.0;
    double bpm = 0.0;
    double confidence = 0.0;
};

struct AudioEqualizerBandConfig {
    double frequency_hz = 1000.0;
    double gain_db = 0.0;
};

struct AudioFrequencyStreamConfig {
    // Stable project-local identity. Clock routes use this value rather than a
    // row index so sorting or renaming the table cannot change their meaning.
    std::string uuid;
    std::string name = "Frequency stream";
    double low_hz = 20.0;
    double high_hz = 20000.0;
};

// Applied to decoded/captured mono audio before beat, envelope, spectrum, or
// chroma analysis. A disabled flat block is an exact semantic bypass. Named
// frequency streams split the already-filtered signal and are analyzed
// independently for use as project or layer clocks.
struct AudioInputProcessingConfig {
    bool high_pass_enabled = false;
    double high_pass_hz = 20.0;
    bool low_pass_enabled = false;
    double low_pass_hz = 20000.0;
    bool equalizer_enabled = false;
    std::vector<AudioEqualizerBandConfig> equalizer_bands = {
        {31.25, 0.0}, {62.5, 0.0}, {125.0, 0.0}, {250.0, 0.0},
        {500.0, 0.0}, {1000.0, 0.0}, {2000.0, 0.0}, {4000.0, 0.0},
        {8000.0, 0.0}, {16000.0, 0.0}};
    std::vector<AudioFrequencyStreamConfig> frequency_streams;
};

// Derived tables for one named range. Source identity and duration are shared
// with the owning MusicAnalysis and are intentionally not duplicated.
struct MusicFrequencyStreamAnalysis {
    std::string uuid;
    double low_hz = 20.0;
    double high_hz = 20000.0;
    double detected_bpm = 0.0;
    double tempo_confidence = 0.0;
    std::vector<double> beat_times_seconds;
    std::vector<MusicTempoPoint> tempo_points;
    std::vector<MusicFeatureSample> feature_samples;
};

// Lossless forward-compatibility data retained while loading a setup written
// by another build. Unknown records keep their original key/value pair.
// Rejected records were recognized but could not be used safely (for example,
// a future enum token or an out-of-range number); serializers keep those in a
// reserved recovery envelope so a later build can try the original value
// again. Repair notes are user-facing diagnostics and are not serialized.
struct PreservedConfigRecord {
    std::string key;
    std::string value;
    bool rejected = false;
};

struct ConfigCompatibility {
    std::vector<PreservedConfigRecord> records;
    std::vector<std::string> repair_notes;
};

// Bounded deterministic analysis stored with a project. Project bundles embed
// the audio under a collision-safe content identity while preserving its exact
// filename and extension. Versions link it by digest and basename. Feature
// samples are evenly spaced over duration_seconds.
struct MusicAnalysis {
    std::uint32_t schema_version = 1;
    std::string analyzer_version;
    std::string source_sha256;
    std::string source_basename;
    std::string source_format;
    std::uint64_t source_frame_count = 0;
    std::uint32_t source_sample_rate = 0;
    std::uint32_t source_channel_count = 0;
    double duration_seconds = 0.0;
    double detected_bpm = 0.0;
    double tempo_confidence = 0.0;
    std::vector<double> beat_times_seconds;
    std::vector<MusicTempoPoint> tempo_points;
    std::vector<MusicFeatureSample> feature_samples;
    std::vector<MusicFrequencyStreamAnalysis> frequency_streams;
    // Snapshot of the exact pre-analysis settings used for this cache. Clock
    // validation compares it with the authored source settings so stale EQ or
    // range edits cannot be rendered as though they had been reanalyzed.
    AudioInputProcessingConfig input_processing;
    ConfigCompatibility compatibility;
};

// BPM is measured against tempo_note_denominator (4 means quarter-note BPM).
// Expressions support 7/8, 3+2+3/8, 5/4 | 6/4, and non-dyadic denominators
// such as 4/3 or 6/7.
struct MeterConfig {
    std::string expression = "4/4";
    double bpm = 120.0;
    int tempo_note_denominator = 4;
};

struct ClockConfig {
    ClockMode mode = ClockMode::Default;
    ClockInterpolation interpolation = ClockInterpolation::Linear;
    ClockFit fit = ClockFit::Exact;
    int frame_interval = 1;
    std::int64_t time_interval_microseconds = 500000;
    MeterConfig meter;
    MusicTempoMode music_tempo = MusicTempoMode::Detected;
    // Retained for setup compatibility. The Swing master checkbox is
    // authoritative; new projects always mix authored swings with Music.
    MusicSwingPolicy music_swing_policy = MusicSwingPolicy::KeepAll;
    std::int64_t beat_offset_microseconds = 0;
    double phase_offset_degrees = 0.0;
    bool reverse = false;
    // Analysis still drives visuals when true, but this source is excluded
    // from preview playback and movie audio. Project clocks default audible.
    bool data_only = false;
    MusicAnalysis music;
    // Appended to preserve aggregate-initializer source compatibility. Empty
    // selects the complete post-filter signal; otherwise it references a
    // configured named range. This still grows the public by-value ABI.
    AudioInputProcessingConfig audio_processing;
    std::string frequency_stream_uuid;
};

struct LayerClockConfig {
    bool enabled = false;
    LayerClockScale scale = LayerClockScale::SmartLoopFit;
    // Layer music is normally a control signal. Audibility is an explicit
    // choice so adding a modulation clip cannot unexpectedly alter a mix.
    ClockConfig clock = [] {
        ClockConfig value;
        value.data_only = true;
        return value;
    }();
    // Appended for aggregate-initializer compatibility. Missing persisted
    // values select Replace, preserving the historical layer-clock override.
    LayerClockMixMode mix = LayerClockMixMode::Replace;
    // Mixing is an explicit opt-in. When false an enabled layer clock always
    // replaces the project clock, regardless of the stored mix selection.
    bool mix_enabled = false;
};

// Music response changes only evaluated values. Authored wave/effect settings
// remain untouched; by default free-running items do not respond.
struct AudioReactiveConfig {
    bool enabled = false;
    bool synchronized_only = true;
    bool waves_enabled = true;
    MusicFeature wave_source = MusicFeature::Beat;
    double wave_amount = 0.35;
    bool effects_enabled = true;
    MusicFeature effect_source = MusicFeature::Energy;
    double effect_amount = 0.45;
    bool color_enabled = true;
    // Energy is intentionally the visible default. Pitch color remains an
    // opt-in tonality-weighted route and can be subtle for noisy/atonal audio.
    MusicFeature color_source = MusicFeature::Energy;
    double color_amount_degrees = 180.0;
};

struct CubicPathNode {
    std::uint64_t id = 0;
    double x = 0.5;
    double y = 0.5;
    double in_x = 0.0;
    double in_y = 0.0;
    double out_x = 0.0;
    double out_y = 0.0;
    PathHandleMode handle_mode = PathHandleMode::Smooth;
};

struct CubicMotionPath {
    std::uint64_t id = 0;
    std::string name = "Path";
    std::vector<CubicPathNode> nodes;
};

struct PathBinding {
    bool enabled = false;
    std::uint64_t path_id = 0;
    bool synchronized = true;
    int cycles_per_loop = 1;
    double phase_degrees = 0.0;
    bool reverse = false;
    double offset_x = 0.0;
    double offset_y = 0.0;
    bool follow_tangent = false;
    // Evaluation scratch populated on a per-frame RenderConfig copy. It is
    // intentionally not persisted as authored path-binding state.
    double resolved_tangent_degrees = 0.0;
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
    PathBinding path;
    // Appended to preserve source compatibility for existing aggregate
    // initializers; omitted values inherit the effective routing profile.
    AudioResponseMode audio_response = AudioResponseMode::Default;
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
// EndlessZoom: intensity (0..1 source mix; above 1 deepens the zoom span),
//              magnitude, frequency, center, edge mode.
// Ripple:      intensity, magnitude, frequency, secondary (falloff), center,
//              edge mode.
// Shake:       intensity, magnitude, frequency, secondary (second-axis ratio),
//              angle, edge mode.
// FlagWave:    intensity, magnitude, frequency, secondary (harmonic mix),
//              center, angle, edge mode.
// Glitch:      intensity (source/effect mix), magnitude (maximum horizontal
//              displacement as a fraction of the short edge), frequency
//              (whole scanline band count), secondary (RGB split from 0..1),
//              center/local area, edge mode. The effect ID and clock phase
//              deterministically choose each band's offset.
// Starburst:   intensity (source/effect mix), magnitude (radial displacement
//              as a fraction of the short edge), frequency (whole ray count),
//              secondary (ray sharpness from 0..1), center, angle, edge mode.
// LensDistortion: intensity (source/effect mix), magnitude (radial bend),
//              frequency (radial exponent), secondary (-1 barrel, +1
//              pincushion, 0 neutral), center/local area, edge mode. Its
//              authored clock animates the bend without changing the loop seam.
// EdgeDetect:   intensity (source/edge mix), magnitude (edge gain), frequency
//              (whole sampling radius in pixels), secondary (edge threshold),
//              center/local area, edge mode.
// Twirl:        intensity (source/effect mix), magnitude (maximum turns),
//              frequency (radial falloff exponent), secondary (-1..1
//              direction/depth), center/local area, edge mode. The signed
//              sine-phase twist is exactly neutral at the loop seam.
// Water:        intensity (source/refraction mix), magnitude (peak refraction
//              as a fraction of the short edge), frequency (spatial wave
//              density), secondary (cross-wave complexity from 0..1), center,
//              angle, and edge mode. Three fixed directional wave fields use
//              integer temporal harmonics, so every authored clock closes at
//              the project loop boundary.
// Glow:        intensity, secondary (pulse depth), radius_pixels, threshold,
//              soft_knee. Glow expands alpha coverage using straight-alpha
//              compositing.
// BlockScale:  intensity (mix), magnitude (minimum block-size multiplier),
//              frequency (maximum multiplier), secondary (whole quantization
//              steps, where 0 is smooth). The animated multiplier is applied
//              to RenderConfig::block_size at this position in the stack.
// ParticleField: intensity (HDR brightness), magnitude (travel as a short-edge
//              fraction), frequency (whole particle count), secondary (trail
//              amount), angle, radius_pixels (exact full-resolution radius),
//              threshold (white core), soft_knee (halo softness), and the
//              appended particle controls below.
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
    PathBinding path;
    // Appended to preserve source compatibility for existing aggregate
    // initializers; omitted values inherit the effective routing profile.
    AudioResponseMode audio_response = AudioResponseMode::Default;

    // Blur-specific controls. Blurs pulse their mix between the authored
    // bounds according to cycles_per_loop without rewriting `intensity`, which
    // remains the compatibility value for older clients.
    BlurType blur_type = BlurType::Gaussian;
    int blur_passes = 1;
    int blur_samples = 9;
    double blur_minimum = 0.0;
    double blur_maximum = 1.0;
    // Deprecated serialized compatibility field from layer format 8. Rendering
    // deliberately ignores it: cycles_per_loop is the sole modulation count.
    int blur_pulses_per_cycle = 1;
    // Procedural particle silhouettes require no external assets and retain
    // CPU/Metal parity. Spark preserves the historical field appearance.
    ParticleShape particle_shape = ParticleShape::Spark;
    // Appended setup-format 15 controls. Their defaults reproduce the old
    // renderer for source and persisted-project compatibility. New particle
    // effects created by default_effect() deliberately select Defined.
    ParticleRenderProfile particle_profile = ParticleRenderProfile::LegacyGlow;
    double particle_size_variation = 0.0;
    double particle_definition = 0.7;
    double particle_twinkle = 1.0;
    std::uint64_t particle_seed = 0U; // zero derives the pattern from effect ID
    ParticleOrientation particle_orientation = ParticleOrientation::Fixed;
    double particle_rotation_degrees = 0.0;
};

enum class PaletteColorEncoding : std::uint8_t {
    Srgb = 0,
    Linear
};

// Palette component values retain their source encoding. Ordinary authored
// colors use display/sRGB; imported HALF/FLOAT EXR and linear Krita values remain
// linear and may carry finite HDR or negative RGB. When enabled, the palette
// selects the procedural layer's starting colors in linear light.
// Procedural slope lighting, Texture effects, surface lighting, mapped-object
// effects, and explicit quantization run afterward and may create other colors.
// Alpha is an authored source component. The layer's source-alpha switch can
// ignore it non-destructively, and procedural alpha modulation multiplies it.
struct PaletteColor {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;
    // Optional interchange name and per-entry encoding are appended so old
    // aggregate initializers continue to author unnamed sRGB colors.
    std::string name;
    PaletteColorEncoding encoding = PaletteColorEncoding::Srgb;
};

struct PaletteConfig {
    bool enabled = false;
    std::string name = "Custom";
    std::vector<PaletteColor> colors;
    // Zero means the source did not declare a preferred grid width.
    std::size_t columns = 0U;
};

struct LayerTransformConfig {
    bool flip_horizontal = false;
    bool flip_vertical = false;
    MirrorMode mirror = MirrorMode::None;
};

struct LayerMotionConfig {
    bool enabled = false;
    LayerMotionPath path = LayerMotionPath::None;
    // Center and travel are fractions of canvas dimensions. Travel may extend
    // beyond the canvas deliberately for projection/installation workflows.
    double center_x = 0.5;
    double center_y = 0.5;
    double travel_x = 0.15;
    double travel_y = 0.15;
    int cycles_x = 1;
    int cycles_y = 2;
    double phase_degrees = 0.0;
    // Whole rotations per loop and a symmetric scale pulse keep the seam
    // closed. scale_pulse=0 is neutral; 0.5 spans 0.5x through 1.5x.
    int rotations_per_loop = 0;
    double rotation_offset_degrees = 0.0;
    double scale_pulse = 0.0;
    // A reusable cubic-path binding overrides only the built-in placement
    // path. Rotation and scale pulse remain independently composable.
    PathBinding custom_path;
    // Reusable paths retain their own neutral-default placement modifiers so
    // switching between a built-in path and a reusable path never discards or
    // silently ignores the controls shown beneath the path selector. Offsets
    // are normalized canvas fractions. Travel adds a seamless sine offset to
    // the sampled path; zero travel is exactly neutral.
    double custom_offset_x = 0.0;
    double custom_offset_y = 0.0;
    double custom_travel_x = 0.0;
    double custom_travel_y = 0.0;
    int custom_cycles_x = 1;
    int custom_cycles_y = 2;
    double custom_phase_degrees = 0.0;
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
    // Ignores alpha carried by starting-palette colors and embedded image pixels
    // when false. Explicitly generated alpha remains controlled by
    // StartingColorConfig::include_alpha. This does not change
    // LayerConfig::opacity, and ignored authored alpha remains available when
    // this is re-enabled.
    bool use_source_alpha = true;
};

struct StartingColorConfig {
    StartingColorMode mode = StartingColorMode::ContinuousHue;
    bool include_alpha = false;
    // Deprecated layer-format-8 controls. Generated rainbow modes use
    // deterministic working-precision float colors and do not quantize to
    // these values. They remain serialized only for lossless older-file round trips.
    int red_steps = 256;
    int green_steps = 256;
    int blue_steps = 256;
    int alpha_steps = 256;
    double red_minimum = 0.0;
    double red_maximum = 1.0;
    double green_minimum = 0.0;
    double green_maximum = 1.0;
    double blue_minimum = 0.0;
    double blue_maximum = 1.0;
    double alpha_minimum = 0.0;
    double alpha_maximum = 1.0;

    // Transient preview sampling reference. Hosts may set all three values to
    // map a reduced preview back to full-resolution source-color blocks. These
    // values are intentionally not serialized; zero selects the render's own
    // width, height, and block size.
    int reference_width = 0;
    int reference_height = 0;
    int reference_block_size = 0;

    // Generated-source pattern shaping. These controls are evaluated only by
    // procedural base generation; an enabled StartingImageConfig bypasses
    // them completely. Neutral defaults preserve pre-format-11 rendering.
    struct KaleidoscopeConfig {
        bool enabled = false;
        int mirrored_segments = 6;
        double rotation_degrees = 0.0;
        double mix = 1.0;
    } kaleidoscope;
    struct DomainWarpConfig {
        bool enabled = false;
        double strength = 0.0; // Fraction of the shorter canvas edge.
        double scale = 2.0;
        int octaves = 3;
        int cycles_per_loop = 1;
        std::uint64_t seed = 0;
    } domain_warp;
};

struct QuantizationConfig {
    bool enabled = false;
    int levels = 16;
    double mix = 1.0;
    QuantizationMode mode = QuantizationMode::Rgb;
};

struct ChannelMapConfig {
    bool enabled = false;
    double mix = 1.0;
    ChannelSource red_source = ChannelSource::Red;
    ChannelSource green_source = ChannelSource::Green;
    ChannelSource blue_source = ChannelSource::Blue;
    ChannelSource alpha_source = ChannelSource::Alpha;
};

// One independently authored item in the layer-local finishing stack. Unlike
// the legacy fixed stage fields below, this representation deliberately allows
// any stage type to appear any number of times. Every item owns its parameters
// so two adjacent red inversions, for example, can use different mixes.
// Fields that do not apply to an item's stage are retained unchanged; this
// makes stage duplication and future type editing lossless.
struct PostProcessEffectConfig {
    std::uint64_t id = 0;
    std::string name;
    PostProcessStage stage = PostProcessStage::InvertRgb;
    bool enabled = true;
    double mix = 1.0;
    ChannelSource red_source = ChannelSource::Red;
    ChannelSource green_source = ChannelSource::Green;
    ChannelSource blue_source = ChannelSource::Blue;
    ChannelSource alpha_source = ChannelSource::Alpha;
    double antialias_strength = 0.75;
    double antialias_threshold = 0.08;
    int antialias_passes = 1;
    int quantization_levels = 16;
    QuantizationMode quantization_mode = QuantizationMode::Rgb;
};

// Layer-local finishing controls. The order is an exact permutation of all
// PostProcessStage values. Its compatibility default reproduces the historical
// pipeline: combined RGB inversion, individual R/G/B/A inversions, the neutral
// channel-map stage, antialiasing, then quantization. RGB is straight-alpha
// linear light throughout the renderer; antialiasing temporarily premultiplies
// its neighborhood so transparent edges cannot leak hidden RGB.
struct PostProcessConfig {
    bool invert_rgb_enabled = false;
    double invert_rgb_mix = 1.0;
    // In the compatibility-default order, per-channel inversions follow the
    // combined RGB inversion. At full mix, adjacent enabled stages therefore
    // apply two inversions to that channel; an authored order may move either.
    bool invert_red_enabled = false;
    double invert_red_mix = 1.0;
    bool invert_green_enabled = false;
    double invert_green_mix = 1.0;
    bool invert_blue_enabled = false;
    double invert_blue_mix = 1.0;
    bool invert_alpha_enabled = false;
    double invert_alpha_mix = 1.0;
    ChannelMapConfig channel_map;
    bool antialias_enabled = false;
    double antialias_strength = 0.75;
    double antialias_threshold = 0.08;
    int antialias_passes = 1;
    std::vector<PostProcessStage> order{
        PostProcessStage::InvertRgb,
        PostProcessStage::InvertRed,
        PostProcessStage::InvertGreen,
        PostProcessStage::InvertBlue,
        PostProcessStage::InvertAlpha,
        PostProcessStage::ChannelMap,
        PostProcessStage::Antialias,
        PostProcessStage::Quantization};
    // Appended for source compatibility. When authoritative, this is the
    // complete ordered stack and may be empty. When false, direct API clients
    // retain the historical fixed-field/order behavior above.
    std::vector<PostProcessEffectConfig> effects;
    bool effects_authoritative = false;
};

// Optional height-field geometry for the built-in Plane surface. The height
// PNG sample codes and HALF/FLOAT OpenEXR channels are interpreted as linear
// grayscale data: `midpoint` is the neutral sample, while samples below/above
// it interpolate to minimum/maximum. A
// pixels_per_node value of 1 creates one mesh vertex for every render pixel;
// larger values provide a deliberate performance/geometry tradeoff.
struct PlaneDisplacementConfig {
    bool enabled = false;
    double minimum = -0.2;
    double maximum = 0.2;
    double midpoint = 0.5;
    int pixels_per_node = 4;
    std::string path;
    std::string sha256;
    std::string basename;
};

// Environment maps use a Y-up equirectangular layout. Auto honors PNG color
// metadata and treats OpenEXR as linear light; the explicit modes override that
// choice for authored assets whose transfer characteristics are known.
enum class EnvironmentMapEncoding : std::uint8_t {
    Auto,
    Srgb,
    Linear,
};

struct EnvironmentMapConfig {
    bool enabled = false;
    EnvironmentMapEncoding encoding = EnvironmentMapEncoding::Auto;
    double rotation_degrees = 0.0;
    double exposure_stops = 0.0;
    double intensity = 1.0;
    // Blends authored ambient/directional lighting at 0 with environment
    // diffuse lighting at 1 without changing either authored source.
    double mix = 0.5;
    std::string path;
    std::string sha256;
    std::string basename;
};

struct MeshConstructionConfig {
    MeshConstructionMode mode = MeshConstructionMode::None;
    MeshFragmentation fragmentation = MeshFragmentation::Automatic;
    int target_fragments = 64;
    int cycles_per_loop = 1;
    double phase_degrees = 0.0;
    double distance = 0.65;
    double rotation_degrees = 45.0;
    double minimum_scale = 0.85;
    double stagger = 0.5;
    std::uint64_t seed = 0U;
};

struct SurfaceConfig {
    bool enabled = false;
    SurfaceMapping mapping = SurfaceMapping::Plane;
    SurfaceProjection projection = SurfaceProjection::Orthographic;
    SurfaceSizing sizing = SurfaceSizing::Contain;
    SurfaceOutside outside = SurfaceOutside::Transparent;
    SurfaceRotationOrder rotation_order = SurfaceRotationOrder::XYZ;
    // Explicit Euler orientation in the authored order. Integer turns per
    // loop make every animated axis close exactly at the project seam.
    int rotation_x_turns_per_loop = 0;
    int rotation_y_turns_per_loop = 0;
    int rotation_z_turns_per_loop = 0;
    double rotation_x_degrees = 0.0;
    double rotation_y_degrees = 0.0;
    double rotation_z_degrees = 0.0;
    // Sizing chooses the visible canvas reference; size_percent and the three
    // object-axis scales are always authored multipliers, never automatic
    // animation. X/Y position is measured in canvas percent. Z is measured in
    // normalized surface units and matters for Perspective projection.
    double size_percent = 100.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double scale_z = 1.0;
    double position_x_percent = 0.0;
    double position_y_percent = 0.0;
    double position_z = 0.0;
    double camera_distance = 3.4;
    double focal_length = 3.4;
    // For Cylinder/Sphere/Cube/CustomObj, values continuously interpolate from
    // the planar source at 0.0 to the full mapped, lit, and masked surface at
    // 1.0. Plane uses curvature only for a displacement mesh.
    double curvature = 1.0;
    double lighting = 0.0;
    double light_direction_x = -0.45;
    double light_direction_y = -0.55;
    double light_direction_z = 0.75;
    double light_ambient = 0.28;
    double light_diffuse = 0.72;
    bool composite_backfaces = true;
    // If true, imported OBJ bounds are recentered and their longest axis is
    // normalized to two units before the authored scale is applied.
    bool normalize_obj = true;
    // Used when mapping is CustomObj. obj_path is the current runtime/source
    // path and is deliberately not part of portable project semantics when an
    // embedded digest is present. New project bundles store the bytes at
    // assets/<obj_sha256>/<obj_basename> and materialize a safe local path when
    // opened.
    std::string obj_path;
    std::string obj_sha256;
    std::string obj_basename;
    PlaneDisplacementConfig plane_displacement;
    EnvironmentMapConfig environment_map;
    MeshConstructionConfig mesh_construction;
};

// An embedded starting image replaces procedural spatial generation. When a
// starting palette is enabled, the fitted image is source-quantized to it and
// then flows through Texture effects, surface mapping, transforms,
// mapped-object effects, and final quantization like any other source.
struct StartingImageConfig {
    bool enabled = false;
    StartingImageFit fit = StartingImageFit::Cover;
    std::string path;
    std::string sha256;
    std::string basename;
    // When an authored starting palette is enabled, the fitted in-memory image
    // is quantized to that palette before effects. Dithering is optional and is
    // distinct from final PNG export dithering.
    bool palette_dither_enabled = false;
    DitherMethod palette_dither_method = DitherMethod::BlueNoise;
};

struct LiveEndpointConfig {
    // Project-local identity for a logical role such as "Stage MIDI". The
    // actual OS/device identity belongs in host preferences, keyed by uuid.
    std::string uuid;
    std::string name = "Live input";
    LiveEndpointProtocol protocol = LiveEndpointProtocol::Midi;
    LiveEndpointDirection direction = LiveEndpointDirection::Input;
    // Signed compensation permits either an early or late measured path. It
    // describes the logical rig and remains useful when moving the project;
    // the host may layer machine-specific compensation on top at runtime.
    std::int64_t input_latency_microseconds = 0;
    std::int64_t output_latency_microseconds = 0;
};

struct LiveControlMapping {
    bool enabled = true;
    std::string name = "Live control";
    std::string endpoint_uuid;
    LiveControlInput input = LiveControlInput::MidiControlChange;
    // Zero means omni; 1-16 selects a MIDI channel. Non-MIDI sources keep 0.
    int midi_channel = 0;
    // CC/note/program number, or the logical footswitch index. Sources that do
    // not address a numbered control keep zero.
    int control_number = 0;
    std::string osc_address;

    LiveMappingTarget target = LiveMappingTarget::Setting;
    // Opaque, stable registry key supplied by the host (for example a layer
    // UUID plus an effect ID and property). Unknown keys remain portable and
    // are reported as unresolved at runtime rather than discarded on load.
    std::string target_path;
    LiveAction action = LiveAction::Freeze;
    std::string scene_uuid;
    LiveMappingMode mode = LiveMappingMode::Absolute;

    // Backends normalize source data before this transform. Descending output
    // ranges intentionally support inversion. Values outside a setting's own
    // range are left to that setting's existing validation/clamping policy.
    double input_minimum = 0.0;
    double input_maximum = 1.0;
    double output_minimum = 0.0;
    double output_maximum = 1.0;
    double curve = 1.0;
    double dead_zone = 0.0;
    int smoothing_milliseconds = 0;
};

struct LiveClockInputConfig {
    bool enabled = false;
    LiveClockTarget target = LiveClockTarget::Project;
    // Empty for Project; a canonical layer UUID for Layer.
    std::string layer_uuid;
    LiveClockInputSource source = LiveClockInputSource::MidiClock;
    std::string endpoint_uuid;
    // Zero mixes all audio channels; positive values are one-based logical
    // channel hints and never OS device channel IDs.
    int audio_channel = 0;
    bool follow_midi_transport = true;
    int holdover_milliseconds = 500;
    // Empty selects the full post-filter signal. Used only for AudioStream.
    // Appended to preserve aggregate-initializer source compatibility.
    std::string frequency_stream_uuid;
};

struct LiveMidiClockOutputConfig {
    bool enabled = false;
    LiveClockTarget source = LiveClockTarget::Project;
    // Empty for Project; a canonical layer UUID for Layer.
    std::string layer_uuid;
    std::string endpoint_uuid;
    bool send_transport = true;
    bool send_song_position = true;
};

struct LiveSceneValue {
    std::string target_path;
    LiveSceneValueType type = LiveSceneValueType::Real;
    // Deterministic text avoids a public ABI dependency on std::variant and is
    // parsed according to type. Reals use locale-independent decimal syntax.
    std::string value = "0";
};

struct LiveSceneConfig {
    std::string uuid;
    std::string name = "Scene";
    // Hosts interpolate numeric values when meaningful and switch discrete
    // values at the end of this transition.
    int transition_milliseconds = 0;
    std::vector<LiveSceneValue> values;
};

struct LiveOutputConfig {
    bool fullscreen = true;
    bool prefer_secondary_display = true;
    bool hide_cursor = true;
};

struct LiveSafetyConfig {
    LiveDropoutBehavior dropout_behavior =
        LiveDropoutBehavior::LastGoodFrame;
    bool frame_time_watchdog_enabled = true;
    int watchdog_timeout_milliseconds = 100;
    int audio_dropout_grace_milliseconds = 250;
    // When LastGoodFrame is selected, zero holds indefinitely and a positive
    // value transitions to blackout after this much time.
    int last_good_frame_timeout_milliseconds = 0;
    // Hosts implement this only where a supported OS assertion API exists.
    bool prevent_device_sleep = false;
};

struct LiveConfig {
    bool enabled = false;
    std::vector<LiveEndpointConfig> endpoints;
    std::vector<LiveControlMapping> mappings;
    std::vector<LiveClockInputConfig> clock_inputs;
    std::vector<LiveMidiClockOutputConfig> midi_clock_outputs;
    std::vector<LiveSceneConfig> scenes;
    // Saved startup choice, not transient currently-playing state.
    std::string startup_scene_uuid;
    LiveOutputConfig output;
    LiveSafetyConfig safety;
    AudioInputProcessingConfig audio_processing;
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
    ClockConfig clock;
    std::vector<CubicMotionPath> motion_paths;
    ConfigCompatibility output_compatibility;
    // Layers that do not author an override inherit this project-wide routing
    // block. It is appended for aggregate-initializer compatibility and is
    // disabled by default until music response is intentionally enabled.
    AudioReactiveConfig audio_reactive_defaults;
    // Portable authoring state only. Captured samples, incremental analysis,
    // current freeze/blackout state, and resolved device/display handles are
    // ephemeral runtime data and must never be written here.
    LiveConfig live;
};

// A layer-local low-frequency oscillator drives one stable numeric target.
// Target paths use the same reorder-safe keys as Live controls, relative to
// the owning layer (for example "surface.rotation_y" or
// "effect/42/intensity"). Integer targets round the evaluated value at the
// point of use. Integer cycles preserve a seamless project loop.
struct ParameterLfo {
    bool enabled = true;
    std::string target_path;
    Waveform waveform = Waveform::Sine;
    double minimum = 0.0;
    double maximum = 1.0;
    int cycles_per_loop = 1;
    double phase_degrees = 0.0;
    // Used by Smooth pulse; neutral for the other waveform choices.
    double shape = 0.5;
};

// Per-layer render data. Canvas/loop and export settings deliberately live
// outside this type so switching layers cannot overwrite project-global data.
struct RenderData {
    std::vector<WaveConfig> waves;
    std::vector<SwingConfig> swings;
    std::vector<EffectConfig> effects;

    bool swings_enabled = true;
    AudioReactiveConfig audio_reactive;
    LayerClockConfig layer_clock;

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
    StartingImageConfig starting_image;
    PaletteConfig palette;
    LayerTransformConfig transform;
    LayerMotionConfig motion;
    ConfigCompatibility source_compatibility;
    // Direct RenderConfig users retain the historical explicit-layer behavior.
    // This is appended for aggregate-initializer compatibility; default_layer()
    // changes it to false so project layers inherit the project-wide defaults.
    bool audio_reactive_override_enabled = true;
    StartingColorConfig starting_colors;
    // Appended so older aggregate initializers retain their field ordering and
    // receive a fully neutral finishing stage.
    PostProcessConfig post_process;
    // Appended project-authoring modulation. Unknown or deleted targets are
    // retained by persistence and ignored at render time so item deletion and
    // older/newer application versions do not destroy authored automation.
    std::vector<ParameterLfo> parameter_lfos;
};

// Backward-compatible single-render configuration. Public field access such
// as config.waves remains source-compatible through the public RenderData base.
struct RenderConfig : RenderData {
    int width = 1920;
    int height = 1080;
    int block_size = 16;
    int total_frames = 480;
    double fps = 60.0;
    ClockConfig clock;
    std::vector<CubicMotionPath> motion_paths;

    ExportConfig output;
    ConfigCompatibility output_compatibility;
    // Appended so aggregate initializers written against earlier releases keep
    // their field ordering and receive the neutral project-wide default.
    AudioReactiveConfig audio_reactive_defaults;
    // Appended project-global Live configuration. Offline rendering ignores
    // it; the setup codec retains it so a standalone setup remains portable.
    LiveConfig live;
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
    // Appended for source compatibility with aggregate initializers from
    // releases before per-layer alpha ordering and groups were introduced.
    AlphaMode alpha_mode = AlphaMode::AlphaOver;
    // Empty means ungrouped. Groups contain a contiguous run of layers so a
    // folder can be moved atomically without creating a second paint order.
    std::string group_uuid;
};

struct LayerGroup {
    std::string uuid;
    std::string name = "Group 1";
    bool enabled = true;
    // Locking is an authoring guard. It never changes rendered pixels.
    bool locked = false;
};

struct ProjectConfig {
    std::string uuid;
    std::string name = "Untitled Fire";
    CanvasLoopConfig canvas;
    ExportConfig output;
    // Paint order is back-to-front: index 0 is the bottom layer and the last
    // enabled entry is composited on top.
    std::vector<LayerConfig> layers;
    // Groups are flat, non-nested folders. Every group must contain at least
    // one contiguous layer run; ordering is therefore derived from `layers`.
    std::vector<LayerGroup> groups;
};

struct PVT_API Image {
    int width = 0;
    int height = 0;
    // Linear-light, straight-alpha RGBA in row-major order. RGB may fall
    // outside [0, 1] (notably after Glow or HDR inversion); alpha is
    // constrained to [0, 1].
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

struct FrameRenderOptions {
    // CPU remains the library/API compatibility default. Applications can opt
    // into CpuAndGpu, which uses Metal for the accelerated pixel pipeline on
    // macOS and OpenGL generated/surface/completion passes on Windows and
    // Linux. Gpu prioritizes that device and reports runtime acceleration
    // failures instead of silently restarting the frame on CPU. Ordered CPU
    // dependencies may remain inside the same GPU-owned frame.
    RenderBackend backend = RenderBackend::Cpu;
    // Metal work is admitted before its frame buffers are allocated. This
    // bound therefore limits both queued command buffers and GPU-visible frame
    // working sets. Zero selects the conservative default of two.
    std::size_t maximum_gpu_frames_in_flight = 0;
    // Maximum independent CPU layer renders inside one composite project
    // frame. Zero adapts to the host's reported hardware concurrency. The
    // actual count is also limited by contributing work, memory admission, and
    // kMaximumSequenceWorkers. GPU-supported layers do not consume this budget.
    std::size_t maximum_cpu_workers = 0;
    // Aggregate host-memory admission budget across CPU and GPU-owned project
    // layer working sets plus completed layer images awaiting ordered
    // compositing. GPU device allocations also have their own device-specific
    // admission bound. Zero selects the conservative
    // kDefaultSequenceMemoryBudgetBytes default. One layer is always admitted
    // so a valid large frame remains renderable instead of deadlocking.
    std::size_t cpu_memory_budget_bytes = 0;
};

struct RendererCapabilities {
    bool metal_compiled = false;
    bool metal_available = false;
    std::string metal_device_name;
    std::string metal_status;
    bool opengl_surface_compiled = false;
    bool opengl_surface_available = false;
    std::string opengl_surface_device_name;
    std::string opengl_surface_status;
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
// Workers render and encode independently. `worker_count` controls that outer
// frame concurrency only. When inner project-layer workers are automatic, the
// renderer divides the host's hardware layer-render budget across the actual
// outer worker count instead of multiplying two auto pools. Thus an explicit
// single outer worker can still use every host core within a multi-layer frame.
// Outer workers may still perform ordered
// compositing or encoding while inner work is active. The automatic per-frame
// host-memory budget is divided from this sequence's aggregate budget the same
// way. Explicit nonzero frame worker and memory values remain intentional
// caller overrides. Final output names are
// installed atomically in ascending frame order, and progress callbacks are
// serialized on the calling thread after each installation. This preserves
// legacy callback cancellation and visible output-order semantics.
struct SequenceRenderOptions {
    std::size_t worker_count = 0;
    std::size_t memory_budget_bytes = 0;
    FrameRenderOptions frame;
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
// Creates the standard four-node cubic approximation of an ellipse. Callers
// provide stable nonzero path/node IDs before inserting it into a project.
PVT_API CubicMotionPath default_ellipse_path(std::uint64_t path_id,
                                             std::uint64_t first_node_id,
                                             std::string name = "Ellipse");
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
// True when the current layer contains the stable numeric target referenced by
// a ParameterLfo path. Dynamic wave/swing/effect paths are resolved by ID.
PVT_API bool parameter_lfo_target_supported(const RenderData& render,
                                            const std::string& target_path);

PVT_API ValidationResult validate(const RenderConfig& config);
PVT_API ValidationResult validate(const ProjectConfig& project);
PVT_API ValidationResult validate(const LiveConfig& live);
PVT_API ValidationResult validate(const AudioInputProcessingConfig& processing);
// Returns the stored manual count except for a render-ready Music clock, where
// it returns ceil(audio duration * FPS). A negative result indicates an invalid
// or unprepared clock and is accompanied by `error` when supplied.
PVT_API int effective_frame_count(const CanvasLoopConfig& canvas,
                                  std::string* error = nullptr);
PVT_API int effective_frame_count(const RenderConfig& config,
                                  std::string* error = nullptr);
// Validates the supported meter grammar and returns a concise parsed summary.
PVT_API bool describe_meter(const std::string& expression,
                            std::string& description,
                            std::string* error = nullptr);
PVT_API bool render_frame_at_phase(const RenderConfig& config,
                                   double normalized_phase,
                                   Image& destination,
                                   std::string* error = nullptr);
PVT_API bool render_frame(const RenderConfig& config,
                          int frame_index,
                          Image& destination,
                          std::string* error = nullptr);
PVT_API bool render_frame_at_phase(const RenderConfig& config,
                                   double normalized_phase,
                                   const FrameRenderOptions& options,
                                   Image& destination,
                                   const std::atomic_bool* cancel = nullptr,
                                   std::string* error = nullptr);
PVT_API bool render_frame(const RenderConfig& config,
                          int frame_index,
                          const FrameRenderOptions& options,
                          Image& destination,
                          const std::atomic_bool* cancel = nullptr,
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
PVT_API bool render_project_frame_at_phase(
    const ProjectConfig& project,
    double normalized_phase,
    const FrameRenderOptions& options,
    Image& destination,
    const std::atomic_bool* cancel = nullptr,
    std::string* error = nullptr);
PVT_API bool render_project_frame(const ProjectConfig& project,
                                  int frame_index,
                                  const FrameRenderOptions& options,
                                  Image& destination,
                                  const std::atomic_bool* cancel = nullptr,
                                  std::string* error = nullptr);

PVT_API RendererCapabilities renderer_capabilities();

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
PVT_API const char* particle_shape_name(ParticleShape value);
PVT_API const char* particle_render_profile_name(ParticleRenderProfile value);
PVT_API const char* particle_orientation_name(ParticleOrientation value);
PVT_API const char* blur_type_name(BlurType value);
PVT_API const char* effect_space_name(EffectSpace value);
PVT_API const char* edge_mode_name(EdgeMode value);
PVT_API const char* dither_method_name(DitherMethod value);
PVT_API const char* surface_mapping_name(SurfaceMapping value);
PVT_API const char* surface_projection_name(SurfaceProjection value);
PVT_API const char* surface_sizing_name(SurfaceSizing value);
PVT_API const char* surface_outside_name(SurfaceOutside value);
PVT_API const char* surface_rotation_order_name(SurfaceRotationOrder value);
PVT_API const char* environment_map_encoding_name(
    EnvironmentMapEncoding value);
PVT_API const char* mesh_construction_mode_name(MeshConstructionMode value);
PVT_API const char* mesh_fragmentation_name(MeshFragmentation value);
PVT_API const char* starting_image_fit_name(StartingImageFit value);
PVT_API const char* starting_color_mode_name(StartingColorMode value);
PVT_API const char* waveform_name(Waveform value);
PVT_API const char* quantization_mode_name(QuantizationMode value);
PVT_API const char* channel_source_name(ChannelSource value);
PVT_API const char* post_process_stage_name(PostProcessStage value);
PVT_API const char* mirror_mode_name(MirrorMode value);
PVT_API const char* blend_mode_name(BlendMode value);
PVT_API const char* alpha_mode_name(AlphaMode value);
PVT_API const char* render_backend_name(RenderBackend value);
PVT_API const char* clock_mode_name(ClockMode value);
PVT_API const char* clock_interpolation_name(ClockInterpolation value);
PVT_API const char* clock_fit_name(ClockFit value);
PVT_API const char* music_tempo_mode_name(MusicTempoMode value);
PVT_API const char* layer_clock_scale_name(LayerClockScale value);
PVT_API const char* layer_clock_mix_mode_name(LayerClockMixMode value);
PVT_API const char* palette_color_encoding_name(PaletteColorEncoding value);
PVT_API const char* layer_motion_path_name(LayerMotionPath value);
PVT_API const char* music_feature_name(MusicFeature value);
PVT_API const char* audio_response_mode_name(AudioResponseMode value);
PVT_API const char* music_swing_policy_name(MusicSwingPolicy value);
PVT_API const char* live_endpoint_protocol_name(LiveEndpointProtocol value);
PVT_API const char* live_endpoint_direction_name(LiveEndpointDirection value);
PVT_API const char* live_control_input_name(LiveControlInput value);
PVT_API const char* live_mapping_mode_name(LiveMappingMode value);
PVT_API const char* live_mapping_target_name(LiveMappingTarget value);
PVT_API const char* live_action_name(LiveAction value);
PVT_API const char* live_clock_target_name(LiveClockTarget value);
PVT_API const char* live_clock_input_source_name(LiveClockInputSource value);
PVT_API const char* live_scene_value_type_name(LiveSceneValueType value);
PVT_API const char* live_dropout_behavior_name(LiveDropoutBehavior value);

} // namespace pvt

#endif
