# Handoff: state on 2026-10-06 (after lamps as campfires)

Start here when picking the work up in a new session.

## Where things are
- Repo: https://github.com/lKaniel/bloodborne-server, a jj checkout at `C:\bb\bloodborne-server`.
  `emulator/` = Wozzardman/shadp2p and `server/` = Wozzardman/shadnet-p2p (subtree imports, full history).
  `backend/` = local ss.info/content server (Python). `tools/` = build, CI and rig scripts.
- Builds go to `C:\bb\out\{emulator\build, emulator\tests, server}`. LLVM 20.1.8, VS 2022 BT, Qt 6.8.3 at `C:\Qt`.
- Rules: jj for VCS, red→green TDD, `tools\ci-check.ps1 -Component emulator|server|backend` green
  before every push. Commit signing is off for this repo only.
- Commit messages: write the message to a file and use `cmd /c "jj describe --stdin < file"`.
  PowerShell piping adds a BOM, and `-m` with embedded quotes breaks.

## Test rig (`C:\bb\rig`)
- `tools\rig-setup.ps1`, then `tools\rig-lowspec.ps1`, creates it: private shadnet on 32313-32315,
  backend on 20453, and host/guest dirs. Accounts are rig_host / rig_guest, password "rig".
- `host\launch.bat` / `guest\launch.bat` set seamless, party `rig:hunters`, role and RE trace.
  The RE trace (`SHADPS4_BLOODBORNE_RE_TRACE`) is now **0** in both: its ~150 hooks on hot code
  made two instances lag. Set it to 1 only while reverse engineering.
- Restart the servers with logs in files (Start-Process with -RedirectStandardError
  `server\shadnet.err` / -RedirectStandardOutput `backend\backend.log`); `start-servers.bat` logs
  to console windows only.
- Low-spec profile: 540p, Potatoborne only (through a junctioned `rig\game` folder), no extra dmem.
  RAM commit is still close to the limit with two instances.
- Logs: `rig\<p>\user\log\shad_log.txt`, `rig\server\shadnet.err` (timestamps),
  `rig\backend\backend.log`, RE captures in `rig\<p>\user\captures\bloodborne-re\*.jsonl`.

## Done
- Phase 0: our build, two instances, cross-map join.
- Phase 1: party header (`SHADPS4_BLOODBORNE_PARTY`, server `NormalizePartyKey`), party-scoped
  broker, `PartyController` auto-rings the role bell (host Beckoning goods 200 / effect 9000; guest
  Small Resonant 205 / 9005) from the SOS-status hook, backend `SyncCharaId`, 5 s summon polling
  and co-op guard timers, stale-sign supersede and claim reset after join. User-confirmed:
  rejoin after a death is near-instant.

## Phase 3 item: bell-less joining (done, confirmed by Kaniel in the rig 2026-10-05)
- RE finding: a rung bell is just special effect 9000 (Beckoning, game adds 9003 too) / 9005 (Small Resonant) on
  the player. The SOS status (`0x0186FE40`) derives "searching" from the effects' special state.
  01.09 adds 9005 itself at `0x01C0D3B0` through the character's add-effect virtual
  (vtable slot `0x3F0` = `0x018C6DB0`), args `(chr, id, chr, 0, 1, 0, 1.0f x5, stack 0)`.
- `StartSummonSearch` (bloodborne_party.cpp) makes that same call. Party auto-rejoin and the
  cross-map responder resume both use it instead of `UseItemNativeApply` (goods 200/205), so no
  item, animation or Insight is involved. The slot is checked against the verified 01.09
  function before calling.
- Log line: `auto-rejoin started search effect=9000|9005 ... result=true`. Note the host's
  effect is 9000; 9001 (used in Phase 1 code and still in the healing-fountain host check) is
  not a search effect, so that fountain fix has likely never fired.
- Eboot RE on this PC: scratch scripts flatten `eboot.bin` (fake SELF) and apply relocations,
  then disassemble with Python capstone (`py -3.12 -m pip install capstone`).

## Session teardown (2026-10-05)
- The seamless keep-matching patch (call at `0x019471B1` in the world teardown `0x019470F0`)
  kept the session over *every* teardown, so a guest quitting to the title stayed in the room.
  A hook at `0x0194716E` (`0x01947167` is RIP-relative and cannot be hooked) now calls the
  native `StopRequest` when the guest is summoned (state 6) and no summon reload is running.
  Phase 2 extended it to the host (state 3): a host quitting to the title ends the session, so
  guests are not left in a room with a ghost host. Confirmed: guest quit-to-menu and reload rejoins.
- Server: signs not refreshed for 75 s are no longer offered (stale sign from a previous run).
- Sign timers: 01.09 waits 5 s before a sign's first post and 30 s between re-posts
  (`0x018A9264/9324`, `0x018A9757/988F`), ignoring ss.info. Patched to 0 s / 5 s.
- Known: the emulator's signaling reuses a lingering (locally deactivated) connection when the
  peer re-activates within 60 s; the game never hears that invite.

## Phase 2: deaths that don't disconnect (done, confirmed by Kaniel in the rig 2026-10-05)
Kaniel's design: nobody leaves on death and the world does **not** reset. Host and guest deaths
work the same: YOU DIED, echoes dropped in a bloodstain, get up, teleport to the player's own
last lamp. Both sides keep seeing and hitting each other; repeated deaths work.
- **The death flow is native.** 01.09 runs a compiled copy of the event scripts; the Lua source
  (`global_event.lua` in `script/eventcommon.luabnd.dcx`) is never loaded, but it is the best map
  of the native code (state names such as "PartyGhostDeath_wait" are string refs in the eboot).
  `py -3.12 tools\re\lua50dis.py <eventcommon.luabnd.dcx> [function regex...]` disassembles it.
  Event script bindings (name -> native function) were extracted from the registration code
  around `0x01340000` (e.g. RevivePlayer `0x01332B20`, SetChrType `0x01338DF0`).
- **Guest death** (bloodborne_party.cpp `GhostDeathKeepSessionPatches`, `ReviveDeadPartyGuest`):
  PartyGhostDeath (`0x01381FD0`) no longer sends 4063 (the host's OnGameLeave drops the guest),
  sets 4047 or SetFlagInitState(2); PartyGhostDeath_1 skips the "returned to your world" menu
  and warp. A hook at `0x01382354` revives instead: clear death flag 4000 and the "returning"
  state ([SprjEventMan+0x10]+8, set to 4 by OnEvent_4000 `0x013813D0`, blocks later deaths),
  RequestFullRecover, UpDateBloodMark (with GlobalState +0x1592 set so the stain uses the death
  position), RevivePlayer, 01.09's local-player revive `0x013179B0` (clears the death's
  controller locks [+0x58]+0x114 / [[+0x58]+0x38]+0x48: no move, no damage, not targeted),
  restore saved alive flags, SetChrType(white) (the revive makes a live player), ChrResetAnimation,
  then the lamp teleport. The self-bloodstain pickup (OnSelfBloodMark 4032) is registered for
  ATTR_WHITE too (`0x0137EBC7`), so guests can pick up their stain in the host's world.
- **Lamp teleport**: GlobalState +0x1528 holds the last lamp's respawn point id (2412951 etc.);
  `0x0154AFC0` resolves it against the loaded blocks ([`0x0553B148`]), then the character's
  vtable +0x5F8 moves it (as the Warp binding does). Only works if the lamp's block is loaded;
  otherwise the player gets up where it fell.
- **Host death** (`ReviveDeadPartyHost`): new `InstallGuestFunctionOverride` in cpu_patches (a
  hook at a function entry that can skip the function and return 0). Native SoloPlayDeath_2
  (`0x01381DE0`, bloodstain + reload at the lamp) is replaced while hosting (state 3) by the same
  revive, minus the white checks. Guests skip HostDead's warp home (`0x013819C6`), and the death
  step no longer removes session members from a dying host's world (`0x018C4D1D`). Solo deaths
  are vanilla.
- Dead ends, so nobody retries them: an in-place RevivePlayer alone (stays in its death state);
  a guest summon reload as a respawn (the host's half of the summon handshake never runs, the
  guest leaves the room ~12 s later, and it always lands at the original summon spot); the Warp
  binding with the respawn id (it wants a map entity).
- Diagnostics (logged on change, cheap): `bloodstain state`, `other players`, `local effects`,
  `revive restored alive state`, `warp to last lamp` / `moved to last lamp`.

