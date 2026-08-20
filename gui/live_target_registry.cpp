#include "live_target_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <QObject>
#include <string>
#include <type_traits>
#include <utility>

namespace {

const double kMaximumRenderParameter =
    pvt::maximum_render_parameter_magnitude();
constexpr double kMinimumPositiveUiValue = 0.000001;
constexpr double kMinimumIntegerParameter =
    static_cast<double>((std::numeric_limits<int>::min)());
constexpr double kMaximumIntegerParameter =
    static_cast<double>((std::numeric_limits<int>::max)());

template <typename Value>
Value bounded(double value, double minimum, double maximum) {
    const double clamped = std::clamp(value, minimum, maximum);
    if constexpr (std::is_same_v<Value, bool>) {
        return clamped >= 0.5;
    } else if constexpr (std::is_integral_v<Value>) {
        return static_cast<Value>(std::llround(clamped));
    } else {
        return static_cast<Value>(clamped);
    }
}

pvt::LayerConfig* find_layer(pvt::ProjectConfig& project,
                             const std::string& uuid) {
    const auto found = std::find_if(
        project.layers.begin(), project.layers.end(),
        [&uuid](const pvt::LayerConfig& layer) { return layer.uuid == uuid; });
    return found == project.layers.end() ? nullptr : &*found;
}

pvt::LayerGroup* find_group(pvt::ProjectConfig& project,
                            const std::string& uuid) {
    const auto found = std::find_if(
        project.groups.begin(), project.groups.end(),
        [&uuid](const pvt::LayerGroup& group) { return group.uuid == uuid; });
    return found == project.groups.end() ? nullptr : &*found;
}

pvt::WaveConfig* find_wave(pvt::LayerConfig& layer, std::uint64_t id) {
    const auto found = std::find_if(
        layer.render.waves.begin(), layer.render.waves.end(),
        [id](const pvt::WaveConfig& item) { return item.id == id; });
    return found == layer.render.waves.end() ? nullptr : &*found;
}

pvt::SwingConfig* find_swing(pvt::LayerConfig& layer, std::uint64_t id) {
    const auto found = std::find_if(
        layer.render.swings.begin(), layer.render.swings.end(),
        [id](const pvt::SwingConfig& item) { return item.id == id; });
    return found == layer.render.swings.end() ? nullptr : &*found;
}

pvt::EffectConfig* find_effect(pvt::LayerConfig& layer, std::uint64_t id) {
    const auto found = std::find_if(
        layer.render.effects.begin(), layer.render.effects.end(),
        [id](const pvt::EffectConfig& item) { return item.id == id; });
    return found == layer.render.effects.end() ? nullptr : &*found;
}

QString layer_section(const pvt::LayerConfig& layer, const QString& suffix) {
    return QObject::tr("%1 — %2")
        .arg(QString::fromStdString(layer.name), suffix);
}

} // namespace

