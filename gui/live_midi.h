#ifndef PVT_LIVE_MIDI_H
#define PVT_LIVE_MIDI_H

#include <QObject>
#include <QStringList>

#include <cstdint>
#include <memory>

class LiveMidiRouter final : public QObject {
    Q_OBJECT

public:
    enum class MessageKind {
        ControlChange = 0,
        Note,
        ProgramChange,
        ChannelPressure,
        PitchBend
    };
    Q_ENUM(MessageKind)

    struct ClockSnapshot {
        double quarter_note_phase = 0.0;
        double bpm = 0.0;
        std::uint64_t ticks = 0U;
        bool running = false;
        bool receiving = false;
    };

    explicit LiveMidiRouter(QObject* parent = nullptr);
    ~LiveMidiRouter() override;

    bool start(QString* error = nullptr);
    void stop() noexcept;
    bool isRunning() const noexcept;
    QStringList inputNames() const;
    // Empty accepts clock from every connected source. A non-empty name keeps
    // control messages from all devices but isolates clock/transport to the
    // selected runtime endpoint, preventing two stage clocks from interleaving.
    bool selectClockInput(const QString& runtimeSourceName);
    ClockSnapshot clockSnapshot() const noexcept;
    ClockSnapshot clockSnapshot(const QString& runtimeSourceName) const noexcept;

    // Creates portable-name virtual MIDI sources. CoreMIDI endpoint IDs remain
    // runtime details and are never returned to the project model.
    bool configureClockOutputs(const QStringList& logicalNames,
                               QString* error = nullptr);
    void sendClockStart(int outputIndex = -1);
    void sendClockStop(int outputIndex = -1);
    void sendClockTick(int outputIndex);
    // MIDI Song Position Pointer counts sixteenth notes from the start and is
    // bounded to the protocol's 14-bit range.
    void sendSongPosition(int outputIndex, std::uint16_t sixteenthNotes);

signals:
    void controlMessage(LiveMidiRouter::MessageKind kind,
                        int channel, int number, double normalizedValue,
                        const QString& runtimeSourceName);
    void clockActivityChanged();
    void endpointsChanged();
    void runtimeError(const QString& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
