<!--
SPDX-FileCopyrightText: 2026 shadNet Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Private Bloodborne co-op server

> [!IMPORTANT]
> **Playing with this repository's port (Windows release, Linux AppImage)? Read
> [Hosting for this repository's game](#hosting-for-this-repositorys-game) first.**
> The step-by-step guide below was written for the older shadPS4 P2P fork and its
> own shadNet (`Wozzardman/shadnet-p2p`). That server does **not** work with this
> repository's game: it listens on UDP `31314` for STUN pings only and never
> answers the game's link, so a game stays at `Server link: no answer from the
> server` and never signs in.

## Hosting for this repository's game

The game signs in through the *link* on UDP `31314`, which only the shadNet in
this repository's `server/` speaks. There is no account to create by hand: the
launcher makes each player's id itself and the server creates the account on the
first sign-in. The AppImage has no server in it (its "Start server" button stays
off), so the host builds one from the same checkout or tag as the game:

```bash
git clone https://github.com/lKaniel/bloodborne-server.git   # or the checkout your game came from
cd bloodborne-server
git submodule update --init server/externals/protobuf
sudo pacman -S --needed base-devel git cmake ninja qt6-base qt6-httpserver   # Arch; Qt 6.8 or newer
cmake -S server -B server/build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C server/build shadnet
bash bbport/coop-run.sh server --listen 100.x.y.z    # the address your friends reach: LAN, Tailscale or ZeroTier
```

`coop-run.sh server` keeps its data in the co-op folder's `server/` (`BB_COOP_DIR`),
turns seamless co-op on, sets `Host`, and tells the server which release it is (the
build check on join). Every player, the host too, enters the same address as *Host IP*
in the launcher. Where a built `server/build/shadnet` sits next to the launcher's
checkout, the launcher's "Start server" button does the same; the AppImage has none.

**Check that it is the right server.** The first lines must include

```text
Link UDP listener on: 0.0.0.0:31314
Link: games must run this server's build: ...
```

(Run from a terminal. Tested 2026-10-31 on CachyOS with Qt 6.12: a fresh clone builds with
the commands above, `coop-run.sh server` starts and prints these lines, and the AppImage's
game signs in to it, alone on the host. Not tested yet: several players, other distributions
(Ubuntu, Debian), the Docker build.)

If you see `STUN UDP listener on: ...` instead, this is the wrong shadNet (for example
`shadnet-p2p`): build `server/` of this repository.

Ports: UDP `31314` (the game's link; all game traffic), TCP `31315` and `20443` (web
API and the game's online services). TCP `31313` is only used locally by the launcher
to see whether the server runs. Qt older than 6.8 builds without the HTTP services, and
Ubuntu 24.04's apt Qt (6.4) is such a Qt: use Docker there.

**Docker.** `server/hosting/` builds the same server in a container (see its
[README](hosting/README.md)); it is meant for a public server. The game and the server must
be the same release: a server built from a clone without a `v*` tag calls itself `dev`
and refuses a `1.3.0` game with a message in the game. Add `BB_RELEASE_VERSION: "1.3.0"`
to the `shadnet` service's `environment` in `docker-compose.yml`, or set
`CheckServerBuild=false` in `shadnet.cfg` on a private server.

*The rest of this file is the older shadPS4 P2P guide, kept for its Tailscale and firewall
notes.*

This shadNet fork provides the account, matchmaking, signaling, and WebAPI
services used by the matching [shadPS4 P2P fork](https://github.com/Wozzardman/shadp2p).
This guide sets up a small private server for Bloodborne's traditional or
experimental seamless co-op. The host rings the Beckoning Bell and a guest
rings the Small Resonant Bell in both modes.

The simplest supported layout is one server and all players connected to the
same Tailscale tailnet. Tailscale encrypts traffic between the PCs and removes
the need to expose shadNet directly to the public Internet.

> [!IMPORTANT]
> The game protocol on TCP `31313` is not encrypted by shadNet itself. Use it
> only on a trusted LAN or private tailnet. Do not forward these ports on your
> router for this setup.

## What runs where

| Component | Computer | Purpose |
| --- | --- | --- |
| shadNet | One always-on PC | Accounts, matchmaking, STUN, and WebAPI |
| shadPS4 P2P build | Every player's PC | Runs Bloodborne and sends peer traffic |
| Tailscale | Server and every player's PC | Private connectivity between all PCs |

The shadNet machine may also be one of the playing PCs. Keep its server terminal
open for the entire session.

## Ports and addresses

| Protocol | Port | Used for |
| --- | --- | --- |
| TCP | `31313` | shadNet login and game protocol |
| UDP | `31314` | matchmaking and STUN address discovery |
| TCP | `31315` | WebAPI and Bloodborne summon broker |
| TCP | `31320` | Optional stats page; not required for co-op |

There are three addresses that are easy to confuse:

- `0.0.0.0` is used only for shadNet's `Host` setting. It tells the server to
  listen on all network interfaces.
- The server's Tailscale address begins with `100.`. Every shadPS4 client uses
  this address for **Server**, **WebAPI Server**, and `host_overrides.json`.
- `127.0.0.1` means this computer only. Friends cannot connect to it.

Never give players `0.0.0.0`, and do not use the server's public Internet IP.

## 1. Create the tailnet

Install Tailscale on the server from the
[official download page](https://tailscale.com/download). On Linux, the official
quick install is:

```bash
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up
```

Follow the login link, then obtain the server address:

```bash
tailscale ip -4
```

Write down the returned `100.x.y.z` address. This guide uses
`100.101.102.103` only as an example.

### Invite the players

1. Open the [Tailscale admin console](https://login.tailscale.com/admin/users).
2. Select **Users**, then **Invite external users**.
3. Enter each player's email address or choose **Copy invite link**.
4. Assign the **Member** role.
5. Have each player accept the invitation, install Tailscale, and sign in on
   the PC that runs shadPS4.

Invite every player into the same tailnet. Bloodborne traffic connects players
directly, so sharing only the shadNet device is insufficient. Invitation links
expire; create a new link if a player cannot accept an old one.

Verify connected machines with:

```bash
tailscale status
tailscale ping <PLAYER_NAME_OR_100.x_ADDRESS>
```

## 2. Download and build shadNet

Use a release package when this fork publishes one. To build from source, clone
with submodules:

```bash
git clone --recursive https://github.com/Wozzardman/shadnet-p2p.git
cd shadnet-p2p
```

If the repository was cloned without `--recursive`, repair it with:

```bash
git submodule update --init --recursive
```

### Ubuntu or Debian-based Linux

Install the build tools, Qt 6 HTTP server module, and SQLite driver:

```bash
sudo apt update
sudo apt install git cmake ninja-build build-essential qt6-base-dev qt6-httpserver-dev libqt6sql6-sqlite
```

### Arch Linux and derivatives

```bash
sudo pacman -S --needed base-devel git cmake ninja qt6-base qt6-httpserver
```

### Build

From the repository root:

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The server executable, `worlds.cfg`, and `scoreboards.cfg` will be in `build/`.
On Windows, install Visual Studio 2022 with C++ tools and a Qt 6 MSVC kit that
contains Core, Network, SQL, Concurrent, and HTTP Server, then run the same
CMake commands from a Developer PowerShell with `CMAKE_PREFIX_PATH` set to the
Qt kit directory.

## 3. Create and edit the server configuration

Run the server once from the repository root:

```bash
./build/shadnet
```

It creates `build/shadnet.cfg` and `build/db/shadnet.db` because shadNet uses
the executable's directory for its runtime files. Press `Ctrl+C` to stop it,
then open `build/shadnet.cfg` in a text editor.

Keep the generated file, but set these values under `[General]`:

```ini
[General]
Host=0.0.0.0
UnsecuredPort=31313
Matching2Enabled=true
MatchingUdpPort=31314
WebApiPort=31315
BloodborneSeamlessCoop=false
StatsEnabled=false
RegistrationSecretKey=YOUR_OWN_LONG_PRIVATE_CODE
```

shadNet does **not** generate the registration key. It is a private signup code
chosen by the server owner, not a player password. Replace
`YOUR_OWN_LONG_PRIVATE_CODE` with any long phrase you invent, using no spaces.
The account tool must be given that exact same value when registering a player.
Leave the key set after setup to prevent uninvited account creation. Players do
not need it when logging in. Leave `EmailValidated` set to `false` unless email
validation has been configured separately.

Leaving `RegistrationSecretKey` empty enables open registration. In that case,
the final key argument is omitted from the registration command. Open
registration is not recommended for an ongoing server.

## Choose a co-op mode

The server and every shadPS4 client must use the same mode. Stop shadNet and all
clients before changing it.

### Traditional co-op

Traditional mode keeps Bloodborne's normal area, boss, level, bell, death, and
session restrictions. Set this in `build/shadnet.cfg`:

```ini
BloodborneSeamlessCoop=false
```

Also make sure `SHADNET_BLOODBORNE_SEAMLESS_COOP` is not set in the terminal or
service that starts shadNet. On Linux:

```bash
unset SHADNET_BLOODBORNE_SEAMLESS_COOP
./build/shadnet
```

Setting the configuration to `false` does not override an environment variable
that is still set to `1`.

### The game's own online services (BloodborneBackend)

Bloodborne also talks to Sony's `ss4.scej-network.jp:20443` for its online check (`ss.info`),
login, messages, bloodstains, wandering ghosts, lantern counts and chalice dungeons, as JSON over
HTTP. `backend/bb_dummy_server.py` answers those; shadNet can answer them itself, the same way
byte for byte (`backend/tests/test_parity.py`), so the host runs one program:

```ini
BloodborneBackend=true
BackendPort=20443
; the address the players reach this PC on (else the address each game connected to);
; the environment variable SHADNET_BACKEND_ADDRESS takes precedence
BackendAddress=
; notice.txt, broadcast.txt and login_message.txt are read from here
BackendFolder=../backend
; a SQLite file to keep players' messages, bloodstains, ghosts, lanterns and chalice
; dungeons in (the Python server's --store file reads unchanged); empty keeps the world empty
BackendStore=
; also show players their own: deaths, ghosts, lanterns or all (messages always)
BackendShowOwn=
BackendGhostTtlMinutes=30
BackendLanternTtlMinutes=10
BackendTombCap=100
; seconds between summon sign posts and searches, and the co-op guard timers (retail 30-60)
BackendSummonPoll=5
```

It is on by default; `BloodborneBackend=false` leaves port 20443 to the Python server. The ss.info template is built in from `backend/ss_info_template.xml`.


Seamless mode lets the broker match bells from different maps and carries the
host's placement to the guest so the client can move the guest before the normal
room join. The persistent and recommended server setting is:

```ini
BloodborneSeamlessCoop=true
```

Then restart shadNet normally:

```bash
./build/shadnet
```

For a temporary test without editing the file, start the server with:

```bash
SHADNET_BLOODBORNE_SEAMLESS_COOP=1 ./build/shadnet
```

The startup log must say `seamless co-op enabled` and `anywhere summons enabled`.
Every player must also start QtLauncher or BBLauncher with
`SHADPS4_BLOODBORNE_SEAMLESS_COOP=1`; the matching client README has commands
for Linux, Windows, and macOS. The server trace variable is optional and is not
needed for normal seamless play.

Bell use and cross-map guest placement are working. Lantern travel after a
co-op session is already established remains incomplete on the client side, so
do not expect the group to travel together from an active session yet.

## 4. Start and test the server

Start it again:

```bash
./build/shadnet
```

A remotely reachable setup should report listeners using `0.0.0.0`, including:

```text
Unsecured TCP listener on: 0.0.0.0:31313
STUN UDP listener on: 0.0.0.0:31314
WebApiServer listening on: 0.0.0.0:31315
```

From another tailnet PC, open the following address after substituting the real
server Tailscale IP:

```text
http://100.101.102.103:31315/status
```

The expected response is:

```json
{"ok":true,"service":"shadnet-webapi"}
```

If the server firewall blocks access, allow the `shadnet` application or allow
TCP `31313`, UDP `31314`, and TCP `31315` on the Tailscale/private interface.
Do not expose TCP `31320` unless you intentionally enable the optional stats
page.

## 5. Create one account per player

Each player needs a unique NP ID and email address. NP IDs are 3 to 16 letters,
numbers, hyphens, or underscores. Build the bundled account tool separately:

```bash
cd clientsample
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

With shadNet still running in its own terminal, register each account from the
`clientsample` directory. Use `127.0.0.1` when running the tool on the server:

```bash
./build/shadnet-sample 127.0.0.1 31313 register HunterOne CHOOSE_A_PASSWORD hunter1@example.com YOUR_OWN_LONG_PRIVATE_CODE
```

Repeat with a different NP ID, password, and email for every player. Account
names and emails are case-insensitively unique. Give each player only their own
NP ID and password. `YOUR_OWN_LONG_PRIVATE_CODE` in the command must be replaced
with the exact registration key selected in the previous step.

The accounts live in `build/db/shadnet.db`. Back up that file while the server
is stopped. If it is deleted or replaced, existing logins will stop working.

## 6. Give players the client settings

Send each player:

- Their Tailscale invitation.
- The server Tailscale IP from `tailscale ip -4`.
- Their own NP ID and password.
- A link to the [shadPS4 client setup guide](https://github.com/Wozzardman/shadp2p).
- A reminder to add this fork's executable as a custom/local build in
  QtLauncher or BBLauncher and select it before launching Bloodborne.

Do not send `0.0.0.0` as the server address. Do not send the registration key
after the accounts are created.

Every player's shadPS4 network settings must use the same values, substituting
the real server Tailscale IP:

| Setting | Value |
| --- | --- |
| Server | `100.101.102.103:31313` |
| WebAPI Server | `http://100.101.102.103:31315` |
| Signaling Info | Blank |
| UPnP | Disabled for Tailscale |

Every player also needs this `host_overrides.json` in their shadPS4 user-data
folder:

```json
{
  "https://ss4.scej-network.jp:20443": "http://thehuntersdream.com",
  "http://thehuntersdream.com:18671/summon_messenger": "http://100.101.102.103:31315"
}
```

The client README lists the exact Linux, Windows, macOS, and portable paths.

## 7. Play co-op

1. Keep shadNet and Tailscale running.
2. Have each player start the matching shadPS4 build and sign in with a unique
   shadNet account.
3. Set the same Bloodborne matchmaking password.
4. Enter compatible, normally co-op-enabled areas.
5. The world host rings the Beckoning Bell.
6. The guest rings the Small Resonant Bell.

Traditional mode preserves Bloodborne's normal boss, area, level, bell, death,
and session rules.

In seamless mode, the host and guest may begin on different maps. Ring the
host's Beckoning Bell first, then the guest's Small Resonant Bell, and leave both
active while shadNet prepares the guest and the game completes its normal room
join. If the host travels before discovery, wait until the host finishes loading
before the guest rings. Established-session lantern travel is not yet supported.

## Troubleshooting

### The log shows `127.0.0.1` listeners

Stop shadNet, change `Host=0.0.0.0` in the `shadnet.cfg` beside the executable,
and restart it. Editing a similarly named file in the repository root will not
change a server running from `build/`.

### Players cannot reach `/status`

- Confirm Tailscale shows every PC in the same tailnet.
- Test `tailscale ping` in both directions.
- Check the server firewall and confirm shadNet is still running.
- Recheck the `100.` server address and TCP port `31315`.

### `Login failed` appears

- Recheck the player's NP ID and password.
- Never assign the same account to two active players.
- Confirm `build/db/shadnet.db` is the database containing the accounts.
- Rebuild/update both repositories if the log reports a protocol mismatch.

### The game log says `Server link: no answer from the server`

The game's hellos reach the host (`sudo tcpdump -ni any udp port 31314` shows them, on `lo`
when the game runs on the host itself) but nothing answers. Look at the server's startup lines:
`STUN UDP listener on` means a shadNet that is not this repository's (see
[Hosting for this repository's game](#hosting-for-this-repositorys-game)). With
`Link UDP listener on` the game and the server may be different releases: the game then
logs `the server refused this game: ...`.

### Login works but summoning does not

- Confirm UDP `31314` is allowed on the server.
- Confirm all player PCs can reach one another over Tailscale, not just the
  shadNet machine.
- Check that every client has valid `host_overrides.json` content and can reach
  `/status`.
- Use the same game version. Traditional mode also requires a normally eligible
  Bloodborne area.
- Confirm shadNet's startup line reports the intended seamless setting and that
  every client was started in the same mode.

## Project status and credits

This is experimental software. Back up the shadNet database and player saves.
The project is based on the upstream
[shadNet server](https://github.com/shadps4-emu/shadNet) and is licensed under
GPL-2.0-or-later.

> [!NOTE]
> This README was generated by ChatGPT and reviewed against the configuration
> and networking code in this repository.
