#include "remote_firewall_support.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace pvt::remote_support {
namespace {

QString joinedPorts(const QList<quint16>& ports) {
    QStringList values;
    values.reserve(ports.size());
    for (const quint16 port : ports) values.append(QString::number(port));
    return values.join(QStringLiteral(", "));
}

QString allowedPlaces(const QJsonObject& config) {
    const QString scope = config.value(QStringLiteral("address_scope"))
                              .toString(QStringLiteral("subnet"));
    if (scope == QStringLiteral("private")) {
        return QStringLiteral(
            "Private address spaces: 10.0.0.0/8, 172.16.0.0/12, "
            "192.168.0.0/16, fc00::/7, and fe80::/10");
    }
    if (scope == QStringLiteral("any")) {
        return QStringLiteral(
            "Any source address. PVT still requires an explicitly paired remote.");
    }
    if (scope == QStringLiteral("custom")) {
        QString ranges = config.value(QStringLiteral("custom_networks")).toString();
        ranges.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
        return QStringLiteral("Only these addresses or ranges: %1").arg(ranges);
    }
    return QStringLiteral("This computer and networks directly attached to it.");
}

QString windowsRemoteAddresses(const QJsonObject& config) {
    const QString scope = config.value(QStringLiteral("address_scope"))
                              .toString(QStringLiteral("subnet"));
    if (scope == QStringLiteral("subnet")) return QStringLiteral("LocalSubnet");
    if (scope == QStringLiteral("private")) {
        return QStringLiteral(
            "10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,fc00::/7,fe80::/10");
    }
    if (scope == QStringLiteral("custom")) {
        QString result = config.value(QStringLiteral("custom_networks")).toString();
        result.replace(QRegularExpression(QStringLiteral("[\\s,]+")), QStringLiteral(","));
        if (!result.isEmpty()) return result;
    }
    return QStringLiteral("any");
}

QString shellQuote(const QString& value) {
    QString result = value;
    result.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + result + QLatin1Char('\'');
}

QString cmdQuote(const QString& value) {
    QString result = value;
    result.replace(QLatin1Char('%'), QStringLiteral("%%"));
    result.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + result + QLatin1Char('"');
}

}  // namespace

