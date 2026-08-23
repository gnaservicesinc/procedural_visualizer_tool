#include "live_workspace.h"

#include "audio_processing_dialog.h"
#include "device_sleep_guard.h"
#include "live_frame_controller.h"
#include "live_midi.h"
#include "live_osc.h"
#include "live_target_registry.h"
#include "stage_output_window.h"
#include "studio_widgets.h"
#include "../src/live_audio_capture.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr int kUiTickMilliseconds = 20;
constexpr int kMaximumClockCatchupTicks = 8;
constexpr int kMaximumUiInteger = (std::numeric_limits<int>::max)();
constexpr double kMaximumLiveMappingMagnitude =
    static_cast<double>((std::numeric_limits<float>::max)());
constexpr double kMaximumAudioPercent =
    kMaximumLiveMappingMagnitude * 100.0;

constexpr double kDefaultRealtimeFps = 60.0;

QString screen_base_identifier(const QScreen* screen) {
    if (screen == nullptr) return {};
    if (!screen->serialNumber().isEmpty()) {
        return QStringLiteral("serial:%1").arg(screen->serialNumber());
    }
    // Screen arrangement and mode changes must not change the persisted
    // identity. Qt's platform name is the best available discriminator when
    // a display does not publish a serial number.
    return QStringLiteral("screen:%1|%2|%3")
        .arg(screen->name(), screen->manufacturer(), screen->model());
}

QString screen_identifier(const QScreen* screen,
                          const QList<QScreen*>& screens) {
    const QString base = screen_base_identifier(screen);
    int duplicate_count = 0;
    int ordinal = 0;
    for (QScreen* candidate : screens) {
        if (screen_base_identifier(candidate) != base) continue;
        ++duplicate_count;
        if (candidate == screen) ordinal = duplicate_count;
    }
    return duplicate_count > 1
        ? base + QStringLiteral("|instance:%1").arg(std::max(1, ordinal))
        : base;
}

QString screen_label(const QScreen* screen, bool primary,
                     const QList<QScreen*>& screens) {
    if (screen == nullptr) return {};
    QString label = screen->name();
    if (!screen->model().isEmpty() && screen->model() != screen->name()) {
        label += QStringLiteral(" · %1").arg(screen->model());
    }
    const QString base = screen_base_identifier(screen);
    int duplicate_count = 0;
    int ordinal = 0;
    for (QScreen* candidate : screens) {
        if (screen_base_identifier(candidate) != base) continue;
        ++duplicate_count;
        if (candidate == screen) ordinal = duplicate_count;
    }
    if (duplicate_count > 1) {
        label += QObject::tr(" · display %1 of %2")
                     .arg(std::max(1, ordinal))
                     .arg(duplicate_count);
    }
    label += primary ? QObject::tr(" · primary") : QObject::tr(" · stage");
    return label;
}

double sanitized_realtime_fps(double fps) {
    return std::isfinite(fps) && fps > 0.0 ? fps : kDefaultRealtimeFps;
}

double project_loop_duration_seconds(const pvt::ProjectConfig& project) {
    const double fps = sanitized_realtime_fps(project.canvas.fps);
    std::string frame_error;
    const int effective_frames = pvt::effective_frame_count(
        project.canvas, &frame_error);
    const int frames = effective_frames > 0
        ? effective_frames : std::max(1, project.canvas.total_frames);
    return std::max(1.0 / fps, static_cast<double>(frames) / fps);
}

double live_audio_loss_age_seconds(
    const pvt::audio::LiveAudioSnapshot& snapshot,
    const QElapsedTimer& capture_start_clock) {
    if (std::isfinite(snapshot.last_valid_callback_age_seconds)
        && snapshot.last_valid_callback_age_seconds >= 0.0) {
        return snapshot.last_valid_callback_age_seconds;
    }
    return capture_start_clock.isValid()
        ? static_cast<double>(capture_start_clock.elapsed()) / 1000.0
        : (std::numeric_limits<double>::infinity)();
}

QSize physical_widget_size(const QWidget* widget) {
    if (widget == nullptr) return QSize(320, 180);
    const double ratio = std::max(1.0, widget->devicePixelRatioF());
    return QSize(std::max(1, static_cast<int>(std::lround(widget->width() * ratio))),
                 std::max(1, static_cast<int>(std::lround(widget->height() * ratio))));
}

QString uuid_text() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

QString qtext(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

std::string narrow(const QString& value) {
    return value.toUtf8().toStdString();
}

QString endpoint_key(const std::string& uuid, const QString& leaf) {
    return QStringLiteral("live/bindings/%1/%2").arg(qtext(uuid), leaf);
}

QString device_latency_key(const std::string& uuid,
                           const QString& runtime_device_id) {
    const QByteArray identity = runtime_device_id.isEmpty()
        ? QByteArrayLiteral("system-default") : runtime_device_id.toUtf8();
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(
        identity, QCryptographicHash::Sha256).toHex());
    return endpoint_key(
        uuid, QStringLiteral("deviceLatencyMicroseconds/%1").arg(digest));
}

QString clock_output_signature(const pvt::LiveConfig& live) {
    QString value;
    for (const auto& output : live.midi_clock_outputs) {
        value += QStringLiteral("%1|%2|%3|%4|%5|%6;")
                     .arg(output.enabled ? 1 : 0)
                     .arg(static_cast<int>(output.source))
                     .arg(qtext(output.layer_uuid), qtext(output.endpoint_uuid))
                     .arg(output.send_transport ? 1 : 0)
                     .arg(output.send_song_position ? 1 : 0);
    }
    return value;
}

QString clock_input_signature(const pvt::LiveConfig& live,
                              const std::string& active_layer_uuid) {
    QString value = QStringLiteral("active:%1;").arg(qtext(active_layer_uuid));
    for (const auto& input : live.clock_inputs) {
        value += QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9;")
                     .arg(input.enabled ? 1 : 0)
                     .arg(static_cast<int>(input.target))
                     .arg(qtext(input.layer_uuid))
                     .arg(static_cast<int>(input.source))
                     .arg(qtext(input.endpoint_uuid))
                     .arg(input.audio_channel)
                     .arg(input.follow_midi_transport ? 1 : 0)
                     .arg(input.holdover_milliseconds)
                     .arg(qtext(input.frequency_stream_uuid));
    }
    return value;
}

QString endpoint_structure_signature(const pvt::LiveConfig& live) {
    QString value;
    for (const auto& endpoint : live.endpoints) {
        value += QStringLiteral("%1|%2|%3;")
                     .arg(qtext(endpoint.uuid))
                     .arg(static_cast<int>(endpoint.protocol))
                     .arg(static_cast<int>(endpoint.direction));
    }
    value += QStringLiteral("audio:%1|%2|%3|%4|%5;")
                 .arg(live.audio_processing.high_pass_enabled ? 1 : 0)
                 .arg(live.audio_processing.high_pass_hz, 0, 'g', 17)
                 .arg(live.audio_processing.low_pass_enabled ? 1 : 0)
                 .arg(live.audio_processing.low_pass_hz, 0, 'g', 17)
                 .arg(live.audio_processing.equalizer_enabled ? 1 : 0);
    for (const auto& band : live.audio_processing.equalizer_bands) {
        value += QStringLiteral("eq:%1|%2;")
                     .arg(band.frequency_hz, 0, 'g', 17)
                     .arg(band.gain_db, 0, 'g', 17);
    }
    for (const auto& stream : live.audio_processing.frequency_streams) {
        value += QStringLiteral("range:%1|%2|%3;")
                     .arg(qtext(stream.uuid))
                     .arg(stream.low_hz, 0, 'g', 17)
                     .arg(stream.high_hz, 0, 'g', 17);
    }
    return value;
}

bool direction_has_input(pvt::LiveEndpointDirection direction) {
    return direction == pvt::LiveEndpointDirection::Input
        || direction == pvt::LiveEndpointDirection::Bidirectional;
}

bool direction_has_output(pvt::LiveEndpointDirection direction) {
    return direction == pvt::LiveEndpointDirection::Output
        || direction == pvt::LiveEndpointDirection::Bidirectional;
}

QString mapping_mode_name(pvt::LiveMappingMode mode) {
    switch (mode) {
        case pvt::LiveMappingMode::Absolute: return QObject::tr("Absolute");
        case pvt::LiveMappingMode::Relative: return QObject::tr("Relative");
        case pvt::LiveMappingMode::Toggle: return QObject::tr("Toggle");
        case pvt::LiveMappingMode::Momentary: return QObject::tr("Momentary");
        case pvt::LiveMappingMode::Trigger: return QObject::tr("Trigger");
    }
    return {};
}

QString input_name(pvt::LiveControlInput input) {
    switch (input) {
        case pvt::LiveControlInput::MidiControlChange: return QObject::tr("MIDI CC");
        case pvt::LiveControlInput::MidiNote: return QObject::tr("MIDI note");
        case pvt::LiveControlInput::MidiProgramChange: return QObject::tr("MIDI program");
        case pvt::LiveControlInput::MidiPitchBend: return QObject::tr("MIDI pitch bend");
        case pvt::LiveControlInput::MidiChannelPressure: return QObject::tr("MIDI pressure");
        case pvt::LiveControlInput::OscValue: return QObject::tr("OSC value");
        case pvt::LiveControlInput::Footswitch: return QObject::tr("Footswitch");
    }
    return {};
}

QString action_name(pvt::LiveAction action) {
    switch (action) {
        case pvt::LiveAction::Freeze: return QObject::tr("Freeze");
        case pvt::LiveAction::Blackout: return QObject::tr("Blackout");
        case pvt::LiveAction::NextScene: return QObject::tr("Next scene");
        case pvt::LiveAction::PreviousScene: return QObject::tr("Previous scene");
        case pvt::LiveAction::RestartScene: return QObject::tr("Restart scene");
        case pvt::LiveAction::TapTempo: return QObject::tr("Tap tempo");
    }
    return {};
}

QString mapping_source(const pvt::LiveControlMapping& mapping) {
    QString value = input_name(mapping.input);
    if (mapping.input == pvt::LiveControlInput::OscValue) {
        return value + QStringLiteral("  ") + qtext(mapping.osc_address);
    }
    if (mapping.input == pvt::LiveControlInput::MidiPitchBend
        || mapping.input == pvt::LiveControlInput::MidiChannelPressure) {
        return mapping.midi_channel == 0
            ? value + QObject::tr(" · omni")
            : value + QObject::tr(" · ch %1").arg(mapping.midi_channel);
    }
    return value + QObject::tr(" · %1 · ch %2")
                       .arg(mapping.control_number)
                       .arg(mapping.midi_channel == 0
                                ? QObject::tr("omni")
                                : QString::number(mapping.midi_channel));
}

bool parse_scene_number(const pvt::LiveSceneValue& value, double& number) {
    bool ok = false;
    const QString text = qtext(value.value).trimmed();
    switch (value.type) {
        case pvt::LiveSceneValueType::Boolean:
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
        case pvt::LiveSceneValueType::Integer:
        case pvt::LiveSceneValueType::Real:
        case pvt::LiveSceneValueType::EnumToken:
            number = text.toDouble(&ok);
            return ok && std::isfinite(number);
        case pvt::LiveSceneValueType::String:
            return false;
    }
    return false;
}

pvt::LiveSceneValueType scene_type_for(LiveTargetKind kind) {
    switch (kind) {
        case LiveTargetKind::Boolean: return pvt::LiveSceneValueType::Boolean;
        case LiveTargetKind::Integer: return pvt::LiveSceneValueType::Integer;
        case LiveTargetKind::Enumeration: return pvt::LiveSceneValueType::EnumToken;
        case LiveTargetKind::Real: return pvt::LiveSceneValueType::Real;
    }
    return pvt::LiveSceneValueType::Real;
}

QString number_text(double value, LiveTargetKind kind) {
    if (kind == LiveTargetKind::Boolean) return value >= 0.5 ? QStringLiteral("1") : QStringLiteral("0");
    if (kind == LiveTargetKind::Integer || kind == LiveTargetKind::Enumeration) {
        return QString::number(static_cast<qlonglong>(std::llround(value)));
    }
    return QString::number(value, 'g', 17);
}

QWidget* titled_group(const QString& title, QFormLayout*& form) {
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("liveRackPanel"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 12, 14, 14);
    auto* caption = new QLabel(title);
    caption->setObjectName(QStringLiteral("liveRackTitle"));
    layout->addWidget(caption);
    form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(9);
    layout->addLayout(form);
    return panel;
}

} // namespace

struct LiveWorkspace::Impl {
    struct OverrideValue {
        double current = 0.0;
        double target = 0.0;
        int smoothing_ms = 0;
        qint64 changed_ms = 0;
    };

    struct MappingRuntime {
        double previous_raw = 0.0;
        bool high = false;
    };

    struct SceneTransition {
        bool active = false;
        QString scene_uuid;
        int duration_ms = 0;
        qint64 started_ms = 0;
        QHash<QString, double> from;
        QHash<QString, double> to;
        QSet<QString> discrete;
    };

    struct ClockOutputRuntime {
        double next_seconds = 0.0;
    };

    LiveWorkspace* q = nullptr;
    ProjectSnapshotProvider project_provider;
    ProjectSnapshotProvider presentation_project_provider;
    PresentationFrameProvider presentation_frame_provider;
    RenderOptionsProvider render_options_provider;
    DocumentRevisionProvider document_revision_provider;
    ActiveLayerUuidProvider active_layer_provider;
    AuthoredConfigEditor authored_editor;
    pvt::LiveConfig config;
    bool active = false;
    bool presentation_active = false;
    bool rebuilding = false;
    bool project_editing_enabled = true;
    bool user_freeze = false;
    bool user_blackout = false;
    bool safety_blackout = false;
    bool render_failed = false;
    int learn_mapping = -1;
    int late_streak = 0;
    int good_streak = 0;
    double adaptive_scale = 1.0;
    double tapped_bpm = 0.0;
    std::uint64_t tapped_beat_count = 0U;
    double tapped_beat_anchor_seconds = 0.0;
    double scheduled_fps = kDefaultRealtimeFps;
    std::uint64_t render_generation = 1U;
    std::uint64_t frame_schedule_tick = 0U;
    QVector<qint64> tempo_taps;

    pvt::audio::LiveAudioCapture audio;
    DeviceSleepGuard sleep_guard;
    LiveMidiRouter midi;
    LiveOscRouter osc;
    LiveFrameController renderer;
    StageOutputWindow stage;
    QTimer render_timer;
    QTimer ui_timer;
    QTimer midi_clock_timer;
    QElapsedTimer run_clock;
    QElapsedTimer last_good_clock;
    QElapsedTimer audio_dropout_clock;
    QElapsedTimer frame_schedule_clock;
    QElapsedTimer presented_frame_clock;
    QImage last_image;
    pvt::audio::LiveAudioSnapshot audio_snapshot;
    std::vector<pvt::audio::LiveAudioDevice> audio_devices;
    QString runtime_input_signature;
    QSet<QString> route_warnings;
    QHash<QString, OverrideValue> overrides;
    std::vector<LiveTargetDescriptor> target_cache;
    QHash<QString, int> target_index;
    QHash<int, MappingRuntime> mapping_runtime;
    SceneTransition scene_transition;
    QVector<ClockOutputRuntime> clock_outputs;
    pvt::ProjectConfig project_cache;
    bool project_cache_valid = false;
    std::vector<QMetaObject::Connection> screen_connections;

    QLabel* monitor = nullptr;
    QLabel* fps_readout = nullptr;
    QLabel* frame_readout = nullptr;
    QLabel* scene_readout = nullptr;
    StatusLamp* render_lamp = nullptr;
    StatusLamp* audio_lamp = nullptr;
    StatusLamp* midi_lamp = nullptr;
    LiveLevelMeter* audio_meter = nullptr;
    QPushButton* live_button = nullptr;
    QPushButton* output_button = nullptr;
    QPushButton* freeze_button = nullptr;
    QPushButton* blackout_button = nullptr;
    QPushButton* learn_button = nullptr;
    QTabWidget* tabs = nullptr;

    QComboBox* audio_role = nullptr;
    QComboBox* audio_device = nullptr;
    QComboBox* midi_role = nullptr;
    QComboBox* midi_device = nullptr;
    QComboBox* foot_role = nullptr;
    QComboBox* foot_device = nullptr;
    QComboBox* osc_role = nullptr;
    QSpinBox* osc_port = nullptr;
    QCheckBox* osc_local = nullptr;
    QComboBox* screen = nullptr;
    QComboBox* quality = nullptr;
    QCheckBox* portable_fullscreen = nullptr;
    QCheckBox* prefer_secondary = nullptr;
    QCheckBox* hide_stage_cursor = nullptr;
    QComboBox* dropout_behavior = nullptr;
    QCheckBox* watchdog_enabled = nullptr;
    QSpinBox* watchdog_timeout = nullptr;
    QSpinBox* audio_grace = nullptr;
    QSpinBox* last_good_timeout = nullptr;
    QCheckBox* prevent_sleep = nullptr;
    StudioKnob* gain = nullptr;
    QDoubleSpinBox* gain_value = nullptr;
    StudioKnob* sensitivity = nullptr;
    QDoubleSpinBox* sensitivity_value = nullptr;
    QSpinBox* audio_period = nullptr;
    QPushButton* audio_processing = nullptr;
    QSpinBox* latency = nullptr;
    QSpinBox* device_latency = nullptr;
    QLabel* detected_tempo = nullptr;
    QComboBox* project_clock = nullptr;
    QComboBox* project_clock_role = nullptr;
    QComboBox* project_clock_stream = nullptr;
    QComboBox* layer_clock = nullptr;
    QComboBox* layer_clock_role = nullptr;
    QComboBox* layer_clock_stream = nullptr;
    QCheckBox* project_clock_out = nullptr;
    QCheckBox* layer_clock_out = nullptr;
    QComboBox* project_clock_out_role = nullptr;
    QComboBox* layer_clock_out_role = nullptr;
    QTableWidget* mapping_table = nullptr;
    QListWidget* scene_list = nullptr;
    QSpinBox* scene_transition_ms = nullptr;

    explicit Impl(LiveWorkspace* owner,
                  ProjectSnapshotProvider projectProvider,
                  ProjectSnapshotProvider presentationProjectProvider,
                  PresentationFrameProvider presentationFrameProvider,
                  RenderOptionsProvider renderOptionsProvider,
                  DocumentRevisionProvider documentRevisionProvider,
                  ActiveLayerUuidProvider layerProvider,
                  AuthoredConfigEditor editor)
        : q(owner),
          project_provider(std::move(projectProvider)),
          presentation_project_provider(std::move(presentationProjectProvider)),
          presentation_frame_provider(std::move(presentationFrameProvider)),
          render_options_provider(std::move(renderOptionsProvider)),
          document_revision_provider(std::move(documentRevisionProvider)),
          active_layer_provider(std::move(layerProvider)),
          authored_editor(std::move(editor)),
          midi(owner), osc(owner), renderer(owner), stage(nullptr) {
        buildUi();
        connectRuntime();
        refreshScreens();
        refreshDevices();
        runtime_input_signature = clock_input_signature(
            config, active_layer_provider ? active_layer_provider()
                                          : std::string{});
        render_timer.setTimerType(Qt::PreciseTimer);
        render_timer.setSingleShot(true);
        ui_timer.setTimerType(Qt::PreciseTimer);
        midi_clock_timer.setTimerType(Qt::PreciseTimer);
        ui_timer.setInterval(kUiTickMilliseconds);
        midi_clock_timer.setInterval(1);
        QObject::connect(&render_timer, &QTimer::timeout, q, [this] {
            requestFrame();
            scheduleNextFrame();
        });
        QObject::connect(&ui_timer, &QTimer::timeout, q,
                         [this] { runtimeTick(); });
        QObject::connect(&midi_clock_timer, &QTimer::timeout, q,
                         [this] { sendClockOutputs(); });
    }

    ~Impl() {
        setPresentationActive(false);
        setActive(false);
    }

    void buildUi();
    QWidget* buildRigTab();
    QWidget* buildMappingTab();
    QWidget* buildSceneTab();
    void connectRuntime();
    void commitConfig(pvt::LiveConfig next, const QString& reason);
    void refreshConfigUi();
    void rebuildTargetCache();
    void refreshRoleCombos();
    void refreshMappings();
    void refreshScenes();
    void refreshClockRouting();
    void refreshDevices();
    void refreshRuntimeRouting();
    void refreshScreens();
    void setActive(bool value);
    void setPresentationActive(bool value);
    bool realtimeActive() const noexcept;
    void resetRealtimeFrame();
    void restartFrameSchedule(double fps);
    void updateScheduledFps(double fps);
    void scheduleNextFrame();
    void updateSleepPrevention();
    void startIo();
    void stopIo();
    void restartAudio();
    void restartOsc();
    void configureClockOutputs();
    void requestFrame();
    void runtimeTick();
    void frameFinished(const LiveFrameController::Result& result);
    void updateMonitor();
    void updateSafety();
    void setFreeze(bool value);
    void setBlackout(bool value);
    void updateOutputState();
    void toggleOutput();
    void dismissOutput();
    void showOutput();
    void applyVisibleOutputPolicy();
    void restoreOutputOwnerFocus();
    void showStartingState();
    void showStandbyState();
    QScreen* selectedScreen() const;
    const pvt::LiveEndpointConfig* endpoint(const std::string& uuid) const;
    pvt::LiveEndpointConfig* endpoint(pvt::LiveConfig& value,
                                      const std::string& uuid) const;
    QString boundSource(const std::string& uuid) const;
    std::string preferredAudioRole() const;
    bool effectiveAudioRoute(const pvt::LiveClockInputConfig& input) const;
    bool effectiveAudioClockReceiving() const;
    bool shouldHoldAudioFrame() const;
    void warnRouteOnce(const QString& key, const QString& message);
    QString sourceEndpoint(pvt::LiveEndpointProtocol protocol,
                           const QString& runtimeSource) const;
    void createStarterRig();
    void addLogicalRole();
    void calibrateLatency();
    void tapTempo();
    void editMapping(int index);
    void removeMapping();
    void beginMidiLearn();
    void handleMidi(LiveMidiRouter::MessageKind kind, int channel, int number,
                    double value, const QString& source);
    void handleOsc(const QString& address, double value,
                   const QString& sender);
    void processControl(pvt::LiveControlInput input, int channel, int number,
                        double value, const QString& endpointUuid,
                        const QString& oscAddress = {});
    double transformedValue(const pvt::LiveControlMapping& mapping,
                            int mappingIndex, double raw, bool& fire);
    void performMapping(const pvt::LiveControlMapping& mapping,
                        double value, bool fire);
    void performAction(pvt::LiveAction action, double value, bool fire);
    void captureScene(bool updateExisting);
    void removeScene();
    void takeSelectedScene();
    void takeScene(const std::string& uuid, bool restart = false);
    void selectRelativeScene(int delta);
    pvt::ProjectConfig runtimeProject();
    void applyOverrides(pvt::ProjectConfig& project);
    double basePhase(const pvt::ProjectConfig& project) const;
    std::optional<double> routedPhase(
        const pvt::ProjectConfig& project,
        const pvt::LiveClockInputConfig& input);
    bool applyClockRoutes(pvt::ProjectConfig& project, double& projectPhase);
    pvt::MusicAnalysis ephemeralAnalysis(
        double durationSeconds, const std::string& selectedStreamUuid) const;
    void sendClockOutputs();
    double outputBpm(const pvt::ProjectConfig& project,
                     const pvt::LiveMidiClockOutputConfig& output) const;
    void editClockRoute(bool layerTarget);
    void editClockOutput(bool layerTarget);
};

