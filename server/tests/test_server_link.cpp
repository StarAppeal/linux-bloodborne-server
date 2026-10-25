// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The server link's datagrams as the emulator writes them
// (tests/contracts/server_link_frames.txt, which the emulator's tests read
// too), and the hub that forwards the games' frames between them.
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "coop/packet_routes.h"
#include "server_link.h"

using namespace ServerLink;

namespace {

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!Check((expression), #expression, __LINE__))                           \
      return 1;                                                                \
  } while (false)

std::map<std::string, QByteArray> ReadContracts(const char *path) {
  std::map<std::string, QByteArray> datagrams;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    datagrams[line.substr(0, tab)] =
        QByteArray::fromHex(QByteArray::fromStdString(line.substr(tab + 1)));
  }
  return datagrams;
}

const Endpoint Ada127{0x7F000001, 0x1234};
const QByteArray GameFrame = QByteArray::fromHex("75307531070000000200000"
                                                 "0abcd");
// The contracts' hello carries the text before the build fingerprint.
const QByteArray ContractBuild = "92829ac4 events 1f protocol 9";
// Builds as the games say them (coop/build_fingerprint.h).
const QByteArray Build =
    "bb-build 1;version=1.2.0;revision=92829ac4;protocol=9;"
    "events=1f;harder=off;fps=60;";
const QByteArray OtherBuild = "bb-build 1;version=1.2.0;revision=13ddd14d;"
                              "protocol=9;events=1f;harder=off;fps=60;";

const Endpoint Kaniel{0x7F000001, 40001};
const Endpoint Ada{0x7F000001, 40002};
const Endpoint Eileen{0xC0A80105, 40003};

QByteArray HelloFrom(const char *who, const QByteArray &build = Build,
                     const QByteArray &secret = "s3cr3t") {
  return Encode(Hello{LinkVersion, who, build, secret});
}

// The sends of `result` to `to`.
QList<QByteArray> SentTo(const Hub::Result &result, const Endpoint &to) {
  QList<QByteArray> sent;
  for (const auto &send : result.sends) {
    if (send.to == to) {
      sent.append(send.datagram);
    }
  }
  return sent;
}

int Contracts() {
  const auto datagrams = ReadContracts(BLOODBORNE_SERVER_LINK_CONTRACTS);
  CHECK(datagrams.size() == 10);
  const auto datagram = [&](const char *name) { return datagrams.at(name); };

  CHECK(Encode(Hello{LinkVersion, "Kaniel", ContractBuild, "s3cr3t"}) ==
        datagram("hello"));
  const auto hello = DecodeHello(datagram("hello"));
  CHECK(hello && hello->version == 2 && hello->online_id == "Kaniel" &&
        hello->build == ContractBuild && hello->secret == "s3cr3t");
  // Phase 11: the old calls (friends, scores) in the link's second stream.
  CHECK(EncodeRpc(QByteArray::fromHex("0102")) == datagram("rpc"));
  CHECK(DecodeRpc(datagram("rpc")) == QByteArray::fromHex("0102"));
  CHECK(!DecodeRpc(datagram("ping")) && !DecodeCoop(datagram("rpc")));

  CHECK(Encode(Welcome{true, {}}) == datagram("welcome.ok"));
  CHECK(Encode(Welcome{false, "another build: 13ddd14d"}) ==
        datagram("welcome.refused"));
  const auto refused = DecodeWelcome(datagram("welcome.refused"));
  CHECK(refused && !refused->accepted &&
        refused->text == "another build: 13ddd14d");

  CHECK(Encode(Data{Ada127, GameFrame}) == datagram("data"));
  const auto data = DecodeData(datagram("data"));
  CHECK(data && data->peer == Ada127 && data->frame == GameFrame);

  CHECK(Encode(Kind::Ping, Ping{7, 1000}) == datagram("ping"));
  CHECK(Encode(Kind::Pong, Ping{7, 1000}) == datagram("pong"));
  const auto ping = DecodePing(Kind::Ping, datagram("ping"));
  CHECK(ping && ping->seq == 7 && ping->sent_ms == 1000);
  CHECK(!DecodePing(Kind::Ping, datagram("pong")));

  CHECK(Encode(Member{MemberState::Quiet, 3500, Ada127, "Ada"}) ==
        datagram("member.quiet"));
  CHECK(Encode(Member{MemberState::Back, 4200, Ada127, "Ada"}) ==
        datagram("member.back"));
  CHECK(Encode(Member{MemberState::Gone, 60000, Ada127, "Ada"}) ==
        datagram("member.gone"));
  const auto gone = DecodeMember(datagram("member.gone"));
  CHECK(gone && gone->state == MemberState::Gone && gone->gap_ms == 60000 &&
        gone->endpoint == Ada127 && gone->online_id == "Ada");

  for (const auto &[name, bytes] : datagrams) {
    CHECK(KindOf(bytes).has_value());
    for (int size = 0; size < bytes.size(); ++size) {
      const QByteArray cut = bytes.left(size);
      if (name == "data" && size >= 15) {
        continue; // a shorter frame is still a frame
      }
      CHECK(!DecodeHello(cut) && !DecodeWelcome(cut) && !DecodeData(cut) &&
            !DecodePing(Kind::Ping, cut) && !DecodePing(Kind::Pong, cut) &&
            !DecodeMember(cut));
    }
  }
  // The STUN ping shares the header.
  CHECK(!KindOf(QByteArray::fromHex("ffffffff014b616e69656c")));
  std::cout << "server link contracts: ok\n";
  return 0;
}

