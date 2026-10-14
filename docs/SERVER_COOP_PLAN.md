# Co-op on our own networking: the plan

Agreed with Kaniel on 2026-10-13, after the native netcode research (docs/NATIVE_NETCODE.md).
Bloodborne's own netcode assumes one host's world and peer-to-peer links; we replace that with a
server that owns the party's world and carries all traffic, a team lobby in the game's own menus,
and one UDP protocol. Each phase ships on its own, behind a switch until rig-checked, on the port
only (bbport is the target; every player and the server run one build).

## Decisions

| Question | Decision | Why |
|---|---|---|
| Peer to peer or server? | **Server.** Every game keeps one UDP connection to it; nothing goes game to game. | Players reach each other over a VPN anyway, so P2P's only advantage (NAT) is gone. The world survives the host, one place orders every change, one link per game instead of a mesh. Cost: guest-to-guest packets take one extra hop (milliseconds on a VPN). |
| Where is the world saved? | **On the server, one world per team;** each save keeps its own personal world for solo play. | See "Team worlds". |
| Who owns the world? | **The server owns the world's state;** the games still play it. | The server records and orders kills, flags, doors, lamps, shortcuts, items, enemy HP and map state, keeps them across sessions and crashes, and hands them to joiners. It runs no game: enemies' AI runs in one game per map (chosen by the server), each player's character in its own game. Saves stay on each PC; the server's world is applied when a game loads. |
| Do we rewrite movement sync? | **No.** The game's own player and enemy sync (channel 6 behavior graph, ch 4/0x24/0x36 enemy movement) keeps running; its packets just travel through the server. | Writing our own animation sync would be a large project for no gain. |
| Which wire format? | **UDP to the server.** Our messages in **protobuf**; the game's packets as raw bytes behind a small fixed header. | shadNet already uses protobuf; a schema ends hand-written frames breaking older readers. Game packets are many per second and the server never reads them. |
| What stays HTTP? | Only what the game itself speaks: backend check-in, messages, bloodstains, ghosts, summon signs for anyone outside a team. | It is the game's client; leaving it costs nothing. The co-op boards all go. |
| How do players team up? | **A lobby in the game's own menus,** from the title screen. Form the team, Start, and everyone is in one session when they load, with no bell or signs. | Kaniel: native UI. Replaces the party name/password in settings. |

## Team worlds (Kaniel 2026-10-13)

- **The save splits in two.** The character (level, stats, gear, inventory, Insight, echoes,
  appearance, NG cycle) stays in each player's save on their PC. The world (bosses beaten, lamps,
  doors, levers, shortcuts, items picked up, NPC and quest states, kills, map state) of a team lives
  on the server, one world per team. The flag table (`bloodborne_flags.h`, docs/FLAGS.md) already
  says which flags are the world's and which the character's; the split follows it.
- **The server stores the worlds itself** (its own world store on disk, atomic writes and backups):
  every accepted world log entry is written at once, so a crash, a quit or a host change loses
  nothing, and any member can be the game-level host.
- **A new team world starts from the leader's progress:** at team creation the leader's save's world
  part is uploaded and becomes the team world.
- **Every save keeps its personal world.** Players play several saves, solo and in different teams:
  loading into a team world stores the save's own world part aside (in the save folder) and applies
  the team world; leaving the team (back to the title, or playing solo) puts the personal world
  back. The character part is never touched by this.
- **Bosses:** inside a team world only the team world counts; bosses beaten solo stay in that
  save's personal world (this replaces the "separate boss progress" rule for teams).
- **NPC gifts are per player** (Kaniel, 2026-10-13, "Networking stack question" thread): an NPC's
  story and position are the world's, but each player's "gift received" flags are the character's,
  so every player talks to the NPC and gets their own gift. The flag split lists them on the
  character side.
- **NG cycle:** the team world has its own cycle. Starting NG+ is the team's choice, made by the
  leader at the final boss; a character joining plays at the team's cycle.
