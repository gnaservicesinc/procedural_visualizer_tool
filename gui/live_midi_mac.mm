#include "live_midi.h"

#include <CoreMIDI/CoreMIDI.h>

#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

std::int64_t monotonic_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QString os_status_message(const QString& action, OSStatus status) {
    return QStringLiteral("%1 (CoreMIDI error %2)").arg(action).arg(status);
}

QString midi_object_name(MIDIObjectRef object) {
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(object, kMIDIPropertyDisplayName, &name)
            != noErr
        || name == nullptr) {
        return QObject::tr("Unnamed MIDI endpoint");
    }
    const CFIndex length = CFStringGetLength(name);
    const CFIndex maximum = CFStringGetMaximumSizeForEncoding(
        length, kCFStringEncodingUTF8) + 1;
    QByteArray bytes(static_cast<qsizetype>(maximum), Qt::Uninitialized);
    const bool converted = CFStringGetCString(
        name, bytes.data(), maximum, kCFStringEncodingUTF8);
    CFRelease(name);
    return converted ? QString::fromUtf8(bytes.constData())
                     : QObject::tr("Unnamed MIDI endpoint");
}

CFStringRef retained_cf_string(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    return CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(utf8.constData()),
        static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
}

} // namespace

struct LiveMidiRouter::Impl {
    struct ClockState {
        std::atomic_bool running{false};
        std::atomic<std::uint64_t> ticks{0U};
        std::atomic<double> bpm{0.0};
        std::atomic<std::int64_t> last_clock_ns{0};
    };

    struct InputSource {
        Impl* owner = nullptr;
        MIDIEndpointRef endpoint = 0;
        QString name;
        std::atomic_bool accepts_clock{true};
        ClockState clock;
        std::uint8_t running_status = 0U;
        std::array<std::uint8_t, 2U> data{};
        int needed = 0;
        int have = 0;
        std::uint8_t system_status = 0U;
        std::array<std::uint8_t, 2U> system_data{};
        int system_needed = 0;
        int system_have = 0;
    };

    QPointer<LiveMidiRouter> facade;
    MIDIClientRef client = 0;
    MIDIPortRef input_port = 0;
    std::vector<std::unique_ptr<InputSource>> inputs;
    std::vector<MIDIEndpointRef> clock_outputs;
    std::atomic_bool active{false};
    std::atomic_bool clock_notification_pending{false};
    ClockState aggregate_clock;

    static void reset_clock(ClockState& clock) noexcept {
        clock.running.store(false, std::memory_order_relaxed);
        clock.ticks.store(0U, std::memory_order_relaxed);
        clock.bpm.store(0.0, std::memory_order_relaxed);
        clock.last_clock_ns.store(0, std::memory_order_relaxed);
    }

    static bool consume_realtime(ClockState& clock, std::uint8_t byte,
                                 std::int64_t now) noexcept {
        if (byte == 0xF8U) {
            const std::int64_t previous = clock.last_clock_ns.exchange(
                now, std::memory_order_relaxed);
            clock.ticks.fetch_add(1U, std::memory_order_relaxed);
            if (previous != 0 && now > previous) {
                const double candidate = 60.0e9
                    / (24.0 * static_cast<double>(now - previous));
                if (candidate >= 20.0 && candidate <= 400.0) {
                    const double old = clock.bpm.load(std::memory_order_relaxed);
                    clock.bpm.store(old > 0.0
                                        ? 0.86 * old + 0.14 * candidate
                                        : candidate,
                                    std::memory_order_relaxed);
                }
            }
            return true;
        }
        if (byte == 0xFAU) {
            clock.ticks.store(0U, std::memory_order_relaxed);
            clock.running.store(true, std::memory_order_relaxed);
            clock.last_clock_ns.store(now, std::memory_order_relaxed);
            return true;
        }
        if (byte == 0xFBU) {
            clock.running.store(true, std::memory_order_relaxed);
            clock.last_clock_ns.store(now, std::memory_order_relaxed);
            return true;
        }
        if (byte == 0xFCU) {
            clock.running.store(false, std::memory_order_relaxed);
            return true;
        }
        if (byte == 0xFFU) {
            reset_clock(clock);
            return true;
        }
        return false;
    }