int HelloAndBuilds() {
  Hub hub;
  auto result = hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  CHECK(SentTo(result, Kaniel).size() == 1);
  CHECK(DecodeWelcome(SentTo(result, Kaniel)[0])->accepted);
  CHECK(hub.Members() == 1 && hub.Build() == Build);

  result = hub.Receive(Ada, HelloFrom("Ada"), 10);
  CHECK(DecodeWelcome(SentTo(result, Ada).value(0))->accepted);
  CHECK(hub.Members() == 2);

  // One build for everyone: the 0x60 record carries a raw vtable pointer.
  result = hub.Receive(Eileen, HelloFrom("Eileen", OtherBuild), 20);
  const auto refused = DecodeWelcome(SentTo(result, Eileen).value(0));
  CHECK(refused && !refused->accepted);
  // The team screen's line first, then the whole reason (as logged).
  CHECK(refused->text ==
        "Co-op refused: your game is build 13ddd14d, the games here run "
        "92829ac4: use the same release\n"
        "another build: your game is build 13ddd14d, the games here run "
        "92829ac4: use the same release");
  CHECK(hub.Members() == 2);
  CHECK(result.log.join('\n').contains(
      "refused Eileen (192.168.1.5:40003): another build: your game is build "
      "13ddd14d"));

  Hello later{3, "Eileen", Build, "s3cr3t"};
  result = hub.Receive(Eileen, Encode(later), 30);
  CHECK(!DecodeWelcome(SentTo(result, Eileen).value(0))->accepted);

  // A hello again (the game restarted on the same port) is welcomed again.
  result = hub.Receive(Kaniel, HelloFrom("Kaniel"), 40);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(hub.Members() == 2);
  // Phase 11: the link starts with the sign-in, before the co-op code says the
  // build. A hello with none is welcomed and holds nothing against the others;
  // the game says hello again once it knows its build, and that one counts.
  Hub fresh;
  result = fresh.Receive(Kaniel, HelloFrom("Kaniel", ""), 45);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(fresh.Build().isEmpty());
  result = fresh.Receive(Ada, HelloFrom("Ada"), 46);
  CHECK(DecodeWelcome(SentTo(result, Ada).value(0))->accepted);
  CHECK(fresh.Build() == Build);
  result = fresh.Receive(Kaniel, HelloFrom("Kaniel", OtherBuild), 47);
  CHECK(!DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  result = fresh.Receive(Kaniel, HelloFrom("Kaniel"), 48);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(fresh.Members() == 2);
  // A game of before phase 11 (link version 1, no secret) is a game of another
  // build.
  QByteArray old = QByteArray::fromHex("ffffffff") + "SHLK";
  old += char(1);
  old += char(1);
  old += char(3);
  old += "Old";
  old += char(Build.size());
  old += Build;
  result = hub.Receive(Eileen, old, 50);
  CHECK(!DecodeWelcome(SentTo(result, Eileen).value(0))->accepted);
  std::cout << "server link hello: ok\n";
  return 0;
}

// Build check on join: each field that differs, against the games here and
// the server's own build.
int BuildChecks() {
  // A setting that changes the game, against the games here.
  Hub hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  QByteArray harder = Build;
  harder.replace("harder=off", "harder=3,1.5,3");
  auto result = hub.Receive(Ada, HelloFrom("Ada", harder), 10);
  auto refused = DecodeWelcome(SentTo(result, Ada).value(0));
  CHECK(refused && !refused->accepted);
  CHECK(refused->text.endsWith(
      "\nanother build: your harder-enemies setting is HP x3 damage x1.5 "
      "poise x3, the games here use off: change it in the launcher's co-op "
      "settings"));
  CHECK(refused->text.startsWith(
      "Co-op refused: your harder enemies are HP x3 damage x1.5 poise x3"));
  CHECK(hub.Members() == 1);

  // What is only logged (the frame rate) does not refuse; the server says it.
  QByteArray faster = Build;
  faster.replace("fps=60", "fps=90 unlocked");
  result = hub.Receive(Ada, HelloFrom("Ada", faster), 20);
  CHECK(DecodeWelcome(SentTo(result, Ada).value(0))->accepted);
  CHECK(result.log.join('\n').contains(
      "Ada (127.0.0.1:40002) runs other settings than the games here (not "
      "checked: frame rate 90 unlocked, theirs 60)"));

  // A hello in the text before the fingerprint: an older release.
  result = hub.Receive(Eileen, HelloFrom("Eileen", ContractBuild), 30);
  refused = DecodeWelcome(SentTo(result, Eileen).value(0));
  CHECK(refused && !refused->accepted);
  CHECK(refused->text.startsWith("Co-op refused: your game is another "
                                 "release: update in the launcher\n"));
  CHECK(refused->text.contains(ContractBuild));

  // The server's own release: refused alone on the server, with nobody here.
  Hub server;
  server.SetServerBuild(
      "bb-build 1;version=1.2.0;revision=92829ac4;protocol=9;");
  QByteArray older = Build;
  older.replace("version=1.2.0", "version=1.1.9");
  result = server.Receive(Kaniel, HelloFrom("Kaniel", older), 0);
  refused = DecodeWelcome(SentTo(result, Kaniel).value(0));
  CHECK(refused && !refused->accepted);
  CHECK(refused->text ==
        "Co-op refused: your game is version 1.1.9, the server runs 1.2.0: "
        "update in the launcher\n"
        "another build: your game is version 1.1.9, the server runs 1.2.0: "
        "update in the launcher");
  CHECK(server.Members() == 0);
  result = server.Receive(Kaniel, HelloFrom("Kaniel"), 10);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  // The server does not know a game's settings or files.
  result = server.Receive(Ada, HelloFrom("Ada", harder), 20);
  CHECK(!DecodeWelcome(SentTo(result, Ada).value(0))->accepted);
  Hub alone;
  alone.SetServerBuild(
      "bb-build 1;version=1.2.0;revision=92829ac4;protocol=9;");
  result = alone.Receive(Ada, HelloFrom("Ada", harder), 30);
  CHECK(DecodeWelcome(SentTo(result, Ada).value(0))->accepted);
  // A server that does not check its own (a rig's, built apart): the games
  // here only.
  Hub apart;
  apart.SetServerBuild({});
  result = apart.Receive(Kaniel, HelloFrom("Kaniel", older), 0);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  std::cout << "server link build checks: ok\n";
  return 0;
}

// Phase 11: the hello is the sign-in. The player's id and its secret (the
// launcher made them) sign in, nothing else; the old calls go in the link's
// second stream (Rpc), from signed-in games only.
int SignsIn() {
  Hub hub;
  QList<QByteArray> asked;
  hub.SetSignIn([&](const QByteArray &online_id, const QByteArray &secret) {
    asked.append(online_id);
    if (online_id == "Squatter") {
      return SignInVerdict{false, "address taken: make a new id"};
    }
    return SignInVerdict{online_id == "Kaniel" && secret == "right", {}};
  });
  auto result = hub.Receive(Kaniel, HelloFrom("Kaniel", Build, "wrong"), 0);
  const auto refused = DecodeWelcome(SentTo(result, Kaniel).value(0));
  CHECK(refused && !refused->accepted && refused->text.contains("sign-in"));
  CHECK(hub.Members() == 0 && asked == QList<QByteArray>{"Kaniel"});
  result = hub.Receive(Kaniel, EncodeRpc("dropped"), 5);
  CHECK(result.rpc.isEmpty() && result.sends.isEmpty());

  result = hub.Receive(Kaniel, HelloFrom("Kaniel", Build, "right"), 10);
  CHECK(DecodeWelcome(SentTo(result, Kaniel).value(0))->accepted);
  CHECK(hub.Members() == 1);
  result = hub.Receive(Kaniel, EncodeRpc("body"), 20);
  CHECK(result.rpc.size() == 1 && result.rpc[0].from == Kaniel &&
        result.rpc[0].online_id == "Kaniel" && result.rpc[0].body == "body");
  CHECK(result.coop.isEmpty());
  // A refusal with its reason (a new account on another's address): the
  // game's player reads it.
  result = hub.Receive(Ada, HelloFrom("Squatter", Build, "any"), 30);
  const auto squatted = DecodeWelcome(SentTo(result, Ada).value(0));
  CHECK(squatted && !squatted->accepted &&
        squatted->text == "address taken: make a new id");
  // The address the games compute too (emulator net_link.h): never changes.
  CHECK(VirtualEndpointOf("p000000000048cf").ipv4 == 0x0ae08f32 &&
        VirtualEndpointOf("p00000000005b99").ipv4 == 0x0ae08f32 &&
        VirtualEndpointOf("Kaniel").port == VirtualPort);
  std::cout << "server link sign-in: ok\n";
  return 0;
}

int Relays() {
  Hub hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);

  auto result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 100);
  const auto forwarded = SentTo(result, Ada);
  CHECK(forwarded.size() == 1 && result.sends.size() == 1);
  const auto data = DecodeData(forwarded[0]);
  CHECK(data && data->peer == VirtualEndpointOf("Kaniel") &&
        data->frame == GameFrame);
  CHECK(!result.log.isEmpty() && result.log[0].contains("Kaniel") &&
        result.log[0].contains("Ada"));

  // Nobody at the destination: dropped, and said.
  result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Eileen"), GameFrame}), 110);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() && result.log[0].contains("no game"));

  // A game that never said hello is not relayed.
  result = hub.Receive(
      Eileen, Encode(Data{VirtualEndpointOf("Kaniel"), GameFrame}), 120);
  CHECK(result.sends.isEmpty());

  // Only the first frames of a pair are logged.
  Hub quiet_log(3000, 60000, 10000, 2);
  quiet_log.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  quiet_log.Receive(Ada, HelloFrom("Ada"), 0);
  int lines = 0;
  for (int index = 0; index < 5; ++index) {
    lines += static_cast<int>(
        quiet_log
            .Receive(Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}),
                     index)
            .log.size());
  }
  CHECK(lines == 2);

  // A ping comes back as a pong with the same body.
  result = hub.Receive(Ada, Encode(Kind::Ping, Ping{3, 777}), 130);
  const auto pong = DecodePing(Kind::Pong, SentTo(result, Ada).value(0));
  CHECK(pong && pong->seq == 3 && pong->sent_ms == 777);
  std::cout << "server link relay: ok\n";
  return 0;
}

