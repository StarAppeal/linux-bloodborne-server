// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "server_link.h"

#include <algorithm>
#include <QtEndian>
#include "coop/build_fingerprint.h"
#include "coop/packet_routes.h"
#include "virtual_address.h"

namespace ServerLink {

namespace {

constexpr int HeaderSize = 9; // ffff ffff, "SHLK", the kind
constexpr int DataHeaderSize = HeaderSize + 6;
constexpr quint64 DropsLogged = 50;

QByteArray Start(Kind kind) {
    QByteArray out = QByteArray::fromHex("ffffffff") + "SHLK";
    out.append(static_cast<char>(kind));
    return out;
}

template <typename T>
void Put(QByteArray& out, T value) {
    for (size_t index = 0; index < sizeof(T); ++index) {
        out.append(static_cast<char>(static_cast<quint64>(value) >> (8 * index)));
    }
}

void PutEndpoint(QByteArray& out, const Endpoint& endpoint) {
    const quint32 ipv4 = qToBigEndian(endpoint.ipv4);
    const quint16 port = qToBigEndian(endpoint.port);
    out.append(reinterpret_cast<const char*>(&ipv4), 4);
    out.append(reinterpret_cast<const char*>(&port), 2);
}

void PutText(QByteArray& out, const QByteArray& text) {
    const int size = std::min<int>(static_cast<int>(text.size()), 0xFF);
    out.append(static_cast<char>(size));
    out.append(text.left(size));
}

// Reads a body from its start; every read fails once the bytes run out.
class Reader {
public:
    Reader(const QByteArray& bytes, int at) : bytes{bytes}, at{at} {}

    template <typename T>
    bool Get(T& value) {
        if (bytes.size() - at < static_cast<int>(sizeof(T))) {
            return false;
        }
        quint64 read = 0;
        for (size_t index = 0; index < sizeof(T); ++index) {
            read |= quint64{static_cast<quint8>(bytes[at + static_cast<int>(index)])}
                    << (8 * index);
        }
        value = static_cast<T>(read);
        at += static_cast<int>(sizeof(T));
        return true;
    }

    bool GetEndpoint(Endpoint& endpoint) {
        if (bytes.size() - at < 6) {
            return false;
        }
        endpoint.ipv4 = qFromBigEndian<quint32>(bytes.constData() + at);
        endpoint.port = qFromBigEndian<quint16>(bytes.constData() + at + 4);
        at += 6;
        return true;
    }

    bool GetText(QByteArray& text) {
        quint8 size = 0;
        if (!Get(size) || bytes.size() - at < size) {
            return false;
        }
        text = bytes.mid(at, size);
        at += size;
        return true;
    }

