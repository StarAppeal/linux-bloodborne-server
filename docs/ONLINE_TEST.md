# Testing seamless co-op with a friend over the internet

Kaniel hosts: his PC runs shadnet, the local backend and his game. The friend only runs the game.
Everything comes from one kit folder built from the current `phase3-travel` build. The old
`Bloodborne.PC.Online` package and the test rig in `C:\bb\rig` are left alone.

## How the old package did it

The old package used **Radmin VPN**. Kaniel's install still points at `26.235.253.56`, a Radmin
address:

- `SERVER - Configure IP.bat` wrote that one IP into `DATA\user\custom_configs\CUSA03173.json`
  (`shadnet_server` `IP:31313`, `shadnet_webapi_server` `http://IP:31315`) and into
  `host_overrides.json`. Everyone, host included, entered the host's VPN IP.
- `SERVER - Host P2P.bat` started `DATA\shadNet-P2P\shadnet.exe` (binds `0.0.0.0`, ports
  31313-31315).
- `SERVER - Register a New User.bat` ran `shadnet-sample.exe 127.0.0.1 31313 register ID PW
  ID.PW@p2p.db` on the host. The account itself goes in `DATA\user\users.json`
  (`shadnet_npid`, `shadnet_password`).
- `LocalServer\SERVER - Start Local Backend.bat` ran `bb_dummy_server.py --address IP` (TCP 20443).
  `SETUP - Switch To Local Backend.bat` pointed `host_overrides.json` at `http://IP:20443`.
- `LAUNCH - shadPS4 Launcher.bat` set `SHADPS4_BLOODBORNE_SEAMLESS_COOP=1` and opened the Qt
  launcher.

The kit does the same job with the new emulator. It adds the party (`SHADPS4_BLOODBORNE_PARTY`,
`SHADPS4_BLOODBORNE_PARTY_ROLE`) and a fixed P2P port (`SHADPS4_P2P_PORT`), and launches
`shadps4.exe` directly the way `C:\bb\rig\host\launch.bat` does.

## 1. Network: use a VPN, not port forwarding

shadnet tells each player the other player's address **as shadnet sees it** (STUN on UDP 31314,
`GetSelfSignalingAddr` in `server/src/cmd_matching.cpp`). If Kaniel reaches his own shadnet
through `127.0.0.1` or his LAN IP, the friend is handed that address and can't connect. On a
VPN, everyone uses the host's VPN IP, so the address shadnet sees is one the friend can reach.

