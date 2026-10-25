// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The server link (docs/SERVER_COOP_PLAN.md phase 2): every game keeps one UDP
// link to shadNet, on the matching UDP port its STUN pings come to, and sends
// through it what it would send to another game. shadNet forwards each frame to
// the game at the frame's destination, naming the sender by the address its
// STUN pings come from, which is the address the other games know it by: the
// games see the same peers as over P2P, and nothing goes game to game.
//
// The datagrams (the emulator's net_link.h writes and reads the same; one of
// each in tests/contracts/server_link_frames.txt): the signaling vport header
// (ffff ffff), "SHLK", the kind, its body. Numbers little-endian, addresses and
// ports in network byte order.
//
// shadNet never reads a frame: it checks who sent it and where it goes. It
// knows each game's link from its pings (once a second) and tells the others
// when one goes quiet, comes back or is gone, and logs enough to say whether a
// stalled join's packets went through (the first frames between each pair, and
// counters per member).
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace ServerLink {

// 2 (phase 11): the hello carries the player's secret and signs it in.
constexpr quint8 LinkVersion = 2;
enum class Kind : quint8 {
    Hello = 1,
    Welcome = 2,
    Data = 3,
    Ping = 4,
    Pong = 5,
    Member = 6,
    // Our messages (coop/coop.proto) between a game and shadNet itself, in the link's reliable
    // stream (coop/coop_stream.h): the body is one stream frame. Phase 3: the party's world.
    Coop = 7,
    // Phase 11: the calls that went over shadNet's TCP port (friends, scores, the NP sign-in), in
    // a second reliable stream (coop/coop_stream.h) of the link, so a host opens one UDP port.
    Rpc = 8,
};
enum class MemberState : quint8 { Quiet = 0, Back = 1, Gone = 2 };

// An IPv4 address and a port, both in host byte order.
struct Endpoint {
    quint32 ipv4 = 0;
    quint16 port = 0;
    quint64 Key() const {
        return (quint64{ipv4} << 16) | port;
    }
    QString Text() const;
    bool operator==(const Endpoint&) const = default;
};

// Phase 7: a game is known to the others by a virtual address made from its account, never by its
// real one: 10.x.y.z (x.y.z from a hash of the online id) and this port, the same from whatever
// port or address the game links from, and after a restart of the server.
constexpr quint16 VirtualPort = 3658;
Endpoint VirtualEndpointOf(const QByteArray& online_id);

// Phase 11: whether a hello signed in, and when not, the text its game hears (empty: the usual).
struct SignInVerdict {
    bool accepted = false;
    QByteArray refusal;
};

struct Hello {
    quint8 version = LinkVersion;
    QByteArray online_id;
    QByteArray build;
    QByteArray secret; // phase 11: the player id's secret (the launcher made it)
};
struct Welcome {
    bool accepted = false;
    QByteArray text;
};
struct Data {
    Endpoint peer; // where it goes (from a game), where it came from (to a game)
    QByteArray frame;
};
struct Ping {
    quint32 seq = 0;
    quint64 sent_ms = 0;
};
struct Member {
    MemberState state = MemberState::Quiet;
    quint32 gap_ms = 0;
    Endpoint endpoint;
    QByteArray online_id;
};

// The kind of a link datagram, or nothing for anything else (the STUN ping).
std::optional<Kind> KindOf(const QByteArray& datagram);
QByteArray Encode(const Hello& hello);
QByteArray Encode(const Welcome& welcome);
QByteArray Encode(const Data& data);
QByteArray Encode(Kind kind, const Ping& ping);
QByteArray Encode(const Member& member);
QByteArray EncodeCoop(const QByteArray& body);
QByteArray EncodeRpc(const QByteArray& body);
std::optional<Hello> DecodeHello(const QByteArray& datagram);
std::optional<Welcome> DecodeWelcome(const QByteArray& datagram);
std::optional<Data> DecodeData(const QByteArray& datagram);
std::optional<Ping> DecodePing(Kind kind, const QByteArray& datagram);
std::optional<Member> DecodeMember(const QByteArray& datagram);
std::optional<QByteArray> DecodeCoop(const QByteArray& datagram);
std::optional<QByteArray> DecodeRpc(const QByteArray& datagram);

// The links of the games on this server, without sockets or clocks: the socket
// hands it each datagram and calls Tick now and then; it says what to send and
// what to log.
class Hub {
public:
    struct Send {
        Endpoint to;
        QByteArray datagram;
    };
    // A Coop datagram's body for the party's world (bloodborne_world.h), from a linked game.
    struct Coop {
        Endpoint from;
        QByteArray online_id;
        QByteArray body;
    };
    struct Result {
        QList<Send> sends;
        QStringList log;
        QList<Coop> coop;
        QList<Coop> rpc;      // phase 11: the Rpc stream's datagram bodies, the same way
        QList<Endpoint> gone; // links forgotten
        // Kaniel 2026-10-25: links newly quiet and heard again (the world's teams: a game gone a
        // few seconds leaves its started team).
        QList<Endpoint> quiet;
        QList<Endpoint> back;
        // Phase 11: links silent for lost_ms (once a silence): their game's sign-in session ends,
        // as its TCP connection's did when its game died. A blip under that rides out.
        QList<Endpoint> lost;
        // Games welcomed, with their virtual addresses (phase 7: what the rooms hand out).
        struct Linked {
            QByteArray online_id;
            Endpoint virtual_endpoint;
            Endpoint endpoint;
        };
        QList<Linked> linked;
    };

    explicit Hub(qint64 quiet_ms = 3000, qint64 gone_ms = 60000, qint64 stats_ms = 10000,
                 int pair_lines = 20, qint64 lost_ms = 12000);

    Result Receive(const Endpoint& from, const QByteArray& datagram, qint64 now_ms);
    // Links newly quiet (told to the others), gone (forgotten) and the counters.
    Result Tick(qint64 now_ms);

    // Phase 7: each link's team (its party, empty before the game said which). Once set, a frame
    // and a member notice go only to games of the sender's team, never to one in no team.
    using PartyOf = std::function<QString(const Endpoint&)>;
    void SetPartyOf(PartyOf party_of) {
        this->party_of = std::move(party_of);
    }
    // Invasions (docs/INVASIONS.md): the party a link visits as a hostile invader (empty: none).
    // A visitor and the visited party's games are one team for the relay.
    void SetVisiting(PartyOf visiting) {
        this->visiting = std::move(visiting);
    }
    // Phase 9: whether a game packet on `channel` goes from one link's game to another's
    // (coop/packet_routes.h). Once set, the hub reads every frame of game packets and forwards
    // only what this lets through; without it frames go unread.
    using PacketRoutes =
        std::function<bool(const Endpoint& from, const Endpoint& to, quint32 channel)>;
    void SetPacketRoutes(PacketRoutes packet_routes) {
        this->packet_routes = std::move(packet_routes);
    }

    // Phase 11: whether a hello's player id and secret sign in. Without it every hello does.
    // Hosted (docs/HOSTED_SERVER.md): nothing yet while the password is checked on another thread;
    // the hello gets no answer and the game's next one (a second later) gets the verdict.
    // A refusal may say why (a new id on another account's address: the launcher makes another).
    using SignIn = std::function<std::optional<SignInVerdict>(const QByteArray& online_id,
                                                              const QByteArray& secret)>;
    void SetSignIn(SignIn sign_in) {
        this->sign_in = std::move(sign_in);
    }

    // Hosted: the build every game must say (empty: the first linked game's, as before).
    void SetRequiredBuild(QByteArray build) {
        required_build = std::move(build);
    }

    // Build check on join (coop/build_fingerprint.h): shadNet's own build, which a game's must
    // match where the server knows the field (its release); empty: the server does not check its
    // own (a dev or rig server built apart from the games). Without it only the games here count.
    void SetServerBuild(QByteArray fingerprint) {
        server_build = std::move(fingerprint);
    }

    int Members() const {
        return static_cast<int>(members.size());
    }
    // The build every game here runs, empty while none is linked.
    QByteArray Build() const;
    // The virtual address of the game linked from `real`, nothing when none is.
    std::optional<Endpoint> VirtualOf(const Endpoint& real) const;

    // The operator's view (admin.h): every link.
    struct LinkRow {
        Endpoint endpoint;
        QByteArray online_id;
        QByteArray build;
        qint64 last_heard_ms = 0;
    };
    QList<LinkRow> Links() const;
    // The operator's kick: the links of `online_id` (when not empty) or from `ipv4` (when not 0)
    // forgotten now, the others told they went. A game whose link is forgotten hears no more pongs
    // and says hello again, as after any outage.
    Result Kick(const QByteArray& online_id, quint32 ipv4, qint64 now_ms);

private:
    struct Link {
        Endpoint endpoint;
        Endpoint virtual_endpoint;
        QByteArray online_id;
        QByteArray build;
        qint64 last_heard_ms = 0;
        bool quiet = false;
        bool lost = false; // its sign-in session ended for this silence
        quint64 frames_in = 0;
        quint64 bytes_in = 0;
        quint64 frames_out = 0;
        quint64 bytes_out = 0;
        quint32 pings = 0;
        std::map<quint32, quint64> refused; // game packets not forwarded, per channel
    };

    void OnHello(const Endpoint& from, const QByteArray& datagram, qint64 now_ms, Result& result);
    void OnData(Link& sender, const QByteArray& datagram, Result& result);
    void Heard(Link& link, qint64 now_ms, Result& result);
    void Unlink(quint64 key);
    void TellOthers(const Link& about, MemberState state, qint64 gap_ms, Result& result);
    QString Name(const Link& link) const;
    bool SameTeam(const Endpoint& a, const Endpoint& b) const;
    // Invasions: one of them visits the other's party as an invader.
    bool Visits(const Endpoint& a, const Endpoint& b) const;
    // The frame with only the game packets the routes let reach `target`, or nothing when it is
    // a frame of game packets that does not read.
    std::optional<QByteArray> Routed(Link& sender, const Link& target, const QByteArray& frame);

    qint64 quiet_ms;
    qint64 gone_ms;
    qint64 stats_ms;
    qint64 lost_ms;
    int pair_lines;
    bool stats_started = false;
    qint64 stats_at_ms = 0;
    QHash<quint64, Link> members;
    QHash<quint64, quint64> by_virtual; // a link's virtual address -> its real one (keys)
    QHash<QPair<quint64, quint64>, quint64> pair_frames;
    quint64 dropped = 0;
    PartyOf party_of;
    PartyOf visiting;
    PacketRoutes packet_routes;
    SignIn sign_in;
    QByteArray required_build;
    QByteArray server_build;
};

} // namespace ServerLink
