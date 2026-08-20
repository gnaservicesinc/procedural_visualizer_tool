#ifndef PVT_LIVE_WORKSPACE_H
#define PVT_LIVE_WORKSPACE_H

#include "procedural_visualizer_tool.h"

#include <QImage>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// The Live workspace is deliberately a sibling of the authoring workspaces:
// it owns transient device bindings and performance state, while the editor
// remains the sole owner of project history and widgets. The runtime may stay
// active while the editor is visible; each frame obtains the current project
// through the snapshot callback. The three callbacks are the only bridge
// needed by MainWindow.
class LiveWorkspace final : public QWidget {
    Q_OBJECT

public:
    using ProjectSnapshotProvider = std::function<pvt::ProjectConfig()>;
    using PresentationFrameProvider = std::function<int()>;
    using RenderOptionsProvider = std::function<pvt::FrameRenderOptions()>;
    using DocumentRevisionProvider = std::function<std::uint64_t()>;
    using ActiveLayerUuidProvider = std::function<std::string()>;
    using AuthoredConfigEditor =
        std::function<void(const pvt::LiveConfig&, const QString& reason)>;

    struct OutputDisplayChoice {
        QString label;
        QString id;
    };

    struct AudioInputChoice {
        QString label;
        // Empty intentionally means the current system default. Non-empty IDs
        // are opaque runtime bindings and belong only in QSettings.
        QString id;
        bool is_default_device = false;
    };

    explicit LiveWorkspace(ProjectSnapshotProvider projectProvider,
                           ProjectSnapshotProvider presentationProjectProvider,
                           PresentationFrameProvider presentationFrameProvider,
                           RenderOptionsProvider renderOptionsProvider,
                           DocumentRevisionProvider documentRevisionProvider,
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

    // Presentation output is the editor preview on a separate display/window.
    // It deliberately starts no capture, MIDI, OSC, scene, route, or sleep-
    // prevention runtime and never opens the Live companion workspace.
    void setPresentationActive(bool active);
    bool isPresentationActive() const noexcept;
    bool isRealtimeOutputActive() const noexcept;
    void requestRealtimeFrame();
    void resetRealtimeFrame();

    // Display and resolution quality are machine-local and shared by the
    // presentation and performance output surfaces. They are never serialized
    // into a project or portable bundle.
    QVector<OutputDisplayChoice> availableOutputDisplays() const;
    QString selectedOutputDisplayId() const;
    void setSelectedOutputDisplayId(const QString& id);
    double outputResolutionScale() const;
    void setOutputResolutionScale(double scale);
    bool presentationFullscreen() const;
    void setPresentationFullscreen(bool fullscreen);
    bool presentationHideCursor() const;
    void setPresentationHideCursor(bool hide);

    // Standard-clock Mic controls reuse the Live input rack through this
    // machine-local bridge. These methods never edit or serialize a project.
    QVector<AudioInputChoice> availableAudioInputs() const;
    void refreshAudioInputs();
    QString audioInputBinding(const std::string& roleUuid) const;
    QString audioInputBindingLabel(const std::string& roleUuid) const;
    bool audioInputBindingAvailable(const std::string& roleUuid) const;
    void setAudioInputBinding(const std::string& roleUuid,
                              const QString& runtimeId,
                              const QString& displayLabel);
    void revealAudioInputSetup(const std::string& roleUuid);

signals:
    void requestEditMode();
    void runtimeStatusChanged(const QString& summary);
    void livePreviewFrame(const QImage& image);
    void runtimeOutputSettingsChanged();
    void presentationActiveChanged(bool active);
    void audioInputsChanged();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