    static void set_song_position(ClockState& clock,
                                  std::uint16_t sixteenth_notes,
                                  std::int64_t now) noexcept {
        clock.ticks.store(static_cast<std::uint64_t>(sixteenth_notes) * 6U,
                          std::memory_order_relaxed);
        clock.last_clock_ns.store(now, std::memory_order_relaxed);
    }

    static ClockSnapshot snapshot(const ClockState& clock, bool router_active,
                                  std::int64_t now) noexcept {
        ClockSnapshot value;
        value.ticks = clock.ticks.load(std::memory_order_relaxed);
        value.bpm = clock.bpm.load(std::memory_order_relaxed);
        value.running = clock.running.load(std::memory_order_relaxed);
        const std::int64_t last = clock.last_clock_ns.load(
            std::memory_order_relaxed);
        value.receiving = router_active && last != 0 && now >= last
                          && now - last < INT64_C(750000000);
        double fractional_tick = 0.0;
        if (value.receiving && value.bpm > 0.0) {
            const double tick_seconds = 60.0 / (24.0 * value.bpm);
            fractional_tick = std::clamp(
                static_cast<double>(now - last) / 1.0e9 / tick_seconds,
                0.0, 1.0);
        }
        value.quarter_note_phase = std::fmod(
            (static_cast<double>(value.ticks % 24U) + fractional_tick) / 24.0,
            1.0);
        return value;
    }

    static void notify_proc(const MIDINotification*, void* refcon) {
        auto* self = static_cast<Impl*>(refcon);
        if (self == nullptr || self->facade.isNull()) return;
        QMetaObject::invokeMethod(
            self->facade, [owner = self->facade] {
                if (owner != nullptr) emit owner->endpointsChanged();
            }, Qt::QueuedConnection);
    }

    static void read_proc(const MIDIPacketList* packets, void* port_refcon,
                          void* source_refcon) {
        auto* self = static_cast<Impl*>(port_refcon);
        auto* source = static_cast<InputSource*>(source_refcon);
        if (self == nullptr || source == nullptr || packets == nullptr
            || !self->active.load(std::memory_order_relaxed)) {
            return;
        }
        const MIDIPacket* packet = &packets->packet[0];
        for (UInt32 packet_index = 0U; packet_index < packets->numPackets;
             ++packet_index) {
            for (UInt16 byte_index = 0U; byte_index < packet->length;
                 ++byte_index) {
                self->consume_byte(*source, packet->data[byte_index]);
            }
            packet = MIDIPacketNext(packet);
        }
    }

    void post_error(const QString& message) {
        if (facade.isNull()) return;
        QMetaObject::invokeMethod(
            facade, [owner = facade, message] {
                if (owner != nullptr) emit owner->runtimeError(message);
            }, Qt::QueuedConnection);
    }

    void post_clock_activity() {
        if (facade.isNull()) return;
        if (clock_notification_pending.exchange(true,
                                                std::memory_order_relaxed)) {
            return;
        }
        const bool queued = QMetaObject::invokeMethod(
            facade, [owner = facade, self = this] {
                self->clock_notification_pending.store(
                    false, std::memory_order_relaxed);
                if (owner != nullptr) emit owner->clockActivityChanged();
            }, Qt::QueuedConnection);
        if (!queued) {
            clock_notification_pending.store(false,
                                             std::memory_order_relaxed);
        }
    }

    void post_control(MessageKind kind, int channel, int number,
                      double value, const QString& source_name) {
        if (facade.isNull()) return;
        QMetaObject::invokeMethod(
            facade,
            [owner = facade, kind, channel, number, value, source_name] {
                if (owner != nullptr) {
                    emit owner->controlMessage(kind, channel, number,
                                               value, source_name);
                }
            }, Qt::QueuedConnection);
    }

