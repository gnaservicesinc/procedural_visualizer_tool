#ifndef PVT_REMOTE_BRIDGE_H
#define PVT_REMOTE_BRIDGE_H
#include <QObject>
#include <QProcess>
#include <QJsonObject>
#include <QImage>
#include <QTimer>
#include <functional>
#include <optional>

// An optional transport worker. The desktop is the sole owner of project state,
// render contexts, devices and undo. Disabled networking never launches listeners.
class RemoteBridge final : public QObject {
    Q_OBJECT
public:
    explicit RemoteBridge(QObject* parent = nullptr);
    ~RemoteBridge() override;
    void start();
    void stop();
    void showManager(QWidget* parent);
    void sendFrame(const QImage& image);
    void sendAudio(const QByteArray& pcm);
    void setStateProvider(std::function<QJsonObject()> provider);
    void reply(const QString& token, const QJsonObject& result);
    bool enabled() const { return enabled_; }
    bool authorized(const QString& remote, const QString& action) const;
    void selectControl(int slot);
    QStringList controlNames() const;
    int activeControlSlot() const;
    bool background() const { return background_; }
    void setBackground(bool value) { background_ = value; }
    bool minimizeOnClose() const;

signals:
    void commandRequested(const QString& token, const QString& remote,
                          const QJsonObject& command);
    void statusChanged(const QString& status);
    void configurationChanged();
    void backgroundRequested(bool enabled);

private:
    void send(const QJsonObject& message);
    void receive();
    void configure(const QJsonObject& config, bool enabled);
    QProcess process_;
    QTimer state_timer_;
    QTimer frame_timer_;
    QImage pending_frame_;
    void flushFrame();
    QByteArray input_;
    QJsonObject config_;
    std::optional<QJsonObject> pending_config_;
    QJsonObject profile_;
    std::function<QJsonObject()> state_provider_;
    bool enabled_ = false;
    bool background_ = false;
    bool requested_enabled_ = false;
    bool ready_ = false;
    bool config_loaded_ = false;
};
#endif
