#ifndef PVT_LIVE_SCENE_MORPH_H
#define PVT_LIVE_SCENE_MORPH_H

#include "live_target_registry.h"

#include <QHash>

struct LiveSceneMorph {
    struct Target {
        double a = 0.0;
        double b = 0.0;
        bool discrete = false;
    };
    QHash<QString, Target> targets;
    int skipped_targets = 0;

    QHash<QString, double> values(double amount) const;
};

// Only shared, finite, currently resolvable scalar settings participate.
// Scene identities and saved values remain untouched during performance.
LiveSceneMorph buildLiveSceneMorph(
    const pvt::LiveSceneConfig& a, const pvt::LiveSceneConfig& b,
    const std::vector<LiveTargetDescriptor>& registry);

// Apply modes before their dependent controls, in registry order. Re-resolve
// domains when a mode changes (e.g. switching a distortion to Particle Field).
void applyLiveTargetValues(pvt::ProjectConfig& project,
                          const std::vector<LiveTargetDescriptor>& registry,
                          const QHash<QString, double>& values);

#endif