    void consume_byte(InputSource& source, std::uint8_t byte) {
        // System real-time bytes may occur between channel-message data bytes.
        if (byte >= 0xF8U) {
            const std::int64_t now = monotonic_nanoseconds();
            const bool activity = consume_realtime(source.clock, byte, now);
            if (source.accepts_clock.load(std::memory_order_relaxed)) {
                (void)consume_realtime(aggregate_clock, byte, now);
            }
            if (activity) post_clock_activity();
            return;
        }
        if ((byte & 0x80U) != 0U) {
            if (byte >= 0xF0U) {
                source.running_status = 0U;
                source.have = 0;
                source.needed = 0;
                source.system_status = byte;
                source.system_have = 0;
                source.system_needed = byte == 0xF2U ? 2
                    : (byte == 0xF1U || byte == 0xF3U ? 1 : 0);
                return;
            }
            source.system_status = 0U;
            source.system_have = 0;
            source.system_needed = 0;
            source.running_status = byte;
            source.have = 0;
            const std::uint8_t command = byte & 0xF0U;
            source.needed = command == 0xC0U || command == 0xD0U ? 1 : 2;
            return;
        }
        if (source.system_status != 0U && source.system_needed > 0) {
            source.system_data[static_cast<std::size_t>(source.system_have++)] =
                byte & 0x7FU;
            if (source.system_have < source.system_needed) return;
            if (source.system_status == 0xF2U) {
                const std::uint16_t position = static_cast<std::uint16_t>(
                    source.system_data[0]
                    | (static_cast<std::uint16_t>(source.system_data[1]) << 7U));
                const std::int64_t now = monotonic_nanoseconds();
                set_song_position(source.clock, position, now);
                if (source.accepts_clock.load(std::memory_order_relaxed)) {
                    set_song_position(aggregate_clock, position, now);
                }
                post_clock_activity();
            }
            source.system_status = 0U;
            source.system_have = 0;
            source.system_needed = 0;
            return;
        }
        if (source.running_status == 0U || source.needed == 0) return;
        source.data[static_cast<std::size_t>(source.have++)] = byte & 0x7FU;
        if (source.have < source.needed) return;
        source.have = 0;
        const int channel = static_cast<int>(source.running_status & 0x0FU) + 1;
        const std::uint8_t command = source.running_status & 0xF0U;
        if (command == 0x80U || command == 0x90U) {
            const bool note_on = command == 0x90U && source.data[1] != 0U;
            post_control(MessageKind::Note, channel, source.data[0],
                         note_on ? static_cast<double>(source.data[1]) / 127.0
                                 : 0.0,
                         source.name);
        } else if (command == 0xB0U) {
            post_control(MessageKind::ControlChange, channel, source.data[0],
                         static_cast<double>(source.data[1]) / 127.0,
                         source.name);
        } else if (command == 0xC0U) {
            post_control(MessageKind::ProgramChange, channel, source.data[0],
                         static_cast<double>(source.data[0]) / 127.0,
                         source.name);
        } else if (command == 0xD0U) {
            post_control(MessageKind::ChannelPressure, channel, 0,
                         static_cast<double>(source.data[0]) / 127.0,
                         source.name);
        } else if (command == 0xE0U) {
            const int value = static_cast<int>(source.data[0])
                              | (static_cast<int>(source.data[1]) << 7);
            post_control(MessageKind::PitchBend, channel, 0,
                         static_cast<double>(value) / 16383.0,
                         source.name);
        }
    }

    void clear_clock_outputs() noexcept {
        for (MIDIEndpointRef endpoint : clock_outputs) {
            if (endpoint != 0) (void)MIDIEndpointDispose(endpoint);
        }
        clock_outputs.clear();
    }

    void uninitialize() noexcept {
        active.store(false, std::memory_order_release);
        clear_clock_outputs();
        if (input_port != 0) {
            for (const auto& source : inputs) {
                if (source->endpoint != 0) {
                    (void)MIDIPortDisconnectSource(input_port,
                                                   source->endpoint);
                }
            }
            (void)MIDIPortDispose(input_port);
            input_port = 0;
        }
        inputs.clear();
        if (client != 0) {
            (void)MIDIClientDispose(client);
            client = 0;
        }
        aggregate_clock.running.store(false, std::memory_order_relaxed);
    }

