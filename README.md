# Yharnam Together: Bloodborne Seamless Co-op

Seamless co-op for **Bloodborne** on PC, in the spirit of
[Elden Ring Seamless Co-op](https://www.nexusmods.com/eldenring/mods/510): up to four hunters
make a team, share one world, join each other without bells, signs or Insight, and keep playing
together through lamp trips, deaths, boss fights and chalice dungeons.

**Website, download and guides: <https://bloodborne.seamlesscoop.dev>**

**New here? Follow the [easy setup guide](https://bloodborne.seamlesscoop.dev/docs/install).**

| [Trailer](https://www.youtube.com/watch?v=MBlRNpzBqmo) | [Gameplay showcase 1](https://youtu.be/2p2LF5YfhRM) | [Gameplay showcase 2](https://youtu.be/JzaeJXrHaFE) |
| :---: | :---: | :---: |
| [![Trailer](https://img.youtube.com/vi/MBlRNpzBqmo/hqdefault.jpg)](https://www.youtube.com/watch?v=MBlRNpzBqmo) | [![Gameplay showcase 1](https://img.youtube.com/vi/2p2LF5YfhRM/hqdefault.jpg)](https://youtu.be/2p2LF5YfhRM) | [![Gameplay showcase 2](https://img.youtube.com/vi/JzaeJXrHaFE/hqdefault.jpg)](https://youtu.be/JzaeJXrHaFE) |

- [Download](https://bloodborne.seamlesscoop.dev/download): the newest release
- [Install and set up](https://bloodborne.seamlesscoop.dev/docs/install): the setup, the
  launcher's Co-op page, reaching the host
- [Playing together](https://bloodborne.seamlesscoop.dev/docs/play): teams, the party
  list, going down, giving items
- [How it works](https://bloodborne.seamlesscoop.dev/docs/architecture) and
  [the server netcode](https://bloodborne.seamlesscoop.dev/docs/netcode)
- [Build from source](https://bloodborne.seamlesscoop.dev/docs/build)

It runs on **Bloodborne PC**, the native Windows port of the PS4 game (the game's own code runs on
your CPU, its graphics go through Vulkan; there is no emulator in between). The co-op server and
everything else it needs run on the players' own PCs.

> [!IMPORTANT]
> **No game files are included.** You need your own copy of Bloodborne for PS4, version **1.09**:
> a decrypted dump of the game folder with the 1.09 update, or the PS4 `.pkg` files (the launcher
> installs them). This project is not affiliated with Sony Interactive Entertainment or
> FromSoftware.

**Status: experimental, playable.** It is played regularly in parties of two and three and
checked by an automated two- and three-game test rig on every change, but it is a fan project:
expect rough edges and keep backups of your saves. New builds come out as GitHub
**pre-releases**; the launcher updates itself from them.

## Contents

1. [What it does](#what-it-does)
2. [Play it](#play-it)
3. [How it works](#how-it-works)
4. [Repository layout](#repository-layout)
5. [Build from source](#build-from-source)
6. [Make a release](#make-a-release)
7. [Credits and forks](#credits-and-forks)
8. [License](#license)

## What it does

- **Teams instead of bells.** Continue, Load Game or New Game opens the team screen: make a team
  of up to four from the hunters online, Ready, and the leader begins the hunt. The loading screen
  stays up until the team is together. In the game, the **Insert** menu (or L3 + R3) opens
  **Co-op Team** any time: invite, join a running team, or leave. Joining is instant, from
  anywhere. There are no bells, signs or summoning.
- **One world.** The team keeps one world on the server, started from the leader's save. Enemies,
  their HP and their deaths are shared, a kill is a kill for everyone, and anyone can light a lamp
  for everyone. Each save's own world comes back when it plays alone.
- **Travel freely.** Every hunter travels on their own: you can be in different areas at the same
  time and stay in the party. Nobody is pulled along or sent home by lamp trips, the Hunter's
  Dream, deaths or boss kills.
- **The party list.** Every hunter's portrait, name, HP, blood vials and quicksilver bullets, in
  the game's own HUD style, wherever they are.
- **Compass and map.** A compass shows where your team is, and the map shows them on every area
  and takes you to a lamp. Both show your own bloodstain.
- **Team pings.** Double tap the touchpad (or press the ping key, T by default) to mark where you
  aim, or the enemy you are locked on to; press Square on the map to mark any spot. Your team sees
  the mark for 20 seconds in the world, on the compass and on the map.
- **Going down** (from Elden Ring Nightreign, the default): a killing blow puts you down instead of
  killing you while a partner is near. They revive you by striking you; when the whole party is
  down, it wipes and wakes at the host's lamp, and the area resets.
- **Or spectate** (the team leader's pick on the team screen, **Death mode**): a killing blow while
  a partner stands near takes you out of the fight instead. You watch your teammates (L1 / R1 the
  next one) until one of them uses a lamp or the area's boss dies, then wake at your own lamp;
  holding Circle wakes you there at once. The whole team falling is still a wipe.
- **Free camera** (optional, Insert > Controls): F7, or Square with L3 held, flies the camera away
  from your hunter for a look around (or a screenshot), and back.
- **Giving items.** Hand items, weapons and Blood Echoes to a teammate from the **Give** menu.
- **Chalice dungeons together.** Team altars: the team shares its chalice dungeons, and teammates
  join a running dungeon from the altar.
- **No stolen bloodstains.** Your echoes stay where you died until you pick them up.
- **Harder enemies** (optional): three times the HP and poise and half again the damage for every
  enemy and boss, to make up for the extra hunters.
- **Host rules** (optional, all off by default; the team leader's launcher sets them for the
  whole team, and the team screen shows them):
  - *Rot*: each real death (not a down a teammate revives) adds a random penalty, up to five
    (-10% max HP, -10% max stamina, +10% stamina use, -15% rally or -10 Item Discovery). A lamp,
    a level up, a boss kill, killing the enemy that killed you or picking up your own bloodstain
    takes it all away.
  - *Travel together*: a lamp or headstone asks "Travel with the team?" and takes everyone there;
    with the vote, the team has 20 s and one No stops the trip.
  - *Enemies scale with the team*: the normal game alone, up to the Harder values for four hunters
    on the same map.
- **Self-hosted.** One player's PC runs the co-op server (the team lobby, the teams' worlds,
  every game's co-op traffic) and a stand-in for the game's online services. No PlayStation
  Network, no community server, nothing in the cloud.
- **Everything from Bloodborne PC**: 60 FPS and above, resolutions up to 4K, DLSS, FSR 3.1/4 and
  XeSS upscaling, frame generation, HDR, ReShade, mouse and keyboard, gamepads.

## Play it

The illustrated version of this is the
[install guide](https://bloodborne.seamlesscoop.dev/docs/install).

1. Download the setup `.exe` from the
   [download page](https://bloodborne.seamlesscoop.dev/download) (or the
   [releases page](https://github.com/lKaniel/bloodborne-server/releases)) and run it. Everyone in a
   team, the host included, uses the same release.
2. Open **Bloodborne Seamless Co-op** (the launcher) and go to its **Co-op** page: your Bloodborne
   folder (**Find it for me**, **Browse…** or **Install from PKG…**), the host's IP address (the
   host types their own) and Enemies (Off or Harder). **Done** lights up once nothing is missing.
   There is no account to make.
3. Press **PLAY**. On the host, PLAY starts the co-op server first. Windows asks once to let co-op
   through its firewall.
4. Continue, Load Game or New Game opens the team screen: create a team or accept an invite, Ready,
   and the leader begins the hunt. Later, the **Insert** menu's **Co-op Team** tab joins or invites
   from inside the game.
5. Everyone must reach the host's PC: the same VPN (Radmin VPN, Tailscale, ZeroTier) or the host's
   public IP with ports forwarded: TCP 31313, 31315, 20443 and **UDP 31314**. The UDP port carries
   every game's co-op traffic (the games never talk to each other directly), so it must be open.
   New to VPNs? The [Radmin VPN guide](https://bloodborne.seamlesscoop.dev/docs/radmin) walks through it.

Requirements: Windows 10 (1903 or later) or 11, 64-bit, and a Vulkan 1.3 graphics card with a
current driver. Everything else is in the setup. Tested with CUSA03173 (the European Game of the
Year edition) at 1.09; all players in a party should use the same edition.

## How it works

```text
 Player's PC (each player)                          Host's PC (also plays)
 ┌──────────────────────────────────────────┐       ┌────────────────────────────────────┐
 │ Bloodborne PC (bbport): the 1.09 eboot   │ TCP   │ shadnet (server/): accounts, rooms │
 │ runs natively, renderer from shadPS4     │◄─────►│ and signaling                      │
 │  + co-op code (emulator/src/core/        │ UDP   │  + the co-op server: the team      │
 │    bloodborne_*): hooks and patches in   │◄─────►│    lobby, the teams' worlds, enemy │
 │    the game, team join, world sync       │ 31314 │    owners, and the relay of every  │
 │  + co-op map events (made by setup from  │       │    game's packets                  │
 │    the player's own event files)         │ HTTP  ├────────────────────────────────────┤
 │                                          │◄─────►│ the game's own online services     │
 └──────────────────────────────────────────┘       └────────────────────────────────────┘
   every game talks only to the server: one UDP link each, no peer-to-peer
```

- **The game runs itself.** Bloodborne PC loads the PS4 game's own x86-64 code and runs it on a
  small runtime with shadPS4's renderer. The co-op code is compiled into it (`bbport/gpu/bbcoop`
  wires it in) and changes the game at known places in the 1.09 eboot: about 125 patch sites and
  hooks, listed in one table (`emulator/src/core/bloodborne_patch_sites.h`); the launcher warns
  when another game patch writes over one of them.
- **Joining uses the game's own multiplayer, without summoning.** The team's host opens its game
  session itself (the game's own call) and sends its join data to the server; each member's game
  joins that session directly as soon as it stands in its world. Both games then run a normal
  Bloodborne session; the co-op code removes the limits that would end it (boss kills, lamp
  trips, deaths, map changes, the area restrictions) and puts players back together after loads.
- **The server in the middle.** Every game packet and co-op message goes through the server over
  one UDP link with its own resend and ordering (`docs/SERVER_COOP_PLAN.md`). The server keeps the
  team's world (kills, event flags, map state, enemy HP) as an ordered log, names one owner per
  map who runs its enemies, and keeps the teams and their worlds on disk across restarts. Who owns
  each event flag (host, per player, shared) is one table (`docs/FLAGS.md`).
- **Map events.** Some of the game's event scripts end early for a summoned player (boss rewards,
  boss fog, the first visit to the Dream). Setup patches copies of the player's own event files
  (`tools/re/party_boss_events.py`, which checks that only the intended instructions changed);
  the release only carries their expected hashes.
- **Self-hosted services.** `server/` is a shadNet server with the co-op server added (`server/coop`:
  the protobuf messages, the world log, the team lobby, trading). The game's own online requests (JSON over HTTP, as the PS4 game sends them)
  are answered by shadnet itself (`BloodborneBackend`, on by default) or the Python `backend/`, so
  the game goes online without the official or community servers.

More detail: [docs/BBPORT.md](docs/BBPORT.md) (the port and its co-op wiring),
[docs/SEAMLESS_COOP_PLAN.md](docs/SEAMLESS_COOP_PLAN.md) (the original plan),
[docs/WORLD_SYNC.md](docs/WORLD_SYNC.md), [docs/FLAGS.md](docs/FLAGS.md), and the handover notes
[docs/HANDOFF-PORT.md](docs/HANDOFF-PORT.md) and [docs/HANDOFF.md](docs/HANDOFF.md).

## Repository layout

| Folder | What it is | Upstream |
|---|---|---|
| `bbport/` | Bloodborne PC, the native port, with the launcher and the Windows packaging | [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc), [AdrianCsT/bloodborne_pc](https://github.com/0xCydral/bloodborne_pc) |
| `emulator/` | shadPS4 with the original P2P networking; `src/core/bloodborne_*` is the co-op code the port compiles in | [Wozzardman/shadp2p](https://github.com/Wozzardman/shadp2p) (from [shadPS4](https://github.com/shadps4-emu/shadPS4)) |
| `server/` | shadNet server: accounts, rooms, signaling, and the co-op server (team lobby, team worlds, the relay) | [Wozzardman/shadnet-p2p](https://github.com/Wozzardman/shadnet-p2p) |
| `backend/` | stand-in for the game's online services | the LocalServer of the Bloodborne PC Online community package |
| `tools/` | build, local CI, the co-op kits and releases, the test rig (`tools/rig`), RE helpers (`tools/re`) | |
| `docs/` | design notes and handovers | |
| `website/` | the project's website ([live](https://bloodborne.seamlesscoop.dev)): landing page, download and docs (Next.js, deploys to Vercel) | |

All upstreams are imported with their full history (subtree merges), so `git log` shows their
authors. The shadPS4 build of the co-op (`emulator/`) still works but the port is the main target;
port and shadPS4 players cannot be in one party.

## Build from source

Windows only for now. Prerequisites:

- the port: [MSYS2](https://www.msys2.org) with the CLANG64 packages listed in
  [bbport/packaging/windows/README.md](bbport/packaging/windows/README.md), and a python.org
  Python 3.10+ (it freezes the launcher);
- the server and emulator: VS 2022 Build Tools (C++ workload, Windows SDK), LLVM **20.1.8**
  (clang-cl; 23 breaks googletest), CMake, Ninja, Qt 6.8.3 `msvc2022_64` in `C:\Qt` (via `aqt`);
- checks: Python 3.12 with `clang-format==19.1.7` and `reuse`; [jj](https://jj-vcs.github.io/jj/)
  for the local CI script.

```powershell
git clone https://github.com/lKaniel/bloodborne-server.git C:\bb\bloodborne-server
cd C:\bb\bloodborne-server
git config submodule.emulator/externals/mesa-kosmickrisp.update none   # macOS-only
git submodule update --init --recursive
```

`BB_ROOT` (default `C:\bb`) holds the checkout. Build output goes to `C:\bb\out`, outside the tree.

| Script | What it does |
|---|---|
| `tools/build-bbport.ps1 [-Test]` | builds the port (`bbport/build.sh` in MSYS2 CLANG64) and, with `-Test`, its co-op tests |
| `tools/env.ps1` | dot-source it for an MSVC x64 dev shell with LLVM and Qt on PATH |
| `tools/build-emu.ps1` | builds the emulator (`out\emulator\build`) and its unit tests (`out\emulator\tests`) |
| `tools/build-server.ps1` | builds shadnet and its tests (`out\server`) |
| `tools/ci-check.ps1 -Component emulator\|server\|bbport\|backend\|tools [-All]` | local CI: clang-format 19, trailing whitespace, REUSE, build, tests |
| `tools/release-src.ps1` | prepares the reused release checkout `C:\bb\release-src` (see below) |
| `tools/make-release.ps1 -Version x.y.z` | builds the release setup `.exe` (see below) |
| `tools/make-port-kit.ps1` | the developers' test kit for a Bloodborne.PC.Online install |
| `tools/rig/run_scenario.py` | the automated multi-game test rig (two or three games on one PC) |

To play a build of your own, run `bash build.sh` and `bash packaging/windows/package.sh` in
`bbport/` (MSYS2 CLANG64 shell) and use `tools/make-release.ps1 -SkipPortBuild -AllowDirty`.

The website builds on its own with Node 22: `npm ci` and `npm run check` in `website/` (format,
lint, type check, tests and the production build); `tools/ci-check.ps1 -Component website` runs
the same. Deploying it is described in [website/README.md](website/README.md).

## Make a release

Releases build in one reused checkout, `C:\bb\release-src`, so everything builds incrementally:

```powershell
tools\release-src.ps1                     # moves C:\bb\release-src to main@origin (makes it the first time)
                                          # and says whether the emulator tests must run
```

It prints the build folders to use: they stay between releases, so the port, the server and the
account tool rebuild only what changed, and the emulator tests (the release ships no emulator) run
only when `emulator/` changed since they last passed (`tools\release-src.ps1 -MarkEmulatorTested`
after they pass). Never delete the checkout or its build folders after a release.

```powershell
tools\make-release.ps1 -Version 0.4.0 -DlssDir C:\bb\dlss-abi2                   # build the setup .exe
tools\make-release.ps1 -Version 0.4.0 -DlssDir C:\bb\dlss-abi2 -Publish -Prerelease  # ... and a draft pre-release
```

It builds the port and packages it, builds shadnet with its Qt DLLs and the account tool, adds the
release's texts (`tools/release/kit`), the backend and the event patcher, checks the result
(`tools/release/check_release.py`: everything a player needs is there, and no game file, save, log
or setting is), builds one setup `.exe` with Inno Setup (`tools/release/setup.iss`) with its
SHA-256 and, with `-Publish`, creates a **draft** release `v<version>` at the build's commit with
the setup attached (`gh`, or the GitHub API with `GITHUB_TOKEN` or git's stored credential). Check
the draft on GitHub and press Publish. New builds go out as pre-releases (`-Prerelease`); the
launcher updates itself from them and the website's download page offers the newest one.

- Build from a clean, pushed commit: the script refuses uncommitted changes unless `-AllowDirty`
  (test builds only; those cannot be published).
- `-GameDir <game folder with the 1.09 update>` names the game whose event files the release is
  tested with (default: the rig's game, `C:\bb\rig\port\game\CUSA03173`). Only their hashes go into
  the release (`event-reference.json`); each player's setup compares its own files against them.
- `-DlssDir <folder with bbport_dlss.dll and nvngx_dlss.dll>` or `-DlssSdk <checkout of
  github.com/NVIDIA/DLSS>` brings DLSS and frame generation; without either the build stops
  unless `-NoDlss`.
- `-SkipPortBuild` / `-SkipServerBuild` reuse the builds that are there.

## Credits and forks

**Made together by** [Yharnam Together](https://github.com/lKaniel/bloodborne-server) (Kaniel),
[AMANISES](https://github.com/CLSAMG22/BBSC) (CLS_AMG) and
[Supermedo](https://github.com/Supermedo/bloodborne_pc).

**Testers:** fra98_ (framilano), Hardwater, Onesadeye, aboood888848, Fahad, Mob, lelial, J2mF,
Kukar3x. **Special thanks** to framilano (fra98_) for the
[Bloodborne Nucleus Co-op Handler](https://github.com/framilano/BloodborneNucleusCoopHandler) and
Fahad (fhody125) for [No Dream For The Hunter](https://github.com/Mob100percent/NoDreamForTheHunterBB).

This project stands on the work of many people. Each part keeps its authors' history and license.

| Project | What it brings |
|---|---|
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | The PS4 emulator: the renderer and shader recompiler the port draws with, and the network and NP libraries the online play uses |
| [Wozzardman/shadp2p](https://github.com/Wozzardman/shadp2p) | shadPS4 with peer-to-peer networking and Bloodborne online play: where this co-op started (`emulator/`) |
| [Wozzardman/shadnet-p2p](https://github.com/Wozzardman/shadnet-p2p) | The shadNet server (by the shadPS4 team) with P2P support (`server/`) |
| [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) | Bloodborne PC, the native port (releases 0.4 to 0.51 merged) |
| [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) | The port on Windows, the launcher in 13 languages, DLSS, FSR 3.1 and FSR 4 |
| [AdrianCsT/bloodborne_pc](https://github.com/0xCydral/bloodborne_pc) (now 0xCydral) | Windows releases 1.6 and 1.7: the redesigned launcher, Install from PKG, frame generation, XeSS, ReShade, the mouse camera and many fixes |
| Bloodborne PC Online community package | The LocalServer stand-in for the game's online services that `backend/` started from |
| [The Hunter's Dream](https://thehuntersdream.com) by droogie | The community server for Bloodborne's online content, whose service layout the backend follows |
| Kyo, Lance McDonald, auser1337, illusion, emoose and others | The community game patches for 1.09 (`bbport/patches/Bloodborne.xml`): frame rate, resolution, effects |
| [LukeYui's Elden Ring Seamless Co-op](https://www.nexusmods.com/eldenring/mods/510), Elden Ring Nightreign | The ideas: seamless co-op and party death (no code) |

Bundled in the release, unmodified unless noted: Intel XeSS (Intel license), NVIDIA DLSS
(`nvngx_dlss.dll`, NVIDIA's license; the bridge is adapted from
[IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR), MIT),
the AMD FidelityFX SDK and [FireBurn's FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) (MIT),
[PkgTool / LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) (LGPL-3.0),
[ReShade](https://reshade.me) (BSD-3-Clause) with effects by CeeJay.dk, AMD and haasn/JPulowski
(MIT), Qt 6 (LGPL-3.0, next to shadnet), and the libraries listed in
[bbport/README.md](bbport/README.md#credits-and-licenses). The release's `CREDITS.txt` and
`licenses\` folder carry them all.

## License

GNU General Public License v2.0 or later, like shadPS4 and the port it is built on
(`emulator/LICENSES/GPL-2.0-or-later.txt`, `bbport/LICENSE`). Third-party components keep their own
licenses (`emulator/LICENSES`, `server/LICENSES`, `bbport/packaging/windows/licenses`).

Bloodborne is a trademark of Sony Interactive Entertainment. This project contains no game code or
assets and is not affiliated with, endorsed or sponsored by Sony Interactive Entertainment,
FromSoftware, AMD, Intel or NVIDIA.