std::vector<LiveTargetDescriptor> buildLiveTargetRegistry(
    const pvt::ProjectConfig& project) {
    std::vector<LiveTargetDescriptor> result;
    try {
        result.reserve(64U + project.groups.size()
                       + project.layers.size() * 160U);
    } catch (...) {
        return {};
    }
    const auto append = [&result](QString path, QString label, QString section,
                                  LiveTargetKind kind, double minimum,
                                  double maximum, double current,
                                  std::function<bool(pvt::ProjectConfig&, double)> apply) {
        auto finite_apply = [apply = std::move(apply)](
                                pvt::ProjectConfig& project,
                                double input) {
            return std::isfinite(input) && apply(project, input);
        };
        result.push_back({std::move(path), std::move(label), std::move(section),
                          kind, minimum, maximum, current,
                          std::move(finite_apply)});
    };

    append(QStringLiteral("project.fps"), QObject::tr("Playback FPS"),
           QObject::tr("Project"), LiveTargetKind::Real,
           kMinimumPositiveUiValue, kMaximumRenderParameter,
           project.canvas.fps,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.fps = bounded<double>(
                   input, kMinimumPositiveUiValue, kMaximumRenderParameter);
               return true;
           });
    append(QStringLiteral("project.clock.bpm"), QObject::tr("Project tempo"),
           QObject::tr("Project clock"), LiveTargetKind::Real,
           kMinimumPositiveUiValue, kMaximumRenderParameter,
           project.canvas.clock.meter.bpm,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.clock.meter.bpm = bounded<double>(
                   input, kMinimumPositiveUiValue, kMaximumRenderParameter);
               return true;
           });
    append(QStringLiteral("project.clock.phase"), QObject::tr("Project clock phase"),
           QObject::tr("Project clock"), LiveTargetKind::Real,
           -kMaximumRenderParameter, kMaximumRenderParameter,
           project.canvas.clock.phase_offset_degrees,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.clock.phase_offset_degrees =
                   bounded<double>(input, -kMaximumRenderParameter,
                                   kMaximumRenderParameter);
               return true;
           });
    append(QStringLiteral("project.clock.reverse"), QObject::tr("Reverse project clock"),
           QObject::tr("Project clock"), LiveTargetKind::Boolean, 0.0, 1.0,
           project.canvas.clock.reverse ? 1.0 : 0.0,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.clock.reverse = bounded<bool>(input, 0.0, 1.0); return true;
           });
    append(QStringLiteral("project.audio.enabled"), QObject::tr("Project audio response"),
           QObject::tr("Project audio"), LiveTargetKind::Boolean, 0.0, 1.0,
           project.canvas.audio_reactive_defaults.enabled ? 1.0 : 0.0,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.enabled =
                   bounded<bool>(input, 0.0, 1.0); return true;
           });
    append(QStringLiteral("project.audio.synchronized_only"), QObject::tr("Synchronized items only"),
           QObject::tr("Project audio"), LiveTargetKind::Boolean, 0.0, 1.0,
           project.canvas.audio_reactive_defaults.synchronized_only ? 1.0 : 0.0,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.synchronized_only = input >= 0.5;
               return true;
           });
    append(QStringLiteral("project.audio.waves_enabled"), QObject::tr("Audio drives waves"),
           QObject::tr("Project audio"), LiveTargetKind::Boolean, 0.0, 1.0,
           project.canvas.audio_reactive_defaults.waves_enabled ? 1.0 : 0.0,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.waves_enabled = input >= 0.5;
               return true;
           });
    append(QStringLiteral("project.audio.wave_source"), QObject::tr("Wave audio feature"),
           QObject::tr("Project audio"), LiveTargetKind::Enumeration, 0.0, 9.0,
           static_cast<double>(project.canvas.audio_reactive_defaults.wave_source),
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.wave_source =
                   static_cast<pvt::MusicFeature>(std::llround(input));
               return true;
           });
    append(QStringLiteral("project.audio.wave_amount"), QObject::tr("Wave response amount"),
           QObject::tr("Project audio"), LiveTargetKind::Real,
           -kMaximumRenderParameter, kMaximumRenderParameter,
           project.canvas.audio_reactive_defaults.wave_amount,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.wave_amount =
                   bounded<double>(input, -kMaximumRenderParameter,
                                   kMaximumRenderParameter); return true;
           });
    append(QStringLiteral("project.audio.effect_amount"), QObject::tr("Effect response amount"),
           QObject::tr("Project audio"), LiveTargetKind::Real,
           -kMaximumRenderParameter, kMaximumRenderParameter,
           project.canvas.audio_reactive_defaults.effect_amount,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.effect_amount =
                   bounded<double>(input, -kMaximumRenderParameter,
                                   kMaximumRenderParameter); return true;
           });
    append(QStringLiteral("project.audio.effects_enabled"), QObject::tr("Audio drives effects"),
           QObject::tr("Project audio"), LiveTargetKind::Boolean, 0.0, 1.0,
           project.canvas.audio_reactive_defaults.effects_enabled ? 1.0 : 0.0,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.effects_enabled = input >= 0.5;
               return true;
           });
    append(QStringLiteral("project.audio.effect_source"), QObject::tr("Effect audio feature"),
           QObject::tr("Project audio"), LiveTargetKind::Enumeration, 0.0, 9.0,
           static_cast<double>(project.canvas.audio_reactive_defaults.effect_source),
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.effect_source =
                   static_cast<pvt::MusicFeature>(std::llround(input));
               return true;
           });
    append(QStringLiteral("project.audio.color_amount"), QObject::tr("Color response degrees"),
           QObject::tr("Project audio"), LiveTargetKind::Real,
           -kMaximumRenderParameter, kMaximumRenderParameter,
           project.canvas.audio_reactive_defaults.color_amount_degrees,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.color_amount_degrees =
                   bounded<double>(input, -kMaximumRenderParameter,
                                   kMaximumRenderParameter); return true;
           });
    append(QStringLiteral("project.audio.color_enabled"), QObject::tr("Audio drives color"),
           QObject::tr("Project audio"), LiveTargetKind::Boolean, 0.0, 1.0,
           project.canvas.audio_reactive_defaults.color_enabled ? 1.0 : 0.0,
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.color_enabled = input >= 0.5;
               return true;
           });
    append(QStringLiteral("project.audio.color_source"), QObject::tr("Color audio feature"),
           QObject::tr("Project audio"), LiveTargetKind::Enumeration, 0.0, 9.0,
           static_cast<double>(project.canvas.audio_reactive_defaults.color_source),
           [](pvt::ProjectConfig& value, double input) {
               value.canvas.audio_reactive_defaults.color_source =
                   static_cast<pvt::MusicFeature>(std::llround(input));
               return true;
           });

    for (const pvt::LayerGroup& authored_group : project.groups) {
        const std::string uuid = authored_group.uuid;
        append(QStringLiteral("group/%1/enabled")
                   .arg(QString::fromStdString(uuid)),
               QObject::tr("Group visible"),
               QObject::tr("Group — %1")
                   .arg(QString::fromStdString(authored_group.name)),
               LiveTargetKind::Boolean, 0.0, 1.0,
               authored_group.enabled ? 1.0 : 0.0,
               [uuid](pvt::ProjectConfig& value, double input) {
                   pvt::LayerGroup* group = find_group(value, uuid);
                   if (group == nullptr) return false;
                   group->enabled = input >= 0.5;
                   return true;
               });
    }

    for (const pvt::LayerConfig& authored_layer : project.layers) {
        const std::string uuid = authored_layer.uuid;
        const QString prefix = QStringLiteral("layer/%1/")
                                   .arg(QString::fromStdString(uuid));
        const auto add_layer = [&](const QString& key, const QString& label,
                                   const QString& section, LiveTargetKind kind,
                                   double minimum, double maximum, double current,
                                   auto setter) {
            append(prefix + key, label, layer_section(authored_layer, section),
                   kind, minimum, maximum, current,
                   [uuid, minimum, maximum, setter](pvt::ProjectConfig& value,
                                                     double input) {
                       pvt::LayerConfig* layer = find_layer(value, uuid);
                       if (layer == nullptr) return false;
                       setter(*layer, std::clamp(input, minimum, maximum));
                       return true;
                   });
        };
        add_layer(QStringLiteral("enabled"), QObject::tr("Layer visible"),
                  QObject::tr("Mix"), LiveTargetKind::Boolean, 0.0, 1.0,
                  authored_layer.enabled ? 1.0 : 0.0,
                  [](pvt::LayerConfig& layer, double v) { layer.enabled = v >= 0.5; });
        add_layer(QStringLiteral("opacity"), QObject::tr("Layer opacity"),
                  QObject::tr("Mix"), LiveTargetKind::Real, 0.0, 1.0,
                  authored_layer.opacity,
                  [](pvt::LayerConfig& layer, double v) { layer.opacity = v; });
        add_layer(QStringLiteral("blend"), QObject::tr("Blend mode"),
                  QObject::tr("Mix"), LiveTargetKind::Enumeration, 0.0, 13.0,
                  static_cast<double>(authored_layer.blend_mode),
                  [](pvt::LayerConfig& layer, double v) {
                      layer.blend_mode = static_cast<pvt::BlendMode>(std::llround(v));
                  });
        add_layer(QStringLiteral("alpha_mode"), QObject::tr("Alpha order"),
                  QObject::tr("Mix"), LiveTargetKind::Enumeration, 0.0, 1.0,
                  static_cast<double>(authored_layer.alpha_mode),
                  [](pvt::LayerConfig& layer, double v) {
                      layer.alpha_mode = static_cast<pvt::AlphaMode>(std::llround(v));
                  });

        const pvt::RenderData& render = authored_layer.render;
        const auto add_render_real = [&](const QString& key, const QString& label,
                                         const QString& section, double minimum,
                                         double maximum, double current,
                                         auto member) {
            add_layer(key, label, section, LiveTargetKind::Real, minimum, maximum,
                      current, [member](pvt::LayerConfig& layer, double v) {
                          layer.render.*member = v;
                      });
        };
        const auto add_render_int = [&](const QString& key, const QString& label,
                                        const QString& section, double minimum,
                                        double maximum, int current, auto member) {
            add_layer(key, label, section, LiveTargetKind::Integer, minimum, maximum,
                      current, [member](pvt::LayerConfig& layer, double v) {
                          layer.render.*member = static_cast<int>(std::llround(v));
                      });
        };
        const auto add_render_bool = [&](const QString& key, const QString& label,
                                         const QString& section, bool current,
                                         auto member) {
            add_layer(key, label, section, LiveTargetKind::Boolean, 0.0, 1.0,
                      current ? 1.0 : 0.0,
                      [member](pvt::LayerConfig& layer, double v) {
                          layer.render.*member = v >= 0.5;
                      });
        };
        add_render_real(QStringLiteral("phrase_warp"), QObject::tr("Phrase warp"),
                        QObject::tr("Rhythm"), -kMaximumRenderParameter,
                        kMaximumRenderParameter, render.phrase_warp,
                        &pvt::RenderData::phrase_warp);
        add_render_real(QStringLiteral("ghost_mix"), QObject::tr("Ghost mix"),
                        QObject::tr("Rhythm"), 0.0, 1.0, render.ghost_mix,
                        &pvt::RenderData::ghost_mix);
        add_render_real(QStringLiteral("ghost_lag"), QObject::tr("Ghost lag"),
                        QObject::tr("Rhythm"), -kMaximumRenderParameter,
                        kMaximumRenderParameter,
                        render.ghost_lag_degrees, &pvt::RenderData::ghost_lag_degrees);
        add_render_bool(QStringLiteral("displacement_enabled"), QObject::tr("Displacement"),
                        QObject::tr("Modifiers"), render.displacement_enabled,
                        &pvt::RenderData::displacement_enabled);
        add_render_real(QStringLiteral("displacement"), QObject::tr("Displacement amount"),
                        QObject::tr("Modifiers"), 0.0,
                        kMaximumRenderParameter, render.displacement,
                        &pvt::RenderData::displacement);
        add_render_bool(QStringLiteral("lighting_enabled"), QObject::tr("Slope lighting"),
                        QObject::tr("Modifiers"), render.lighting_enabled,
                        &pvt::RenderData::lighting_enabled);
        add_render_real(QStringLiteral("wave_depth"), QObject::tr("Lighting depth"),
                        QObject::tr("Modifiers"), 0.0,
                        kMaximumRenderParameter, render.wave_depth,
                        &pvt::RenderData::wave_depth);
        add_render_bool(QStringLiteral("spiral_enabled"), QObject::tr("Spiral"),
                        QObject::tr("Modifiers"), render.spiral_enabled,
                        &pvt::RenderData::spiral_enabled);
        add_render_real(QStringLiteral("spiral_frequency"), QObject::tr("Spiral frequency"),
                        QObject::tr("Modifiers"), 0.0, kMaximumRenderParameter,
                        render.spiral_frequency, &pvt::RenderData::spiral_frequency);
        add_render_int(QStringLiteral("spiral_arms"), QObject::tr("Spiral arms"),
                       QObject::tr("Modifiers"), kMinimumIntegerParameter,
                       kMaximumIntegerParameter,
                       render.spiral_arms, &pvt::RenderData::spiral_arms);
        add_render_bool(QStringLiteral("wall_enabled"), QObject::tr("Wall reflection"),
                        QObject::tr("Modifiers"), render.wall_reflection_enabled,
                        &pvt::RenderData::wall_reflection_enabled);
        add_render_real(QStringLiteral("wall_frequency"), QObject::tr("Wall frequency"),
                        QObject::tr("Modifiers"), 0.0, kMaximumRenderParameter,
                        render.wall_frequency, &pvt::RenderData::wall_frequency);
        add_render_real(QStringLiteral("wall_mix"), QObject::tr("Wall mix"),
                        QObject::tr("Modifiers"), -kMaximumRenderParameter,
                        kMaximumRenderParameter, render.wall_mix,
                        &pvt::RenderData::wall_mix);
        add_render_int(QStringLiteral("hue_cycles"), QObject::tr("Hue cycles"),
                       QObject::tr("Color"), kMinimumIntegerParameter,
                       kMaximumIntegerParameter, render.hue_cycles,
                       &pvt::RenderData::hue_cycles);
        add_render_real(QStringLiteral("saturation"), QObject::tr("Saturation"),
                        QObject::tr("Color"), 0.0, 1.0, render.saturation,
                        &pvt::RenderData::saturation);
        add_render_bool(QStringLiteral("swings_enabled"), QObject::tr("Swing master"),
                        QObject::tr("Rhythm"), render.swings_enabled,
                        &pvt::RenderData::swings_enabled);

        const auto add_nested = [&](const QString& key, const QString& label,
                                    const QString& section, LiveTargetKind kind,
                                    double minimum, double maximum, double current,
                                    auto setter) {
            add_layer(key, label, section, kind, minimum, maximum, current,
                      [setter](pvt::LayerConfig& layer, double v) {
                          setter(layer.render, v);
                      });
        };
        add_nested(QStringLiteral("audio.override"), QObject::tr("Override project audio"),
                   QObject::tr("Audio"), LiveTargetKind::Boolean, 0, 1,
                   render.audio_reactive_override_enabled, [](pvt::RenderData& r, double v) { r.audio_reactive_override_enabled = v >= 0.5; });
        add_nested(QStringLiteral("audio.enabled"), QObject::tr("Audio response"),
                   QObject::tr("Audio"), LiveTargetKind::Boolean, 0, 1,
                   render.audio_reactive.enabled, [](pvt::RenderData& r, double v) { r.audio_reactive.enabled = v >= 0.5; });
        add_nested(QStringLiteral("audio.synchronized_only"), QObject::tr("Synchronized items only"),
                   QObject::tr("Audio"), LiveTargetKind::Boolean, 0, 1,
                   render.audio_reactive.synchronized_only, [](pvt::RenderData& r, double v) { r.audio_reactive.synchronized_only = v >= 0.5; });
        add_nested(QStringLiteral("audio.waves_enabled"), QObject::tr("Audio drives waves"),
                   QObject::tr("Audio"), LiveTargetKind::Boolean, 0, 1,
                   render.audio_reactive.waves_enabled, [](pvt::RenderData& r, double v) { r.audio_reactive.waves_enabled = v >= 0.5; });
        add_nested(QStringLiteral("audio.wave_source"), QObject::tr("Wave audio feature"),
                   QObject::tr("Audio"), LiveTargetKind::Enumeration, 0, 9,
                   static_cast<double>(render.audio_reactive.wave_source), [](pvt::RenderData& r, double v) { r.audio_reactive.wave_source = static_cast<pvt::MusicFeature>(std::llround(v)); });
        add_nested(QStringLiteral("audio.wave_amount"), QObject::tr("Wave response amount"),
                   QObject::tr("Audio"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.audio_reactive.wave_amount, [](pvt::RenderData& r, double v) { r.audio_reactive.wave_amount = v; });
        add_nested(QStringLiteral("audio.effects_enabled"), QObject::tr("Audio drives effects"),
                   QObject::tr("Audio"), LiveTargetKind::Boolean, 0, 1,
                   render.audio_reactive.effects_enabled, [](pvt::RenderData& r, double v) { r.audio_reactive.effects_enabled = v >= 0.5; });
        add_nested(QStringLiteral("audio.effect_source"), QObject::tr("Effect audio feature"),
                   QObject::tr("Audio"), LiveTargetKind::Enumeration, 0, 9,
                   static_cast<double>(render.audio_reactive.effect_source), [](pvt::RenderData& r, double v) { r.audio_reactive.effect_source = static_cast<pvt::MusicFeature>(std::llround(v)); });
        add_nested(QStringLiteral("audio.effect_amount"), QObject::tr("Effect response amount"),
                   QObject::tr("Audio"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.audio_reactive.effect_amount, [](pvt::RenderData& r, double v) { r.audio_reactive.effect_amount = v; });
        add_nested(QStringLiteral("audio.color_enabled"), QObject::tr("Audio drives color"),
                   QObject::tr("Audio"), LiveTargetKind::Boolean, 0, 1,
                   render.audio_reactive.color_enabled, [](pvt::RenderData& r, double v) { r.audio_reactive.color_enabled = v >= 0.5; });
        add_nested(QStringLiteral("audio.color_source"), QObject::tr("Color audio feature"),
                   QObject::tr("Audio"), LiveTargetKind::Enumeration, 0, 9,
                   static_cast<double>(render.audio_reactive.color_source), [](pvt::RenderData& r, double v) { r.audio_reactive.color_source = static_cast<pvt::MusicFeature>(std::llround(v)); });
        add_nested(QStringLiteral("audio.color_amount"), QObject::tr("Color response degrees"),
                   QObject::tr("Audio"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.audio_reactive.color_amount_degrees, [](pvt::RenderData& r, double v) { r.audio_reactive.color_amount_degrees = v; });
        add_nested(QStringLiteral("clock.enabled"), QObject::tr("Layer clock"),
                   QObject::tr("Clock"), LiveTargetKind::Boolean, 0, 1,
                   render.layer_clock.enabled, [](pvt::RenderData& r, double v) { r.layer_clock.enabled = v >= 0.5; });
        add_nested(QStringLiteral("clock.bpm"), QObject::tr("Layer tempo"),
                   QObject::tr("Clock"), LiveTargetKind::Real,
                   kMinimumPositiveUiValue, kMaximumRenderParameter,
                   render.layer_clock.clock.meter.bpm, [](pvt::RenderData& r, double v) { r.layer_clock.clock.meter.bpm = v; });
        add_nested(QStringLiteral("clock.phase"), QObject::tr("Layer clock phase"),
                   QObject::tr("Clock"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.layer_clock.clock.phase_offset_degrees, [](pvt::RenderData& r, double v) { r.layer_clock.clock.phase_offset_degrees = v; });
        add_nested(QStringLiteral("clock.reverse"), QObject::tr("Reverse layer clock"),
                   QObject::tr("Clock"), LiveTargetKind::Boolean, 0, 1,
                   render.layer_clock.clock.reverse, [](pvt::RenderData& r, double v) { r.layer_clock.clock.reverse = v >= 0.5; });
        add_nested(QStringLiteral("clock.scale"), QObject::tr("Layer clock fit"),
                   QObject::tr("Clock"), LiveTargetKind::Enumeration, 0, 4,
                   static_cast<double>(render.layer_clock.scale), [](pvt::RenderData& r, double v) { r.layer_clock.scale = static_cast<pvt::LayerClockScale>(std::llround(v)); });
        add_nested(QStringLiteral("clock.mix_enabled"), QObject::tr("Mix layer clock"),
                   QObject::tr("Clock"), LiveTargetKind::Boolean, 0, 1,
                   render.layer_clock.mix_enabled, [](pvt::RenderData& r, double v) { r.layer_clock.mix_enabled = v >= 0.5; });
        add_nested(QStringLiteral("clock.mix"), QObject::tr("Layer clock mix mode"),
                   QObject::tr("Clock"), LiveTargetKind::Enumeration, 0, 4,
                   static_cast<double>(render.layer_clock.mix), [](pvt::RenderData& r, double v) { r.layer_clock.mix = static_cast<pvt::LayerClockMixMode>(std::llround(v)); });
        add_nested(QStringLiteral("alpha.enabled"), QObject::tr("Procedural alpha"),
                   QObject::tr("Alpha"), LiveTargetKind::Boolean, 0, 1,
                   render.alpha.enabled, [](pvt::RenderData& r, double v) { r.alpha.enabled = v >= 0.5; });
        add_nested(QStringLiteral("alpha.minimum"), QObject::tr("Alpha minimum"),
                   QObject::tr("Alpha"), LiveTargetKind::Real, 0, 1,
                   render.alpha.minimum, [](pvt::RenderData& r, double v) {
                       r.alpha.minimum = v;
                       r.alpha.maximum = std::max(r.alpha.maximum, v);
                   });
        add_nested(QStringLiteral("alpha.maximum"), QObject::tr("Alpha maximum"),
                   QObject::tr("Alpha"), LiveTargetKind::Real, 0, 1,
                   render.alpha.maximum, [](pvt::RenderData& r, double v) {
                       r.alpha.maximum = v;
                       r.alpha.minimum = std::min(r.alpha.minimum, v);
                   });
        add_nested(QStringLiteral("alpha.frequency"), QObject::tr("Alpha frequency"),
                   QObject::tr("Alpha"), LiveTargetKind::Real, 0,
                   kMaximumRenderParameter,
                   render.alpha.spatial_frequency, [](pvt::RenderData& r, double v) { r.alpha.spatial_frequency = v; });
        add_nested(QStringLiteral("alpha.cycles"), QObject::tr("Alpha cycles"),
                   QObject::tr("Alpha"), LiveTargetKind::Integer,
                   kMinimumIntegerParameter, kMaximumIntegerParameter,
                   render.alpha.cycles_per_loop, [](pvt::RenderData& r, double v) { r.alpha.cycles_per_loop = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("alpha.phase"), QObject::tr("Alpha phase"),
                   QObject::tr("Alpha"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.alpha.phase_degrees, [](pvt::RenderData& r, double v) { r.alpha.phase_degrees = v; });
        add_nested(QStringLiteral("alpha.use_source"), QObject::tr("Use palette/image alpha"),
                   QObject::tr("Alpha"), LiveTargetKind::Boolean, 0, 1,
                   render.alpha.use_source_alpha, [](pvt::RenderData& r, double v) { r.alpha.use_source_alpha = v >= 0.5; });
        add_nested(QStringLiteral("quantization.enabled"), QObject::tr("Quantization"),
                   QObject::tr("Post Effects"), LiveTargetKind::Boolean, 0, 1,
                   render.quantization.enabled, [](pvt::RenderData& r, double v) { r.quantization.enabled = v >= 0.5; });
        add_nested(QStringLiteral("quantization.levels"), QObject::tr("Quantization levels"),
                   QObject::tr("Post Effects"), LiveTargetKind::Integer, 2,
                   kMaximumIntegerParameter,
                   render.quantization.levels, [](pvt::RenderData& r, double v) { r.quantization.levels = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("quantization.mix"), QObject::tr("Quantization mix"),
                   QObject::tr("Post Effects"), LiveTargetKind::Real, 0, 1,
                   render.quantization.mix, [](pvt::RenderData& r, double v) { r.quantization.mix = v; });
        add_nested(QStringLiteral("quantization.mode"), QObject::tr("Quantization mode"),
                   QObject::tr("Post Effects"), LiveTargetKind::Enumeration, 0, 2,
                   static_cast<double>(render.quantization.mode), [](pvt::RenderData& r, double v) { r.quantization.mode = static_cast<pvt::QuantizationMode>(std::llround(v)); });
        add_nested(QStringLiteral("post.invert_rgb"), QObject::tr("Invert colors"),
                   QObject::tr("Post Effects"), LiveTargetKind::Boolean, 0, 1,
                   render.post_process.invert_rgb_enabled, [](pvt::RenderData& r, double v) { r.post_process.invert_rgb_enabled = v >= 0.5; });
        add_nested(QStringLiteral("post.invert_rgb_mix"), QObject::tr("Invert color mix"),
                   QObject::tr("Post Effects"), LiveTargetKind::Real, 0, 1,
                   render.post_process.invert_rgb_mix, [](pvt::RenderData& r, double v) { r.post_process.invert_rgb_mix = v; });
        add_nested(QStringLiteral("post.invert_alpha"), QObject::tr("Invert alpha"),
                   QObject::tr("Post Effects"), LiveTargetKind::Boolean, 0, 1,
                   render.post_process.invert_alpha_enabled, [](pvt::RenderData& r, double v) { r.post_process.invert_alpha_enabled = v >= 0.5; });
        add_nested(QStringLiteral("post.invert_alpha_mix"), QObject::tr("Invert alpha mix"),
                   QObject::tr("Post Effects"), LiveTargetKind::Real, 0, 1,
                   render.post_process.invert_alpha_mix, [](pvt::RenderData& r, double v) { r.post_process.invert_alpha_mix = v; });
        add_nested(QStringLiteral("post.antialias"), QObject::tr("Edge antialias"),
                   QObject::tr("Post Effects"), LiveTargetKind::Boolean, 0, 1,
                   render.post_process.antialias_enabled, [](pvt::RenderData& r, double v) { r.post_process.antialias_enabled = v >= 0.5; });
        add_nested(QStringLiteral("post.antialias_strength"), QObject::tr("Antialias strength"),
                   QObject::tr("Post Effects"), LiveTargetKind::Real, 0, 1,
                   render.post_process.antialias_strength, [](pvt::RenderData& r, double v) { r.post_process.antialias_strength = v; });
        add_nested(QStringLiteral("post.antialias_threshold"), QObject::tr("Antialias threshold"),
                   QObject::tr("Post Effects"), LiveTargetKind::Real, 0, 1,
                   render.post_process.antialias_threshold, [](pvt::RenderData& r, double v) { r.post_process.antialias_threshold = v; });
        add_nested(QStringLiteral("post.antialias_passes"), QObject::tr("Antialias passes"),
                   QObject::tr("Post Effects"), LiveTargetKind::Integer, 1,
                   kMaximumIntegerParameter,
                   render.post_process.antialias_passes, [](pvt::RenderData& r, double v) { r.post_process.antialias_passes = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("motion.enabled"), QObject::tr("Layer motion"),
                   QObject::tr("Movement"), LiveTargetKind::Boolean, 0, 1,
                   render.motion.enabled, [](pvt::RenderData& r, double v) { r.motion.enabled = v >= 0.5; });
        add_nested(QStringLiteral("motion.center_x"), QObject::tr("Motion center X"),
                   QObject::tr("Movement"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.motion.center_x, [](pvt::RenderData& r, double v) { r.motion.center_x = v; });
        add_nested(QStringLiteral("motion.center_y"), QObject::tr("Motion center Y"),
                   QObject::tr("Movement"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.motion.center_y, [](pvt::RenderData& r, double v) { r.motion.center_y = v; });
        add_nested(QStringLiteral("motion.travel_x"), QObject::tr("Horizontal travel"),
                   QObject::tr("Movement"), LiveTargetKind::Real, 0,
                   kMaximumRenderParameter,
                   render.motion.travel_x, [](pvt::RenderData& r, double v) { r.motion.travel_x = v; });
        add_nested(QStringLiteral("motion.travel_y"), QObject::tr("Vertical travel"),
                   QObject::tr("Movement"), LiveTargetKind::Real, 0,
                   kMaximumRenderParameter,
                   render.motion.travel_y, [](pvt::RenderData& r, double v) { r.motion.travel_y = v; });
        add_nested(QStringLiteral("motion.phase"), QObject::tr("Motion phase"),
                   QObject::tr("Movement"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.motion.phase_degrees, [](pvt::RenderData& r, double v) { r.motion.phase_degrees = v; });
        add_nested(QStringLiteral("motion.scale"), QObject::tr("Motion scale pulse"),
                   QObject::tr("Movement"), LiveTargetKind::Real, 0,
                   kMaximumRenderParameter,
                   render.motion.scale_pulse, [](pvt::RenderData& r, double v) { r.motion.scale_pulse = v; });
        add_nested(QStringLiteral("motion.path"), QObject::tr("Motion path"),
                   QObject::tr("Movement"), LiveTargetKind::Enumeration, 0, 4,
                   static_cast<double>(render.motion.path), [](pvt::RenderData& r, double v) { r.motion.path = static_cast<pvt::LayerMotionPath>(std::llround(v)); });
        add_nested(QStringLiteral("motion.cycles_x"), QObject::tr("Motion X cycles"),
                   QObject::tr("Movement"), LiveTargetKind::Integer,
                   kMinimumIntegerParameter, kMaximumIntegerParameter,
                   render.motion.cycles_x, [](pvt::RenderData& r, double v) { r.motion.cycles_x = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("motion.cycles_y"), QObject::tr("Motion Y cycles"),
                   QObject::tr("Movement"), LiveTargetKind::Integer,
                   kMinimumIntegerParameter, kMaximumIntegerParameter,
                   render.motion.cycles_y, [](pvt::RenderData& r, double v) { r.motion.cycles_y = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("motion.rotations"), QObject::tr("Motion rotations"),
                   QObject::tr("Movement"), LiveTargetKind::Integer,
                   kMinimumIntegerParameter, kMaximumIntegerParameter,
                   render.motion.rotations_per_loop, [](pvt::RenderData& r, double v) { r.motion.rotations_per_loop = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("motion.rotation_offset"), QObject::tr("Motion rotation offset"),
                   QObject::tr("Movement"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.motion.rotation_offset_degrees, [](pvt::RenderData& r, double v) { r.motion.rotation_offset_degrees = v; });
        add_nested(QStringLiteral("surface.enabled"), QObject::tr("Surface mapping"),
                   QObject::tr("Surface"), LiveTargetKind::Boolean, 0, 1,
                   render.surface.enabled, [](pvt::RenderData& r, double v) { r.surface.enabled = v >= 0.5; });
        add_nested(QStringLiteral("surface.curvature"), QObject::tr("Surface curvature"),
                   QObject::tr("Surface"), LiveTargetKind::Real, 0, 1,
                   render.surface.curvature, [](pvt::RenderData& r, double v) { r.surface.curvature = v; });
        add_nested(QStringLiteral("surface.lighting"), QObject::tr("Surface lighting"),
                   QObject::tr("Surface"), LiveTargetKind::Real, 0,
                   kMaximumRenderParameter,
                   render.surface.lighting, [](pvt::RenderData& r, double v) { r.surface.lighting = v; });
        add_nested(QStringLiteral("surface.mapping"), QObject::tr("Surface type"),
                   QObject::tr("Surface"), LiveTargetKind::Enumeration, 0,
                   render.surface.obj_path.empty()
                           && render.surface.obj_sha256.empty() ? 3 : 4,
                   static_cast<double>(render.surface.mapping), [](pvt::RenderData& r, double v) {
                       r.surface.mapping = static_cast<pvt::SurfaceMapping>(
                           std::llround(v));
                       if (r.surface.mapping != pvt::SurfaceMapping::Plane) {
                           r.surface.plane_displacement.enabled = false;
                       }
                   });
        add_nested(QStringLiteral("surface.projection"), QObject::tr("Surface projection"),
                   QObject::tr("Surface"), LiveTargetKind::Enumeration, 0, 1,
                   static_cast<double>(render.surface.projection), [](pvt::RenderData& r, double v) { r.surface.projection = static_cast<pvt::SurfaceProjection>(std::llround(v)); });
        add_nested(QStringLiteral("surface.sizing"), QObject::tr("Surface fit policy"),
                   QObject::tr("Surface"), LiveTargetKind::Enumeration, 0, 3,
                   static_cast<double>(render.surface.sizing), [](pvt::RenderData& r, double v) { r.surface.sizing = static_cast<pvt::SurfaceSizing>(std::llround(v)); });
        add_nested(QStringLiteral("surface.outside"), QObject::tr("Surface outside pixels"),
                   QObject::tr("Surface"), LiveTargetKind::Enumeration, 0, 2,
                   static_cast<double>(render.surface.outside), [](pvt::RenderData& r, double v) { r.surface.outside = static_cast<pvt::SurfaceOutside>(std::llround(v)); });
        add_nested(QStringLiteral("surface.rotation_order"), QObject::tr("Surface rotation order"),
                   QObject::tr("Surface"), LiveTargetKind::Enumeration, 0, 5,
                   static_cast<double>(render.surface.rotation_order), [](pvt::RenderData& r, double v) { r.surface.rotation_order = static_cast<pvt::SurfaceRotationOrder>(std::llround(v)); });
        add_nested(QStringLiteral("surface.rotation_x_turns"), QObject::tr("Surface X rotations"),
                   QObject::tr("Surface"), LiveTargetKind::Integer,
                   kMinimumIntegerParameter, kMaximumIntegerParameter,
                   render.surface.rotation_x_turns_per_loop, [](pvt::RenderData& r, double v) { r.surface.rotation_x_turns_per_loop = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("surface.rotation_y_turns"), QObject::tr("Surface Y rotations"),
                   QObject::tr("Surface"), LiveTargetKind::Integer,
                   kMinimumIntegerParameter, kMaximumIntegerParameter,
                   render.surface.rotation_y_turns_per_loop, [](pvt::RenderData& r, double v) { r.surface.rotation_y_turns_per_loop = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("surface.rotation_z_turns"), QObject::tr("Surface Z rotations"),
                   QObject::tr("Surface"), LiveTargetKind::Integer,
                   kMinimumIntegerParameter, kMaximumIntegerParameter,
                   render.surface.rotation_z_turns_per_loop, [](pvt::RenderData& r, double v) { r.surface.rotation_z_turns_per_loop = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("surface.rotation_x"), QObject::tr("Surface X angle"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.surface.rotation_x_degrees, [](pvt::RenderData& r, double v) { r.surface.rotation_x_degrees = v; });
        add_nested(QStringLiteral("surface.rotation_y"), QObject::tr("Surface Y angle"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.surface.rotation_y_degrees, [](pvt::RenderData& r, double v) { r.surface.rotation_y_degrees = v; });
        add_nested(QStringLiteral("surface.rotation_z"), QObject::tr("Surface Z angle"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.surface.rotation_z_degrees, [](pvt::RenderData& r, double v) { r.surface.rotation_z_degrees = v; });
        add_nested(QStringLiteral("surface.size"), QObject::tr("Surface visible size"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   kMinimumPositiveUiValue, kMaximumRenderParameter,
                   render.surface.size_percent, [](pvt::RenderData& r, double v) { r.surface.size_percent = v; });
        add_nested(QStringLiteral("surface.scale_x"), QObject::tr("Surface X scale"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   kMinimumPositiveUiValue, kMaximumRenderParameter,
                   render.surface.scale_x, [](pvt::RenderData& r, double v) { r.surface.scale_x = v; });
        add_nested(QStringLiteral("surface.scale_y"), QObject::tr("Surface Y scale"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   kMinimumPositiveUiValue, kMaximumRenderParameter,
                   render.surface.scale_y, [](pvt::RenderData& r, double v) { r.surface.scale_y = v; });
        add_nested(QStringLiteral("surface.scale_z"), QObject::tr("Surface Z scale"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   kMinimumPositiveUiValue, kMaximumRenderParameter,
                   render.surface.scale_z, [](pvt::RenderData& r, double v) { r.surface.scale_z = v; });
        add_nested(QStringLiteral("surface.position_x"), QObject::tr("Surface X position"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.surface.position_x_percent, [](pvt::RenderData& r, double v) { r.surface.position_x_percent = v; });
        add_nested(QStringLiteral("surface.position_y"), QObject::tr("Surface Y position"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.surface.position_y_percent, [](pvt::RenderData& r, double v) { r.surface.position_y_percent = v; });
        add_nested(QStringLiteral("surface.position_z"), QObject::tr("Surface Z position"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.surface.position_z, [](pvt::RenderData& r, double v) { r.surface.position_z = v; });
        add_nested(QStringLiteral("surface.camera_distance"), QObject::tr("Surface camera distance"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   kMinimumPositiveUiValue, kMaximumRenderParameter,
                   render.surface.camera_distance, [](pvt::RenderData& r, double v) { r.surface.camera_distance = v; });
        add_nested(QStringLiteral("surface.focal_length"), QObject::tr("Surface focal length"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   kMinimumPositiveUiValue, kMaximumRenderParameter,
                   render.surface.focal_length, [](pvt::RenderData& r, double v) { r.surface.focal_length = v; });
        add_nested(QStringLiteral("surface.light_x"), QObject::tr("Surface light X"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.surface.light_direction_x, [](pvt::RenderData& r, double v) { r.surface.light_direction_x = v; });
        add_nested(QStringLiteral("surface.light_y"), QObject::tr("Surface light Y"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.surface.light_direction_y, [](pvt::RenderData& r, double v) { r.surface.light_direction_y = v; });
        add_nested(QStringLiteral("surface.light_z"), QObject::tr("Surface light Z"),
                   QObject::tr("Surface"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.surface.light_direction_z, [](pvt::RenderData& r, double v) { r.surface.light_direction_z = v; });
        add_nested(QStringLiteral("surface.light_ambient"), QObject::tr("Surface ambient light"),
                   QObject::tr("Surface"), LiveTargetKind::Real, 0,
                   kMaximumRenderParameter,
                   render.surface.light_ambient, [](pvt::RenderData& r, double v) { r.surface.light_ambient = v; });
        add_nested(QStringLiteral("surface.light_diffuse"), QObject::tr("Surface diffuse light"),
                   QObject::tr("Surface"), LiveTargetKind::Real, 0,
                   kMaximumRenderParameter,
                   render.surface.light_diffuse, [](pvt::RenderData& r, double v) { r.surface.light_diffuse = v; });
        add_nested(QStringLiteral("surface.composite_backfaces"), QObject::tr("Surface rear compositing"),
                   QObject::tr("Surface"), LiveTargetKind::Boolean, 0, 1,
                   render.surface.composite_backfaces, [](pvt::RenderData& r, double v) { r.surface.composite_backfaces = v >= 0.5; });
        add_nested(QStringLiteral("surface.normalize_obj"), QObject::tr("Normalize surface OBJ"),
                   QObject::tr("Surface"), LiveTargetKind::Boolean, 0, 1,
                   render.surface.normalize_obj, [](pvt::RenderData& r, double v) { r.surface.normalize_obj = v >= 0.5; });
        if (!render.surface.plane_displacement.path.empty()
            || !render.surface.plane_displacement.sha256.empty()) {
            add_nested(
                QStringLiteral("surface.plane_displacement.enabled"),
                QObject::tr("Plane displacement"), QObject::tr("Surface"),
                LiveTargetKind::Boolean, 0, 1,
                render.surface.plane_displacement.enabled,
                [](pvt::RenderData& r, double v) {
                    const bool enabled = v >= 0.5;
                    r.surface.plane_displacement.enabled = enabled;
                    if (enabled) {
                        r.surface.enabled = true;
                        r.surface.mapping = pvt::SurfaceMapping::Plane;
                    }
                });
            add_nested(
                QStringLiteral("surface.plane_displacement.minimum"),
                QObject::tr("Plane minimum height"), QObject::tr("Surface"),
                LiveTargetKind::Real, -kMaximumRenderParameter,
                kMaximumRenderParameter,
                render.surface.plane_displacement.minimum,
                [](pvt::RenderData& r, double v) {
                    r.surface.plane_displacement.minimum = v;
                    r.surface.plane_displacement.maximum = std::max(
                        r.surface.plane_displacement.maximum, v);
                });
            add_nested(
                QStringLiteral("surface.plane_displacement.maximum"),
                QObject::tr("Plane maximum height"), QObject::tr("Surface"),
                LiveTargetKind::Real, -kMaximumRenderParameter,
                kMaximumRenderParameter,
                render.surface.plane_displacement.maximum,
                [](pvt::RenderData& r, double v) {
                    r.surface.plane_displacement.maximum = v;
                    r.surface.plane_displacement.minimum = std::min(
                        r.surface.plane_displacement.minimum, v);
                });
            add_nested(
                QStringLiteral("surface.plane_displacement.midpoint"),
                QObject::tr("Plane neutral midpoint"), QObject::tr("Surface"),
                LiveTargetKind::Real, 0, 1,
                render.surface.plane_displacement.midpoint,
                [](pvt::RenderData& r, double v) {
                    r.surface.plane_displacement.midpoint = v;
                });
        }
        add_nested(QStringLiteral("transform.flip_horizontal"), QObject::tr("Flip horizontal"),
                   QObject::tr("Modifiers"), LiveTargetKind::Boolean, 0, 1,
                   render.transform.flip_horizontal, [](pvt::RenderData& r, double v) { r.transform.flip_horizontal = v >= 0.5; });
        add_nested(QStringLiteral("transform.flip_vertical"), QObject::tr("Flip vertical"),
                   QObject::tr("Modifiers"), LiveTargetKind::Boolean, 0, 1,
                   render.transform.flip_vertical, [](pvt::RenderData& r, double v) { r.transform.flip_vertical = v >= 0.5; });
        add_nested(QStringLiteral("transform.mirror"), QObject::tr("Mirror mode"),
                   QObject::tr("Modifiers"), LiveTargetKind::Enumeration, 0, 5,
                   static_cast<double>(render.transform.mirror), [](pvt::RenderData& r, double v) { r.transform.mirror = static_cast<pvt::MirrorMode>(std::llround(v)); });
        if (!render.palette.colors.empty()) {
            add_nested(QStringLiteral("palette.enabled"), QObject::tr("Starting palette"),
                       QObject::tr("Starting Colors"), LiveTargetKind::Boolean, 0, 1,
                       render.palette.enabled, [](pvt::RenderData& r, double v) { r.palette.enabled = v >= 0.5; });
        }
        if (!render.starting_image.path.empty()
            || !render.starting_image.sha256.empty()) {
            add_nested(QStringLiteral("starting_image.enabled"), QObject::tr("Starting image"),
                       QObject::tr("Starting Colors"), LiveTargetKind::Boolean, 0, 1,
                       render.starting_image.enabled, [](pvt::RenderData& r, double v) { r.starting_image.enabled = v >= 0.5; });
        }
        add_nested(QStringLiteral("starting_image.fit"), QObject::tr("Starting image fit"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Enumeration, 0, 3,
                   static_cast<double>(render.starting_image.fit), [](pvt::RenderData& r, double v) { r.starting_image.fit = static_cast<pvt::StartingImageFit>(std::llround(v)); });
        add_nested(QStringLiteral("starting.mode"), QObject::tr("Generated color pattern"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Enumeration, 0, 6,
                   static_cast<double>(render.starting_colors.mode), [](pvt::RenderData& r, double v) { r.starting_colors.mode = static_cast<pvt::StartingColorMode>(std::llround(v)); });
        add_nested(QStringLiteral("starting.include_alpha"), QObject::tr("Generate alpha"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Boolean, 0, 1,
                   render.starting_colors.include_alpha, [](pvt::RenderData& r, double v) { r.starting_colors.include_alpha = v >= 0.5; });
        const auto add_starting_range = [&](const QString& channel,
                                            const QString& label,
                                            bool maximum) {
            const auto current = [&](const pvt::StartingColorConfig& colors) {
                if (channel == QStringLiteral("red")) return maximum ? colors.red_maximum : colors.red_minimum;
                if (channel == QStringLiteral("green")) return maximum ? colors.green_maximum : colors.green_minimum;
                if (channel == QStringLiteral("blue")) return maximum ? colors.blue_maximum : colors.blue_minimum;
                return maximum ? colors.alpha_maximum : colors.alpha_minimum;
            }(render.starting_colors);
            add_nested(QStringLiteral("starting.%1_%2")
                           .arg(channel, maximum ? QStringLiteral("maximum")
                                                 : QStringLiteral("minimum")),
                       label, QObject::tr("Starting Colors"),
                       LiveTargetKind::Real, 0, 1, current,
                       [channel, maximum](pvt::RenderData& r, double v) {
                           double* low = nullptr;
                           double* high = nullptr;
                           if (channel == QStringLiteral("red")) {
                               low = &r.starting_colors.red_minimum; high = &r.starting_colors.red_maximum;
                           } else if (channel == QStringLiteral("green")) {
                               low = &r.starting_colors.green_minimum; high = &r.starting_colors.green_maximum;
                           } else if (channel == QStringLiteral("blue")) {
                               low = &r.starting_colors.blue_minimum; high = &r.starting_colors.blue_maximum;
                           } else {
                               low = &r.starting_colors.alpha_minimum; high = &r.starting_colors.alpha_maximum;
                           }
                           if (maximum) {
                               *high = v; *low = std::min(*low, v);
                           } else {
                               *low = v; *high = std::max(*high, v);
                           }
                       });
        };
        add_starting_range(QStringLiteral("red"), QObject::tr("Red minimum"), false);
        add_starting_range(QStringLiteral("red"), QObject::tr("Red maximum"), true);
        add_starting_range(QStringLiteral("green"), QObject::tr("Green minimum"), false);
        add_starting_range(QStringLiteral("green"), QObject::tr("Green maximum"), true);
        add_starting_range(QStringLiteral("blue"), QObject::tr("Blue minimum"), false);
        add_starting_range(QStringLiteral("blue"), QObject::tr("Blue maximum"), true);
        add_starting_range(QStringLiteral("alpha"), QObject::tr("Alpha minimum"), false);
        add_starting_range(QStringLiteral("alpha"), QObject::tr("Alpha maximum"), true);
        add_nested(QStringLiteral("starting.kaleidoscope.enabled"), QObject::tr("Kaleidoscope"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Boolean, 0, 1,
                   render.starting_colors.kaleidoscope.enabled, [](pvt::RenderData& r, double v) { r.starting_colors.kaleidoscope.enabled = v >= 0.5; });
        add_nested(QStringLiteral("starting.kaleidoscope.segments"), QObject::tr("Kaleidoscope segments"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Integer, 2,
                   kMaximumIntegerParameter,
                   render.starting_colors.kaleidoscope.mirrored_segments, [](pvt::RenderData& r, double v) { r.starting_colors.kaleidoscope.mirrored_segments = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("starting.kaleidoscope.rotation"), QObject::tr("Kaleidoscope rotation"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Real,
                   -kMaximumRenderParameter, kMaximumRenderParameter,
                   render.starting_colors.kaleidoscope.rotation_degrees, [](pvt::RenderData& r, double v) { r.starting_colors.kaleidoscope.rotation_degrees = v; });
        add_nested(QStringLiteral("starting.kaleidoscope.mix"), QObject::tr("Kaleidoscope mix"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Real, 0, 1,
                   render.starting_colors.kaleidoscope.mix, [](pvt::RenderData& r, double v) { r.starting_colors.kaleidoscope.mix = v; });
        add_nested(QStringLiteral("starting.warp.enabled"), QObject::tr("Domain warp"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Boolean, 0, 1,
                   render.starting_colors.domain_warp.enabled, [](pvt::RenderData& r, double v) { r.starting_colors.domain_warp.enabled = v >= 0.5; });
        add_nested(QStringLiteral("starting.warp.strength"), QObject::tr("Domain warp strength"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Real, 0,
                   kMaximumRenderParameter,
                   render.starting_colors.domain_warp.strength, [](pvt::RenderData& r, double v) { r.starting_colors.domain_warp.strength = v; });
        add_nested(QStringLiteral("starting.warp.scale"), QObject::tr("Domain warp scale"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Real,
                   kMinimumPositiveUiValue, kMaximumRenderParameter,
                   render.starting_colors.domain_warp.scale, [](pvt::RenderData& r, double v) { r.starting_colors.domain_warp.scale = v; });
        add_nested(QStringLiteral("starting.warp.octaves"), QObject::tr("Domain warp octaves"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Integer, 1,
                   kMaximumIntegerParameter,
                   render.starting_colors.domain_warp.octaves, [](pvt::RenderData& r, double v) { r.starting_colors.domain_warp.octaves = static_cast<int>(std::llround(v)); });
        add_nested(QStringLiteral("starting.warp.cycles"), QObject::tr("Domain warp cycles"),
                   QObject::tr("Starting Colors"), LiveTargetKind::Integer,
                   kMinimumIntegerParameter, kMaximumIntegerParameter,
                   render.starting_colors.domain_warp.cycles_per_loop, [](pvt::RenderData& r, double v) { r.starting_colors.domain_warp.cycles_per_loop = static_cast<int>(std::llround(v)); });

        for (const pvt::WaveConfig& wave : render.waves) {
            const std::uint64_t id = wave.id;
            const QString item_prefix = prefix + QStringLiteral("wave/%1/").arg(id);
            const QString section = layer_section(
                authored_layer, QObject::tr("Wave — %1")
                                    .arg(QString::fromStdString(wave.name)));
            const auto add_wave = [&](const QString& key, const QString& label,
                                      LiveTargetKind kind, double minimum,
                                      double maximum, double current, auto setter) {
                append(item_prefix + key, label, section, kind, minimum, maximum,
                       current, [uuid, id, setter, minimum, maximum](
                                    pvt::ProjectConfig& value, double input) {
                           pvt::LayerConfig* layer = find_layer(value, uuid);
                           if (layer == nullptr) return false;
                           pvt::WaveConfig* item = find_wave(*layer, id);
                           if (item == nullptr) return false;
                           setter(*item, std::clamp(input, minimum, maximum));
                           return true;
                       });
            };
            add_wave(QStringLiteral("enabled"), QObject::tr("Enabled"), LiveTargetKind::Boolean, 0, 1, wave.enabled, [](pvt::WaveConfig& w, double v) { w.enabled = v >= 0.5; });
            add_wave(QStringLiteral("synchronized"), QObject::tr("Use master clock"), LiveTargetKind::Boolean, 0, 1, wave.synchronized, [](pvt::WaveConfig& w, double v) { w.synchronized = v >= 0.5; });
            add_wave(QStringLiteral("audio_response"), QObject::tr("Audio response"), LiveTargetKind::Enumeration, 0, 12, static_cast<double>(wave.audio_response), [](pvt::WaveConfig& w, double v) { w.audio_response = static_cast<pvt::AudioResponseMode>(std::llround(v)); });
            add_wave(QStringLiteral("amplitude"), QObject::tr("Amplitude"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, wave.amplitude, [](pvt::WaveConfig& w, double v) { w.amplitude = v; });
            add_wave(QStringLiteral("frequency"), QObject::tr("Spatial frequency"), LiveTargetKind::Real, 0, kMaximumRenderParameter, wave.spatial_frequency, [](pvt::WaveConfig& w, double v) { w.spatial_frequency = v; });
            add_wave(QStringLiteral("cycles"), QObject::tr("Cycles per loop"), LiveTargetKind::Integer, kMinimumIntegerParameter, kMaximumIntegerParameter, wave.cycles_per_loop, [](pvt::WaveConfig& w, double v) { w.cycles_per_loop = static_cast<int>(std::llround(v)); });
            add_wave(QStringLiteral("phase"), QObject::tr("Phase"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, wave.phase_degrees, [](pvt::WaveConfig& w, double v) { w.phase_degrees = v; });
            add_wave(QStringLiteral("direction"), QObject::tr("Direction"), LiveTargetKind::Real, 0, 1, wave.direction, [](pvt::WaveConfig& w, double v) { w.direction = v; });
            add_wave(QStringLiteral("x"), QObject::tr("Center X"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, wave.x_percent, [](pvt::WaveConfig& w, double v) { w.x_percent = v; });
            add_wave(QStringLiteral("y"), QObject::tr("Center Y"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, wave.y_percent, [](pvt::WaveConfig& w, double v) { w.y_percent = v; });
        }
        for (const pvt::SwingConfig& swing : render.swings) {
            const std::uint64_t id = swing.id;
            const QString item_prefix = prefix + QStringLiteral("swing/%1/").arg(id);
            const QString section = layer_section(
                authored_layer, QObject::tr("Swing — %1")
                                    .arg(QString::fromStdString(swing.name)));
            const auto add_swing = [&](const QString& key, const QString& label,
                                       LiveTargetKind kind, double minimum,
                                       double maximum, double current, auto setter) {
                append(item_prefix + key, label, section, kind, minimum, maximum,
                       current, [uuid, id, setter, minimum, maximum](
                                    pvt::ProjectConfig& value, double input) {
                           pvt::LayerConfig* layer = find_layer(value, uuid);
                           if (layer == nullptr) return false;
                           pvt::SwingConfig* item = find_swing(*layer, id);
                           if (item == nullptr) return false;
                           setter(*item, std::clamp(input, minimum, maximum));
                           return true;
                       });
            };
            add_swing(QStringLiteral("enabled"), QObject::tr("Enabled"), LiveTargetKind::Boolean, 0, 1, swing.enabled, [](pvt::SwingConfig& s, double v) { s.enabled = v >= 0.5; });
            add_swing(QStringLiteral("waveform"), QObject::tr("Waveform"), LiveTargetKind::Enumeration, 0, 3, static_cast<double>(swing.waveform), [](pvt::SwingConfig& s, double v) { s.waveform = static_cast<pvt::Waveform>(std::llround(v)); });
            add_swing(QStringLiteral("amount"), QObject::tr("Amount"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, swing.amount, [](pvt::SwingConfig& s, double v) { s.amount = v; });
            add_swing(QStringLiteral("cycles"), QObject::tr("Cycles per loop"), LiveTargetKind::Integer, kMinimumIntegerParameter, kMaximumIntegerParameter, swing.cycles_per_loop, [](pvt::SwingConfig& s, double v) { s.cycles_per_loop = static_cast<int>(std::llround(v)); });
            add_swing(QStringLiteral("phase"), QObject::tr("Phase"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, swing.phase_degrees, [](pvt::SwingConfig& s, double v) { s.phase_degrees = v; });
            add_swing(QStringLiteral("shape"), QObject::tr("Shape"), LiveTargetKind::Real, 0, 1, swing.shape, [](pvt::SwingConfig& s, double v) { s.shape = v; });
            add_swing(QStringLiteral("center_x"), QObject::tr("Center X"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, swing.center_x, [](pvt::SwingConfig& s, double v) { s.center_x = v; });
            add_swing(QStringLiteral("center_y"), QObject::tr("Center Y"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, swing.center_y, [](pvt::SwingConfig& s, double v) { s.center_y = v; });
            add_swing(QStringLiteral("radius"), QObject::tr("Local radius"), LiveTargetKind::Real, 0, kMaximumRenderParameter, swing.radius, [](pvt::SwingConfig& s, double v) { s.radius = v; });
        }
        for (const pvt::EffectConfig& effect : render.effects) {
            const std::uint64_t id = effect.id;
            const QString item_prefix = prefix + QStringLiteral("effect/%1/").arg(id);
            const QString section = layer_section(
                authored_layer, QObject::tr("Effect — %1")
                                    .arg(QString::fromStdString(effect.name)));
            const bool block_scale = effect.type == pvt::EffectType::BlockScale;
            const bool particles = effect.type == pvt::EffectType::ParticleField;
            const bool glitch = effect.type == pvt::EffectType::Glitch;
            const bool starburst = effect.type == pvt::EffectType::Starburst;
            const bool lens = effect.type == pvt::EffectType::LensDistortion;
            const bool normalized_intensity =
                block_scale || glitch || starburst || lens;
            const double frequency_minimum = block_scale
                ? std::max(kMinimumPositiveUiValue, effect.magnitude)
                : (particles || glitch || starburst ? 1.0
                                                     : (lens ? 0.25 : 0.0));
            const double frequency_maximum = particles || glitch
                ? kMaximumIntegerParameter : kMaximumRenderParameter;
            const double secondary_minimum = block_scale || particles
                                                     || glitch || starburst
                ? 0.0 : (lens ? -1.0 : -kMaximumRenderParameter);
            const double secondary_maximum = block_scale
                ? kMaximumIntegerParameter
                : (particles || glitch || starburst || lens
                       ? 1.0 : kMaximumRenderParameter);
            const auto add_effect = [&](const QString& key, const QString& label,
                                        LiveTargetKind kind, double minimum,
                                        double maximum, double current, auto setter) {
                append(item_prefix + key, label, section, kind, minimum, maximum,
                       current, [uuid, id, setter, minimum, maximum](
                                    pvt::ProjectConfig& value, double input) {
                           pvt::LayerConfig* layer = find_layer(value, uuid);
                           if (layer == nullptr) return false;
                           pvt::EffectConfig* item = find_effect(*layer, id);
                           if (item == nullptr) return false;
                           setter(*item, std::clamp(input, minimum, maximum));
                           return true;
                       });
            };
            add_effect(QStringLiteral("enabled"), QObject::tr("Enabled"), LiveTargetKind::Boolean, 0, 1, effect.enabled, [](pvt::EffectConfig& e, double v) { e.enabled = v >= 0.5; });
            add_effect(QStringLiteral("type"), QObject::tr("Effect type"), LiveTargetKind::Enumeration, 0, 12, static_cast<double>(effect.type), [](pvt::EffectConfig& e, double v) { e.type = static_cast<pvt::EffectType>(std::llround(v)); });
            add_effect(QStringLiteral("space"), QObject::tr("Effect space"), LiveTargetKind::Enumeration, 0, 1, static_cast<double>(effect.space), [](pvt::EffectConfig& e, double v) { e.space = static_cast<pvt::EffectSpace>(std::llround(v)); });
            add_effect(QStringLiteral("synchronized"), QObject::tr("Use master clock"), LiveTargetKind::Boolean, 0, 1, effect.synchronized, [](pvt::EffectConfig& e, double v) { e.synchronized = v >= 0.5; });
            add_effect(QStringLiteral("edge_mode"), QObject::tr("Edge mode"), LiveTargetKind::Enumeration, 0, 3, static_cast<double>(effect.edge_mode), [](pvt::EffectConfig& e, double v) { e.edge_mode = static_cast<pvt::EdgeMode>(std::llround(v)); });
            add_effect(QStringLiteral("audio_response"), QObject::tr("Audio response"), LiveTargetKind::Enumeration, 0, 12, static_cast<double>(effect.audio_response), [](pvt::EffectConfig& e, double v) { e.audio_response = static_cast<pvt::AudioResponseMode>(std::llround(v)); });
            add_effect(QStringLiteral("intensity"), QObject::tr("Intensity"), LiveTargetKind::Real, 0, normalized_intensity ? 1.0 : kMaximumRenderParameter, effect.intensity, [](pvt::EffectConfig& e, double v) { e.intensity = v; });
            add_effect(QStringLiteral("magnitude"), QObject::tr("Magnitude"), LiveTargetKind::Real, block_scale ? kMinimumPositiveUiValue : 0.0, kMaximumRenderParameter, effect.magnitude, [](pvt::EffectConfig& e, double v) { e.magnitude = v; });
            add_effect(QStringLiteral("frequency"), particles ? QObject::tr("Particle count") : QObject::tr("Frequency"), particles ? LiveTargetKind::Integer : LiveTargetKind::Real, frequency_minimum, frequency_maximum, effect.frequency, [particles](pvt::EffectConfig& e, double v) { e.frequency = particles ? std::round(v) : v; });
            add_effect(QStringLiteral("secondary"), QObject::tr("Secondary"), LiveTargetKind::Real, secondary_minimum, secondary_maximum, effect.secondary, [](pvt::EffectConfig& e, double v) { e.secondary = v; });
            add_effect(QStringLiteral("center_x"), QObject::tr("Center X"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, effect.center_x, [](pvt::EffectConfig& e, double v) { e.center_x = v; });
            add_effect(QStringLiteral("center_y"), QObject::tr("Center Y"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, effect.center_y, [](pvt::EffectConfig& e, double v) { e.center_y = v; });
            add_effect(QStringLiteral("angle"), QObject::tr("Angle"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, effect.angle_degrees, [](pvt::EffectConfig& e, double v) { e.angle_degrees = v; });
            add_effect(QStringLiteral("radius"), particles ? QObject::tr("Particle size (output-pixel radius)") : QObject::tr("Radius"), LiveTargetKind::Real, particles ? kMinimumPositiveUiValue : 0.0, kMaximumRenderParameter, effect.radius_pixels, [](pvt::EffectConfig& e, double v) { e.radius_pixels = v; });
            add_effect(QStringLiteral("threshold"), QObject::tr("Threshold"), LiveTargetKind::Real, 0, particles ? 1.0 : kMaximumRenderParameter, effect.threshold, [](pvt::EffectConfig& e, double v) { e.threshold = v; });
            add_effect(QStringLiteral("soft_knee"), QObject::tr("Soft knee"), LiveTargetKind::Real, 0, 1, effect.soft_knee, [](pvt::EffectConfig& e, double v) { e.soft_knee = v; });
            add_effect(QStringLiteral("area"), QObject::tr("Local area"), LiveTargetKind::Real, 0, kMaximumRenderParameter, effect.area_radius, [](pvt::EffectConfig& e, double v) { e.area_radius = v; });
            add_effect(QStringLiteral("cycles"), QObject::tr("Cycles per loop"), LiveTargetKind::Integer, kMinimumIntegerParameter, kMaximumIntegerParameter, effect.cycles_per_loop, [](pvt::EffectConfig& e, double v) { e.cycles_per_loop = static_cast<int>(std::llround(v)); });
            add_effect(QStringLiteral("phase"), QObject::tr("Phase"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, effect.phase_degrees, [](pvt::EffectConfig& e, double v) { e.phase_degrees = v; });
            if (particles) {
                add_effect(QStringLiteral("particle_shape"), QObject::tr("Particle shape"), LiveTargetKind::Enumeration, 0, 4, static_cast<double>(effect.particle_shape), [](pvt::EffectConfig& e, double v) { e.particle_shape = static_cast<pvt::ParticleShape>(std::llround(v)); });
                add_effect(QStringLiteral("particle_profile"), QObject::tr("Particle render profile"), LiveTargetKind::Enumeration, 0, 1, static_cast<double>(effect.particle_profile), [](pvt::EffectConfig& e, double v) { e.particle_profile = static_cast<pvt::ParticleRenderProfile>(std::llround(v)); });
                add_effect(QStringLiteral("particle_size_variation"), QObject::tr("Particle size variation"), LiveTargetKind::Real, 0, 1, effect.particle_size_variation, [](pvt::EffectConfig& e, double v) { e.particle_size_variation = v; });
                add_effect(QStringLiteral("particle_definition"), QObject::tr("Shape definition"), LiveTargetKind::Real, 0, 1, effect.particle_definition, [](pvt::EffectConfig& e, double v) { e.particle_definition = v; });
                add_effect(QStringLiteral("particle_twinkle"), QObject::tr("Twinkle amount"), LiveTargetKind::Real, 0, 1, effect.particle_twinkle, [](pvt::EffectConfig& e, double v) { e.particle_twinkle = v; });
                add_effect(QStringLiteral("particle_orientation"), QObject::tr("Particle orientation"), LiveTargetKind::Enumeration, 0, 2, static_cast<double>(effect.particle_orientation), [](pvt::EffectConfig& e, double v) { e.particle_orientation = static_cast<pvt::ParticleOrientation>(std::llround(v)); });
                add_effect(QStringLiteral("particle_rotation"), QObject::tr("Particle rotation"), LiveTargetKind::Real, -kMaximumRenderParameter, kMaximumRenderParameter, effect.particle_rotation_degrees, [](pvt::EffectConfig& e, double v) { e.particle_rotation_degrees = v; });
            }
            add_effect(QStringLiteral("blur_type"), QObject::tr("Blur type"), LiveTargetKind::Enumeration, 0, 4, static_cast<double>(effect.blur_type), [](pvt::EffectConfig& e, double v) { e.blur_type = static_cast<pvt::BlurType>(std::llround(v)); });
            add_effect(QStringLiteral("blur_passes"), QObject::tr("Blur passes"), LiveTargetKind::Integer, 1, kMaximumIntegerParameter, effect.blur_passes, [](pvt::EffectConfig& e, double v) { e.blur_passes = static_cast<int>(std::llround(v)); });
            add_effect(QStringLiteral("blur_samples"), QObject::tr("Blur samples"), LiveTargetKind::Integer, 2, kMaximumIntegerParameter, effect.blur_samples, [](pvt::EffectConfig& e, double v) {
                int samples = static_cast<int>(std::llround(v));
                if (e.blur_type == pvt::BlurType::Gaussian && samples % 2 == 0) {
                    ++samples;
                }
                e.blur_samples = samples;
            });
            add_effect(QStringLiteral("blur_minimum"), QObject::tr("Blur minimum"), LiveTargetKind::Real, 0, 1, effect.blur_minimum, [](pvt::EffectConfig& e, double v) {
                e.blur_minimum = v;
                e.blur_maximum = std::max(e.blur_maximum, v);
            });
            add_effect(QStringLiteral("blur_maximum"), QObject::tr("Blur maximum"), LiveTargetKind::Real, 0, 1, effect.blur_maximum, [](pvt::EffectConfig& e, double v) {
                e.blur_maximum = v;
                e.blur_minimum = std::min(e.blur_minimum, v);
            });
        }
    }
    return result;
}