    ~Impl() { uninitialize(); }
};

LiveMidiRouter::LiveMidiRouter(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
    impl_->facade = this;
}

LiveMidiRouter::~LiveMidiRouter() = default;

bool LiveMidiRouter::start(QString* error) {
    if (error != nullptr) error->clear();
    stop();
    CFStringRef client_name = retained_cf_string(
        tr("Procedural Visualizer Tool Live"));
    if (client_name == nullptr) {
        if (error != nullptr) *error = tr("Could not allocate the MIDI client name.");
        return false;
    }
    const OSStatus client_status = MIDIClientCreate(
        client_name, &Impl::notify_proc, impl_.get(), &impl_->client);
    CFRelease(client_name);
    if (client_status != noErr) {
        if (error != nullptr) {
            *error = os_status_message(tr("Could not create the MIDI client"),
                                       client_status);
        }
        return false;
    }
    CFStringRef port_name = retained_cf_string(tr("Live control input"));
    const OSStatus port_status = port_name == nullptr
        ? static_cast<OSStatus>(-1)
        : MIDIInputPortCreate(impl_->client, port_name, &Impl::read_proc,
                              impl_.get(), &impl_->input_port);
    if (port_name != nullptr) CFRelease(port_name);
    if (port_status != noErr) {
        if (error != nullptr) {
            *error = os_status_message(tr("Could not create the MIDI input"),
                                       port_status);
        }
        impl_->uninitialize();
        return false;
    }

    const ItemCount count = MIDIGetNumberOfSources();
    try {
        impl_->inputs.reserve(static_cast<std::size_t>(count));
        for (ItemCount index = 0; index < count; ++index) {
            MIDIEndpointRef endpoint = MIDIGetSource(index);
            if (endpoint == 0) continue;
            auto source = std::make_unique<Impl::InputSource>();
            source->owner = impl_.get();
            source->endpoint = endpoint;
            source->name = midi_object_name(endpoint);
            const OSStatus status = MIDIPortConnectSource(
                impl_->input_port, endpoint, source.get());
            if (status == noErr) {
                impl_->inputs.push_back(std::move(source));
            }
        }
    } catch (...) {
        if (error != nullptr) *error = tr("Not enough memory to connect MIDI inputs.");
        impl_->uninitialize();
        return false;
    }
    impl_->active.store(true, std::memory_order_release);
    emit endpointsChanged();
    return true;
}

void LiveMidiRouter::stop() noexcept {
    impl_->uninitialize();
}

bool LiveMidiRouter::isRunning() const noexcept {
    return impl_->active.load(std::memory_order_acquire);
}

QStringList LiveMidiRouter::inputNames() const {
    QStringList names;
    for (const auto& source : impl_->inputs) names.push_back(source->name);
    return names;
}

bool LiveMidiRouter::selectClockInput(const QString& runtime_source_name) {
    const QString selected = runtime_source_name.trimmed();
    if (!selected.isEmpty()) {
        const auto found = std::find_if(
            impl_->inputs.begin(), impl_->inputs.end(),
            [&selected](const auto& source) { return source->name == selected; });
        if (found == impl_->inputs.end()) return false;
    }
    for (const auto& source : impl_->inputs) {
        source->accepts_clock.store(
            selected.isEmpty() || source->name == selected,
            std::memory_order_relaxed);
    }
    Impl::reset_clock(impl_->aggregate_clock);
    emit clockActivityChanged();
    return true;
}

LiveMidiRouter::ClockSnapshot LiveMidiRouter::clockSnapshot() const noexcept {
    const std::int64_t now = monotonic_nanoseconds();
    return Impl::snapshot(impl_->aggregate_clock, isRunning(), now);
}