QList<quint16> connectionPorts(const QJsonObject& config,
                               const QJsonObject& public_profile) {
    QList<quint16> result;
    const int configured = config.value(QStringLiteral("port")).toInt();
    if (configured >= 1024 && configured <= 65535) {
        result.append(static_cast<quint16>(configured));
    }

    QString identity = public_profile.value(QStringLiteral("id")).toString();
    identity.remove(QLatin1Char('-'));
    bool ok = false;
    const quint32 prefix = identity.left(8).toUInt(&ok, 16);
    if (ok) {
        const int first = 49152 + static_cast<int>(prefix % 16000U);
        for (const int offset : {0, 4093, 8191, 12289}) {
            const quint16 port = static_cast<quint16>(
                49152 + (first - 49152 + offset) % 16000);
            if (!result.contains(port)) result.append(port);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

QString networkAdminInstructions(const QJsonObject& config,
                                 const QJsonObject& public_profile) {
    const QList<quint16> ports = connectionPorts(config, public_profile);
    QStringList host_addresses;
    for (const auto& value : public_profile.value(QStringLiteral("endpoints")).toArray()) {
        const QUrl endpoint(value.toString());
        if (!endpoint.host().isEmpty() && endpoint.host() != QStringLiteral("127.0.0.1")
            && endpoint.host() != QStringLiteral("::1")) {
            host_addresses.append(endpoint.host());
        }
    }
    host_addresses.removeDuplicates();

    return QStringLiteral(
        "PVT Remotes - network administrator instructions\n"
        "================================================\n\n"
        "PVT computer addresses: %1\n"
        "Remote sources allowed in PVT: %2\n\n"
        "Required routed-firewall policy\n"
        "--------------------------------\n"
        "1. Allow TCP connections from the allowed remote sources to the PVT "
        "computer on ports %3. PVT tries these stable ports automatically and "
        "uses the first available one.\n"
        "2. Allow UDP traffic in both directions between the PVT computer and "
        "the allowed remote sources, including established or related reply "
        "traffic. The browser and PVT select the live-stream UDP ports "
        "automatically.\n"
        "3. Multicast DNS on UDP 5353 is optional and usually does not cross "
        "routed networks. The imported PVT pairing file already includes direct "
        "addresses, so discovery is not required. Re-export the pairing file if "
        "the PVT computer's address changes.\n\n"
        "Local firewall\n"
        "--------------\n"
        "On Windows, allow the bundled pvt-remote program to receive inbound "
        "connections from the selected sources. On macOS, allow the bundled "
        "pvt-remote program in Firewall Options. On Ubuntu with UFW, allow TCP "
        "to the ports listed above; normal state tracking permits replies to "
        "PVT's outgoing live-stream traffic. PVT can export a local setup file "
        "for the operating system on which it is running.\n\n"
        "Routing and internet boundary\n"
        "-----------------------------\n"
        "The two networks must already have a route between them. If they are "
        "separated by address translation, use a trusted VPN that routes the "
        "two networks. Forwarding only a TCP port is not sufficient for the "
        "direct live stream. Do not expose PVT Remotes directly to the public "
        "internet. PVT accepts only explicitly paired remotes and also enforces "
        "the address selection shown above.\n")
        .arg(host_addresses.isEmpty() ? QStringLiteral("not available; use the address of this computer")
                                      : host_addresses.join(QStringLiteral(", ")),
             allowedPlaces(config),
             ports.isEmpty() ? QStringLiteral("not available until PVT Remotes is ready")
                             : joinedPorts(ports));
}

FirewallSetupFile localFirewallSetup(FirewallPlatform platform,
                                     const QJsonObject& config,
                                     const QJsonObject& public_profile,
                                     const QString& worker_path) {
    const QList<quint16> ports = connectionPorts(config, public_profile);
    const QString port_text = joinedPorts(ports);
    const QString policy = allowedPlaces(config);

    if (platform == FirewallPlatform::Windows) {
        const QString rule_name = QStringLiteral("PVT Remotes (generated by PVT)");
        const QString contents = QStringLiteral(
            "@echo off\r\n"
            "setlocal\r\n"
            "net session >nul 2>&1\r\n"
            "if not %errorlevel%==0 (\r\n"
            "  powershell.exe -NoProfile -Command \"Start-Process -FilePath '%~f0' -Verb RunAs\"\r\n"
            "  exit /b\r\n"
            ")\r\n"
            "echo Setting up Windows Firewall for PVT Remotes...\r\n"
            "netsh advfirewall firewall delete rule name=%1 >nul 2>&1\r\n"
            "netsh advfirewall firewall add rule name=%1 dir=in action=allow enable=yes profile=any program=%2 remoteip=%3\r\n"
            "if errorlevel 1 (\r\n"
            "  echo Windows could not add the firewall rule. Give this file to your administrator.\r\n"
            ") else (\r\n"
            "  echo PVT Remotes is allowed through this computer's firewall.\r\n"
            ")\r\n"
            "echo PVT connection ports for routers or routed firewalls: %4\r\n"
            "pause\r\n")
            .arg(cmdQuote(rule_name), cmdQuote(worker_path),
                 windowsRemoteAddresses(config), port_text);
        return {QStringLiteral("Set-up PVT Remotes firewall.cmd"),
                QStringLiteral("Windows setup (*.cmd)"), contents, false};
    }

    if (platform == FirewallPlatform::MacOS) {
        const QString contents = QStringLiteral(
            "#!/bin/sh\n"
            "set -eu\n"
            "worker=%1\n"
            "firewall=/usr/libexec/ApplicationFirewall/socketfilterfw\n"
            "if [ \"$(id -u)\" -ne 0 ]; then\n"
            "  exec sudo \"$0\" \"$@\"\n"
            "fi\n"
            "if [ ! -x \"$worker\" ] || [ ! -x \"$firewall\" ]; then\n"
            "  echo \"PVT Remotes or the macOS firewall tool was not found.\" >&2\n"
            "  exit 1\n"
            "fi\n"
            "\"$firewall\" --add \"$worker\" >/dev/null 2>&1 || true\n"
            "\"$firewall\" --unblockapp \"$worker\"\n"
            "echo \"PVT Remotes is allowed through this Mac's firewall.\"\n"
            "echo \"Connection ports for routers or routed firewalls: %2\"\n")
            .arg(shellQuote(worker_path), port_text);
        return {QStringLiteral("Set up PVT Remotes firewall.command"),
                QStringLiteral("macOS setup (*.command)"), contents, true};
    }

    QString port_commands;
    for (const quint16 port : ports) {
        port_commands += QStringLiteral(
            "ufw allow proto tcp from any to any port %1 comment 'PVT Remotes'\n")
                             .arg(port);
    }
    const QString contents = QStringLiteral(
        "#!/bin/sh\n"
        "set -eu\n"
        "if [ \"$(id -u)\" -ne 0 ]; then\n"
        "  if command -v pkexec >/dev/null 2>&1; then\n"
        "    exec pkexec \"$0\" \"$@\"\n"
        "  fi\n"
        "  exec sudo \"$0\" \"$@\"\n"
        "fi\n"
        "if ! command -v ufw >/dev/null 2>&1; then\n"
        "  echo \"Ubuntu's UFW firewall tool is not installed. Give this file to your administrator.\" >&2\n"
        "  exit 1\n"
        "fi\n"
        "# PVT itself continues to enforce this source policy:\n"
        "# %1\n"
        "%2"
        "echo \"PVT Remotes is allowed through this computer's UFW firewall.\"\n"
        "echo \"Connection ports for routers or routed firewalls: %3\"\n")
        .arg(policy, port_commands, port_text);
    return {QStringLiteral("set-up-pvt-remotes-firewall.sh"),
            QStringLiteral("Ubuntu setup (*.sh)"), contents, true};
}

}  // namespace pvt::remote_support
