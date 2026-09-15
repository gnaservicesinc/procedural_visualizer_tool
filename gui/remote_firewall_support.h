#ifndef PVT_REMOTE_FIREWALL_SUPPORT_H
#define PVT_REMOTE_FIREWALL_SUPPORT_H

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace pvt::remote_support {

enum class FirewallPlatform {
    MacOS,
    Windows,
    Ubuntu,
};

struct FirewallSetupFile {
    QString suggested_name;
    QString dialog_filter;
    QString contents;
    bool executable = false;
};

QList<quint16> connectionPorts(const QJsonObject& config,
                               const QJsonObject& public_profile);
QString networkAdminInstructions(const QJsonObject& config,
                                 const QJsonObject& public_profile);
FirewallSetupFile localFirewallSetup(FirewallPlatform platform,
                                     const QJsonObject& config,
                                     const QJsonObject& public_profile,
                                     const QString& worker_path);

}  // namespace pvt::remote_support

#endif