// Phase 11: a game silent for 12 s (its game died, or its connection is down
// for longer than a blip) loses its sign-in session (`lost`, once a silence):
// what the TCP connection's end did at once when its game died. A blip under
// that rides out.
int ALongSilenceEndsTheSignIn() {
  Hub hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);
  for (qint64 at = 1000; at <= 30000; at += 1000) {
    hub.Receive(Ada, Encode(Kind::Ping, Ping{1, 1}), at);
    const auto result = hub.Tick(at + 1);
    if (at == 12000) {
      CHECK(result.lost.size() == 1 && result.lost[0] == Kaniel);
    } else {
      CHECK(result.lost.isEmpty());
    }
    if (at == 20000) {
      hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1}), at); // back
    }
  }
  CHECK(hub.Tick(32002).lost.size() == 1); // silent again since 20 s
  std::cout << "server link: a long silence ends the sign-in\n";
  return 0;
}

int Liveness() {
  Hub hub;
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1000}), 1000);

  auto result = hub.Tick(3000);
  CHECK(result.sends.isEmpty());

  // Ada's link quiet for longer than 3 s: Kaniel is told, once.
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{2, 3000}), 3000);
  result = hub.Tick(3500);
  const auto told = SentTo(result, Kaniel);
  CHECK(told.size() == 1 && SentTo(result, Ada).isEmpty());
  const auto quiet = DecodeMember(told[0]);
  CHECK(quiet && quiet->state == MemberState::Quiet &&
        quiet->endpoint == VirtualEndpointOf("Ada") &&
        quiet->online_id == "Ada" && quiet->gap_ms == 3500);
  CHECK(!result.log.isEmpty() && result.log[0].contains("Ada"));
  // Kaniel 2026-10-25: the world hears of it too (a game gone a few seconds
  // leaves its started team).
  CHECK(result.quiet.size() == 1 && result.quiet[0] == Ada);
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{3, 4000}), 4000);
  result = hub.Tick(4500);
  CHECK(SentTo(result, Kaniel).isEmpty() && result.quiet.isEmpty());

  // Heard again.
  result = hub.Receive(Ada, Encode(Kind::Ping, Ping{1, 6000}), 6000);
  const auto back = DecodeMember(SentTo(result, Kaniel).value(0));
  CHECK(back && back->state == MemberState::Back && back->gap_ms == 6000);
  CHECK(result.back.size() == 1 && result.back[0] == Ada);

  // Kaniel's game is gone after a minute without a word; the build is free
  // again once nobody is linked.
  hub.Receive(Ada, Encode(Kind::Ping, Ping{2, 64000}), 64000);
  result = hub.Tick(64001);
  CHECK(SentTo(result, Ada).size() == 1); // gone, not quiet first
  const auto gone = DecodeMember(SentTo(result, Ada).value(0));
  CHECK(gone && gone->state == MemberState::Gone &&
        gone->online_id == "Kaniel");
  CHECK(hub.Members() == 1);
  hub.Tick(200000);
  CHECK(hub.Members() == 0 && hub.Build().isEmpty());
  result = hub.Receive(Eileen, HelloFrom("Eileen", OtherBuild), 200001);
  CHECK(DecodeWelcome(SentTo(result, Eileen).value(0))->accepted);

  // The counters every 10 s.
  Hub counted;
  counted.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  counted.Receive(Ada, HelloFrom("Ada"), 0);
  counted.Receive(Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 1);
  counted.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1}), 9000);
  counted.Receive(Ada, Encode(Kind::Ping, Ping{1, 1}), 9000);
  CHECK(counted.Tick(9999).log.isEmpty());
  const auto counters = counted.Tick(10000).log;
  CHECK(counters.size() == 2);
  CHECK(counters.join('\n').contains("Kaniel") &&
        counters.join('\n').contains("Ada"));
  std::cout << "server link liveness: ok\n";
  return 0;
}