- **Team size:** up to 4 (the game's remote player slots).
- **Chalice dungeons:** shared too, as team worlds (dungeon progress and glyphs per team). Never
  tested on our version yet: a rig check of plain co-op in a dungeon comes first.
- **Server down:** team play needs the server; solo play with the personal world needs nothing, so
  nothing extra is built for it.
- **Port only:** the new design is built for the port; shadPS4 keeps the old path until it is dropped.
  No mixed parties.
- Open points for the design: what the character keeps from team play when it goes solo (items and
  levels yes; key items tied to world progress, e.g. a boss's key, to be listed), and several teams
  per character (each its own world on the server).

## Architecture

```
 game (port) ──┐                       ┌── game (port)
  hooks at the │  one UDP connection    │  hooks at the
  packet choke │  per game: our proto-  │  packet choke
  point        │  buf messages + game   │  point
               ▼  packets (raw)         ▼
            ┌─────────────── server (shadNet) ───────────────┐
            │ accounts, presence, teams (lobby)               │
            │ session: members, places, enemy owner per map   │
            │ world log: ordered entries, snapshot per map,   │
            │   stored on disk (survives everything)          │
            │ relay: game packets to the right members        │
            └─────────────────────────────────────────────────┘
```

### Traffic and who owns it
| Traffic | Through the server | Server's role |
|---|---|---|
| Team and session: lobby, members, who runs which map's enemies | yes | owns |
| World state: kills, flags, doors/levers, lamps, shortcuts, items, enemy HP, map state, echoes held by enemies | yes | owns: orders, stores, settles conflicts, snapshots for joiners |
| Player movement, animation, HP (game ch 6) | yes | relays unread to members on the same map |
| Player hits (0x14), throws (0x12/0x37/0x30), bullets (0x3E/0x40), forced animations (0x24/0x25), character data (8, 0x0A-0x0D, 0x13, 0x3B, 0x3C, 0x43) | yes | relays; drops what is meant for members on other maps |
| Enemy movement and state from the map's owner (4, 0x29, 0x36, 0x3D, 0x41, 0x34) | yes | relays to the others on that map |
| Enemy damage (0x15) and HP (5) | yes | replaced by world log HP entries (proposed by the games, ordered by the server) |
| Event sync (0x0E Lua calls, 0x31 EMEVD instances, 0x0F CompleteEvent) | yes | relays on the same map; flags they set are proposed to the world log |
| Objects (0x2D, 0x2E, 0x2F, 0x2A, 0x2B) | yes | relays on the same map; the resulting state goes to the world log (0x2F's host correction is dropped) |
| Join snapshots (0x10 flags, 0x11 decals/objects/dead enemies, 0x3F bullets) and handshake (7, 0x19, 0x42) | yes | handshake relayed; snapshots built from the world log for any map, not just the host's current one |
| Boss "thanks" kick (0x27), map item sync (0x2C), Insight transfer (0x39) | | dropped (already neutralised today) or replaced by the world log |

## The protocol

- **One UDP socket per game** to the server (the shadNet host, a fixed port next to the TCP one).
  Each datagram holds messages; each message has a 1-byte kind and a length.
- **Kinds:** `Game` (fixed header {channel u8, reliable bit, sender member u8, map u32, visit u16} +
  the game's packet untouched), `Proto` (one protobuf message, see below), `Ack`, `Ping`.
- **Reliability layer (ours, built once):** per member and per class, sequence numbers, acks,
  resends and in-order delivery for reliable traffic (the game's channels 7 and up, world log
  entries, snapshots); unreliable traffic (game channels 0-6, movement) is never resent and the
  newest wins. Liveness (who is connected, outages, timeouts) comes from this layer, not from the
  game's own timers.
- **Protobuf messages** (`server/proto/coop.proto`, shared by the game and the server): `Hello`
  (account, build, protocol), lobby (`TeamCreate`, `Invite`, `InviteAnswer`, `TeamState`, `Ready`,
  `Start`), session (`Place` {map, visit, state}, `MemberJoined/Left`, `EnemyOwner` {map, member}),
  world log (`Propose` {kills, flags, HP, objects, pickups}, `Entries` {seq, epoch, entries},
  `SnapshotRequest`, `Snapshot` {map, parts}), vitals and party death, portraits.
- **Versioning:** the `Hello` carries the build; a different build is refused at the lobby with a
  clear message (one build for all, no fallbacks).

## Hooks in the game (from the netcode research)

| Hook | Address | Use |
|---|---|---|
| Packet send | 0x017894E0 (+ 0x01789A40 "to host") | every game packet: policy table, then out through our socket |
| Packet receive | fill path 0x0178D510 -> 0x01790C20 (per-member queues), pops 0x01789200 / 0x017893E0 | inject packets that came from the server into the game's queues |
| Resend and idle timer | 0x01ED0B20 (+0x2E0/+0x2F0, resend queue +0x188) | kept quiet: our layer decides liveness |
| Session events | 0x0178B5E0 (vtable 0x0532C090 slot 0) | feed member joined/left from the server's session |
| Summon path | 0x01872360, 0x01875320, 0x01788ED0 (CreateSession), 0x01789010 (JoinSession) | team join without signs |
| Join snapshots | 0x01786050 (0x10), 0x01786360 (0x11), 0x01A04190 (0x3F), apply 0x013BECA0 | built from the world log |
| Enemy authority | NetWorldChrSync WorldChrMan+0xA40 (ownership words), body 0x018ECEB0, guest hit 0x01A29E40 | owner per map from the server |
| NP emulation (our code) | np_matching2*, np_signaling, p2p_sockets | rooms and links answered from the server's session |

Every hook goes in `bloodborne_patch_sites.h` with its byte check.

## The team lobby (native UI)

- **Where:** a "Co-op Party" item in the title menu, drawn with the game's own menu dialogs and
  fonts (no overlay), in the same way the party HUD patches the game's own movies at runtime from the
  player's files (never shipped).
- **Flow:**
  1. Co-op Party: the game signs in to the server with the account from setup (no typing).
  2. The list shows the team (if any) and the friends online (shadNet presence), with each one's
     state: in a team, in the lobby, playing.
  3. Create a team, or invite a friend from the list; the friend sees the invite and accepts.
     A member can leave; the leader can remove one. Up to 4 (the game's remote slots).
  4. Each member picks the character (the save slot) to play; members mark Ready.
  5. The leader presses Start: every member's game loads its save. The first to load is the
     session's host at the game level (the server still owns the world); the others join its
     session the moment they finish loading, no bell, no signs.
  6. The team lives on the server: after a crash or a quit, starting the game again and choosing
     Continue in Co-op Party rejoins it where it is.
- **What the game offers** (static RE, details in docs/native-netcode/title-menu.md):
  - The live menus are Scaleform dialogs (`menu:/<name>.gfx`) opened with OpenDialog 0x01C1CDF0;
    the `FrpgMenuDlg*` classes are dead code. Clips are found with 0x01ADB220, the same lookup the
    party HUD bridge uses (SetText slots 0x130/0x138, GotoAndPlay 0x158), so the HUD's way of
    driving Scaleform carries over.
  - **Title menu** built by 0x01B4A3D0: each row is a label (FMG 200/201 id), a help line and a
    handler functor added with 0x01B4C9A0, bound to the `CommandList` clip by 0x01BEA820. Rows:
    Continue 401301, Load Game 401302, New Game 401303, System 401304, Information 401305,
    Log In 401306, Opinion Box 401307 (dormant).
  - **The "Co-op Party" row:** reuse the dormant Opinion Box row (handler 0x01B46030; its gate
    0x01AE9AC0 returns 1; our label through the message lookup 0xF6F340), or add our own row with
    0x01B4C9A0 before the bind at 0x01B4AD45.
  - **Holding the start:** Continue and Load Game build their start job with 0x01B3BFE0; wrapping it
    in a job that waits for the team's Start keeps the title menu on screen. (TitleStep's done
    handler 0x0195CB70 is the hand-off to state 6, load the save + network.)
  - **Dialogs:** native message boxes OK 0x01C1F500, OK/Cancel 0x01C1EB40, Yes/No 0x01C1ED10
    (result 100/200), chained with 0x01C1A070, taking plain text; the generic item list dialog
    (the session kick list "kickout", 0x01BC2A50, vtable 0x0539A880, 4 rows) and the scrolling
    `commandList` movie for the roster and the friends list.
  - **Text input:** the game's on-screen keyboard (sceImeDialog, already in bbport's runtime) for a
    team name, if one is wanted.
- **Screens:** Co-op Party (team members with state, Invite, Ready, Start, Leave) built on the item
  list dialog; Invite (friends online) on the same list; invites and errors (another build, server
  down) as native message boxes; a status line in the title dialog's own text lines (ServerVersion,
  Info, ProgressInfo, set by 0x01B38820).
- **Unknown until tried in the game:** whether the title's CommandList scrolls past 5 rows, the
  string object's layout for building text without a message id, the item list's row record.

## Phases

Each phase: red-green tests first, all CI checks, rig scenarios named below, merged on its own.

0. **Trace.** Hook send/receive, write every game packet (channel, size, member, place, NetSync
   state) to the coop trace; no behavior change. Rig: join, trip, hit3, boss-kill. Settles the
   unknown channels (0x34, 0x43, the 0x60/0x14 enemy records) and measures traffic per channel.
   **Done 2026-10-13** (SHADPS4_BLOODBORNE_PACKET_TRACE, results in NATIVE_NETCODE.md "Phase 0
   trace"): all settled but 0x43.
1. **Channel policy table.** One table at the choke point, `{channel -> pass | same-map only | ours
   | drop}`, read on send and receive; retires the scattered hooks it covers (map item header, the
   0x27 kick, 0x2F corrections). Rig: the full suite.
   **Built 2026-10-13** behind `SHADPS4_BLOODBORNE_CHANNEL_TABLE=1` (port only, off by default):
   `ChannelPolicy` / `NativeChannelPolicy` in `bloodborne_party.h`, hooks on the send's entry
   0x017894E0 (Ours/Drop answer "not sent") and on both receive paths' channel checks in
   0x01790C20 (0x01790C73 unreliable, 0x01790DDE reliable: a dropped packet gets channel 0x44 and
   the game's own `ja` frees it). SameMap is decided by the receiver, from the sender's map on the
   map board (unknown or loading: passed), so the transport still carries every packet. It replaces
   the map item header hook; the 0x27 kick and 0x2F corrections had no packet hook of their own
   (the boss clear patches stay: they also stop the host's own leave), so the table now drops them.
2. **One UDP link to the server.** The socket, our reliability layer, `Hello`/`Ping`, and the game's
   packets relayed by the server instead of going P2P (the game still forms its rooms; only the
   bytes take the new path). Rig: join x25 (the join stall), host-crash, crash-rejoin, wipe3, the
   ping rig (50-100 ms).
   **2a built 2026-10-13** behind `SHADPS4_BLOODBORNE_SERVER_LINK=1` (port only, off by default):
   the P2P transport (p2p_sockets.cpp) sends every frame for a peer to shadNet's matching UDP port
   (the STUN pings' socket) wrapped as a link datagram (`net_link.h`, shadNet `server_link.h`,
   one of each in server/tests/contracts/server_link_frames.txt); shadNet forwards it to the game
   at that address and names the sender by the address its STUN pings come from, which is the
   address the peers already know it by. So signaling, NP emulation and the game see the same
   peers, and no virtual addresses are needed. `Hello` (link version, online id, build; shadNet
   takes one build at a time and refuses others), `Ping`/`Pong` once a second (round trip and
   outages in the game's log and on the co-op trace, "link" records), per-member liveness on
   shadNet (quiet after 3 s, gone after 60 s, told to the other members), the first 20 frames
   between each pair and counters every 10 s in shadNet's log. `SHADPS4_P2P_DELAY_MS` still holds
   every send. Rig: join 25/25 through the server. The join stall still happens (once in
   crash-rejoin-hp): every frame of the failed join, the host's 271-byte one included, reached
   the guest through the server and was acked, and the guest's game never called JoinRoom; the
   stall is in the game's matching/join, not in signaling or UDP.
   **2b built 2026-10-13**, on with the server link (`SHADPS4_BLOODBORNE_GAME_PACKETS=native`
   keeps the game's own transport under it): the game's packets leave at the transport send
   under NetSync's send and its resend (0x00C8F0A0, a hook that answers "sent") and travel in our
   frames (SHAD 0x40, `bloodborne_game_link.h`): per member and class, reliable ones (channels 7
   and up) with seq/ack/resend (150 ms) and in-order delivery, unreliable ones never resent and
   the newest per channel wins; an instance number per run restarts both streams when a member's
   game restarts (a restarted game ignores frames meant for its predecessor and answers at once;
   an idle link sends an empty frame every second). The receiver hands them to the member's queues
   from the link's own thread, as the game's receive does from its network thread, with the game's
   own fill 0x01790C20 (it locks the queues), so the channel table's receive checks still run. A
   first version delivered from a hook on NetSync's per-frame update (0x01789B60) and saw 5 join
   stalls in about 28 joins (none in about 60 on P2P and 2a); without a per-frame hook, none in 21
   joins and 4 crash-rejoins. The SprjSessionManager timer +0x2E0/+0x2F0 turned out to count only a stuck resend
   backlog (180 s), and the resend queue fills only when the transport send fails, so answering
   "sent" keeps both quiet: OnIrregularLeaveSession can no longer fire from them. Liveness and
   counters per member in the log and on the co-op trace ("glink"). A packet for a member whose
   address is not known yet goes the game's own way (a handful per join). Hooks can now choose the
   result of a skipped function (`InstallGuestFunctionResult`). Not done: the server applying the
   channel table (frames batch packets, and the server never reads them).
3. **The world log moves into the server.** The host side (`WorldLogHost`) runs in shadNet, stores
   to disk, and serves snapshots per map; members propose to the server. Kills, shown, flags, map
   state and HP go over it; then the HTTP boards (/bloodborne/party/*) and their pollers are removed.
   Protobuf messages from here on. Rig: kill-apart, hp-apart, lamp-share, echoes-apart, host-crash.
   **3a built 2026-10-13** behind `SHADPS4_BLOODBORNE_SERVER_WORLD=1` (port only, needs
   `SHADPS4_BLOODBORNE_SERVER_LINK=1`; off by default): the world log's logic and frames moved into
   `server/coop/` (`world_log.h`, the game link's `game_link.h`), which the emulator, the port and
   shadNet all compile; `server/coop/coop.proto` is the one file for our messages (and shadNet's
   `shadnet.proto` is now one copy too: the games build theirs from `server/src`). The messages
   travel in the link's new `Coop` kind, in a reliable stream of their own (`coop_stream.h`: the
   game link's reliable class, messages chunked). shadNet (`bloodborne_world.h`) keeps one world
   per party key (on disk by its hash, `BloodborneWorldPath`): `world.pb` written whole with
   `QSaveFile`, three backups, and a journal of the changes since, loaded at its start. Its epochs
   are the world's id and its count of logs; a wipe starts a log once per party (15 s), the
   session host's join, leave or new character starts one, a guest's take its share out. Every
   game (the host too) proposes its standing kills and follows the server's log
   (`bloodborne_server_world.cpp`, its own thread); the kill board is not posted. The rest (shown,
   flags, map state, HP) stays on the host's log for 3b. Rig: `server-restart` (shadNet restarts
   mid-session, the first kill stands and a second one comes through), kill-apart, dog.
   **3b built 2026-10-13**: the host's log no longer runs with the server world. The session host
   reports its characters (shown bits), its map's state and its return point (`ShownReport`,
   `MapStateReport`, `ReturnPointReport`; a guest's count for nothing: its hello says who hosts),
   every member its shared flag events (`FlagProposal`, the FlagTable's FlagsToShare only: per
   player NPC gift flags never go) and its HP report (`HpProposal`); shadNet orders them into the
   party's log, journals them and puts them in its snapshots. The direct damage frames stay as the
   fast copy (phase 4 retires them). Rig: story-hidden, lamp-share, hp-apart, hit3, iosefka-gift,
   echoes-apart.
   **3c built 2026-10-13**: the HTTP boards are gone. Each member sends its own part of them over
   the stream (`MemberMap`, `Rest`, `Trophies`, `Echoes`, `Vitals`, `Portrait`: the portrait whole
   in one message instead of frame 0x3E's parts) and shadNet sends the party the whole board when
   it changes (`bloodborne_party_boards.h`: the HTTP boards' rules, a map's wipe declared once,
   a part not heard of for 5 s dropped, each member's last portrait kept for the ones that come
   later). Deleted: the board clients in `bloodborne_boards.cpp`, the `/bloodborne/party/*` routes
   (the summon routes stay: the game's own HTTP), `bloodborne_party_rest.{h,cpp}` and the boards'
   contract file with its tests on both sides (a fuzz test of the world service replaces the
   boards' one). With them the server link and the server world are on by default in the port
   (`=0` switches either off, for tests; there are no boards then). The shadPS4 emulator has no
   server link, so it has no boards any more. Rig: down-revive, wipe, lamp-share, echoes-apart,
   give-up, down-revive3, join3, kill-apart.
4. **Enemy ownership from the server.** One owner per map (the first member there, handed on when it
   leaves); game enemy channels gated by it; own-hit counting, SharedDamage and the HP board retired.
   Rig: hit3, guest-hit-once, hp-together, lone-guest, dog.
   **Built 2026-10-14** behind `SHADPS4_BLOODBORNE_ENEMY_OWNER=1` (port only, with the server
   world; it turns the channel table on): shadNet names one owner per occupied **map** (a block,
   not an area: the game wakes a block's enemies only near a player in that world, so a guest in
   Cathedral Ward following the host in Central Yharnam saw them stand still), `EnemyOwners`
   {map, member, epoch} in the Coop stream, sent on a change and with every answer to a member's
   map (games now send map 0 in a load). The owner is the session's host while it is on the map
   (a host that followed a guest put its own ownership words back every frame), else the member
   there first; a load, a move away or 2.5 s of silence hands it on to the member there longest.
   For games that run the owners (hello `enemy_owner`), shadNet takes new kills and HP reports on
   a map only from its owner (kills made there before stand; a map nobody runs takes anyone's).
   The game (`EnemyRole`, bloodborne_party.h): owner words 0 / follower 0xFFFF (a host that owns
   keeps the game's own); the HP sync's host-only checks read 3 on the owner and 6 on a follower
   (hooks 0x018EF26C, 0x018EF578/0x018EF587/0x018EF684); a follower reads ch 5 from the owner's
   member (0x01789495 in the receive from the host); a follower's 0x15 goes to the owner over our
   link, the owner's own is dropped; ch 4/5/0x29/0x36 only from and to the owner, 0x16/0x17
   dropped; the owner reports its hurt enemies to the log as it reads them (at once when another
   member's hit arrives), a new owner's copies take the log's HP for 5 s and a follower's always
   (the game applies the owner's ch 5 only in one world, not after a trip; lower only, so a
   follower's own hit counts once), on the next frame after the server's entries come; nobody
   sends the direct damage frames. hit3: the others' bars 0.15 s behind a hit. The owner logs its frames and sends per second
   every 10 s. Not as planned: the server does not filter the game's packets (the reliable ones
   travel in our seq'd frames, so dropping one there breaks the stream); the receiver gates them.
   0x34/0x3D/0x41 pass as they are (the game sends its own hand-over when the words change).
   Rig: owner-leaves, owner-crash, border-fight (new) and the list above.
5. **Team lobby in the game's menus.** Server teams and presence, the title menu item and dialogs,
   Start, and the join from the team (session events and rooms from the server's session; join
   snapshots from the world log). Rig: new verbs for the lobby, join, join3, join-apart, resume.
   **5a + 5c built 2026-10-14** behind `SHADPS4_BLOODBORNE_TEAMS=1` (port, with the server world;
   the party settings are not read then):
   - **Server** (`server/coop/team_lobby.h`, in shadNet's world service): players sign in with a
     `LobbyHello` on the Coop stream before any party hello; teams of up to 4 with a leader,
     invites, Ready (with the save slot), Start; every signed-in player gets its `LobbyState`
     after any change, a refused request comes back with a notice. The teams and invites are kept
     in `teams.pb` beside the worlds (they outlive the server); a team is started while one of
     its members plays, and a member that readies then goes at once (a crash and Continue rejoin).
     A team's party key is `team:<id>`, so its world is the server world of phase 3.
   - **Game** (`bloodborne_team.h`, `bloodborne_server_world.cpp`): the party controller waits
     for its role. TitleStep's load state (0x0195B480, called every frame until it moves on) is
     skipped while the team decides: a member's Continue or Load Game is its Ready, the leader's
     its Ready and the team's Start; the Start sets the party key and the role (**the leader
     hosts**, not the first to load: one fixed host per team, as the party roles were). Not in a
     team, or no word from the lobby in 5 s: the save loads alone. The join under it is still the
     automatic bell and signs of the party (phase 6 replaces it).
   - **Native page** (`bbport_game_menu.cpp`, `bbcoop_lobby_view`): a Co-op Team page in the
     System menu, also on the title: Team (create / leave), Invite (players online), Invitations
     (join / decline), Members (leader, ready, playing, at the title, offline), Remove (leader),
     Start (how Continue starts the team). Each row is the game's own pop-up list; a choice is a
     request at once, and the page shows the result when it opens again.
   - Rig: title verbs `team-create`, `team-invite <online id>`, `team-accept`, `team-wait <n>`,
     `title-press <button> [ms]`, `title-wait <s>`, `title-shot <name>`; scenarios `team-join`,
     `team-join3`, `team-menu` (drawn: invite and accept through the pages, with pictures).
   - A save loaded alone (not in a team, or after a team's session) rings nothing and plays no
     team's world (the controller waits again); the party's hooks stay in until the game restarts.
   - The pop-up lists draw over the rows below them.
   - **The team screen** (Kaniel 2026-10-14: "if you click continue or new game you instead sent
     to it", a modern team lobby in the game's style; built after the page above): Continue, Load
     Game or New Game now open it while the load is held, and nothing loads by itself any more
     (`TeamGate::Choose`). Drawn by the port's overlay (`bbport_overlay.cpp` `Team::Screen`, the
     menu's colours and ornaments, the game's serif): four hunters' places (emblem, name, state:
     choosing, ready, in the hunt, away; the leader marked), the hunters online with Invite, the
     invitations with Join and Decline, and Create / Leave the team, Ready / Not ready (a member)
     or Begin the hunt (the leader: its Ready and the Start), Play alone. It takes the pad and the
     keyboard (the arrows / d-pad move over the buttons, Cross / Enter presses, the mouse works);
     Insert and L3 + R3 still open the settings. Rig: `team-screen` (shows it), `team-ready`,
     `team-start`, `team-alone` press its buttons once it is up (the title's Cross until then);
     scenario `team-screen` (drawn, pictures). Placeholder emblems, no portraits yet (the game has
     drawn no character at the title).

   **5b built 2026-10-14** (with the team lobby): a team's world is its event flags that are the
   world's (FlagOwner Host and Shared in the flag table; a character's own flags, NPC gifts among
   them, never go in), `TeamWorld` {on flags, blocks, version}.
   - **Made by the host.** A team with no world yet takes its host's: the host's game reports
     its world flags every 10 s when they changed (`TickTeamWorld`), and the server keeps the
     latest whole in `team_world.pb` in the team's world folder, sent to every member after its
     party hello (sent at the load's release, so it comes back while the save loads) and when it
     changes. Only the team's host's report counts. So the team's world starts from the leader's
     save, and the team's progress is what the host's world has (the guests' shared flags reach
     it the way phase 3 has them).
   - **Taken by every save as it loads** (`ApplyTeamWorldOnLoad`, from the map items' creation
     hook, before the map's events): each flag block the load brings that the team's world
     covers and no earlier load this session took gets the team's values (only the flags that
     differ are set, with the game's SetEventFlag); later loads leave a taken block alone, so the
     session's own changes stay.
   - **The save's own world aside.** Before a block takes the team's values, its world flags go
     to `user\bloodborne_party_personal_world_slot<N>.txt` (blocks already in the file keep what it
     says: a save that played a team before). A save loaded alone gets them back block by block
     as its loads bring them, and the file goes when all are back.
   - Rig: `team-world` (host's flag in the team's world, the guest's save takes it in the next
     session and loses it again alone), title verb `team-leave`.
   - Not yet: the NG cycle per team (the leader's own), chalice dungeons, several teams per save
     share the one file of the save's own world (it is the save's world from before any team).
6. **Clean up.** Remove signs/bell auto-ringing for teams, the signaling handshake, the P2P vport
   transport, superseded patch sites and boards; update the kit, README and release tooling.
   **Done 2026-10-14** (PRs #108, #109 and the cleanup after them), what changed against the plan:
   - **No switches.** The port always runs the server link, the server world, the channel table,
     the enemy owners and teams; `SHADPS4_BLOODBORNE_SERVER_LINK`, `_SERVER_WORLD`,
     `_CHANNEL_TABLE`, `_ENEMY_OWNER` and `_TEAMS` are gone. shadPS4 keeps the old party path
     (party settings, bells, signs, P2P) until it is dropped.
   - **Team join without a bell or signs** (`TeamJoin`, bloodborne_team.h; docs/native-netcode/
     session.md, "Team join"): the host opens its session with the game's own CreateSession and
     sends the join data a summon would send (`TeamSession`, coop protocol 4); the server relays
     it; each guest joins with the game's own JoinSession in place. Rig: join, join3, team-join3,
     trip pass with no `/summon_messenger` request at all; join x25 for the stalls.
   - **One load** (Kaniel: "load directly to the game"): a guest loads its save once and joins in
     its world without the summon reload (the join's start event skipped, the join finished as a
     load's end finishes it). Not from the title: the game's session layer needs a world.
   - **Launcher:** no party name/password or role; "host" only says which PC runs the server.
     The team is made on the team screen. The System menu's Co-op Team page is gone.
   - **Rig:** the runner makes every scenario's team on the title (`team_prelude`), so every
     scenario runs as a team on the port.
   - **Transport:** the port sends a peer nothing straight and takes no datagram from a peer once
     the link is up (`SendsToPeer`, `TakesDatagramFrom`). Kept, not removed: our NP signaling
     handshake and the STUN ping still answer the game's connection machine and fill the
     server's address keys; they run through the server link (removing them needs the server to
     answer the game's signaling itself: later, with the NP-local proposal Kaniel has not
     decided).
   - **Boards:** the HTTP boards' last receivers (kills, HP, shown characters) are gone; the
     boards live in the server world (phase 3c). Patch sites: none removed; the ones the new
     paths replaced already skip themselves on the port (map item sync), and the bell's are
     shadPS4's.

Later, not planned now (Kaniel 2026-10-13): a headless world host, the port running on the server
without rendering, one copy per occupied map with a hidden anchor character, owning that map's
enemies. Phase 4's owner-per-map protocol takes it without changes.

Phases 0-1 are low risk; 2 and 5 are the large ones. The world keeps working at every step: each
phase replaces one path while the old one stays behind its switch until the rig passes.

## Tracked from main (merged into server-coop)

Main keeps getting fixes while the phases run; what each merge brings that a phase must carry:

- **2026-10-13 (#82, #84): NPC gifts for guests.** `NpcGifts` (bloodborne_party.h) lists every
  talk gift's trigger flag, lot and what its event does after the gift (ClearTrigger, KeepTrigger,
  SetDoneFlag); a guest's own talk (hook 0x0204232E) is given the lot later on the game's thread.
  - Phase 5 (team worlds): those trigger and done flags are the **character's** in the flag split
    (gifts are per player), so a team world never carries them; take the list from `NpcGifts`.
  - Phase 1/2: talks run on the event channels (0x0E, 0x31, same map only); add rig `iosefka-gift`
    to the regression runs.
  - Not covered on main yet (docs/HANDOFF.md): Valtr's helm, Simon's bowblade, Queenly Flesh, the
    Doll's ornament.
- **2026-10-13 (#85): portraits without the status page.** The status model is drawn off-screen and
  read back by the port; portraits still travel as our frame 0x3E (PortraitPartType).
  - Phase 2: 0x3E is relayed like every co-op frame; add rig `portrait-auto` to the regression runs.
  - Phase 3: becomes the `Portrait` protobuf message.

## Risks

- **The game's join state machine** (NetSync 1 -> 2 -> 3 -> 4, ch 7/0x19/0x42/0x10/0x11) must see
  every effect it expects; skipping a packet without supplying its effect stalls a join.
- **Ordering:** the game assumes reliable channels arrive in order per member; our layer keeps
  per-member, per-class order.
- **Identity:** the game addresses members by NpId; joins built from the server's session must carry
  real identities the game's registry knows.
- **Server load and loss:** the server relays everything; it must stay light (no parsing of game
  packets) and its world store must survive restarts (atomic writes, backups).
- **Unknown payloads** pass through untouched until phase 0's trace explains them.
- **Host checks baked into the game** (`+0x124 == 3/6`) for snapshots and enemy authority are patched
  or answered per call, as the ownership-word work already does.
- **Static RE only** so far: phase 0 confirms before code depends on it.