void LiveWorkspace::Impl::buildUi() {
    q->setObjectName(QStringLiteral("liveWorkspace"));
    q->setMinimumSize(900, 600);
    q->setStyleSheet(QStringLiteral(R"(
        #liveWorkspace { background: #171a1f; color: #e7eaed; }
        #liveHeader { background: #20242a; border: 1px solid #343a42;
                      border-radius: 7px; }
        #liveTitle { color: #f1f5f7; font-weight: 800; font-size: 17px;
                     letter-spacing: 2px; }
        #liveSubTitle { color: #8d99a5; font-size: 11px; }
        #liveRackPanel { background: #21252b; border: 1px solid #383e46;
                         border-radius: 7px; }
        #liveRackTitle { color: #7ed6ce; font-weight: 700; font-size: 12px;
                         letter-spacing: 1px; }
        #liveProgramFrame { background: #070809; border: 2px solid #3a414a;
                            border-radius: 7px; }
        #liveProgramLabel { background: #000; color: #69737d; }
        #liveWorkspace QPushButton { min-height: 25px; padding: 4px 11px; }
        #liveWorkspace QPushButton:checked { background: #227d78;
                                             color: white; border-color: #65d2c9; }
        #liveWorkspace QPushButton#blackoutButton:checked {
            background: #a52f35; border-color: #ff7279; }
        #liveWorkspace QPushButton#freezeButton:checked {
            background: #98671d; border-color: #edb454; }
        #liveWorkspace QTabWidget::pane { border: 1px solid #383e46;
                                         background: #1b1e23; }
        #liveWorkspace QTabBar::tab:selected { color: #86ded6;
                                               border-bottom: 2px solid #4fc8be; }
        #liveWorkspace QTableWidget, #liveWorkspace QListWidget {
            background: #15181c; alternate-background-color: #1d2126;
            border: 1px solid #353b43; }
    )"));

    auto* root = new QVBoxLayout(q);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(9);

    auto* header = new QFrame;
    header->setObjectName(QStringLiteral("liveHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(14, 9, 10, 9);
    auto* names = new QVBoxLayout;
    auto* title = new QLabel(q->tr("LIVE / PERFORMANCE"));
    title->setObjectName(QStringLiteral("liveTitle"));
    auto* subtitle = new QLabel(q->tr(
        "Portable roles and scenes · machine bindings stay on this computer"));
    subtitle->setObjectName(QStringLiteral("liveSubTitle"));
    names->addWidget(title);
    names->addWidget(subtitle);
    header_layout->addLayout(names);
    header_layout->addStretch(1);
    render_lamp = new StatusLamp;
    render_lamp->setText(q->tr("STANDBY"));
    header_layout->addWidget(render_lamp);
    live_button = new QPushButton(q->tr("GO LIVE"));
    live_button->setObjectName(QStringLiteral("liveRuntimeButton"));
    live_button->setCheckable(true);
    live_button->setMinimumWidth(105);
    header_layout->addWidget(live_button);
    auto* edit = new QPushButton(q->tr("Edit Project"));
    edit->setObjectName(QStringLiteral("editLiveProjectButton"));
    edit->setToolTip(q->tr(
        "Bring the full project editor forward without stopping Live input, rendering, or stage output."));
    header_layout->addWidget(edit);
    root->addWidget(header);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    auto* program_column = new QWidget;
    auto* program_layout = new QVBoxLayout(program_column);
    program_layout->setContentsMargins(0, 0, 0, 0);
    program_layout->setSpacing(8);

    auto* program_frame = new QFrame;
    program_frame->setObjectName(QStringLiteral("liveProgramFrame"));
    auto* program_frame_layout = new QVBoxLayout(program_frame);
    program_frame_layout->setContentsMargins(6, 6, 6, 6);
    monitor = new QLabel(q->tr("PROGRAM OUTPUT\nStandby"));
    monitor->setObjectName(QStringLiteral("liveProgramLabel"));
    monitor->setAlignment(Qt::AlignCenter);
    monitor->setMinimumSize(420, 240);
    monitor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    program_frame_layout->addWidget(monitor);
    program_layout->addWidget(program_frame, 1);

    auto* transport = new QHBoxLayout;
    output_button = new QPushButton(q->tr("Full-screen Output"));
    output_button->setObjectName(QStringLiteral("liveStageOutputButton"));
    output_button->setCheckable(true);
    freeze_button = new QPushButton(q->tr("FREEZE"));
    freeze_button->setObjectName(QStringLiteral("freezeButton"));
    freeze_button->setCheckable(true);
    freeze_button->setMinimumHeight(38);
    blackout_button = new QPushButton(q->tr("BLACKOUT"));
    blackout_button->setObjectName(QStringLiteral("blackoutButton"));
    blackout_button->setCheckable(true);
    blackout_button->setMinimumHeight(38);
    transport->addWidget(output_button, 2);
    transport->addWidget(freeze_button, 1);
    transport->addWidget(blackout_button, 1);
    program_layout->addLayout(transport);

    auto* telemetry = new QFrame;
    telemetry->setObjectName(QStringLiteral("liveRackPanel"));
    auto* telemetry_layout = new QHBoxLayout(telemetry);
    telemetry_layout->setContentsMargins(10, 7, 10, 7);
    audio_lamp = new StatusLamp;
    audio_lamp->setText(q->tr("AUDIO"));
    midi_lamp = new StatusLamp;
    midi_lamp->setText(q->tr("MIDI"));
    fps_readout = new QLabel(q->tr("— fps"));
    frame_readout = new QLabel(q->tr("No frame"));
    scene_readout = new QLabel(q->tr("Scene: —"));
    telemetry_layout->addWidget(audio_lamp);
    telemetry_layout->addWidget(midi_lamp);
    telemetry_layout->addSpacing(8);
    telemetry_layout->addWidget(fps_readout);
    telemetry_layout->addWidget(frame_readout);
    telemetry_layout->addStretch(1);
    telemetry_layout->addWidget(scene_readout);
    program_layout->addWidget(telemetry);

    tabs = new QTabWidget;
    tabs->addTab(buildRigTab(), q->tr("Rig"));
    tabs->addTab(buildMappingTab(), q->tr("Control Map"));
    tabs->addTab(buildSceneTab(), q->tr("Scenes"));
    splitter->addWidget(program_column);
    splitter->addWidget(tabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({650, 440});
    root->addWidget(splitter, 1);

    QObject::connect(live_button, &QPushButton::toggled, q,
                     [this](bool checked) { setActive(checked); });
    QObject::connect(edit, &QPushButton::clicked, q,
                     [this] { emit q->requestEditMode(); });
    QObject::connect(output_button, &QPushButton::clicked, q,
                     [this] { toggleOutput(); });
    QObject::connect(freeze_button, &QPushButton::toggled, q,
                     [this](bool checked) { setFreeze(checked); });
    QObject::connect(blackout_button, &QPushButton::toggled, q,
                     [this](bool checked) { setBlackout(checked); });
    QObject::connect(&stage, &StageOutputWindow::dismissRequested, q, [this] {
        dismissOutput();
    });
    QObject::connect(&stage, &StageOutputWindow::outputMetricsChanged, q, [this] {
        if (!realtimeActive()) return;
        ++render_generation;
        renderer.cancelCurrent();
        requestFrame();
    });
}

QWidget* LiveWorkspace::Impl::buildRigTab() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);

    auto* rig_actions = new QHBoxLayout;
    auto* starter = new QPushButton(q->tr("Create Starter Rig"));
    starter->setToolTip(q->tr(
        "Adds portable Audio, MIDI, OSC, foot-controller, and MIDI-out roles. "
        "No device identity is saved in the project."));
    auto* add_role = new QPushButton(q->tr("Add Logical Role…"));
    rig_actions->addWidget(starter);
    rig_actions->addWidget(add_role);
    rig_actions->addStretch(1);
    layout->addLayout(rig_actions);

    QFormLayout* form = nullptr;
    QWidget* inputs = titled_group(q->tr("INPUT RACK"), form);
    audio_role = new QComboBox;
    audio_device = new QComboBox;
    audio_role->setObjectName(QStringLiteral("liveAudioRole"));
    audio_device->setObjectName(QStringLiteral("liveAudioDevice"));
    auto* audio_refresh = new QPushButton(q->tr("Refresh"));
    audio_refresh->setObjectName(QStringLiteral("liveAudioRefresh"));
    audio_refresh->setToolTip(q->tr("Detect connected microphone and audio-interface inputs again."));
    auto* audio_row = new QWidget;
    auto* audio_row_layout = new QHBoxLayout(audio_row);
    audio_row_layout->setContentsMargins(0, 0, 0, 0);
    audio_row_layout->addWidget(audio_role, 1);
    audio_row_layout->addWidget(audio_device, 2);
    audio_row_layout->addWidget(audio_refresh);
    form->addRow(q->tr("Audio role / device"), audio_row);
    midi_role = new QComboBox;
    midi_device = new QComboBox;
    auto* midi_row = new QWidget;
    auto* midi_row_layout = new QHBoxLayout(midi_row);
    midi_row_layout->setContentsMargins(0, 0, 0, 0);
    midi_row_layout->addWidget(midi_role, 1);
    midi_row_layout->addWidget(midi_device, 2);
    form->addRow(q->tr("MIDI role / source"), midi_row);
    foot_role = new QComboBox;
    foot_device = new QComboBox;
    auto* foot_row = new QWidget;
    auto* foot_row_layout = new QHBoxLayout(foot_row);
    foot_row_layout->setContentsMargins(0, 0, 0, 0);
    foot_row_layout->addWidget(foot_role, 1);
    foot_row_layout->addWidget(foot_device, 2);
    form->addRow(q->tr("Foot role / source"), foot_row);
    osc_role = new QComboBox;
    osc_port = new QSpinBox;
    osc_port->setRange(1, 65535);
    osc_port->setValue(QSettings().value(QStringLiteral("live/oscPort"), 7000).toInt());
    osc_local = new QCheckBox(q->tr("Local only"));
    osc_local->setChecked(QSettings().value(QStringLiteral("live/oscLocalOnly"), true).toBool());
    auto* osc_row = new QWidget;
    auto* osc_row_layout = new QHBoxLayout(osc_row);
    osc_row_layout->setContentsMargins(0, 0, 0, 0);
    osc_row_layout->addWidget(osc_role, 1);
    osc_row_layout->addWidget(osc_port);
    osc_row_layout->addWidget(osc_local);
    form->addRow(q->tr("OSC role / UDP port"), osc_row);
    layout->addWidget(inputs);

    QFormLayout* audio_form = nullptr;
    QWidget* analysis = titled_group(q->tr("AUDIO ANALYSIS + CALIBRATION"), audio_form);
    audio_period = new QSpinBox;
    audio_period->setObjectName(QStringLiteral("liveAudioPeriodFrames"));
    audio_period->setRange(1, kMaximumUiInteger);
    audio_period->setSuffix(q->tr(" frames"));
    const int stored_period = QSettings().value(
        QStringLiteral("live/audioPeriodFrames"), 128).toInt();
    audio_period->setValue(std::max(1, stored_period));
    audio_period->setToolTip(q->tr(
        "Machine-local capture callback size. Smaller buffers reduce latency but demand steadier CPU scheduling."));
    audio_form->addRow(q->tr("Input buffer"), audio_period);
    auto* knob_row = new QWidget;
    auto* knob_layout = new QHBoxLayout(knob_row);
    knob_layout->setContentsMargins(0, 0, 0, 0);
    const auto stored_percent = [](const QString& key) {
        const double requested = QSettings().value(key, 100.0).toDouble();
        return std::isfinite(requested)
            ? std::clamp(requested, 0.0, kMaximumAudioPercent) : 100.0;
    };
    const double stored_gain = stored_percent(
        QStringLiteral("live/audioGain"));
    const double stored_sensitivity = stored_percent(
        QStringLiteral("live/audioSensitivity"));
    gain = new StudioKnob;
    gain->setRange(0, 400);
    gain->setValue(static_cast<int>(std::lround(
        std::min(stored_gain, 400.0))));
    gain->setUnit(QStringLiteral("%"));
    gain_value = new QDoubleSpinBox;
    gain_value->setObjectName(QStringLiteral("liveAudioGainValue"));
    gain_value->setDecimals(3);
    gain_value->setRange(0.0, kMaximumAudioPercent);
    gain_value->setSuffix(QStringLiteral("%"));
    gain_value->setValue(stored_gain);
    gain_value->setMaximumWidth(145);
    sensitivity = new StudioKnob;
    sensitivity->setRange(0, 400);
    sensitivity->setValue(static_cast<int>(std::lround(
        std::min(stored_sensitivity, 400.0))));
    sensitivity->setUnit(QStringLiteral("%"));
    sensitivity_value = new QDoubleSpinBox;
    sensitivity_value->setObjectName(
        QStringLiteral("liveAudioSensitivityValue"));
    sensitivity_value->setDecimals(3);
    sensitivity_value->setRange(0.0, kMaximumAudioPercent);
    sensitivity_value->setSuffix(QStringLiteral("%"));
    sensitivity_value->setValue(stored_sensitivity);
    sensitivity_value->setMaximumWidth(145);
    const QString direct_value_tooltip = q->tr(
        "The knob provides a fast 0–400% performance range. Enter any larger finite value here when the input or response needs it.");
    gain_value->setToolTip(direct_value_tooltip);
    sensitivity_value->setToolTip(direct_value_tooltip);
    audio_meter = new LiveLevelMeter;
    audio_meter->setCaption(q->tr("INPUT"));
    knob_layout->addWidget(new QLabel(q->tr("GAIN")));
    knob_layout->addWidget(gain);
    knob_layout->addWidget(gain_value);
    knob_layout->addWidget(new QLabel(q->tr("RESPONSE")));
    knob_layout->addWidget(sensitivity);
    knob_layout->addWidget(sensitivity_value);
    knob_layout->addWidget(audio_meter, 1);
    audio_form->addRow(knob_row);
    latency = new QSpinBox;
    latency->setRange((std::numeric_limits<int>::min)(), kMaximumUiInteger);
    latency->setSuffix(q->tr(" ms"));
    latency->setToolTip(q->tr(
        "Portable rig offset. Positive values advance a beat that reaches the analyzer late; negative values delay an early source."));
    auto* calibrate = new QPushButton(q->tr("Tap beat to align"));
    auto* latency_row = new QWidget;
    auto* latency_layout = new QHBoxLayout(latency_row);
    latency_layout->setContentsMargins(0, 0, 0, 0);
    latency_layout->addWidget(latency);
    latency_layout->addWidget(calibrate);
    audio_form->addRow(q->tr("Portable input offset"), latency_row);
    device_latency = new QSpinBox;
    device_latency->setObjectName(QStringLiteral("liveAudioDeviceLatency"));
    device_latency->setRange((std::numeric_limits<int>::min)(),
                             kMaximumUiInteger);
    device_latency->setSuffix(q->tr(" ms"));
    device_latency->setToolTip(q->tr(
        "Machine-local correction for this logical role and selected device. Positive values advance late capture; it is never saved in the project."));
    audio_form->addRow(q->tr("This device correction"), device_latency);
    detected_tempo = new QLabel(q->tr("Waiting for audio…"));
    audio_form->addRow(q->tr("Causal analysis"), detected_tempo);
    audio_processing = new QPushButton(q->tr("Filters, EQ + Frequency Streams…"));
    audio_processing->setToolTip(q->tr(
        "Configure the portable processing chain that runs before every Live analysis feature."));
    audio_form->addRow(q->tr("Before analysis"), audio_processing);
    layout->addWidget(analysis);

    QFormLayout* clock_form = nullptr;
    QWidget* clocks = titled_group(q->tr("CLOCK PATCH BAY"), clock_form);
    project_clock = new QComboBox;
    project_clock->addItem(q->tr("Project timeline"), -1);
    project_clock->addItem(q->tr("MIDI Clock in"), static_cast<int>(pvt::LiveClockInputSource::MidiClock));
    project_clock->addItem(q->tr("Audio beat clock"), static_cast<int>(pvt::LiveClockInputSource::AudioStream));
    project_clock_role = new QComboBox;
    project_clock_stream = new QComboBox;
    auto* project_clock_row = new QWidget;
    auto* pcr = new QHBoxLayout(project_clock_row);
    pcr->setContentsMargins(0, 0, 0, 0);
    pcr->addWidget(project_clock, 1);
    pcr->addWidget(project_clock_role, 1);
    pcr->addWidget(project_clock_stream, 1);
    clock_form->addRow(q->tr("Project clock"), project_clock_row);
    layer_clock = new QComboBox;
    layer_clock->addItem(q->tr("Follow project"), -1);
    layer_clock->addItem(q->tr("MIDI Clock in"), static_cast<int>(pvt::LiveClockInputSource::MidiClock));
    layer_clock->addItem(q->tr("Audio beat clock"), static_cast<int>(pvt::LiveClockInputSource::AudioStream));
    layer_clock_role = new QComboBox;
    layer_clock_stream = new QComboBox;
    auto* layer_clock_row = new QWidget;
    auto* lcr = new QHBoxLayout(layer_clock_row);
    lcr->setContentsMargins(0, 0, 0, 0);
    lcr->addWidget(layer_clock, 1);
    lcr->addWidget(layer_clock_role, 1);
    lcr->addWidget(layer_clock_stream, 1);
    clock_form->addRow(q->tr("Active-layer clock"), layer_clock_row);
    project_clock_out = new QCheckBox(q->tr("Send project clock"));
    project_clock_out_role = new QComboBox;
    auto* project_out_row = new QWidget;
    auto* por = new QHBoxLayout(project_out_row);
    por->setContentsMargins(0, 0, 0, 0);
    por->addWidget(project_clock_out);
    por->addWidget(project_clock_out_role, 1);
    clock_form->addRow(q->tr("Virtual MIDI Clock out"), project_out_row);
    layer_clock_out = new QCheckBox(q->tr("Send active layer"));
    layer_clock_out_role = new QComboBox;
    auto* layer_out_row = new QWidget;
    auto* lor = new QHBoxLayout(layer_out_row);
    lor->setContentsMargins(0, 0, 0, 0);
    lor->addWidget(layer_clock_out);
    lor->addWidget(layer_clock_out_role, 1);
    clock_form->addRow(q->tr("Layer Clock out"), layer_out_row);
    layout->addWidget(clocks);

    QFormLayout* output_form = nullptr;
    QWidget* output = titled_group(q->tr("STAGE OUTPUT"), output_form);
    screen = new QComboBox;
    screen->setObjectName(QStringLiteral("liveOutputDisplay"));
    quality = new QComboBox;
    quality->setObjectName(QStringLiteral("liveOutputQuality"));
    quality->addItem(q->tr("Auto · watchdog managed"), 0.0);
    quality->addItem(q->tr("Full resolution"), 1.0);
    quality->addItem(q->tr("75%"), 0.75);
    quality->addItem(q->tr("50%"), 0.5);
    quality->addItem(q->tr("25%"), 0.25);
    const double stored_quality = QSettings().value(QStringLiteral("live/resolutionScale"), 0.0).toDouble();
    const int quality_index = quality->findData(stored_quality);
    quality->setCurrentIndex(quality_index < 0 ? 0 : quality_index);
    output_form->addRow(q->tr("Display"), screen);
    output_form->addRow(q->tr("Render quality"), quality);
    portable_fullscreen = new QCheckBox(q->tr("Open as full-screen stage output"));
    prefer_secondary = new QCheckBox(q->tr("Prefer a secondary display on a new machine"));
    hide_stage_cursor = new QCheckBox(q->tr("Hide the pointer over stage output"));
    output_form->addRow(q->tr("Portable output policy"), portable_fullscreen);
    output_form->addRow({}, prefer_secondary);
    output_form->addRow({}, hide_stage_cursor);
    dropout_behavior = new QComboBox;
    dropout_behavior->addItem(q->tr("Hold last good frame"),
        static_cast<int>(pvt::LiveDropoutBehavior::LastGoodFrame));
    dropout_behavior->addItem(q->tr("Blackout immediately"),
        static_cast<int>(pvt::LiveDropoutBehavior::Blackout));
    output_form->addRow(q->tr("On dropout"), dropout_behavior);
    watchdog_enabled = new QCheckBox(q->tr("Enable frame-time watchdog"));
    watchdog_timeout = new QSpinBox;
    watchdog_timeout->setRange(1, kMaximumUiInteger);
    watchdog_timeout->setSuffix(q->tr(" ms"));
    auto* watchdog_row = new QWidget;
    auto* watchdog_layout = new QHBoxLayout(watchdog_row);
    watchdog_layout->setContentsMargins(0, 0, 0, 0);
    watchdog_layout->addWidget(watchdog_enabled);
    watchdog_layout->addWidget(watchdog_timeout);
    output_form->addRow(q->tr("Frame deadline"), watchdog_row);
    audio_grace = new QSpinBox;
    audio_grace->setRange(0, kMaximumUiInteger);
    audio_grace->setSuffix(q->tr(" ms"));
    last_good_timeout = new QSpinBox;
    last_good_timeout->setObjectName(QStringLiteral("liveLastGoodTimeout"));
    last_good_timeout->setRange(0, kMaximumUiInteger);
    last_good_timeout->setSuffix(q->tr(" ms"));
    output_form->addRow(q->tr("Audio dropout grace"), audio_grace);
    output_form->addRow(q->tr("Last-good then black (0 = hold)"),
                        last_good_timeout);
    prevent_sleep = new QCheckBox(q->tr(
        "Prevent device sleep while Live is running (supported platforms)"));
    output_form->addRow(q->tr("Show continuity"), prevent_sleep);
    auto* safety_label = new QLabel(q->tr(
        "The renderer keeps only one pending frame. A missed frame holds the last good image; "
        "the project watchdog may switch to black according to its saved safety policy."));
    safety_label->setWordWrap(true);
    output_form->addRow(safety_label);
    layout->addWidget(output);
    layout->addStretch(1);
    scroll->setWidget(body);

    QObject::connect(starter, &QPushButton::clicked, q, [this] { createStarterRig(); });
    QObject::connect(add_role, &QPushButton::clicked, q, [this] { addLogicalRole(); });
    QObject::connect(audio_role, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] {
        if (!rebuilding) {
            QSettings().setValue(QStringLiteral("live/audioRole"),
                                 audio_role->currentData());
        }
        route_warnings.clear();
        refreshDevices();
        restartAudio();
    });
    QObject::connect(audio_device, qOverload<int>(&QComboBox::currentIndexChanged), q, [this] {
        if (rebuilding || audio_role->currentData().toString().isEmpty()) return;
        const std::string role = narrow(audio_role->currentData().toString());
        QSettings settings;
        settings.setValue(endpoint_key(role, QStringLiteral("audioDevice")),
                          audio_device->currentData());
        settings.setValue(endpoint_key(role, QStringLiteral("audioDeviceName")),
                          audio_device->currentData(Qt::UserRole + 1));
        {
            const QSignalBlocker blocker(device_latency);
            device_latency->setValue(static_cast<int>(std::clamp<qint64>(
                settings.value(device_latency_key(
                                   role, audio_device->currentData().toString()),
                               0)
                    .toLongLong()
                    / 1000,
                (std::numeric_limits<int>::min)(), kMaximumUiInteger)));
        }
        emit q->audioInputsChanged();
        restartAudio();
    });
    QObject::connect(audio_refresh, &QPushButton::clicked, q, [this] {
        refreshDevices();
        emit q->audioInputsChanged();
        restartAudio();
    });
    const auto store_midi_binding = [this](QComboBox* role, QComboBox* device) {
        if (rebuilding || role->currentData().toString().isEmpty()) return;
        QSettings().setValue(endpoint_key(narrow(role->currentData().toString()),
                                          QStringLiteral("midiSource")),
                             device->currentData());
    };
    QObject::connect(midi_role, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { refreshDevices(); });
    QObject::connect(midi_device, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this, store_midi_binding] { store_midi_binding(midi_role, midi_device); });
    QObject::connect(foot_role, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { refreshDevices(); });
    QObject::connect(foot_device, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this, store_midi_binding] { store_midi_binding(foot_role, foot_device); });
    QObject::connect(osc_role, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { restartOsc(); });
    QObject::connect(osc_port, qOverload<int>(&QSpinBox::valueChanged), q, [this](int value) {
        if (rebuilding) return;
        QSettings().setValue(QStringLiteral("live/oscPort"), value);
        restartOsc();
    });
    QObject::connect(osc_local, &QCheckBox::toggled, q, [this](bool checked) {
        if (rebuilding) return;
        QSettings().setValue(QStringLiteral("live/oscLocalOnly"), checked);
        restartOsc();
    });
    QObject::connect(gain, &QDial::valueChanged, q, [this](int value) {
        const QSignalBlocker blocker(gain_value);
        gain_value->setValue(static_cast<double>(value));
        QSettings().setValue(QStringLiteral("live/audioGain"),
                             static_cast<double>(value));
        audio.set_gain(static_cast<double>(value) / 100.0);
    });
    QObject::connect(gain_value, qOverload<double>(&QDoubleSpinBox::valueChanged),
                     q, [this](double value) {
        const QSignalBlocker blocker(gain);
        gain->setValue(static_cast<int>(std::lround(
            std::min(value, 400.0))));
        QSettings().setValue(QStringLiteral("live/audioGain"), value);
        audio.set_gain(value / 100.0);
    });
    QObject::connect(sensitivity, &QDial::valueChanged, q, [this](int value) {
        const QSignalBlocker blocker(sensitivity_value);
        sensitivity_value->setValue(static_cast<double>(value));
        QSettings().setValue(QStringLiteral("live/audioSensitivity"),
                             static_cast<double>(value));
        audio.set_sensitivity(static_cast<double>(value) / 100.0);
    });
    QObject::connect(
        sensitivity_value,
        qOverload<double>(&QDoubleSpinBox::valueChanged), q,
        [this](double value) {
            const QSignalBlocker blocker(sensitivity);
            sensitivity->setValue(static_cast<int>(std::lround(
                std::min(value, 400.0))));
            QSettings().setValue(QStringLiteral("live/audioSensitivity"), value);
            audio.set_sensitivity(value / 100.0);
        });
    QObject::connect(audio_period, &QSpinBox::valueChanged, q,
                     [this] {
        QSettings().setValue(QStringLiteral("live/audioPeriodFrames"),
                             audio_period->value());
        restartAudio();
    });
    QObject::connect(latency, qOverload<int>(&QSpinBox::valueChanged), q, [this](int value) {
        if (rebuilding) return;
        pvt::LiveConfig next = config;
        const std::string role = narrow(audio_role->currentData().toString());
        if (auto* item = endpoint(next, role)) {
            item->input_latency_microseconds = static_cast<std::int64_t>(value) * 1000;
            commitConfig(std::move(next), q->tr("Change live input calibration"));
        }
    });
    QObject::connect(
        device_latency, qOverload<int>(&QSpinBox::valueChanged), q,
        [this](int value) {
        if (rebuilding) return;
        const std::string role = narrow(audio_role->currentData().toString());
        if (role.empty()) return;
        QSettings().setValue(
            device_latency_key(role, audio_device->currentData().toString()),
            static_cast<qint64>(value) * 1000);
    });
    QObject::connect(calibrate, &QPushButton::clicked, q, [this] { calibrateLatency(); });
    QObject::connect(audio_processing, &QPushButton::clicked, q, [this] {
        AudioProcessingDialog dialog(config.audio_processing,
                                     q->tr("Live input"), q);
        if (dialog.exec() != QDialog::Accepted) return;
        pvt::LiveConfig next = config;
        next.audio_processing = dialog.processing();
        commitConfig(std::move(next), q->tr("Change live audio input processing"));
    });
    QObject::connect(project_clock, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { editClockRoute(false); });
    QObject::connect(project_clock_role, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { editClockRoute(false); });
    QObject::connect(project_clock_stream, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { editClockRoute(false); });
    QObject::connect(layer_clock, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { editClockRoute(true); });
    QObject::connect(layer_clock_role, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { editClockRoute(true); });
    QObject::connect(layer_clock_stream, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { editClockRoute(true); });
    QObject::connect(project_clock_out, &QCheckBox::toggled, q,
                     [this] { editClockOutput(false); });
    QObject::connect(project_clock_out_role, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { editClockOutput(false); });
    QObject::connect(layer_clock_out, &QCheckBox::toggled, q,
                     [this] { editClockOutput(true); });
    QObject::connect(layer_clock_out_role, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [this] { editClockOutput(true); });
    QObject::connect(screen, qOverload<int>(&QComboBox::currentIndexChanged), q, [this] {
        if (rebuilding) return;
        q->setSelectedOutputDisplayId(screen->currentData().toString());
    });
    QObject::connect(quality, qOverload<int>(&QComboBox::currentIndexChanged), q, [this] {
        if (rebuilding) return;
        q->setOutputResolutionScale(quality->currentData().toDouble());
    });
    const auto author_output_safety = [this] {
        if (rebuilding) return;
        pvt::LiveConfig next = config;
        next.output.fullscreen = portable_fullscreen->isChecked();
        next.output.prefer_secondary_display = prefer_secondary->isChecked();
        next.output.hide_cursor = hide_stage_cursor->isChecked();
        next.safety.dropout_behavior = static_cast<pvt::LiveDropoutBehavior>(
            dropout_behavior->currentData().toInt());
        next.safety.frame_time_watchdog_enabled = watchdog_enabled->isChecked();
        next.safety.watchdog_timeout_milliseconds = watchdog_timeout->value();
        next.safety.audio_dropout_grace_milliseconds = audio_grace->value();
        next.safety.last_good_frame_timeout_milliseconds = last_good_timeout->value();
        next.safety.prevent_device_sleep = prevent_sleep->isChecked();
        commitConfig(std::move(next), q->tr("Change portable live output safety"));
    };
    QObject::connect(portable_fullscreen, &QCheckBox::toggled, q,
                     [author_output_safety] { author_output_safety(); });
    QObject::connect(prefer_secondary, &QCheckBox::toggled, q,
                     [author_output_safety] { author_output_safety(); });
    QObject::connect(hide_stage_cursor, &QCheckBox::toggled, q,
                     [author_output_safety] { author_output_safety(); });
    QObject::connect(dropout_behavior, qOverload<int>(&QComboBox::currentIndexChanged), q,
                     [author_output_safety] { author_output_safety(); });
    QObject::connect(watchdog_enabled, &QCheckBox::toggled, q,
                     [author_output_safety] { author_output_safety(); });
    QObject::connect(watchdog_timeout, qOverload<int>(&QSpinBox::valueChanged), q,
                     [author_output_safety] { author_output_safety(); });
    QObject::connect(audio_grace, qOverload<int>(&QSpinBox::valueChanged), q,
                     [author_output_safety] { author_output_safety(); });
    QObject::connect(last_good_timeout, qOverload<int>(&QSpinBox::valueChanged), q,
                     [author_output_safety] { author_output_safety(); });
    QObject::connect(prevent_sleep, &QCheckBox::toggled, q,
                     [author_output_safety] { author_output_safety(); });
    return scroll;
}