// Phase 7: the relay and the member notices stay inside the sender's team.
int TeamOnly() {
  Hub hub;
  const Endpoint Lone{0x7F000001, 40004};
  hub.SetPartyOf([&](const Endpoint &link) -> QString {
    if (link == Kaniel || link == Ada) {
      return QStringLiteral("team-a");
    }
    return link == Eileen ? QStringLiteral("team-b") : QString{};
  });
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);
  hub.Receive(Eileen, HelloFrom("Eileen"), 0);
  hub.Receive(Lone, HelloFrom("Lone"), 0);

  auto result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 10);
  CHECK(SentTo(result, Ada).size() == 1);
  // Another team's game, or one in no team yet: dropped, and said.
  result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Eileen"), GameFrame}), 20);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() && result.log[0].contains("another team"));
  result =
      hub.Receive(Lone, Encode(Data{VirtualEndpointOf("Ada"), GameFrame}), 30);
  CHECK(result.sends.isEmpty());
  result =
      hub.Receive(Ada, Encode(Data{VirtualEndpointOf("Lone"), GameFrame}), 40);
  CHECK(result.sends.isEmpty());

  // Eileen going quiet is told to nobody in team a.
  for (const Endpoint &game : {Kaniel, Ada, Lone}) {
    hub.Receive(game, Encode(Kind::Ping, Ping{1, 3000}), 3000);
  }
  result = hub.Tick(3500);
  CHECK(result.sends.isEmpty());
  // Ada going quiet is told to Kaniel only.
  for (const Endpoint &game : {Kaniel, Eileen, Lone}) {
    hub.Receive(game, Encode(Kind::Ping, Ping{2, 6000}), 6000);
  }
  result = hub.Tick(7000);
  CHECK(SentTo(result, Kaniel).size() == 1 && result.sends.size() == 1);
  std::cout << "server link team only: ok\n";
  return 0;
}

