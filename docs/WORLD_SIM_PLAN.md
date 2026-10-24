# Enemy AI on the server: the world sim

Proposed 2026-10-24 for Kaniel's review (not agreed yet). Kaniel: "I am basically still running the
game on one of the players' clients and retranslating it through the server. What I want to do is
always run it on the server", for the whole map at once, with no per-area limit, and without a
headless copy of the game. This replaces the "headless world host" in docs/SERVER_COOP_PLAN.md
("Later, not planned now").

## Today (phase 4, docs/SERVER_COOP_PLAN.md)

shadNet names one owner per occupied map (`EnemyOwners`, server/src/bloodborne_party_boards.h). The
owner is a player's game: its enemies think, the others' copies follow its ch 4/5/0x29/0x36, and
their hits go to it on 0x15. What that costs, from the handoffs and play tests:

- **Ownership moves with the players.** A load, a move away or 2.5 s of silence hands a map on; the
  host takes a map back after 3 s. Each move restarts enemies from the new owner's copies (the
  hurt enemy that went back to its spawn, 2026-10-18), and needs `EnemyOwnerChangeSettles` and the
  5 s HP window to stay correct.
- **A map with no player has no AI.** The game wakes a block's enemies only near a player in that
  world, so enemies stand still or reset whenever nobody is there.
- **The owner's game is everyone's enemy clock.** Its frame rate, stutters, cutscenes (flag 9180)
  and crashes (owner-crash) reach every follower.
- **Two kinds of game.** Owner and follower run different paths (the `+0x124 == 3/6` answers, ch 5
  only from the owner, 0x15 forwarded), and every feature is tested both ways.

## Goal

A **world sim**: one process on the host's PC, next to shadNet, that runs the enemies of every map
of the team's world, all the time. It is the permanent owner of every map it runs; every game is a
follower everywhere. The sim is not the game: it has no player, no rendering, no menus, no loading
screens and no per-area limit. It keeps every block's enemies awake at once.

The seam already exists. Phase 4 made enemy authority "one owner per map, chosen by the server",
and the games already know how to follow an owner that is not the session host. The sim joins as
that owner and speaks the game's own enemy channels, so the games need little or no change.

## How it fits

```text
 Each player's game (follower on every map)        Host's PC
 ┌───────────────────────────────┐   UDP 31314   ┌───────────────────────────────┐
 │ bbport + co-op code           │◄────────────►│ shadNet: lobby, world log,    │
 │  own player: ch 6 out         │               │  relay, EnemyOwners           │
 │  enemies: follow ch 4/5/0x29/ │               │      ▲ local link             │
 │   0x36/0x3D from the owner    │               │      ▼                        │
 │  hits on enemies: 0x15 out    │               │ world sim (owner of all maps) │
 └───────────────────────────────┘               └───────────────────────────────┘
```

- **shadNet (this repo):** a sim member signs in with its own hello (`world_sim`, the maps it
  runs). `EnemyOwners` gives it every map it runs, ahead of any game; maps it does not run keep
  today's rules, so the switch can go map by map. Kills and HP reports on a sim map come from the
  sim only. Players' ch 6 and 0x15 are routed to the sim; its enemy channels go to every game on
  that map.
- **The world sim (private repo, lKaniel/bloodborne-recomp):** its decompiled code must not be
  public, so it lives with the recomp, not here. It is C++, loads the host's own 1.09 eboot at
  runtime (as bbport does) and calls the game's code for what is not decompiled yet (Havok, Lua,
  the AI's helpers), without running the game's boot or main loop. It reads collision, navmesh,
  placements and params from the host's install. Nothing from the game goes into either repo.
- **The games:** followers everywhere; the owner path stays for maps the sim does not run, until
  W5 removes it.

## Stages

Each ends with a rig run, as the phases did; each ships behind a switch until it passes.

| Stage | What | Done when | Needs decompiling |
| --- | --- | --- | --- |
| **W0 Owner seam** | shadNet: the `world_sim` hello, the sim first in `EnemyOwners` for its maps, kills/HP from the sim only, routing of ch 6/0x15 to it. A test sim in `server/tests` that owns a map. | Unit tests; rig with a stub sim that owns one map and sends nothing: the followers' enemies on it stand still, the other maps play as today. | None |
| **W1 Replay sim** | Record an owner game's enemy channels for one map on the rig (kept under `C:\bb\out`, never committed), and have a test sim send them back to a follower. | A follower's enemies move, attack and die as recorded, with no game owning the map. Pins the byte formats of ch 4, 5, 0x29, 0x36, 0x3D and 0x41 for the sim. | None |
| **W2 Eboot as a library** | Private repo: the sim loads the 1.09 eboot, skips its boot, and sets up just enough (memory, files, Havok) to call it. | A map's collision loads and a ray cast returns the game's hit. | A little: the start-up it needs |
| **W3 One map thinks** | All enemies of one map (Central Yharnam's first block) run in the sim: placements, params, Lua AI, animation, physics, targets from the players' ch 6. | Rig: two players fight there with the sim as owner; their enemies' behaviour matches a game-owner run within tolerance. | Character state, the AI's C++ side, animation events (as they are reached) |
| **W4 Hits and HP** | 0x15 into the sim; damage, deaths and drops decided there; HP to the world log from the sim. | Rig hit3, guest-hit-once, hp-together pass on a sim map. | Damage calculation |
| **W5 Every map** | Every block of the world loaded and awake at once, tiered tick rates (near a player 30 Hz, else 1-5 Hz), enemies handed across block edges. Game ownership retired; the owner paths in the games go. | Rig: four players in four areas; host CPU and memory measured. | Map loading and block offsets |
| **W6 The rest** | Bosses, NPC enemies, mist ambushes and other enemies driven by map events, chalice dungeons (one sim instance each), respawn rules. | Per feature. | Event script interpreter, boss code |

W0 and W1 need no decompiling and can start now. W2 is the risk that decides the approach (below).

## Open questions for Kaniel

1. **Respawns.** Today a rest or a wipe resets a map for the members there (`Reset(map)`). With one
   always-running world, what resets enemies: a rest by anyone, a timer, or a wipe only?
2. **Map events stay in the games.** Event scripts (bosses' fog, ambushes waiting for player 10000
   near an enemy, enemies enabled by flags) run in each game today. Until W6 the sim must follow
   the world log's flags and the games' event effects on enemies. Is "enemies driven by events stay
   game-owned until W6" acceptable as a stop-gap?
3. **Decompile order.** The recomp is doing the co-op list now. W3 needs character and AI code
   instead; should the recomp switch to it after its M3?

## Risks

- **W2 may fail:** if Havok or the AI cannot be set up without the game's boot, the fallback is to
  run the game's boot with rendering and the player stubbed out and then take over its loop. That
  is closer to a headless game, but it still runs one process for the whole map.
- **Ties to a local player:** the AI and character code read managers that assume a local player, a
  camera and a loaded area (WorldChrMan and friends). Each one found must be fed by the sim.
- **Unknown payloads:** the 0x60 AI record (0x29/0x3D), the 0x14 record and 0x34 are only partly
  known (docs/NATIVE_NETCODE.md). W1 pins what the followers need from them.
- **Latency:** the enemies are now one hop from everyone instead of zero hops from the owner. The
  followers already play this way today, so it changes nothing for them; it is new only for the
  player who used to own the map.
- **Host load:** the sim adds CPU and memory to the host's PC, which also plays. W5 measures it.
