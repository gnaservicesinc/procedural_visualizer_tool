#ifndef PVT_LIVE_TARGET_REGISTRY_H
#define PVT_LIVE_TARGET_REGISTRY_H

#include "procedural_visualizer_tool.h"

#include <QString>

#include <functional>
#include <vector>

enum class LiveTargetKind {
    Boolean = 0,
    Integer,
    Real,
    Enumeration
};

struct LiveTargetDescriptor {
    QString path;
    QString label;
    QString section;
    LiveTargetKind kind = LiveTargetKind::Real;
    double minimum = 0.0;
    double maximum = 1.0;
    double current_value = 0.0;
    std::function<bool(pvt::ProjectConfig&, double)> apply;
};

// Returns every render-safe scalar that the Live overlay can modulate without
// touching editor widgets, undo history, project assets, filesystem paths, or
// export actions. UUID/ID paths stay stable across layer/effect reordering.
std::vector<LiveTargetDescriptor> buildLiveTargetRegistry(
    const pvt::ProjectConfig& project);

#endif