    QByteArray Rest() const {
        return bytes.mid(at);
    }

private:
    const QByteArray& bytes;
    int at;
};

std::optional<Reader> Body(const QByteArray& datagram, Kind kind) {
    if (datagram.size() < HeaderSize ||
        static_cast<quint8>(datagram[8]) != static_cast<quint8>(kind) ||
        !datagram.startsWith(QByteArray::fromHex("ffffffff") + "SHLK")) {
        return std::nullopt;
    }
    return Reader{datagram, HeaderSize};
}

// The first bytes of a frame, past its vports, in hex.
QString Head(const QByteArray& frame) {
    return QString::fromLatin1(frame.mid(4, 16).toHex());
}

quint16 Vport(const QByteArray& frame, int at) {
    return frame.size() >= at + 2 ? qFromBigEndian<quint16>(frame.constData() + at) : 0;
}

} // namespace

Endpoint VirtualEndpointOf(const QByteArray& online_id) {
    return {VirtualAddressOf(online_id), VirtualPort};
}

QString Endpoint::Text() const {
    return QStringLiteral("%1.%2.%3.%4:%5")
        .arg(ipv4 >> 24)
        .arg((ipv4 >> 16) & 0xFF)
        .arg((ipv4 >> 8) & 0xFF)
        .arg(ipv4 & 0xFF)
        .arg(port);
}

std::optional<Kind> KindOf(const QByteArray& datagram) {
    if (datagram.size() < HeaderSize) {
        return std::nullopt;
    }
    const quint8 kind = static_cast<quint8>(datagram[8]);
    if (kind < static_cast<quint8>(Kind::Hello) || kind > static_cast<quint8>(Kind::Rpc) ||
        !Body(datagram, static_cast<Kind>(kind))) {
        return std::nullopt;
    }
    return static_cast<Kind>(kind);
}

QByteArray Encode(const Hello& hello) {
    QByteArray out = Start(Kind::Hello);
    out.append(static_cast<char>(hello.version));
    PutText(out, hello.online_id);
    PutText(out, hello.build);
    PutText(out, hello.secret);
    return out;
}

QByteArray Encode(const Welcome& welcome) {
    QByteArray out = Start(Kind::Welcome);
    out.append(static_cast<char>(welcome.accepted ? 1 : 0));
    PutText(out, welcome.text);
    return out;
}

QByteArray Encode(const Data& data) {
    QByteArray out = Start(Kind::Data);
    out.reserve(DataHeaderSize + data.frame.size());
    PutEndpoint(out, data.peer);
    out.append(data.frame);
    return out;
}

QByteArray Encode(Kind kind, const Ping& ping) {
    QByteArray out = Start(kind);
    Put(out, ping.seq);
    Put(out, ping.sent_ms);
    return out;
}

QByteArray Encode(const Member& member) {
    QByteArray out = Start(Kind::Member);
    out.append(static_cast<char>(member.state));
    Put(out, member.gap_ms);
    PutEndpoint(out, member.endpoint);
    PutText(out, member.online_id);
    return out;
}

QByteArray EncodeCoop(const QByteArray& body) {
    return Start(Kind::Coop) + body;
}

QByteArray EncodeRpc(const QByteArray& body) {
    return Start(Kind::Rpc) + body;
}

std::optional<Hello> DecodeHello(const QByteArray& datagram) {
    auto body = Body(datagram, Kind::Hello);
    Hello hello;
    if (!body || !body->Get(hello.version) || !body->GetText(hello.online_id) ||
        !body->GetText(hello.build)) {
        return std::nullopt;
    }
    // Link version 1 had no secret (a game of another build, refused for its version).
    if (hello.version >= 2 && !body->GetText(hello.secret)) {
        return std::nullopt;
    }
    return hello;
}

std::optional<Welcome> DecodeWelcome(const QByteArray& datagram) {
    auto body = Body(datagram, Kind::Welcome);
    quint8 accepted = 0;
    Welcome welcome;
    if (!body || !body->Get(accepted) || !body->GetText(welcome.text)) {
        return std::nullopt;
    }
    welcome.accepted = accepted != 0;
    return welcome;
}

std::optional<Data> DecodeData(const QByteArray& datagram) {
    auto body = Body(datagram, Kind::Data);
    Data data;
    if (!body || !body->GetEndpoint(data.peer)) {
        return std::nullopt;
    }
    data.frame = body->Rest();
    return data;
}

std::optional<Ping> DecodePing(Kind kind, const QByteArray& datagram) {
    auto body = Body(datagram, kind);
    Ping ping;
    if (!body || !body->Get(ping.seq) || !body->Get(ping.sent_ms)) {
        return std::nullopt;
    }
    return ping;
}

std::optional<Member> DecodeMember(const QByteArray& datagram) {
    auto body = Body(datagram, Kind::Member);
    quint8 state = 0;
    Member member;
    if (!body || !body->Get(state) || state > static_cast<quint8>(MemberState::Gone) ||
        !body->Get(member.gap_ms) || !body->GetEndpoint(member.endpoint) ||
        !body->GetText(member.online_id)) {
        return std::nullopt;
    }
    member.state = static_cast<MemberState>(state);
    return member;
}

std::optional<QByteArray> DecodeCoop(const QByteArray& datagram) {
    auto body = Body(datagram, Kind::Coop);
    if (!body) {
        return std::nullopt;
    }
    return body->Rest();
}

std::optional<QByteArray> DecodeRpc(const QByteArray& datagram) {
    auto body = Body(datagram, Kind::Rpc);
    if (!body) {
        return std::nullopt;
    }
    return body->Rest();
}

Hub::Hub(qint64 quiet_ms, qint64 gone_ms, qint64 stats_ms, int pair_lines, qint64 lost_ms)
    : quiet_ms{quiet_ms}, gone_ms{gone_ms}, stats_ms{stats_ms}, lost_ms{lost_ms},
      pair_lines{pair_lines} {}

QByteArray Hub::Build() const {
    for (const Link& link : members) {
        if (!link.build.isEmpty()) {
            return link.build;
        }
    }
    return {};
}

std::optional<Endpoint> Hub::VirtualOf(const Endpoint& real) const {
    const auto found = members.find(real.Key());
    return found == members.end() ? std::nullopt : std::optional{found->virtual_endpoint};
}

void Hub::Unlink(quint64 key) {
    const Link link = members.take(key);
    by_virtual.remove(link.virtual_endpoint.Key());
    for (auto it = pair_frames.begin(); it != pair_frames.end();) {
        it =
            it.key().first == key || it.key().second == key ? pair_frames.erase(it) : std::next(it);
    }
}

QString Hub::Name(const Link& link) const {
    return QStringLiteral("%1 (%2)").arg(QString::fromUtf8(link.online_id), link.endpoint.Text());
}

bool Hub::SameTeam(const Endpoint& a, const Endpoint& b) const {
    if (!party_of) {
        return true; // nobody said the teams (the hub alone)
    }
    const QString party = party_of(a);
    const QString other = party_of(b);
    if (!party.isEmpty() && party == other) {
        return true;
    }
    return Visits(a, b);
}

bool Hub::Visits(const Endpoint& a, const Endpoint& b) const {
    if (!visiting || !party_of) {
        return false;
    }
    const QString a_visits = visiting(a);
    const QString b_visits = visiting(b);
    return (!a_visits.isEmpty() && a_visits == party_of(b)) ||
           (!b_visits.isEmpty() && b_visits == party_of(a));
}

Hub::Result Hub::Receive(const Endpoint& from, const QByteArray& datagram, qint64 now_ms) {
    Result result;
    const auto kind = KindOf(datagram);
    if (!kind) {
        return result;
    }
    if (*kind == Kind::Hello) {
        OnHello(from, datagram, now_ms, result);
        return result;
    }
    const auto found = members.find(from.Key());
    if (found == members.end()) {
        if (dropped++ < DropsLogged) {
            result.log.append(QStringLiteral("server link: kind %1 from %2, which "
                                             "said no hello: dropped")
                                  .arg(static_cast<int>(*kind))
                                  .arg(from.Text()));
        }
        return result;
    }
    Link& link = *found;
    Heard(link, now_ms, result);
    switch (*kind) {
    case Kind::Data:
        OnData(link, datagram, result);
        break;
    case Kind::Ping:
        if (const auto ping = DecodePing(Kind::Ping, datagram)) {
            ++link.pings;
            result.sends.append({from, Encode(Kind::Pong, *ping)});
        }
        break;
    case Kind::Coop:
        if (auto body = DecodeCoop(datagram)) {
            result.coop.append({from, link.online_id, std::move(*body)});
        }
        break;
    case Kind::Rpc:
        if (auto body = DecodeRpc(datagram)) {
            result.rpc.append({from, link.online_id, std::move(*body)});
        }
        break;
    default:
        break; // the games' own kinds only
    }
    return result;
}

void Hub::OnHello(const Endpoint& from, const QByteArray& datagram, qint64 now_ms, Result& result) {
    const auto hello = DecodeHello(datagram);
    if (!hello) {
        return;
    }
    QByteArray refusal;
    QByteArray refusal_logged; // when the game's text says more than its log needs
    QString checked_aside;     // settings that differ but are only logged
    if (hello->version != LinkVersion) {
        refusal = "link version " + QByteArray::number(hello->version) + ", this server speaks " +
                  QByteArray::number(LinkVersion);
    } else if (!required_build.isEmpty() && !required_build.startsWith("bb-build ")) {
        // Hosted: a HostedBuild that is no build fingerprint is the whole text every game must
        // say.
        if (!hello->build.isEmpty() && hello->build != required_build) {
            refusal = "another build: everyone needs the same release (this game runs " +
                      hello->build + ", the server needs " + required_build + ")";
        }
    } else if (!hello->build.isEmpty()) {
        // Build check on join: the games linked here (a game saying hello again is not one; all
        // of them run one build, so the first says it) and the server's own release (Hosted: a
        // HostedBuild fingerprint in its place). Phase 11: a game signs in before its co-op code
        // says the build; none said holds nothing, either way.
        std::optional<BloodborneCoop::BuildFingerprint> here;
        for (const Link& link : std::as_const(members)) {
            if (!(link.endpoint == from) && !link.build.isEmpty()) {
                here = BloodborneCoop::ParseBuildFingerprint(link.build.toStdString());
                break;
            }
        }
        const QByteArray& own = required_build.isEmpty() ? server_build : required_build;
        const auto server =
            own.isEmpty() ? std::nullopt : BloodborneCoop::ParseBuildFingerprint(own.toStdString());
        const auto build = BloodborneCoop::BuildRefusalOf(
            hello->build.toStdString(), server ? &*server : nullptr, here ? &*here : nullptr);
        if (!build.text.empty()) {
            // The team screen's line first, then the whole reason (the link's text holds 255
            // bytes: the log has all of it).
            refusal = QByteArray::fromStdString(build.notice + "\n" + build.text);
            refusal_logged = QByteArray::fromStdString(build.text);
        } else if (const auto game =
                       BloodborneCoop::ParseBuildFingerprint(hello->build.toStdString());
                   game && here) {
            if (const std::string other = BloodborneCoop::DescribeDifferences(*game, *here);
                !other.empty()) {
                checked_aside = QString::fromStdString(other);
            }
        }
    }
    if (refusal.isEmpty() && sign_in) {
        const std::optional<SignInVerdict> signed_in = sign_in(hello->online_id, hello->secret);
        if (!signed_in) {
            return; // still checking: the next hello hears
        }
        if (!signed_in->accepted) {
            refusal =
                !signed_in->refusal.isEmpty()
                    ? signed_in->refusal
                    : "sign-in refused: another player has this id (" + hello->online_id + ")";
        }
    }
    const Endpoint virtual_endpoint = VirtualEndpointOf(hello->online_id);
    if (refusal.isEmpty()) {
        const auto holder = by_virtual.find(virtual_endpoint.Key());
        if (holder != by_virtual.end() && *holder != from.Key() &&
            members.value(*holder).online_id != hello->online_id) {
            refusal = "another account has this game's address (" +
                      members.value(*holder).online_id + "): use another account name";
        }
    }
    if (refusal.isEmpty()) {
        // Two PCs with one player id (a friend's install copied with its user\coop.json): each
        // game's hello took the link from the other every few seconds, so a team made on one left
        // at once and neither saw the other (player report 2026-10-24). A game that restarts or
        // whose connection moves leaves its old link quiet first; one still heard is another PC.
        for (const Link& other : std::as_const(members)) {
            if (other.online_id == hello->online_id && other.endpoint.ipv4 != from.ipv4 &&
                now_ms - other.last_heard_ms <= quiet_ms) {
                refusal = "another PC plays with this player id, a copied install: on one PC "
                          "delete user\\coop.json and set up co-op again (" +
                          other.endpoint.Text().toUtf8() + ")";
                break;
            }
        }
    }
    if (!refusal.isEmpty()) {
        result.sends.append({from, Encode(Welcome{false, refusal.left(0xFF)})});
        result.log.append(
            QStringLiteral("server link: refused %1 (%2): %3")
                .arg(QString::fromUtf8(hello->online_id), from.Text(),
                     QString::fromUtf8(refusal_logged.isEmpty() ? refusal : refusal_logged)));
        return;
    }
    if (!checked_aside.isEmpty()) {
        result.log.append(
            QStringLiteral("server link: %1 (%2) runs other settings than the games here (%3)")
                .arg(QString::fromUtf8(hello->online_id), from.Text(), checked_aside));
    }
    if (!stats_started) {
        stats_started = true;
        stats_at_ms = now_ms;
    }
    // The same account linked from another port (its game restarted, or its connection moved): one
    // member, so the old link is forgotten without telling the others it went.
    QList<quint64> moved;
    for (const Link& other : std::as_const(members)) {
        if (other.online_id == hello->online_id && !(other.endpoint == from)) {
            moved.append(other.endpoint.Key());
        }
    }
    for (const quint64 key : moved) {
        const Endpoint old = members.value(key).endpoint;
        Unlink(key);
        result.gone.append(old);
        result.log.append(QStringLiteral("server link: %1 moved from %2 to %3")
                              .arg(QString::fromUtf8(hello->online_id), old.Text(), from.Text()));
    }
    const bool again = members.contains(from.Key());
    Link& link = members[from.Key()];
    link.virtual_endpoint = virtual_endpoint;
    by_virtual[virtual_endpoint.Key()] = from.Key();
    link.endpoint = from;
    link.online_id = hello->online_id;
    link.build = hello->build;
    link.last_heard_ms = now_ms;
    link.quiet = false;
    result.sends.append({from, Encode(Welcome{true, {}})});
    result.linked.append({link.online_id, link.virtual_endpoint, link.endpoint});
    result.log.append(
        QStringLiteral("server link: %1 %2, build %3, %4 linked")
            .arg(again ? "hello again from" : "linked", Name(link), QString::fromUtf8(link.build))
            .arg(members.size()));
}

void Hub::OnData(Link& sender, const QByteArray& datagram, Result& result) {
    const auto data = DecodeData(datagram);
    if (!data) {
        return;
    }
    ++sender.frames_in;
    sender.bytes_in += static_cast<quint64>(data->frame.size());
    // A game names the others by their virtual addresses only (phase 9: a real one finds nobody).
    const auto by_address = by_virtual.find(data->peer.Key());
    const auto target = by_address != by_virtual.end() ? members.find(*by_address) : members.end();
    if (target == members.end()) {
        if (dropped++ < DropsLogged) {
            result.log.append(QStringLiteral("server link: %1 sent %2 bytes to %3: no game linked "
                                             "there, dropped")
                                  .arg(Name(sender))
                                  .arg(data->frame.size())
                                  .arg(data->peer.Text()));
        }
        return;
    }
    if (!SameTeam(sender.endpoint, target->endpoint)) {
        if (dropped++ < DropsLogged) {
            result.log.append(QStringLiteral("server link: %1 sent %2 bytes to %3, a game of "
                                             "another team or none: dropped")
                                  .arg(Name(sender))
                                  .arg(data->frame.size())
                                  .arg(Name(*target)));
        }
        return;
    }
    // Invasions: between an invader and the team it visits only the game's own packets go (they
    // pass the hostile table); our co-op frames (direct vitals, kills, the world's) never.
    if (Visits(sender.endpoint, target->endpoint) && data->frame.size() >= 9 &&
        data->frame.mid(4, 4) == "SHAD" &&
        static_cast<quint8>(data->frame[8]) != BloodborneCoop::GamePacketsType) {
        ++sender.refused[BloodborneCoop::NativeChannelCount];
        return;
    }
    const auto routed = Routed(sender, *target, data->frame);
    if (!routed) {
        if (dropped++ < DropsLogged) {
            result.log.append(QStringLiteral("server link: %1 sent %2 bytes of game packets to %3 "
                                             "that does not read: dropped")
                                  .arg(Name(sender))
                                  .arg(data->frame.size())
                                  .arg(Name(*target)));
        }
        return;
    }
    ++target->frames_out;
    target->bytes_out += static_cast<quint64>(routed->size());
    result.sends.append({target->endpoint, Encode(Data{sender.virtual_endpoint, *routed})});
    // The first frames between each pair: a stalled join shows which went.
    const quint64 count = ++pair_frames[qMakePair(sender.endpoint.Key(), target->endpoint.Key())];
    if (count <= static_cast<quint64>(pair_lines)) {
        result.log.append(QStringLiteral("server link: %1 -> %2 #%3: %4 bytes, vport %5 -> %6, %7")
                              .arg(Name(sender), Name(*target))
                              .arg(count)
                              .arg(data->frame.size() - 4)
                              .arg(Vport(data->frame, 0))
                              .arg(Vport(data->frame, 2))
                              .arg(Head(data->frame)));
    }
}

std::optional<QByteArray> Hub::Routed(Link& sender, const Link& target, const QByteArray& frame) {
    // The signaling vports (4 bytes), "SHAD", the type, the game frame.
    constexpr qsizetype GameFrameAt = 9;
    if (!packet_routes || frame.size() < GameFrameAt || frame.mid(4, 4) != "SHAD" ||
        static_cast<quint8>(frame[8]) != BloodborneCoop::GamePacketsType) {
        return frame;
    }
    auto game_frame = BloodborneCoop::DecodeGameFrame(
        {reinterpret_cast<const u8*>(frame.constData()) + GameFrameAt,
         static_cast<size_t>(frame.size() - GameFrameAt)});
    if (!game_frame) {
        return std::nullopt;
    }
    const size_t refused = BloodborneCoop::RouteGameFrame(*game_frame, [&](u32 channel) {
        if (packet_routes(sender.endpoint, target.endpoint, channel)) {
            return true;
        }
        ++sender.refused[channel];
        return false;
    });
    if (refused == 0) {
        return frame;
    }
    const auto payload = BloodborneCoop::EncodeGameFrame(*game_frame);
    return frame.left(GameFrameAt) + QByteArray(reinterpret_cast<const char*>(payload.data()),
                                                static_cast<qsizetype>(payload.size()));
}

void Hub::Heard(Link& link, qint64 now_ms, Result& result) {
    if (link.quiet) {
        const qint64 gap = now_ms - link.last_heard_ms;
        link.quiet = false;
        result.log.append(
            QStringLiteral("server link: %1 heard again after %2 ms").arg(Name(link)).arg(gap));
        TellOthers(link, MemberState::Back, gap, result);
        result.back.append(link.endpoint);
    }
    link.last_heard_ms = now_ms;
    link.lost = false;
}

void Hub::TellOthers(const Link& about, MemberState state, qint64 gap_ms, Result& result) {
    const QByteArray told =
        Encode(Member{state, static_cast<quint32>(std::clamp<qint64>(gap_ms, 0, 0xFFFFFFFF)),
                      about.virtual_endpoint, about.online_id});
    for (const Link& other : std::as_const(members)) {
        if (!(other.endpoint == about.endpoint) && SameTeam(about.endpoint, other.endpoint)) {
            result.sends.append({other.endpoint, told});
        }
    }
}

QList<Hub::LinkRow> Hub::Links() const {
    QList<LinkRow> rows;
    rows.reserve(members.size());
    for (const Link& link : members) {
        rows.append({link.endpoint, link.online_id, link.build, link.last_heard_ms});
    }
    return rows;
}

Hub::Result Hub::Kick(const QByteArray& online_id, quint32 ipv4, qint64 now_ms) {
    Result result;
    QList<quint64> kicked;
    for (const Link& link : std::as_const(members)) {
        if ((!online_id.isEmpty() && link.online_id == online_id) ||
            (ipv4 != 0 && link.endpoint.ipv4 == ipv4)) {
            kicked.append(link.endpoint.Key());
        }
    }
    for (const quint64 key : kicked) {
        const Link link = members.value(key);
        Unlink(key);
        result.log.append(QStringLiteral("server link: %1 kicked by the operator, %2 linked")
                              .arg(Name(link))
                              .arg(members.size()));
        TellOthers(link, MemberState::Gone, now_ms - link.last_heard_ms, result);
        result.gone.append(link.endpoint);
    }
    return result;
}

Hub::Result Hub::Tick(qint64 now_ms) {
    Result result;
    QList<quint64> gone;
    for (const Link& link : std::as_const(members)) {
        const qint64 gap = now_ms - link.last_heard_ms;
        if (gap > gone_ms) {
            gone.append(link.endpoint.Key());
        }
    }
    for (const quint64 key : gone) {
        const Link link = members.value(key);
        Unlink(key);
        result.log.append(QStringLiteral("server link: %1 gone, nothing for %2 ms, %3 linked")
                              .arg(Name(link))
                              .arg(now_ms - link.last_heard_ms)
                              .arg(members.size()));
        TellOthers(link, MemberState::Gone, now_ms - link.last_heard_ms, result);
        result.gone.append(link.endpoint);
    }
    for (Link& link : members) {
        const qint64 gap = now_ms - link.last_heard_ms;
        if (!link.quiet && gap > quiet_ms) {
            link.quiet = true;
            result.log.append(
                QStringLiteral("server link: nothing from %1 for %2 ms").arg(Name(link)).arg(gap));
            TellOthers(link, MemberState::Quiet, gap, result);
            result.quiet.append(link.endpoint);
        }
        if (!link.lost && gap > lost_ms) {
            link.lost = true;
            result.lost.append(link.endpoint);
            result.log.append(QStringLiteral("server link: nothing from %1 for %2 ms: its "
                                             "sign-in ends until it is back")
                                  .arg(Name(link))
                                  .arg(gap));
        }
    }
    if (stats_started && now_ms - stats_at_ms >= stats_ms) {
        stats_at_ms = now_ms;
        for (Link& link : members) {
            QString refused;
            for (const auto& [channel, count] : link.refused) {
                refused += QStringLiteral("%1%2: %3")
                               .arg(refused.isEmpty() ? ", refused " : ", ")
                               .arg(QStringLiteral("0x%1").arg(channel, 0, 16))
                               .arg(count);
            }
            link.refused.clear();
            result.log.append(QStringLiteral("server link: %1 in %2 frames/%3 bytes, out %4 "
                                             "frames/%5 bytes, %6 pings%7%8")
                                  .arg(Name(link))
                                  .arg(link.frames_in)
                                  .arg(link.bytes_in)
                                  .arg(link.frames_out)
                                  .arg(link.bytes_out)
                                  .arg(link.pings)
                                  .arg(link.quiet ? ", quiet" : "")
                                  .arg(refused));
            link.frames_in = link.bytes_in = link.frames_out = link.bytes_out = 0;
            link.pings = 0;
        }
    }
    return result;
}

} // namespace ServerLink
