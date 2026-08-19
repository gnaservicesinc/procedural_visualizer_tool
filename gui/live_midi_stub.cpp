#include "live_midi.h"

#include <QTimer>

struct LiveMidiRouter::Impl {
    bool active = false;
};

LiveMidiRouter::LiveMidiRouter(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {}
LiveMidiRouter::~LiveMidiRouter() = default;

bool LiveMidiRouter::start(QString* error) {
    if (error != nullptr) {
        *error = tr("Native MIDI is not available in this build. OSC mappings remain available.");
    }
    impl_->active = false;
    return false;
}
void LiveMidiRouter::stop() noexcept { impl_->active = false; }
bool LiveMidiRouter::isRunning() const noexcept { return impl_->active; }
QStringList LiveMidiRouter::inputNames() const { return {}; }
bool LiveMidiRouter::selectClockInput(const QString& runtimeSourceName) {
    return runtimeSourceName.trimmed().isEmpty();
}
LiveMidiRouter::ClockSnapshot LiveMidiRouter::clockSnapshot() const noexcept {
    return {};
}
LiveMidiRouter::ClockSnapshot LiveMidiRouter::clockSnapshot(
    const QString&) const noexcept {
    return {};
}
bool LiveMidiRouter::configureClockOutputs(const QStringList&, QString* error) {
    if (error != nullptr) *error = tr("Native MIDI clock output is unavailable in this build.");
    return false;
}
void LiveMidiRouter::sendClockStart(int) {}
void LiveMidiRouter::sendClockStop(int) {}
void LiveMidiRouter::sendClockTick(int) {}
void LiveMidiRouter::sendSongPosition(int, std::uint16_t) {}