// Phase 7: no game learns another's address. Each game is known by a virtual
// address made from its account (10.x.y.z, one port), the same from any port
// it links from; the hub forwards between the virtual addresses.
int VirtualMembers() {
  Hub hub;
  const Endpoint ada_virtual = VirtualEndpointOf("Ada");
  const Endpoint kaniel_virtual = VirtualEndpointOf("Kaniel");
  CHECK((ada_virtual.ipv4 >> 24) == 10 && ada_virtual.port == VirtualPort);
  CHECK(!(ada_virtual == kaniel_virtual));
  CHECK(VirtualEndpointOf("Ada") == ada_virtual); // the same every time
  // The games compute the same (emulator/tests/network/test_net_link.cpp).
  CHECK(VirtualEndpointOf("Kaniel").ipv4 == 0x0A89C805u);
  CHECK(ada_virtual.ipv4 == 0x0AC3A55Bu);
  CHECK(!hub.VirtualOf(Ada)); // not linked yet
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  // A game welcomed is said with its virtual address: the rooms and signaling
  // hand that one out (no STUN needed).
  const auto linked = hub.Receive(Ada, HelloFrom("Ada"), 0).linked;
  CHECK(linked.size() == 1 && linked[0].online_id == "Ada" &&
        linked[0].virtual_endpoint == ada_virtual);
  CHECK(hub.VirtualOf(Ada) == ada_virtual);

  // Sent to Ada's virtual address: Ada gets it from Kaniel's virtual address.
  auto result = hub.Receive(Kaniel, Encode(Data{ada_virtual, GameFrame}), 10);
  const auto forwarded = DecodeData(SentTo(result, Ada).value(0));
  CHECK(forwarded && forwarded->peer == kaniel_virtual &&
        forwarded->frame == GameFrame);
  // Phase 9: a real address finds nobody, even a linked game's.
  result = hub.Receive(Kaniel, Encode(Data{Ada, GameFrame}), 15);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() && result.log[0].contains("no game"));

  // Ada's game links again from another port: still the same address.
  const Endpoint ada_moved{0x7F000001, 40099};
  // Its old link is forgotten (the world's member there goes), told to nobody:
  // Ada is still here.
  result = hub.Receive(ada_moved, HelloFrom("Ada"), 20);
  CHECK(result.gone.size() == 1 && result.gone[0] == Ada);
  CHECK(SentTo(result, Kaniel).isEmpty() && hub.Members() == 2);
  CHECK(hub.VirtualOf(ada_moved) == ada_virtual && !hub.VirtualOf(Ada));
  result = hub.Receive(Kaniel, Encode(Data{ada_virtual, GameFrame}), 30);
  CHECK(SentTo(result, ada_moved).size() == 1 && result.sends.size() == 1);

  // The notices name the virtual address too.
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 3000}), 3000);
  result = hub.Tick(3500);
  const auto quiet = DecodeMember(SentTo(result, Kaniel).value(0));
  CHECK(quiet && quiet->state == MemberState::Quiet &&
        quiet->endpoint == ada_virtual);
  std::cout << "server link virtual members: ok\n";
  return 0;
}

