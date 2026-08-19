#ifndef PVT_LIVE_OSC_H
#define PVT_LIVE_OSC_H

#include <QObject>
#include <QString>

#include <cstdint>

class QUdpSocket;

class LiveOscRouter final : public QObject {
    Q_OBJECT

public:
    explicit LiveOscRouter(QObject* parent = nullptr);
    ~LiveOscRouter() override;

    bool listen(std::uint16_t port, bool localOnly, QString* error = nullptr);
    void stop();
    bool isListening() const;
    std::uint16_t port() const;
    std::uint64_t malformedPacketCount() const noexcept;

signals:
    // Values remain raw OSC numbers. Each portable mapping owns its input and
    // output range, so a sender can use either normalized controls or physical
    // units without changing the listener.
    void valueMessage(const QString& address, double value,
                      const QString& runtimeSender);
    void runtimeError(const QString& message);

private:
    void readPendingDatagrams();

    QUdpSocket* socket_ = nullptr;
    std::uint64_t malformed_packets_ = 0U;
};

#endif