**Radmin VPN (recommended, it's what you used before)**
1. Both install Radmin VPN from https://www.radmin-vpn.com.
2. Kaniel: *Network > Create network*, with a name and a password.
3. Friend: *Network > Join network*, with the same name and password.
4. Kaniel's IP is the `26.x.x.x` shown next to his PC name. That is the **host IP** for both of
   you.

**Tailscale (alternative)**: both sign in at https://tailscale.com/download. The host IP is
Kaniel's `100.x.x.x`. Tailscale's MTU is 1280. If you connect but get frequent disconnects, go
back to Radmin.

**Port forwarding (last resort, untested)**: forward TCP 31313, 31315 and 20443, plus UDP 31314
and 51610, to Kaniel's PC. Both players enter Kaniel's **public** IP in setup, Kaniel included,
so his router must support NAT loopback (hairpin). The friend's side relies on UDP hole punching
through their router. I haven't tried this path. I'm inferring from the code that it needs
hairpin NAT.

## 2. Build the kit (Kaniel)

```powershell
powershell -ExecutionPolicy Bypass -File C:\bb\bloodborne-server\tools\online-kit.ps1 -Force
```

This creates `C:\bb\online-kit\` and `C:\bb\online-kit.zip` (about 40 MB). The kit contains:

| File | Who | What |
|---|---|---|
| `shadps4.exe` | both | snapshot of `C:\bb\out\emulator\build\shadps4.exe` |
| `event-patch\` | both | 17 map event files from `tools\re\party_boss_events.py` (boss loot for the guest, death wakes, per-player messenger gifts); identical to the rig's |
| `1 - SETUP.bat` | both | asks host IP, role, account, party, P2P port; copies your `DATA\user` into the kit and configures it |
| `2 - PLAY.bat` | both | sets the co-op/party env vars and starts the game |
| `FIREWALL - Allow (run as admin).bat` | both | inbound rules for `shadps4.exe`, TCP 31313/31315/20443, UDP 31314/51610-51611 |
| `HOST - Start Servers.bat` | host | starts `server\shadnet.exe` (seamless on, `0.0.0.0`) and `backend\bb_dummy_server.py` |
| `HOST - Register Account.bat` | host | registers accounts on the running shadnet |
| `READ ME FIRST.txt` | guest | the friend's short version of this guide |

The snapshot is whatever is built right now, including work in progress from other threads
(for example the guest HP work). Rebuild the kit when you want a newer build, and send the new
zip. **Both players must run the same `shadps4.exe`.** The script prints the start of its
SHA-256 so you can compare. `-Force` keeps your `user` folder and `settings.bat`.

## 3. What the friend needs

- `online-kit.zip` (send it any way you like).
- Their own `Bloodborne.PC.Online` folder with the 01.09 game and **the same mods enabled as
  yours** (`LAUNCH - Mod Manager`). Different mods can desync.
- Radmin VPN, joined to your network.
- The Visual C++ x64 runtime, if `shadps4.exe` complains about `MSVCP140.dll`:
  https://aka.ms/vs/17/release/vc_redist.x64.exe
- They do **not** need Python, shadnet or the backend.

## 4. Set up (both)

Unzip `online-kit.zip` straight into your Bloodborne.PC.Online folder (next to
`LAUNCH - shadPS4 Launcher.bat`). It adds a `SeamlessCoop` folder (the kit) and four bats in
the old package's style: `SETUP - Seamless Co-op`, `LAUNCH - Seamless Co-op` (runs SETUP the
first time, then plays), `SERVER - Seamless Co-op Host` and `SERVER - Seamless Co-op Register
User`. The old `LAUNCH - shadPS4 Launcher` keeps starting the old version through the Qt
launcher; the Qt launcher can't set the party env vars, so the new build is started by the bat.
Run `SeamlessCoop\FIREWALL - Allow (run as admin)` as administrator, then
`SETUP - Seamless Co-op` (the install folder defaults to the one the kit sits in):

| Question | Kaniel | Friend |
|---|---|---|
| Bloodborne.PC.Online folder | `C:\Bloodborne.PC.Online` | theirs |
| host or guest | `host` | `guest` |
| Host IP | **your own** Radmin `26.x` IP (`127.0.0.1` is refused) | your Radmin `26.x` IP |
| Account name / password | e.g. `kaniel` / anything | e.g. their name / anything |
| Party (`name:secret`) | e.g. `hunters:<something>` | exactly the same |
| P2P UDP port | Enter (51610) | Enter (51611) |

SETUP writes `settings.bat` and, on first run, copies your `DATA\user` (saves, keys, controls)
into `kit\user`. It also builds `kit\game`: junctions to the install's `CUSA03173`,
`CUSA03173-patch` and each active mod folder, plus a real `event` folder with the patched
events on top. `2 - PLAY` starts `kit\game\CUSA03173\eboot.bin`, so the install's own mods
folder is never changed (the same trick as `rig-lowspec.ps1`). A mod that edits event files
would be overridden by the patched ones for those maps. The copied files it then edits are `custom_configs\CUSA03173.json` (shadnet
and WebAPI at the host IP, UPnP off), `users.json` (the account) and `host_overrides.json`
(ss4 to `http://HOSTIP:20443`). Progress made in the kit stays in the kit's own save. Run SETUP
again any time to change answers.

## 5. Start the servers (Kaniel)

1. Close the test rig's shadnet and anything from the old package's `SERVER - Host P2P`. The
   kit uses 31313-31315 and 20443; the rig uses 32313-32315 and 20453, so the two can run
   together, but two shadnets confuse matters.
2. Run `HOST - Start Servers`. Two windows open (`co-op shadnet`, `co-op backend`). Leave them
   open.
3. Run `HOST - Register Account` and enter both accounts (yours and the friend's), exactly as
   typed in SETUP. Each should print `Account created successfully.` Registering is once per
   kit; the database is `kit\server\db`.
4. Have the friend open `http://<your 26.x IP>:20443/status`. `{"ok": true, ...}` means the
   VPN and firewall are fine.

**A third player** sets up exactly like the friend: the same zip (so the same `shadps4.exe`;
mixed builds don't share kills or echoes), role `guest`, the host's VPN IP, the same party
string, and their own account, which you register with `SERVER - Seamless Co-op Register User`.
They also need to join your Radmin network.

## 6. Play

1. Both run `2 - PLAY`, sign in and load your characters. Kaniel goes first.
2. Don't ring any bells. The party controller searches for you, and the friend is pulled into
   Kaniel's world after a few seconds (the host polls every 5 s). After a death or a reload it
   rejoins by itself.
3. Test what you care about: travel through lamps together, deaths on both sides, NPCs, item
   pick-ups, and anything the guest-HP thread asks for.

## Troubleshooting

- **Game says it is offline**: the backend window must be running, TCP 20443 must be allowed
  (FIREWALL bat), and the host IP in SETUP must be the VPN IP. shadPS4 reads settings only at
  start, so restart it after changing them.
- **Login fails / not signed in to shadnet**: the account isn't registered, or the
  name/password differs from SETUP. Register it again on the host.
- **Online but never joins**: both party strings must match exactly, and both must run the same
  `shadps4.exe`. The `co-op shadnet` window should show both players logging in, and later
  `RequestSignalingInfos` lines. If it once worked and then stops (seen once in the rig after a
  level-up), restart shadnet and both games.
- **Joins, then drops or stutters badly**: check the Radmin connection (ping the other
  player's 26.x IP). On Tailscale, try Radmin instead.
- **Logs**: `kit\user\log\shad_log.txt` on each PC, plus the two server windows. The friend
  can send their log for a look.
- **Back to normal**: just use the old `Bloodborne.PC.Online` launcher. The kit never touches
  it.