QWidget* LiveWorkspace::Impl::buildMappingTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    auto* intro = new QLabel(q->tr(
        "Map a logical MIDI, OSC, or foot-controller input to any render-safe "
        "setting, performance action, or scene. Live values are an ephemeral "
        "overlay—moving a pedal does not flood project history."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* tools = new QHBoxLayout;
    auto* add = new QPushButton(q->tr("Add Mapping…"));
    auto* edit = new QPushButton(q->tr("Edit…"));
    auto* remove = new QPushButton(q->tr("Remove"));
    learn_button = new QPushButton(q->tr("MIDI Learn"));
    tools->addWidget(add);
    tools->addWidget(edit);
    tools->addWidget(remove);
    tools->addWidget(learn_button);
    tools->addStretch(1);
    layout->addLayout(tools);
    mapping_table = new QTableWidget(0, 5);
    mapping_table->setHorizontalHeaderLabels(
        {q->tr("On"), q->tr("Name"), q->tr("Source"),
         q->tr("Target"), q->tr("Mode")});
    mapping_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    mapping_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    mapping_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    mapping_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    mapping_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    mapping_table->verticalHeader()->hide();
    mapping_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    mapping_table->setSelectionMode(QAbstractItemView::SingleSelection);
    mapping_table->setAlternatingRowColors(true);
    mapping_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(mapping_table, 1);
    auto* hint = new QLabel(q->tr(
        "Tip: MIDI Learn changes only the selected portable mapping. Runtime "
        "source names remain in this machine's preferences."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    QObject::connect(add, &QPushButton::clicked, q, [this] { editMapping(-1); });
    QObject::connect(edit, &QPushButton::clicked, q, [this] {
        editMapping(mapping_table->currentRow());
    });
    QObject::connect(remove, &QPushButton::clicked, q, [this] { removeMapping(); });
    QObject::connect(learn_button, &QPushButton::clicked, q, [this] { beginMidiLearn(); });
    QObject::connect(mapping_table, &QTableWidget::cellDoubleClicked, q,
                     [this](int row, int) { editMapping(row); });
    QObject::connect(mapping_table, &QTableWidget::cellClicked, q,
                     [this](int row, int column) {
        if (column != 0 || row < 0
            || row >= static_cast<int>(config.mappings.size())) return;
        pvt::LiveConfig next = config;
        next.mappings[static_cast<std::size_t>(row)].enabled =
            !next.mappings[static_cast<std::size_t>(row)].enabled;
        commitConfig(std::move(next), q->tr("Toggle live mapping"));
    });
    return page;
}

QWidget* LiveWorkspace::Impl::buildSceneTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    auto* intro = new QLabel(q->tr(
        "Scenes snapshot every currently resolved Live target. Numeric controls "
        "crossfade; switches and modes change at the end of the transition."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    scene_list = new QListWidget;
    scene_list->setAlternatingRowColors(true);
    scene_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(scene_list, 1);
    auto* transition_row = new QHBoxLayout;
    transition_row->addWidget(new QLabel(q->tr("Transition")));
    scene_transition_ms = new QSpinBox;
    scene_transition_ms->setRange(0, kMaximumUiInteger);
    scene_transition_ms->setSuffix(q->tr(" ms"));
    scene_transition_ms->setSingleStep(50);
    transition_row->addWidget(scene_transition_ms);
    transition_row->addStretch(1);
    layout->addLayout(transition_row);
    auto* capture_row = new QHBoxLayout;
    auto* capture = new QPushButton(q->tr("Capture New…"));
    auto* update = new QPushButton(q->tr("Update Snapshot"));
    auto* remove = new QPushButton(q->tr("Remove"));
    capture_row->addWidget(capture);
    capture_row->addWidget(update);
    capture_row->addWidget(remove);
    layout->addLayout(capture_row);
    auto* take_row = new QHBoxLayout;
    auto* previous = new QPushButton(q->tr("◀ Previous"));
    auto* take = new QPushButton(q->tr("TAKE SCENE"));
    take->setMinimumHeight(38);
    auto* next = new QPushButton(q->tr("Next ▶"));
    take_row->addWidget(previous);
    take_row->addWidget(take, 1);
    take_row->addWidget(next);
    layout->addLayout(take_row);
    auto* startup = new QCheckBox(q->tr("Use selected scene when Live starts"));
    layout->addWidget(startup);

    QObject::connect(capture, &QPushButton::clicked, q,
                     [this] { captureScene(false); });
    QObject::connect(update, &QPushButton::clicked, q,
                     [this] { captureScene(true); });
    QObject::connect(remove, &QPushButton::clicked, q, [this] { removeScene(); });
    QObject::connect(take, &QPushButton::clicked, q, [this] { takeSelectedScene(); });
    QObject::connect(previous, &QPushButton::clicked, q,
                     [this] { selectRelativeScene(-1); });
    QObject::connect(next, &QPushButton::clicked, q,
                     [this] { selectRelativeScene(1); });
    QObject::connect(scene_list, &QListWidget::itemDoubleClicked, q,
                     [this](QListWidgetItem*) { takeSelectedScene(); });
    QObject::connect(scene_list, &QListWidget::currentRowChanged, q, [this](int row) {
        QSignalBlocker block(scene_transition_ms);
        if (row >= 0 && row < static_cast<int>(config.scenes.size())) {
            scene_transition_ms->setValue(
                config.scenes[static_cast<std::size_t>(row)].transition_milliseconds);
        }
    });
    QObject::connect(scene_transition_ms, qOverload<int>(&QSpinBox::valueChanged), q,
                     [this](int value) {
        if (rebuilding) return;
        const int row = scene_list->currentRow();
        if (row < 0 || row >= static_cast<int>(config.scenes.size())) return;
        pvt::LiveConfig next = config;
        next.scenes[static_cast<std::size_t>(row)].transition_milliseconds = value;
        commitConfig(std::move(next), q->tr("Change live scene transition"));
    });
    QObject::connect(startup, &QCheckBox::toggled, q, [this](bool checked) {
        if (rebuilding) return;
        const int row = scene_list->currentRow();
        pvt::LiveConfig next = config;
        next.startup_scene_uuid = checked && row >= 0
                && row < static_cast<int>(next.scenes.size())
            ? next.scenes[static_cast<std::size_t>(row)].uuid : std::string{};
        commitConfig(std::move(next), q->tr("Change live startup scene"));
    });
    QObject::connect(scene_list, &QListWidget::currentRowChanged, q,
                     [this, startup](int row) {
        QSignalBlocker block(startup);
        startup->setChecked(row >= 0
            && row < static_cast<int>(config.scenes.size())
            && config.scenes[static_cast<std::size_t>(row)].uuid
                   == config.startup_scene_uuid);
    });
    return page;
}

void LiveWorkspace::Impl::connectRuntime() {
    QObject::connect(&renderer, &LiveFrameController::frameFinished, q,
                     [this](const LiveFrameController::Result& result) {
        frameFinished(result);
    });
    QObject::connect(&renderer, &LiveFrameController::watchdogExpired, q,
                     [this](std::uint64_t sequence,
                            std::uint64_t session_generation,
                            std::uint64_t document_revision) {
        const std::uint64_t current_revision = document_revision_provider
            ? document_revision_provider() : 0U;
        if (!realtimeActive() || session_generation != render_generation
            || document_revision != current_revision) {
            return;
        }
        ++late_streak;
        good_streak = 0;
        render_lamp->setState(StatusLamp::State::Warning);
        render_lamp->setText(q->tr("WATCHDOG"));
        render_lamp->setToolTip(q->tr("Frame %1 exceeded its live deadline; the last good frame remains on stage.")
                                    .arg(sequence));
        if (quality->currentData().toDouble() == 0.0) {
            adaptive_scale = std::max(0.25, adaptive_scale * 0.8);
        }
        if (presentation_active) {
            emit q->runtimeStatusChanged(q->tr(
                "Live Preview Output frame %1 exceeded its 60-second render watchdog; the last delivered frame remains visible and Auto quality was reduced.")
                .arg(sequence));
        }
        // Presentation output has no performance safety blackout. Its shared
        // quality controller still degrades and reports the timeout above.
        if (active && !user_freeze
            && config.safety.dropout_behavior
                   == pvt::LiveDropoutBehavior::Blackout) {
            safety_blackout = true;
            updateOutputState();
        }
    });
    QObject::connect(&midi, &LiveMidiRouter::controlMessage, q,
                     [this](LiveMidiRouter::MessageKind kind, int channel,
                            int number, double value, const QString& source) {
        handleMidi(kind, channel, number, value, source);
    });
    QObject::connect(&midi, &LiveMidiRouter::clockActivityChanged, q, [this] {
        const auto clock = midi.clockSnapshot();
        midi_lamp->setState(clock.receiving ? StatusLamp::State::Ready
                                            : StatusLamp::State::Warning);
    });
    QObject::connect(&midi, &LiveMidiRouter::endpointsChanged, q,
                     [this] { refreshDevices(); });
    QObject::connect(&midi, &LiveMidiRouter::runtimeError, q,
                     [this](const QString& message) {
        midi_lamp->setState(StatusLamp::State::Fault);
        midi_lamp->setToolTip(message);
        emit q->runtimeStatusChanged(message);
    });
    QObject::connect(&osc, &LiveOscRouter::valueMessage, q,
                     [this](const QString& address, double value,
                            const QString& sender) {
        handleOsc(address, value, sender);
    });
    QObject::connect(&osc, &LiveOscRouter::runtimeError, q,
                     [this](const QString& message) {
        emit q->runtimeStatusChanged(message);
    });
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        QObject::connect(app, &QGuiApplication::screenAdded, q,
                         [this](QScreen*) { refreshScreens(); });
        QObject::connect(app, &QGuiApplication::screenRemoved, q,
                         [this](QScreen*) { refreshScreens(); });
        QObject::connect(app, &QGuiApplication::primaryScreenChanged, q,
                         [this](QScreen*) { refreshScreens(); });
    }
}

void LiveWorkspace::Impl::commitConfig(pvt::LiveConfig next,
                                       const QString& reason) {
    if (!project_editing_enabled) {
        refreshConfigUi();
        return;
    }
    const pvt::ValidationResult validation = pvt::validate(next);
    if (!validation.ok) {
        QMessageBox::warning(q, q->tr("Live configuration"),
                             q->tr("That patch would make the Live rig invalid:\n%1")
                                 .arg(qtext(validation.message)));
        return;
    }
    const bool outputs_changed = clock_output_signature(config)
        != clock_output_signature(next);
    const bool endpoints_changed = endpoint_structure_signature(config)
        != endpoint_structure_signature(next);
    config = next;
    if (authored_editor) authored_editor(config, reason);
    refreshConfigUi();
    if (active) {
        updateSleepPrevention();
        if (outputs_changed) configureClockOutputs();
        if (endpoints_changed) {
            restartAudio();
            restartOsc();
        }
    }
    refreshRuntimeRouting();
    if (stage.isVisible()) applyVisibleOutputPolicy();
}

void LiveWorkspace::Impl::refreshConfigUi() {
    rebuilding = true;
    refreshRoleCombos();
    refreshMappings();
    refreshScenes();
    refreshClockRouting();
    portable_fullscreen->setChecked(config.output.fullscreen);
    prefer_secondary->setChecked(config.output.prefer_secondary_display);
    hide_stage_cursor->setChecked(config.output.hide_cursor);
    dropout_behavior->setCurrentIndex(std::max(0, dropout_behavior->findData(
        static_cast<int>(config.safety.dropout_behavior))));
    watchdog_enabled->setChecked(config.safety.frame_time_watchdog_enabled);
    watchdog_timeout->setValue(config.safety.watchdog_timeout_milliseconds);
    audio_grace->setValue(config.safety.audio_dropout_grace_milliseconds);
    last_good_timeout->setValue(config.safety.last_good_frame_timeout_milliseconds);
    prevent_sleep->setChecked(config.safety.prevent_device_sleep);
    watchdog_timeout->setEnabled(config.safety.frame_time_watchdog_enabled);
    rebuilding = false;
    refreshDevices();
}

void LiveWorkspace::Impl::rebuildTargetCache() {
    target_cache.clear();
    target_index.clear();
    if (!project_provider) return;
    project_cache = project_provider();
    project_cache_valid = true;
    target_cache = buildLiveTargetRegistry(project_cache);
    target_index.reserve(static_cast<qsizetype>(target_cache.size()));
    for (int index = 0; index < static_cast<int>(target_cache.size()); ++index) {
        target_index.insert(target_cache[static_cast<std::size_t>(index)].path, index);
    }
}

const pvt::LiveEndpointConfig* LiveWorkspace::Impl::endpoint(
    const std::string& uuid) const {
    const auto found = std::find_if(
        config.endpoints.begin(), config.endpoints.end(),
        [&uuid](const pvt::LiveEndpointConfig& item) { return item.uuid == uuid; });
    return found == config.endpoints.end() ? nullptr : &*found;
}

pvt::LiveEndpointConfig* LiveWorkspace::Impl::endpoint(
    pvt::LiveConfig& value, const std::string& uuid) const {
    const auto found = std::find_if(
        value.endpoints.begin(), value.endpoints.end(),
        [&uuid](const pvt::LiveEndpointConfig& item) { return item.uuid == uuid; });
    return found == value.endpoints.end() ? nullptr : &*found;
}

std::string LiveWorkspace::Impl::preferredAudioRole() const {
    const auto usable = [this](const std::string& uuid) {
        const auto* role = endpoint(uuid);
        return role != nullptr
            && role->protocol == pvt::LiveEndpointProtocol::Audio
            && direction_has_input(role->direction);
    };
    const std::string active_layer = active_layer_provider
        ? active_layer_provider() : std::string{};
    for (const auto& route : config.clock_inputs) {
        if (route.enabled
            && route.source == pvt::LiveClockInputSource::AudioStream
            && route.target == pvt::LiveClockTarget::Project
            && usable(route.endpoint_uuid)) {
            return route.endpoint_uuid;
        }
    }
    for (const auto& route : config.clock_inputs) {
        if (route.enabled
            && route.source == pvt::LiveClockInputSource::AudioStream
            && route.target == pvt::LiveClockTarget::Layer
            && route.layer_uuid == active_layer
            && usable(route.endpoint_uuid)) {
            return route.endpoint_uuid;
        }
    }
    const std::string stored = narrow(
        QSettings().value(QStringLiteral("live/audioRole")).toString());
    if (usable(stored)) return stored;
    const std::string current = audio_role == nullptr
        ? std::string{} : narrow(audio_role->currentData().toString());
    return usable(current) ? current : std::string{};
}

bool LiveWorkspace::Impl::effectiveAudioRoute(
    const pvt::LiveClockInputConfig& input) const {
    if (!input.enabled
        || input.source != pvt::LiveClockInputSource::AudioStream) {
        return false;
    }
    if (input.target == pvt::LiveClockTarget::Project) return true;
    const std::string active_layer = active_layer_provider
        ? active_layer_provider() : std::string{};
    return !active_layer.empty() && input.layer_uuid == active_layer;
}

bool LiveWorkspace::Impl::effectiveAudioClockReceiving() const {
    const std::string selected_role = audio_role == nullptr
        ? std::string{} : narrow(audio_role->currentData().toString());
    bool found = false;
    for (const auto& input : config.clock_inputs) {
        if (!effectiveAudioRoute(input)) continue;
        found = true;
        if (input.endpoint_uuid != selected_role
            || !pvt::audio::live_audio_callback_within_holdover(
                live_audio_loss_age_seconds(
                    audio_snapshot, audio_dropout_clock),
                input.holdover_milliseconds)) {
            return false;
        }
        if (!input.frequency_stream_uuid.empty()) {
            const bool stream_found = std::any_of(
                audio_snapshot.frequency_streams.begin(),
                audio_snapshot.frequency_streams.end(),
                [&input](
                    const pvt::audio::LiveAudioSnapshot::FrequencyStream& item) {
                    return item.uuid == input.frequency_stream_uuid;
                });
            if (!stream_found) return false;
        }
    }
    return !found || audio_snapshot.receiving;
}

bool LiveWorkspace::Impl::shouldHoldAudioFrame() const {
    if (!active || config.safety.dropout_behavior
                       != pvt::LiveDropoutBehavior::LastGoodFrame) {
        return false;
    }
    const std::string selected_role = audio_role == nullptr
        ? std::string{} : narrow(audio_role->currentData().toString());
    for (const auto& input : config.clock_inputs) {
        if (!effectiveAudioRoute(input)) continue;
        if (input.endpoint_uuid != selected_role) return true;
        if (!input.frequency_stream_uuid.empty()) {
            const bool stream_found = std::any_of(
                audio_snapshot.frequency_streams.begin(),
                audio_snapshot.frequency_streams.end(),
                [&input](
                    const pvt::audio::LiveAudioSnapshot::FrequencyStream& item) {
                    return item.uuid == input.frequency_stream_uuid;
                });
            if (!stream_found) return true;
        }
        if (!pvt::audio::live_audio_callback_within_holdover(
                live_audio_loss_age_seconds(
                    audio_snapshot, audio_dropout_clock),
                input.holdover_milliseconds)) {
            return true;
        }
    }
    return false;
}

void LiveWorkspace::Impl::warnRouteOnce(const QString& key,
                                        const QString& message) {
    if (route_warnings.contains(key)) return;
    route_warnings.insert(key);
    emit q->runtimeStatusChanged(message);
}

void LiveWorkspace::Impl::refreshRoleCombos() {
    const auto fill = [this](QComboBox* combo, pvt::LiveEndpointProtocol protocol,
                             bool input, bool output,
                             const QString& preferred) {
        const QString before = preferred.isEmpty()
            ? combo->currentData().toString() : preferred;
        QSignalBlocker block(combo);
        combo->clear();
        combo->addItem(q->tr("No logical role"), QString{});
        for (const auto& item : config.endpoints) {
            if (item.protocol != protocol
                || (input && !direction_has_input(item.direction))
                || (output && !direction_has_output(item.direction))) continue;
            combo->addItem(qtext(item.name), qtext(item.uuid));
        }
        int index = combo->findData(before);
        if (index < 0 && combo->count() > 1) index = 1;
        combo->setCurrentIndex(std::max(0, index));
    };
    fill(audio_role, pvt::LiveEndpointProtocol::Audio, true, false,
         qtext(preferredAudioRole()));
    fill(midi_role, pvt::LiveEndpointProtocol::Midi, true, false, {});
    fill(foot_role, pvt::LiveEndpointProtocol::FootController, true, false, {});
    fill(osc_role, pvt::LiveEndpointProtocol::Osc, true, false, {});
    fill(project_clock_out_role, pvt::LiveEndpointProtocol::Midi, false, true, {});
    fill(layer_clock_out_role, pvt::LiveEndpointProtocol::Midi, false, true, {});
    if (!audio_role->currentData().toString().isEmpty()) {
        QSettings().setValue(QStringLiteral("live/audioRole"),
                             audio_role->currentData());
    }
}

void LiveWorkspace::Impl::refreshMappings() {
    const int selected = mapping_table->currentRow();
    QSignalBlocker block(mapping_table);
    mapping_table->setRowCount(static_cast<int>(config.mappings.size()));
    for (int row = 0; row < static_cast<int>(config.mappings.size()); ++row) {
        const auto& mapping = config.mappings[static_cast<std::size_t>(row)];
        QString target;
        if (mapping.target == pvt::LiveMappingTarget::Setting) {
            target = qtext(mapping.target_path);
            if (project_provider) {
                const auto registry = buildLiveTargetRegistry(project_provider());
                const auto found = std::find_if(
                    registry.begin(), registry.end(),
                    [&mapping](const LiveTargetDescriptor& item) {
                        return narrow(item.path) == mapping.target_path;
                    });
                if (found != registry.end()) target = found->section + QStringLiteral(" · ") + found->label;
                else target = q->tr("Unresolved · %1").arg(target);
            }
        } else if (mapping.target == pvt::LiveMappingTarget::Action) {
            target = action_name(mapping.action);
        } else {
            const auto found = std::find_if(
                config.scenes.begin(), config.scenes.end(),
                [&mapping](const pvt::LiveSceneConfig& scene) {
                    return scene.uuid == mapping.scene_uuid;
                });
            target = found == config.scenes.end()
                ? q->tr("Unresolved scene") : qtext(found->name);
        }
        auto* enabled = new QTableWidgetItem(mapping.enabled ? QStringLiteral("●")
                                                             : QStringLiteral("○"));
        enabled->setTextAlignment(Qt::AlignCenter);
        mapping_table->setItem(row, 0, enabled);
        mapping_table->setItem(row, 1, new QTableWidgetItem(qtext(mapping.name)));
        mapping_table->setItem(row, 2, new QTableWidgetItem(mapping_source(mapping)));
        mapping_table->setItem(row, 3, new QTableWidgetItem(target));
        mapping_table->setItem(row, 4, new QTableWidgetItem(mapping_mode_name(mapping.mode)));
    }
    if (selected >= 0 && selected < mapping_table->rowCount()) {
        mapping_table->selectRow(selected);
    }
    if (learn_mapping >= static_cast<int>(config.mappings.size())) learn_mapping = -1;
    learn_button->setText(learn_mapping >= 0 ? q->tr("Listening…") : q->tr("MIDI Learn"));
}

void LiveWorkspace::Impl::refreshScenes() {
    const QString selected = scene_list->currentItem()
        ? scene_list->currentItem()->data(Qt::UserRole).toString() : QString{};
    QSignalBlocker block(scene_list);
    scene_list->clear();
    for (const auto& scene : config.scenes) {
        auto* item = new QListWidgetItem(
            q->tr("%1    %2 targets    %3 ms")
                .arg(qtext(scene.name))
                .arg(scene.values.size())
                .arg(scene.transition_milliseconds));
        item->setData(Qt::UserRole, qtext(scene.uuid));
        if (scene.uuid == config.startup_scene_uuid) {
            item->setText(QStringLiteral("★  ") + item->text());
        }
        scene_list->addItem(item);
    }
    int row = -1;
    for (int i = 0; i < scene_list->count(); ++i) {
        if (scene_list->item(i)->data(Qt::UserRole).toString() == selected) {
            row = i;
            break;
        }
    }
    if (row < 0 && scene_list->count() > 0) row = 0;
    scene_list->setCurrentRow(row);
    if (row >= 0) {
        QSignalBlocker transition_block(scene_transition_ms);
        scene_transition_ms->setValue(
            config.scenes[static_cast<std::size_t>(row)].transition_milliseconds);
    }
}

void LiveWorkspace::Impl::refreshClockRouting() {
    const std::string layer_uuid = active_layer_provider ? active_layer_provider() : std::string{};
    const auto refresh_input = [this, &layer_uuid](bool layerTarget,
                                                   QComboBox* source,
                                                   QComboBox* role,
                                                   QComboBox* stream) {
        const auto found = std::find_if(
            config.clock_inputs.begin(), config.clock_inputs.end(),
            [layerTarget, &layer_uuid](const pvt::LiveClockInputConfig& item) {
                return item.enabled
                    && item.target == (layerTarget ? pvt::LiveClockTarget::Layer
                                                   : pvt::LiveClockTarget::Project)
                    && (!layerTarget || item.layer_uuid == layer_uuid);
            });
        QSignalBlocker b1(source);
        QSignalBlocker b2(role);
        QSignalBlocker b3(stream);
        const int source_value = found == config.clock_inputs.end()
            ? -1 : static_cast<int>(found->source);
        source->setCurrentIndex(std::max(0, source->findData(source_value)));
        role->clear();
        role->addItem(q->tr("Choose role"), QString{});
        const pvt::LiveEndpointProtocol protocol =
            source_value == static_cast<int>(pvt::LiveClockInputSource::AudioStream)
                ? pvt::LiveEndpointProtocol::Audio : pvt::LiveEndpointProtocol::Midi;
        for (const auto& item : config.endpoints) {
            if (item.protocol == protocol && direction_has_input(item.direction)) {
                role->addItem(qtext(item.name), qtext(item.uuid));
            }
        }
        if (found != config.clock_inputs.end()) {
            const int index = role->findData(qtext(found->endpoint_uuid));
            if (index >= 0) role->setCurrentIndex(index);
        }
        role->setEnabled(source_value >= 0);
        stream->clear();
        stream->addItem(q->tr("Full filtered signal"), QString{});
        for (const auto& item : config.audio_processing.frequency_streams) {
            stream->addItem(qtext(item.name), qtext(item.uuid));
        }
        if (found != config.clock_inputs.end()) {
            const int index = stream->findData(
                qtext(found->frequency_stream_uuid));
            if (index >= 0) stream->setCurrentIndex(index);
        }
        stream->setEnabled(
            source_value
            == static_cast<int>(pvt::LiveClockInputSource::AudioStream));
    };
    refresh_input(false, project_clock, project_clock_role,
                  project_clock_stream);
    refresh_input(true, layer_clock, layer_clock_role,
                  layer_clock_stream);

    const auto refresh_output = [this, &layer_uuid](bool layerTarget,
                                                    QCheckBox* enabled,
                                                    QComboBox* role) {
        const auto found = std::find_if(
            config.midi_clock_outputs.begin(), config.midi_clock_outputs.end(),
            [layerTarget, &layer_uuid](const pvt::LiveMidiClockOutputConfig& item) {
                return item.source == (layerTarget ? pvt::LiveClockTarget::Layer
                                                   : pvt::LiveClockTarget::Project)
                    && (!layerTarget || item.layer_uuid == layer_uuid);
            });
        QSignalBlocker b1(enabled);
        QSignalBlocker b2(role);
        enabled->setChecked(found != config.midi_clock_outputs.end() && found->enabled);
        if (found != config.midi_clock_outputs.end()) {
            const int index = role->findData(qtext(found->endpoint_uuid));
            if (index >= 0) role->setCurrentIndex(index);
        }
        role->setEnabled(enabled->isChecked());
    };
    refresh_output(false, project_clock_out, project_clock_out_role);
    refresh_output(true, layer_clock_out, layer_clock_out_role);

    const auto* audio_endpoint = endpoint(narrow(audio_role->currentData().toString()));
    {
        QSignalBlocker block(latency);
        const qint64 authored_milliseconds = audio_endpoint == nullptr ? 0
            : audio_endpoint->input_latency_microseconds / 1000;
        const qint64 minimum = static_cast<qint64>(
            (std::numeric_limits<int>::min)());
        const qint64 maximum = static_cast<qint64>(kMaximumUiInteger);
        const qint64 displayed = std::clamp(
            authored_milliseconds, minimum, maximum);
        latency->setValue(static_cast<int>(displayed));
        const bool clipped = displayed != authored_milliseconds;
        latency->setToolTip(clipped
            ? q->tr("The authored portable offset is outside this editor's range. The boundary is displayed, but the exact project value is preserved until you edit it.")
            : q->tr("Portable rig offset. Positive values advance a beat that reaches the analyzer late; negative values delay an early source."));
        if (clipped && audio_endpoint != nullptr) {
            warnRouteOnce(
                qtext(audio_endpoint->uuid)
                    + QStringLiteral(":portable-latency-ui-range"),
                q->tr("The portable input offset for %1 is outside the millisecond editor range; its exact authored value remains preserved.")
                    .arg(qtext(audio_endpoint->name)));
        }
    }
}

void LiveWorkspace::Impl::refreshDevices() {
    rebuilding = true;
    const auto restore_combo = [](QComboBox* combo, const QStringList& names,
                                  const QString& stored, const QString& emptyLabel) {
        QSignalBlocker block(combo);
        combo->clear();
        combo->addItem(emptyLabel, QString{});
        for (const QString& name : names) combo->addItem(name, name);
        int index = combo->findData(stored);
        if (index < 0 && combo->count() > 1) index = 1;
        combo->setCurrentIndex(std::max(0, index));
    };
    {
        QSignalBlocker block(audio_device);
        audio_device->clear();
        audio_device->addItem(q->tr("System default"), QString{});
        audio_device->setItemData(0, q->tr("System default"),
                                  Qt::UserRole + 1);
        std::string discovery_error;
        audio_devices = audio.devices(&discovery_error);
        for (const auto& device : audio_devices) {
            audio_device->addItem(device.is_default
                                      ? q->tr("%1 · default").arg(
                                            qtext(device.display_name))
                                      : qtext(device.display_name),
                                  qtext(device.runtime_id));
            audio_device->setItemData(
                audio_device->count() - 1, qtext(device.name),
                Qt::UserRole + 1);
        }
        const std::string role = narrow(audio_role->currentData().toString());
        QSettings settings;
        const QString stored = settings.value(
            endpoint_key(role, QStringLiteral("audioDevice"))).toString();
        QString selected = stored;
        if (!stored.isEmpty()) {
            std::string match_error;
            const auto match = pvt::audio::find_live_audio_device(
                audio_devices, narrow(stored), &match_error);
            if (match.has_value()) {
                selected = qtext(audio_devices[*match].runtime_id);
                settings.setValue(
                    endpoint_key(role, QStringLiteral("audioDevice")),
                    selected);
                settings.setValue(
                    endpoint_key(role, QStringLiteral("audioDeviceName")),
                    qtext(audio_devices[*match].name));
            }
        }
        int index = audio_device->findData(selected);
        if (!stored.isEmpty() && index < 0) {
            QString hint = settings.value(
                endpoint_key(role, QStringLiteral("audioDeviceName")))
                               .toString().trimmed();
            if (hint.isEmpty()) hint = q->tr("previously selected input");
            audio_device->insertItem(
                1, q->tr("Unavailable · %1").arg(hint), stored);
            audio_device->setItemData(1, hint, Qt::UserRole + 1);
            index = 1;
        }
        if (index < 0) index = 0;
        audio_device->setCurrentIndex(index);
        if (device_latency != nullptr) {
            const qint64 local_microseconds = settings.value(
                device_latency_key(role, audio_device->currentData().toString()),
                0).toLongLong();
            device_latency->setValue(static_cast<int>(std::clamp<qint64>(
                local_microseconds / 1000,
                (std::numeric_limits<int>::min)(), kMaximumUiInteger)));
            device_latency->setEnabled(!role.empty());
        }
        if (!discovery_error.empty()) {
            audio_device->setToolTip(q->tr("Input discovery failed: %1")
                                         .arg(qtext(discovery_error)));
        } else if (!stored.isEmpty() && index == 1
                   && audio_device->itemText(1).startsWith(
                       q->tr("Unavailable"))) {
            audio_device->setToolTip(q->tr(
                "The saved microphone is unavailable. Live will not capture the system default unless you choose it explicitly."));
        } else {
            audio_device->setToolTip(q->tr(
                "This device binding is stored only on this computer."));
        }
    }
    const QStringList midi_names = midi.inputNames();
    const auto fill_midi = [&](QComboBox* role, QComboBox* device) {
        const std::string uuid = narrow(role->currentData().toString());
        const QString stored = QSettings().value(
            endpoint_key(uuid, QStringLiteral("midiSource"))).toString();
        restore_combo(device, midi_names, stored, q->tr("Any MIDI source"));
    };
    fill_midi(midi_role, midi_device);
    fill_midi(foot_role, foot_device);
    rebuilding = false;
    refreshClockRouting();
    emit q->audioInputsChanged();
}

void LiveWorkspace::Impl::refreshRuntimeRouting() {
    const QString next = clock_input_signature(
        config, active_layer_provider ? active_layer_provider()
                                      : std::string{});
    if (next == runtime_input_signature) return;
    runtime_input_signature = next;
    route_warnings.clear();
    audio_dropout_clock.restart();
    if (active) {
        // Clock-input edits and active-layer changes can select a different
        // MIDI source or Audio role even when endpoint/output tables did not
        // structurally change.
        restartAudio();
        configureClockOutputs();
    }
}

void LiveWorkspace::Impl::refreshScreens() {
    const QString stored = QSettings().value(QStringLiteral("live/outputScreen")).toString();
    const QString before = screen == nullptr ? QString{} : screen->currentData().toString();
    const QString before_name = screen == nullptr
        ? QString{} : screen->currentData(Qt::UserRole + 1).toString();
    if (screen == nullptr) return;
    for (const auto& connection : screen_connections) {
        QObject::disconnect(connection);
    }
    screen_connections.clear();
    QSignalBlocker block(screen);
    screen->clear();
    const auto screens = QGuiApplication::screens();
    for (int index = 0; index < screens.size(); ++index) {
        QScreen* item = screens[index];
        const bool primary = item == QGuiApplication::primaryScreen();
        screen->addItem(screen_label(item, primary, screens),
                        screen_identifier(item, screens));
        screen->setItemData(index, item->name(), Qt::UserRole + 1);
        screen->setItemData(index, screen_base_identifier(item),
                            Qt::UserRole + 2);
        screen_connections.push_back(QObject::connect(
            item, &QScreen::geometryChanged, q,
            [this](const QRect&) { refreshScreens(); }));
        screen_connections.push_back(QObject::connect(
            item, &QScreen::availableGeometryChanged, q,
            [this](const QRect&) { refreshScreens(); }));
        screen_connections.push_back(QObject::connect(
            item, &QScreen::logicalDotsPerInchChanged, q,
            [this](qreal) { refreshScreens(); }));
    }
    QString wanted = before.isEmpty() ? stored : before;
    int index = screen->findData(wanted);
    if (index < 0) {
        for (int candidate = 0; candidate < screen->count(); ++candidate) {
            if (screen->itemData(candidate, Qt::UserRole + 2).toString()
                == wanted) {
                index = candidate;
                break;
            }
        }
    }
    if (index < 0) {
        const QString legacy_name = before_name.isEmpty() ? stored : before_name;
        for (int candidate = 0; candidate < screen->count(); ++candidate) {
            if (screen->itemData(candidate, Qt::UserRole + 1).toString()
                == legacy_name) {
                index = candidate;
                break;
            }
        }
    }
    if (index < 0 && config.output.prefer_secondary_display && screens.size() > 1) {
        index = screens.indexOf(QGuiApplication::primaryScreen()) == 0 ? 1 : 0;
    }
    screen->setCurrentIndex(std::max(0, index));
    if (screen->currentIndex() >= 0) {
        QSettings().setValue(QStringLiteral("live/outputScreen"),
                             screen->currentData().toString());
    }
    if (stage.isVisible()) applyVisibleOutputPolicy();
    emit q->runtimeOutputSettingsChanged();
}

bool LiveWorkspace::Impl::realtimeActive() const noexcept {
    return active || presentation_active;
}

void LiveWorkspace::Impl::updateScheduledFps(double fps) {
    const double sanitized = sanitized_realtime_fps(fps);
    if (std::abs(scheduled_fps - sanitized) <= 1.0e-9
        && frame_schedule_clock.isValid()) {
        return;
    }
    scheduled_fps = sanitized;
    frame_schedule_tick = 0U;
    frame_schedule_clock.restart();
}

void LiveWorkspace::Impl::restartFrameSchedule(double fps) {
    render_timer.stop();
    scheduled_fps = sanitized_realtime_fps(fps);
    frame_schedule_tick = 0U;
    frame_schedule_clock.restart();
    scheduleNextFrame();
}

void LiveWorkspace::Impl::scheduleNextFrame() {
    if (!realtimeActive()) return;
    if (!frame_schedule_clock.isValid()) frame_schedule_clock.start();
    const double elapsed = static_cast<double>(frame_schedule_clock.elapsed());
    const std::uint64_t elapsed_tick = static_cast<std::uint64_t>(
        std::max(0.0, std::floor(elapsed * scheduled_fps / 1000.0)));
    frame_schedule_tick = std::max(frame_schedule_tick + 1U, elapsed_tick + 1U);
    const double due = static_cast<double>(frame_schedule_tick)
                       * 1000.0 / scheduled_fps;
    const int delay = std::max(1, static_cast<int>(std::ceil(due - elapsed)));
    render_timer.start(delay);
}

void LiveWorkspace::Impl::setActive(bool value) {
    if (active == value) {
        QSignalBlocker block(live_button);
        live_button->setChecked(value);
        updateSleepPrevention();
        return;
    }
    if (value && presentation_active) setPresentationActive(false);
    active = value;
    {
        QSignalBlocker block(live_button);
        live_button->setChecked(value);
        live_button->setText(value ? q->tr("LIVE · ON") : q->tr("GO LIVE"));
    }
    if (value) {
        // Publish ownership before launching the first realtime render so the
        // editor can cancel queued/in-flight preview work first.
        emit q->liveActiveChanged(true);
        ++render_generation;
        renderer.cancelCurrent();
        audio_snapshot = {};
        last_image = {};
        stage.clearFrame();
        presented_frame_clock.invalidate();
        updateSleepPrevention();
        run_clock.restart();
        tempo_taps.clear();
        tapped_bpm = 0.0;
        tapped_beat_count = 0U;
        tapped_beat_anchor_seconds = 0.0;
        last_good_clock.invalidate();
        audio_dropout_clock.restart();
        late_streak = 0;
        good_streak = 0;
        render_failed = false;
        adaptive_scale = 1.0;
        showStartingState();
        startIo();
        const pvt::ProjectConfig project = project_provider
            ? project_provider() : pvt::default_project();
        restartFrameSchedule(project.canvas.fps);
        ui_timer.start();
        midi_clock_timer.start();
        if (!config.startup_scene_uuid.empty()) takeScene(config.startup_scene_uuid);
        requestFrame();
        emit q->runtimeStatusChanged(q->tr("Live performance runtime started."));
    } else {
        ++render_generation;
        (void)sleep_guard.setPrevented(false);
        render_timer.stop();
        ui_timer.stop();
        midi_clock_timer.stop();
        stage.setFrozen(false);
        stage.setBlackout(false);
        // Ask the window system to leave the stage/full-screen Space before
        // waiting for a cooperative render cancellation to drain.
        stage.dismiss();
        renderer.stop();
        stopIo();
        scene_transition = {};
        overrides.clear();
        mapping_runtime.clear();
        tempo_taps.clear();
        tapped_bpm = 0.0;
        tapped_beat_count = 0U;
        tapped_beat_anchor_seconds = 0.0;
        learn_mapping = -1;
        user_freeze = false;
        user_blackout = false;
        safety_blackout = false;
        render_failed = false;
        {
            QSignalBlocker f(freeze_button);
            freeze_button->setChecked(false);
        }
        {
            QSignalBlocker b(blackout_button);
            blackout_button->setChecked(false);
        }
        stage.clearFrame();
        audio_snapshot = {};
        last_image = {};
        presented_frame_clock.invalidate();
        output_button->setChecked(false);
        showStandbyState();
        emit q->runtimeStatusChanged(q->tr("Live performance runtime stopped."));
        // A false transition is published only after the synchronous renderer
        // drain so editor export/preview work cannot overlap teardown.
        emit q->liveActiveChanged(false);
    }
}

void LiveWorkspace::Impl::setPresentationActive(bool value) {
    if (presentation_active == value) return;
    if (value && active) setActive(false);
    presentation_active = value;
    if (value) {
        // Give the editor a chance to surrender its preview budget before the
        // presentation controller submits the first frame.
        emit q->presentationActiveChanged(true);
    }
    ++render_generation;
    if (value) {
        renderer.cancelCurrent();
        adaptive_scale = 1.0;
        late_streak = 0;
        good_streak = 0;
        render_failed = false;
        safety_blackout = false;
        last_image = {};
        stage.clearFrame();
        stage.setFrozen(false);
        stage.setBlackout(false);
        presented_frame_clock.invalidate();
        showStartingState();
        const pvt::ProjectConfig project = presentation_project_provider
            ? presentation_project_provider()
            : (project_provider ? project_provider() : pvt::default_project());
        restartFrameSchedule(project.canvas.fps);
        showOutput();
        requestFrame();
        emit q->runtimeStatusChanged(
            q->tr("Live Preview Output started without performance inputs."));
    } else {
        render_timer.stop();
        stage.setFrozen(false);
        stage.setBlackout(false);
        // Exit the stage surface promptly even if the in-flight renderer
        // needs a moment to observe cooperative cancellation.
        stage.dismiss();
        // Do not report the real-time output as inactive while a Metal frame
        // can still drain: export admission relies on this synchronous,
        // cooperative teardown having completed.
        renderer.stop();
        stage.clearFrame();
        last_image = {};
        presented_frame_clock.invalidate();
        output_button->setChecked(false);
        showStandbyState();
        emit q->runtimeStatusChanged(q->tr("Live Preview Output stopped."));
        emit q->presentationActiveChanged(false);
    }
}

void LiveWorkspace::Impl::resetRealtimeFrame() {
    ++render_generation;
    renderer.cancelCurrent();
    last_image = {};
    stage.clearFrame();
    last_good_clock.invalidate();
    presented_frame_clock.invalidate();
    if (realtimeActive()) requestFrame();
}

void LiveWorkspace::Impl::updateSleepPrevention() {
    QString error;
    if (!sleep_guard.setPrevented(
            active && config.safety.prevent_device_sleep, &error)
        && !error.isEmpty()) {
        emit q->runtimeStatusChanged(error);
    }
}

void LiveWorkspace::Impl::startIo() {
    QString midi_error;
    if (midi.start(&midi_error)) {
        midi_lamp->setState(StatusLamp::State::Ready);
        midi_lamp->setToolTip(q->tr("Core MIDI routing is active."));
    } else {
        midi_lamp->setState(StatusLamp::State::Warning);
        midi_lamp->setToolTip(midi_error);
    }
    refreshDevices();
    restartAudio();
    restartOsc();
    configureClockOutputs();
}

void LiveWorkspace::Impl::stopIo() {
    int output_index = 0;
    for (const auto& output : config.midi_clock_outputs) {
        if (!output.enabled) continue;
        if (output.send_transport) midi.sendClockStop(output_index);
        ++output_index;
    }
    audio.stop();
    osc.stop();
    midi.stop();
    clock_outputs.clear();
}

void LiveWorkspace::Impl::restartAudio() {
    if (!active) return;
    audio.stop();
    if (audio_role->currentData().toString().isEmpty()) {
        audio_lamp->setState(StatusLamp::State::Off);
        audio_lamp->setToolTip(q->tr("Add or select an Audio input role."));
        return;
    }
    audio.set_gain(gain_value->value() / 100.0);
    audio.set_sensitivity(sensitivity_value->value() / 100.0);
    const QString device = audio_device->currentData().toString();
    std::string error;
    if (!audio.set_processing_config(config.audio_processing, &error)) {
        audio_lamp->setState(StatusLamp::State::Fault);
        audio_lamp->setToolTip(qtext(error));
        emit q->runtimeStatusChanged(
            q->tr("Audio input processing could not start: %1")
                .arg(qtext(error)));
        return;
    }
    const int period = std::clamp(
        QSettings().value(QStringLiteral("live/audioPeriodFrames"), 128).toInt(),
        1, kMaximumUiInteger);
    if (!audio.start(narrow(device), static_cast<std::uint32_t>(period), &error)) {
        audio_lamp->setState(StatusLamp::State::Fault);
        audio_lamp->setToolTip(qtext(error));
        emit q->runtimeStatusChanged(q->tr("Audio input could not start: %1")
                                         .arg(qtext(error)));
        return;
    }
    audio_lamp->setState(StatusLamp::State::Warning);
    audio_lamp->setToolTip(q->tr("Audio capture started; waiting for callbacks."));
    audio_dropout_clock.restart();
}

void LiveWorkspace::Impl::restartOsc() {
    if (!active) return;
    osc.stop();
    if (osc_role->currentData().toString().isEmpty()) return;
    QString error;
    if (!osc.listen(static_cast<std::uint16_t>(osc_port->value()),
                    osc_local->isChecked(), &error)) {
        emit q->runtimeStatusChanged(q->tr("OSC could not listen: %1").arg(error));
    }
}

void LiveWorkspace::Impl::configureClockOutputs() {
    QStringList names;
    for (const auto& output : config.midi_clock_outputs) {
        if (!output.enabled) continue;
        const auto* role = endpoint(output.endpoint_uuid);
        QString name = role == nullptr ? q->tr("PVT Clock") : qtext(role->name);
        if (output.source == pvt::LiveClockTarget::Layer) {
            name += q->tr(" · Layer");
        } else {
            name += q->tr(" · Project");
        }
        names.push_back(name);
    }
    QString error;
    if (!midi.configureClockOutputs(names, &error) && !error.isEmpty()) {
        emit q->runtimeStatusChanged(error);
    }
    clock_outputs.resize(names.size());
    const double now = run_clock.isValid()
        ? static_cast<double>(run_clock.elapsed()) / 1000.0 : 0.0;
    for (auto& item : clock_outputs) item.next_seconds = now;
    QString selected_clock_source;
    bool accept_all_clock_sources = false;
    const std::string active_layer = active_layer_provider
        ? active_layer_provider() : std::string{};
    for (const auto& input : config.clock_inputs) {
        if (!input.enabled
            || input.source != pvt::LiveClockInputSource::MidiClock
            || (input.target == pvt::LiveClockTarget::Layer
                && input.layer_uuid != active_layer)) {
            continue;
        }
        const QString source = boundSource(input.endpoint_uuid);
        if (source.isEmpty()) {
            accept_all_clock_sources = true;
        } else if (selected_clock_source.isEmpty()) {
            selected_clock_source = source;
        } else if (selected_clock_source != source) {
            // CoreMIDI keeps a clock state per source. Accept both here and
            // let routedPhase query the bound source explicitly; selecting
            // just the project source would silently starve a layer route.
            accept_all_clock_sources = true;
        }
    }
    (void)midi.selectClockInput(
        accept_all_clock_sources ? QString{} : selected_clock_source);
    if (active) {
        int output_index = 0;
        for (const auto& output : config.midi_clock_outputs) {
            if (!output.enabled) continue;
            if (output.send_song_position) midi.sendSongPosition(output_index, 0U);
            if (output.send_transport) midi.sendClockStart(output_index);
            ++output_index;
        }
    }
}

QScreen* LiveWorkspace::Impl::selectedScreen() const {
    const QString id = screen == nullptr ? QString{} : screen->currentData().toString();
    const QString legacy_name = screen == nullptr
        ? QString{} : screen->currentData(Qt::UserRole + 1).toString();
    const auto screens = QGuiApplication::screens();
    for (QScreen* item : screens) {
        if (screen_identifier(item, screens) == id
            || (!legacy_name.isEmpty() && item->name() == legacy_name)
            || item->name() == id) {
            return item;
        }
    }
    return QGuiApplication::primaryScreen();
}

void LiveWorkspace::Impl::applyVisibleOutputPolicy() {
    QScreen* target = selectedScreen();
    const bool fullscreen = presentation_active
        ? QSettings().value(QStringLiteral("previewOutput/fullscreen"), true).toBool()
        : config.output.fullscreen;
    const bool hide_cursor = presentation_active
        ? QSettings().value(QStringLiteral("previewOutput/hideCursor"), true).toBool()
        : config.output.hide_cursor;
    stage.setCursor(hide_cursor ? Qt::BlankCursor : Qt::ArrowCursor);
    if (fullscreen) {
        stage.showOnScreen(target);
        return;
    }
    const QRect available = target == nullptr
        ? QRect(100, 100, 960, 540) : target->availableGeometry();
    const int width = std::min(
        std::max(1, available.width()),
        std::max(320, available.width() * 4 / 5));
    const int height = std::min(
        std::max(1, available.height()),
        std::max(180, available.height() * 4 / 5));
    const QSize size(width, height);
    stage.showWindowedOnScreen(
        target,
        QRect(available.center() - QPoint(size.width() / 2, size.height() / 2),
              size));
}

void LiveWorkspace::Impl::showOutput() {
    stage.setSmoothScaling(true);
    stage.setFrame(last_image);
    // The screen-specific helper performs the only show. In particular, macOS
    // must create/assign the native window to the selected display before it
    // becomes visible or a projector output can flash on the primary display.
    applyVisibleOutputPolicy();
    output_button->setChecked(active && stage.isVisible());
}

void LiveWorkspace::Impl::dismissOutput() {
    if (presentation_active) {
        setPresentationActive(false);
        restoreOutputOwnerFocus();
        return;
    }
    stage.dismiss();
    output_button->setChecked(false);
    if (active) {
        // Discard any frame sized for the stage and immediately replace it
        // with one sized for the in-window program monitor.
        ++render_generation;
        renderer.cancelCurrent();
        requestFrame();
    }
    restoreOutputOwnerFocus();
}

void LiveWorkspace::Impl::restoreOutputOwnerFocus() {
    // User dismissal should return keyboard focus to the window that owns the
    // output mode: the editor for presentation-only output, or the LIVE
    // companion for performance output. State teardown calls stage.dismiss()
    // directly and therefore never steal focus during shutdown.
    QPointer<QWidget> owner(q->window());
    QTimer::singleShot(0, q, [owner] {
        if (owner == nullptr || !owner->isVisible()) return;
        owner->raise();
        owner->activateWindow();
    });
}

void LiveWorkspace::Impl::showStartingState() {
    render_lamp->setState(StatusLamp::State::Warning);
    render_lamp->setText(q->tr("STARTING"));
    render_lamp->setToolTip({});
    fps_readout->setText(q->tr("0.0 fps delivered"));
    frame_readout->setText(q->tr("Waiting for first frame"));
    monitor->setPixmap({});
    monitor->setText(q->tr("PROGRAM OUTPUT\nWaiting for first frame"));
}

void LiveWorkspace::Impl::showStandbyState() {
    render_lamp->setState(StatusLamp::State::Off);
    render_lamp->setText(q->tr("STANDBY"));
    render_lamp->setToolTip({});
    audio_lamp->setState(StatusLamp::State::Off);
    audio_lamp->setToolTip({});
    midi_lamp->setState(StatusLamp::State::Off);
    midi_lamp->setToolTip({});
    audio_meter->setLevel(0.0);
    audio_meter->setPeakWarning(false);
    detected_tempo->setText(q->tr("Waiting for audio…"));
    fps_readout->setText(q->tr("— fps"));
    frame_readout->setText(q->tr("No frame"));
    monitor->setPixmap({});
    monitor->setText(q->tr("PROGRAM OUTPUT\nStandby"));
    freeze_button->setText(q->tr("FREEZE"));
    blackout_button->setText(q->tr("BLACKOUT"));
}

void LiveWorkspace::Impl::toggleOutput() {
    if (stage.isVisible()) {
        dismissOutput();
        return;
    }
    if (!active) setActive(true);
    showOutput();
}

void LiveWorkspace::Impl::setFreeze(bool value) {
    user_freeze = value;
    if (value) {
        render_failed = false;
        safety_blackout = false;
    }
    stage.setFrozen(value);
    freeze_button->setText(value ? q->tr("FROZEN") : q->tr("FREEZE"));
    updateOutputState();
    if (!value) requestFrame();
}

void LiveWorkspace::Impl::setBlackout(bool value) {
    user_blackout = value;
    blackout_button->setText(value ? q->tr("BLACK") : q->tr("BLACKOUT"));
    updateOutputState();
}

void LiveWorkspace::Impl::updateOutputState() {
    stage.setFrozen(user_freeze);
    stage.setBlackout(user_blackout || safety_blackout);
    if (user_blackout || safety_blackout) {
        monitor->setPixmap({});
        monitor->setText(safety_blackout && !user_blackout
                             ? q->tr("SAFETY BLACKOUT\nLast-good watchdog")
                             : q->tr("BLACKOUT"));
    } else {
        updateMonitor();
    }
}

void LiveWorkspace::Impl::updateMonitor() {
    if (last_image.isNull() || user_blackout || safety_blackout) return;
    monitor->setText({});
    monitor->setPixmap(QPixmap::fromImage(last_image).scaled(
        monitor->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void LiveWorkspace::Impl::requestFrame() {
    if (!realtimeActive() || (active && user_freeze)) return;
    if (active && shouldHoldAudioFrame()) {
        const std::string selected_role = narrow(
            audio_role->currentData().toString());
        for (const auto& input : config.clock_inputs) {
            if (!effectiveAudioRoute(input)) continue;
            if (input.endpoint_uuid != selected_role) {
                warnRouteOnce(
                    qtext(input.endpoint_uuid)
                        + QStringLiteral(":wrong-audio-role"),
                    q->tr("An effective audio clock uses a different logical role than the active microphone. The last good frame is being held; choose the same role in Live or edit the advanced route."));
            } else if (!input.frequency_stream_uuid.empty()
                       && std::none_of(
                           audio_snapshot.frequency_streams.begin(),
                           audio_snapshot.frequency_streams.end(),
                           [&input](const pvt::audio::LiveAudioSnapshot::FrequencyStream& stream) {
                               return stream.uuid
                                   == input.frequency_stream_uuid;
                           })) {
                warnRouteOnce(
                    qtext(input.frequency_stream_uuid)
                        + QStringLiteral(":missing-frequency-stream"),
                    q->tr("An effective audio clock's named frequency stream is unavailable. The last good frame is being held; choose an authored stream or edit the advanced route."));
            }
        }
        updateSafety();
        return;
    }
    pvt::ProjectConfig project = presentation_active
        ? (presentation_project_provider
               ? presentation_project_provider()
               : (project_provider ? project_provider() : pvt::default_project()))
        : runtimeProject();
    updateScheduledFps(project.canvas.fps);
    double phase = 0.0;
    std::optional<int> synchronized_frame;
    if (presentation_active) {
        const int frame = std::max(
            0, presentation_frame_provider ? presentation_frame_provider() : 0);
        synchronized_frame = frame;
        std::string frame_error;
        const int frame_count = std::max(
            1, pvt::effective_frame_count(project.canvas, &frame_error));
        phase = static_cast<double>(frame % frame_count)
                / static_cast<double>(frame_count);
    } else {
        phase = basePhase(project);
        const bool project_routed = applyClockRoutes(project, phase);
        if (!project_routed) {
            // Feed an authoritative wall-clock frame through the normal core
            // evaluator so Frame/Time/Meter/Music, reverse, and authored
            // offsets remain the actual Live fallback. A routed layer can
            // still override its own local clock in the temporary project.
            std::string frame_error;
            const int frame_count = std::max(
                1, pvt::effective_frame_count(project.canvas, &frame_error));
            synchronized_frame = std::clamp(
                static_cast<int>(std::floor(
                    phase * static_cast<double>(frame_count))),
                0, frame_count - 1);
        }
    }
    QSize output_size = stage.isVisible()
        ? stage.outputPixelSize() : physical_widget_size(monitor);
    output_size.setWidth(std::max(320, output_size.width()));
    output_size.setHeight(std::max(180, output_size.height()));
    const double selected = q->outputResolutionScale();
    const double resolution_scale = selected > 0.0 ? selected : adaptive_scale;
    pvt::FrameRenderOptions options = render_options_provider
        ? render_options_provider() : pvt::FrameRenderOptions{};
    const double deadline_milliseconds = 1000.0 / scheduled_fps;
    const int watchdog_milliseconds = active
        && config.safety.frame_time_watchdog_enabled
            ? config.safety.watchdog_timeout_milliseconds : 60000;
    const std::uint64_t revision = document_revision_provider
        ? document_revision_provider() : 0U;
    renderer.request(std::move(project), phase, synchronized_frame,
                     output_size, resolution_scale, deadline_milliseconds,
                     watchdog_milliseconds, options, render_generation,
                     revision);
}

void LiveWorkspace::Impl::frameFinished(
    const LiveFrameController::Result& result) {
    const std::uint64_t current_revision = document_revision_provider
        ? document_revision_provider() : 0U;
    if (!realtimeActive() || result.cancelled
        || result.session_generation != render_generation
        || result.document_revision != current_revision) {
        return;
    }
    // A render requested immediately before an audio loss must not replace the
    // genuine last-good image with its deterministic fallback.
    if (active && shouldHoldAudioFrame()) {
        updateSafety();
        return;
    }
    if (!result.error.isEmpty() || result.image.isNull()) {
        render_failed = active;
        ++late_streak;
        good_streak = 0;
        render_lamp->setState(StatusLamp::State::Fault);
        render_lamp->setText(q->tr("HOLDING"));
        render_lamp->setToolTip(result.error);
        frame_readout->setText(q->tr("Last good · %1 dropped")
                                   .arg(result.dropped_requests));
        if (presentation_active) {
            emit q->runtimeStatusChanged(q->tr(
                "Live Preview Output is holding the last frame: %1")
                    .arg(result.error));
            return;
        }
        if (!user_freeze
            && config.safety.dropout_behavior
                   == pvt::LiveDropoutBehavior::Blackout) {
            safety_blackout = true;
            updateOutputState();
        }
        updateSafety();
        return;
    }
    last_image = result.image;
    emit q->livePreviewFrame(result.image);
    render_failed = false;
    last_good_clock.restart();
    if (presentation_active || !user_freeze) stage.setFrame(result.image);
    if (!user_blackout && !safety_blackout) updateMonitor();
    frame_readout->setText(q->tr("%1 ms · %2 dropped")
                               .arg(result.render_milliseconds, 0, 'f', 1)
                               .arg(result.dropped_requests));
    double delivered_fps = 0.0;
    if (presented_frame_clock.isValid()) {
        const qint64 elapsed = presented_frame_clock.restart();
        if (elapsed > 0) delivered_fps = 1000.0 / static_cast<double>(elapsed);
    } else {
        presented_frame_clock.start();
    }
    fps_readout->setText(q->tr("%1 fps delivered")
                              .arg(delivered_fps, 0, 'f', 1));
    if (result.late) {
        ++late_streak;
        good_streak = 0;
        render_lamp->setState(StatusLamp::State::Warning);
        render_lamp->setText(q->tr("LATE"));
        if (quality->currentData().toDouble() == 0.0 && late_streak >= 3) {
            adaptive_scale = std::max(0.25, adaptive_scale * 0.8);
            late_streak = 0;
        }
    } else {
        late_streak = 0;
        ++good_streak;
        render_lamp->setState(StatusLamp::State::Ready);
        render_lamp->setText(q->tr("ON AIR"));
        if (quality->currentData().toDouble() == 0.0 && good_streak >= 120) {
            adaptive_scale = std::min(1.0, adaptive_scale * 1.1);
            good_streak = 0;
        }
    }
    if (active) updateSafety();
}

void LiveWorkspace::Impl::runtimeTick() {
    if (!active) return;
    audio_snapshot = audio.snapshot();
    audio_meter->setLevel(audio_snapshot.features.energy);
    audio_meter->setPeakWarning(audio_snapshot.features.energy > 0.97F);
    if (effectiveAudioClockReceiving()) {
        audio_lamp->setState(StatusLamp::State::Ready);
    } else if (audio.is_running()) {
        audio_lamp->setState(StatusLamp::State::Warning);
    }
    double display_bpm = audio_snapshot.detected_bpm;
    double display_phase = audio_snapshot.beat_phase;
    const auto selected_route = std::find_if(
        config.clock_inputs.begin(), config.clock_inputs.end(),
        [this](const pvt::LiveClockInputConfig& input) {
            return effectiveAudioRoute(input)
                && input.endpoint_uuid
                       == narrow(audio_role->currentData().toString());
        });
    if (selected_route != config.clock_inputs.end()
        && !selected_route->frequency_stream_uuid.empty()) {
        const auto selected_stream = std::find_if(
            audio_snapshot.frequency_streams.begin(),
            audio_snapshot.frequency_streams.end(),
            [&selected_route](
                const pvt::audio::LiveAudioSnapshot::FrequencyStream& item) {
                return item.uuid == selected_route->frequency_stream_uuid;
            });
        if (selected_stream != audio_snapshot.frequency_streams.end()) {
            display_bpm = selected_stream->detected_bpm;
            display_phase = selected_stream->beat_phase;
        }
    }
    detected_tempo->setText(display_bpm > 0.0
        ? q->tr("%1 BPM · phase %2 · %3 ms input")
              .arg(display_bpm, 0, 'f', 1)
              .arg(display_phase, 0, 'f', 2)
              .arg(audio_snapshot.estimated_input_latency_ms, 0, 'f', 1)
        : q->tr("Listening · causal features active"));
    const auto midi_clock = midi.clockSnapshot();
    midi_lamp->setState(midi_clock.receiving ? StatusLamp::State::Ready
                                              : (midi.isRunning()
                                                     ? StatusLamp::State::Warning
                                                     : StatusLamp::State::Off));
    updateSafety();
}

void LiveWorkspace::Impl::updateSafety() {
    if (!active) return;
    if (user_freeze) {
        if (safety_blackout) {
            safety_blackout = false;
            updateOutputState();
        }
        return;
    }
    bool dropout = render_failed;
    if (last_good_clock.isValid()
        && config.safety.last_good_frame_timeout_milliseconds > 0
        && last_good_clock.elapsed()
               > config.safety.last_good_frame_timeout_milliseconds) {
        dropout = true;
    }
    const bool audio_clock_enabled = std::any_of(
        config.clock_inputs.begin(), config.clock_inputs.end(),
        [this](const pvt::LiveClockInputConfig& item) {
            return effectiveAudioRoute(item);
        });
    if (audio_clock_enabled) {
        const std::string selected_role = narrow(
            audio_role->currentData().toString());
        bool bindings_match = true;
        for (const auto& input : config.clock_inputs) {
            if (!effectiveAudioRoute(input)) continue;
            if (input.endpoint_uuid != selected_role) {
                bindings_match = false;
                break;
            }
            if (!input.frequency_stream_uuid.empty()
                && std::none_of(
                    audio_snapshot.frequency_streams.begin(),
                    audio_snapshot.frequency_streams.end(),
                    [&input](const pvt::audio::LiveAudioSnapshot::FrequencyStream& stream) {
                        return stream.uuid == input.frequency_stream_uuid;
                    })) {
                bindings_match = false;
                break;
            }
        }
        const double age_seconds = bindings_match
            ? live_audio_loss_age_seconds(
                  audio_snapshot, audio_dropout_clock)
            : (audio_dropout_clock.isValid()
                   ? static_cast<double>(audio_dropout_clock.elapsed()) / 1000.0
                   : (std::numeric_limits<double>::infinity)());
        if (!pvt::audio::live_audio_callback_within_holdover(
                age_seconds,
                config.safety.audio_dropout_grace_milliseconds)) {
            dropout = true;
        }
    }
    const bool timed_last_good = last_good_clock.isValid()
        && config.safety.last_good_frame_timeout_milliseconds > 0
        && last_good_clock.elapsed()
               > config.safety.last_good_frame_timeout_milliseconds;
    const bool blackout = dropout
        && (config.safety.dropout_behavior == pvt::LiveDropoutBehavior::Blackout
            || timed_last_good);
    if (blackout != safety_blackout) {
        safety_blackout = blackout;
        updateOutputState();
    }
}

QString LiveWorkspace::Impl::boundSource(const std::string& uuid) const {
    const auto* item = endpoint(uuid);
    if (item == nullptr) return {};
    const QString leaf = item->protocol == pvt::LiveEndpointProtocol::Audio
        ? QStringLiteral("audioDevice") : QStringLiteral("midiSource");
    return QSettings().value(endpoint_key(uuid, leaf)).toString();
}

QString LiveWorkspace::Impl::sourceEndpoint(
    pvt::LiveEndpointProtocol protocol, const QString& runtimeSource) const {
    QString fallback;
    for (const auto& item : config.endpoints) {
        if (item.protocol != protocol || !direction_has_input(item.direction)) continue;
        const QString uuid = qtext(item.uuid);
        if (fallback.isEmpty()) fallback = uuid;
        const QString binding = boundSource(item.uuid);
        if (!binding.isEmpty() && binding == runtimeSource) return uuid;
    }
    return fallback;
}

void LiveWorkspace::Impl::createStarterRig() {
    const bool already_has_roles = std::any_of(
        config.endpoints.begin(), config.endpoints.end(),
        [](const pvt::LiveEndpointConfig& item) {
            return item.protocol == pvt::LiveEndpointProtocol::Audio
                || item.protocol == pvt::LiveEndpointProtocol::Midi
                || item.protocol == pvt::LiveEndpointProtocol::Osc
                || item.protocol == pvt::LiveEndpointProtocol::FootController;
        });
    if (already_has_roles
        && QMessageBox::question(q, q->tr("Create starter rig"),
             q->tr("This project already has Live roles. Add a complete starter set as well?"))
               != QMessageBox::Yes) return;
    pvt::LiveConfig next = config;
    const auto append = [&next](const QString& name,
                                pvt::LiveEndpointProtocol protocol,
                                pvt::LiveEndpointDirection direction) {
        pvt::LiveEndpointConfig endpoint_value;
        endpoint_value.uuid = narrow(uuid_text());
        endpoint_value.name = narrow(name);
        endpoint_value.protocol = protocol;
        endpoint_value.direction = direction;
        const std::string uuid = endpoint_value.uuid;
        next.endpoints.push_back(std::move(endpoint_value));
        return uuid;
    };
    const std::string audio_uuid = append(
        q->tr("Stage audio"), pvt::LiveEndpointProtocol::Audio,
        pvt::LiveEndpointDirection::Input);
    append(q->tr("Stage MIDI"), pvt::LiveEndpointProtocol::Midi,
           pvt::LiveEndpointDirection::Bidirectional);
    append(q->tr("Stage OSC"), pvt::LiveEndpointProtocol::Osc,
           pvt::LiveEndpointDirection::Input);
    append(q->tr("Foot controller"), pvt::LiveEndpointProtocol::FootController,
           pvt::LiveEndpointDirection::Input);
    append(q->tr("Layer clock return"), pvt::LiveEndpointProtocol::Midi,
           pvt::LiveEndpointDirection::Output);
    const bool has_project_clock = std::any_of(
        next.clock_inputs.begin(), next.clock_inputs.end(),
        [](const pvt::LiveClockInputConfig& route) {
            return route.enabled
                && route.target == pvt::LiveClockTarget::Project;
        });
    if (!has_project_clock) {
        pvt::LiveClockInputConfig route;
        route.enabled = true;
        route.target = pvt::LiveClockTarget::Project;
        route.source = pvt::LiveClockInputSource::AudioStream;
        route.endpoint_uuid = audio_uuid;
        next.clock_inputs.push_back(std::move(route));
    }
    next.enabled = true;
    commitConfig(std::move(next), q->tr("Create portable live starter rig"));
}

void LiveWorkspace::Impl::addLogicalRole() {
    QDialog dialog(q);
    dialog.setWindowTitle(q->tr("Add Logical Live Role"));
    auto* layout = new QFormLayout(&dialog);
    auto* name = new QLineEdit(q->tr("Live control"));
    auto* protocol = new QComboBox;
    protocol->addItem(q->tr("Audio"), static_cast<int>(pvt::LiveEndpointProtocol::Audio));
    protocol->addItem(q->tr("MIDI"), static_cast<int>(pvt::LiveEndpointProtocol::Midi));
    protocol->addItem(q->tr("OSC"), static_cast<int>(pvt::LiveEndpointProtocol::Osc));
    protocol->addItem(q->tr("Foot controller"), static_cast<int>(pvt::LiveEndpointProtocol::FootController));
    auto* direction = new QComboBox;
    direction->addItem(q->tr("Input"), static_cast<int>(pvt::LiveEndpointDirection::Input));
    direction->addItem(q->tr("Output"), static_cast<int>(pvt::LiveEndpointDirection::Output));
    direction->addItem(q->tr("Input + output"), static_cast<int>(pvt::LiveEndpointDirection::Bidirectional));
    auto* note = new QLabel(q->tr(
        "The project stores this role and its calibration. Device names, network "
        "addresses, and port identities stay in local preferences."));
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(q->tr("Name"), name);
    layout->addRow(q->tr("Protocol"), protocol);
    layout->addRow(q->tr("Direction"), direction);
    layout->addRow(note);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || name->text().trimmed().isEmpty()) return;
    pvt::LiveEndpointConfig role;
    role.uuid = narrow(uuid_text());
    role.name = narrow(name->text().trimmed());
    role.protocol = static_cast<pvt::LiveEndpointProtocol>(protocol->currentData().toInt());
    role.direction = static_cast<pvt::LiveEndpointDirection>(direction->currentData().toInt());
    pvt::LiveConfig next = config;
    const std::string role_uuid = role.uuid;
    const bool infer_audio_clock =
        role.protocol == pvt::LiveEndpointProtocol::Audio
        && direction_has_input(role.direction)
        && std::none_of(next.clock_inputs.begin(), next.clock_inputs.end(),
                        [](const pvt::LiveClockInputConfig& route) {
                            return route.enabled
                                && route.target == pvt::LiveClockTarget::Project;
                        });
    next.endpoints.push_back(std::move(role));
    if (infer_audio_clock) {
        pvt::LiveClockInputConfig route;
        route.enabled = true;
        route.target = pvt::LiveClockTarget::Project;
        route.source = pvt::LiveClockInputSource::AudioStream;
        route.endpoint_uuid = role_uuid;
        next.clock_inputs.push_back(std::move(route));
    }
    next.enabled = true;
    commitConfig(std::move(next), q->tr("Add portable live role"));
}

void LiveWorkspace::Impl::calibrateLatency() {
    double detected_bpm = audio_snapshot.detected_bpm;
    double beat_phase = audio_snapshot.beat_phase;
    const auto selected_route = std::find_if(
        config.clock_inputs.begin(), config.clock_inputs.end(),
        [this](const pvt::LiveClockInputConfig& input) {
            return effectiveAudioRoute(input)
                && input.endpoint_uuid
                       == narrow(audio_role->currentData().toString());
        });
    if (selected_route != config.clock_inputs.end()
        && !selected_route->frequency_stream_uuid.empty()) {
        const auto selected_stream = std::find_if(
            audio_snapshot.frequency_streams.begin(),
            audio_snapshot.frequency_streams.end(),
            [&selected_route](
                const pvt::audio::LiveAudioSnapshot::FrequencyStream& item) {
                return item.uuid == selected_route->frequency_stream_uuid;
            });
        if (selected_stream != audio_snapshot.frequency_streams.end()) {
            detected_bpm = selected_stream->detected_bpm;
            beat_phase = selected_stream->beat_phase;
        }
    }
    if (!audio_snapshot.receiving || detected_bpm <= 0.0
        || audio_role->currentData().toString().isEmpty()) {
        QMessageBox::information(q, q->tr("Align audio beat"),
            q->tr("Start Live and play a steady beat first. Tap this button on the beat; "
                  "the current causal beat phase becomes a correction for this computer and device."));
        return;
    }
    double phase = beat_phase;
    if (phase > 0.5) phase -= 1.0;
    const double beat_ms = 60000.0 / detected_bpm;
    // The user taps against the physical beat. A late capture presents as a
    // small negative wrapped phase at that instant, so negate that measured
    // phase. Do not also add the backend buffer estimate: that would double
    // count the same observed delay on common devices.
    const double requested = std::round(-phase * beat_ms);
    const int correction = static_cast<int>(std::clamp(
        requested,
        static_cast<double>((std::numeric_limits<int>::min)()),
        static_cast<double>(kMaximumUiInteger)));
    device_latency->setValue(correction);
    emit q->runtimeStatusChanged(q->tr(
        "Saved a %1 ms machine-local correction for this microphone. The portable rig offset was left unchanged.")
        .arg(correction));
}

void LiveWorkspace::Impl::tapTempo() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!tempo_taps.isEmpty() && now - tempo_taps.back() > 2500) {
        tempo_taps.clear();
        tapped_beat_count = 0U;
        tapped_bpm = 0.0;
    }
    tempo_taps.push_back(now);
    ++tapped_beat_count;
    tapped_beat_anchor_seconds = run_clock.isValid()
        ? static_cast<double>(run_clock.elapsed()) / 1000.0 : 0.0;
    while (tempo_taps.size() > 8) tempo_taps.removeFirst();
    if (tempo_taps.size() < 2) return;
    double total = 0.0;
    for (int i = 1; i < tempo_taps.size(); ++i) {
        total += static_cast<double>(tempo_taps[i] - tempo_taps[i - 1]);
    }
    const double interval = total / static_cast<double>(tempo_taps.size() - 1);
    if (interval > 0.0) tapped_bpm = 60000.0 / interval;
}

void LiveWorkspace::Impl::editMapping(int index) {
    const bool editing = index >= 0 && index < static_cast<int>(config.mappings.size());
    pvt::LiveControlMapping initial;
    if (editing) initial = config.mappings[static_cast<std::size_t>(index)];

    QDialog dialog(q);
    dialog.setWindowTitle(editing ? q->tr("Edit Live Mapping")
                                  : q->tr("Add Live Mapping"));
    dialog.resize(760, 720);
    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(editing ? qtext(initial.name) : q->tr("Performance control"));
    auto* enabled = new QCheckBox(q->tr("Enabled"));
    enabled->setChecked(!editing || initial.enabled);
    auto* input = new QComboBox;
    const std::initializer_list<pvt::LiveControlInput> inputs = {
        pvt::LiveControlInput::MidiControlChange,
        pvt::LiveControlInput::MidiNote,
        pvt::LiveControlInput::MidiProgramChange,
        pvt::LiveControlInput::MidiPitchBend,
        pvt::LiveControlInput::MidiChannelPressure,
        pvt::LiveControlInput::OscValue,
        pvt::LiveControlInput::Footswitch};
    for (const auto value : inputs) input->addItem(input_name(value), static_cast<int>(value));
    input->setCurrentIndex(std::max(0, input->findData(static_cast<int>(initial.input))));
    auto* endpoint_combo = new QComboBox;
    auto* channel = new QSpinBox;
    channel->setRange(0, 16);
    channel->setValue(initial.midi_channel);
    auto* number = new QSpinBox;
    number->setRange(0, 127);
    number->setValue(initial.control_number);
    auto* address = new QLineEdit(editing ? qtext(initial.osc_address)
                                          : QStringLiteral("/pvt/control"));
    auto* mode = new QComboBox;
    for (int value = 0; value <= static_cast<int>(pvt::LiveMappingMode::Trigger); ++value) {
        const auto item = static_cast<pvt::LiveMappingMode>(value);
        mode->addItem(mapping_mode_name(item), value);
    }
    mode->setCurrentIndex(std::max(0, mode->findData(static_cast<int>(initial.mode))));
    form->addRow(q->tr("Name"), name);
    form->addRow({}, enabled);
    form->addRow(q->tr("Input"), input);
    form->addRow(q->tr("Logical role"), endpoint_combo);
    form->addRow(q->tr("MIDI channel (0 = omni)"), channel);
    form->addRow(q->tr("Control / note"), number);
    form->addRow(q->tr("OSC address"), address);
    form->addRow(q->tr("Behavior"), mode);
    outer->addLayout(form);

    auto* target_kind = new QComboBox;
    target_kind->addItem(q->tr("Setting"), static_cast<int>(pvt::LiveMappingTarget::Setting));
    target_kind->addItem(q->tr("Performance action"), static_cast<int>(pvt::LiveMappingTarget::Action));
    target_kind->addItem(q->tr("Scene"), static_cast<int>(pvt::LiveMappingTarget::Scene));
    target_kind->setCurrentIndex(std::max(0, target_kind->findData(static_cast<int>(initial.target))));
    outer->addWidget(new QLabel(q->tr("TARGET")));
    outer->addWidget(target_kind);
    auto* targets = new QStackedWidget;
    auto* setting_page = new QWidget;
    auto* setting_layout = new QVBoxLayout(setting_page);
    setting_layout->setContentsMargins(0, 0, 0, 0);
    auto* setting_search = new QLineEdit;
    setting_search->setPlaceholderText(q->tr(
        "Search targets by layer, section, or control…"));
    setting_search->setClearButtonEnabled(true);
    auto* setting = new QTreeWidget;
    setting->setHeaderLabels({q->tr("Target"), q->tr("Current")});
    setting->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    setting->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    setting->setSelectionMode(QAbstractItemView::SingleSelection);
    setting->setAlternatingRowColors(true);
    setting_layout->addWidget(setting_search);
    setting_layout->addWidget(setting, 1);
    const auto registry = project_provider
        ? buildLiveTargetRegistry(project_provider())
        : std::vector<LiveTargetDescriptor>{};
    QHash<QString, QTreeWidgetItem*> target_sections;
    QTreeWidgetItem* first_target = nullptr;
    for (const auto& target : registry) {
        QTreeWidgetItem* section = target_sections.value(target.section);
        if (section == nullptr) {
            section = new QTreeWidgetItem(setting, {target.section});
            section->setFlags(section->flags() & ~Qt::ItemIsSelectable);
            section->setExpanded(false);
            target_sections.insert(target.section, section);
        }
        auto* item = new QTreeWidgetItem(
            section, {target.label, QString::number(target.current_value, 'g', 8)});
        item->setData(0, Qt::UserRole, target.path);
        item->setData(0, Qt::UserRole + 1, target.minimum);
        item->setData(0, Qt::UserRole + 2, target.maximum);
        item->setData(0, Qt::UserRole + 3, static_cast<int>(target.kind));
        item->setToolTip(0, target.path);
        if (first_target == nullptr) first_target = item;
    }
    if (editing && initial.target == pvt::LiveMappingTarget::Setting) {
        const auto matches = setting->findItems(
            QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive, 0);
        QTreeWidgetItem* selected = nullptr;
        for (QTreeWidgetItem* item : matches) {
            if (item->data(0, Qt::UserRole).toString()
                == qtext(initial.target_path)) {
                selected = item;
                break;
            }
        }
        if (selected == nullptr) {
            auto* unresolved = new QTreeWidgetItem(
                setting, {q->tr("Unresolved · %1").arg(qtext(initial.target_path))});
            unresolved->setData(0, Qt::UserRole, qtext(initial.target_path));
            unresolved->setData(0, Qt::UserRole + 1, initial.output_minimum);
            unresolved->setData(0, Qt::UserRole + 2, initial.output_maximum);
            unresolved->setData(0, Qt::UserRole + 3,
                                static_cast<int>(LiveTargetKind::Real));
            selected = unresolved;
        }
        setting->setCurrentItem(selected);
        if (selected->parent() != nullptr) selected->parent()->setExpanded(true);
    } else if (first_target != nullptr) {
        setting->setCurrentItem(first_target);
        first_target->parent()->setExpanded(true);
    }
    targets->addWidget(setting_page);
    auto* action = new QComboBox;
    for (int value = 0; value <= static_cast<int>(pvt::LiveAction::TapTempo); ++value) {
        const auto item = static_cast<pvt::LiveAction>(value);
        action->addItem(action_name(item), value);
    }
    action->setCurrentIndex(std::max(0, action->findData(static_cast<int>(initial.action))));
    targets->addWidget(action);
    auto* scene = new QComboBox;
    for (const auto& item : config.scenes) scene->addItem(qtext(item.name), qtext(item.uuid));
    if (editing) {
        const int scene_index = scene->findData(qtext(initial.scene_uuid));
        if (scene_index >= 0) scene->setCurrentIndex(scene_index);
    }
    targets->addWidget(scene);
    targets->setCurrentIndex(target_kind->currentIndex());
    outer->addWidget(targets);

    auto* transform = new QFormLayout;
    auto* input_min = new QDoubleSpinBox;
    auto* input_max = new QDoubleSpinBox;
    auto* output_min = new QDoubleSpinBox;
    auto* output_max = new QDoubleSpinBox;
    for (auto* spin : {input_min, input_max, output_min, output_max}) {
        spin->setDecimals(6);
        spin->setRange(-kMaximumLiveMappingMagnitude,
                       kMaximumLiveMappingMagnitude);
    }
    input_min->setValue(editing ? initial.input_minimum : 0.0);
    input_max->setValue(editing ? initial.input_maximum : 1.0);
    const auto selected_target_value = [setting](int role) {
        const QTreeWidgetItem* item = setting->currentItem();
        return item != nullptr ? item->data(0, role) : QVariant{};
    };
    output_min->setValue(editing ? initial.output_minimum
                                 : selected_target_value(Qt::UserRole + 1).toDouble());
    output_max->setValue(editing ? initial.output_maximum
                                 : selected_target_value(Qt::UserRole + 2).toDouble());
    auto* curve = new QDoubleSpinBox;
    curve->setDecimals(6);
    curve->setRange(0.000001, kMaximumLiveMappingMagnitude);
    curve->setValue(editing ? initial.curve : 1.0);
    auto* dead = new QDoubleSpinBox;
    dead->setDecimals(6);
    dead->setRange(0.0, 0.999999);
    dead->setValue(editing ? initial.dead_zone : 0.0);
    auto* smoothing = new QSpinBox;
    smoothing->setRange(0, kMaximumUiInteger);
    smoothing->setSuffix(q->tr(" ms"));
    smoothing->setValue(editing ? initial.smoothing_milliseconds : 0);
    auto* input_range = new QWidget;
    auto* ir = new QHBoxLayout(input_range);
    ir->setContentsMargins(0, 0, 0, 0);
    ir->addWidget(input_min); ir->addWidget(input_max);
    auto* output_range = new QWidget;
    auto* orow = new QHBoxLayout(output_range);
    orow->setContentsMargins(0, 0, 0, 0);
    orow->addWidget(output_min); orow->addWidget(output_max);
    transform->addRow(q->tr("Input range"), input_range);
    transform->addRow(q->tr("Output range"), output_range);
    transform->addRow(q->tr("Response curve"), curve);
    transform->addRow(q->tr("Dead zone"), dead);
    transform->addRow(q->tr("Smoothing"), smoothing);
    outer->addLayout(transform);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(buttons);

    const auto refresh_source = [this, input, endpoint_combo, channel, number,
                                 address, &initial, editing] {
        const auto selected = static_cast<pvt::LiveControlInput>(input->currentData().toInt());
        const pvt::LiveEndpointProtocol protocol = selected == pvt::LiveControlInput::OscValue
            ? pvt::LiveEndpointProtocol::Osc
            : (selected == pvt::LiveControlInput::Footswitch
                   ? pvt::LiveEndpointProtocol::FootController
                   : pvt::LiveEndpointProtocol::Midi);
        const QString before = endpoint_combo->currentData().toString();
        endpoint_combo->clear();
        for (const auto& role : config.endpoints) {
            if (role.protocol == protocol && direction_has_input(role.direction)) {
                endpoint_combo->addItem(qtext(role.name), qtext(role.uuid));
            }
        }
        QString wanted = before;
        if (editing && selected == initial.input) wanted = qtext(initial.endpoint_uuid);
        const int role_index = endpoint_combo->findData(wanted);
        if (role_index >= 0) endpoint_combo->setCurrentIndex(role_index);
        const bool osc_input = selected == pvt::LiveControlInput::OscValue;
        const bool midi_input = selected != pvt::LiveControlInput::Footswitch && !osc_input;
        const bool numbered = selected == pvt::LiveControlInput::MidiControlChange
            || selected == pvt::LiveControlInput::MidiNote
            || selected == pvt::LiveControlInput::MidiProgramChange
            || selected == pvt::LiveControlInput::Footswitch;
        channel->setEnabled(midi_input);
        number->setEnabled(numbered);
        address->setEnabled(osc_input);
    };
    refresh_source();
    QObject::connect(input, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                     [refresh_source] { refresh_source(); });
    QObject::connect(target_kind, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                     [targets](int row) { targets->setCurrentIndex(row); });
    QObject::connect(setting, &QTreeWidget::currentItemChanged, &dialog,
                     [output_min, output_max](QTreeWidgetItem* item) {
        if (item == nullptr || item->data(0, Qt::UserRole).toString().isEmpty()) return;
        output_min->setValue(item->data(0, Qt::UserRole + 1).toDouble());
        output_max->setValue(item->data(0, Qt::UserRole + 2).toDouble());
    });
    QObject::connect(setting_search, &QLineEdit::textChanged, &dialog,
                     [setting](const QString& query) {
        const QString needle = query.trimmed();
        for (int section_index = 0;
             section_index < setting->topLevelItemCount(); ++section_index) {
            QTreeWidgetItem* section = setting->topLevelItem(section_index);
            bool any_visible = false;
            for (int child_index = 0; child_index < section->childCount(); ++child_index) {
                QTreeWidgetItem* child = section->child(child_index);
                const bool match = needle.isEmpty()
                    || section->text(0).contains(needle, Qt::CaseInsensitive)
                    || child->text(0).contains(needle, Qt::CaseInsensitive)
                    || child->data(0, Qt::UserRole).toString().contains(
                        needle, Qt::CaseInsensitive);
                child->setHidden(!match);
                any_visible = any_visible || match;
            }
            const bool selectable_root = !section->data(0, Qt::UserRole).toString().isEmpty();
            const bool root_match = selectable_root
                && (needle.isEmpty()
                    || section->text(0).contains(needle, Qt::CaseInsensitive)
                    || section->data(0, Qt::UserRole).toString().contains(
                        needle, Qt::CaseInsensitive));
            section->setHidden(!any_visible && !root_match);
            if (!needle.isEmpty() && any_visible) section->setExpanded(true);
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (name->text().trimmed().isEmpty() || endpoint_combo->currentData().toString().isEmpty()) {
        QMessageBox::warning(q, q->tr("Live mapping"),
                             q->tr("A mapping needs a name and a compatible logical role."));
        return;
    }
    if (input_min->value() >= input_max->value()) {
        QMessageBox::warning(q, q->tr("Live mapping"),
                             q->tr("The input maximum must be greater than its minimum."));
        return;
    }
    pvt::LiveControlMapping mapping;
    mapping.enabled = enabled->isChecked();
    mapping.name = narrow(name->text().trimmed());
    mapping.input = static_cast<pvt::LiveControlInput>(input->currentData().toInt());
    mapping.endpoint_uuid = narrow(endpoint_combo->currentData().toString());
    const bool osc_input = mapping.input == pvt::LiveControlInput::OscValue;
    const bool foot_input = mapping.input == pvt::LiveControlInput::Footswitch;
    const bool numbered = mapping.input == pvt::LiveControlInput::MidiControlChange
        || mapping.input == pvt::LiveControlInput::MidiNote
        || mapping.input == pvt::LiveControlInput::MidiProgramChange || foot_input;
    mapping.midi_channel = (!osc_input && !foot_input) ? channel->value() : 0;
    mapping.control_number = numbered ? number->value() : 0;
    mapping.osc_address = osc_input ? narrow(address->text().trimmed()) : std::string{};
    mapping.target = static_cast<pvt::LiveMappingTarget>(target_kind->currentData().toInt());
    if (mapping.target == pvt::LiveMappingTarget::Setting) {
        const QTreeWidgetItem* selected = setting->currentItem();
        if (selected == nullptr || selected->data(0, Qt::UserRole).toString().isEmpty()) {
            QMessageBox::warning(q, q->tr("Live mapping"),
                                 q->tr("Choose a setting target."));
            return;
        }
        mapping.target_path = narrow(selected->data(0, Qt::UserRole).toString());
    } else if (mapping.target == pvt::LiveMappingTarget::Action) {
        mapping.action = static_cast<pvt::LiveAction>(action->currentData().toInt());
    } else {
        mapping.scene_uuid = narrow(scene->currentData().toString());
    }
    mapping.mode = static_cast<pvt::LiveMappingMode>(mode->currentData().toInt());
    mapping.input_minimum = input_min->value();
    mapping.input_maximum = input_max->value();
    mapping.output_minimum = output_min->value();
    mapping.output_maximum = output_max->value();
    mapping.curve = curve->value();
    mapping.dead_zone = dead->value();
    mapping.smoothing_milliseconds = smoothing->value();
    pvt::LiveConfig next = config;
    if (editing) next.mappings[static_cast<std::size_t>(index)] = std::move(mapping);
    else next.mappings.push_back(std::move(mapping));
    commitConfig(std::move(next), editing ? q->tr("Edit live mapping")
                                          : q->tr("Add live mapping"));
}

void LiveWorkspace::Impl::removeMapping() {
    const int row = mapping_table->currentRow();
    if (row < 0 || row >= static_cast<int>(config.mappings.size())) return;
    pvt::LiveConfig next = config;
    next.mappings.erase(next.mappings.begin() + row);
    commitConfig(std::move(next), q->tr("Remove live mapping"));
}

void LiveWorkspace::Impl::beginMidiLearn() {
    const int row = mapping_table->currentRow();
    if (row < 0 || row >= static_cast<int>(config.mappings.size())) {
        QMessageBox::information(q, q->tr("MIDI Learn"),
                                 q->tr("Select a MIDI or foot-controller mapping first."));
        return;
    }
    learn_mapping = learn_mapping == row ? -1 : row;
    learn_button->setText(learn_mapping >= 0 ? q->tr("Listening…") : q->tr("MIDI Learn"));
}

void LiveWorkspace::Impl::handleMidi(LiveMidiRouter::MessageKind kind,
                                     int channel, int number, double value,
                                     const QString& source) {
    pvt::LiveControlInput input = pvt::LiveControlInput::MidiControlChange;
    switch (kind) {
        case LiveMidiRouter::MessageKind::ControlChange:
            input = pvt::LiveControlInput::MidiControlChange; break;
        case LiveMidiRouter::MessageKind::Note:
            input = pvt::LiveControlInput::MidiNote; break;
        case LiveMidiRouter::MessageKind::ProgramChange:
            input = pvt::LiveControlInput::MidiProgramChange; break;
        case LiveMidiRouter::MessageKind::ChannelPressure:
            input = pvt::LiveControlInput::MidiChannelPressure; break;
        case LiveMidiRouter::MessageKind::PitchBend:
            input = pvt::LiveControlInput::MidiPitchBend; break;
    }
    if (learn_mapping >= 0
        && learn_mapping < static_cast<int>(config.mappings.size())) {
        pvt::LiveConfig next = config;
        auto& mapping = next.mappings[static_cast<std::size_t>(learn_mapping)];
        const bool foot = mapping.input == pvt::LiveControlInput::Footswitch;
        mapping.input = foot ? pvt::LiveControlInput::Footswitch : input;
        mapping.endpoint_uuid = narrow(sourceEndpoint(
            foot ? pvt::LiveEndpointProtocol::FootController
                 : pvt::LiveEndpointProtocol::Midi, source));
        mapping.midi_channel = foot ? 0 : channel;
        mapping.control_number = (input == pvt::LiveControlInput::MidiControlChange
                                  || input == pvt::LiveControlInput::MidiNote
                                  || foot) ? number : 0;
        mapping.osc_address.clear();
        learn_mapping = -1;
        commitConfig(std::move(next), q->tr("Learn live MIDI control"));
        return;
    }
    const QString midi_endpoint = sourceEndpoint(pvt::LiveEndpointProtocol::Midi, source);
    processControl(input, channel, number, value, midi_endpoint);
    // A foot role is intentionally distinct in the portable model even when
    // the physical pedal presents as an ordinary CoreMIDI endpoint.
    const QString foot_endpoint = sourceEndpoint(
        pvt::LiveEndpointProtocol::FootController, source);
    if (!foot_endpoint.isEmpty()
        && (input == pvt::LiveControlInput::MidiNote
            || input == pvt::LiveControlInput::MidiControlChange)) {
        processControl(pvt::LiveControlInput::Footswitch, 0, number, value,
                       foot_endpoint);
    }
}

void LiveWorkspace::Impl::handleOsc(const QString& address, double value,
                                    const QString&) {
    const QString endpoint_uuid = osc_role->currentData().toString();
    processControl(pvt::LiveControlInput::OscValue, 0, 0, value,
                   endpoint_uuid, address);
}

void LiveWorkspace::Impl::processControl(
    pvt::LiveControlInput input, int channel, int number, double value,
    const QString& endpointUuid, const QString& oscAddress) {
    if (!active || endpointUuid.isEmpty() || !std::isfinite(value)) return;
    for (int index = 0; index < static_cast<int>(config.mappings.size()); ++index) {
        const auto& mapping = config.mappings[static_cast<std::size_t>(index)];
        if (!mapping.enabled || mapping.input != input
            || qtext(mapping.endpoint_uuid) != endpointUuid) continue;
        if (input == pvt::LiveControlInput::OscValue) {
            if (qtext(mapping.osc_address) != oscAddress) continue;
        } else if (input == pvt::LiveControlInput::Footswitch) {
            if (mapping.control_number != number) continue;
        } else {
            if (mapping.midi_channel != 0 && mapping.midi_channel != channel) continue;
            if ((input == pvt::LiveControlInput::MidiControlChange
                 || input == pvt::LiveControlInput::MidiNote
                 || input == pvt::LiveControlInput::MidiProgramChange)
                && mapping.control_number != number) continue;
        }
        bool fire = false;
        const double transformed = transformedValue(mapping, index, value, fire);
        performMapping(mapping, transformed, fire);
    }
}

double LiveWorkspace::Impl::transformedValue(
    const pvt::LiveControlMapping& mapping, int mappingIndex, double raw,
    bool& fire) {
    MappingRuntime& state = mapping_runtime[mappingIndex];
    const double denominator = mapping.input_maximum - mapping.input_minimum;
    double normalized = denominator > 0.0
        ? (raw - mapping.input_minimum) / denominator : 0.0;
    normalized = std::clamp(normalized, 0.0, 1.0);
    if (normalized <= mapping.dead_zone) {
        normalized = 0.0;
    } else if (mapping.dead_zone > 0.0) {
        normalized = (normalized - mapping.dead_zone) / (1.0 - mapping.dead_zone);
    }
    normalized = std::pow(std::clamp(normalized, 0.0, 1.0), mapping.curve);
    const bool high = normalized >= 0.5;
    const bool rising = high && !state.high;
    state.high = high;
    fire = true;
    double output = mapping.output_minimum
        + normalized * (mapping.output_maximum - mapping.output_minimum);
    switch (mapping.mode) {
        case pvt::LiveMappingMode::Absolute:
            if (mapping.target != pvt::LiveMappingTarget::Setting) fire = rising;
            break;
        case pvt::LiveMappingMode::Relative: {
            // Support the common centered relative encoding without assuming
            // a particular controller's two's-complement convention.
            const double direction = raw > 0.5 ? raw - 1.0 : raw;
            const QString path = qtext(mapping.target_path);
            const double current = overrides.contains(path)
                ? overrides[path].target : mapping.output_minimum;
            output = current + direction
                * (mapping.output_maximum - mapping.output_minimum) * 0.1;
            output = std::clamp(output,
                std::min(mapping.output_minimum, mapping.output_maximum),
                std::max(mapping.output_minimum, mapping.output_maximum));
            break;
        }
        case pvt::LiveMappingMode::Toggle:
            fire = rising;
            if (fire) {
                const QString path = qtext(mapping.target_path);
                const bool on = overrides.contains(path)
                    && overrides[path].target
                           > (mapping.output_minimum + mapping.output_maximum) * 0.5;
                output = on ? mapping.output_minimum : mapping.output_maximum;
            }
            break;
        case pvt::LiveMappingMode::Momentary:
            output = high ? mapping.output_maximum : mapping.output_minimum;
            break;
        case pvt::LiveMappingMode::Trigger:
            fire = rising;
            output = mapping.output_maximum;
            break;
    }
    state.previous_raw = raw;
    return output;
}

void LiveWorkspace::Impl::performMapping(
    const pvt::LiveControlMapping& mapping, double value, bool fire) {
    if (!fire) return;
    if (mapping.target == pvt::LiveMappingTarget::Setting) {
        const QString path = qtext(mapping.target_path);
        const bool existed = overrides.contains(path);
        OverrideValue& target = overrides[path];
        if (!existed) target.current = value;
        target.target = value;
        target.smoothing_ms = mapping.smoothing_milliseconds;
        target.changed_ms = run_clock.elapsed();
    } else if (mapping.target == pvt::LiveMappingTarget::Action) {
        if (mapping.mode == pvt::LiveMappingMode::Momentary
            && mapping.action == pvt::LiveAction::Freeze) {
            setFreeze(value >= (mapping.output_minimum + mapping.output_maximum) * 0.5);
        } else if (mapping.mode == pvt::LiveMappingMode::Momentary
                   && mapping.action == pvt::LiveAction::Blackout) {
            setBlackout(value >= (mapping.output_minimum + mapping.output_maximum) * 0.5);
        } else {
            performAction(mapping.action, value, fire);
        }
    } else {
        takeScene(mapping.scene_uuid);
    }
}

void LiveWorkspace::Impl::performAction(pvt::LiveAction action, double value,
                                        bool fire) {
    if (!fire) return;
    switch (action) {
        case pvt::LiveAction::Freeze:
            if (value >= 0.5) setFreeze(!user_freeze);
            break;
        case pvt::LiveAction::Blackout:
            if (value >= 0.5) setBlackout(!user_blackout);
            break;
        case pvt::LiveAction::NextScene:
            selectRelativeScene(1); break;
        case pvt::LiveAction::PreviousScene:
            selectRelativeScene(-1); break;
        case pvt::LiveAction::RestartScene:
            if (!scene_transition.scene_uuid.isEmpty()) {
                takeScene(narrow(scene_transition.scene_uuid), true);
            } else {
                takeSelectedScene();
            }
            break;
        case pvt::LiveAction::TapTempo:
            tapTempo(); break;
    }
}

void LiveWorkspace::Impl::captureScene(bool updateExisting) {
    int row = scene_list->currentRow();
    if (updateExisting
        && (row < 0 || row >= static_cast<int>(config.scenes.size()))) return;
    QString name;
    if (updateExisting) {
        name = qtext(config.scenes[static_cast<std::size_t>(row)].name);
    } else {
        bool ok = false;
        name = QInputDialog::getText(q, q->tr("Capture Live Scene"),
                                     q->tr("Scene name"), QLineEdit::Normal,
                                     q->tr("Scene %1").arg(config.scenes.size() + 1),
                                     &ok).trimmed();
        if (!ok || name.isEmpty()) return;
    }
    pvt::ProjectConfig snapshot = runtimeProject();
    const auto registry = buildLiveTargetRegistry(snapshot);
    pvt::LiveSceneConfig scene;
    if (updateExisting) scene = config.scenes[static_cast<std::size_t>(row)];
    else scene.uuid = narrow(uuid_text());
    scene.name = narrow(name);
    scene.transition_milliseconds = scene_transition_ms->value();
    scene.values.clear();
    scene.values.reserve(registry.size());
    for (const auto& target : registry) {
        pvt::LiveSceneValue value;
        value.target_path = narrow(target.path);
        value.type = scene_type_for(target.kind);
        value.value = narrow(number_text(target.current_value, target.kind));
        scene.values.push_back(std::move(value));
    }
    pvt::LiveConfig next = config;
    if (updateExisting) next.scenes[static_cast<std::size_t>(row)] = std::move(scene);
    else next.scenes.push_back(std::move(scene));
    commitConfig(std::move(next), updateExisting ? q->tr("Update live scene")
                                                 : q->tr("Capture live scene"));
    if (!updateExisting) scene_list->setCurrentRow(scene_list->count() - 1);
}

void LiveWorkspace::Impl::removeScene() {
    const int row = scene_list->currentRow();
    if (row < 0 || row >= static_cast<int>(config.scenes.size())) return;
    const std::string uuid = config.scenes[static_cast<std::size_t>(row)].uuid;
    pvt::LiveConfig next = config;
    next.scenes.erase(next.scenes.begin() + row);
    if (next.startup_scene_uuid == uuid) next.startup_scene_uuid.clear();
    next.mappings.erase(std::remove_if(next.mappings.begin(), next.mappings.end(),
        [&uuid](const pvt::LiveControlMapping& mapping) {
            return mapping.target == pvt::LiveMappingTarget::Scene
                && mapping.scene_uuid == uuid;
        }), next.mappings.end());
    commitConfig(std::move(next), q->tr("Remove live scene"));
}

void LiveWorkspace::Impl::takeSelectedScene() {
    const int row = scene_list->currentRow();
    if (row < 0 || row >= static_cast<int>(config.scenes.size())) return;
    takeScene(config.scenes[static_cast<std::size_t>(row)].uuid);
}

void LiveWorkspace::Impl::selectRelativeScene(int delta) {
    if (config.scenes.empty()) return;
    int row = scene_list->currentRow();
    if (row < 0) row = 0;
    row = (row + delta) % static_cast<int>(config.scenes.size());
    if (row < 0) row += static_cast<int>(config.scenes.size());
    scene_list->setCurrentRow(row);
    takeSelectedScene();
}

void LiveWorkspace::Impl::takeScene(const std::string& uuid, bool) {
    const auto found = std::find_if(
        config.scenes.begin(), config.scenes.end(),
        [&uuid](const pvt::LiveSceneConfig& scene) { return scene.uuid == uuid; });
    if (found == config.scenes.end()) return;
    pvt::ProjectConfig current = project_provider
        ? project_provider() : pvt::default_project();
    applyOverrides(current);
    const auto registry = buildLiveTargetRegistry(current);
    QHash<QString, LiveTargetDescriptor> targets;
    for (const auto& item : registry) targets.insert(item.path, item);
    SceneTransition next;
    next.active = true;
    next.scene_uuid = qtext(found->uuid);
    next.duration_ms = found->transition_milliseconds;
    next.started_ms = run_clock.isValid() ? run_clock.elapsed() : 0;
    for (const auto& value : found->values) {
        const QString path = qtext(value.target_path);
        if (!targets.contains(path)) continue;
        double destination = 0.0;
        if (!parse_scene_number(value, destination)) continue;
        next.from.insert(path, targets[path].current_value);
        next.to.insert(path, destination);
        if (value.type == pvt::LiveSceneValueType::Boolean
            || value.type == pvt::LiveSceneValueType::Integer
            || value.type == pvt::LiveSceneValueType::EnumToken) {
            next.discrete.insert(path);
        }
    }
    scene_transition = std::move(next);
    scene_readout->setText(q->tr("Scene: %1").arg(qtext(found->name)));
    for (int row = 0; row < scene_list->count(); ++row) {
        if (scene_list->item(row)->data(Qt::UserRole).toString() == qtext(uuid)) {
            scene_list->setCurrentRow(row);
            break;
        }
    }
    if (scene_transition.duration_ms == 0) {
        for (auto it = scene_transition.to.cbegin(); it != scene_transition.to.cend(); ++it) {
            OverrideValue& target = overrides[it.key()];
            target.current = it.value();
            target.target = it.value();
            target.smoothing_ms = 0;
        }
        scene_transition.active = false;
    }
}

pvt::ProjectConfig LiveWorkspace::Impl::runtimeProject() {
    // Authoring and performance are concurrent. Always take the latest
    // project; the cached registry only supplies stable target paths/setters
    // for transient overrides and must never freeze ordinary UI edits.
    pvt::ProjectConfig project = project_provider
        ? project_provider()
        : (project_cache_valid ? project_cache : pvt::default_project());
    project.canvas.live = config;
    applyOverrides(project);
    return project;
}

void LiveWorkspace::Impl::applyOverrides(pvt::ProjectConfig& project) {
    const qint64 now = run_clock.isValid() ? run_clock.elapsed() : 0;
    if (scene_transition.active) {
        const double amount = scene_transition.duration_ms <= 0 ? 1.0
            : std::clamp(static_cast<double>(now - scene_transition.started_ms)
                             / scene_transition.duration_ms,
                         0.0, 1.0);
        const double smooth = amount * amount * (3.0 - 2.0 * amount);
        for (auto it = scene_transition.to.cbegin(); it != scene_transition.to.cend(); ++it) {
            if (scene_transition.discrete.contains(it.key()) && amount < 1.0) continue;
            const double from = scene_transition.from.value(it.key(), it.value());
            const double value = scene_transition.discrete.contains(it.key())
                ? it.value() : from + (it.value() - from) * smooth;
            OverrideValue& target = overrides[it.key()];
            target.current = value;
            target.target = value;
            target.smoothing_ms = 0;
            target.changed_ms = now;
        }
        if (amount >= 1.0) scene_transition.active = false;
    }
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        OverrideValue& value = it.value();
        if (value.smoothing_ms > 0 && value.current != value.target) {
            const qint64 elapsed = std::max<qint64>(0, now - value.changed_ms);
            const double amount = std::clamp(
                static_cast<double>(elapsed) / value.smoothing_ms, 0.0, 1.0);
            value.current += (value.target - value.current) * amount;
            value.changed_ms = now;
            if (std::fabs(value.target - value.current) < 1.0e-9) {
                value.current = value.target;
            }
        } else {
            value.current = value.target;
        }
        const auto cached = target_index.constFind(it.key());
        if (cached != target_index.cend()) {
            const int index = cached.value();
            if (index >= 0 && index < static_cast<int>(target_cache.size())) {
                (void)target_cache[static_cast<std::size_t>(index)].apply(
                    project, value.current);
            }
        }
    }
}

double LiveWorkspace::Impl::basePhase(const pvt::ProjectConfig& project) const {
    const double duration = project_loop_duration_seconds(project);
    const double seconds = run_clock.isValid()
        ? static_cast<double>(run_clock.elapsed()) / 1000.0 : 0.0;
    double phase = std::fmod(seconds / duration, 1.0);
    return phase < 0.0 ? phase + 1.0 : phase;
}

std::optional<double> LiveWorkspace::Impl::routedPhase(
    const pvt::ProjectConfig& project,
    const pvt::LiveClockInputConfig& input) {
    const auto* role = endpoint(input.endpoint_uuid);
    const double duration = project_loop_duration_seconds(project);
    const pvt::ClockConfig* reference_clock = &project.canvas.clock;
    if (input.target == pvt::LiveClockTarget::Layer) {
        const auto layer = std::find_if(
            project.layers.begin(), project.layers.end(),
            [&input](const pvt::LayerConfig& item) {
                return item.uuid == input.layer_uuid;
            });
        if (layer != project.layers.end()) {
            reference_clock = &layer->render.layer_clock.clock;
        }
    }
    const double reference_bpm = std::isfinite(reference_clock->meter.bpm)
                                      && reference_clock->meter.bpm > 0.0
        ? reference_clock->meter.bpm : 120.0;
    const double reference_beats_per_loop = duration * reference_bpm / 60.0;
    const auto transform = [reference_clock](double phase) {
        phase = (reference_clock->reverse ? -phase : phase)
            + reference_clock->phase_offset_degrees / 360.0;
        phase -= std::floor(phase);
        return phase < 0.0 ? phase + 1.0 : phase;
    };
    const double portable_latency_seconds = role == nullptr ? 0.0
        : static_cast<double>(role->input_latency_microseconds) / 1000000.0;
    if (input.source == pvt::LiveClockInputSource::MidiClock) {
        const auto snapshot = midi.clockSnapshot(boundSource(input.endpoint_uuid));
        if (!snapshot.receiving || (input.follow_midi_transport && !snapshot.running)) {
            return std::nullopt;
        }
        const double live_bpm = std::isfinite(snapshot.bpm) && snapshot.bpm > 0.0
            ? snapshot.bpm : reference_bpm;
        const auto phase = pvt::audio::live_beat_route_phase(
            1U, static_cast<double>(snapshot.ticks) / 24.0,
            reference_beats_per_loop,
            portable_latency_seconds * live_bpm / 60.0,
            reference_clock->interpolation);
        return phase.has_value()
            ? std::optional<double>(transform(*phase)) : std::nullopt;
    }

    const std::string selected_role = narrow(
        audio_role->currentData().toString());
    if (selected_role != input.endpoint_uuid) {
        warnRouteOnce(
            qtext(input.endpoint_uuid) + QStringLiteral(":wrong-audio-role"),
            q->tr("An effective audio clock uses a different logical role than the active microphone. Its deterministic clock is being used instead; choose the same role in Live or edit the advanced route."));
        return std::nullopt;
    }
    const double callback_age_seconds = live_audio_loss_age_seconds(
        audio_snapshot, audio_dropout_clock);
    if (!pvt::audio::live_audio_callback_within_holdover(
            callback_age_seconds, input.holdover_milliseconds)) {
        return std::nullopt;
    }

    std::uint64_t beat_count = audio_snapshot.beat_count;
    double beat_position = audio_snapshot.beat_position;
    double anchor_seconds = audio_snapshot.beat_anchor_seconds;
    double detected_bpm = audio_snapshot.detected_bpm;
    if (!input.frequency_stream_uuid.empty()) {
        const auto stream = std::find_if(
            audio_snapshot.frequency_streams.begin(),
            audio_snapshot.frequency_streams.end(),
            [&input](
                const pvt::audio::LiveAudioSnapshot::FrequencyStream& item) {
                return item.uuid == input.frequency_stream_uuid;
            });
        if (stream == audio_snapshot.frequency_streams.end()) {
            warnRouteOnce(
                qtext(input.frequency_stream_uuid)
                    + QStringLiteral(":missing-frequency-stream"),
                q->tr("An audio clock's frequency stream is unavailable. Its deterministic clock is being used instead."));
            return std::nullopt;
        }
        beat_count = stream->beat_count;
        beat_position = stream->beat_position;
        anchor_seconds = stream->beat_anchor_seconds;
        detected_bpm = stream->detected_bpm;
    }

    const double live_bpm = std::isfinite(detected_bpm) && detected_bpm > 0.0
        ? detected_bpm
        : (std::isfinite(tapped_bpm) && tapped_bpm > 0.0
               ? tapped_bpm : reference_bpm);
    bool used_tapped_fallback = false;
    if (beat_count > 0U && (!(std::isfinite(detected_bpm))
                            || detected_bpm <= 0.0)) {
        const double elapsed = std::max(
            0.0, audio_snapshot.stream_seconds - anchor_seconds);
        beat_position = static_cast<double>(beat_count - 1U)
            + elapsed * live_bpm / 60.0;
    } else if (beat_count == 0U && tapped_beat_count > 0U
               && tapped_bpm > 0.0) {
        const double now_seconds = run_clock.isValid()
            ? static_cast<double>(run_clock.elapsed()) / 1000.0 : 0.0;
        beat_count = tapped_beat_count;
        beat_position = static_cast<double>(beat_count - 1U)
            + std::max(0.0, now_seconds - tapped_beat_anchor_seconds)
                  * tapped_bpm / 60.0;
        used_tapped_fallback = true;
    }
    if (beat_count == 0U) return std::nullopt;
    // The snapshot position ends at the last callback containing samples.
    // Extrapolate once from that callback's monotonic age during both normal
    // callback spacing and dropout holdover. This makes the transition across
    // the 500 ms receiving indicator continuous and avoids a second timer.
    if (audio_snapshot.last_valid_callback_age_seconds >= 0.0
        && !used_tapped_fallback) {
        beat_position = pvt::audio::live_audio_extrapolated_beat_position(
            beat_position, live_bpm,
            audio_snapshot.last_valid_callback_age_seconds);
    }

    const qint64 local_latency_microseconds = QSettings().value(
        device_latency_key(input.endpoint_uuid,
                           audio_device->currentData().toString()),
        0).toLongLong();
    const double signed_latency_beats =
        (portable_latency_seconds
         + static_cast<double>(local_latency_microseconds) / 1000000.0)
        * live_bpm / 60.0;
    const auto phase = pvt::audio::live_beat_route_phase(
        beat_count, beat_position, reference_beats_per_loop,
        signed_latency_beats, reference_clock->interpolation);
    return phase.has_value()
        ? std::optional<double>(transform(*phase)) : std::nullopt;
}

pvt::MusicAnalysis LiveWorkspace::Impl::ephemeralAnalysis(
    double durationSeconds, const std::string& selectedStreamUuid) const {
    pvt::MusicAnalysis analysis;
    analysis.schema_version = 1;
    analysis.analyzer_version = "pvt-live-causal-1";
    analysis.source_sha256 = std::string(64U, '0');
    analysis.source_basename = "live-input";
    analysis.source_format = "stream";
    analysis.source_sample_rate = 48000U;
    analysis.source_channel_count = 1U;
    analysis.source_frame_count = std::max<std::uint64_t>(
        1U, static_cast<std::uint64_t>(std::llround(
                std::max(1.0 / 48000.0, durationSeconds) * 48000.0)));
    analysis.duration_seconds = static_cast<double>(analysis.source_frame_count)
        / static_cast<double>(analysis.source_sample_rate);
    pvt::MusicFeatureSample selected_features = audio_snapshot.features;
    double selected_bpm = audio_snapshot.detected_bpm;
    std::uint64_t selected_beat_count = audio_snapshot.beat_count;
    if (!selectedStreamUuid.empty()) {
        const auto selected = std::find_if(
            audio_snapshot.frequency_streams.begin(),
            audio_snapshot.frequency_streams.end(),
            [&selectedStreamUuid](
                const pvt::audio::LiveAudioSnapshot::FrequencyStream& item) {
                return item.uuid == selectedStreamUuid;
            });
        if (selected != audio_snapshot.frequency_streams.end()) {
            selected_features = selected->features;
            selected_bpm = selected->detected_bpm;
            selected_beat_count = selected->beat_count;
        }
    }
    analysis.detected_bpm = std::clamp(
        selected_bpm > 0.0 ? selected_bpm
                           : (tapped_bpm > 0.0 ? tapped_bpm : 120.0),
        std::numeric_limits<double>::min(),
        pvt::maximum_render_parameter_magnitude());
    analysis.tempo_confidence = audio_snapshot.receiving
        ? (selected_beat_count > 1U ? 0.75 : 0.25) : 0.0;
    analysis.beat_times_seconds = {0.0};
    analysis.tempo_points = {{0.0, analysis.detected_bpm,
                              analysis.tempo_confidence}};
    analysis.feature_samples = {selected_features, selected_features};
    analysis.input_processing = config.audio_processing;
    analysis.frequency_streams.reserve(
        config.audio_processing.frequency_streams.size());
    for (const auto& authored : config.audio_processing.frequency_streams) {
        pvt::MusicFrequencyStreamAnalysis stream;
        stream.uuid = authored.uuid;
        stream.low_hz = authored.low_hz;
        stream.high_hz = authored.high_hz;
        pvt::MusicFeatureSample features;
        double bpm = 0.0;
        std::uint64_t beat_count = 0U;
        const auto captured = std::find_if(
            audio_snapshot.frequency_streams.begin(),
            audio_snapshot.frequency_streams.end(),
            [&authored](
                const pvt::audio::LiveAudioSnapshot::FrequencyStream& item) {
                return item.uuid == authored.uuid;
            });
        if (captured != audio_snapshot.frequency_streams.end()) {
            features = captured->features;
            bpm = captured->detected_bpm;
            beat_count = captured->beat_count;
        }
        stream.detected_bpm = bpm > 0.0 ? bpm
            : (tapped_bpm > 0.0 ? tapped_bpm : 120.0);
        stream.tempo_confidence = audio_snapshot.receiving
            ? (beat_count > 1U ? 0.75 : 0.25) : 0.0;
        stream.beat_times_seconds = {0.0};
        stream.tempo_points = {{0.0, stream.detected_bpm,
                                stream.tempo_confidence}};
        stream.feature_samples = {features, features};
        analysis.frequency_streams.push_back(std::move(stream));
    }
    return analysis;
}

bool LiveWorkspace::Impl::applyClockRoutes(pvt::ProjectConfig& project,
                                           double& projectPhase) {
    const std::string active_layer = active_layer_provider
        ? active_layer_provider() : std::string{};
    const auto project_route = std::find_if(
        config.clock_inputs.begin(), config.clock_inputs.end(),
        [](const pvt::LiveClockInputConfig& item) {
            return item.enabled && item.target == pvt::LiveClockTarget::Project;
        });
    const auto layer_route = std::find_if(
        config.clock_inputs.begin(), config.clock_inputs.end(),
        [&active_layer](const pvt::LiveClockInputConfig& item) {
            return item.enabled && item.target == pvt::LiveClockTarget::Layer
                && item.layer_uuid == active_layer;
        });
    const auto project_routed = project_route == config.clock_inputs.end()
        ? std::optional<double>{} : routedPhase(project, *project_route);
    const auto layer_routed = layer_route == config.clock_inputs.end()
        ? std::optional<double>{} : routedPhase(project, *layer_route);
    const double duration = project_loop_duration_seconds(project);
    const auto prepare_audio_clock =
        [this, duration](pvt::ClockConfig& clock,
                         const std::string& selected_stream) {
        const bool stream_is_authored = selected_stream.empty()
            || std::any_of(
                config.audio_processing.frequency_streams.begin(),
                config.audio_processing.frequency_streams.end(),
                [&selected_stream](
                    const pvt::AudioFrequencyStreamConfig& item) {
                    return item.uuid == selected_stream;
                });
        clock.audio_processing = config.audio_processing;
        clock.frequency_stream_uuid = stream_is_authored
            ? selected_stream : std::string{};
        clock.music = ephemeralAnalysis(duration,
                                        clock.frequency_stream_uuid);
        clock.mode = pvt::ClockMode::Music;
        clock.music_tempo = pvt::MusicTempoMode::Detected;
    };

    if (project_routed.has_value()) {
        projectPhase = *project_routed;
        if (project_route->source
            == pvt::LiveClockInputSource::AudioStream) {
            prepare_audio_clock(project.canvas.clock,
                                project_route->frequency_stream_uuid);
        } else {
            project.canvas.clock.mode = pvt::ClockMode::Default;
        }
        project.canvas.clock.phase_offset_degrees = 0.0;
        project.canvas.clock.reverse = false;
    }
    if (layer_routed.has_value()) {
        const double desired = *layer_routed;
        const auto found = std::find_if(
            project.layers.begin(), project.layers.end(),
            [&active_layer](const pvt::LayerConfig& layer) {
                return layer.uuid == active_layer;
            });
        if (found != project.layers.end()) {
            found->render.layer_clock.enabled = true;
            if (layer_route->source
                == pvt::LiveClockInputSource::AudioStream) {
                prepare_audio_clock(
                    found->render.layer_clock.clock,
                    layer_route->frequency_stream_uuid);
                found->render.layer_clock.scale =
                    pvt::LayerClockScale::StraightFit;
            } else {
                found->render.layer_clock.clock.mode = pvt::ClockMode::Default;
            }
            found->render.layer_clock.clock.reverse = false;
            found->render.layer_clock.clock.phase_offset_degrees =
                (desired - projectPhase) * 360.0;
            found->render.layer_clock.mix_enabled = false;
            found->render.layer_clock.mix = pvt::LayerClockMixMode::Replace;
        }
    }
    return project_routed.has_value();
}

double LiveWorkspace::Impl::outputBpm(
    const pvt::ProjectConfig& project,
    const pvt::LiveMidiClockOutputConfig& output) const {
    if (output.source == pvt::LiveClockTarget::Project) {
        const auto input = std::find_if(
            config.clock_inputs.begin(), config.clock_inputs.end(),
            [](const pvt::LiveClockInputConfig& item) {
                return item.enabled && item.target == pvt::LiveClockTarget::Project;
            });
        if (input != config.clock_inputs.end()
            && input->source == pvt::LiveClockInputSource::MidiClock
            && midi.clockSnapshot(boundSource(input->endpoint_uuid)).bpm > 0.0) {
            return midi.clockSnapshot(boundSource(input->endpoint_uuid)).bpm;
        }
        if (input != config.clock_inputs.end()
            && input->source == pvt::LiveClockInputSource::AudioStream) {
            if (!input->frequency_stream_uuid.empty()) {
                const auto stream = std::find_if(
                    audio_snapshot.frequency_streams.begin(),
                    audio_snapshot.frequency_streams.end(),
                    [&input](const pvt::audio::LiveAudioSnapshot::FrequencyStream& item) {
                        return item.uuid == input->frequency_stream_uuid;
                    });
                if (stream != audio_snapshot.frequency_streams.end()
                    && stream->detected_bpm > 0.0) return stream->detected_bpm;
            }
            if (audio_snapshot.detected_bpm > 0.0) {
                return audio_snapshot.detected_bpm;
            }
        }
        return project.canvas.clock.meter.bpm;
    }
    const auto layer = std::find_if(
        project.layers.begin(), project.layers.end(),
        [&output](const pvt::LayerConfig& item) {
            return item.uuid == output.layer_uuid;
        });
    const auto input = std::find_if(
        config.clock_inputs.begin(), config.clock_inputs.end(),
        [&output](const pvt::LiveClockInputConfig& item) {
            return item.enabled && item.target == pvt::LiveClockTarget::Layer
                && item.layer_uuid == output.layer_uuid;
        });
    if (input != config.clock_inputs.end()) {
        if (input->source == pvt::LiveClockInputSource::MidiClock
            && midi.clockSnapshot(boundSource(input->endpoint_uuid)).bpm > 0.0) {
            return midi.clockSnapshot(boundSource(input->endpoint_uuid)).bpm;
        }
        if (input->source == pvt::LiveClockInputSource::AudioStream) {
            if (!input->frequency_stream_uuid.empty()) {
                const auto stream = std::find_if(
                    audio_snapshot.frequency_streams.begin(),
                    audio_snapshot.frequency_streams.end(),
                    [&input](const pvt::audio::LiveAudioSnapshot::FrequencyStream& item) {
                        return item.uuid == input->frequency_stream_uuid;
                    });
                if (stream != audio_snapshot.frequency_streams.end()
                    && stream->detected_bpm > 0.0) return stream->detected_bpm;
            }
            if (audio_snapshot.detected_bpm > 0.0) {
                return audio_snapshot.detected_bpm;
            }
        }
    }
    return layer == project.layers.end() ? project.canvas.clock.meter.bpm
        : layer->render.layer_clock.clock.meter.bpm;
}

void LiveWorkspace::Impl::sendClockOutputs() {
    if (!active || clock_outputs.isEmpty() || !run_clock.isValid()
        || !project_cache_valid) return;
    const pvt::ProjectConfig& project = project_cache;
    const double now = static_cast<double>(run_clock.elapsed()) / 1000.0;
    int runtime_index = 0;
    for (const auto& output : config.midi_clock_outputs) {
        if (!output.enabled) continue;
        if (runtime_index >= clock_outputs.size()) break;
        const double bpm = std::clamp(
            outputBpm(project, output), std::numeric_limits<double>::min(),
            pvt::maximum_render_parameter_magnitude());
        const double period = 60.0 / (bpm * 24.0);
        int sent = 0;
        while (now >= clock_outputs[runtime_index].next_seconds
               && sent < kMaximumClockCatchupTicks) {
            midi.sendClockTick(runtime_index);
            clock_outputs[runtime_index].next_seconds += period;
            ++sent;
        }
        if (sent == kMaximumClockCatchupTicks
            && now >= clock_outputs[runtime_index].next_seconds) {
            clock_outputs[runtime_index].next_seconds = now + period;
        }
        ++runtime_index;
    }
}

void LiveWorkspace::Impl::editClockRoute(bool layerTarget) {
    if (rebuilding) return;
    QComboBox* source_combo = layerTarget ? layer_clock : project_clock;
    QComboBox* role_combo = layerTarget ? layer_clock_role : project_clock_role;
    QComboBox* stream_combo = layerTarget ? layer_clock_stream
                                          : project_clock_stream;
    const int source_value = source_combo->currentData().toInt();
    const std::string layer_uuid = active_layer_provider
        ? active_layer_provider() : std::string{};
    if (layerTarget && layer_uuid.empty()) return;
    pvt::LiveConfig next = config;
    next.clock_inputs.erase(
        std::remove_if(next.clock_inputs.begin(), next.clock_inputs.end(),
            [layerTarget, &layer_uuid](const pvt::LiveClockInputConfig& item) {
                return item.target == (layerTarget ? pvt::LiveClockTarget::Layer
                                                   : pvt::LiveClockTarget::Project)
                    && (!layerTarget || item.layer_uuid == layer_uuid);
            }), next.clock_inputs.end());
    if (source_value >= 0) {
        const auto source = static_cast<pvt::LiveClockInputSource>(source_value);
        const pvt::LiveEndpointProtocol required =
            source == pvt::LiveClockInputSource::AudioStream
                ? pvt::LiveEndpointProtocol::Audio : pvt::LiveEndpointProtocol::Midi;
        std::string role_uuid = narrow(role_combo->currentData().toString());
        const auto* selected = endpoint(role_uuid);
        if (selected == nullptr || selected->protocol != required
            || !direction_has_input(selected->direction)) {
            const auto found = std::find_if(
                next.endpoints.begin(), next.endpoints.end(),
                [required](const pvt::LiveEndpointConfig& item) {
                    return item.protocol == required && direction_has_input(item.direction);
                });
            if (found == next.endpoints.end()) {
                refreshClockRouting();
                return;
            }
            role_uuid = found->uuid;
        }
        pvt::LiveClockInputConfig route;
        route.enabled = true;
        route.target = layerTarget ? pvt::LiveClockTarget::Layer
                                   : pvt::LiveClockTarget::Project;
        route.layer_uuid = layerTarget ? layer_uuid : std::string{};
        route.source = source;
        route.endpoint_uuid = role_uuid;
        route.frequency_stream_uuid =
            source == pvt::LiveClockInputSource::AudioStream
                ? narrow(stream_combo->currentData().toString())
                : std::string{};
        route.follow_midi_transport = true;
        route.holdover_milliseconds = 500;
        next.clock_inputs.push_back(std::move(route));
    }
    commitConfig(std::move(next), layerTarget
        ? q->tr("Patch active-layer live clock")
        : q->tr("Patch project live clock"));
}

void LiveWorkspace::Impl::editClockOutput(bool layerTarget) {
    if (rebuilding) return;
    QCheckBox* enabled = layerTarget ? layer_clock_out : project_clock_out;
    QComboBox* role_combo = layerTarget ? layer_clock_out_role
                                        : project_clock_out_role;
    role_combo->setEnabled(enabled->isChecked());
    const std::string layer_uuid = active_layer_provider
        ? active_layer_provider() : std::string{};
    if (layerTarget && layer_uuid.empty()) return;
    pvt::LiveConfig next = config;
    next.midi_clock_outputs.erase(
        std::remove_if(next.midi_clock_outputs.begin(), next.midi_clock_outputs.end(),
            [layerTarget, &layer_uuid](const pvt::LiveMidiClockOutputConfig& item) {
                return item.source == (layerTarget ? pvt::LiveClockTarget::Layer
                                                   : pvt::LiveClockTarget::Project)
                    && (!layerTarget || item.layer_uuid == layer_uuid);
            }), next.midi_clock_outputs.end());
    if (enabled->isChecked()) {
        const std::string role_uuid = narrow(role_combo->currentData().toString());
        const auto* role = endpoint(role_uuid);
        if (role == nullptr || role->protocol != pvt::LiveEndpointProtocol::Midi
            || !direction_has_output(role->direction)) {
            refreshClockRouting();
            return;
        }
        pvt::LiveMidiClockOutputConfig output;
        output.enabled = true;
        output.source = layerTarget ? pvt::LiveClockTarget::Layer
                                    : pvt::LiveClockTarget::Project;
        output.layer_uuid = layerTarget ? layer_uuid : std::string{};
        output.endpoint_uuid = role_uuid;
        output.send_transport = true;
        output.send_song_position = true;
        next.midi_clock_outputs.push_back(std::move(output));
    }
    commitConfig(std::move(next), layerTarget
        ? q->tr("Change active-layer MIDI clock output")
        : q->tr("Change project MIDI clock output"));
}

LiveWorkspace::LiveWorkspace(ProjectSnapshotProvider projectProvider,
                             ProjectSnapshotProvider presentationProjectProvider,
                             PresentationFrameProvider presentationFrameProvider,
                             RenderOptionsProvider renderOptionsProvider,
                             DocumentRevisionProvider documentRevisionProvider,
                             ActiveLayerUuidProvider activeLayerProvider,
                             AuthoredConfigEditor authoredConfigEditor,
                             QWidget* parent)
    : QWidget(parent),
      impl_(std::make_unique<Impl>(this, std::move(projectProvider),
                                   std::move(presentationProjectProvider),
                                   std::move(presentationFrameProvider),
                                   std::move(renderOptionsProvider),
                                   std::move(documentRevisionProvider),
                                   std::move(activeLayerProvider),
                                   std::move(authoredConfigEditor))) {}

LiveWorkspace::~LiveWorkspace() = default;

void LiveWorkspace::setProjectLiveConfig(const pvt::LiveConfig& config) {
    const bool outputs_changed = clock_output_signature(impl_->config)
        != clock_output_signature(config);
    const bool endpoints_changed = endpoint_structure_signature(impl_->config)
        != endpoint_structure_signature(config);
    impl_->config = config;
    if (impl_->target_cache.empty()) impl_->rebuildTargetCache();
    impl_->refreshConfigUi();
    if (impl_->active) {
        impl_->updateSleepPrevention();
        if (outputs_changed) impl_->configureClockOutputs();
        if (endpoints_changed) {
            impl_->restartAudio();
            impl_->restartOsc();
        }
    }
    impl_->refreshRuntimeRouting();
    if (impl_->stage.isVisible()) impl_->applyVisibleOutputPolicy();
}

void LiveWorkspace::refreshProjectSnapshot() {
    ++impl_->render_generation;
    impl_->renderer.cancelCurrent();
    impl_->rebuildTargetCache();
    impl_->refreshConfigUi();
    impl_->refreshRuntimeRouting();
    if (impl_->realtimeActive()) {
        const pvt::ProjectConfig project = impl_->presentation_active
            ? (impl_->presentation_project_provider
                   ? impl_->presentation_project_provider()
                   : pvt::default_project())
            : (impl_->project_provider
                   ? impl_->project_provider() : pvt::default_project());
        impl_->restartFrameSchedule(project.canvas.fps);
        impl_->requestFrame();
    }
}

void LiveWorkspace::setProjectEditingEnabled(bool enabled) {
    impl_->project_editing_enabled = enabled;
    if (!enabled) impl_->learn_mapping = -1;
    if (impl_->tabs != nullptr) impl_->tabs->setEnabled(enabled);
    if (impl_->learn_button != nullptr) {
        impl_->learn_button->setText(
            impl_->learn_mapping >= 0 ? tr("Listening…")
                                      : tr("MIDI Learn"));
    }
}

void LiveWorkspace::setLiveActive(bool active) {
    impl_->setActive(active);
}

bool LiveWorkspace::isLiveActive() const noexcept {
    return impl_->active;
}

void LiveWorkspace::setPresentationActive(bool active) {
    impl_->setPresentationActive(active);
}

bool LiveWorkspace::isPresentationActive() const noexcept {
    return impl_->presentation_active;
}

bool LiveWorkspace::isRealtimeOutputActive() const noexcept {
    return impl_->realtimeActive();
}

void LiveWorkspace::requestRealtimeFrame() {
    if (impl_->realtimeActive()) impl_->requestFrame();
}

void LiveWorkspace::resetRealtimeFrame() {
    impl_->resetRealtimeFrame();
}

QVector<LiveWorkspace::OutputDisplayChoice>
LiveWorkspace::availableOutputDisplays() const {
    QVector<OutputDisplayChoice> choices;
    const auto screens = QGuiApplication::screens();
    choices.reserve(screens.size());
    for (QScreen* screen : screens) {
        choices.push_back({screen_label(
                               screen,
                               screen == QGuiApplication::primaryScreen(),
                               screens),
                           screen_identifier(screen, screens)});
    }
    return choices;
}

QString LiveWorkspace::selectedOutputDisplayId() const {
    if (impl_->screen != nullptr && impl_->screen->currentIndex() >= 0) {
        return impl_->screen->currentData().toString();
    }
    return QSettings().value(QStringLiteral("live/outputScreen")).toString();
}

void LiveWorkspace::setSelectedOutputDisplayId(const QString& id) {
    QString selected = id;
    int index = impl_->screen == nullptr ? -1 : impl_->screen->findData(selected);
    if (index < 0 && impl_->screen != nullptr) {
        for (int candidate = 0; candidate < impl_->screen->count(); ++candidate) {
            if (impl_->screen->itemData(candidate, Qt::UserRole + 1).toString()
                    == selected
                || impl_->screen->itemData(candidate,
                                           Qt::UserRole + 2).toString()
                       == selected) {
                index = candidate;
                selected = impl_->screen->itemData(candidate).toString();
                break;
            }
        }
    }
    if (index < 0 && impl_->screen != nullptr && impl_->screen->count() > 0) {
        index = 0;
        selected = impl_->screen->itemData(0).toString();
    }
    QSettings().setValue(QStringLiteral("live/outputScreen"), selected);
    if (impl_->screen != nullptr && index >= 0) {
        const QSignalBlocker blocker(impl_->screen);
        impl_->screen->setCurrentIndex(index);
    }
    ++impl_->render_generation;
    impl_->renderer.cancelCurrent();
    if (impl_->stage.isVisible()) impl_->applyVisibleOutputPolicy();
    if (impl_->realtimeActive()) impl_->requestFrame();
    emit runtimeOutputSettingsChanged();
}

double LiveWorkspace::outputResolutionScale() const {
    const double stored = QSettings().value(
        QStringLiteral("live/resolutionScale"), 0.0).toDouble();
    for (const double allowed : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        if (std::abs(stored - allowed) <= 1.0e-9) return allowed;
    }
    return 0.0;
}

void LiveWorkspace::setOutputResolutionScale(double scale) {
    double selected = 0.0;
    for (const double allowed : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        if (std::isfinite(scale) && std::abs(scale - allowed) <= 1.0e-9) {
            selected = allowed;
            break;
        }
    }
    QSettings().setValue(QStringLiteral("live/resolutionScale"), selected);
    if (impl_->quality != nullptr) {
        const QSignalBlocker blocker(impl_->quality);
        const int index = impl_->quality->findData(selected);
        impl_->quality->setCurrentIndex(index < 0 ? 0 : index);
    }
    impl_->adaptive_scale = 1.0;
    ++impl_->render_generation;
    impl_->renderer.cancelCurrent();
    if (impl_->realtimeActive()) impl_->requestFrame();
    emit runtimeOutputSettingsChanged();
}

QVector<LiveWorkspace::AudioInputChoice>
LiveWorkspace::availableAudioInputs() const {
    QVector<AudioInputChoice> choices;
    choices.reserve(static_cast<qsizetype>(impl_->audio_devices.size()) + 1);
    choices.push_back({tr("System default"), QString{}, true});
    for (const auto& device : impl_->audio_devices) {
        choices.push_back({
            device.is_default
                ? tr("%1 · default").arg(qtext(device.display_name))
                : qtext(device.display_name),
            qtext(device.runtime_id), device.is_default});
    }
    return choices;
}

void LiveWorkspace::refreshAudioInputs() {
    impl_->refreshDevices();
    impl_->restartAudio();
}

QString LiveWorkspace::audioInputBinding(const std::string& roleUuid) const {
    return QSettings().value(
        endpoint_key(roleUuid, QStringLiteral("audioDevice"))).toString();
}

QString LiveWorkspace::audioInputBindingLabel(
    const std::string& roleUuid) const {
    const QString binding = audioInputBinding(roleUuid);
    if (binding.isEmpty()) return tr("System default");
    const auto match = pvt::audio::find_live_audio_device(
        impl_->audio_devices, narrow(binding));
    if (match.has_value()) {
        return qtext(impl_->audio_devices[*match].display_name);
    }
    QString hint = QSettings().value(
        endpoint_key(roleUuid, QStringLiteral("audioDeviceName"))).toString();
    if (hint.trimmed().isEmpty()) hint = tr("previously selected input");
    return tr("Unavailable · %1").arg(hint);
}

bool LiveWorkspace::audioInputBindingAvailable(
    const std::string& roleUuid) const {
    const QString binding = audioInputBinding(roleUuid);
    return binding.isEmpty()
        || pvt::audio::find_live_audio_device(
               impl_->audio_devices, narrow(binding)).has_value();
}

void LiveWorkspace::setAudioInputBinding(
    const std::string& roleUuid, const QString& runtimeId,
    const QString& displayLabel) {
    const auto* role = impl_->endpoint(roleUuid);
    if (role == nullptr || role->protocol != pvt::LiveEndpointProtocol::Audio
        || !direction_has_input(role->direction)) {
        emit runtimeStatusChanged(
            tr("The selected Live role is not an audio input."));
        return;
    }
    QString normalized_label = displayLabel.trimmed();
    const QString unavailable_prefix = tr("Unavailable · ");
    while (normalized_label.startsWith(unavailable_prefix)) {
        normalized_label.remove(0, unavailable_prefix.size());
    }
    const QString default_suffix = tr(" · default");
    if (normalized_label.endsWith(default_suffix)) {
        normalized_label.chop(default_suffix.size());
    }
    if (normalized_label.isEmpty()) normalized_label = tr("System default");
    QSettings settings;
    settings.setValue(QStringLiteral("live/audioRole"), qtext(roleUuid));
    settings.setValue(endpoint_key(roleUuid, QStringLiteral("audioDevice")),
                      runtimeId);
    settings.setValue(
        endpoint_key(roleUuid, QStringLiteral("audioDeviceName")),
        normalized_label);
    if (impl_->audio_role != nullptr) {
        const QSignalBlocker blocker(impl_->audio_role);
        const int index = impl_->audio_role->findData(qtext(roleUuid));
        if (index >= 0) impl_->audio_role->setCurrentIndex(index);
    }
    impl_->route_warnings.clear();
    impl_->refreshDevices();
    impl_->restartAudio();
    emit audioInputsChanged();
}

void LiveWorkspace::revealAudioInputSetup(const std::string& roleUuid) {
    if (impl_->audio_role != nullptr) {
        const int index = impl_->audio_role->findData(qtext(roleUuid));
        if (index >= 0) {
            const QSignalBlocker blocker(impl_->audio_role);
            impl_->audio_role->setCurrentIndex(index);
            QSettings().setValue(QStringLiteral("live/audioRole"),
                                 qtext(roleUuid));
        }
    }
    impl_->route_warnings.clear();
    impl_->refreshDevices();
    if (!impl_->active) impl_->setActive(true);
    else impl_->restartAudio();
    if (impl_->tabs != nullptr) impl_->tabs->setCurrentIndex(0);
    if (impl_->audio_device != nullptr) {
        impl_->audio_device->setFocus(Qt::OtherFocusReason);
    }
}

bool LiveWorkspace::presentationFullscreen() const {
    return QSettings().value(
        QStringLiteral("previewOutput/fullscreen"), true).toBool();
}

void LiveWorkspace::setPresentationFullscreen(bool fullscreen) {
    QSettings().setValue(QStringLiteral("previewOutput/fullscreen"), fullscreen);
    if (impl_->presentation_active && impl_->stage.isVisible()) {
        impl_->applyVisibleOutputPolicy();
    }
    emit runtimeOutputSettingsChanged();
}

bool LiveWorkspace::presentationHideCursor() const {
    return QSettings().value(
        QStringLiteral("previewOutput/hideCursor"), true).toBool();
}

void LiveWorkspace::setPresentationHideCursor(bool hide) {
    QSettings().setValue(QStringLiteral("previewOutput/hideCursor"), hide);
    if (impl_->presentation_active && impl_->stage.isVisible()) {
        impl_->applyVisibleOutputPolicy();
    }
    emit runtimeOutputSettingsChanged();
}