LiveMidiRouter::ClockSnapshot LiveMidiRouter::clockSnapshot(
    const QString& runtime_source_name) const noexcept {
    if (runtime_source_name.isEmpty()) return clockSnapshot();
    const Impl::InputSource* selected = nullptr;
    std::int64_t newest = (std::numeric_limits<std::int64_t>::min)();
    for (const auto& source : impl_->inputs) {
        if (source->name != runtime_source_name) continue;
        const std::int64_t last = source->clock.last_clock_ns.load(
            std::memory_order_relaxed);
        if (selected == nullptr || last > newest) {
            selected = source.get();
            newest = last;
        }
    }
    if (selected == nullptr) return {};
    return Impl::snapshot(selected->clock, isRunning(),
                          monotonic_nanoseconds());
}

bool LiveMidiRouter::configureClockOutputs(const QStringList& logical_names,
                                           QString* error) {
    if (error != nullptr) error->clear();
    impl_->clear_clock_outputs();
    if (impl_->client == 0 && !logical_names.isEmpty()) {
        if (error != nullptr) *error = tr("Start Live MIDI before enabling clock outputs.");
        return false;
    }
    try {
        impl_->clock_outputs.reserve(
            static_cast<std::size_t>(logical_names.size()));
        for (const QString& requested : logical_names) {
            const QString name = requested.trimmed().isEmpty()
                                     ? tr("PVT Clock Out") : requested.trimmed();
            CFStringRef cf_name = retained_cf_string(name);
            MIDIEndpointRef endpoint = 0;
            const OSStatus status = cf_name == nullptr
                ? static_cast<OSStatus>(-1)
                : MIDISourceCreate(impl_->client, cf_name, &endpoint);
            if (cf_name != nullptr) CFRelease(cf_name);
            if (status != noErr || endpoint == 0) {
                impl_->clear_clock_outputs();
                if (error != nullptr) {
                    *error = os_status_message(
                        tr("Could not create MIDI clock output %1").arg(name),
                        status);
                }
                return false;
            }
            impl_->clock_outputs.push_back(endpoint);
        }
    } catch (...) {
        impl_->clear_clock_outputs();
        if (error != nullptr) *error = tr("Not enough memory for MIDI clock outputs.");
        return false;
    }
    return true;
}

namespace {

void send_midi_bytes(const std::vector<MIDIEndpointRef>& outputs,
                     int output_index, const std::uint8_t* bytes,
                     ByteCount size) {
    alignas(MIDIPacketList) Byte storage[sizeof(MIDIPacketList) + 16U]{};
    auto* packets = reinterpret_cast<MIDIPacketList*>(storage);
    MIDIPacket* packet = MIDIPacketListInit(packets);
    packet = MIDIPacketListAdd(packets, sizeof(storage), packet, 0, size, bytes);
    if (packet == nullptr) return;
    if (output_index >= 0
        && static_cast<std::size_t>(output_index) < outputs.size()) {
        (void)MIDIReceived(outputs[static_cast<std::size_t>(output_index)],
                           packets);
        return;
    }
    for (MIDIEndpointRef endpoint : outputs) {
        (void)MIDIReceived(endpoint, packets);
    }
}

void send_realtime_byte(const std::vector<MIDIEndpointRef>& outputs,
                        int output_index, std::uint8_t byte) {
    send_midi_bytes(outputs, output_index, &byte, 1U);
}

} // namespace

void LiveMidiRouter::sendClockStart(int output_index) {
    send_realtime_byte(impl_->clock_outputs, output_index, 0xFAU);
}

void LiveMidiRouter::sendClockStop(int output_index) {
    send_realtime_byte(impl_->clock_outputs, output_index, 0xFCU);
}

void LiveMidiRouter::sendClockTick(int output_index) {
    send_realtime_byte(impl_->clock_outputs, output_index, 0xF8U);
}

void LiveMidiRouter::sendSongPosition(int output_index,
                                      std::uint16_t sixteenth_notes) {
    const std::uint16_t position = std::min<std::uint16_t>(
        sixteenth_notes, UINT16_C(16383));
    const std::array<std::uint8_t, 3U> message{
        0xF2U,
        static_cast<std::uint8_t>(position & 0x7FU),
        static_cast<std::uint8_t>((position >> 7U) & 0x7FU)};
    send_midi_bytes(impl_->clock_outputs, output_index, message.data(),
                    static_cast<ByteCount>(message.size()));
}