## Lamps as campfires (done for now, rig-tested by Kaniel 2026-10-06)
Kaniel's design: a rest works "like a reload, but in place, without breaking the session". In a
party session (party configured and matching state 3 or 6) only:
- **Lamps work in a session.** The lamp talk scripts (t<map><nnn>.esd, 46 lamps; see `tools\re\esdump.py`) hide a lit lamp while ESD function 52 ("in multiplayer": session
  players > 1 or a pending matching) is true. A register hook at `0x0203C55F` (ESD function
  dispatcher `0x0203AD60`, case 52) makes it false. The healing fountain's +0x49 byte is cleared
  for both roles (`0x012F836E`, previously host-with-9001 only).
- **Using a lamp is a rest, not a trip.** "Travel to the Hunter's Dream?" OK sets flag
  70000100 + map*10000 + lamp (7MMM01LL; 7MMM02LL lights a lamp) for common event 7200, which
  touches the lamp (2004[14], anim 101160) and warps. Talk command 11 (`0x0204232E`, flag in
  r15d) is pointed at a zero flag range for that flag, so it is never set; the player plays
  101160 through ForcePlayAnimation (`0x0132E290`, started 0.5 s later or the closing talk
  cancels it) and rests until walking 4 m away. ForcePlayAnimation's 7725 does nothing.
- **Phantoms get no lamp prompt** (cause not found: the guest's lamp script runs and f52 is
  false). A guest joins a rest by standing within 2.5 m of where a member used the lamp; only a
  rest started at the lamp is joinable.
- **Coordination:** shadnet `POST /bloodborne/party/rest` ("<member> <lamp> [x y z]", party
  header) returns the party's current rests (fresh for 5 s). Each emulator posts once a second
  in a session (`RunRestBoardClient`, own thread, cpp-httplib) and refreshes once on the rising
  edge of "every session player rests at my lamp" (session players: [GlobalState+0x16F8]+0x14).
- **The refresh** (`RefreshWorldAtLamp`): RequestFullRecover + the bonfire regene
  `0x018FAC80(player, 0)` (rest special effects 100-109; RequestFullRecover alone left HP as it
  was), the respawn load's restock from storage `0x014DBB70` then `0x014DACE0` (vials/bullets),
  and the Dark Souls bonfire respawn left in 01.09: WorldChrMan(`0x0553E878`) +0xA6C = 1.
  WorldChrMan freezes all block characters (+0x359 |= 4), respawns blocks (`0x0190E3B0`) and
  WorldChrMan +0xA40 (`0x018EB900`), state 3; SprjEmkSystem (`0x012F0F90`) restarts all event
  scripts and resets temporary flags in state 3, then 4, 5 (thaw), 0.
- What the party rest changes in that respawn: the event restart is skipped (hook `0x012F0FCB`
  moves 3 -> 4); only characters dead now and seen alive during this map visit respawn (hooks on
  the NPC param loads `0x0190E445`/`0x0190E572` hand living ones a stand-in param with the skip
  bit +0x145 & 0x2000000); disabled-alive characters ([+0x18]+0x20 bit 0, set by `0x018C6390`)
  are disabled again; state flags 0x4000/0x20000/0x40000000 of live characters are put back.
- **After a party wipe** the same respawn (rests on or off) runs 5 s after the wipe's trip
  arrives, for characters the member's own save left dead: 0 HP, no death mark (+0x1E3 & 0x27
  clear), not disabled or hidden (`LeftDeadByItsSave`). Rig smoke 2026-10-12: a guest restarted
  mid-session loaded its save with 0x1001400b dead, joined as a phantom, and the wipe's trip home
  (`WarpNextStageBonfire` -> stage request `0x013CDE30`, as in SoloPlayDeath_2 `0x01381DE0`)
  brought its world back as the save had it; host and guest2's reloads respawned theirs. Log:
  `party wipe: N characters left dead by this member's save ... respawning=`.
- New hook API: `InstallGuestRegisterEditHook` (cpu_patches): the trampoline already reloads
  registers from the snapshot, so a hook can change them.
- `py -3.12 tools\re\esdump.py <talkesdbnd.dcx> [esd]` dumps talk scripts (states, conditions,
  commands). Bloodborne Enhanced's decompiled talk/event scripts (github
  colorsolid/bloodborne-enhanced) are a good map; its Reawaken is a real reload.

## NPC gifts for party guests (2026-10-13)

Kaniel: each player gets its own gift from its own talk, with every NPC. A guest runs an NPC's talk
script as the host would, so a gift the talk script gives itself already reaches it; a gift that an
event gives after the talk sets a trigger flag did not: those events end unless the local player is
alive (4:3 player 10000 type 0), and a party guest is a phantom. The emulator now gives a guest's
own talk's gift itself (`NpcGifts`, `GivesGuestNpcGift`, `QueueGuestNpcGift`), then does what the
event does once it has given it: clear the trigger (repeatable gifts), keep it (once by the
trigger), or set the event's own flag (once by the event id or id + slot), so the guest's own world
does not give it again at home. The host's own events give the host its own, unchanged.

| NPC (map) | Item | Path | Trigger -> lot | Guest before / after |
|---|---|---|---|---|
| Iosefka (m24_01) | Iosefka's Blood Vial | common 9100 | 72410310 -> 23000 | no / yes (#82, rig iosefka-gift) |
| Central Yharnam NPC (m24_01, t241314) | Blue Elixir | common 9100 | 72410359 -> 24060 | no / yes |
| Cathedral Ward window NPCs (m24_00) | Blood of Adella; Blood of Arianna | common 9100 | 72400398 -> 32020; 72400489 -> 28000 | no / yes |
| Queenly Flesh (m25 event 12500020) | Queenly Flesh | common 9100 | 9043 -> 17010 | no / no (set by an event that ends for a client, not a talk) |
| Annalise (m25) | rune | common 9040 | 9042 -> 17000 | no / yes |
| Yharnam window NPCs (m24_00, m24_01, m27) | Pebble, Numbing Mist, Beast Blood Pellet, Lead Elixir, Sedative, Pungent Blood Cocktail | common 9040 | 9045, 9047-9052, 9054-9064 -> 22000-27060 | no / yes |
| Gilbert (m24_01) | Flamesprayer | common 9040 | 9066 -> 29000 | no / yes (unit test; a rig guest talking at his window (guest-talk-gilbert's spot) with Cross only does not reach the gift's dialogue branch, so not rig-tested) |
| Little girl (m24_01) | Tiny Music Box | common 9040 | 9067 -> 30000 | no / yes |
| Sobbing NPC (m28) | Madman's Knowledge | common 9040 | 9069 -> 32000 | no / yes |
| Djura (m23) | Powder Keg Hunter Badge | common 9040 | 9071 -> 33000 | no / yes |
| Alfred (m24_00) | Fire Paper; Wheel Hunter Badge | common 9040 | 9072 -> 34000; 9073 -> 34010 | no / yes |
| Eileen (m24_00, m24_01) | Bold Hunter's Mark; rune | common 9040 | 9074 -> 35000; 9075 -> 35010 | no / yes |
| (unused slots) | Iosefka's Blood Vial; Tonsil Stone | common 9040 | 9046 -> 23000; 9076 -> 39000 | no setter found |
| Nightmare Frontier NPC (m32) | rune | common 9040 | 9077 -> 39010 | no / yes |
| The Doll (m21) | Tear Stone (for the Small Hair Ornament) | m21 12100123 | 72100112 -> 14000 | no / yes (the ornament stays with the guest: no item removal yet) |
| Cathedral Ward NPC (m24_00) | Madman's Knowledge | m24_00 12400403 | 72400441 -> 37000 | no / yes |
| Valtr (m27) | rune; League Staff | m27 12700906 | 72700321 -> 43120; 72700322 -> 43130 | no / yes (the lots give nothing while their flags 6676 / 6678 are on, and the host's came over from the team world: a guest's own gift clears them first, `ClearsNpcGiftLotFlag`, playtest 2026-10-25) |
| Hunter's Nightmare NPC (m34) | Holy Moonlight Sword | m34 13400980 | 73400424 -> 43211 | no / yes |
| Research Hall (m35) | Balcony Key; rune; Blood of Adeline | m35 13501920, 13501940 | 73500321 -> 43010; 73500322 -> 43020; 73500320 -> 43000 | no / yes |
| Fishing Hamlet (m36) | Accursed Brew | m36 13600943 | 73600300 -> 43600 | no / yes |
| Valtr (m27) | One-eyed Iron Helm | m27 12700908 (also his death flag 1790) | 72700330 -> 43110 | no / yes (talk: `NpcGifts`; death: `NpcDeathDrops`, 2026-10-28) |
| Simon (m34, m35, m36) | Simon's Bowblade | 13400953, 13500953, 13600954 (also his death flags 1710-1713) | 73400430/31, 73500630, 73600530 -> 43200-43220 | no / yes (talk: `NpcGifts`; death: `NpcDeathDrops`, 2026-10-28) |
| Hunter's Nightmare beast hunter (m34, entity 3400910) | Firing Hammer Badge (goods 4120) | m34 13400995 (ends for a client) | death 13400970 -> 43800 | no / yes (`NpcDeathDrops`: the lot's flag 6671 is the host's world, so the guest's own badge decides; DLC playtest 2026-10-28) |
| Hunter's Nightmare Gatling Gun hunter (m34, entity 3400911) | Gatling Gun | m34 13400980 slot 1 | death 13400971 -> 43810 | no / yes (`NpcDeathDrops`; krolebord, DLC playtest 2026-10-28) |
| Research Hall NPCs (m35, 13501900 slots) | attire, weapon, helm | m35 13501920 slots 3-5 | death 13501900/03/05 -> 43830, 43820, 43710 | no / yes (`NpcDeathDrops`) |
| Forbidden Woods NPC (m27, entity 2700930); Cathedral Ward NPC (m24_02, entity 2420268) | goods 2140; goods 4006 | m27 12700909, m24_02 12420700 | their own flags -> 43840, 2420900 | no / yes (`NpcDeathDrops`) |
| Micolash (m26) | Mensis Cage | m26 12601850 (a boss kill event) | 72600301 -> 21000 | unknown (check the boss event patch) |
| Tonsil Stone voices (17: m24_00, m24_01, m22, m27's Forbidden Woods house) | Tonsil Stone | the talk script itself, on the first talk only (its flag 73200330-35, 40-44, 50-55) | none | no / yes (the first-talk flags were the host's world, so a guest got the "talked before" line: per player now, `GiftTalkFlags`, and off while the player holds no stone, `KeepGiftTalkFlagsOwn`; playtest 2026-10-25) |
| Blood Vial and Blood of Adella givers (the shelter) | Blood Vial, Blood of Adella | the talk script itself (c1_52 adds, c1_65 the popup) | none | yes (runs in the guest's own game; its cooldown flags 72400390-98 are still the host's world; not rig-tested) |
| Eileen, Alfred, Djura, Valtr and others | gestures | the talk script itself (c1_66), skipped while the gesture's flag is on | none | no / yes (see NPC gestures below, rig gesture-own) |
| Hunter's Dream messengers | Notebook, bells, Eye of a Blood-drunk Hunter | m21 12101022-28 (made local) | 12101023-29 -> 10000-10050 | yes (earlier fix) |
| Cleric Beast (m24_01 12411700) | Sword Hunter Badge | the boss event, skipped while 6645 is on | lot 50000010 | no / yes (rig together/boss-kill) |

