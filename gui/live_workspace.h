#ifndef PVT_LIVE_WORKSPACE_H
#define PVT_LIVE_WORKSPACE_H

#include "procedural_visualizer_tool.h"

#include <QWidget>

#include <functional>
#include <memory>
#include <string>

// The Live workspace is deliberately a sibling of the authoring workspaces:
// it owns transient device bindings and performance state, while the editor
// remains the sole owner of project history and widgets.  The three callbacks
// are the only bridge needed by MainWindow.
class LiveWorkspace final : public QWidget {
    Q_OBJECT

public:
    using ProjectSnapshotProvider = std::function<pvt::ProjectConfig()>;
    using ActiveLayerUuidProvider = std::function<std::string()>;
    using AuthoredConfigEditor =
        std::function<void(const pvt::LiveConfig&, const QString& reason)>;

    explicit LiveWorkspace(ProjectSnapshotProvider projectProvider,
                           ActiveLayerUuidProvider activeLayerProvider,
                           AuthoredConfigEditor authoredConfigEditor,
                           QWidget* parent = nullptr);
    ~LiveWorkspace() override;

    // Call after project load/undo/redo or an active-layer change.  Neither
    // method starts hardware nor writes settings.
    void setProjectLiveConfig(const pvt::LiveConfig& config);
    void refreshProjectSnapshot();

    // Entering Live starts the bounded renderer and configured runtime I/O;
    // leaving it tears those down. Freeze, blackout, current scene, captured
    // samples, learned-value overlays, and resolved devices are never saved.
    void setLiveActive(bool active);
    bool isLiveActive() const noexcept;

signals:
    void requestEditMode();
    void runtimeStatusChanged(const QString& summary);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
