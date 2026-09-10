#include "../gui/live_scene_morph.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << message << '\n'; }
    };
    auto project = pvt::default_project();
    const auto registry = buildLiveTargetRegistry(project);
    const QString layer_prefix = QStringLiteral("layer/%1/")
        .arg(QString::fromStdString(project.layers.front().uuid));
    const auto displacement_target = std::find_if(
        registry.begin(), registry.end(), [&layer_prefix](const auto& target) {
            return target.path
                   == layer_prefix + QStringLiteral("displacement_enabled");
        });
    check(displacement_target != registry.end(),
          "The layer displacement flag must be a LIVE target.");
    if (displacement_target != registry.end()) {
        const std::uint32_t other_flags =
            project.layers.front().render.flags
            & ~pvt::RenderData::DisplacementEnabledFlag;
        check(displacement_target->apply(project, 0.0)
                  && !project.layers.front().render.displacement_enabled
                  && (project.layers.front().render.flags
                      & ~pvt::RenderData::DisplacementEnabledFlag)
                         == other_flags,
              "LIVE must clear the packed flag directly without disturbing its bank.");
        check(displacement_target->apply(project, 1.0)
                  && project.layers.front().render.displacement_enabled
                  && (project.layers.front().render.flags
                      & pvt::RenderData::DisplacementEnabledFlag) != 0U,
              "LIVE must set the packed flag directly without a Boolean copy.");
    }
    pvt::LiveSceneConfig a;
    pvt::LiveSceneConfig b;
    using Type = pvt::LiveSceneValueType;
    a.values = {{"project.fps", Type::Real, "24"},
                {"project.clock.reverse", Type::Boolean, "false"},
                {"project.clock.bpm", Type::Real, "80"},
                {"missing/target", Type::Real, "0"}};
    b.values = {{"project.fps", Type::Real, "60"},
                {"project.clock.reverse", Type::Boolean, "true"},
                {"missing/target", Type::Real, "1"}};
    const auto morph = buildLiveSceneMorph(a, b, registry);
    check(morph.targets.size() == 2 && morph.skipped_targets == 2,
          "Only shared, resolved controls should morph.");
    const double original_bpm = project.canvas.clock.meter.bpm;
    for (double amount : {0.0, 0.25, 0.499, 0.5, 0.75, 1.0, 0.25, 0.0}) {
        auto result = project;
        applyLiveTargetValues(result, registry, morph.values(amount));
        check(std::fabs(result.canvas.fps - (24.0 + 36.0 * amount)) < 1e-10,
              "Scrubbing in either direction must interpolate from saved endpoints.");
        check(result.canvas.clock.reverse == (amount >= 0.5),
              "Switches must choose the nearest endpoint with a stable midpoint.");
        check(result.canvas.clock.meter.bpm == original_bpm,
              "Controls missing from one scene must stay untouched.");
    }
    check(a.values.front().value == "24" && b.values.front().value == "60",
          "Performance must not modify saved scenes.");
    check(morph.values(-1).value(QStringLiteral("project.fps")) == 24.0
          && morph.values(2).value(QStringLiteral("project.fps")) == 60.0,
          "Out-of-range positions must clamp to exact endpoints.");
    check(morph.values(std::numeric_limits<double>::quiet_NaN()).isEmpty(),
          "Non-finite positions must not reach the renderer.");
    b.values.front().value = "nan";
    check(buildLiveSceneMorph(a, b, registry).targets.size() == 1,
          "Non-finite scene values must be skipped.");
    b.values.front() = {"project.fps", Type::String, "60"};
    check(buildLiveSceneMorph(a, b, registry).targets.size() == 1,
          "Unsupported scene strings must not become numeric controls.");

    const auto count = std::find_if(registry.begin(), registry.end(),
        [](const LiveTargetDescriptor& target) { return target.kind == LiveTargetKind::Integer; });
    check(count != registry.end(), "The test project must expose a count control.");
    if (count != registry.end()) {
        a.values = {{count->path.toStdString(), Type::Integer, "1"}};
        b.values = {{count->path.toStdString(), Type::Integer, "5"}};
        const auto count_morph = buildLiveSceneMorph(a, b, registry);
        check(count_morph.values(0.499).value(count->path) == 1.0
              && count_morph.values(0.5).value(count->path) == 5.0,
              "Counts must switch endpoints without interpolating intermediate counts.");
    }

    LiveSceneMorph extremes;
    const double maximum = (std::numeric_limits<double>::max)();
    extremes.targets.insert(QStringLiteral("x"), {-maximum, maximum, false});
    check(extremes.values(0.5).value(QStringLiteral("x")) == 0.0,
          "Interpolation must avoid finite-endpoint subtraction overflow.");

    // Endpoint B changes the meaning and domain of several controls. Insertion
    // order and the authored effect type must not clip the saved B settings.
    auto& render = project.layers.front().render;
    render.effects.clear();
    auto effect = pvt::default_effect(pvt::EffectType::EdgeDetect);
    effect.id = pvt::allocate_id(render);
    render.effects.push_back(effect);
    const QString prefix = QStringLiteral("layer/%1/effect/%2/")
        .arg(QString::fromStdString(project.layers.front().uuid)).arg(effect.id);
    const auto effect_registry = buildLiveTargetRegistry(project);
    QHash<QString, double> values;
    values.insert(prefix + QStringLiteral("frequency"), 75.0);
    values.insert(prefix + QStringLiteral("intensity"), 8.0);
    values.insert(prefix + QStringLiteral("type"), static_cast<double>(pvt::EffectType::Ripple));
    applyLiveTargetValues(project, effect_registry, values);
    check(render.effects.front().type == pvt::EffectType::Ripple
          && render.effects.front().intensity == 8.0
          && render.effects.front().frequency == 75.0,
          "Apply modes before controls and resolve their new domains.");
    values.insert(prefix + QStringLiteral("type"), static_cast<double>(pvt::EffectType::ParticleField));
    values.insert(prefix + QStringLiteral("frequency"), 75.4);
    applyLiveTargetValues(project, effect_registry, values);
    check(render.effects.front().frequency == 75.0,
          "Type-dependent counts must retain the destination domain's rounding.");
    project.output.write_alpha = true;
    check(pvt::validate(project).ok, "Morphed effect settings must remain valid.");

    render.effects.front() = pvt::default_effect(pvt::EffectType::BlockScale);
    render.effects.front().id = effect.id;
    render.effects.front().magnitude = 10.0;
    render.effects.front().frequency = 12.0;
    const auto block_registry = buildLiveTargetRegistry(project);
    values.clear();
    values.insert(prefix + QStringLiteral("magnitude"), 2.0);
    values.insert(prefix + QStringLiteral("frequency"), 3.0);
    applyLiveTargetValues(project, block_registry, values);
    check(render.effects.front().magnitude == 2.0
          && render.effects.front().frequency == 3.0,
          "Dependent numeric ranges must follow the morphed magnitude, not cached UI limits.");

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "Live scene morph checks passed.\n";
    return EXIT_SUCCESS;
}