// Phase 9: the hub reads each game packet in a frame (SHAD GamePacketsType)
// and forwards only what the sender's routes let reach that member: a refused
// unreliable packet leaves the frame, a refused reliable one keeps its
// sequence number (coop/packet_routes.h). Other frames go as they are.
QByteArray GamePacketsFrame(const BloodborneCoop::GameFrame &frame) {
  QByteArray bytes = QByteArray::fromHex("ffffffff") + "SHAD" +
                     char(BloodborneCoop::GamePacketsType);
  const auto payload = BloodborneCoop::EncodeGameFrame(frame);
  bytes.append(reinterpret_cast<const char *>(payload.data()),
               static_cast<qsizetype>(payload.size()));
  return bytes;
}

std::vector<u8> PacketOn(u32 channel) {
  std::vector<u8> packet;
  BloodborneCoop::GameLinkDetail::Put(packet, channel);
  BloodborneCoop::GameLinkDetail::Put(packet, u32{2});
  packet.push_back(0xAB);
  packet.push_back(0xCD);
  return packet;
}

int RoutesGamePackets() {
  using BloodborneCoop::PacketClass;
  Hub hub;
  std::vector<std::tuple<Endpoint, Endpoint, u32>> asked;
  hub.SetPacketRoutes(
      [&](const Endpoint &from, const Endpoint &to, u32 channel) {
        asked.emplace_back(from, to, channel);
        return channel != 0x06 && channel != 0x31;
      });
  hub.Receive(Kaniel, HelloFrom("Kaniel"), 0);
  hub.Receive(Ada, HelloFrom("Ada"), 0);

  BloodborneCoop::GameFrame frame{"Kaniel", 5, 6, 2, {}};
  frame.entries.push_back({PacketClass::Unreliable, 0, PacketOn(0x06)});
  frame.entries.push_back({PacketClass::Unreliable, 0, PacketOn(0x04)});
  frame.entries.push_back({PacketClass::Reliable, 4, PacketOn(0x31)});
  frame.entries.push_back({PacketClass::Reliable, 5, PacketOn(0x10)});
  auto result = hub.Receive(
      Kaniel, Encode(Data{VirtualEndpointOf("Ada"), GamePacketsFrame(frame)}),
      10);
  CHECK(asked.size() == 4 && std::get<0>(asked[0]) == Kaniel &&
        std::get<1>(asked[0]) == Ada && std::get<2>(asked[0]) == 0x06);
  const auto data = DecodeData(SentTo(result, Ada).value(0));
  CHECK(data &&
        data->frame.startsWith(QByteArray::fromHex("ffffffff") + "SHAD" +
                               char(BloodborneCoop::GamePacketsType)));
  const auto routed = BloodborneCoop::DecodeGameFrame(
      {reinterpret_cast<const u8 *>(data->frame.constData()) + 9,
       static_cast<size_t>(data->frame.size() - 9)});
  CHECK(routed && routed->sender == "Kaniel" && routed->ack == 2 &&
        routed->entries.size() == 3);
  CHECK(routed->entries[0].packet == PacketOn(0x04));
  CHECK(routed->entries[1].seq == 4 &&
        routed->entries[1].packet == BloodborneCoop::DroppedNativePacket());
  CHECK(routed->entries[2].packet == PacketOn(0x10));

  // Nothing left but its ack: the frame still goes (the stream needs the ack).
  BloodborneCoop::GameFrame only_refused{"Kaniel", 5, 6, 3, {}};
  only_refused.entries.push_back({PacketClass::Unreliable, 0, PacketOn(0x06)});
  result = hub.Receive(
      Kaniel,
      Encode(Data{VirtualEndpointOf("Ada"), GamePacketsFrame(only_refused)}),
      20);
  CHECK(SentTo(result, Ada).size() == 1);

  // A frame that is not the game's packets is not read.
  asked.clear();
  result = hub.Receive(Kaniel,
                       Encode(Data{VirtualEndpointOf("Ada"), ::GameFrame}), 30);
  CHECK(asked.empty() &&
        DecodeData(SentTo(result, Ada).value(0))->frame == ::GameFrame);
  // A game frame that does not read is dropped, and said.
  result = hub.Receive(
      Kaniel,
      Encode(Data{VirtualEndpointOf("Ada"),
                  QByteArray::fromHex("ffffffff") + "SHAD" +
                      char(BloodborneCoop::GamePacketsType) + "\x01"}),
      40);
  CHECK(result.sends.isEmpty());
  CHECK(!result.log.isEmpty() &&
        result.log.join('\n').contains("does not read"));

  // The counters say how much each sender had refused, per channel.
  hub.Receive(Kaniel, Encode(Kind::Ping, Ping{1, 1}), 9000);
  hub.Receive(Ada, Encode(Kind::Ping, Ping{1, 1}), 9000);
  const QString counters = hub.Tick(10000).log.join('\n');
  CHECK(counters.contains("refused") && counters.contains("0x6: 2") &&
        counters.contains("0x31: 1"));
  std::cout << "server link routes game packets: ok\n";
  return 0;
}
// Invasions (docs/INVASIONS.md): an invader visiting team a shares its relay
// (and its quiet notices), but only the game's packets go between them, never
// our co-op frames; it still reaches no other team.
int AnInvaderSharesTheVisitedTeamsRelay() {
  Hub hub;
  const Endpoint Red{0x7F000001, 40005};
  hub.SetPartyOf([&](const Endpoint &link) -> QString {
    if (link == Kaniel || link == Ada) {
      return QStringLiteral("team-a");
    }
    return link == Eileen ? QStringLiteral("team-b") : QString{};
  });
  hub.SetVisiting([&](const Endpoint &link) -> QString {
    return link == Red ? QStringLiteral("team-a") : QString{};
  });
  hub.SetPacketRoutes(
      [](const Endpoint &, const Endpoint &, u32) { return true; });
  for (const auto &[game, name] :
       {std::pair{Kaniel, "Kaniel"}, std::pair{Ada, "Ada"},
        std::pair{Eileen, "Eileen"}, std::pair{Red, "Red"}}) {
    hub.Receive(game, HelloFrom(name), 0);
  }
  BloodborneCoop::GameFrame frame{"Red", 5, 6, 2, {}};
  frame.entries.push_back(
      {BloodborneCoop::PacketClass::Unreliable, 0, PacketOn(0x06)});
  auto result = hub.Receive(
      Red, Encode(Data{VirtualEndpointOf("Kaniel"), GamePacketsFrame(frame)}),
      10);
  CHECK(SentTo(result, Kaniel).size() == 1);
  result = hub.Receive(
      Ada, Encode(Data{VirtualEndpointOf("Red"), GamePacketsFrame(frame)}), 20);
  CHECK(SentTo(result, Red).size() == 1);
  // Our own frames (another SHAD type) never, either way.
  const QByteArray coop =
      QByteArray::fromHex("ffffffff") + "SHAD" + char(0x41) + "vitals";
  result =
      hub.Receive(Red, Encode(Data{VirtualEndpointOf("Kaniel"), coop}), 30);
  CHECK(result.sends.isEmpty());
  result =
      hub.Receive(Kaniel, Encode(Data{VirtualEndpointOf("Red"), coop}), 40);
  CHECK(result.sends.isEmpty());
  // Team a among itself still carries them; team b never hears the invader.
  result =
      hub.Receive(Kaniel, Encode(Data{VirtualEndpointOf("Ada"), coop}), 50);
  CHECK(SentTo(result, Ada).size() == 1);
  result = hub.Receive(
      Red, Encode(Data{VirtualEndpointOf("Eileen"), GamePacketsFrame(frame)}),
      60);
  CHECK(result.sends.isEmpty());
  std::cout << "server link invader relay: ok\n";
  return 0;
}

