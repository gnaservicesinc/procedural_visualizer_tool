#include "bundle_archive.h"
#include "config_codec.h"
#include "path_utf8.h"
#include "project_bundle.h"

#include "mz.h"
#include "mz_os.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
int failures = 0;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                             \
                      << ": check failed: " #expression << '\n';                \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

std::string as_utf8(const fs::path& path) {
    return pvt::detail::path_to_utf8(path);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto ticks = std::chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
#if defined(_WIN32)
        const int process = _getpid();
#else
        const int process = static_cast<int>(::getpid());
#endif
        path_ = fs::temp_directory_path()
                / pvt::detail::path_from_utf8(
                    "pvt-bundle-tests-" + std::to_string(process) + "-"
                    + std::to_string(ticks));
        std::error_code error;
        CHECK(fs::create_directory(path_, error));
        CHECK(!error);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        const fs::path temporary = fs::temp_directory_path();
        const std::string name = as_utf8(path_.filename());
        const fs::file_status status = fs::symlink_status(path_, error);
        if (!error && path_.parent_path() == temporary
            && name.rfind("pvt-bundle-tests-", 0U) == 0U
            && fs::is_directory(status) && !fs::is_symlink(status)) {
            fs::remove_all(path_, error);
        }
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

void append_u16(std::string& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xffU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xffU));
}

void append_u32(std::string& bytes, std::uint32_t value) {
    append_u16(bytes, static_cast<std::uint16_t>(value & 0xffffU));
    append_u16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

std::string readable_click_wave() {
    constexpr std::uint32_t sample_rate = 8000U;
    constexpr std::uint32_t seconds = 8U;
    constexpr std::uint32_t sample_count = sample_rate * seconds;
    constexpr std::uint32_t data_bytes = sample_count * 2U;
    std::string bytes;
    bytes.reserve(44U + data_bytes);
    bytes.append("RIFF", 4U);
    append_u32(bytes, 36U + data_bytes);
    bytes.append("WAVEfmt ", 8U);
    append_u32(bytes, 16U);
    append_u16(bytes, 1U);
    append_u16(bytes, 1U);
    append_u32(bytes, sample_rate);
    append_u32(bytes, sample_rate * 2U);
    append_u16(bytes, 2U);
    append_u16(bytes, 16U);
    bytes.append("data", 4U);
    append_u32(bytes, data_bytes);
    for (std::uint32_t sample = 0U; sample < sample_count; ++sample) {
        const std::uint32_t within_half_second = sample % (sample_rate / 2U);
        const std::int16_t value = within_half_second < 12U
            ? static_cast<std::int16_t>(30000 - within_half_second * 2000U)
            : 0;
        append_u16(bytes, static_cast<std::uint16_t>(value));
    }
    return bytes;
}

bool replace_once(std::string& bytes, const std::string& before,
                  const std::string& after) {
    const std::size_t position = bytes.find(before);
    if (position == std::string::npos) return false;
    bytes.replace(position, before.size(), after);
    return true;
}

bool replace_record_value(std::string& bytes, const std::string& key,
                          const std::string& value) {
    const std::string prefix = key + "\t";
    std::size_t begin = bytes.find(prefix);
    if (begin == std::string::npos
        || (begin != 0U && bytes[begin - 1U] != '\n')) return false;
    begin += prefix.size();
    const std::size_t end = bytes.find('\n', begin);
    if (end == std::string::npos) return false;
    bytes.replace(begin, end - begin, value);
    return true;
}

bool erase_record(std::string& bytes, const std::string& key) {
    const std::string prefix = key + "\t";
    std::size_t begin = bytes.find(prefix);
    if (begin == std::string::npos
        || (begin != 0U && bytes[begin - 1U] != '\n')) return false;
    const std::size_t end = bytes.find('\n', begin);
    if (end == std::string::npos) return false;
    bytes.erase(begin, end + 1U - begin);
    return true;
}

const pvt::BundleVersionInfo* version_info(const pvt::ProjectDocument& document,
                                           std::uint64_t number) {
    const auto found = std::find_if(
        document.versions.begin(), document.versions.end(),
        [number](const pvt::BundleVersionInfo& value) {
            return value.number == number;
        });
    return found == document.versions.end() ? nullptr : &*found;
}

std::string portable_root(const std::string& name) {
    std::string filename = pvt::portable_project_filename(name);
    CHECK(filename.size() >= 4U && filename.substr(filename.size() - 4U) == ".zip");
    filename.resize(filename.size() - 4U);
    return filename;
}

bool rewrite_root_checksum(const fs::path& bundle) {
    const std::string metadata = read_bytes(bundle / "metadata.txt");
    std::string digest;
    std::string error;
    if (!pvt::detail::sha256_hex(metadata, digest, &error)) return false;
    return write_bytes(bundle / "metadata.sha256",
                       "PVT_SHA256\t1\nmetadata.sha256\t" + digest + "\n");
}

bool write_test_zip(const fs::path& path,
                    const std::vector<std::string>& names,
                    bool symlink_entry = false) {
    void* writer = mz_zip_writer_create();
    if (writer == nullptr) return false;
    bool ok = mz_zip_writer_open_file(writer, as_utf8(path).c_str(), 0, 0) == MZ_OK;
    if (ok) {
        mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_STORE);
        for (const std::string& name : names) {
            mz_zip_file info{};
            info.version_madeby = symlink_entry
                ? static_cast<std::uint16_t>(
                      (MZ_HOST_SYSTEM_UNIX << 8U)
                      | (MZ_VERSION_MADEBY & 0xffU))
                : MZ_VERSION_MADEBY;
            info.compression_method = MZ_COMPRESS_METHOD_STORE;
            info.external_fa = (symlink_entry ? 0120777U : 0100644U) << 16U;
            info.filename = name.c_str();
            info.filename_size = static_cast<std::uint16_t>(name.size());
            const std::string value = "x";
            if (mz_zip_writer_add_buffer(
                    writer, const_cast<char*>(value.data()), 1, &info) != MZ_OK) {
                ok = false;
                break;
            }
        }
    }
    if (ok) ok = mz_zip_writer_close(writer) == MZ_OK;
    else (void)mz_zip_writer_close(writer);
    mz_zip_writer_delete(&writer);
#if defined(_WIN32)
    // minizip normalizes backslashes supplied to its Windows writer. Restore
    // them in both equal-length filename records so this remains a genuine
    // hostile-archive fixture for the reader's platform-neutral path guard.
    if (ok) {
        std::string archive = read_bytes(path);
        bool needs_patch = false;
        bool changed = false;
        for (const std::string& name : names) {
            if (name.find('\\') == std::string::npos) continue;
            needs_patch = true;
            std::string normalized = name;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            std::size_t position = 0U;
            while ((position = archive.find(normalized, position))
                   != std::string::npos) {
                archive.replace(position, normalized.size(), name);
                position += name.size();
                changed = true;
            }
        }
        if (needs_patch) {
            ok = changed && write_bytes(path, archive);
        }
    }
#endif
    return ok;
}