NPC gestures (2026-10-23, an outside player on v1.1.4: a guest never learned Eileen's and
Alfred's gestures, the host did, and the guest got the items from the same talks). Every gesture
an NPC gives comes from one step of its talk script (debug string ジェスチャー取得判定, "gesture
get check"): while the gesture's flag is on it does nothing, else it shows the gesture (c1_65),
learns it (c1_66) and sets the flag (c1_11). Those flags had no row in the flag table, so they
were the host's world: the guest's copy followed the host's, a host who had the gesture made the
guest's talk skip it, and a team world carried the host's flag into the guest's save. They are
each player's own now (`GestureFlags`, PerPlayer, kept for the guest's own world), and every
second a gesture flag that is on while this player has not learned its gesture goes off
(`GestureFlagFix`, `KeepGestureFlagsOwn`), so a save that took the host's flag gets the gesture
from the NPC's talk again. A learned gesture's flag that is off stays off: Church Bow (Female)
has two givers, each with its own flag.

The game keeps the gestures in the local PlayerGameData (`[0x0553B130] +0x08`) +0x5B8: 30 u32
slots from +0x10, bit 0 learned, bits 1-15 the row of the gesture table (0x014EC980: 31 rows of 16
bytes, row 0 none, row k gesture k - 1 with goods 800 + k - 1). c1_66 (dispatcher case 0x02041691,
body 0x02042AD1) looks for the slot whose row holds the gesture and sets its bit 0
(0x02043E9E); the list is the guest's own, a phantom's as well, and it stays over trips (rig
gesture-own). Rig verbs: `talk-gesture <gesture> <flag>` (that talk step), `expect-gesture
<gesture> <0|1>`.

| NPC (talk script) | Map | Gesture | Flag |
|---|---|---|---|
| Eileen (t240325, t241325) | m24_00, m24_01 | 4 Shake off Cape | 6744 |
| Eileen (t240325) | m24_00 | 11 Shh! | 6751 |
| Eileen (t240342) | m24_00 | 10 Wait | 6750 |
| Eileen (t241325) | m24_01 | 14 Approval | 6754 |
| Alfred (t240333) | m24_00 | 9 Pray | 6749 |
| t240343 (talk ids 34xxxxx, as Alfred's) | m24_00 | 5 Church Bow (Male) | 6745 |
| t240318 | m24_00 | 6 Curtsy | 6746 |
| t240322 | m24_00 | 7 Church Bow (Female) | 6747 |
| t240339 | m24_00 | 7 Church Bow (Female) | 70000007 |
| t240312, t240504 (talk ids 22xxxxx) | m24_00 | 13 Triumph | 6753 |
| t242480 | m24_02 | 20 Make Contact | 6760 |
| Djura (t230323) | m23 | 17 Brush off Dust | 6757 |
| t250307 | m25 | 18 Respect; 19 Deep Respect | 6758; 6759 |
| t250324 | m25 | 15 Roar | 6755 |
| Valtr (t270503) | m27 | 22 League Oath | 6770 |
| Nightmare Frontier NPC (t320329) | m32 | 16 Beg for Life | 6756 |

Each step runs on the talk's own conditions (a dialogue branch, the NPC's story step); the table
only says which flag holds it back. Found with `tools/re/esdump.py` over every
script/talk/*.talkesdbnd.dcx (a talk script group with `c1_66(arg0)`, its callers' arguments
gesture, goods 800 + gesture, flag).

Hunter badges (2026-10-18, Kaniel: a guest never got the Sword Hunter Badge): the Dream's 12100450
sets 6630-6633 and 6640-6649 once the player holds a workshop tool or badge, and the badges' givers
skip theirs while it is on (the Cleric Beast's 6645; m21_00 12101800's 6642; m23_00 12301700's
Spark Hunter Badge, 6644; common 9910 marks the NPC badge gifts' lots taken by 6641 6643 6647
6649). Those flags were world flags, so a team world put the host's into every member's save and a
guest whose host held the badge never got its own. They are each player's own now
(`HeldItemFlags`, FLAGS.md), and in a party they follow what the player holds
(`HeldItemFlagFix`, `KeepHeldItemFlagsOwn`, every second): a save with the host's value gets its
badge, a player who holds one gets no second.

Key items gained in the join's state (2026-10-18, krolebord's badge, logs 44/45): until its first
trip a party guest is a summoned phantom (character type +0x78 = 1), and the game gives it a key
item list of its own: the inventory's +0x58 (the key items, the entries below the split at +0x24)
points to another list, without the guest's own key items. A key item the guest gains then (the
Cleric Beast's badge at 50000010, a boss key, an NPC gift, a pickup) goes into that list, and at
the first trip the guest's own list comes back and the game drops the other, with what was gained.
Goods outside the key items, echoes and vials stay; a later trip drops nothing. The guest's tick
looks at the phantom's list every 250 ms and, once another list is there and it is no phantom,
gives back what it gained (`PhantomKeyItemsToGiveBack`, `GiveBackPhantomKeyItems`; never a bell).
Rig: together/boss-badge-kept.

Runes in a party (player report 2026-10-22): the League's "Impurity" rune (GemeffectParam 2127001)
only puts the marker 6142 on its wearer. Common event 9035 (from event 0 with 6142, 180040) adds
180040 (max HP x1.02) while the wearer is a white phantom (4:3 10000 type 1) and takes it off when
not, so a party guest lost it at its first trip and the host never had it. `KeepImpurityBonus`
puts it on every second for anyone in a party session wearing the rune, the host too (Kaniel
2026-10-22; `ImpurityBonusChange`, `ImpurityBonusForHost`). The other oath runes (Radiance
2107001/2, Corruption 2108001/2, Hunter 2110001/2, Beast's Embrace 2128001-5, Milkweed 2129001-3)
carry their effects in the gem rows and need no type. Cursed chalice dungeons (m29 12907420) pick
4736 instead of 4735 for a League phantom (the same type-1 check) to cancel the 2%; corruption
trophies (9360, 9480, m29 12900260) take type 0 or 1. Rig: impurity-rune.

Open: the death-flag events (Valtr's helm, Simon's bowblade) wait on 1003:101 checks not decoded
yet; Queenly Flesh's trigger comes from an event that ends for a client; the Doll's exchange keeps
the ornament on a guest. Shops, repair, upgrade and storage already run in each player's own game.
Inventory scripts: the session scratchpad's inv/ (talk scripts' item commands, every 2003:4 award).

## Next (user-approved order)
1. Rest of Phase 3 (travel together, boss kill doesn't dismiss), then Phase 4 (shared progress,
   guests as full players: loot, full HP/vials, their own save progress; they stay white
   phantoms as the network role).
2. The backlog at the end of this file.

## Planned for the end (Kaniel's call)
- **Death refinements** (Kaniel moved these to the end, 2026-10-05):
   - Test with more than 2 players (rig has two; needs a third instance or a friend).
   - World refresh: dying to a boss should refresh the world; decide the general rule for when
     the world refreshes (see lamps as campfires above).
   - Lamp teleport camera: it swoops to the lamp instead of cutting; look for the snap flag of
     the move (vtable +0x5F8's mode argument is 1 now).
   - Lamps in another block/map: currently no teleport (needs a load, which needs the summon
     handshake, see "Travel together").
   - Invaders (black phantoms) dying in seamless mode skip the warp home too; only relevant if
     invasions are kept.
- Near-zero join wait: the host only searches every 5 s (`--summon-poll`; also drives the
  guard timers, so don't set 0). Plan: have shadnet hold `summon_messenger/get` open ~3 s and
  answer as soon as a same-party sign arrives (long-poll). Check first whether the game's
  matching task tolerates a slow answer (it sets a 15 s connect timeout, no receive timeout).
  Fallback: 1 s interval.
- Signaling: fix the lingering-connection reuse above if it shows up again.

## Phase 3: seamless travel (branch `phase3-travel`, 2026-10-06)
Kaniel's design: either player travels on their own, nobody is pulled along or sent home, and the
session stays up across maps. Kaniel chose option A (stretch the native session), see
`docs/SEAMLESS_TRAVEL_PLAN.md` for the full findings and dead ends.
- **Rig-confirmed:** host round trip (lamp to the Dream, headstone back) and guest round trip; both
  see each other and hit enemies afterwards. The guest uses the Dream's NPCs, Doll and pickups.
- **How:** `PartyTrip` (stage transition 0x013CDE30 marks a trip until the new stage is up) skips
  the teardown StopRequest, every LeaveSession and CSMultiPlayMan StopRequest (0x01ED07A0). On
  arrival the join tasks the reload wiped are rebuilt once every member is on the map (shadNet
  party map board, `POST /bloodborne/party/map`): allocate, construct 0x01E4F9F0, add
  0x01E55A70, saved slot/flags, SetStep(4). A guest also rebuilds its own task (kind 2). Bell
  effects (9000/9003, 9005/9006) are put back.
- **Guests use lamps:** talk hooks for ESD f14 (0x0203B7FB) and the talk object's phantom check
  (0x0203AC04). Lamp event 7200 and headstone event 12107000 end for clients, so the emulator does
  a party guest's trip (WarpNextStage_Bonfire; headstone flag 7210MMML -> point MMM2950+L),
  then moves it to the lamp a second later.
- **Party rests are off (WIP, Kaniel's call):** a lamp travels on the first press again;
  `SHADPS4_BLOODBORNE_LAMP_REST=1` turns rests back on.
- **Boss kill keeps the session** (BlockClear2_2/2_3 patches). Not rig-tested yet.
- Guest-as-host experiment (`SHADPS4_BLOODBORNE_GUEST_AS_HOST=1`): full HP bar, but the model is
  invisible and lamps stay hidden. Off by default.
- **Fixed and rig-confirmed (2026-10-06 evening):** multiplayer fog walls (event 7600) are off in a
  party session; a guest dying far away revives in place (a travelled guest is a live player, so
  `ShouldReviveHostInPlace` covers state 6 too) and a same-map reload counts as a trip's end;
  the Doll's dialog (721001xx) is no longer taken for a lamp trip (it warped the guest and
  dropped it).
- **Seen once, then gone:** after a level-up the guest ignored the host's join invitation
  (shadNet delivered it every 5 s, the guest never fetched it), even with a new character.
  Restarting shadNet and both games fixed it, and a high-level guest joined fine afterwards.
  If it returns, compare the sign body and check the server's retained advertisements.
- Diagnostics to trim later: guest talk answers/flags, join-task and member-leave logs.
- Rig launchers have `SHADPS4_BLOODBORNE_RE_TRACE=1` on (Kaniel asked for it).
- Both rig clients log "permission denied" opening `userdata0005`/`0010` on every save; check
  whether saves land.

## Next (Kaniel 2026-10-06, in this order)
1. **Guest HP and the nerf on join:** a summoned guest's max HP and damage are cut like a
   phantom's. The guest-as-host experiment showed the full bar returns with a live type, but that
   also hid the model, so find the phantom reductions themselves and switch them off for party
   guests.
   - Tried (commit "party guests keep their full HP", on `phase3-travel`): SpEffectParam row 9006
     (added to a summoned guest) has maxHpRate 0.7 at +0x10. `KeepGuestFullHp` finds the loaded
     param through SoloParamRepository (`0x05540340`, holder +0x70 -> +0x70) and writes 1.0. Both
     rig logs confirm the write ("SpEffect 9006 max HP rate 0.7 -> 1"), but Kaniel's rig run
     (2026-10-06) still shows **lowered HP and lowered damage**. So either 9006 isn't the effect
     doing it, the rate was already applied to the character before the write (it happens on the
     first party tick), or the cut comes from elsewhere (phantom-type scaling code, another
     SpEffect such as 9000/9003/9005, or a multiplayer correction param). Next: read the guest's
     active SpEffect list and max HP fields live, and find what reads the phantom type when
     computing max HP and attack. Keep or drop the 9006 write depending on what that shows.
   - **Found (2026-10-06):** the cut is level sync. The guest's effects were `6300 40 9006 9200 2`;
     9200 is added by `0x01507DDC` when the player's level is above the host's band (host level *
     (1 + rate) + add, from [global+0x18]+0x8E4/+0x8E0) and removed when it no longer is. The
     max HP aggregate (`0x017E46E0`, product of the effects' maxHpRate) and the stamina/attack
     aggregates multiply by `0x01582940` (CalcCorrectGraph rows 0x78-0x7C, host level vs own)
     whenever 9200 is present. The rig's guest outlevels the host, hence lower HP and damage.
     Fix: register hook at `0x01507DB5` clears the decision (bl) in a party session, so the game
     removes 9200 itself (`InstallNoLevelSyncInParty`). The 9006 write stays (guests get 100%).
     Param tooling: the game ships its paramdefs (`paramdef\paramdef.paramdefbnd.dcx`), so
     gameparam rows can be decoded with field names.
2. **NPCs look transparent** to a guest (it can talk to them already).
   - **Found (2026-10-06):** character creation (`0x018E4FB6`, `0x018D8934`) turns an NPC whose
     NpcParam has isChangeWanderGhost (+0x145 bit 7; 76 rows, mostly friendly hunters, npcType 2
     team 26) into a wandering ghost (type 4, ghost model +0x10C) when the matching state is 6
     or [session+0x98] != -1. `InstallSolidNpcsInParty` points the flag read at a zero row in a
     party session. Pushed as 0dd37445; rig-confirmed by Kaniel (all NPCs solid).
   - Item 1 (level sync, 668a39d4) is rig-confirmed too.
3. **Shared loot and achievements.**
   - **Loot done, rig-confirmed by Kaniel (2026-10-06):** Kaniel chose "each player picks up its
     own copy". Enemy drops already worked for both. Map items (MapItemMan `[0x0553D6E0]`, lists
     +0x18/+0x28/+0x38, item handle +0x20, flags +0x48 with 0x40 = usable, position +0x30) are
     host-authoritative for a summoned guest: it skips creating them and numbering them
     (`0x017DC3CF/DC`, `0x017D52D2/DB`), may pick up only with GlobalState +0x1592 set
     (`0x017D6831`, `0x017D6AEF`), only asks the host to take one (`0x017D85F4`) and, decisive,
     clears all its map items when the host's item sync arrives (packet channel 0x2C read in
     `0x017D33D8`, kinds via jump table; clears at `0x017D3498-0x017D34A8`). `InstallSharedLoot`:
     every one of those checks reads from `SoloMapItemStandIn` whenever a party is configured
     (the join reload runs before the state reads as a session), and channel 0x2C packets read
     as empty (`IgnorePartyMapItemSync`). Diagnostics (RE trace): `LogItemModuleChecks`,
     `LogItemButtonAndMapItems` (prompt path 0x012FD880 / search 0x017DA500); trim later.
   - **Guest pickups persist, rig-confirmed (2026-10-07):** the join reload brings the host's
     event flags, and the guest's own save never sees what it picked up there.
     `KeepGuestProgress` polls the ItemLotParam getItemFlagId flags (`IsMapPickupFlag`, 969 of
     them; EventFlagMan `[0x0553B100]`, read via `0x013CFD80(man, flag, 1)`) while the guest is
     in a session, keeps new ones for good in `user/bloodborne_party_progress.txt`, and sets them
     (SetEventFlag binding) before map items are created on every load
     (`SetGuestPickupsBeforeMapItems`, from the item creation hook) and once after 10 s at home.
   - **Trophies: built, NOT rig-tested (Kaniel 2026-10-07: mark untested).** shadNet
     `POST /bloodborne/party/trophy` (`PartyTrophyBoard`, union per party for the server's
     lifetime); np_trophy reports the game's own unlocks (`SetTrophyUnlockObserver`) and applies
     the party's (`UnlockTrophyFromParty`, the game's last context/handle). Unlocks made in a
     party session are posted every 3 s by the rest board thread.
4. **Shared progression:** lamps lit, shortcuts and so on reach the guest's own save.
5. Then bosses (below). Kaniel: one item at a time.

#### Shared progression (2026-10-07, Kaniel chose "everything")
- `LogFlagChangesInSession` diffs the whole event flag store (`ReadFlagStore`, EventFlagMan
  `[0x0553B100]`: block map +0x38, +0x1C flags per block, MSB first) every 3 s in a session.
  A guest keeps the changes (`CarriesToGuestWorld`: not 4xxx, not pickups; >64 at once is a
  load) per character slot in `user/bloodborne_party_world_slotN.txt` and sets them at home
  after 10 s. The slot is the last `/savedata0/userdata000N` opened (`NoteSaveFileOpened` from
  kernel open). Pickups: `bloodborne_party_progress_slotN.txt` (kept for good).
- Guest changes also go to the host: shadNet `POST /bloodborne/party/flags` (`PartyFlagBoard`);
  the host sets them (`SetPartyWorldChanges`). Needed because a guest lighting a lamp only sent
  7116/70000030 to the host, not the lamp's own 12417830/31, so the host's lamp got stuck.
- Lamp lit / shortcut / boss flags seen: lamp 6715 7116 70000030 12417830/31; shortcut
  12410102/17 62411303; boss 2411 7017 9456 12411700/01.
- Boss loot: echoes x0.5 for phantoms at `0x018FC947`, `0x018FCAA7`, `0x01335589` (now 1.0);
  boss events patched by `tools\re\party_boss_events.py` (1003:105 client state, 4:3 "player
  alive" wait moved to an unused condition group).
- Lost echoes: a join clears GameDataMan +0x28 but keeps the record; `KeepGuestBloodstainMark`.
- **Lamps are shared both ways** (rig-confirmed 2026-10-07): every member posts the lamp active
  flags it saw turn on (`SharedWithParty`/`FlagsToShare`, shadNet `/bloodborne/party/flags`) and
  sets the other members' ones (`SetPartyWorldChanges`); the game itself only copies a lamp's
  counters. Nothing else goes through the board: sending everything moved the host along on the
  guest's lamp trip (7MMM01LL) and took the host's messenger gifts. Trip flags (lamp, headstone)
  never go into the guest's own world either.
- **Lamp lit by another member** (rig-confirmed): the first registration of the lamp for a
  character (`0x0146DA20`, from the bonfire registration `0x01327794`) wrote its flags
  base..base+11 off. Two hooks keep flags already on.
- **Flag tracer** (`bloodborne_write_watch.cpp`): hardware write watch (DR0) on one flag's byte,
  logging `flag tracer: write ...` with the stack. Armed for lamp active flags on the host, or for
  `SHADPS4_BLOODBORNE_TRACE_FLAG=<flag>` (rig launchers: 12101020). Found both bugs in one run each.
- **Messenger gifts are per player** (rig-confirmed): the other player's set-flag message
  (`0x017C0565` hook) and event-end message (`0x017C0901`) leave 12101020-29 and the talk picks
  72101000-99 alone (`KeepsLocalFlag`), and `party_boss_events.py` prepends 2000:2 0 (no network
  sync) to the m21_00 gift events 12101020-28; map events are otherwise kept in step between
  players. Diagnostic: `the other player sends flag ...` for 1210xxxx/7210xxxx.
- **Mist ambushes** (not play-tested): enemies that rise out of the mist (2003:18 anim 6200) woke
  only for the host's approach: their event waits for player 10000 near the enemy and alive in
  its own world (4:3 type 0) in one group. `party_boss_events.py` (AMBUSH_EVENTS) moves that
  check to an unused group in m22_00 12204000 (Hemwick woods), m24_01 12414000, m25_00
  12504000, m26_00 12604000 and m29 12905406, so a guest's approach wakes them too.
- **One-time enemies** (rig nightmare-flags 2026-10-24): the Wandering Nightmares and the like
  (m24_00 12400200, m23 12300220, m28 12800500 and 9 more templates, a slot per enemy with its
  drop's flag) go for good once their event ends: the slot flag switches them off (2004:5) or hides
  them (2004:29) at every load. The event ends when an alive player (4:3 10000 0) has the drop's
  flag, but a client skips that check (1003:5 1 1). A party guest reads alive after a trip, so its
  game ended every one on the map with no kill and the session's sync and the team's world took
  them off for everybody. `party_boss_events.py` (keep_one_time_drop_checks) makes that skip never
  match. Saves and team worlds that already have the flags keep them.
- **Death triggers** (rig-confirmed): events set the respawn point on a death (common 9400: first
  death -> Dream 2102962; 9404: Dream 2102961; 9421: kidnapping (flag 9420) -> Gaol 2802959). A
  party revive goes there, travelling 0.2 s later when it is on another map
  (`ChooseDeathRespawn`, also covers a last lamp on another map). `party_boss_events.py` lets a
  client run those events (1003:6) and the Dream's first visit (m21_00 9401: cutscene + first
  Yharnam lamp, so the guest's headstone works). Kidnapping not tested.
- **Boss item popup**: the guest's 2003:36 award (`0x017DDC50` from `0x0132E4B5`) is held and
  given 8 s later (`DefersItemLotAward`); in the boss-clear sequence its dialog left the guest
  unable to press X. Also `MenuSeesAClient` (menu condition `0x01A13FE5`). The first build had a
  broken hook (prologue cut mid-instruction); the fixed one is NOT rig-tested yet. Test first.

## Friend test (Kaniel hosting, a friend as guest, SeamlessCoop kit, 2026-10-07 evening)
The kit's shadps4.exe and event files match the rig (phase3-travel). Host logs:
`C:\Bloodborne.PC.Online - Copy\SeamlessCoop\user\log`; the guest's (friend's) logs are copied
to the log folder of the shared download.
- **Guest stuck without X after Gascoigne.** The guest's log shows the kill (+3200 from
  0x01385263; flags 2410 2412 4000 4047 ... set, 4000/4047 cleared later) but no item award at
  all, so the boss-clear sequence itself blocks X, not the item dialog (the 8 s item hold is
  beside the point). `LogActionBlockingState` ("Bloodborne action state: ...", every 0.5 s on
  change) now logs the candidates; next step is a guest boss kill in the rig.
- **Crash catcher (2026-10-08):** an unhandled exception writes `user/log/crash-<time>.txt`
  (code, address, eboot offset, thread, registers, the eboot words on the guest stack) and a
  minidump. First catch (guest, 2026-10-08 20:14, solo between sessions, lamp trip Cathedral
  Ward -> Dream): access violation reading 0 at eboot+0x1F3DD51 in thread EzWorkPool_TypeB3
  while the Dream's entryfilelist loaded. 0x1F3DD51 is the game heap's free-list unlink (tagged
  pointers `and ~0xF`), so the heap was corrupted earlier; the minidump lacks the guest stack,
  hence the stack line added after it. Read crash-*.dmp with `%TEMP%/bbre/mdstack.py`-style
  parsing (lldb here has no Python).
- Second catch (guest, 2026-10-08 21:20): Game:Main, read from 0x4 at eboot+0xBAFC0 inside a
  destructor chain (0x23D3270 array destroy, 0x01F586F5 free, 0x01834930 virtual destructor)
  while its world was torn down for a headstone trip out of a session. Both catches: a guest
  outside a session, its bell search (9005) on, a trip's world teardown. Suspect: the forked
  keep-matching patch carries the search over the teardown (`ShouldEndSessionOnWorldTeardown`
  leaves searches alone on purpose, for the cross-map handoff). The teardown now logs its
  matching state and effects to confirm before changing that.
- **Crashes** "sometimes after certain actions or boss kills" (no pattern known). All four runs
  (both players, both runs) closed cleanly, so none was captured.
- **Boss fog after a world reset:** the guest died, went to the Dream, the location was reset
  twice; back at Gascoigne the guest saw the boss fog and could not pass (the Bosses item below).
- **Pulled to the host at the Cathedral Ward:** instead of joining by bell the guest was
  teleported to the host, where joining is impossible because the bell is disabled there until
  the key is used. Kaniel's guess (likely): a leftover of the forked seamless patches, whose
  cross-map candidate selection summons a guest straight onto the host's map. In the friend's
  second run the guest loaded m24_00 and then only searched (effect 9005), never joined. Fix
  idea: don't take a sign across maps while the host's area forbids multiplayer, or let the
  party join there anyway. **Kaniel's call (2026-10-07): "join everywhere anywhere anytime
  instantly"**: a party joins in every area and state, including areas the game locks for
  multiplayer (Cathedral Ward before the key, boss areas, etc.), with no wait.
- **Boss HP bar** sometimes did not update for both players' damage.
- Connection problems (already in the backlog: slow joins).

## Automated rig tests (2026-10-09): test every change without players
Kaniel's ask: "you should be able to test every change yourself".
- **Autoplay** (`SHADPS4_BLOODBORNE_AUTOPLAY=<script>`): the pad reader (pad.cpp ProcessStates ->
  `AutoplayPad`) presses Cross on the title screen until the player is in the world (about 25 s),
  then the party tick runs the script (`RunAutoplayStep`): wait, wait-session, wait-members,
  wait-map, travel / ensure-map (the direct warp lands at (0, 0, 0) over the void: the respawn
  point is written again and `MoveToRespawnPoint` runs after arrival), hurt, kill (the game's kill
  0x018BF9A0; it kills bosses, but holds back an ordinary enemy's death when
  `[[chr+0x18]+0x30]+0x18 & 1`), attack (ordinary kills: 1 HP, then the player is teleported
  beside it, heading written at transform +0x1D4, and R1 pressed, a side and a quarter turn per
  1.5 s cycle until it dies, so the kill reward and death run as in play; `AttackStanceFor`),
  mark, approach, teleport, press, stick, set/expect/wait-flag, godmode, snapshot,
  expect-hp/session/map. Each step logs `Bloodborne autoplay: step i/N ... ok|FAILED`, then
  `done (ok|failed)`.
- **Runner**: `py -3.12 tools\rig\run_scenario.py <scenario>...` (`--list`, `--save-baseline`,
  `--add-guest2`). For each scenario it restarts the rig's shadNet/backend (only what listens on
  the rig's ports 32313/20453; a killed game's summon sign stayed claimable for 75 s and the next
  test's host claimed it, so three tests in a row never joined), refuses an old shadNet without
  `/bloodborne/party/hp`, puts every role's saves back to `rig\<role>\baseline`, launches each
  role (host, guest, guest2) with its script, waits for done, retries once on a crash (the GPU's
  device lost shows up in the rig too), and checks steps, `expect`/`forbid` lines and
  check_party_log problems. It stops only its own games (the build's shadPS4.exe) and does not
  start while another copy runs (someone playing). Every run's game logs, scripts and server logs
  (summon request bodies traced) stay in `rig\runs\<when>\<scenario>\` (last 20 batches).
  Scenarios: `tools\rig\scenarios.py`; their commands, roles and patterns are unit-tested
  (`tools\rig\tests`, part of `ci-check.ps1 -Component tools`).
- Bugs it found on its first runs, all fixed: a guest's trip start posted kills (host hid living
  enemies); the HP board kept a quit member's report (next session's enemy started hurt); the
  guest found the host's online id only deeper in the identity object.
- Then (2026-10-09 morning), fixed: the kill board took only deaths with +0x1E3 bit 0 (the
  game's kill); a fight's death sets bit 2 (0x018C52D1, the handler that gives the local player
  its echoes), so almost nothing was shared (Kaniel's friend-run log: 67 kill rewards, 2 kills
  posted); `IsRealDeath`. And the guest's resume ran after the guest had already travelled on
  (it decides once settled 3 s on a map): the rig's guest went to the Ward and was taken back to
  Central Yharnam, away from its host; now a guest no longer on the map it loaded on stays.
- Then (2026-10-09 midday, after a reboot freed 34 GB that crashed games had left with the GPU
  driver), each found by a rig scenario and fixed:
  - **Joins across maps never happened** (join-apart): shadNet kept a party sign on another map
    in Preparing until its guest came to the host's map, which a guest joining in place never
    does. A party's sign is offered at once (spoofed beside the host) and its claim delivered on
    the guest's own map (server `SummonBroker`, party signs only).
  - **A guest whose host crashed stood on as a phantom** (host-crash) and a phantom never rings.
    `StayWhenTheHostLeaves` now stays only while the session has someone else; otherwise the
    game takes the guest home and it resumes where it was (`SkipsLeaveLimit`).
  - **The host reused a dead guest's connection** (guest-crash): the host's game closed it and
    opened it again for the restarted guest, and the emulator handed back the old one because
    the dead peer never closed its side. A connection this game closed is never reused
    (`LingeringConnectionIsFinished`).
  - **The host went to the title screen** when its guest's game died apart from it
    (crash-apart): the game ran the leave limit (event 4046, from eboot+0x1393323) on the host,
    and its warp home found no map. A live player always skips it (`SkipsLeaveLimit`).
  - **Joins could miss**: a guest hears of a claim only when it re-posts its sign, the host's
    connection reaches it at once, and its game drops a connection it cannot place after ~5 s;
    with a re-post every 5 s a claim right after one came too late. Signs re-post every 1 s.
  - **False kills**: a guest's world loading on the same map (new objects, `SeenCharacter`
    instance), and a guest taking over a map's enemies when the host left, put down enemies its
    own save had killed for good (Father Gascoigne, 0x1001409c/0x1001409e, dead flags 0x26), and
    the host hid its living Gascoigne. Deaths in the 5 s after a takeover or hand-back are not
    kills (`EnemyOwnerChangeSettles`, per character), nor deaths in the 5 s after a load (many
    flags at once) or after a guest starts or ends running the map alone (`world_changed_ms`,
    `CharacterSettles`): the dog still posted Gascoigne, who was at 0 HP in the host's world, so
    the takeover (living enemies only) never marked him, and the guest's own world put him down
    the moment it took the map over.
- **Boss progress stays separate** (Kaniel, 2026-10-09): bosses beaten together count for both;
  one beaten before teaming up stays with whoever beat it (no union of boss flags). The kill
  board is for ordinary enemies only: hiding a boss's body would leave the host without its
  "defeated" flag, key and lamp. Scenario boss-refight: the host clears Gascoigne's flag
  (12411800) and kills him with a guest whose save beat him; nothing may hide him before.
- Rig-confirmed on 2026-10-09: boot, join (the guest keeps its max HP, no level sync 9200),
  trip, hp-apart, boss-kill (Cleric Beast), lamp-share, lone-guest, kill-apart, echoes-apart
  (+202 to the guest in the Ward, +202 to the host in Central Yharnam), host-death,
  guest-death, join-ward, join-apart, join-after-trip, host-crash, guest-crash, crash-apart,
  resume (the guest back at the spot it stood in the host's world), join3 (one bell summons
  both guests), trip3 (the host re-creates only the guest on its map, then the second guest),
  dog (the guest kills an enemy while the host is in the Dream; the host finds it dead and
  nothing else hidden), boss-refight. The last build of the day (aab45f97, in both kits) re-ran
  dog, boss-refight, kill-apart and join; the rest passed on that day's earlier builds.
- Memory: a rig game commits about 12.3 GB (its 8.25 GB SEC_COMMIT backing, address_space.cpp,
  then Vulkan); the PC's commit limit is about 80 GB. Games that died in the GPU driver ("Device
  lost") seem to leave their memory held until a reboot (a clean kill gives it back). The runner
  checks the free commit first (12.5 GB a game), closes its games gently (taskkill, then /F
  after 15 s), and a game that exits without a crash report counts as a crash (one retry). A
  role's "crash" (a stage ending in done) is a hard kill, which leaves 0.2-0.4 GB each time.
- Gone (6a7b26ff): `waiting_to_meet` (GuestApart) kept a guest that travelled on from ringing
  until the host was on its map, so after a host crash it never rejoined (host-crash-apart, red
  then green). The join's stored placement (the guest's own spot) is now spent when its session
  ends (`JoinPlacementLife`); kept, it warped the guest back to where it had first joined. And
  `IsRealDeath` skips deaths with bit 0x20 (a world's put-down, Gascoigne 0x26): the dog's false
  kill was a race the settle lost. On 6a7b26ff the rig ran host-crash-apart, join3, trip3, boot,
  join, trip, kill-apart, hp-apart, boss-kill, boss-refight, join-ward (all passed), dog (failed
  before the 0x20 rule) and lamp-share (failed: the flag was set within the post-join reload's
  3 s diff; the scenario now waits 30 s). Not yet run on it: join-apart onward, dog, lamp-share.

## Friend playtest 2026-10-09 afternoon (6a7b26ff kit, three players) — open
Logs and crash dumps: project library `bug-reports/2026-10-09-friend-forest-crash/` (one guest's
shad_log.txt / .prev.txt, two crash reports). Kaniel wants them fixed after the bloodborne_pc port.
- **Cathedral Ward crash** still happens when the party goes there together.
- **Forest after Amelia:** a guest's game crashed; after it joined again it saw Amelia's fog wall
  as if the fight were ahead, and the party got random amounts of echoes (Kaniel suspects bosses
  being killed: likely the kill board or HP board after the rejoin; check its log for "kill board
  gets" around the rejoin).
- Crash reports: `0xc0000005` at eboot+0x1524b24 on CSChrThread5 (read from 0x18; stack
  eboot+0x1515677, +0x2032405) and at eboot+0x1d6fa68 on GXWorker:1 (read from -1; deep render
  stack through +0x227e455/+0x1d20101).

## Friend runs 2026-10-08/09 (online kit, Kaniel + friend over Radmin VPN)
All built, CI green, pushed on `phase3-travel`; none confirmed in a game yet unless noted.
- **Boss kill outside a PSN friends party** (945aff4d): Lua `BlockClear2_1` sends a live player
  not in a friends party to `SoloBlockClear` (compiled at 0x013859F0), which ends with
  StopRequest and a tail jump to LeaveSession. Patched to return after the flags (0x01385A4A).
  The rig accounts take the friends-party route, so the rig never hit it.
- **Bell spam and Insight duping** (9edd8ec0): the guest's world file carried flags 9180-9199;
  9185 ("a bell is ringing") made common event 9183 refund 1 Insight on every load.
- **Second dog** (c41d647e): a visit ended when the player pointer changed (a guest rebuilt
  when the host arrives); now `VisitTracker` (trip, map change, reload outside a session).
- **Join anywhere** (3b338b33): the co-op area table check (0x0191A750, flags per area/block,
  Cathedral Ward 2405/2406 until the player's own world opened the gate 12400160) reads as open.
- **Time-of-day reload** (31a20f7f): Gascoigne's door cutscene (2002:6) reloads through the
  stage load at 0x0193C5xx (StopRequest returns to 0x0193C65C), not a stage transition; in a
  party it starts a trip.
- **Enemy HP** (f6e12ff5): since seamless travel each member runs its own copy of a map, so
  the host's hits never reached the guest's Gascoigne. `/bloodborne/party/hp` (map state board
  format, `EncodeEnemyHp`): each member posts the enemies it hurt, others lower theirs to the
  lowest share, never to 0.
- **Parties of three** (1ad074d2): host rings again while hosting with room (`room_for_more`);
  board members are online ids (hex); after a trip only session members on this map are
  re-created (`PlanRecreate`, identity object scanned for the online id, fallback to "everyone
  here"); host-on-my-map from the host's map state post; two guests without the host both run
  enemies; echoes carry the kill's map (members there skip them); the kill board names posters.
  Unverified: that a member's identity object holds its online id (watch for "re-creating
  <names> on map" vs "re-creating ?" in the log).
- Host crash at the Cathedral Ward door (2026-10-09 00:52): `Device lost` in
  `vk_presenter.cpp:1135` (GPU driver / emulator), not co-op.
- The friend's game fails the sign-timer signature at 0x018A9757 (first two sites match); the
  log now shows the bytes found.

## State on 2026-10-08 (Kaniel's rig runs)
Rig-confirmed today: a guest enters a boss fog first and starts the fight (13 maps patched by
`party_boss_events.py`); the X button after a boss kill (BlockClear2's SetLoadWait skipped);
headstone lamps after a trip (world changes applied once per trip and on arrival); quick re-summon
(signaling: a connection both sides closed is never reused); a guest alone on a map runs its
enemies (ownership word in WorldChrMan +0xA40, `ClaimEnemiesWhenAlone`); kill echoes shared
apart (`/bloodborne/party/echoes`); the host's map state taken by a guest loading its map
(`/bloodborne/party/mapstate`, the dog).
Built, not rig-confirmed: boss co-op HP bonus on a hidden boss (`CoopHpScale`; note the game
recomputes max HP every frame at 0x01A0D881, so the write may not stick), Gascoigne's voice
loop (should follow from the HP fix), boards shared outside a session (lamps, trophies,
`SharesWithParty`), enemy deaths across loads (`/bloodborne/party/kills`, `VisitKills`), a
guest's map changes reaching the host (`SharedWithParty` with `TakesHostMapFlag`).
Open: the teardown crash suspect (bell search kept over a trip's teardown, see the friend test
notes), the Doll/Yosefka hidden NPCs (mirror +0x28), lost echoes whose holder dies while the
owner is away, guest cutscenes freezing the host's enemies (flag 9180).
Diagnostics to trim later: nearby enemy log (vtables, ownership, effects), boss HP log,
teardown log, enemy HP tracer (SHADPS4_BLOODBORNE_TRACE_ENEMY_HP, off in the launchers).

## Bosses
Order (Kaniel 2026-10-07): bosses first (including the guest's stuck X after a boss kill, the
boss fog after a reset and the boss HP bar from the friend test), then "join everywhere anywhere
anytime instantly" (Cathedral Ward pull, slow joins), then the rest of the backlog.
- A guest triggers a boss the same way the host can (enters the boss fog first, starts the fight).
- No boss fog a guest cannot enter until the host goes there (phantom fog-wall gate, plan item 6).
- **Boss fog for guests (2026-10-08, built, NOT rig-tested):** a phantom's fog event (m24_01
  12414731) waits for the host's "entered" flag H (12414700, set by the host's 12414730) and
  sets H+1; the boss start (12414702) waits on H. `party_boss_events.py` now moves the wait on H
  to an unused condition group and turns the final set into Batch Set H..H+1 (2003:22, args
  appended to the arguments table), in 13 maps (m29 chalice templates and m32 13204831 skipped).
  Arena-walk entrances that set H (12411702 Gascoigne, 12411802) no longer require an alive
  player. The phantom event isn't synced, so the emulator posts H to the host over the flag
  board (`IsBossFogEntryFlag`, `SharedWithParty`); H never goes into the guest's own world.
  Test: py -3.12 -m unittest discover -s tools
e	ests. Rig: guest enters Gascoigne's fog
  first, then after a guest death/reset.
- Rig-test that a boss kill keeps the session (`BossClearKeepSessionPatches`, merged, untested).
- **Full boss loot for the guest** (Kaniel 2026-10-07): on a boss kill the host got 4000 echoes
  and the boss item, the guest 2000 and no item; both should get full loot. Boss echoes skip
  the normal kill-soul path (NpcParam isSoulGetByBoss, +0x149 bit 4, checked at `0x018C51C3`);
  look for the boss reward's client half rate and the host-only item lot award.
  Boss kill flags seen in the guest's world copy: 12411700/01/02/03, 12414700-12414733, 7017.
  - **Echoes fixed (6ff9843a, not rig-tested):** every kill reward multiplies a summoned guest's
    echoes by 0.5 (`0x018FC947`, `0x018FCAA7` load it); party clients load the 1.0 instead.
  - **Boss item (not rig-tested):** a boss defeat event (e.g. 12411700) runs 2003:12 (boss kill,
    echoes), then 1003:105 "end the event by multiplayer state" with state 1 (client), then the
    trophy (2003:28), AwardItemLot (2003:4) and progress flags (2411, 9456). The native check
    wasn't found; instead `tools\re\party_boss_events.py <mods event dir> <game event dir>
    <update event dir>` writes map event files whose boss events compare against a state that
    never happens (26 checks in 16 maps), into the rig's `CUSA03173-mods\dvdroot_ps4\event`.
    Every party member (friends too) needs these files. Instruction names are in
    `event\common.emedf.dcx` (UTF-16 strings, e.g. "マルチプレイ状態でイベント終了").

## Backlog (Kaniel 2026-10-06: "add all of the current findings and problems to the backlog")
- **NPC hidden on the host shows on the guest: the switch is known (2026-10-08).** A character's
  byte +0x28 is the event disable ("hidden by the story"): when it is on, WorldChrMan's
  activation pass (`0x019192A0`) sets the character's wake-up target ([+0x18]+0x14) to 0 and it
  is neither drawn nor run. Clearing it on a lone guest showed exactly the NPCs the host's events
  hide (Cathedral Ward, like Yosefka in her clinic). Fix idea: post the host's +0x28 per entity
  id (entity lookup: WorldChrMan +0x90/+0x98 tables, see `KeepBossHpInStep`) and mirror it on the
  guest. Kaniel asked for this to be fixed.
- **NPC hidden on the host shows on the guest** (Kaniel, 2026-10-06, two new characters, Hunter's
  Dream workshop): the guest sees the living Doll standing while the host's is hidden (the host's
  event scripts disable it before the Insight story step). Likely the event-side disable
  ([+0x18]+0x20 bit 0, `0x018C6390`) only runs on the host and isn't sent to clients; before the
  solid-NPC fix the guest had the Doll as a see-through wandering ghost instead. Idea: mirror the
  host's disabled NPCs to the guest (party board) or run the guest's own map events for NPCs.
- **Window monster after a rest:** a hostile character near the Central Yharnam lamp (only meant
  to appear after a quest) appears the moment a party rest refreshes the world, on fresh saves
  too. Bisected: not the lamp use, not WorldChrMan +0xA40, gone when the respawn is skipped. Not
  explained by the disable bit, the +0x1E0 flags, dead-since-load, the event restart or the
  +0x359 freeze (no character was frozen by the scripts). Logged suspect: handle 0x1001400c,
  alive but inactive (control state 0) before the rest and active after, around
  (-198, -28, 61). Untested suspect: the respawn's per-block call `0x018DAD90`. Alternative:
  a real reload of the whole party at the lamp once "travel together" exists.
- **Breakables** (boxes) are not restored by a rest (objects are outside the chr respawn); Kaniel
  also saw boxes vanish, possibly the rig's low-spec profile. Check without the profile.
- **Both rig clients crashed** during a second rest in a build with the freeze/thaw experiments,
  running without Potatoborne; the experiments are reverted and the re-hide now checks
  pointers against the live world. Watch for it.
- **Living wounded enemies keep their damage** after a rest (only the killed respawn).
- **Phantoms:** guests get no lamp prompt (rest by standing near instead) and see quest
  characters as transparent phantoms; Kaniel wants guests to use lamps and interact with quest
  characters (Phase 4).
- **Full lamp menu** like Bloodborne Enhanced: rest, level up, travel, storage, workshop, shops.
- The travel dialog still says "Travel to the Hunter's Dream?" for a rest.
- Diagnostics to remove once the window monster is solved: `LogCharacterChanges`,
  `LogCharactersActivatedByRest`, the lamp availability and talk-script counters.
- Rig: Potatoborne drops enemy corpses at once (Kaniel saw bodies vanish); without it two
  instances run out of memory more easily. A join once failed until both games restarted
  (claim delivered every 5 s, guest never joined).
- **Changes made while the other player is loading (Kaniel 2026-10-07).** A messenger gift
  picked while the other player was on a loading screen stayed for both, so the game's own sync
  (set-flag 0x017C0570, event end 0x017C0901, event network sync) behaves differently during a
  load; other progression may be lost or doubled the same way. Plan: a rig test matrix (lamp,
  shortcut, boss kill, pickup, NPC talk, each with the other player loading), a log line for
  every sync message received while loading (and whether it applied), then catch up after the
  load: re-post shared flags (lamps board) once the stage is up, and compare the guest's kept
  world changes against its flag store on arrival.
- **Death respawn polish (Kaniel 2026-10-07, only if it cannot break anything):** a death that
  travels (`ChooseDeathRespawn` Travel) shows the body standing up at the death spot for ~0.2 s
  before the loading screen; hide it. A death respawn on the same map (MoveToPoint, vtable
  +0x5F8 teleport) just slides the player oddly; give it a loading screen too, like a trip.
- **Slow joins (last in the backlog, Kaniel 2026-10-07).** The host picked the guest's sign three
  times (19:18, 19:19, 19:21; rooms 16-18), shadnet delivered each claim within seconds, but the
  host only activated signaling to the guest at ~19:22:55 (host log: CreateJoinRoom, then nothing
  until sceNpSignalingActivateConnection); the guest joined at once after that. One claim had
  host-placement-bytes 0. shadnet also stopped silently at 20:37 the same evening. Rig launchers
  now keep the previous run's log as `shad_log.prev.txt`.

- **Lost echoes held by an enemy, killed while the owner is away (Kaniel 2026-10-08):** side by
  side the shared enemy holding a member's echoes works. Broken: the guest died to the first
  wolf, the host killed that wolf while the guest was in the Dream; the guest got nothing, and
  after joining its game brought back its own local copy of the wolf (red eyes, attacks only the
  guest, can't hurt the host). Kaniel says this desync predates our work. Fix: when the holder
  dies while its owner is apart, the owner gets its echoes back (or a bloodstain), and its game
  doesn't revive its copy. The bloodstain record (GameDataMan +0x30: +0x18 position, +0x38
  echoes, +0x3C block, +0x30 always -1 so far) does not name the holder; find where it is kept.

- **A guest resumes where it left (Kaniel 2026-10-08, session improvements):** after hours in a
  session, a guest that quits or disconnects restarts at its own save's position (where it was
  last summoned from) and has to walk back. Its game should remember the map, position and world
  state it last had in the session, load there next time, and be summoned there.

- **Slow joins are back (2026-10-08 22:42, new characters):** shadnet: claimed 22:42:21.98,
  delivered the claim to the guest every 5 s, a "repeated claim" from the host at 22:43:17.03,
  and the host's RequestSignalingInfos to the guest at 22:43:17.79 (56 s after the claim; the
  join four minutes earlier took 1 s). Host: CreateJoinRoom, join task kind 3 step 0 -> 6, then
  nothing until the activation. Guest: JoinRoom only after the host's activation. Suspect: the
  host's first claim goes unanswered and its game retries after a timeout; find what the host's
  join task waits for at step 6.

- **A guest sometimes loses the host after the host's Dream trip (rig only, Kaniel 2026-10-17:
  not seen in play, may be the test).** smoke's trip chapter (run 20261017-010558): the host goes
  to the Dream and back while both guests stay on Central Yharnam; the host re-creates both
  guests, but on both guests the host's character is gone 20 s later (`expect-others-near 2 15`:
  only the other guest) and nothing logs its leaving or a re-create (only the member that
  travelled re-creates the others). Intermittent: 1 of 3 smoke runs that night, the trips
  suite's known_bug since 2026-10-15. Kaniel reports it if he sees it in play.

## Known loose ends
- shadnet logs `unhandled POST /v1/users/<npid>/feed`. Harmless so far.
- The backend answers `ChairMessRespawnPointNotice` generically.
