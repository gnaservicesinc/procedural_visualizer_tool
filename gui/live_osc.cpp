#include "live_osc.h"

#include <QByteArray>
#include <QHostAddress>
#include <QMetaObject>
#include <QNetworkDatagram>
#include <QStringDecoder>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

constexpr qsizetype kMaximumOscDatagramBytes = 65507;
constexpr int kMaximumBundleDepth = 4;
constexpr std::size_t kMaximumMessagesPerDatagram = 256U;
constexpr int kMaximumDatagramsPerEventTurn = 8;

struct ParsedOscValue {
    QString address;
    double value = 0.0;
};

bool checked_align4(qsizetype value, qsizetype& aligned) {
    if (value < 0 || value > (std::numeric_limits<qsizetype>::max)() - 3) {
        return false;
    }
    aligned = (value + 3) & ~qsizetype(3);
    return true;
}

bool read_osc_string(const QByteArray& bytes, qsizetype& offset,
                     QString& result) {
    if (offset < 0 || offset >= bytes.size()) return false;
    const qsizetype terminator = bytes.indexOf('\0', offset);
    if (terminator < offset) return false;
    const QByteArray encoded = bytes.mid(offset, terminator - offset);
    QStringDecoder decoder(QStringDecoder::Utf8);
    result = decoder.decode(encoded);
    if (decoder.hasError()) return false;
    qsizetype aligned = 0;
    if (!checked_align4(terminator + 1, aligned) || aligned > bytes.size()) {
        return false;
    }
    for (qsizetype index = terminator + 1; index < aligned; ++index) {
        if (bytes.at(index) != '\0') return false;
    }
    offset = aligned;
    return true;
}

template <typename Unsigned>
bool read_big_endian(const QByteArray& bytes, qsizetype& offset,
                     Unsigned& value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    if (offset < 0 || offset > bytes.size()
        || static_cast<std::size_t>(bytes.size() - offset) < sizeof(Unsigned)) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
        value = static_cast<Unsigned>(
            (value << 8U)
            | static_cast<unsigned char>(bytes.at(
                offset + static_cast<qsizetype>(index))));
    }
    offset += static_cast<qsizetype>(sizeof(Unsigned));
    return true;
}

bool parse_message(const QByteArray& bytes,
                   std::vector<ParsedOscValue>& values) {
    qsizetype offset = 0;
    QString address;
    QString tags;
    if (!read_osc_string(bytes, offset, address)
        || address.isEmpty() || !address.startsWith(QLatin1Char('/'))
        || address.size() > 1024
        || !read_osc_string(bytes, offset, tags)
        || tags.isEmpty() || tags.front() != QLatin1Char(',')) {
        return false;
    }
    bool emitted = false;
    for (qsizetype index = 1; index < tags.size(); ++index) {
        double numeric = 0.0;
        bool has_numeric = true;
        switch (tags.at(index).toLatin1()) {
            case 'f': {
                std::uint32_t bits = 0U;
                if (!read_big_endian(bytes, offset, bits)) return false;
                float value = 0.0F;
                std::memcpy(&value, &bits, sizeof(value));
                if (!std::isfinite(value)) return false;
                numeric = value;
                break;
            }
            case 'd': {
                std::uint64_t bits = 0U;
                if (!read_big_endian(bytes, offset, bits)) return false;
                std::memcpy(&numeric, &bits, sizeof(numeric));
                if (!std::isfinite(numeric)) return false;
                break;
            }
            case 'i': {
                std::uint32_t raw = 0U;
                if (!read_big_endian(bytes, offset, raw)) return false;
                std::int32_t signed_value = 0;
                std::memcpy(&signed_value, &raw, sizeof(signed_value));
                numeric = static_cast<double>(signed_value);
                break;
            }
            case 'h': {
                std::uint64_t raw = 0U;
                if (!read_big_endian(bytes, offset, raw)) return false;
                std::int64_t signed_value = 0;
                std::memcpy(&signed_value, &raw, sizeof(signed_value));
                numeric = static_cast<double>(signed_value);
                break;
            }
            case 'T': numeric = 1.0; break;
            case 'F': numeric = 0.0; break;
            case 'I': numeric = 1.0; break;
            case 'N': numeric = 0.0; break;
            // Strings/blobs are bounded and skipped so a later numeric
            // argument can still drive a control.
            case 's': {
                QString ignored;
                if (!read_osc_string(bytes, offset, ignored)) return false;
                has_numeric = false;
                break;
            }
            case 'b': {
                std::uint32_t size = 0U;
                if (!read_big_endian(bytes, offset, size)
                    || size > static_cast<std::uint32_t>(bytes.size() - offset)) {
                    return false;
                }
                qsizetype aligned = 0;
                if (!checked_align4(offset + static_cast<qsizetype>(size), aligned)
                    || aligned > bytes.size()) {
                    return false;
                }
                offset = aligned;
                has_numeric = false;
                break;
            }
            default:
                return false;
        }
        if (has_numeric && !emitted) {
            values.push_back({address, numeric});
            emitted = true;
            if (values.size() > kMaximumMessagesPerDatagram) return false;
        }
    }
    return offset == bytes.size();
}