void test_layer_codec_backward_compatibility() {
    pvt::RenderData original = pvt::default_layer().render;
    original.waves.front().direction = 0.123;
    original.swings.front().center_x = 0.31;
    original.swings.front().center_y = 0.69;
    original.swings.front().radius = 0.27;
    original.effects.front().space = pvt::EffectSpace::Surface;
    original.effects.front().area_radius = 0.36;
    original.palette = pvt::default_palette(2U);
    original.palette.columns = 5U;
    original.palette.colors.front().name = "Linear cyan";
    original.palette.colors.front().encoding =
        pvt::PaletteColorEncoding::Linear;
    original.palette.colors.back().name = "Display magenta";
    original.transform.flip_vertical = true;
    original.transform.mirror = pvt::MirrorMode::RightToLeft;
    original.swings_enabled = false;
    original.audio_reactive_override_enabled = false;
    original.audio_reactive.enabled = true;
    original.audio_reactive.synchronized_only = false;
    original.audio_reactive.wave_source = pvt::MusicFeature::Bass;
    original.audio_reactive.wave_amount = 0.64;
    original.audio_reactive.effect_source = pvt::MusicFeature::Onset;
    original.audio_reactive.effect_amount = 0.73;
    original.audio_reactive.color_enabled = true;
    original.audio_reactive.color_source = pvt::MusicFeature::Midrange;
    original.audio_reactive.color_amount_degrees = 38.0;
    original.waves.front().audio_response = pvt::AudioResponseMode::Beat;
    original.effects.front().audio_response = pvt::AudioResponseMode::Energy;
    original.effects.front().particle_shape = pvt::ParticleShape::Star;
    original.effects.front().particle_profile =
        pvt::ParticleRenderProfile::Defined;
    original.effects.front().particle_size_variation = 0.41;
    original.effects.front().particle_definition = 0.86;
    original.effects.front().particle_twinkle = 0.28;
    original.effects.front().particle_seed = UINT64_C(0xfedcba9876543210);
    original.effects.front().particle_orientation =
        pvt::ParticleOrientation::Random;
    original.effects.front().particle_rotation_degrees = -21.5;
    original.waves.front().path.enabled = true;
    original.waves.front().path.path_id = 91U;
    original.waves.front().path.cycles_per_loop = 2;
    original.effects.front().path.path_id = 91U;
    original.motion.custom_path.path_id = 91U;
    original.layer_clock.mix = pvt::LayerClockMixMode::Difference;
    original.layer_clock.mix_enabled = true;
    original.starting_colors.kaleidoscope.enabled = true;
    original.starting_colors.kaleidoscope.mirrored_segments = 13;
    original.starting_colors.domain_warp.enabled = true;
    original.starting_colors.domain_warp.strength = 0.21;
    original.starting_colors.domain_warp.seed = UINT64_C(0x123456789abcdef0);
    original.post_process.invert_rgb_enabled = true;
    original.post_process.invert_rgb_mix = 0.67;
    original.post_process.invert_alpha_enabled = true;
    original.post_process.invert_alpha_mix = 0.42;
    original.post_process.antialias_enabled = true;
    original.post_process.antialias_strength = 0.81;
    original.post_process.antialias_threshold = 0.11;
    original.post_process.antialias_passes = 2;
    original.surface.projection = pvt::SurfaceProjection::Perspective;
    original.surface.sizing = pvt::SurfaceSizing::Cover;
    original.surface.outside = pvt::SurfaceOutside::Source;
    original.surface.rotation_order = pvt::SurfaceRotationOrder::YZX;
    original.surface.rotation_x_turns_per_loop = -2;
    original.surface.rotation_y_turns_per_loop = 3;
    original.surface.rotation_z_turns_per_loop = 4;
    original.surface.rotation_x_degrees = -17.0;
    original.surface.rotation_y_degrees = 29.0;
    original.surface.rotation_z_degrees = 8.0;
    original.surface.size_percent = 81.0;
    original.surface.scale_x = 1.2;
    original.surface.scale_y = 0.8;
    original.surface.scale_z = 1.4;
    original.surface.position_x_percent = 6.0;
    original.surface.position_y_percent = -5.0;
    original.surface.position_z = 0.2;
    original.surface.camera_distance = 4.0;
    original.surface.focal_length = 3.0;
    original.surface.light_direction_x = 0.3;
    original.surface.light_direction_y = -0.4;
    original.surface.light_direction_z = 0.5;
    original.surface.light_ambient = 0.2;
    original.surface.light_diffuse = 1.1;
    original.surface.composite_backfaces = false;
    original.surface.normalize_obj = false;
    original.surface.plane_displacement.enabled = false;
    original.surface.plane_displacement.minimum = -0.31;
    original.surface.plane_displacement.maximum = 0.62;
    original.surface.plane_displacement.midpoint = 0.43;
    original.surface.plane_displacement.pixels_per_node = 6;
    original.surface.plane_displacement.path = "height-map.png";
    pvt::ParameterLfo amplitude_lfo;
    amplitude_lfo.target_path =
        "wave/" + std::to_string(original.waves.front().id) + "/amplitude";
    amplitude_lfo.waveform = pvt::Waveform::Triangle;
    amplitude_lfo.minimum = -0.4;
    amplitude_lfo.maximum = 1.7;
    amplitude_lfo.cycles_per_loop = 3;
    amplitude_lfo.phase_degrees = 27.0;
    original.parameter_lfos.push_back(amplitude_lfo);
    CHECK(pvt::parameter_lfo_target_supported(
        original, amplitude_lfo.target_path));
    const std::vector<pvt::CubicMotionPath> motion_paths{
        pvt::default_ellipse_path(91U, 100U, "Layer ellipse")};
    const auto has_suffix = [](const std::string& value,
                               const std::string& suffix) {
        return value.size() >= suffix.size()
               && value.compare(value.size() - suffix.size(), suffix.size(),
                                suffix) == 0;
    };

    std::string current_layer;
    std::string error;
    CHECK(pvt::detail::serialize_layer_config(
        original, current_layer, &error, &motion_paths));
    CHECK(current_layer.rfind("PVT_LAYER\t15\n", 0U) == 0U);
    pvt::RenderData current_round_trip;
    CHECK(pvt::detail::deserialize_layer_config(
        current_layer, current_round_trip, &error, &motion_paths));
    CHECK(current_round_trip.swings.front().radius == 0.27);
    CHECK(current_round_trip.effects.front().space
          == pvt::EffectSpace::Surface);
    CHECK(current_round_trip.palette.enabled);
    CHECK(current_round_trip.palette.name == "Vaporwave");
    CHECK(current_round_trip.palette.columns == original.palette.columns);
    CHECK(current_round_trip.palette.colors.size()
          == original.palette.colors.size());
    if (current_round_trip.palette.colors.size()
        == original.palette.colors.size()) {
        for (std::size_t index = 0U;
             index < current_round_trip.palette.colors.size(); ++index) {
            CHECK(current_round_trip.palette.colors[index].red
                  == original.palette.colors[index].red);
            CHECK(current_round_trip.palette.colors[index].green
                  == original.palette.colors[index].green);
            CHECK(current_round_trip.palette.colors[index].blue
                  == original.palette.colors[index].blue);
            CHECK(current_round_trip.palette.colors[index].name
                  == original.palette.colors[index].name);
            CHECK(current_round_trip.palette.colors[index].encoding
                  == original.palette.colors[index].encoding);
        }
    }
    CHECK(current_round_trip.transform.mirror
          == pvt::MirrorMode::RightToLeft);
    CHECK(!current_round_trip.swings_enabled);
    CHECK(!current_round_trip.audio_reactive_override_enabled);
    CHECK(current_round_trip.audio_reactive.enabled);
    CHECK(!current_round_trip.audio_reactive.synchronized_only);
    CHECK(current_round_trip.audio_reactive.wave_source
          == pvt::MusicFeature::Bass);
    CHECK(current_round_trip.audio_reactive.effect_source
          == pvt::MusicFeature::Onset);
    CHECK(current_round_trip.audio_reactive.color_source
          == pvt::MusicFeature::Midrange);
    CHECK(current_round_trip.waves.front().audio_response
          == pvt::AudioResponseMode::Beat);
    CHECK(current_round_trip.effects.front().audio_response
          == pvt::AudioResponseMode::Energy);
    CHECK(current_round_trip.effects.front().particle_shape
          == pvt::ParticleShape::Star);
    CHECK(current_round_trip.effects.front().particle_profile
          == pvt::ParticleRenderProfile::Defined);
    CHECK(current_round_trip.effects.front().particle_size_variation == 0.41);
    CHECK(current_round_trip.effects.front().particle_definition == 0.86);
    CHECK(current_round_trip.effects.front().particle_twinkle == 0.28);
    CHECK(current_round_trip.effects.front().particle_seed
          == UINT64_C(0xfedcba9876543210));
    CHECK(current_round_trip.effects.front().particle_orientation
          == pvt::ParticleOrientation::Random);
    CHECK(current_round_trip.effects.front().particle_rotation_degrees == -21.5);
    CHECK(current_round_trip.waves.front().path.enabled);
    CHECK(current_round_trip.waves.front().path.path_id == 91U);
    CHECK(current_round_trip.layer_clock.mix
          == pvt::LayerClockMixMode::Difference);
    CHECK(current_round_trip.layer_clock.mix_enabled);
    CHECK(current_round_trip.starting_colors.kaleidoscope.enabled);
    CHECK(current_round_trip.starting_colors.kaleidoscope.mirrored_segments
          == 13);
    CHECK(current_round_trip.starting_colors.domain_warp.enabled);
    CHECK(current_round_trip.starting_colors.domain_warp.strength == 0.21);
    CHECK(current_round_trip.starting_colors.domain_warp.seed
          == UINT64_C(0x123456789abcdef0));
    CHECK(current_round_trip.post_process.invert_rgb_enabled);
    CHECK(current_round_trip.post_process.invert_rgb_mix == 0.67);
    CHECK(current_round_trip.post_process.invert_alpha_enabled);
    CHECK(current_round_trip.post_process.invert_alpha_mix == 0.42);
    CHECK(current_round_trip.post_process.antialias_enabled);
    CHECK(current_round_trip.post_process.antialias_strength == 0.81);
    CHECK(current_round_trip.post_process.antialias_threshold == 0.11);
    CHECK(current_round_trip.post_process.antialias_passes == 2);
    CHECK(current_round_trip.surface.projection
          == pvt::SurfaceProjection::Perspective);
    CHECK(current_round_trip.surface.sizing == pvt::SurfaceSizing::Cover);
    CHECK(current_round_trip.surface.outside == pvt::SurfaceOutside::Source);
    CHECK(current_round_trip.surface.rotation_order
          == pvt::SurfaceRotationOrder::YZX);
    CHECK(current_round_trip.surface.rotation_x_turns_per_loop == -2);
    CHECK(current_round_trip.surface.rotation_y_turns_per_loop == 3);
    CHECK(current_round_trip.surface.rotation_z_turns_per_loop == 4);
    CHECK(current_round_trip.surface.rotation_x_degrees == -17.0);
    CHECK(current_round_trip.surface.rotation_y_degrees == 29.0);
    CHECK(current_round_trip.surface.rotation_z_degrees == 8.0);
    CHECK(current_round_trip.surface.size_percent == 81.0);
    CHECK(current_round_trip.surface.scale_x == 1.2);
    CHECK(current_round_trip.surface.scale_y == 0.8);
    CHECK(current_round_trip.surface.scale_z == 1.4);
    CHECK(current_round_trip.surface.position_x_percent == 6.0);
    CHECK(current_round_trip.surface.position_y_percent == -5.0);
    CHECK(current_round_trip.surface.position_z == 0.2);
    CHECK(current_round_trip.surface.camera_distance == 4.0);
    CHECK(current_round_trip.surface.focal_length == 3.0);
    CHECK(current_round_trip.surface.light_direction_x == 0.3);
    CHECK(current_round_trip.surface.light_direction_y == -0.4);
    CHECK(current_round_trip.surface.light_direction_z == 0.5);
    CHECK(current_round_trip.surface.light_ambient == 0.2);
    CHECK(current_round_trip.surface.light_diffuse == 1.1);
    CHECK(!current_round_trip.surface.composite_backfaces);
    CHECK(!current_round_trip.surface.normalize_obj);
    CHECK(current_round_trip.surface.plane_displacement.minimum == -0.31);
    CHECK(current_round_trip.surface.plane_displacement.maximum == 0.62);
    CHECK(current_round_trip.surface.plane_displacement.midpoint == 0.43);
    CHECK(current_round_trip.surface.plane_displacement.pixels_per_node == 6);
    CHECK(current_round_trip.surface.plane_displacement.path
          == "height-map.png");
    CHECK(current_round_trip.parameter_lfos.size() == 1U);
    if (current_round_trip.parameter_lfos.size() == 1U) {
        const auto& loaded_lfo = current_round_trip.parameter_lfos.front();
        CHECK(loaded_lfo.target_path == amplitude_lfo.target_path);
        CHECK(loaded_lfo.waveform == amplitude_lfo.waveform);
        CHECK(loaded_lfo.minimum == amplitude_lfo.minimum);
        CHECK(loaded_lfo.maximum == amplitude_lfo.maximum);
        CHECK(loaded_lfo.cycles_per_loop == amplitude_lfo.cycles_per_loop);
        CHECK(loaded_lfo.phase_degrees == amplitude_lfo.phase_degrees);
        CHECK(pvt::parameter_lfo_target_supported(
            current_round_trip, loaded_lfo.target_path));
    }

    // Layer v12/setup v14 predates the independent particle controls. Their
    // compatibility defaults retain the old renderer exactly.
    std::istringstream current_v14_input(current_layer);
    std::ostringstream version_twelve_output;
    std::string line;
    CHECK(static_cast<bool>(std::getline(current_v14_input, line)));
    CHECK(line == "PVT_LAYER\t15");
    version_twelve_output << "PVT_LAYER\t12\n";
    while (std::getline(current_v14_input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        const bool v13_only = has_suffix(key, ".particle_profile")
            || has_suffix(key, ".particle_size_variation")
            || has_suffix(key, ".particle_definition")
            || has_suffix(key, ".particle_twinkle")
            || has_suffix(key, ".particle_seed")
            || has_suffix(key, ".particle_orientation")
            || has_suffix(key, ".particle_rotation_degrees");
        const bool v14_only = key == "surface.projection"
            || key == "surface.sizing" || key == "surface.outside"
            || key.rfind("surface.rotation_", 0U) == 0U
            || key == "surface.size_percent"
            || key.rfind("surface.scale_", 0U) == 0U
            || key.rfind("surface.position_", 0U) == 0U
            || key == "surface.camera_distance"
            || key == "surface.focal_length"
            || key.rfind("surface.light_", 0U) == 0U
            || key == "surface.composite_backfaces"
            || key == "surface.normalize_obj";
        const bool v15_only = key.rfind("parameter_lfos.", 0U) == 0U;
        if (!v13_only && !v14_only && !v15_only) {
            version_twelve_output << line << '\n';
        }
    }
    version_twelve_output << "surface.rotations_per_loop\t0\n"
                          << "surface.phase_degrees\t0\n";
    const std::string version_twelve = version_twelve_output.str();
    pvt::RenderData loaded_version_twelve;
    CHECK(pvt::detail::deserialize_layer_config(
        version_twelve, loaded_version_twelve, &error, &motion_paths));
    CHECK(loaded_version_twelve.effects.front().particle_profile
          == pvt::ParticleRenderProfile::LegacyGlow);
    CHECK(loaded_version_twelve.effects.front().particle_size_variation == 0.0);
    CHECK(loaded_version_twelve.effects.front().particle_twinkle == 1.0);
    CHECK(loaded_version_twelve.effects.front().particle_seed == 0U);
    CHECK(loaded_version_twelve.effects.front().particle_orientation
          == pvt::ParticleOrientation::Fixed);

    // A portable layer has no canvas, so layer migration must preserve an
    // authored particle workload verbatim. Admission and any recovery belong
    // to the assembled project, whose real canvas establishes the budget.
    const auto original_particles = std::find_if(
        original.effects.begin(), original.effects.end(), [](const auto& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    CHECK(original_particles != original.effects.end());
    if (original_particles != original.effects.end()) {
        const std::size_t particle_index = static_cast<std::size_t>(
            std::distance(original.effects.begin(), original_particles));
        const std::string prefix = "effects."
                                   + std::to_string(particle_index) + ".";
        std::string unsafe_version_twelve = version_twelve;
        CHECK(replace_record_value(
            unsafe_version_twelve, prefix + "enabled", "1"));
        CHECK(replace_record_value(
            unsafe_version_twelve, prefix + "intensity", "1"));
        CHECK(replace_record_value(
            unsafe_version_twelve, prefix + "frequency", "1001"));
        CHECK(replace_record_value(
            unsafe_version_twelve, prefix + "secondary", "1"));
        CHECK(replace_record_value(
            unsafe_version_twelve, prefix + "radius_pixels", "16384"));
        pvt::RenderData recovered_unsafe;
        CHECK(pvt::detail::deserialize_layer_config(
            unsafe_version_twelve, recovered_unsafe, &error, &motion_paths));
        const auto recovered_particles = std::find_if(
            recovered_unsafe.effects.begin(), recovered_unsafe.effects.end(),
            [](const auto& effect) {
                return effect.type == pvt::EffectType::ParticleField;
            });
        CHECK(recovered_particles != recovered_unsafe.effects.end());
        if (recovered_particles != recovered_unsafe.effects.end()) {
            CHECK(recovered_particles->enabled);
            CHECK(recovered_particles->frequency == 1001.0);
            CHECK(recovered_particles->secondary == 1.0);
            CHECK(recovered_particles->radius_pixels == 16384.0);
        }
        const std::string recovery_key =
            prefix + "recovery_unsafe_particle_enabled";
        CHECK(!std::any_of(
            recovered_unsafe.source_compatibility.records.begin(),
            recovered_unsafe.source_compatibility.records.end(),
            [&recovery_key](const pvt::PreservedConfigRecord& record) {
                return record.key == recovery_key
                       && record.value == "1" && !record.rejected;
            }));

        std::string recovered_layer;
        CHECK(pvt::detail::serialize_layer_config(
            recovered_unsafe, recovered_layer, &error, &motion_paths));
        pvt::RenderData recovered_round_trip;
        CHECK(pvt::detail::deserialize_layer_config(
            recovered_layer, recovered_round_trip, &error, &motion_paths));
        const auto round_trip_particles = std::find_if(
            recovered_round_trip.effects.begin(),
            recovered_round_trip.effects.end(), [](const auto& effect) {
                return effect.type == pvt::EffectType::ParticleField;
            });
        CHECK(round_trip_particles != recovered_round_trip.effects.end());
        if (round_trip_particles != recovered_round_trip.effects.end()) {
            CHECK(round_trip_particles->enabled);
            CHECK(round_trip_particles->frequency == 1001.0);
            CHECK(round_trip_particles->radius_pixels == 16384.0);
        }
        CHECK(!std::any_of(
            recovered_round_trip.source_compatibility.records.begin(),
            recovered_round_trip.source_compatibility.records.end(),
            [&recovery_key](const pvt::PreservedConfigRecord& record) {
                return record.key == recovery_key
                       && record.value == "1" && !record.rejected;
            }));
    }

    // Layer v11/setup v13 predates audio-input processing, named frequency
    // streams, and procedural particle silhouettes.
    std::istringstream current_v12_input(version_twelve);
    std::ostringstream version_eleven_output;
    CHECK(static_cast<bool>(std::getline(current_v12_input, line)));
    CHECK(line == "PVT_LAYER\t12");
    version_eleven_output << "PVT_LAYER\t11\n";
    while (std::getline(current_v12_input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        const bool v12_only = key.find("audio_input.") != std::string::npos
            || key.find("input_processing.") != std::string::npos
            || key.find("frequency_streams.") != std::string::npos
            || key.find("frequency_stream_uuid") != std::string::npos
            || key.find("particle_shape") != std::string::npos;
        if (!v12_only) version_eleven_output << line << '\n';
    }
    const std::string version_eleven = version_eleven_output.str();
    pvt::RenderData loaded_version_eleven;
    CHECK(pvt::detail::deserialize_layer_config(
        version_eleven, loaded_version_eleven, &error, &motion_paths));

    // Layer v10/setup v12 predates plane-displacement geometry. Missing
    // records recover an off plane with the current safe numeric defaults.
    std::istringstream current_v11_input(version_eleven);
    std::ostringstream version_ten_output;
    CHECK(static_cast<bool>(std::getline(current_v11_input, line)));
    CHECK(line == "PVT_LAYER\t11");
    version_ten_output << "PVT_LAYER\t10\n";
    while (std::getline(current_v11_input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        if (key.rfind("surface.plane_displacement.", 0U) != 0U) {
            version_ten_output << line << '\n';
        }
    }
    const std::string version_ten = version_ten_output.str();
    pvt::RenderData loaded_version_ten;
    CHECK(pvt::detail::deserialize_layer_config(
        version_ten, loaded_version_ten, &error, &motion_paths));
    CHECK(!loaded_version_ten.surface.plane_displacement.enabled);
    CHECK(loaded_version_ten.surface.plane_displacement.path.empty());
    CHECK(loaded_version_ten.surface.plane_displacement.pixels_per_node == 4);

    // Layer v9/setup v11 predates the post-process block. Missing records
    // recover a completely bypassed finishing stage.
    std::istringstream current_v10_input(version_ten);
    std::ostringstream version_nine_output;
    CHECK(static_cast<bool>(std::getline(current_v10_input, line)));
    CHECK(line == "PVT_LAYER\t10");
    version_nine_output << "PVT_LAYER\t9\n";
    while (std::getline(current_v10_input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        if (key.rfind("post_process.", 0U) != 0U) {
            version_nine_output << line << '\n';
        }
    }
    const std::string version_nine = version_nine_output.str();
    pvt::RenderData loaded_version_nine;
    CHECK(pvt::detail::deserialize_layer_config(
        version_nine, loaded_version_nine, &error, &motion_paths));
    CHECK(!loaded_version_nine.post_process.invert_rgb_enabled);
    CHECK(!loaded_version_nine.post_process.invert_alpha_enabled);
    CHECK(!loaded_version_nine.post_process.antialias_enabled);
    CHECK(loaded_version_nine.post_process.antialias_passes == 1);

    // Layer v8/setup v10 predates layer-clock mixing and generated pattern
    // shaping. Missing records recover the neutral, legacy behavior.
    std::istringstream current_v9_input(version_nine);
    std::ostringstream version_eight_output;
    CHECK(static_cast<bool>(std::getline(current_v9_input, line)));
    CHECK(line == "PVT_LAYER\t9");
    version_eight_output << "PVT_LAYER\t8\n";
    while (std::getline(current_v9_input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        const bool v9_only = key == "layer_clock.mix"
                             || key == "layer_clock.mix_enabled"
                             || key == "palette.columns"
                             || (key.rfind("palette.colors.", 0U) == 0U
                                 && (has_suffix(key, ".name")
                                     || has_suffix(key, ".encoding")))
                             || key.rfind(
                                    "starting_colors.kaleidoscope.", 0U)
                                    == 0U
                             || key.rfind(
                                    "starting_colors.domain_warp.", 0U)
                                    == 0U;
        if (!v9_only) version_eight_output << line << '\n';
    }
    const std::string version_eight = version_eight_output.str();
    pvt::RenderData loaded_version_eight;
    CHECK(pvt::detail::deserialize_layer_config(
        version_eight, loaded_version_eight, &error, &motion_paths));
    CHECK(loaded_version_eight.layer_clock.mix
          == pvt::LayerClockMixMode::Replace);
    CHECK(!loaded_version_eight.layer_clock.mix_enabled);
    CHECK(!loaded_version_eight.starting_colors.kaleidoscope.enabled);
    CHECK(!loaded_version_eight.starting_colors.domain_warp.enabled);
    CHECK(loaded_version_eight.palette.columns == 0U);
    for (const auto& color : loaded_version_eight.palette.colors) {
        CHECK(color.name.empty());
        CHECK(color.encoding == pvt::PaletteColorEncoding::Srgb);
    }

    // Layer v7/setup v9 predates RGBA source generation and blur controls.
    std::istringstream current_v8_input(version_eight);
    std::ostringstream version_seven_output;
    CHECK(static_cast<bool>(std::getline(current_v8_input, line)));
    CHECK(line == "PVT_LAYER\t8");
    version_seven_output << "PVT_LAYER\t7\n";
    while (std::getline(current_v8_input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        const bool v8_only = key.rfind("starting_colors.", 0U) == 0U
                             || key == "alpha.use_source_alpha"
                             || key == "source_image.palette_dither_enabled"
                             || key == "source_image.palette_dither_method"
                             || (key.rfind("palette.colors.", 0U) == 0U
                                 && has_suffix(key, ".alpha"))
                             || (key.rfind("effects.", 0U) == 0U
                                 && key.find(".blur_") != std::string::npos);
        if (!v8_only) version_seven_output << line << '\n';
    }
    const std::string version_seven = version_seven_output.str();

    // Layer v6/setup v8 used three-state force/ignore routing. Those values
    // retain their exact meaning in the richer current selector.
    std::string version_six = version_seven;
    version_six.replace(0U, std::string("PVT_LAYER\t7").size(),
                        "PVT_LAYER\t6");
    const auto replace_record_value = [](std::string& setup,
                                         const std::string& key,
                                         const std::string& value) {
        const std::string prefix = key + "\t";
        const std::size_t position = setup.find(prefix);
        CHECK(position != std::string::npos);
        if (position == std::string::npos) return;
        const std::size_t newline = setup.find('\n', position);
        CHECK(newline != std::string::npos);
        if (newline != std::string::npos) {
            setup.replace(position + prefix.size(),
                          newline - position - prefix.size(), value);
        }
    };
    replace_record_value(version_six, "waves.0.audio_response", "enabled");
    replace_record_value(version_six, "effects.0.audio_response", "disabled");
    pvt::RenderData loaded_version_six;
    CHECK(pvt::detail::deserialize_layer_config(
        version_six, loaded_version_six, &error, &motion_paths));
    CHECK(loaded_version_six.waves.front().audio_response
          == pvt::AudioResponseMode::Enabled);
    CHECK(loaded_version_six.effects.front().audio_response
          == pvt::AudioResponseMode::Disabled);

    // Layer v5 predates inheritable routing. Its layer profile remains
    // authoritative, while per-item selectors receive the neutral Default.
    std::istringstream version_six_input(version_six);
    std::ostringstream version_five_output;
    CHECK(static_cast<bool>(std::getline(version_six_input, line)));
    CHECK(line == "PVT_LAYER\t6");
    version_five_output << "PVT_LAYER\t5\n";
    while (std::getline(version_six_input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        const bool v6_only = key == "audio_reactive.override_enabled"
                             || has_suffix(key, ".audio_response");
        if (!v6_only) version_five_output << line << '\n';
    }
    const std::string version_five = version_five_output.str();
    pvt::RenderData loaded_version_five;
    CHECK(pvt::detail::deserialize_layer_config(
        version_five, loaded_version_five, &error, &motion_paths));
    CHECK(loaded_version_five.audio_reactive_override_enabled);
    CHECK(loaded_version_five.waves.front().audio_response
          == pvt::AudioResponseMode::Default);
    CHECK(loaded_version_five.effects.front().audio_response
          == pvt::AudioResponseMode::Default);

    // PVT_LAYER v2 carried the PVT_SETUP v4 render subset. Strip every v3/v4
    // synchronization and animation record while preserving v2 spatial data.
    std::istringstream current_input(version_five);
    std::ostringstream version_two_output;
    CHECK(static_cast<bool>(std::getline(current_input, line)));
    CHECK(line == "PVT_LAYER\t5");
    version_two_output << "PVT_LAYER\t2\n";
    while (std::getline(current_input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        const bool v3_only = key == "rhythm.swings_enabled"
                             || key.rfind("audio_reactive.", 0U) == 0U
                             || key.rfind("layer_clock.", 0U) == 0U
                             || key.rfind("motion.", 0U) == 0U
                             || key.rfind("source_image.", 0U) == 0U
                             || key.find(".path.") != std::string::npos
                             || key == "surface.obj_sha256"
                             || key == "surface.obj_basename";
        if (!v3_only) version_two_output << line << '\n';
    }
    const std::string version_two = version_two_output.str();
    pvt::RenderData loaded_version_two = original;
    CHECK(pvt::detail::deserialize_layer_config(
        version_two, loaded_version_two, &error));
    CHECK(loaded_version_two.swings_enabled);
    CHECK(!loaded_version_two.audio_reactive.enabled);
    CHECK(loaded_version_two.swings.front().radius == 0.27);
    CHECK(loaded_version_two.effects.front().space
          == pvt::EffectSpace::Surface);
    CHECK(loaded_version_two.palette.enabled);
    CHECK(loaded_version_two.transform.mirror
          == pvt::MirrorMode::RightToLeft);

    // PVT_LAYER v1 carried the PVT_SETUP v3 render subset. Strip every v2-only
    // record to emulate a real legacy layer rather than merely changing the
    // header on a modern document.
    std::istringstream input(version_two);
    std::ostringstream legacy;
    CHECK(static_cast<bool>(std::getline(input, line)));
    CHECK(line == "PVT_LAYER\t2");
    legacy << "PVT_LAYER\t1\n";
    while (std::getline(input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        const bool v2_only = key.rfind("palette.", 0U) == 0U
                             || key.rfind("transform.", 0U) == 0U
                             || (key.rfind("swings.", 0U) == 0U
                                 && (has_suffix(key, ".center_x")
                                     || has_suffix(key, ".center_y")
                                     || has_suffix(key, ".radius")))
                             || (key.rfind("effects.", 0U) == 0U
                                 && (has_suffix(key, ".space")
                                     || has_suffix(key, ".area_radius")));
        if (!v2_only) legacy << line << '\n';
    }

    pvt::RenderData loaded = original;
    CHECK(pvt::detail::deserialize_layer_config(
        legacy.str(), loaded, &error));
    CHECK(loaded.waves.front().direction == 0.123);
    CHECK(loaded.swings.front().center_x == 0.5);
    CHECK(loaded.swings.front().center_y == 0.5);
    CHECK(loaded.swings.front().radius == 0.0);
    CHECK(loaded.effects.front().space == pvt::EffectSpace::Texture);
    CHECK(loaded.effects.front().area_radius == 0.0);
    CHECK(!loaded.palette.enabled && loaded.palette.colors.empty());
    CHECK(!loaded.transform.flip_horizontal
          && !loaded.transform.flip_vertical);
    CHECK(loaded.transform.mirror == pvt::MirrorMode::None);
    CHECK(loaded.swings_enabled);
    CHECK(!loaded.audio_reactive.enabled);

    // A field in the wrong block is kept verbatim but not applied.
    pvt::RenderData recovered = original;
    recovered.phrase_warp = 0.123456;
    const std::string malformed_layer =
        current_layer + "timing.clock.mode\tdefault\n";
    CHECK(pvt::detail::deserialize_layer_config(
        malformed_layer, recovered, &error));
    CHECK(recovered.phrase_warp == original.phrase_warp);
    CHECK(std::any_of(
        recovered.source_compatibility.records.begin(),
        recovered.source_compatibility.records.end(),
        [](const pvt::PreservedConfigRecord& record) {
            return record.key == "timing.clock.mode"
                   && record.value == "default";
        }));
    std::string recovered_layer;
    CHECK(pvt::detail::serialize_layer_config(
        recovered, recovered_layer, &error, &motion_paths));
    CHECK(recovered_layer.find("timing.clock.mode\tdefault\n")
          != std::string::npos);
}

void test_particle_workload_canvas_and_project_boundaries() {
    std::string error;

    // This layer is too expensive on the old synthetic 1920x1080 codec canvas
    // but valid on its real UHD project canvas. Standalone serialization must
    // preserve it enabled and defer admission until project assembly.
    pvt::ProjectConfig high_resolution = pvt::default_project();
    for (auto& effect : high_resolution.layers.front().render.effects) {
        effect.enabled = false;
    }
    auto particles = std::find_if(
        high_resolution.layers.front().render.effects.begin(),
        high_resolution.layers.front().render.effects.end(),
        [](const pvt::EffectConfig& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    CHECK(particles
          != high_resolution.layers.front().render.effects.end());
    if (particles
        == high_resolution.layers.front().render.effects.end()) {
        return;
    }
    particles->enabled = true;
    particles->intensity = 1.0;
    particles->magnitude = 0.1;
    particles->frequency = 10000.0;
    particles->secondary = 0.0;
    particles->radius_pixels = 9.0;
    particles->particle_profile = pvt::ParticleRenderProfile::Defined;
    particles->particle_size_variation = 0.0;
    particles->particle_orientation = pvt::ParticleOrientation::Fixed;

    std::string portable_layer;
    CHECK(pvt::detail::serialize_layer_config(
        high_resolution.layers.front().render, portable_layer, &error));
    pvt::RenderData loaded_layer;
    CHECK(pvt::detail::deserialize_layer_config(
        portable_layer, loaded_layer, &error));
    const auto loaded_particles = std::find_if(
        loaded_layer.effects.begin(), loaded_layer.effects.end(),
        [](const pvt::EffectConfig& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    CHECK(loaded_particles != loaded_layer.effects.end());
    if (loaded_particles != loaded_layer.effects.end()) {
        CHECK(loaded_particles->enabled);
        CHECK(loaded_particles->frequency == 10000.0);
    }
    high_resolution.layers.front().render = loaded_layer;
    CHECK(!pvt::validate(high_resolution).ok);
    high_resolution.canvas.width = 3840;
    high_resolution.canvas.height = 2160;
    CHECK(pvt::validate(high_resolution).ok);

    // A stationary Defined field with a non-fixed orientation draws one stamp
    // per particle regardless of its authored trail amount. Both admission
    // and rendering retain that exact rule; motion makes the same setup exceed
    // the bound again.
    pvt::ProjectConfig stationary = pvt::default_project();
    for (auto& effect : stationary.layers.front().render.effects) {
        effect.enabled = false;
    }
    auto stationary_particles = std::find_if(
        stationary.layers.front().render.effects.begin(),
        stationary.layers.front().render.effects.end(),
        [](const pvt::EffectConfig& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    CHECK(stationary_particles
          != stationary.layers.front().render.effects.end());
    if (stationary_particles
        != stationary.layers.front().render.effects.end()) {
        stationary_particles->enabled = true;
        stationary_particles->intensity = 1.0;
        stationary_particles->magnitude = 0.0;
        stationary_particles->frequency = 4000.0;
        stationary_particles->secondary = 1.0;
        stationary_particles->radius_pixels = 9.0;
        stationary_particles->particle_profile =
            pvt::ParticleRenderProfile::Defined;
        stationary_particles->particle_size_variation = 0.0;
        stationary_particles->particle_orientation =
            pvt::ParticleOrientation::FollowMotion;
        CHECK(pvt::validate(stationary).ok);
        const pvt::RenderConfig stationary_config = pvt::apply_global_config(
            stationary.canvas, stationary.output,
            stationary.layers.front().render);
        std::string stationary_setup;
        CHECK(pvt::detail::serialize_setup_config(
            stationary_config, stationary_setup, &error));
        pvt::RenderConfig stationary_loaded;
        CHECK(pvt::detail::deserialize_setup_config(
            stationary_setup, stationary_loaded, &error));
        const auto stationary_loaded_particles = std::find_if(
            stationary_loaded.effects.begin(), stationary_loaded.effects.end(),
            [](const pvt::EffectConfig& effect) {
                return effect.type == pvt::EffectType::ParticleField;
            });
        CHECK(stationary_loaded_particles != stationary_loaded.effects.end());
        if (stationary_loaded_particles != stationary_loaded.effects.end()) {
            CHECK(stationary_loaded_particles->enabled);
        }
        stationary_particles->magnitude = 0.01;
        CHECK(!pvt::validate(stationary).ok);
    }

    // Individually admissible layers still share one project-frame workload
    // bound. Disabled, grouped-off, and zero-opacity layers are not charged.
    pvt::ProjectConfig stacked = pvt::default_project();
    for (auto& effect : stacked.layers.front().render.effects) {
        effect.enabled = false;
    }
    auto stacked_particles = std::find_if(
        stacked.layers.front().render.effects.begin(),
        stacked.layers.front().render.effects.end(),
        [](const pvt::EffectConfig& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    CHECK(stacked_particles != stacked.layers.front().render.effects.end());
    if (stacked_particles != stacked.layers.front().render.effects.end()) {
        stacked_particles->enabled = true;
        stacked_particles->intensity = 1.0;
        stacked_particles->magnitude = 0.1;
        stacked_particles->frequency = 3000.0;
        stacked_particles->secondary = 0.0;
        stacked_particles->radius_pixels = 9.0;
        stacked_particles->particle_profile =
            pvt::ParticleRenderProfile::Defined;
        stacked_particles->particle_size_variation = 0.0;
        stacked_particles->particle_orientation =
            pvt::ParticleOrientation::Fixed;
        CHECK(pvt::validate(stacked).ok);
        pvt::LayerConfig second = stacked.layers.front();
        second.uuid = pvt::generate_uuid();
        second.file_id += 1U;
        second.name = "Layer 2";
        stacked.layers.push_back(std::move(second));
        CHECK(!pvt::validate(stacked).ok);
        stacked.layers.back().opacity = 0.0;
        CHECK(pvt::validate(stacked).ok);
    }
}

void test_aggregate_particle_bundle_recovery(const fs::path& directory) {
    pvt::ProjectDocument document = pvt::default_project_document();
    document.project.name = "Aggregate Particle Recovery";
    pvt::LayerConfig& first = document.project.layers.front();
    for (pvt::EffectConfig& effect : first.render.effects) {
        effect.enabled = false;
    }
    auto first_particles = std::find_if(
        first.render.effects.begin(), first.render.effects.end(),
        [](const pvt::EffectConfig& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    CHECK(first_particles != first.render.effects.end());
    if (first_particles == first.render.effects.end()) return;
    const std::size_t particle_index = static_cast<std::size_t>(
        std::distance(first.render.effects.begin(), first_particles));
    first_particles->enabled = true;
    first_particles->intensity = 1.0;
    first_particles->magnitude = 0.1;
    first_particles->frequency = 3100.0;
    first_particles->secondary = 0.0;
    first_particles->radius_pixels = 9.0;
    first_particles->particle_profile =
        pvt::ParticleRenderProfile::LegacyGlow;
    first_particles->particle_size_variation = 0.0;
    first_particles->particle_orientation =
        pvt::ParticleOrientation::Fixed;

    pvt::LayerConfig second = first;
    second.uuid = pvt::generate_uuid();
    second.file_id += 1U;
    second.name = "Later current layer";
    second.opacity = 0.0;
    second.render.effects[particle_index].particle_profile =
        pvt::ParticleRenderProfile::Defined;
    document.project.layers.push_back(std::move(second));

    // Each layer is safe alone. Enabling both exceeds the one-frame project
    // budget, so the saved fixture temporarily hides the later layer and then
    // simulates an older/directly edited snapshot that enables it.
    CHECK(pvt::validate(document.project).ok);
    pvt::ProjectConfig later_only = document.project;
    later_only.layers.front().opacity = 0.0;
    later_only.layers.back().opacity = 1.0;
    CHECK(pvt::validate(later_only).ok);
    pvt::ProjectConfig aggregate = document.project;
    aggregate.layers.back().opacity = 1.0;
    CHECK(!pvt::validate(aggregate).ok);

    const fs::path bundle = directory
                            / pvt::detail::path_from_utf8(
                                portable_root(document.project.name));
    std::string error;
    pvt::BundleSaveReport report;
    CHECK(pvt::save_project_document(
        document, as_utf8(bundle), &report, &error));
    CHECK(report.created_version && report.version == 0U);

    // Keep one layer on the prior layer/setup schema and the other current.
    // Their independent admission remains valid on the real project canvas.
    const fs::path legacy_layer_path = bundle / "0" / "0.pvt";
    std::istringstream current_layer(read_bytes(legacy_layer_path));
    std::ostringstream legacy_layer;
    std::string line;
    CHECK(static_cast<bool>(std::getline(current_layer, line)));
    CHECK(line == "PVT_LAYER\t15");
    legacy_layer << "PVT_LAYER\t12\n";
    const auto has_suffix = [](const std::string& value,
                               const std::string& suffix) {
        return value.size() >= suffix.size()
               && value.compare(value.size() - suffix.size(), suffix.size(),
                                suffix) == 0;
    };
    while (std::getline(current_layer, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        const bool current_particle_field =
            has_suffix(key, ".particle_profile")
            || has_suffix(key, ".particle_size_variation")
            || has_suffix(key, ".particle_definition")
            || has_suffix(key, ".particle_twinkle")
            || has_suffix(key, ".particle_seed")
            || has_suffix(key, ".particle_orientation")
            || has_suffix(key, ".particle_rotation_degrees");
        const bool current_surface_field = key == "surface.projection"
            || key == "surface.sizing" || key == "surface.outside"
            || key.rfind("surface.rotation_", 0U) == 0U
            || key == "surface.size_percent"
            || key.rfind("surface.scale_", 0U) == 0U
            || key.rfind("surface.position_", 0U) == 0U
            || key == "surface.camera_distance"
            || key == "surface.focal_length"
            || key.rfind("surface.light_", 0U) == 0U
            || key == "surface.composite_backfaces"
            || key == "surface.normalize_obj";
        const bool current_lfo_field =
            key.rfind("parameter_lfos.", 0U) == 0U;
        if (!current_particle_field && !current_surface_field
            && !current_lfo_field) {
            legacy_layer << line << '\n';
        }
    }
    legacy_layer << "surface.rotations_per_loop\t0\n"
                 << "surface.phase_degrees\t0\n";
    CHECK(write_bytes(legacy_layer_path, legacy_layer.str()));

    const fs::path version_metadata_path = bundle / "0" / "metadata.txt";
    std::string version_metadata = read_bytes(version_metadata_path);
    CHECK(replace_record_value(
        version_metadata, "layers.1.opacity", "1"));
    CHECK(write_bytes(version_metadata_path, version_metadata));

    pvt::ProjectDocument loaded;
    CHECK(pvt::load_project_document(as_utf8(bundle), loaded, &error));
    CHECK(loaded.project.layers.size() == 2U);
    if (loaded.project.layers.size() != 2U) return;
    const pvt::EffectConfig& admitted =
        loaded.project.layers.front().render.effects[particle_index];
    const pvt::EffectConfig& recovered =
        loaded.project.layers.back().render.effects[particle_index];
    CHECK(admitted.enabled);
    CHECK(admitted.particle_profile
          == pvt::ParticleRenderProfile::LegacyGlow);
    CHECK(!recovered.enabled);
    CHECK(recovered.particle_profile
          == pvt::ParticleRenderProfile::Defined);
    CHECK(pvt::validate(loaded.project).ok);

    const std::string recovery_key =
        "effects." + std::to_string(particle_index)
        + ".recovery_unsafe_particle_enabled";
    const pvt::ConfigCompatibility& compatibility =
        loaded.project.layers.back().render.source_compatibility;
    CHECK(std::count_if(
              compatibility.records.begin(), compatibility.records.end(),
              [&recovery_key](const pvt::PreservedConfigRecord& record) {
                  return record.key == recovery_key && record.value == "1"
                         && !record.rejected;
              })
          == 1);
    CHECK(std::count_if(
              compatibility.repair_notes.begin(),
              compatibility.repair_notes.end(),
              [&recovery_key](const std::string& note) {
                  return note.find(recovery_key) != std::string::npos;
              })
          == 1);
}

void test_render_output_codec_backward_compatibility() {
    pvt::ProjectConfig defaults = pvt::default_project();
    pvt::CanvasLoopConfig canvas = defaults.canvas;
    pvt::ExportConfig output = defaults.output;
    canvas.width = 320;
    canvas.height = 180;
    canvas.total_frames = 144;
    canvas.fps = 48.0;
    canvas.clock.mode = pvt::ClockMode::Music;
    canvas.clock.interpolation = pvt::ClockInterpolation::Smoothstep;
    canvas.clock.fit = pvt::ClockFit::FitSequence;
    canvas.clock.frame_interval = 6;
    canvas.clock.time_interval_microseconds = 125000;
    canvas.clock.meter.expression = "3+2+3/8";
    canvas.clock.meter.bpm = 132.0;
    canvas.clock.meter.tempo_note_denominator = 8;
    canvas.clock.music_tempo = pvt::MusicTempoMode::Half;
    canvas.clock.music_swing_policy = pvt::MusicSwingPolicy::SuppressGlobal;
    canvas.clock.beat_offset_microseconds = 17000;
    canvas.clock.phase_offset_degrees = 9.5;
    canvas.clock.reverse = true;
    canvas.clock.music.analyzer_version = "codec-test/1";
    canvas.clock.music.source_sha256 = std::string(64U, 'b');
    canvas.clock.music.source_basename = "meter test.wav";
    canvas.clock.music.source_format = "wav-f32";
    canvas.clock.music.source_frame_count = 96000U;
    canvas.clock.music.source_sample_rate = 48000U;
    canvas.clock.music.source_channel_count = 2U;
    canvas.clock.music.duration_seconds = 2.0;
    canvas.clock.music.detected_bpm = 132.0;
    canvas.clock.music.tempo_confidence = 0.88;
    canvas.clock.music.beat_times_seconds = {0.0, 0.4545, 0.9091, 1.3636};
    canvas.clock.music.tempo_points = {{0.0, 132.0, 0.88}};
    canvas.clock.music.feature_samples = {
        {0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F},
        {0.7F, 0.6F, 0.5F, 0.4F, 0.3F, 0.2F},
    };
    canvas.clock.audio_processing.high_pass_enabled = true;
    canvas.clock.audio_processing.high_pass_hz = 35.0;
    canvas.clock.audio_processing.equalizer_enabled = true;
    canvas.clock.audio_processing.equalizer_bands[4U].gain_db = 2.25;
    canvas.clock.audio_processing.frequency_streams = {
        {"music-bass", "Music bass", 35.0, 300.0}};
    canvas.clock.frequency_stream_uuid = "music-bass";
    canvas.clock.music.input_processing = canvas.clock.audio_processing;
    pvt::MusicFrequencyStreamAnalysis music_bass;
    music_bass.uuid = "music-bass";
    music_bass.low_hz = 35.0;
    music_bass.high_hz = 300.0;
    music_bass.detected_bpm = 132.0;
    music_bass.tempo_confidence = 0.8;
    music_bass.beat_times_seconds = canvas.clock.music.beat_times_seconds;
    music_bass.tempo_points = canvas.clock.music.tempo_points;
    music_bass.feature_samples = canvas.clock.music.feature_samples;
    canvas.clock.music.frequency_streams = {music_bass};
    canvas.audio_reactive_defaults.enabled = true;
    canvas.audio_reactive_defaults.synchronized_only = false;
    canvas.audio_reactive_defaults.wave_source = pvt::MusicFeature::Bass;
    canvas.audio_reactive_defaults.wave_amount = 0.26;
    canvas.audio_reactive_defaults.effect_source = pvt::MusicFeature::Onset;
    canvas.audio_reactive_defaults.effect_amount = 0.37;
    canvas.audio_reactive_defaults.color_enabled = false;
    canvas.motion_paths.push_back(
        pvt::default_ellipse_path(91U, 100U, "Codec ellipse"));
    const std::string midi_uuid =
        "11111111-1111-4111-8111-111111111111";
    const std::string audio_uuid =
        "22222222-2222-4222-8222-222222222222";
    const std::string osc_uuid =
        "33333333-3333-4333-8333-333333333333";
    const std::string foot_uuid =
        "44444444-4444-4444-8444-444444444444";
    const std::string layer_uuid =
        "55555555-5555-4555-8555-555555555555";
    const std::string scene_uuid =
        "66666666-6666-4666-8666-666666666666";
    const std::string layer_midi_output_uuid =
        "77777777-7777-4777-8777-777777777777";
    canvas.live.enabled = true;
    canvas.live.endpoints = {
        {midi_uuid, "Stage MIDI", pvt::LiveEndpointProtocol::Midi,
         pvt::LiveEndpointDirection::Bidirectional, 1700, -800},
        {audio_uuid, "Band mix", pvt::LiveEndpointProtocol::Audio,
         pvt::LiveEndpointDirection::Input, 23000, 0},
        {osc_uuid, "Lighting desk", pvt::LiveEndpointProtocol::Osc,
         pvt::LiveEndpointDirection::Input, 0, 0},
        {foot_uuid, "Pedal board", pvt::LiveEndpointProtocol::FootController,
         pvt::LiveEndpointDirection::Input, 4000, 0},
        {layer_midi_output_uuid, "Layer MIDI out",
         pvt::LiveEndpointProtocol::Midi,
         pvt::LiveEndpointDirection::Output, 0, 1200},
    };
    pvt::LiveSceneConfig chorus;
    chorus.uuid = scene_uuid;
    chorus.name = "Chorus blast";
    chorus.transition_milliseconds = 125;
    chorus.values = {
        {"project.canvas.fps", pvt::LiveSceneValueType::Real, "59.94"},
        {"project.output.write_alpha", pvt::LiveSceneValueType::Boolean, "1"},
        {"layers/primary/effects/glow/enabled",
         pvt::LiveSceneValueType::Boolean, "1"},
    };
    canvas.live.scenes.push_back(chorus);
    canvas.live.startup_scene_uuid = scene_uuid;
    pvt::LiveControlMapping midi_mapping;
    midi_mapping.name = "Glow pedal";
    midi_mapping.endpoint_uuid = midi_uuid;
    midi_mapping.midi_channel = 2;
    midi_mapping.control_number = 64;
    midi_mapping.target_path = "layers/primary/effects/glow/intensity";
    midi_mapping.output_minimum = -3.0;
    midi_mapping.output_maximum = 7.5;
    midi_mapping.curve = 1.4;
    midi_mapping.dead_zone = 0.02;
    midi_mapping.smoothing_milliseconds = 18;
    pvt::LiveControlMapping osc_mapping;
    osc_mapping.name = "Desk blackout";
    osc_mapping.endpoint_uuid = osc_uuid;
    osc_mapping.input = pvt::LiveControlInput::OscValue;
    osc_mapping.osc_address = "/pvt/blackout";
    osc_mapping.target = pvt::LiveMappingTarget::Action;
    osc_mapping.target_path.clear();
    osc_mapping.action = pvt::LiveAction::Blackout;
    osc_mapping.mode = pvt::LiveMappingMode::Momentary;
    pvt::LiveControlMapping foot_mapping;
    foot_mapping.name = "Recall chorus";
    foot_mapping.endpoint_uuid = foot_uuid;
    foot_mapping.input = pvt::LiveControlInput::Footswitch;
    foot_mapping.control_number = 3;
    foot_mapping.target = pvt::LiveMappingTarget::Scene;
    foot_mapping.target_path.clear();
    foot_mapping.scene_uuid = scene_uuid;
    foot_mapping.mode = pvt::LiveMappingMode::Trigger;
    canvas.live.mappings = {midi_mapping, osc_mapping, foot_mapping};
    canvas.live.clock_inputs = {
        {true, pvt::LiveClockTarget::Project, {},
         pvt::LiveClockInputSource::MidiClock, midi_uuid, 0, true, 750, {}},
        {true, pvt::LiveClockTarget::Layer, layer_uuid,
         pvt::LiveClockInputSource::AudioStream, audio_uuid, 2, false, 300, {}},
    };
    canvas.live.midi_clock_outputs = {
        {true, pvt::LiveClockTarget::Project, {}, midi_uuid, true, true},
        {true, pvt::LiveClockTarget::Layer, layer_uuid,
         layer_midi_output_uuid, false, false},
    };
    canvas.live.output.fullscreen = true;
    canvas.live.output.prefer_secondary_display = true;
    canvas.live.output.hide_cursor = false;
    canvas.live.safety.dropout_behavior =
        pvt::LiveDropoutBehavior::LastGoodFrame;
    canvas.live.safety.watchdog_timeout_milliseconds = 83;
    canvas.live.safety.audio_dropout_grace_milliseconds = 410;
    canvas.live.safety.last_good_frame_timeout_milliseconds = 5000;
    canvas.live.safety.prevent_device_sleep = true;
    canvas.live.audio_processing.low_pass_enabled = true;
    canvas.live.audio_processing.low_pass_hz = 16000.0;
    canvas.live.audio_processing.frequency_streams = {
        {"live-mid", "Live mid", 250.0, 2500.0}};
    canvas.live.clock_inputs[1U].frequency_stream_uuid = "live-mid";
    output.filename_prefix = "music-sync_";

    std::string version_two;
    std::string error;
    CHECK(pvt::detail::serialize_render_output_config(
        canvas, output, version_two, &error));
    CHECK(version_two.rfind("PVT_RENDER_OUTPUT\t7\n", 0U) == 0U);
    pvt::CanvasLoopConfig round_trip;
    pvt::ExportConfig round_trip_output;
    CHECK(pvt::detail::deserialize_render_output_config(
        version_two, round_trip, round_trip_output, &error));
    CHECK(round_trip.width == 320 && round_trip.height == 180);
    CHECK(round_trip.clock.mode == pvt::ClockMode::Music);
    CHECK(round_trip.clock.interpolation
          == pvt::ClockInterpolation::Smoothstep);
    CHECK(round_trip.clock.music_swing_policy
          == pvt::MusicSwingPolicy::SuppressGlobal);
    CHECK(round_trip.clock.music.source_sha256 == std::string(64U, 'b'));
    CHECK(round_trip.clock.music.beat_times_seconds.size() == 4U);
    CHECK(round_trip.clock.audio_processing.high_pass_enabled);
    CHECK(round_trip.clock.audio_processing.equalizer_bands[4U].gain_db == 2.25);
    CHECK(round_trip.clock.frequency_stream_uuid == "music-bass");
    CHECK(round_trip.clock.music.frequency_streams.size() == 1U);
    CHECK(round_trip.audio_reactive_defaults.enabled);
    CHECK(!round_trip.audio_reactive_defaults.synchronized_only);
    CHECK(round_trip.audio_reactive_defaults.wave_source
          == pvt::MusicFeature::Bass);
    CHECK(round_trip.audio_reactive_defaults.effect_amount == 0.37);
    CHECK(!round_trip.audio_reactive_defaults.color_enabled);
    CHECK(round_trip.motion_paths.size() == 1U);
    CHECK(round_trip.motion_paths.front().nodes.size() == 4U);
    CHECK(round_trip.live.enabled);
    CHECK(round_trip.live.endpoints.size() == 5U);
    CHECK(round_trip.live.endpoints[0].name == "Stage MIDI");
    CHECK(round_trip.live.endpoints[1].input_latency_microseconds == 23000);
    CHECK(round_trip.live.mappings.size() == 3U);
    CHECK(round_trip.live.mappings[0].output_minimum == -3.0);
    CHECK(round_trip.live.mappings[1].osc_address == "/pvt/blackout");
    CHECK(round_trip.live.mappings[2].scene_uuid == scene_uuid);
    CHECK(round_trip.live.clock_inputs.size() == 2U);
    CHECK(round_trip.live.clock_inputs[0].source
          == pvt::LiveClockInputSource::MidiClock);
    CHECK(round_trip.live.clock_inputs[1].source
          == pvt::LiveClockInputSource::AudioStream);
    CHECK(round_trip.live.midi_clock_outputs.size() == 2U);
    CHECK(round_trip.live.midi_clock_outputs[1].source
          == pvt::LiveClockTarget::Layer);
    CHECK(round_trip.live.scenes.size() == 1U);
    CHECK(round_trip.live.scenes.front().values.size() == 3U);
    CHECK(round_trip.live.startup_scene_uuid == scene_uuid);
    CHECK(round_trip.live.safety.watchdog_timeout_milliseconds == 83);
    CHECK(round_trip.live.safety.prevent_device_sleep);
    CHECK(round_trip.live.audio_processing.low_pass_hz == 16000.0);
    CHECK(round_trip.live.clock_inputs[1U].frequency_stream_uuid == "live-mid");
    CHECK(!round_trip.live.output.hide_cursor);
    CHECK(round_trip_output.filename_prefix == "music-sync_");
    CHECK(version_two.find("device_id") == std::string::npos);
    CHECK(version_two.find("device_uid") == std::string::npos);
    CHECK(version_two.find("captured_stream") == std::string::npos);

    // Render/output v5 is setup-v8 data. Loading it must keep the historical
    // project settings and supply a neutral Live block rather than requiring
    // any of the v6 live.* records.
    std::istringstream version_six_input(version_two);
    std::ostringstream version_five_output;
    std::string version_line;
    CHECK(static_cast<bool>(std::getline(version_six_input, version_line)));
    version_five_output << "PVT_RENDER_OUTPUT\t5\n";
    while (std::getline(version_six_input, version_line)) {
        const std::string key =
            version_line.substr(0U, version_line.find('\t'));
        if (key.rfind("live.", 0U) != 0U) {
            version_five_output << version_line << '\n';
        }
    }
    pvt::CanvasLoopConfig version_five_canvas;
    pvt::ExportConfig version_five_export;
    CHECK(pvt::detail::deserialize_render_output_config(
        version_five_output.str(), version_five_canvas,
        version_five_export, &error));
    CHECK(!version_five_canvas.live.enabled);
    CHECK(version_five_canvas.live.endpoints.empty());
    CHECK(version_five_canvas.clock.mode == pvt::ClockMode::Music);

    std::string analysis;
    std::string split_output;
    CHECK(pvt::detail::serialize_music_analysis_config(
        canvas.clock.music, analysis, &error));
    CHECK(analysis.rfind("PVT_MUSIC_ANALYSIS\t2\n", 0U) == 0U);
    CHECK(pvt::detail::serialize_split_render_output_config(
        canvas, output, split_output, &error));
    CHECK(split_output.rfind("PVT_RENDER_OUTPUT_SPLIT\t5\n", 0U) == 0U);
    CHECK(split_output.find("timing.music.") == std::string::npos);
    CHECK(split_output.size() < version_two.size());
    pvt::CanvasLoopConfig combined_canvas;
    pvt::ExportConfig combined_output;
    CHECK(pvt::detail::deserialize_split_render_output_config(
        split_output, analysis, combined_canvas, combined_output, &error));
    std::string combined_canonical;
    CHECK(pvt::detail::serialize_render_output_config(
        combined_canvas, combined_output, combined_canonical, &error));
    CHECK(combined_canonical == version_two);

    std::istringstream split_version_four_input(split_output);
    std::ostringstream split_version_three_output;
    CHECK(static_cast<bool>(
        std::getline(split_version_four_input, version_line)));
    split_version_three_output << "PVT_RENDER_OUTPUT_SPLIT\t3\n";
    while (std::getline(split_version_four_input, version_line)) {
        const std::string key =
            version_line.substr(0U, version_line.find('\t'));
        if (key.rfind("live.", 0U) != 0U) {
            split_version_three_output << version_line << '\n';
        }
    }
    pvt::CanvasLoopConfig split_version_three_canvas;
    pvt::ExportConfig split_version_three_export;
    CHECK(pvt::detail::deserialize_split_render_output_config(
        split_version_three_output.str(), analysis,
        split_version_three_canvas, split_version_three_export, &error));
    CHECK(!split_version_three_canvas.live.enabled);
    CHECK(split_version_three_canvas.live.mappings.empty());

    const std::string split_with_misplaced_music =
        split_output + "timing.music.duration_seconds\t999\n";
    pvt::CanvasLoopConfig misplaced_canvas;
    pvt::ExportConfig misplaced_output;
    CHECK(pvt::detail::deserialize_split_render_output_config(
        split_with_misplaced_music, analysis, misplaced_canvas,
        misplaced_output, &error));
    CHECK(misplaced_canvas.clock.music.duration_seconds == 2.0);
    CHECK(std::any_of(
        misplaced_canvas.output_compatibility.records.begin(),
        misplaced_canvas.output_compatibility.records.end(),
        [](const pvt::PreservedConfigRecord& record) {
            return record.key == "timing.music.duration_seconds"
                   && record.value == "999";
        }));

    // Split v1 shipped before reusable paths. It must map to setup v6 instead
    // of being reinterpreted as today's schema and demanding paths.count.
    std::istringstream split_input(split_output);
    std::ostringstream legacy_split;
    std::string split_line;
    CHECK(static_cast<bool>(std::getline(split_input, split_line)));
    legacy_split << "PVT_RENDER_OUTPUT_SPLIT\t1\n";
    while (std::getline(split_input, split_line)) {
        const std::string key = split_line.substr(0U, split_line.find('\t'));
        if (key.rfind("paths.", 0U) != 0U
            && key.rfind("audio_response_defaults.", 0U) != 0U
            && key.rfind("live.", 0U) != 0U) {
            legacy_split << split_line << '\n';
        }
    }
    pvt::CanvasLoopConfig legacy_split_canvas;
    pvt::ExportConfig legacy_split_output;
    CHECK(pvt::detail::deserialize_split_render_output_config(
        legacy_split.str(), analysis, legacy_split_canvas,
        legacy_split_output, &error));
    CHECK(legacy_split_canvas.motion_paths.empty());
    CHECK(!legacy_split_canvas.audio_reactive_defaults.enabled);
    CHECK(legacy_split_canvas.clock.mode == pvt::ClockMode::Music);
    CHECK(legacy_split_output.filename_prefix == "music-sync_");
    pvt::MusicAnalysis analysis_round_trip;
    CHECK(pvt::detail::deserialize_music_analysis_config(
        analysis, analysis_round_trip, &error));
    CHECK(analysis_round_trip.source_sha256 == std::string(64U, 'b'));
    CHECK(analysis_round_trip.feature_samples.size() == 2U);
    const std::string malformed_analysis =
        analysis + "output.filename_prefix\twrong-block\n";
    analysis_round_trip.source_basename = "unchanged.wav";
    CHECK(pvt::detail::deserialize_music_analysis_config(
        malformed_analysis, analysis_round_trip, &error));
    CHECK(analysis_round_trip.source_basename == "meter test.wav");
    CHECK(std::any_of(
        analysis_round_trip.compatibility.records.begin(),
        analysis_round_trip.compatibility.records.end(),
        [](const pvt::PreservedConfigRecord& record) {
            return record.key == "output.filename_prefix"
                   && record.value == "wrong-block";
        }));
    std::string future_analysis = malformed_analysis;
    future_analysis.replace(0U, std::string("PVT_MUSIC_ANALYSIS\t2").size(),
                            "PVT_MUSIC_ANALYSIS\t999");
    CHECK(pvt::detail::deserialize_music_analysis_config(
        future_analysis, analysis_round_trip, &error));
    CHECK(analysis_round_trip.source_basename == "meter test.wav");

    std::istringstream input(version_two);
    std::ostringstream legacy;
    std::string line;
    CHECK(static_cast<bool>(std::getline(input, line)));
    CHECK(line == "PVT_RENDER_OUTPUT\t7");
    legacy << "PVT_RENDER_OUTPUT\t1\n";
    while (std::getline(input, line)) {
        const std::size_t tab = line.find('\t');
        const std::string key = line.substr(0U, tab);
        const bool v2_only = key.rfind("timing.clock.", 0U) == 0U
                             || key.rfind("timing.music.", 0U) == 0U
                             || key.rfind("paths.", 0U) == 0U
                             || key.rfind("audio_response_defaults.", 0U)
                                    == 0U
                             || key.rfind("live.", 0U) == 0U;
        if (!v2_only) legacy << line << '\n';
    }

    pvt::CanvasLoopConfig loaded_legacy = canvas;
    pvt::ExportConfig loaded_legacy_output;
    CHECK(pvt::detail::deserialize_render_output_config(
        legacy.str(), loaded_legacy, loaded_legacy_output, &error));
    CHECK(loaded_legacy.width == 320 && loaded_legacy.height == 180);
    CHECK(loaded_legacy.clock.mode == pvt::ClockMode::Default);
    CHECK(loaded_legacy.clock.music.source_sha256.empty());
    CHECK(loaded_legacy.clock.music.beat_times_seconds.empty());
    CHECK(!loaded_legacy.audio_reactive_defaults.enabled);
    CHECK(loaded_legacy_output.filename_prefix == "music-sync_");

    loaded_legacy.width = 777;
    const std::string malformed =
        version_two + "audio_reactive.enabled\t1\n";
    CHECK(pvt::detail::deserialize_render_output_config(
        malformed, loaded_legacy, loaded_legacy_output, &error));
    CHECK(loaded_legacy.width == 320);
    CHECK(std::any_of(
        loaded_legacy.output_compatibility.records.begin(),
        loaded_legacy.output_compatibility.records.end(),
        [](const pvt::PreservedConfigRecord& record) {
            return record.key == "audio_reactive.enabled"
                   && record.value == "1";
        }));
}

void test_sha_and_archive_guards(const fs::path& directory) {
    std::string digest;
    std::string error;
    CHECK(pvt::detail::sha256_hex("", digest, &error));
    CHECK(digest == "e3b0c44298fc1c149afbf4c8996fb924"
                    "27ae41e4649b934ca495991b7852b855");
    CHECK(pvt::detail::sha256_hex("abc", digest, &error));
    CHECK(digest == "ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad");

    pvt::detail::BundleFileSet oversized;
    oversized.root_name = "Guard";
    oversized.files["metadata.txt"] = std::string(8U * 1024U * 1024U + 1U, 'x');
    const fs::path rejected = directory / "oversized.zip";
    CHECK(!pvt::detail::write_bundle_file_set(as_utf8(rejected), oversized, &error));
    CHECK(!fs::exists(rejected));

    pvt::detail::BundleFileSet colliding;
    colliding.root_name = "Guard";
    colliding.files["A.txt"] = "a";
    colliding.files["a.txt"] = "b";
    CHECK(!pvt::detail::write_bundle_file_set(
        as_utf8(directory / "colliding.zip"), colliding, &error));

    const fs::path traversal = directory / "traversal.zip";
    CHECK(write_test_zip(traversal, {"Root/../outside"}));
    pvt::detail::BundleFileSet loaded;
    CHECK(!pvt::detail::read_bundle_file_set(as_utf8(traversal), loaded, &error));

    const fs::path backslash = directory / "backslash.zip";
    CHECK(write_test_zip(backslash, {"Root\\metadata.txt"}));
    CHECK(!pvt::detail::read_bundle_file_set(as_utf8(backslash), loaded, &error));

    const fs::path duplicate = directory / "duplicate.zip";
    CHECK(write_test_zip(duplicate, {"Root/A.txt", "Root/a.txt"}));
    CHECK(!pvt::detail::read_bundle_file_set(as_utf8(duplicate), loaded, &error));

    const fs::path symlink = directory / "symlink.zip";
    CHECK(write_test_zip(symlink, {"Root/link"}, true));
    CHECK(!pvt::detail::read_bundle_file_set(as_utf8(symlink), loaded, &error));
}

void test_archive_compare_and_swap(const fs::path& directory) {
    for (const bool zip : {false, true}) {
        const fs::path path = directory / (zip ? "cas.zip" : "cas-directory");
        pvt::detail::BundleFileSet state_a;
        state_a.root_name = zip ? "cas" : "cas-directory";
        state_a.files["metadata.txt"] = "state-a";
        state_a.files["metadata.sha256"] = "digest-a";
        state_a.files["current"] = "current-a";
        state_a.files["assets/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/stable.bin"] =
            std::string(512U * 1024U, 's');
        std::string error;
        CHECK(pvt::detail::write_bundle_file_set(as_utf8(path), state_a, &error));

        pvt::detail::BundleFileSet observed_a;
        std::string digest_a;
        CHECK(pvt::detail::read_bundle_file_set(as_utf8(path), observed_a, &error));
        CHECK(pvt::detail::bundle_file_set_digest(observed_a, digest_a, &error));

        pvt::detail::BundleFileSet state_b = state_a;
        state_b.files["metadata.txt"] = "state-b";
        state_b.files["metadata.sha256"] = "digest-b";
        state_b.files["current"] = "current-b";
        const bool wrote_b =
            pvt::detail::write_bundle_file_set_if_unchanged(
                as_utf8(path), state_b, true, digest_a, &error);
        if (!wrote_b) std::cerr << "CAS setup failed: " << error << '\n';
        CHECK(wrote_b);

        pvt::detail::BundleFileSet stale_write = state_a;
        stale_write.files["metadata.txt"] = "stale-overwrite";
        CHECK(!pvt::detail::write_bundle_file_set_if_unchanged(
            as_utf8(path), stale_write, true, digest_a, &error));

        pvt::detail::BundleFileSet final_state;
        CHECK(pvt::detail::read_bundle_file_set(as_utf8(path), final_state, &error));
        CHECK(final_state.files == state_b.files);
        CHECK(final_state.files.at(
                  "assets/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/stable.bin")
              == state_a.files.at(
                  "assets/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/stable.bin"));
    }

#if !defined(_WIN32)
    pvt::detail::BundleFileSet lock_state;
    lock_state.root_name = "locked";
    lock_state.files["metadata.txt"] = "lock-test";
    const fs::path locked_path = directory / "locked.zip";
    const fs::path lock_path = directory / ".locked.zip.pvt-save.lock";
    const int lock_descriptor = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
    CHECK(lock_descriptor >= 0);
    if (lock_descriptor >= 0) {
        CHECK(::flock(lock_descriptor, LOCK_EX | LOCK_NB) == 0);
        std::string error;
        CHECK(!pvt::detail::write_bundle_file_set(
            as_utf8(locked_path), lock_state, &error));
        CHECK(error.find("already saving") != std::string::npos);
        CHECK(!fs::exists(locked_path));
        CHECK(::flock(lock_descriptor, LOCK_UN) == 0);
        CHECK(::close(lock_descriptor) == 0);
        CHECK(pvt::detail::write_bundle_file_set(
            as_utf8(locked_path), lock_state, &error));
        CHECK(!fs::exists(lock_path));
    }

    const fs::path victim = directory / "lock-victim.txt";
    const fs::path hostile_path = directory / "hostile-lock.zip";
    const fs::path hostile_lock =
        directory / ".hostile-lock.zip.pvt-save.lock";
    CHECK(write_bytes(victim, "do not touch"));
    std::error_code symlink_error;
    fs::create_symlink(victim, hostile_lock, symlink_error);
    if (!symlink_error) {
        std::string error;
        CHECK(!pvt::detail::write_bundle_file_set(
            as_utf8(hostile_path), lock_state, &error));
        CHECK(read_bytes(victim) == "do not touch");
        CHECK(!fs::exists(hostile_path));
    }
#endif
}

void test_directory_versions_and_names(const fs::path& directory) {
    pvt::ProjectDocument document = pvt::default_project_document();
    document.project.name = "Fire: Night";
    document.project.canvas.clock.mode = pvt::ClockMode::Time;
    document.project.canvas.clock.interpolation = pvt::ClockInterpolation::Hold;
    document.project.canvas.clock.time_interval_microseconds = 250000;
    document.project.canvas.clock.meter.expression = "3+2+3/8";
    auto& initial_render = document.project.layers.front().render;
    initial_render.swings.front().center_x = 0.31;
    initial_render.swings.front().center_y = 0.69;
    initial_render.swings.front().radius = 0.27;
    initial_render.effects.front().space = pvt::EffectSpace::Surface;
    initial_render.effects.front().center_x = 0.42;
    initial_render.effects.front().center_y = 0.58;
    initial_render.effects.front().area_radius = 0.36;
    initial_render.palette = pvt::default_palette(2U);
    initial_render.transform.flip_horizontal = true;
    initial_render.transform.mirror = pvt::MirrorMode::TopToBottom;
    initial_render.swings_enabled = false;
    document.project.canvas.audio_reactive_defaults.enabled = true;
    document.project.canvas.audio_reactive_defaults.wave_source =
        pvt::MusicFeature::Onset;
    document.project.canvas.audio_reactive_defaults.wave_amount = 0.41;
    initial_render.audio_reactive_override_enabled = false;
    initial_render.audio_reactive.wave_source = pvt::MusicFeature::Treble;
    initial_render.audio_reactive.wave_amount = 0.57;
    initial_render.waves.front().audio_response =
        pvt::AudioResponseMode::Beat;
    initial_render.effects.front().audio_response =
        pvt::AudioResponseMode::Energy;
    document.project.canvas.motion_paths.push_back(
        pvt::default_ellipse_path(91U, 100U, "Bundle ellipse"));
    initial_render.waves.front().path.enabled = true;
    initial_render.waves.front().path.path_id = 91U;
    initial_render.waves.front().path.cycles_per_loop = 3;
    initial_render.waves.front().path.follow_tangent = true;
    document.project.canvas.output_compatibility.records.push_back(
        {"future.output.sparkle", "maximum", false});
    document.project.canvas.output_compatibility.repair_notes.push_back(
        "Recovered a missing test field.");
    const std::string opened = document.last_opened_utc;
    const fs::path bundle = directory
                            / pvt::detail::path_from_utf8(
                                portable_root(document.project.name));
    std::string error;
    pvt::BundleSaveReport report;
    CHECK(pvt::save_project_document(document, as_utf8(bundle), &report, &error));
    CHECK(report.created_version && report.version == 0U);
    CHECK(document.project.canvas.output_compatibility.repair_notes.empty());
    CHECK(document.project.canvas.output_compatibility.records.size() == 1U);
    CHECK(document.last_opened_utc == opened);
    CHECK(!document.last_saved_utc.empty());
    CHECK(fs::is_regular_file(fs::symlink_status(bundle / "current")));
    CHECK(fs::exists(bundle / "0" / "metadata.txt"));
    CHECK(fs::exists(bundle / "0" / "render_output.txt"));
    CHECK(fs::exists(bundle / "0" / "0.pvt"));

    pvt::ProjectDocument loaded;
    CHECK(pvt::load_project_document(as_utf8(bundle), loaded, &error));
    CHECK(loaded.project.name == "Fire: Night");
    CHECK(loaded.project.canvas.clock.mode == pvt::ClockMode::Time);
    CHECK(loaded.project.canvas.clock.interpolation
          == pvt::ClockInterpolation::Hold);
    CHECK(loaded.project.canvas.clock.time_interval_microseconds == 250000);
    CHECK(loaded.project.canvas.clock.meter.expression == "3+2+3/8");
    CHECK(loaded.project.canvas.motion_paths.size() == 1U);
    CHECK(loaded.project.canvas.motion_paths.front().name == "Bundle ellipse");
    CHECK(loaded.project.canvas.motion_paths.front().nodes.size() == 4U);
    CHECK(loaded.project.canvas.audio_reactive_defaults.enabled);
    CHECK(loaded.project.canvas.audio_reactive_defaults.wave_source
          == pvt::MusicFeature::Onset);
    CHECK(loaded.project.canvas.audio_reactive_defaults.wave_amount == 0.41);
    const auto& loaded_render = loaded.project.layers.front().render;
    CHECK(loaded_render.swings.front().center_x == 0.31);
    CHECK(loaded_render.swings.front().center_y == 0.69);
    CHECK(loaded_render.swings.front().radius == 0.27);
    CHECK(loaded_render.effects.front().space == pvt::EffectSpace::Surface);
    CHECK(loaded_render.effects.front().area_radius == 0.36);
    CHECK(loaded_render.palette.enabled);
    CHECK(loaded_render.palette.name == "Vaporwave");
    CHECK(loaded_render.palette.colors.size()
          == pvt::default_palette(2U).colors.size());
    CHECK(loaded_render.transform.flip_horizontal);
    CHECK(loaded_render.transform.mirror == pvt::MirrorMode::TopToBottom);
    CHECK(!loaded_render.swings_enabled);
    CHECK(!loaded_render.audio_reactive_override_enabled);
    CHECK(loaded_render.audio_reactive.wave_source
          == pvt::MusicFeature::Treble);
    CHECK(loaded_render.audio_reactive.wave_amount == 0.57);
    CHECK(loaded_render.waves.front().audio_response
          == pvt::AudioResponseMode::Beat);
    CHECK(loaded_render.effects.front().audio_response
          == pvt::AudioResponseMode::Energy);
    CHECK(loaded_render.waves.front().path.enabled);
    CHECK(loaded_render.waves.front().path.path_id == 91U);
    CHECK(loaded_render.waves.front().path.follow_tangent);
    CHECK(std::any_of(
        loaded.project.canvas.output_compatibility.records.begin(),
        loaded.project.canvas.output_compatibility.records.end(),
        [](const pvt::PreservedConfigRecord& record) {
            return record.key == "future.output.sparkle"
                   && record.value == "maximum";
        }));
    loaded.project.layers[0].name = "Brighter base";
    loaded.project.canvas.clock.reverse = true;
    loaded.project.canvas.clock.meter.expression = "5+3/8";
    loaded.project.canvas.clock.music.analyzer_version = "analysis % pass";
    loaded.project.output.filename_prefix = "ember % glow_";
    loaded.project.layers[0].render.waves[0].name = "Warm % core";
    loaded.project.layers[0].render.palette.name = "Night % Sky";
    loaded.project.layers[0].render.palette.enabled = false;
    loaded.project.layers[0].render.palette.colors[0].green = 0.123456789;
    loaded.project.layers[0].render.swings_enabled = true;
    CHECK(pvt::save_project_document(loaded, as_utf8(bundle), &report, &error));
    CHECK(report.created_version && report.version == 1U);

    std::vector<pvt::BundleDiffEntry> readable_differences;
    CHECK(pvt::diff_project_versions(loaded, 0U, 1U,
                                     readable_differences, &error));
    CHECK(std::any_of(readable_differences.begin(), readable_differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field == "global.output.filename_prefix"
                                 && value.after == "ember % glow_";
                      }));
    CHECK(std::any_of(readable_differences.begin(), readable_differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field == "global.timing.clock.reverse"
                                 && value.before == "0" && value.after == "1";
                      }));
    CHECK(std::any_of(readable_differences.begin(), readable_differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field
                                     == "global.timing.clock.meter.expression"
                                 && value.after == "5+3/8";
                      }));
    CHECK(std::any_of(readable_differences.begin(), readable_differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field
                                     == "global.timing.music.analyzer_version"
                                 && value.after == "analysis % pass";
                      }));
    CHECK(std::any_of(readable_differences.begin(), readable_differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field.find("render.rhythm.swings_enabled")
                                     != std::string::npos
                                 && value.before == "0" && value.after == "1";
                      }));
    CHECK(std::any_of(readable_differences.begin(), readable_differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field.find("render.waves.0.name")
                                     != std::string::npos
                                 && value.after == "Warm % core";
                      }));
    CHECK(std::any_of(readable_differences.begin(), readable_differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field.find("render.palette.name")
                                     != std::string::npos
                                 && value.after == "Night % Sky";
                      }));
    CHECK(std::any_of(readable_differences.begin(), readable_differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field.find("render.palette.enabled")
                                     != std::string::npos
                                 && value.before == "1" && value.after == "0";
                      }));
    CHECK(std::any_of(readable_differences.begin(), readable_differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field.find("render.palette.colors.0.green")
                                     != std::string::npos
                                 && value.after == "0.123456789";
                      }));

    pvt::ProjectDocument reloaded_palette_off;
    CHECK(pvt::load_project_document(
        as_utf8(bundle), reloaded_palette_off, &error));
    const auto& reloaded_palette =
        reloaded_palette_off.project.layers.front().render.palette;
    CHECK(!reloaded_palette.enabled);
    CHECK(reloaded_palette.name == "Night % Sky");
    CHECK(!reloaded_palette.colors.empty());
    if (!reloaded_palette.colors.empty()) {
        CHECK(reloaded_palette.colors.front().green == 0.123456789);
    }

    loaded.project.name = "Renamed: Flame";
    CHECK(pvt::save_project_document(loaded, as_utf8(bundle), &report, &error));
    CHECK(report.version == 2U);
    CHECK(as_utf8(bundle.filename()) == portable_root("Fire: Night"));
    std::vector<pvt::BundleDiffEntry> differences;
    CHECK(pvt::diff_project_versions(loaded, 1U, 2U, differences, &error));
    CHECK(std::any_of(differences.begin(), differences.end(),
                      [](const pvt::BundleDiffEntry& value) {
                          return value.field == "project.name";
                      }));
    pvt::ProjectConfig old_project;
    CHECK(pvt::load_project_version(loaded, 0U, old_project, &error));
    CHECK(old_project.name == "Fire: Night");

    CHECK(pvt::make_project_version_current(loaded, 0U, &report, &error));
    CHECK(loaded.current_version == 0U);
    CHECK(loaded.project.name == "Fire: Night");
    CHECK(pvt::revert_project_as_new(loaded, 0U, &report, &error));
    CHECK(report.created_version && report.version == 3U);
    CHECK(loaded.current_version == 3U);

    CHECK(pvt::save_project_document(loaded, as_utf8(bundle), &report, &error));
    CHECK(report.validated_only && !report.created_version);
    CHECK(pvt::validate_project_bundle(as_utf8(bundle), nullptr, &error));

    const fs::path copied = directory / "Copied Bundle";
    std::error_code copy_error;
    fs::copy(bundle, copied, fs::copy_options::recursive, copy_error);
    CHECK(!copy_error);
    loaded.project.name = "Copied Display Name";
    CHECK(pvt::save_project_document(loaded, as_utf8(copied), &report, &error));
    CHECK(report.created_version && report.version == 4U);
    pvt::ProjectDocument copied_document;
    CHECK(pvt::load_project_document(as_utf8(copied), copied_document, &error));
    CHECK(copied_document.project.name == "Copied Display Name");
}

void test_independent_current_state_copy(const fs::path& directory) {
    pvt::ProjectDocument source = pvt::default_project_document();
    source.project.name = "Original History";
    pvt::LayerConfig upper = pvt::default_layer(1U);
    upper.name = "Upper layer";
    upper.file_id = 7U;
    source.project.layers.push_back(std::move(upper));
    pvt::LayerGroup live_group;
    live_group.uuid = pvt::generate_uuid();
    live_group.name = "Live copy group";
    source.project.groups.push_back(live_group);
    source.project.layers.back().group_uuid = live_group.uuid;

    const std::string live_audio_uuid = pvt::generate_uuid();
    const std::string live_midi_uuid = pvt::generate_uuid();
    pvt::LiveConfig& live = source.project.canvas.live;
    live.enabled = true;
    live.endpoints = {
        {live_audio_uuid, "Copy microphone",
         pvt::LiveEndpointProtocol::Audio,
         pvt::LiveEndpointDirection::Input, 0, 0},
        {live_midi_uuid, "Copy MIDI output",
         pvt::LiveEndpointProtocol::Midi,
         pvt::LiveEndpointDirection::Output, 0, 0},
    };
    pvt::LiveClockInputConfig live_input;
    live_input.enabled = true;
    live_input.target = pvt::LiveClockTarget::Layer;
    live_input.layer_uuid = source.project.layers.back().uuid;
    live_input.source = pvt::LiveClockInputSource::AudioStream;
    live_input.endpoint_uuid = live_audio_uuid;
    live_input.follow_midi_transport = false;
    live.clock_inputs.push_back(live_input);
    pvt::LiveMidiClockOutputConfig live_output;
    live_output.enabled = true;
    live_output.source = pvt::LiveClockTarget::Layer;
    live_output.layer_uuid = source.project.layers.back().uuid;
    live_output.endpoint_uuid = live_midi_uuid;
    live.midi_clock_outputs.push_back(live_output);
    pvt::LiveControlMapping layer_mapping;
    layer_mapping.enabled = false;
    layer_mapping.name = "Copied particle size";
    layer_mapping.target_path = "layer/"
        + source.project.layers.back().uuid
        + "/effect/42/particle_size";
    live.mappings.push_back(layer_mapping);
    pvt::LiveControlMapping group_mapping = layer_mapping;
    group_mapping.name = "Copied group visibility";
    group_mapping.target_path = "group/" + live_group.uuid + "/enabled";
    live.mappings.push_back(group_mapping);
    pvt::LiveSceneConfig live_scene;
    live_scene.uuid = pvt::generate_uuid();
    live_scene.name = "Copied scene";
    live_scene.values = {
        {"layer/" + source.project.layers.back().uuid + "/opacity",
         pvt::LiveSceneValueType::Real, "0.75"},
        {"group/" + live_group.uuid + "/enabled",
         pvt::LiveSceneValueType::Boolean, "1"},
    };
    live.scenes.push_back(live_scene);
    live.startup_scene_uuid = live_scene.uuid;
    const fs::path source_path = directory / "independent-source.zip";
    pvt::BundleSaveReport report;
    std::string error;
    CHECK(pvt::save_project_document(source, as_utf8(source_path), &report, &error));
    source.project.output.filename_prefix = "second-version_";
    CHECK(pvt::save_project_document(source, as_utf8(source_path), &report, &error));
    CHECK(source.versions.size() == 2U);

    const std::string source_project_uuid = source.project.uuid;
    std::vector<std::string> source_layer_uuids;
    for (const pvt::LayerConfig& layer : source.project.layers) {
        source_layer_uuids.push_back(layer.uuid);
    }
    pvt::ProjectConfig renamed_snapshot = source.project;
    renamed_snapshot.name = "Independent Current State";

    pvt::ProjectDocument independent;
    CHECK(pvt::make_independent_project_copy(
        renamed_snapshot, independent, &error));
    CHECK(independent.project.name == renamed_snapshot.name);
    CHECK(independent.project.uuid != source_project_uuid);
    CHECK(independent.project.layers.size() == source.project.layers.size());
    CHECK(independent.source_path.empty());
    CHECK(independent.imported_from_path.empty());
    CHECK(independent.loaded_snapshot_digest.empty());
    CHECK(independent.loaded_bundle_state_digest.empty());
    CHECK(independent.versions.empty());
    CHECK(independent.dirty && !independent.legacy_import);
    CHECK(independent.project.output.filename_prefix == "second-version_");
    for (std::size_t index = 0U;
         index < independent.project.layers.size(); ++index) {
        CHECK(independent.project.layers[index].uuid
              != source_layer_uuids[index]);
        CHECK(std::find(source_layer_uuids.begin(), source_layer_uuids.end(),
                        independent.project.layers[index].uuid)
              == source_layer_uuids.end());
        CHECK(independent.project.layers[index].file_id
              == static_cast<std::uint64_t>(index));
        CHECK(independent.project.layers[index].name
              == renamed_snapshot.layers[index].name);
    }
    CHECK(independent.project.groups.size() == 1U);
    if (independent.project.groups.size() == 1U
        && independent.project.layers.size() == 2U) {
        const std::string& copied_layer_uuid =
            independent.project.layers.back().uuid;
        const std::string& copied_group_uuid =
            independent.project.groups.front().uuid;
        const pvt::LiveConfig& copied_live = independent.project.canvas.live;
        CHECK(copied_group_uuid != live_group.uuid);
        CHECK(independent.project.layers.back().group_uuid
              == copied_group_uuid);
        CHECK(copied_live.clock_inputs.size() == 1U);
        CHECK(copied_live.midi_clock_outputs.size() == 1U);
        CHECK(copied_live.mappings.size() == 2U);
        CHECK(copied_live.scenes.size() == 1U);
        if (copied_live.clock_inputs.size() == 1U) {
            CHECK(copied_live.clock_inputs.front().layer_uuid
                  == copied_layer_uuid);
        }
        if (copied_live.midi_clock_outputs.size() == 1U) {
            CHECK(copied_live.midi_clock_outputs.front().layer_uuid
                  == copied_layer_uuid);
        }
        if (copied_live.mappings.size() == 2U) {
            CHECK(copied_live.mappings[0].target_path
                  == "layer/" + copied_layer_uuid
                         + "/effect/42/particle_size");
            CHECK(copied_live.mappings[1].target_path
                  == "group/" + copied_group_uuid + "/enabled");
        }
        if (copied_live.scenes.size() == 1U
            && copied_live.scenes.front().values.size() == 2U) {
            CHECK(copied_live.scenes.front().values[0].target_path
                  == "layer/" + copied_layer_uuid + "/opacity");
            CHECK(copied_live.scenes.front().values[1].target_path
                  == "group/" + copied_group_uuid + "/enabled");
        }
    }
    CHECK(pvt::validate(independent.project).ok);

    const fs::path independent_path = directory / "independent-copy.zip";
    CHECK(pvt::save_project_document(
        independent, as_utf8(independent_path), &report, &error));
    CHECK(report.created_version && report.version == 0U);
    CHECK(independent.versions.size() == 1U);
    CHECK(independent.source_path == as_utf8(independent_path));
    CHECK(source.source_path == as_utf8(source_path));
    CHECK(source.versions.size() == 2U);

    pvt::ProjectDocument reloaded;
    CHECK(pvt::load_project_document(as_utf8(independent_path), reloaded, &error));
    CHECK(reloaded.project.uuid == independent.project.uuid);
    CHECK(reloaded.project.uuid != source.project.uuid);
    CHECK(reloaded.project.name == "Independent Current State");
    CHECK(reloaded.versions.size() == 1U);

    // A second independent copy has no inherited destination identity or
    // compare-and-swap token, so it cannot replace the first copy.
    pvt::ProjectDocument collision;
    CHECK(pvt::make_independent_project_copy(
        renamed_snapshot, collision, &error));
    const std::string collision_uuid = collision.project.uuid;
    CHECK(!pvt::save_project_document(
        collision, as_utf8(independent_path), &report, &error));
    CHECK(collision.project.uuid == collision_uuid);
    CHECK(collision.source_path.empty());
    CHECK(!error.empty());
    pvt::ProjectDocument after_collision;
    CHECK(pvt::load_project_document(
        as_utf8(independent_path), after_collision, &error));
    CHECK(after_collision.project.uuid == independent.project.uuid);
    CHECK(after_collision.versions.size() == 1U);

    // The ProjectConfig-only overload cannot retain embedded attachment bytes.
    // Starting images must be rejected just like music, custom OBJ, and Plane
    // height-map sources; callers with attachments must use the
    // ProjectDocument overload.
    pvt::ProjectConfig attachment_bearing = renamed_snapshot;
    attachment_bearing.layers.front().render.starting_image.basename =
        "source.png";
    attachment_bearing.layers.front().render.starting_image.sha256 =
        std::string(64U, 'a');
    pvt::ProjectDocument attachment_untouched =
        pvt::default_project_document();
    const std::string attachment_untouched_uuid =
        attachment_untouched.project.uuid;
    CHECK(!pvt::make_independent_project_copy(
        attachment_bearing, attachment_untouched, &error));
    CHECK(error.find("Attachment-bearing snapshots") != std::string::npos);
    CHECK(attachment_untouched.project.uuid == attachment_untouched_uuid);

    pvt::ProjectConfig height_bearing = renamed_snapshot;
    height_bearing.layers.front().render.surface.plane_displacement.sha256 =
        std::string(64U, 'b');
    height_bearing.layers.front().render.surface.plane_displacement.basename =
        "height.png";
    CHECK(!pvt::make_independent_project_copy(
        height_bearing, attachment_untouched, &error));
    CHECK(error.find("Attachment-bearing snapshots") != std::string::npos);
    CHECK(attachment_untouched.project.uuid == attachment_untouched_uuid);

    pvt::ProjectConfig invalid = renamed_snapshot;
    invalid.uuid.clear();
    pvt::ProjectDocument untouched = pvt::default_project_document();
    const std::string untouched_uuid = untouched.project.uuid;
    CHECK(!pvt::make_independent_project_copy(invalid, untouched, &error));
    CHECK(untouched.project.uuid == untouched_uuid);
}

void test_zip_unicode_and_legacy(const fs::path& directory) {
    pvt::ProjectDocument document = pvt::default_project_document();
    document.project.name = "Flame \xCE\xB3";
    document.project.output.write_alpha = true;
    pvt::LayerConfig second = pvt::default_layer(1U);
    second.name = "Glow \xE2\x9C\xA8";
    second.blend_mode = pvt::BlendMode::Add;
    second.alpha_mode = pvt::AlphaMode::AlphaUnder;
    pvt::LayerGroup group;
    group.uuid = pvt::generate_uuid();
    group.name = "Glow folder";
    group.locked = true;
    second.group_uuid = group.uuid;
    document.project.groups.push_back(group);
    document.project.layers.push_back(std::move(second));
    const fs::path zip = directory / pvt::detail::path_from_utf8(
                                         pvt::portable_project_filename(
                                             document.project.name));
    std::string error;
    pvt::BundleSaveReport report;
    CHECK(pvt::save_project_document(document, as_utf8(zip), &report, &error));
    CHECK(report.wrote_zip);
    pvt::ProjectDocument loaded;
    CHECK(pvt::load_project_document(as_utf8(zip), loaded, &error));
    CHECK(loaded.project.name == document.project.name);
    CHECK(loaded.project.layers.size() == 2U);
    CHECK(loaded.project.layers[1].name == "Glow \xE2\x9C\xA8");
    CHECK(loaded.project.layers[1].blend_mode == pvt::BlendMode::Add);
    CHECK(loaded.project.layers[1].alpha_mode == pvt::AlphaMode::AlphaUnder);
    CHECK(loaded.project.groups.size() == 1U);
    CHECK(loaded.project.groups.front().name == "Glow folder");
    CHECK(loaded.project.groups.front().locked);
    CHECK(loaded.project.layers[1].group_uuid
          == loaded.project.groups.front().uuid);

    pvt::ProjectDocument independent;
    CHECK(pvt::make_independent_project_copy(loaded, independent, &error));
    CHECK(independent.project.groups.size() == 1U);
    CHECK(independent.project.groups.front().uuid
          != loaded.project.groups.front().uuid);
    CHECK(independent.project.layers[1].group_uuid
          == independent.project.groups.front().uuid);
    CHECK(pvt::validate(independent.project).ok);

    CHECK(pvt::portable_project_filename("CON.txt").rfind("_CON.txt", 0U) == 0U);
    CHECK(pvt::portable_project_filename("Fire. ") == "Fire.zip");
    CHECK(pvt::portable_project_filename("Fire: Night") == "Fire_ Night.zip");
    std::string long_unicode;
    for (std::size_t index = 0U; index < 128U; ++index) {
        long_unicode += "\xC3\xA9";
    }
    const std::string portable = pvt::portable_project_filename(long_unicode);
    CHECK(portable.size() <= 244U);
    CHECK(pvt::detail::valid_utf8(portable.substr(0U, portable.size() - 4U)));
    pvt::ProjectDocument long_name_document = pvt::default_project_document();
    long_name_document.project.name = long_unicode;
    const fs::path long_bundle = directory
                                 / pvt::detail::path_from_utf8(
                                     portable.substr(0U, portable.size() - 4U));
    CHECK(pvt::save_project_document(long_name_document, as_utf8(long_bundle),
                                     &report, &error));
    long_name_document.project.layers[0].name = "Second save";
    CHECK(pvt::save_project_document(long_name_document, as_utf8(long_bundle),
                                     &report, &error));
    CHECK(report.created_version && report.version == 1U);

    pvt::RenderConfig legacy = pvt::default_config();
    legacy.width = 64;
    legacy.height = 64;
    const fs::path legacy_path = directory / "LEGACY.PVT";
    CHECK(pvt::save_setup(legacy, as_utf8(legacy_path), &error));
    const std::string original = read_bytes(legacy_path);
    pvt::ProjectDocument imported;
    CHECK(pvt::load_project_document(as_utf8(legacy_path), imported, &error));
    CHECK(imported.legacy_import && imported.source_path.empty());
    const fs::path promoted = directory / "legacy-import.zip";
    CHECK(pvt::save_project_document(imported, as_utf8(promoted), &report, &error));
    CHECK(report.created_version && report.version == 0U);
    CHECK(read_bytes(legacy_path) == original);
}

void test_external_change_lifecycle(const fs::path& directory) {
    pvt::ProjectDocument document = pvt::default_project_document();
    document.project.name = "External Change";
    const fs::path bundle = directory / portable_root(document.project.name);
    std::string error;
    pvt::BundleSaveReport report;
    CHECK(pvt::save_project_document(document, as_utf8(bundle), &report, &error));
    const fs::path raw_layer = bundle / "0" / "0.pvt";
    std::string edited = read_bytes(raw_layer);
    CHECK(replace_once(edited, "appearance.hue_cycles\t2\n",
                       "appearance.hue_cycles\t3\n"));
    CHECK(write_bytes(raw_layer, edited));

    pvt::ProjectDocument external;
    CHECK(pvt::load_project_document(as_utf8(bundle), external, &error));
    CHECK(external.externally_modified && external.dirty);
    const pvt::BundleVersionInfo* zero = version_info(external, 0U);
    CHECK(zero != nullptr && zero->valid && zero->externally_modified
          && zero->changed_since_recorded);
    CHECK(pvt::save_project_document(external, as_utf8(bundle), &report, &error));
    CHECK(report.promoted_external_change && report.version == 1U);
    CHECK(read_bytes(raw_layer) == edited);
    zero = version_info(external, 0U);
    CHECK(zero != nullptr && zero->externally_modified
          && !zero->changed_since_recorded);
    CHECK(pvt::save_project_document(external, as_utf8(bundle), &report, &error));
    CHECK(report.validated_only);
    CHECK(pvt::validate_project_bundle(as_utf8(bundle), nullptr, &error));

    std::string edited_again = read_bytes(raw_layer);
    CHECK(replace_once(edited_again, "appearance.hue_cycles\t3\n",
                       "appearance.hue_cycles\t4\n"));
    CHECK(write_bytes(raw_layer, edited_again));
    pvt::ProjectDocument changed_again;
    CHECK(pvt::load_project_document(as_utf8(bundle), changed_again, &error));
    zero = version_info(changed_again, 0U);
    CHECK(changed_again.externally_modified && zero != nullptr
          && zero->changed_since_recorded);
    CHECK(pvt::save_project_document(changed_again, as_utf8(bundle), &report, &error));
    CHECK(report.promoted_external_change && report.version == 2U);
    CHECK(pvt::validate_project_bundle(as_utf8(bundle), nullptr, &error));

    // Editing an old manifest after it already has children preserves both its
    // recorded digest as a lineage alias and its exact newly observed raw tree.
    const fs::path old_metadata = bundle / "0" / "metadata.txt";
    std::string changed_manifest = read_bytes(old_metadata);
    CHECK(replace_once(changed_manifest, "version.reason\tsave\n",
                       "version.reason\texternal_origin\n"));
    CHECK(write_bytes(old_metadata, changed_manifest));
    pvt::ProjectDocument manifest_change;
    CHECK(pvt::load_project_document(as_utf8(bundle), manifest_change, &error));
    zero = version_info(manifest_change, 0U);
    CHECK(manifest_change.externally_modified && zero != nullptr
          && zero->changed_since_recorded);
    CHECK(pvt::save_project_document(manifest_change, as_utf8(bundle),
                                     &report, &error));
    CHECK(report.promoted_external_change && report.version == 3U);
    CHECK(pvt::validate_project_bundle(as_utf8(bundle), nullptr, &error));
}

void test_fallback_orphan_and_stale(const fs::path& directory) {
    std::string error;
    pvt::BundleSaveReport report;
    pvt::ProjectDocument document = pvt::default_project_document();
    document.project.name = "Orphan Recovery";
    const fs::path bundle = directory / portable_root(document.project.name);
    CHECK(pvt::save_project_document(document, as_utf8(bundle), &report, &error));
    const std::string root0 = read_bytes(bundle / "metadata.txt");
    const std::string checksum0 = read_bytes(bundle / "metadata.sha256");
    document.project.layers[0].name = "Version one";
    CHECK(pvt::save_project_document(document, as_utf8(bundle), &report, &error));
    CHECK(report.version == 1U);
    CHECK(write_bytes(bundle / "metadata.txt", root0));
    CHECK(write_bytes(bundle / "metadata.sha256", checksum0));
    CHECK(write_bytes(bundle / "current", "broken current\n"));

    pvt::ProjectDocument recovered;
    CHECK(pvt::load_project_document(as_utf8(bundle), recovered, &error));
    CHECK(recovered.current_version == 1U && recovered.externally_modified);
    const pvt::BundleVersionInfo* orphan = version_info(recovered, 1U);
    CHECK(orphan != nullptr && orphan->valid && !orphan->indexed);
    CHECK(pvt::save_project_document(recovered, as_utf8(bundle), &report, &error));
    CHECK(report.version == 2U && report.promoted_external_change);
    orphan = version_info(recovered, 1U);
    CHECK(orphan != nullptr && orphan->indexed);
    CHECK(pvt::validate_project_bundle(as_utf8(bundle), nullptr, &error));

    pvt::ProjectDocument first;
    pvt::ProjectDocument second;
    CHECK(pvt::load_project_document(as_utf8(bundle), first, &error));
    CHECK(pvt::load_project_document(as_utf8(bundle), second, &error));
    second.project.layers[0].name = "Second writer";
    CHECK(pvt::save_project_document(second, as_utf8(bundle), &report, &error));
    const std::string first_last_saved = first.last_saved_utc;
    const std::uint64_t first_version = first.current_version;
    first.project.layers[0].name = "Stale writer";
    pvt::BundleSaveReport untouched;
    untouched.path = "sentinel";
    CHECK(!pvt::save_project_document(first, as_utf8(bundle), &untouched, &error));
    CHECK(first.last_saved_utc == first_last_saved
          && first.current_version == first_version);
    CHECK(untouched.path == "sentinel");

    pvt::ProjectDocument fallback = pvt::default_project_document();
    fallback.project.name = "Malformed Current";
    const fs::path fallback_bundle = directory / portable_root(fallback.project.name);
    CHECK(pvt::save_project_document(fallback, as_utf8(fallback_bundle),
                                     &report, &error));
    fallback.project.layers[0].name = "Bad head";
    CHECK(pvt::save_project_document(fallback, as_utf8(fallback_bundle),
                                     &report, &error));
    CHECK(write_bytes(fallback_bundle / "1" / "metadata.txt", "malformed\n"));
    pvt::ProjectDocument fallback_loaded;
    CHECK(pvt::load_project_document(as_utf8(fallback_bundle), fallback_loaded,
                                     &error));
    CHECK(fallback_loaded.current_version == 0U
          && fallback_loaded.externally_modified);
    CHECK(pvt::save_project_document(fallback_loaded, as_utf8(fallback_bundle),
                                     &report, &error));
    CHECK(report.created_version && report.version == 2U);
    const pvt::BundleVersionInfo* bad = version_info(fallback_loaded, 1U);
    CHECK(bad != nullptr && !bad->valid && !bad->indexed);
    CHECK(pvt::save_project_document(fallback_loaded, as_utf8(fallback_bundle),
                                     &report, &error));
    CHECK(report.validated_only);
    CHECK(pvt::validate_project_bundle(as_utf8(fallback_bundle), nullptr, &error));
}

void test_complete_history_accounting(const fs::path& directory) {
    std::string error;
    pvt::BundleSaveReport report;

    // A valid crash-orphan that is unrelated to the intact current pointer is
    // surfaced on load, then retained as byte-exact noncanonical history when
    // Save appends the explicit external-change version.
    pvt::ProjectDocument orphan_document = pvt::default_project_document();
    orphan_document.project.name = "Unrelated Orphan";
    const fs::path orphan_bundle =
        directory / portable_root(orphan_document.project.name);
    CHECK(pvt::save_project_document(orphan_document, as_utf8(orphan_bundle),
                                     &report, &error));
    const std::string root_zero = read_bytes(orphan_bundle / "metadata.txt");
    const std::string checksum_zero =
        read_bytes(orphan_bundle / "metadata.sha256");
    const std::string current_zero = read_bytes(orphan_bundle / "current");
    orphan_document.project.layers[0].name = "Unindexed but valid";
    CHECK(pvt::save_project_document(orphan_document, as_utf8(orphan_bundle),
                                     &report, &error));
    CHECK(write_bytes(orphan_bundle / "metadata.txt", root_zero));
    CHECK(write_bytes(orphan_bundle / "metadata.sha256", checksum_zero));
    CHECK(write_bytes(orphan_bundle / "current", current_zero));
    CHECK(!pvt::validate_project_bundle(as_utf8(orphan_bundle), nullptr, &error));

    pvt::ProjectDocument recovered_orphan;
    CHECK(pvt::load_project_document(as_utf8(orphan_bundle), recovered_orphan,
                                     &error));
    CHECK(recovered_orphan.current_version == 0U
          && recovered_orphan.externally_modified && recovered_orphan.dirty);
    const pvt::BundleVersionInfo* orphan = version_info(recovered_orphan, 1U);
    CHECK(orphan != nullptr && orphan->valid && !orphan->indexed
          && orphan->changed_since_recorded);
    CHECK(pvt::save_project_document(recovered_orphan, as_utf8(orphan_bundle),
                                     &report, &error));
    CHECK(report.promoted_external_change && report.version == 2U);
    orphan = version_info(recovered_orphan, 1U);
    CHECK(orphan != nullptr && orphan->valid && !orphan->indexed
          && !orphan->changed_since_recorded);
    CHECK(pvt::validate_project_bundle(as_utf8(orphan_bundle), nullptr, &error));
    CHECK(pvt::save_project_document(recovered_orphan, as_utf8(orphan_bundle),
                                     &report, &error));
    CHECK(report.validated_only && !report.created_version);

    // A malformed unrelated numeric directory follows the same lifecycle. It
    // remains recoverable raw data, but its exact tree is explicitly recorded
    // rather than disappearing from validation.
    pvt::ProjectDocument malformed_document = pvt::default_project_document();
    malformed_document.project.name = "Unrelated Malformed";
    const fs::path malformed_bundle =
        directory / portable_root(malformed_document.project.name);
    CHECK(pvt::save_project_document(malformed_document,
                                     as_utf8(malformed_bundle), &report, &error));
    std::error_code directory_error;
    CHECK(fs::create_directory(malformed_bundle / "7", directory_error));
    CHECK(!directory_error);
    CHECK(write_bytes(malformed_bundle / "7" / "metadata.txt", "malformed\n"));
    CHECK(!pvt::validate_project_bundle(as_utf8(malformed_bundle), nullptr,
                                        &error));
    pvt::ProjectDocument recovered_malformed;
    CHECK(pvt::load_project_document(as_utf8(malformed_bundle),
                                     recovered_malformed, &error));
    const pvt::BundleVersionInfo* malformed =
        version_info(recovered_malformed, 7U);
    CHECK(recovered_malformed.externally_modified && recovered_malformed.dirty);
    CHECK(malformed != nullptr && !malformed->valid && !malformed->indexed
          && malformed->changed_since_recorded);
    CHECK(pvt::save_project_document(recovered_malformed,
                                     as_utf8(malformed_bundle), &report, &error));
    CHECK(report.promoted_external_change && report.version == 8U);
    malformed = version_info(recovered_malformed, 7U);
    CHECK(malformed != nullptr && !malformed->valid && !malformed->indexed
          && !malformed->changed_since_recorded);
    CHECK(pvt::validate_project_bundle(as_utf8(malformed_bundle), nullptr,
                                       &error));
    CHECK(pvt::save_project_document(recovered_malformed,
                                     as_utf8(malformed_bundle), &report, &error));
    CHECK(report.validated_only && !report.created_version);

    // Deleting a previously recorded preserved directory is also surfaced as
    // an external history change. Repair Save records the deletion as a new
    // version instead of allowing load to appear clean and fail only later.
    std::error_code remove_error;
    CHECK(fs::remove_all(malformed_bundle / "7", remove_error) > 0U);
    CHECK(!remove_error);
    pvt::ProjectDocument deleted_preserved;
    CHECK(pvt::load_project_document(as_utf8(malformed_bundle),
                                     deleted_preserved, &error));
    CHECK(deleted_preserved.current_version == 8U
          && deleted_preserved.externally_modified && deleted_preserved.dirty);
    CHECK(!pvt::validate_project_bundle(as_utf8(malformed_bundle), nullptr,
                                        &error));
    CHECK(pvt::save_project_document(deleted_preserved,
                                     as_utf8(malformed_bundle), &report, &error));
    CHECK(report.promoted_external_change && report.version == 9U);
    CHECK(pvt::validate_project_bundle(as_utf8(malformed_bundle), nullptr,
                                       &error));

    // Moving a corrupted indexed ancestor into preserved history retains its
    // former metadata digest as a lineage alias for valid descendants.
    pvt::ProjectDocument lineage_document = pvt::default_project_document();
    lineage_document.project.name = "Preserved Lineage";
    const fs::path lineage_bundle =
        directory / portable_root(lineage_document.project.name);
    CHECK(pvt::save_project_document(lineage_document, as_utf8(lineage_bundle),
                                     &report, &error));
    lineage_document.project.layers[0].name = "Valid child";
    CHECK(pvt::save_project_document(lineage_document, as_utf8(lineage_bundle),
                                     &report, &error));
    CHECK(report.version == 1U);
    CHECK(write_bytes(lineage_bundle / "0" / "metadata.txt", "corrupt\n"));
    pvt::ProjectDocument repaired_lineage;
    CHECK(pvt::load_project_document(as_utf8(lineage_bundle), repaired_lineage,
                                     &error));
    CHECK(repaired_lineage.current_version == 1U
          && repaired_lineage.externally_modified);
    repaired_lineage.project.layers[0].name = "Valid grandchild";
    CHECK(pvt::save_project_document(repaired_lineage, as_utf8(lineage_bundle),
                                     &report, &error));
    CHECK(report.created_version && report.version == 2U);
    CHECK(pvt::validate_project_bundle(as_utf8(lineage_bundle), nullptr, &error));
    const pvt::BundleVersionInfo* ancestor =
        version_info(repaired_lineage, 0U);
    CHECK(ancestor != nullptr && !ancestor->valid && !ancestor->indexed
          && !ancestor->changed_since_recorded);

    // A deleted indexed ancestor must not prevent a valid current child from
    // loading. Repair retains the vanished digest as a lineage alias and
    // appends a new snapshot without inventing replacement raw data.
    pvt::ProjectDocument missing_indexed = pvt::default_project_document();
    missing_indexed.project.name = "Missing Indexed Ancestor";
    const fs::path missing_indexed_bundle =
        directory / portable_root(missing_indexed.project.name);
    CHECK(pvt::save_project_document(missing_indexed,
                                     as_utf8(missing_indexed_bundle),
                                     &report, &error));
    missing_indexed.project.layers[0].name = "Surviving child";
    CHECK(pvt::save_project_document(missing_indexed,
                                     as_utf8(missing_indexed_bundle),
                                     &report, &error));
    remove_error.clear();
    CHECK(fs::remove_all(missing_indexed_bundle / "0", remove_error) > 0U);
    CHECK(!remove_error);
    pvt::ProjectDocument missing_indexed_loaded;
    CHECK(pvt::load_project_document(as_utf8(missing_indexed_bundle),
                                     missing_indexed_loaded, &error));
    CHECK(missing_indexed_loaded.current_version == 1U
          && missing_indexed_loaded.externally_modified
          && missing_indexed_loaded.dirty);
    ancestor = version_info(missing_indexed_loaded, 0U);
    CHECK(ancestor != nullptr && !ancestor->valid && ancestor->indexed
          && ancestor->changed_since_recorded);
    CHECK(pvt::save_project_document(missing_indexed_loaded,
                                     as_utf8(missing_indexed_bundle),
                                     &report, &error));
    CHECK(report.created_version && report.version == 2U);
    CHECK(pvt::validate_project_bundle(as_utf8(missing_indexed_bundle),
                                       nullptr, &error));
}

void test_corrupt_history_and_root_metadata(const fs::path& directory) {
    std::string error;
    pvt::BundleSaveReport report;
    pvt::ProjectDocument document = pvt::default_project_document();
    document.project.name = "Corrupt History";
    const fs::path bundle = directory / portable_root(document.project.name);
    CHECK(pvt::save_project_document(document, as_utf8(bundle), &report, &error));
    document.project.layers[0].name = "Current valid";
    CHECK(pvt::save_project_document(document, as_utf8(bundle), &report, &error));
    CHECK(write_bytes(bundle / "0" / "metadata.txt", "not metadata\n"));
    pvt::ProjectDocument loaded;
    CHECK(pvt::load_project_document(as_utf8(bundle), loaded, &error));
    CHECK(loaded.current_version == 1U);
    const pvt::BundleVersionInfo* invalid = version_info(loaded, 0U);
    CHECK(invalid != nullptr && !invalid->valid);
    CHECK(!pvt::validate_project_bundle(as_utf8(bundle), nullptr, &error));

    pvt::ProjectDocument checksum_document = pvt::default_project_document();
    checksum_document.project.name = "Root Checksum";
    const fs::path checksum_bundle = directory
                                     / portable_root(checksum_document.project.name);
    CHECK(pvt::save_project_document(checksum_document, as_utf8(checksum_bundle),
                                     &report, &error));
    std::error_code remove_error;
    CHECK(fs::remove(checksum_bundle / "metadata.sha256", remove_error));
    CHECK(!remove_error);
    CHECK(pvt::load_project_document(as_utf8(checksum_bundle), checksum_document,
                                     &error));
    CHECK(checksum_document.externally_modified);
    CHECK(pvt::save_project_document(checksum_document, as_utf8(checksum_bundle),
                                     &report, &error));
    CHECK(report.promoted_external_change);
    CHECK(pvt::validate_project_bundle(as_utf8(checksum_bundle), nullptr, &error));

    std::string root = read_bytes(checksum_bundle / "metadata.txt");
    CHECK(replace_once(root,
                       "project.last_changed_with_version\t"
                           + checksum_document.last_changed_with_version + "\n",
                       "project.last_changed_with_version\t99.0.0\n"));
    CHECK(write_bytes(checksum_bundle / "metadata.txt", root));
    CHECK(rewrite_root_checksum(checksum_bundle));
    pvt::ProjectDocument newer;
    CHECK(pvt::load_project_document(as_utf8(checksum_bundle), newer, &error));
    CHECK(newer.newer_program_version);

    // A future root header and unknown records are not, by themselves,
    // evidence of damage. Parse the known structural fields and preserve the
    // rest when the mutable root file is rewritten.
    CHECK(replace_once(root, "PVT_BUNDLE\t1\n", "PVT_BUNDLE\t999\n"));
    root.append("future.root.sparkle\tmaximum\n");
    CHECK(write_bytes(checksum_bundle / "metadata.txt", root));
    CHECK(rewrite_root_checksum(checksum_bundle));
    pvt::ProjectDocument future_root;
    CHECK(pvt::load_project_document(as_utf8(checksum_bundle), future_root,
                                     &error));
    future_root.project.layers[0].name = "Future root round trip";
    CHECK(pvt::save_project_document(future_root, as_utf8(checksum_bundle),
                                     &report, &error));
    root = read_bytes(checksum_bundle / "metadata.txt");
    CHECK(root.rfind("PVT_BUNDLE\t999\n", 0U) == 0U);
    CHECK(root.find("future.root.sparkle\tmaximum\n")
          != std::string::npos);

    pvt::ProjectDocument future_manifest = pvt::default_project_document();
    future_manifest.project.name = "Future Version Metadata";
    const fs::path future_manifest_bundle =
        directory / portable_root(future_manifest.project.name);
    CHECK(pvt::save_project_document(
        future_manifest, as_utf8(future_manifest_bundle), &report, &error));
    std::string future_version_bytes =
        read_bytes(future_manifest_bundle / "0" / "metadata.txt");
    CHECK(replace_once(future_version_bytes, "PVT_VERSION\t5\n",
                       "PVT_VERSION\t999\n"));
    future_version_bytes.append("future.version.sparkle\tmaximum\n");
    CHECK(write_bytes(future_manifest_bundle / "0" / "metadata.txt",
                      future_version_bytes));
    pvt::ProjectDocument future_manifest_loaded;
    CHECK(pvt::load_project_document(as_utf8(future_manifest_bundle),
                                     future_manifest_loaded, &error));
    CHECK(future_manifest_loaded.externally_modified);
    future_manifest_loaded.project.layers[0].name = "Promoted safely";
    CHECK(pvt::save_project_document(future_manifest_loaded,
                                     as_utf8(future_manifest_bundle),
                                     &report, &error));
    CHECK(read_bytes(future_manifest_bundle / "0" / "metadata.txt")
          == future_version_bytes);

    // Unicode C1 controls are rejected even when the root checksum is updated.
    const std::string newer_root = root;
    std::string control_name_root = newer_root;
    CHECK(replace_once(control_name_root, "project.name\tRoot%20Checksum\n",
                       "project.name\tBad%C2%80Name\n"));
    CHECK(write_bytes(checksum_bundle / "metadata.txt", control_name_root));
    CHECK(rewrite_root_checksum(checksum_bundle));
    pvt::ProjectDocument control_rejected;
    CHECK(!pvt::load_project_document(as_utf8(checksum_bundle), control_rejected,
                                      &error));

    root = newer_root;
    const std::string key = "project.first_created_utc\t";
    const std::size_t at = root.find(key);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        root.replace(at + key.size() + 5U, 2U, "99");
    }
    CHECK(write_bytes(checksum_bundle / "metadata.txt", root));
    CHECK(rewrite_root_checksum(checksum_bundle));
    pvt::ProjectDocument rejected = pvt::default_project_document();
    const std::string original_uuid = rejected.project.uuid;
    CHECK(!pvt::load_project_document(as_utf8(checksum_bundle), rejected, &error));
    CHECK(rejected.project.uuid == original_uuid);
}

std::size_t asset_entry_count(const pvt::detail::BundleFileSet& files) {
    return static_cast<std::size_t>(std::count_if(
        files.files.begin(), files.files.end(), [](const auto& entry) {
            const std::string suffix = "/music_analysis.txt";
            return entry.first.rfind("assets/", 0U) == 0U
                   && (entry.first.size() < suffix.size()
                       || entry.first.compare(entry.first.size() - suffix.size(),
                                              suffix.size(), suffix) != 0);
        }));
}

std::size_t music_analysis_entry_count(
    const pvt::detail::BundleFileSet& files) {
    const std::string suffix = "/music_analysis.txt";
    return static_cast<std::size_t>(std::count_if(
        files.files.begin(), files.files.end(), [&suffix](const auto& entry) {
            return entry.first.rfind("assets/", 0U) == 0U
                   && entry.first.size() >= suffix.size()
                   && entry.first.compare(entry.first.size() - suffix.size(),
                                          suffix.size(), suffix) == 0;
        }));
}

std::string readable_asset_path(const pvt::ProjectAttachment& attachment) {
    return "assets/" + attachment.sha256 + "/" + attachment.basename;
}

void test_content_addressed_embedded_assets(const fs::path& directory) {
    const std::string audio_bytes =
        std::string("RIFF\x24\0\0\0WAVEfmt \x10\0\0\0", 20U)
        + std::string("\x03\0\x02\0\x80\xbb\0\0\0\xee\x02\0\x08\0\x20\0", 16U)
        + std::string("data\0\0\0\0", 8U);
    const std::string obj_bytes =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const std::string image_bytes =
        std::string("\x89PNG\r\n\x1a\n", 8U) + "portable-image-fixture";
    const std::string height_bytes =
        std::string("\x89PNG\r\n\x1a\n", 8U) + "portable-height-fixture";
    const fs::path audio_source = directory / "source-track.wav";
    const fs::path obj_source = directory / "source-mesh.obj";
    const fs::path image_source = directory / "source-image.png";
    const fs::path height_source = directory / "source-height.png";
    CHECK(write_bytes(audio_source, audio_bytes));
    CHECK(write_bytes(obj_source, obj_bytes));
    CHECK(write_bytes(image_source, image_bytes));
    CHECK(write_bytes(height_source, height_bytes));

    pvt::ProjectDocument document = pvt::default_project_document();
    document.project.name = "Embedded Assets";
    std::string error;
    pvt::ProjectAttachment music_attachment;
    pvt::ProjectAttachment layer_music_attachment;
    pvt::ProjectAttachment obj_attachment;
    pvt::ProjectAttachment image_attachment;
    pvt::ProjectAttachment height_attachment;
    CHECK(pvt::attach_project_file(
        document, pvt::kMusicSourceAttachmentId, as_utf8(audio_source),
        &music_attachment, &error));
    CHECK(pvt::attach_project_file(
        document,
        pvt::layer_music_attachment_id(
            document.project.layers.front().uuid),
        as_utf8(audio_source), &layer_music_attachment, &error));
    CHECK(pvt::attach_project_file(
        document,
        pvt::surface_obj_attachment_id(document.project.layers.front().uuid),
        as_utf8(obj_source), &obj_attachment, &error));
    CHECK(pvt::attach_project_file(
        document,
        pvt::starting_image_attachment_id(
            document.project.layers.front().uuid),
        as_utf8(image_source),
        &image_attachment, &error));
    CHECK(pvt::attach_project_file(
        document,
        pvt::plane_displacement_attachment_id(
            document.project.layers.front().uuid),
        as_utf8(height_source), &height_attachment, &error));
    // A second logical reference with the same bytes and original filename
    // reuses the same readable bundle entry.
    CHECK(pvt::attach_project_file(
        document, "audio.safety_copy", as_utf8(audio_source), nullptr, &error));

    pvt::MusicAnalysis& music = document.project.canvas.clock.music;
    music.analyzer_version = "bundle-test/1";
    music.source_sha256 = music_attachment.sha256;
    music.source_basename = music_attachment.basename;
    music.source_format = "wav-f32";
    music.source_frame_count = 48000U;
    music.source_sample_rate = 48000U;
    music.source_channel_count = 2U;
    music.duration_seconds = 1.0;
    music.detected_bpm = 120.0;
    music.tempo_confidence = 0.9;
    music.beat_times_seconds = {0.0, 0.5};
    music.tempo_points = {{0.0, 120.0, 0.9}};
    music.feature_samples = {
        {0.5F, 0.4F, 0.3F, 0.2F, 0.8F, 1.0F},
    };
    auto& layer_clock =
        document.project.layers.front().render.layer_clock;
    layer_clock.enabled = true;
    layer_clock.scale = pvt::LayerClockScale::StraightFit;
    layer_clock.clock = document.project.canvas.clock;
    layer_clock.clock.data_only = true;
    layer_clock.clock.music.source_sha256 = layer_music_attachment.sha256;
    layer_clock.clock.music.source_basename = layer_music_attachment.basename;
    pvt::SurfaceConfig& surface =
        document.project.layers.front().render.surface;
    surface.mapping = pvt::SurfaceMapping::CustomObj;
    surface.obj_path = obj_attachment.local_path;
    surface.obj_sha256 = obj_attachment.sha256;
    surface.obj_basename = obj_attachment.basename;
    surface.plane_displacement.enabled = false;
    surface.plane_displacement.path = height_attachment.local_path;
    surface.plane_displacement.sha256 = height_attachment.sha256;
    surface.plane_displacement.basename = height_attachment.basename;
    auto& starting_image =
        document.project.layers.front().render.starting_image;
    starting_image.enabled = true;
    starting_image.fit = pvt::StartingImageFit::Contain;
    starting_image.path = image_attachment.local_path;
    starting_image.sha256 = image_attachment.sha256;
    starting_image.basename = image_attachment.basename;
    document.project.output.write_alpha = true;

    // Registration owns a managed copy immediately, so all originals may move
    // before the first save without losing the attachment.
    std::error_code filesystem_error;
    CHECK(fs::remove(audio_source, filesystem_error) && !filesystem_error);
    filesystem_error.clear();
    CHECK(fs::remove(obj_source, filesystem_error) && !filesystem_error);
    filesystem_error.clear();
    CHECK(fs::remove(image_source, filesystem_error) && !filesystem_error);
    filesystem_error.clear();
    CHECK(fs::remove(height_source, filesystem_error) && !filesystem_error);

    const fs::path bundle = directory / portable_root(document.project.name);
    pvt::BundleSaveReport report;
    CHECK(pvt::save_project_document(document, as_utf8(bundle), &report, &error));
    CHECK(report.created_version && report.version == 0U);
    // Merely browsing an unpacked project in Finder must not break it. The
    // out-of-band metadata remains on disk but is excluded from project state.
    CHECK(write_bytes(bundle / ".DS_Store", "finder root metadata"));
    CHECK(write_bytes(bundle / "0" / ".DS_Store", "finder folder metadata"));
    pvt::detail::BundleFileSet directory_files;
    CHECK(pvt::detail::read_bundle_file_set(
        as_utf8(bundle), directory_files, &error));
    CHECK(asset_entry_count(directory_files) == 4U);
    CHECK(directory_files.files.count(readable_asset_path(music_attachment)) == 1U);
    CHECK(directory_files.files.count(readable_asset_path(obj_attachment)) == 1U);
    CHECK(directory_files.files.count(readable_asset_path(image_attachment)) == 1U);
    CHECK(directory_files.files.count(readable_asset_path(height_attachment)) == 1U);
    CHECK(read_bytes(bundle / "0" / "metadata.txt").rfind(
              "PVT_VERSION\t5\n", 0U) == 0U);
    CHECK(music_analysis_entry_count(directory_files) == 1U);
    CHECK(directory_files.files["0/render_output.txt"].find(
              "timing.music.feature_samples") == std::string::npos);
    CHECK(directory_files.files.count("0/music_analysis.txt") == 1U);
    CHECK(directory_files.files.count("0/0.music_analysis.txt") == 1U);
    CHECK(directory_files.files["0/0.pvt"].find(
              "layer_clock.music.feature_samples") == std::string::npos);
    CHECK(directory_files.files.count(".DS_Store") == 0U
          && directory_files.files.count("0/.DS_Store") == 0U);

    // The no-change fast path must still notice direct API edits inside the
    // large music tables even when a caller does not separately set dirty.
    const fs::path fingerprint_bundle = directory / "analysis-fingerprint-edit";
    filesystem_error.clear();
    fs::copy(bundle, fingerprint_bundle, fs::copy_options::recursive,
             filesystem_error);
    CHECK(!filesystem_error);
    pvt::ProjectDocument fingerprint_document;
    CHECK(pvt::load_project_document(
        as_utf8(fingerprint_bundle), fingerprint_document, &error));
    CHECK(pvt::save_project_document(
        fingerprint_document, as_utf8(fingerprint_bundle), &report, &error));
    CHECK(report.validated_only && !report.created_version);
    CHECK(!fingerprint_document.project.canvas.clock.music.feature_samples.empty());
    const float changed_energy = 0.625F;
    if (!fingerprint_document.project.canvas.clock.music.feature_samples.empty()) {
        fingerprint_document.project.canvas.clock.music.feature_samples.front().energy =
            changed_energy;
    }
    CHECK(pvt::save_project_document(
        fingerprint_document, as_utf8(fingerprint_bundle), &report, &error));
    CHECK(report.created_version && report.version == 1U);
    pvt::ProjectDocument fingerprint_reloaded;
    CHECK(pvt::load_project_document(
        as_utf8(fingerprint_bundle), fingerprint_reloaded, &error));
    CHECK(!fingerprint_reloaded.project.canvas.clock.music.feature_samples.empty());
    if (!fingerprint_reloaded.project.canvas.clock.music.feature_samples.empty()) {
        CHECK(fingerprint_reloaded.project.canvas.clock.music.feature_samples.front().energy
              == changed_energy);
    }
    CHECK(pvt::validate_project_bundle(
        as_utf8(fingerprint_bundle), nullptr, &error));

    // The shared analysis object is also directly editable. A valid edit is
    // loaded dirty and promoted to a new content identity on Save instead of
    // corrupting or silently discarding the user's manual change.
    const fs::path edited_analysis_bundle = directory / "direct-analysis-edit";
    filesystem_error.clear();
    fs::copy(bundle, edited_analysis_bundle, fs::copy_options::recursive,
             filesystem_error);
    CHECK(!filesystem_error);
    auto analysis_entry = std::find_if(
        directory_files.files.begin(), directory_files.files.end(),
        [](const auto& entry) {
            return entry.first.rfind("assets/", 0U) == 0U
                   && entry.first.size() >= 19U
                   && entry.first.compare(entry.first.size() - 19U, 19U,
                                          "/music_analysis.txt") == 0;
        });
    CHECK(analysis_entry != directory_files.files.end());
    if (analysis_entry != directory_files.files.end()) {
        // Missing references, missing objects, and malformed objects all fail
        // transactionally instead of producing a partially loaded project.
        for (const std::string& failure_case :
             {"missing-analysis-reference", "missing-analysis-object",
              "malformed-analysis-object"}) {
            const fs::path rejected_bundle = directory / failure_case;
            filesystem_error.clear();
            fs::copy(bundle, rejected_bundle, fs::copy_options::recursive,
                     filesystem_error);
            CHECK(!filesystem_error);
            if (failure_case == "missing-analysis-reference") {
                filesystem_error.clear();
                CHECK(fs::remove(rejected_bundle / "0" / "music_analysis.txt",
                                 filesystem_error));
                CHECK(!filesystem_error);
            } else if (failure_case == "missing-analysis-object") {
                filesystem_error.clear();
                CHECK(fs::remove(
                    rejected_bundle
                        / pvt::detail::path_from_utf8(analysis_entry->first),
                    filesystem_error));
                CHECK(!filesystem_error);
            } else {
                CHECK(write_bytes(
                    rejected_bundle
                        / pvt::detail::path_from_utf8(analysis_entry->first),
                    "not a music analysis\n"));
            }
            pvt::ProjectDocument rejected = pvt::default_project_document();
            rejected.project.name = "transaction sentinel";
            const std::string sentinel_uuid = rejected.project.uuid;
            CHECK(!pvt::load_project_document(
                as_utf8(rejected_bundle), rejected, &error));
            CHECK(rejected.project.uuid == sentinel_uuid
                  && rejected.project.name == "transaction sentinel");
        }

        std::string edited_analysis = analysis_entry->second;
        CHECK(replace_record_value(
            edited_analysis, "timing.music.detected_bpm", "121"));
        CHECK(write_bytes(edited_analysis_bundle
                              / pvt::detail::path_from_utf8(analysis_entry->first),
                          edited_analysis));
        pvt::ProjectDocument analysis_edited;
        CHECK(pvt::load_project_document(
            as_utf8(edited_analysis_bundle), analysis_edited, &error));
        CHECK(analysis_edited.externally_modified && analysis_edited.dirty);
        CHECK(analysis_edited.project.canvas.clock.music.detected_bpm == 121.0);
        CHECK(pvt::save_project_document(
            analysis_edited, as_utf8(edited_analysis_bundle), &report, &error));
        CHECK(report.promoted_external_change && report.version == 1U);
        pvt::detail::BundleFileSet promoted_analysis_files;
        CHECK(pvt::detail::read_bundle_file_set(
            as_utf8(edited_analysis_bundle), promoted_analysis_files, &error));
        CHECK(music_analysis_entry_count(promoted_analysis_files) == 2U);
        CHECK(pvt::validate_project_bundle(
            as_utf8(edited_analysis_bundle), nullptr, &error));
    }

    // Replacing readable music bytes directly performs the same bounded
    // analysis transaction as choosing a new source in the GUI.
    const fs::path edited_music_bundle = directory / "direct-music-edit";
    filesystem_error.clear();
    fs::copy(bundle, edited_music_bundle, fs::copy_options::recursive,
             filesystem_error);
    CHECK(!filesystem_error);
    const std::string replacement_wave = readable_click_wave();
    CHECK(write_bytes(edited_music_bundle / "assets" / music_attachment.sha256
                          / music_attachment.basename,
                      replacement_wave));
    pvt::ProjectDocument edited_music;
    CHECK(pvt::load_project_document(as_utf8(edited_music_bundle), edited_music,
                                     &error));
    CHECK(edited_music.externally_modified && edited_music.dirty);
    CHECK(edited_music.project.canvas.clock.music.source_basename
          == music_attachment.basename);
    CHECK(edited_music.project.canvas.clock.music.source_sha256
          != music_attachment.sha256);
    CHECK(edited_music.project.canvas.clock.music.duration_seconds > 7.9);
    CHECK(pvt::save_project_document(edited_music, as_utf8(edited_music_bundle),
                                     &report, &error));
    CHECK(report.promoted_external_change && report.version == 1U);
    CHECK(pvt::validate_project_bundle(as_utf8(edited_music_bundle), nullptr,
                                       &error));

    pvt::ProjectDocument loaded;
    CHECK(pvt::load_project_document(as_utf8(bundle), loaded, &error));
    CHECK(loaded.attachments.size() == 6U);
    CHECK(read_bytes(pvt::detail::path_from_utf8(
              pvt::project_attachment_path(loaded,
                                           pvt::kMusicSourceAttachmentId)))
          == audio_bytes);
    CHECK(read_bytes(pvt::detail::path_from_utf8(
              pvt::project_attachment_path(
                  loaded, pvt::layer_music_attachment_id(
                              loaded.project.layers.front().uuid))))
          == audio_bytes);
    CHECK(loaded.project.layers.front().render.layer_clock.enabled);
    CHECK(loaded.project.layers.front().render.layer_clock.clock.data_only);
    CHECK(read_bytes(pvt::detail::path_from_utf8(
              loaded.project.layers.front().render.surface.obj_path))
          == obj_bytes);
    CHECK(read_bytes(pvt::detail::path_from_utf8(
              loaded.project.layers.front().render.surface
                  .plane_displacement.path))
          == height_bytes);
    CHECK(loaded.project.layers.front().render.surface
              .plane_displacement.sha256
          == height_attachment.sha256);
    CHECK(read_bytes(pvt::detail::path_from_utf8(
              pvt::project_attachment_path(
                  loaded, pvt::starting_image_attachment_id(
                              loaded.project.layers.front().uuid))))
          == image_bytes);
    CHECK(loaded.project.layers.front().render.starting_image.enabled);
    CHECK(loaded.project.layers.front().render.starting_image.fit
          == pvt::StartingImageFit::Contain);
    CHECK(loaded.project.layers.front().render.starting_image.sha256
          == image_attachment.sha256);

    loaded.project.layers.front().name = "Second version";
    CHECK(pvt::save_project_document(loaded, as_utf8(bundle), &report, &error));
    CHECK(report.created_version && report.version == 1U);
    CHECK(pvt::detail::read_bundle_file_set(
        as_utf8(bundle), directory_files, &error));
    CHECK(asset_entry_count(directory_files) == 4U);
    CHECK(music_analysis_entry_count(directory_files) == 1U);
    CHECK(fs::exists(bundle / ".DS_Store")
          && fs::exists(bundle / "0" / ".DS_Store"));

    // Renaming/moving a source preserves its exact new filename and extension.
    // Deleting it after Attach but before Save remains safe.
    const fs::path renamed_audio = directory / "renamed-track.wav";
    CHECK(write_bytes(renamed_audio, audio_bytes));
    pvt::ProjectAttachment renamed_attachment;
    CHECK(pvt::attach_project_file(
        loaded, pvt::kMusicSourceAttachmentId, as_utf8(renamed_audio),
        &renamed_attachment, &error));
    CHECK(renamed_attachment.sha256 == music_attachment.sha256);
    loaded.project.canvas.clock.music.source_sha256 = renamed_attachment.sha256;
    loaded.project.canvas.clock.music.source_basename = renamed_attachment.basename;
    filesystem_error.clear();
    CHECK(fs::remove(renamed_audio, filesystem_error) && !filesystem_error);
    CHECK(pvt::save_project_document(loaded, as_utf8(bundle), &report, &error));
    CHECK(report.created_version && report.version == 2U);
    CHECK(pvt::detail::read_bundle_file_set(
        as_utf8(bundle), directory_files, &error));
    CHECK(asset_entry_count(directory_files) == 4U);
    CHECK(static_cast<std::size_t>(std::count_if(
              directory_files.files.begin(), directory_files.files.end(),
              [&renamed_attachment](const auto& entry) {
                  return entry.first.rfind(
                             "assets/" + renamed_attachment.sha256 + "/",
                             0U) == 0U
                         && entry.first.find("/music_analysis.txt")
                                == std::string::npos;
              })) == 1U);

    const std::string changed_audio_bytes = audio_bytes + "changed";
    CHECK(write_bytes(renamed_audio, changed_audio_bytes));
    pvt::ProjectAttachment changed_attachment;
    CHECK(pvt::attach_project_file(
        loaded, pvt::kMusicSourceAttachmentId, as_utf8(renamed_audio),
        &changed_attachment, &error));
    CHECK(changed_attachment.sha256 != music_attachment.sha256);
    loaded.project.canvas.clock.music.source_sha256 = changed_attachment.sha256;
    loaded.project.canvas.clock.music.source_basename = changed_attachment.basename;
    filesystem_error.clear();
    CHECK(fs::remove(renamed_audio, filesystem_error) && !filesystem_error);
    CHECK(pvt::save_project_document(loaded, as_utf8(bundle), &report, &error));
    CHECK(report.created_version && report.version == 3U);
    CHECK(pvt::detail::read_bundle_file_set(
        as_utf8(bundle), directory_files, &error));
    CHECK(asset_entry_count(directory_files) == 5U);
    CHECK(directory_files.files.count(readable_asset_path(music_attachment)) == 1U);
    CHECK(directory_files.files.count(readable_asset_path(changed_attachment)) == 1U);

    // An independent ZIP copy carries the current referenced bytes without
    // copying unreachable history assets.
    CHECK(pvt::detach_project_file(loaded, "audio.safety_copy", &error));
    pvt::ProjectDocument independent;
    CHECK(pvt::make_independent_project_copy(loaded, independent, &error));
    independent.project.name = "Embedded Assets ZIP";
    const fs::path zip = directory / "embedded-assets.zip";
    CHECK(pvt::save_project_document(independent, as_utf8(zip), &report, &error));
    pvt::detail::BundleFileSet zip_files;
    CHECK(pvt::detail::read_bundle_file_set(as_utf8(zip), zip_files, &error));
    CHECK(zip_files.from_zip && asset_entry_count(zip_files) == 5U);
    pvt::ProjectDocument zip_loaded;
    CHECK(pvt::load_project_document(as_utf8(zip), zip_loaded, &error));
    CHECK(read_bytes(pvt::detail::path_from_utf8(
              pvt::project_attachment_path(zip_loaded,
                                           pvt::kMusicSourceAttachmentId)))
          == changed_audio_bytes);
    CHECK(read_bytes(pvt::detail::path_from_utf8(
              zip_loaded.project.layers.front().render.surface.obj_path))
          == obj_bytes);
    CHECK(read_bytes(pvt::detail::path_from_utf8(
              zip_loaded.project.layers.front().render.surface
                  .plane_displacement.path))
          == height_bytes);

    // Version-2 bare-digest payloads remain loadable and the next changed Save
    // writes a readable current-format snapshot without rewriting legacy history.
    pvt::detail::BundleFileSet legacy_files = zip_files;
    std::string legacy_output;
    std::string legacy_output_digest;
    CHECK(pvt::detail::serialize_render_output_config(
        independent.project.canvas, independent.project.output,
        legacy_output, &error));
    CHECK(pvt::detail::sha256_hex(
        legacy_output, legacy_output_digest, &error));
    legacy_files.files["0/render_output.txt"] = legacy_output;
    legacy_files.files.erase("0/music_analysis.txt");
    std::string legacy_layer;
    std::string legacy_layer_digest;
    CHECK(pvt::detail::serialize_layer_config(
        independent.project.layers.front().render, legacy_layer, &error,
        &independent.project.canvas.motion_paths));
    CHECK(pvt::detail::sha256_hex(
        legacy_layer, legacy_layer_digest, &error));
    legacy_files.files["0/0.pvt"] = legacy_layer;
    legacy_files.files.erase("0/0.music_analysis.txt");
    std::vector<std::pair<std::string, std::string>> legacy_assets;
    for (auto iterator = legacy_files.files.begin();
         iterator != legacy_files.files.end();) {
        if (iterator->first.rfind("assets/", 0U) != 0U) {
            ++iterator;
            continue;
        }
        const std::size_t slash = iterator->first.find('/', 7U);
        CHECK(slash == 7U + 64U);
        if (iterator->first.substr(slash + 1U) == "music_analysis.txt") {
            iterator = legacy_files.files.erase(iterator);
            continue;
        }
        legacy_assets.emplace_back(iterator->first.substr(0U, slash),
                                   iterator->second);
        iterator = legacy_files.files.erase(iterator);
    }
    for (auto& asset : legacy_assets) {
        CHECK(legacy_files.files.emplace(std::move(asset)).second);
    }
    std::string& legacy_manifest = legacy_files.files["0/metadata.txt"];
    CHECK(replace_once(legacy_manifest, "PVT_VERSION\t5\n",
                       "PVT_VERSION\t2\n"));
    CHECK(erase_record(legacy_manifest, "music_analysis.sha256"));
    CHECK(replace_record_value(legacy_manifest, "render_output.sha256",
                               legacy_output_digest));
    CHECK(replace_record_value(legacy_manifest, "layers.0.sha256",
                               legacy_layer_digest));
    const fs::path legacy_zip = directory / "legacy-assets-v2.zip";
    CHECK(pvt::detail::write_bundle_file_set(as_utf8(legacy_zip), legacy_files,
                                             &error));
    pvt::ProjectDocument legacy_loaded;
    CHECK(pvt::load_project_document(as_utf8(legacy_zip), legacy_loaded, &error));
    CHECK(legacy_loaded.externally_modified);
    legacy_loaded.project.layers.front().name = "Readable asset upgrade";
    CHECK(pvt::save_project_document(legacy_loaded, as_utf8(legacy_zip),
                                     &report, &error));
    CHECK(report.version == 1U && report.created_version
          && report.compacted_storage);
    pvt::detail::BundleFileSet upgraded_files;
    CHECK(pvt::detail::read_bundle_file_set(as_utf8(legacy_zip), upgraded_files,
                                            &error));
    CHECK(upgraded_files.files.count(readable_asset_path(changed_attachment)) == 1U);
    CHECK(upgraded_files.files.count("0/music_analysis.txt") == 1U);
    CHECK(upgraded_files.files["0/render_output.txt"].size()
          < legacy_output.size());
    pvt::ProjectConfig compacted_legacy_version;
    CHECK(pvt::load_project_version(legacy_loaded, 0U,
                                    compacted_legacy_version, &error));
    CHECK(compacted_legacy_version.canvas.clock.music.feature_samples.size()
          == independent.project.canvas.clock.music.feature_samples.size());
    CHECK(pvt::validate_project_bundle(as_utf8(legacy_zip), nullptr, &error));

    // The same migration is an allowed atomic storage update for unpacked
    // bundles; it does not require rewriting unrelated immutable files.
    pvt::detail::BundleFileSet legacy_directory_files = zip_files;
    legacy_directory_files.files["0/render_output.txt"] = legacy_output;
    legacy_directory_files.files.erase("0/music_analysis.txt");
    legacy_directory_files.files["0/0.pvt"] = legacy_layer;
    legacy_directory_files.files.erase("0/0.music_analysis.txt");
    for (auto iterator = legacy_directory_files.files.begin();
         iterator != legacy_directory_files.files.end();) {
        if (iterator->first.rfind("assets/", 0U) == 0U
            && iterator->first.size() >= 19U
            && iterator->first.compare(iterator->first.size() - 19U, 19U,
                                       "/music_analysis.txt") == 0) {
            iterator = legacy_directory_files.files.erase(iterator);
        } else {
            ++iterator;
        }
    }
    std::string& legacy_directory_manifest =
        legacy_directory_files.files["0/metadata.txt"];
    CHECK(replace_once(legacy_directory_manifest, "PVT_VERSION\t5\n",
                       "PVT_VERSION\t3\n"));
    CHECK(erase_record(legacy_directory_manifest, "music_analysis.sha256"));
    CHECK(replace_record_value(legacy_directory_manifest,
                               "render_output.sha256",
                               legacy_output_digest));
    CHECK(replace_record_value(legacy_directory_manifest,
                               "layers.0.sha256",
                               legacy_layer_digest));
    std::string legacy_directory_metadata_digest;
    CHECK(pvt::detail::sha256_hex(legacy_directory_manifest,
                                  legacy_directory_metadata_digest, &error));
    pvt::detail::BundleFileSet legacy_directory_tree;
    legacy_directory_tree.root_name = "0";
    for (const auto& entry : legacy_directory_files.files) {
        if (entry.first.rfind("0/", 0U) == 0U) {
            legacy_directory_tree.files.emplace(entry.first.substr(2U),
                                                entry.second);
        }
    }
    std::string legacy_directory_tree_digest;
    CHECK(pvt::detail::bundle_file_set_digest(
        legacy_directory_tree, legacy_directory_tree_digest, &error));
    std::string& legacy_directory_root =
        legacy_directory_files.files["metadata.txt"];
    CHECK(replace_record_value(legacy_directory_root,
                               "versions.0.metadata_sha256",
                               legacy_directory_metadata_digest));
    CHECK(replace_record_value(legacy_directory_root,
                               "versions.0.tree_sha256",
                               legacy_directory_tree_digest));
    CHECK(replace_record_value(legacy_directory_files.files["current"],
                               "metadata.sha256",
                               legacy_directory_metadata_digest));
    const fs::path legacy_parent = directory / "legacy-directory-parent";
    filesystem_error.clear();
    CHECK(fs::create_directory(legacy_parent, filesystem_error));
    CHECK(!filesystem_error);
    const fs::path legacy_directory = legacy_parent
        / pvt::detail::path_from_utf8(legacy_directory_files.root_name);
    CHECK(pvt::detail::write_bundle_file_set(
        as_utf8(legacy_directory), legacy_directory_files, &error));
    CHECK(rewrite_root_checksum(legacy_directory));
    pvt::ProjectDocument legacy_directory_document;
    CHECK(pvt::load_project_document(
        as_utf8(legacy_directory), legacy_directory_document, &error));
    CHECK(!legacy_directory_document.externally_modified
          && !legacy_directory_document.dirty);
    const bool saved_legacy_directory = pvt::save_project_document(
        legacy_directory_document, as_utf8(legacy_directory), &report, &error);
    if (!saved_legacy_directory) {
        std::cerr << "legacy directory migration failed: " << error << '\n';
    }
    CHECK(saved_legacy_directory);
    CHECK(report.validated_only && !report.created_version
          && report.compacted_storage);
    CHECK(fs::file_size(legacy_directory / "0" / "render_output.txt")
          < legacy_output.size());
    CHECK(fs::exists(legacy_directory / "0" / "music_analysis.txt"));
    CHECK(pvt::validate_project_bundle(
        as_utf8(legacy_directory), nullptr, &error));

    // A direct edit to a readable asset is a first-class project edit. The
    // loader derives fresh identity metadata, marks the project dirty, and Save
    // promotes it to a new version without requiring manifest surgery.
    const std::string edited_image_bytes = image_bytes + "-direct-edit";
    const fs::path original_image_asset = bundle / "assets"
                                          / image_attachment.sha256
                                          / image_attachment.basename;
    const fs::path renamed_image_asset = original_image_asset.parent_path()
                                         / "cover-edited.png";
    filesystem_error.clear();
    fs::rename(original_image_asset, renamed_image_asset, filesystem_error);
    CHECK(!filesystem_error);
    CHECK(write_bytes(renamed_image_asset, edited_image_bytes));
    pvt::ProjectDocument directly_edited;
    CHECK(pvt::load_project_document(as_utf8(bundle), directly_edited, &error));
    CHECK(directly_edited.externally_modified && directly_edited.dirty);
    const pvt::ProjectAttachment* edited_image =
        pvt::find_project_attachment(
            directly_edited,
            pvt::starting_image_attachment_id(
                directly_edited.project.layers.front().uuid));
    CHECK(edited_image != nullptr);
    if (edited_image != nullptr) {
        CHECK(edited_image->basename == "cover-edited.png");
        CHECK(edited_image->sha256 != image_attachment.sha256);
        CHECK(edited_image->externally_modified);
        CHECK(read_bytes(pvt::detail::path_from_utf8(edited_image->local_path))
              == edited_image_bytes);
    }
    CHECK(pvt::save_project_document(directly_edited, as_utf8(bundle),
                                     &report, &error));
    CHECK(report.promoted_external_change && report.version == 4U);
    CHECK(pvt::validate_project_bundle(as_utf8(bundle), nullptr, &error));

    // An unreadable direct replacement is rejected transactionally when it
    // cannot be treated like the corresponding GUI import. Here the current
    // music source is replaced with non-audio bytes.
    const fs::path corrupt_directory = directory / "corrupt-assets";
    filesystem_error.clear();
    fs::copy(bundle, corrupt_directory, fs::copy_options::recursive,
             filesystem_error);
    CHECK(!filesystem_error);
    CHECK(write_bytes(corrupt_directory / "assets" / changed_attachment.sha256
                          / changed_attachment.basename,
                      "corrupt"));
    CHECK(write_bytes(corrupt_directory / "assets" / music_attachment.sha256
                          / music_attachment.basename,
                      "corrupt"));
    CHECK(write_bytes(corrupt_directory / "assets" / renamed_attachment.sha256
                          / renamed_attachment.basename,
                      "corrupt"));
    pvt::ProjectDocument untouched = pvt::default_project_document();
    const std::string untouched_uuid = untouched.project.uuid;
    CHECK(!pvt::load_project_document(
        as_utf8(corrupt_directory), untouched, &error));
    CHECK(untouched.project.uuid == untouched_uuid);

    pvt::detail::BundleFileSet corrupt_zip_files = zip_files;
    corrupt_zip_files.files[readable_asset_path(changed_attachment)] = "corrupt";
    const fs::path corrupt_zip = directory / "corrupt-assets.zip";
    CHECK(pvt::detail::write_bundle_file_set(
        as_utf8(corrupt_zip), corrupt_zip_files, &error));
    CHECK(!pvt::load_project_document(as_utf8(corrupt_zip), untouched, &error));

    pvt::detail::BundleFileSet hostile = zip_files;
    hostile.files["assets/not-a-lowercase-sha256/source.wav"] = "hostile";
    const fs::path hostile_zip = directory / "hostile-asset.zip";
    CHECK(pvt::detail::write_bundle_file_set(
        as_utf8(hostile_zip), hostile, &error));
    CHECK(!pvt::load_project_document(as_utf8(hostile_zip), untouched, &error));

    // Oversized input is rejected from file metadata without allocating its
    // contents. The sparse fixture keeps the test cheap on supported filesystems.
    const fs::path oversized = directory / "oversized-asset.wav";
    {
        std::ofstream output(oversized, std::ios::binary | std::ios::trunc);
        output.seekp(static_cast<std::streamoff>(
            pvt::kMaximumProjectAttachmentBytes));
        output.put('x');
        CHECK(static_cast<bool>(output));
    }
    CHECK(!pvt::attach_project_file(
        loaded, "oversized.asset", as_utf8(oversized), nullptr, &error));
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    test_layer_codec_backward_compatibility();
    test_particle_workload_canvas_and_project_boundaries();
    test_aggregate_particle_bundle_recovery(temporary.path());
    test_render_output_codec_backward_compatibility();
    test_sha_and_archive_guards(temporary.path());
    test_archive_compare_and_swap(temporary.path());
    test_directory_versions_and_names(temporary.path());
    test_independent_current_state_copy(temporary.path());
    test_zip_unicode_and_legacy(temporary.path());
    test_external_change_lifecycle(temporary.path());
    test_fallback_orphan_and_stale(temporary.path());
    test_complete_history_accounting(temporary.path());
    test_corrupt_history_and_root_metadata(temporary.path());
    test_content_addressed_embedded_assets(temporary.path());
    if (failures != 0) {
        std::cerr << failures << " bundle test(s) failed.\n";
        return 1;
    }
    std::cout << "All project bundle tests passed.\n";
    return 0;
}
