// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bloodborne_backend_server.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QRegularExpression>

using namespace Bloodborne::Backend;

namespace {

QByteArray MethodName(QHttpServerRequest::Method method) {
    switch (method) {
    case QHttpServerRequest::Method::Get:
        return "GET";
    case QHttpServerRequest::Method::Post:
        return "POST";
    case QHttpServerRequest::Method::Put:
        return "PUT";
    case QHttpServerRequest::Method::Delete:
        return "DELETE";
    case QHttpServerRequest::Method::Head:
        return "HEAD";
    default:
        return "OTHER";
    }
}

QString InFolder(const QString& folder, const QString& name) {
    return QDir(folder).filePath(name);
}

// The address a game reached this server at, from its Host header ("1.2.3.4:20443"), as
// bb_dummy_server.py's address_from_host_header: a guest over the internet reaches the host's
// public address while the host's own is a LAN one no guest can reach. Empty when the header is
// missing, not a plain name or IPv4 address, or loopback.
QString AddressFromHostHeader(const QByteArray& value) {
    QString host = QString::fromLatin1(value).trimmed();
    if (host.count(QLatin1Char(':')) == 1) {
        const qsizetype colon = host.indexOf(QLatin1Char(':'));
        const QString port = host.mid(colon + 1);
        static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
        if (!digits.match(port).hasMatch()) {
            return {};
        }
        host.truncate(colon);
    }
    static const QRegularExpression name(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$"));
    static const QRegularExpression loopback(QStringLiteral("^127(\\.[0-9]{1,3}){3}$"));
    if (!name.match(host).hasMatch() || host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 ||
        loopback.match(host).hasMatch()) {
        return {};
    }
    return host;
}

} // namespace

BloodborneBackendServer::BloodborneBackendServer(QObject* parent) : QObject(parent) {}
BloodborneBackendServer::~BloodborneBackendServer() = default;

bool BloodborneBackendServer::Start(ConfigManager* config) {
    m_config = config;
    const BackendConfig settings = config->GetBackend();
    m_port = settings.port;
    m_webApiPort = config->GetWebApiPort();
    // The host's address (the kit's HOSTIP), for a request whose Host header names none; else the
    // address the connection came in on. A Host header wins over both (AddressFor).
    m_fixedAddress = qEnvironmentVariable("SHADNET_BACKEND_ADDRESS", settings.address).trimmed();

    BackendOptions options;
    QFile templ(QStringLiteral(":/backend/ss_info_template.xml"));
    if (!templ.open(QIODevice::ReadOnly)) {
        qCritical() << "Backend: the ss.info template is missing from this build";
        return false;
    }
    options.ssInfoTemplate = templ.readAll();
    options.storePath = settings.store;
    const auto showOwn = ParseShowOwn(settings.showOwn);
    if (!showOwn) {
        qCritical().noquote() << "Backend: BackendShowOwn: unknown value in" << settings.showOwn
                              << "; choose from" << ShowOwnChoices.join(QStringLiteral(", "))
                              << "or all";
        return false;
    }
    options.showOwn = *showOwn;
    options.ghostTtlSeconds = qint64{settings.ghostTtlMinutes} * 60;
    options.lanternTtlSeconds = qint64{settings.lanternTtlMinutes} * 60;
    options.tombCap = settings.tombCap;
    options.summonPoll = settings.summonPoll;
    options.noticePath = InFolder(settings.folder, QStringLiteral("notice.txt"));
    options.broadcastPath = InFolder(settings.folder, QStringLiteral("broadcast.txt"));
    options.loginMessagePath = InFolder(settings.folder, QStringLiteral("login_message.txt"));

    // Everything that can fail is checked before the port is taken.
    QString error;
    if (!m_backend.Open(options, &error)) {
        qCritical().noquote() << "Backend:" << error;
        return false;
    }

    m_http = std::make_unique<QHttpServer>(this);
    // Every path and method goes to the backend, as in the Python server.
    m_http->setMissingHandler(this, [this](const QHttpServerRequest& request,
                                           QHttpServerResponder& responder) {
        BackendRequest backendRequest;
        backendRequest.method = MethodName(request.method());
        backendRequest.path = request.url().path();
        backendRequest.body = request.body();
        const QString address = AddressFor(request);
        backendRequest.servicesUrl = QStringLiteral("http://%1:%2").arg(address).arg(m_port);
        backendRequest.summonUrl = QStringLiteral("http://%1:%2").arg(address, m_webApiPort);
        const BackendReply reply = m_backend.Handle(backendRequest);
        QHttpServerResponse response(reply.contentType, reply.body,
                                     static_cast<QHttpServerResponse::StatusCode>(reply.status));
        if (!reply.headers.isEmpty()) {
            QHttpHeaders headers = response.headers();
            for (const auto& [name, value] : reply.headers) {
                headers.append(name, value);
            }
            response.setHeaders(std::move(headers));
        }
        responder.sendResponse(response);
    });

    m_tcp = std::make_unique<QTcpServer>(this);
    const QString host = config->GetHost();
    if (!m_tcp->listen(QHostAddress(host), m_port)) {
        qCritical() << "Backend: failed to bind" << host << ":" << m_port << "-"
                    << m_tcp->errorString();
        return false;
    }
    if (!m_http->bind(m_tcp.get())) {
        qCritical() << "Backend: QHttpServer failed to attach to listener";
        return false;
    }

    qInfo().nospace().noquote() << "Backend (the game's online services) listening on: " << host
                                << ":" << m_port;
    qInfo().noquote() << "  ss.info, services and summons at"
                      << QStringLiteral("the address each game connects to")
                      << (m_fixedAddress.isEmpty()
                              ? QString()
                              : QStringLiteral("(%1 when it does not say one)").arg(m_fixedAddress));
    if (m_backend.HasStore()) {
        const Json counts = m_backend.Counts();
        qInfo().noquote() << "  content ->" << QFileInfo(settings.store).absoluteFilePath();
        qInfo().noquote() << QStringLiteral(
                                 "            %1 players, %2 messages, %3 bloodstains, %4 ghosts, "
                                 "%5 chalice dungeons")
                                 .arg(counts.Get(QStringLiteral("users")).IntValue())
                                 .arg(counts.Get(QStringLiteral("blood_messages")).IntValue())
                                 .arg(counts.Get(QStringLiteral("tombstones")).IntValue())
                                 .arg(counts.Get(QStringLiteral("ghosts")).IntValue())
                                 .arg(counts.Get(QStringLiteral("chalice_dungeons")).IntValue());
        QStringList shown(options.showOwn.begin(), options.showOwn.end());
        shown.sort();
        qInfo().noquote() << "            own content shown:"
                          << (shown.isEmpty() ? QStringLiteral("messages only")
                                              : shown.join(QStringLiteral(", ")));
    } else {
        qInfo().noquote() << "  content -> off (world stays empty)";
    }
    qInfo().noquote() << "  notice, broadcast and login message from"
                      << QFileInfo(settings.folder).absoluteFilePath();
    return true;
}

QString BloodborneBackendServer::AddressFor(const QHttpServerRequest& request) const {
    // Each game is sent back to the address it connected to; the given one (HOSTIP) when it does
    // not say one.
    const QString reached = AddressFromHostHeader(request.value("Host"));
    if (!reached.isEmpty()) {
        return reached;
    }
    if (!m_fixedAddress.isEmpty()) {
        return m_fixedAddress;
    }
    QHostAddress local = request.localAddress();
    bool isV4 = false;
    const quint32 v4 = local.toIPv4Address(&isV4);
    if (isV4) {
        return QHostAddress(v4).toString();
    }
    return local.isNull() ? QStringLiteral("127.0.0.1") : local.toString();
}