bool parse_packet(const QByteArray& bytes, int depth,
                  std::vector<ParsedOscValue>& values) {
    if (bytes.isEmpty() || bytes.size() > kMaximumOscDatagramBytes
        || depth > kMaximumBundleDepth) {
        return false;
    }
    constexpr char kBundleMarker[] = "#bundle\0";
    if (bytes.size() < 8
        || std::memcmp(bytes.constData(), kBundleMarker, 8U) != 0) {
        return parse_message(bytes, values);
    }
    if (bytes.size() < 16) return false;
    qsizetype offset = 16; // marker plus timetag; scheduling stays sender-side.
    while (offset < bytes.size()) {
        std::uint32_t element_size = 0U;
        if (!read_big_endian(bytes, offset, element_size)
            || element_size == 0U
            || element_size > static_cast<std::uint32_t>(bytes.size() - offset)) {
            return false;
        }
        const QByteArray element = bytes.mid(
            offset, static_cast<qsizetype>(element_size));
        if (!parse_packet(element, depth + 1, values)) return false;
        offset += static_cast<qsizetype>(element_size);
    }
    return offset == bytes.size();
}

} // namespace

LiveOscRouter::LiveOscRouter(QObject* parent)
    : QObject(parent), socket_(new QUdpSocket(this)) {
    connect(socket_, &QUdpSocket::readyRead,
            this, &LiveOscRouter::readPendingDatagrams);
    connect(socket_, &QUdpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                emit runtimeError(socket_->errorString());
            });
}

LiveOscRouter::~LiveOscRouter() = default;

bool LiveOscRouter::listen(std::uint16_t requested_port, bool local_only,
                           QString* error) {
    if (error != nullptr) error->clear();
    stop();
    if (requested_port == 0U) {
        if (error != nullptr) *error = tr("OSC listen port must be between 1 and 65535.");
        return false;
    }
    const QHostAddress address = local_only ? QHostAddress::LocalHost
                                            : QHostAddress::AnyIPv4;
    if (!socket_->bind(address, requested_port,
                       QUdpSocket::DontShareAddress)) {
        if (error != nullptr) {
            *error = tr("Could not listen for OSC on UDP %1: %2")
                         .arg(requested_port).arg(socket_->errorString());
        }
        return false;
    }
    return true;
}

void LiveOscRouter::stop() {
    socket_->close();
}

bool LiveOscRouter::isListening() const {
    return socket_->state() == QAbstractSocket::BoundState;
}

std::uint16_t LiveOscRouter::port() const {
    return static_cast<std::uint16_t>(socket_->localPort());
}

std::uint64_t LiveOscRouter::malformedPacketCount() const noexcept {
    return malformed_packets_;
}

void LiveOscRouter::readPendingDatagrams() {
    int handled = 0;
    while (socket_->hasPendingDatagrams()
           && handled < kMaximumDatagramsPerEventTurn) {
        ++handled;
        const QNetworkDatagram datagram = socket_->receiveDatagram(
            kMaximumOscDatagramBytes);
        if (!datagram.isValid() || datagram.data().isEmpty()
            || datagram.data().size() > kMaximumOscDatagramBytes) {
            ++malformed_packets_;
            continue;
        }
        std::vector<ParsedOscValue> parsed;
        try {
            parsed.reserve(8U);
            if (!parse_packet(datagram.data(), 0, parsed)) {
                ++malformed_packets_;
                continue;
            }
        } catch (...) {
            ++malformed_packets_;
            continue;
        }
        const QString sender = QStringLiteral("%1:%2")
            .arg(datagram.senderAddress().toString())
            .arg(datagram.senderPort());
        for (const ParsedOscValue& value : parsed) {
            emit valueMessage(value.address, value.value, sender);
        }
    }
    if (socket_->hasPendingDatagrams()) {
        QMetaObject::invokeMethod(
            this, &LiveOscRouter::readPendingDatagrams,
            Qt::QueuedConnection);
    }
}
