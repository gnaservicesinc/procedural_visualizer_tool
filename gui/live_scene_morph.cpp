#include "live_scene_morph.h"

#include <QSet>

#include <algorithm>
#include <cmath>

namespace {

bool sceneNumber(const pvt::LiveSceneValue& value, double& number) {
    const QString text = QString::fromStdString(value.value).trimmed();
    if (value.type == pvt::LiveSceneValueType::String) return false;
    if (value.type == pvt::LiveSceneValueType::Boolean) {
        if (text == QStringLiteral("1")
            || text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) {
            number = 1.0;
            return true;
        }
        if (text == QStringLiteral("0")
            || text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) {
            number = 0.0;
            return true;
        }
        return false;
    }
    bool ok = false;
    number = text.toDouble(&ok);
    return ok && std::isfinite(number);
}

bool isMode(LiveTargetKind kind) {
    return kind == LiveTargetKind::Enumeration || kind == LiveTargetKind::Boolean;
}

} // namespace

QHash<QString, double> LiveSceneMorph::values(double amount) const {
    QHash<QString, double> result;
    if (!std::isfinite(amount)) return result;
    amount = std::clamp(amount, 0.0, 1.0);
    result.reserve(targets.size());
    for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
        const Target& target = it.value();
        // Weighted terms avoid overflowing b-a for opposite finite extremes.
        const double value = target.discrete ? (amount < 0.5 ? target.a : target.b)
            : amount == 0.0 ? target.a : amount == 1.0 ? target.b
            : (1.0 - amount) * target.a + amount * target.b;
        result.insert(it.key(), value);
    }
    return result;
}

LiveSceneMorph buildLiveSceneMorph(
    const pvt::LiveSceneConfig& a, const pvt::LiveSceneConfig& b,
    const std::vector<LiveTargetDescriptor>& registry) {
    LiveSceneMorph result;
    QHash<QString, const pvt::LiveSceneValue*> a_values;
    QSet<QString> paths;
    QSet<QString> resolved;
    for (const auto& target : registry) resolved.insert(target.path);
    for (const auto& value : a.values) {
        const QString path = QString::fromStdString(value.target_path);
        a_values.insert(path, &value);
        paths.insert(path);
    }
    for (const auto& value : b.values) {
        const QString path = QString::fromStdString(value.target_path);
        paths.insert(path);
        const auto found = a_values.constFind(path);
        if (found == a_values.cend() || !resolved.contains(path)) continue;
        LiveSceneMorph::Target target;
        if (!sceneNumber(**found, target.a) || !sceneNumber(value, target.b)) continue;
        target.discrete = (*found)->type != pvt::LiveSceneValueType::Real
            || value.type != pvt::LiveSceneValueType::Real;
        result.targets.insert(path, target);
    }
    result.skipped_targets = static_cast<int>(paths.size() - result.targets.size());
    return result;
}

void applyLiveTargetValues(pvt::ProjectConfig& project,
                          const std::vector<LiveTargetDescriptor>& registry,
                          const QHash<QString, double>& values) {
    if (values.isEmpty()) return;
    bool changed_mode = false;
    for (const auto& target : registry) {
        if (!isMode(target.kind)) continue;
        const auto found = values.constFind(target.path);
        if (found == values.cend() || !std::isfinite(*found)) continue;
        const double value = std::clamp(*found, target.minimum, target.maximum);
        changed_mode = changed_mode || value != target.current_value;
        (void)target.apply(project, value);
    }
    // A mode may expose additional controls or change a setting's range/kind.
    const auto resolved = changed_mode ? buildLiveTargetRegistry(project)
                                      : std::vector<LiveTargetDescriptor>{};
    const auto& controls = changed_mode ? resolved : registry;
    for (const auto& target : controls) {
        const auto found = values.constFind(target.path);
        if (found != values.cend() && std::isfinite(*found)) {
            (void)target.apply(project, isMode(target.kind)
                ? std::clamp(*found, target.minimum, target.maximum) : *found);
        }
    }
}