// Two PCs with one player id (a copied install, player report 2026-10-24): the
// second one's hello no longer takes the link from the first while that one is
// still heard.
int TwoPCsOneId() {
  Hub hub;
  const Endpoint home{0x1A0BABBA, 59881};
  const Endpoint friends_pc{0x1AFE174E, 50084};
  auto result = hub.Receive(home, HelloFrom("Eloise"), 0);
  CHECK(DecodeWelcome(SentTo(result, home).value(0))->accepted);
  result = hub.Receive(friends_pc, HelloFrom("Eloise"), 1000);
  const auto refused = DecodeWelcome(SentTo(result, friends_pc).value(0));
  CHECK(refused && !refused->accepted && refused->text.contains("another PC") &&
        refused->text.contains("coop.json"));
  CHECK(result.gone.isEmpty() && hub.Members() == 1 && hub.VirtualOf(home) &&
        !hub.VirtualOf(friends_pc));
  // Its link still works.
  hub.Receive(home, Encode(Kind::Ping, Ping{1, 2500}), 2500);
  result = hub.Receive(friends_pc, HelloFrom("Eloise"), 5000);
  CHECK(!DecodeWelcome(SentTo(result, friends_pc).value(0))->accepted);
  // The first PC gone quiet (its game closed, or its connection moved): the id
  // links from there.
  result = hub.Receive(friends_pc, HelloFrom("Eloise"), 5600);
  CHECK(DecodeWelcome(SentTo(result, friends_pc).value(0))->accepted);
  CHECK(result.gone.size() == 1 && result.gone[0] == home &&
        hub.Members() == 1);
  std::cout << "server link one id on two PCs: ok\n";
  return 0;
}
} // namespace

int main() {
  if (Contracts() != 0 || HelloAndBuilds() != 0 || BuildChecks() != 0 ||
      Relays() != 0 || Liveness() != 0 || TeamOnly() != 0 ||
      VirtualMembers() != 0 || RoutesGamePackets() != 0 || SignsIn() != 0 ||
      ALongSilenceEndsTheSignIn() != 0 || TwoPCsOneId() != 0 ||
      AnInvaderSharesTheVisitedTeamsRelay() != 0) {
    return 1;
  }
  return 0;
}
