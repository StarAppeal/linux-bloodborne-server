# Boss tempo (host rule)

Kaniel 2026-10-28: an optional host rule that makes bosses rest less between their attacks and
turn faster. Levels: **Vanilla** (the default, the game's own), **Aggressive** and
**Relentless**. Only bosses (NpcParam `npcType` 1, the same rule as the boss hand-out); their
moves, damage and HP stay the game's.

| Level      | Breather life (`gap`) | Breathers kept (`odds`) | Turn speed (`turn`) |
|------------|-----------------------|-------------------------|---------------------|
| Vanilla    | x1                    | all                     | x1                  |
| Aggressive | x0.5                  | about half              | x1.3                |
| Relentless | x0.25                 | about a quarter         | x1.6                |

It is wired like the other host rules (`TeamRules.boss_tempo` in the server's team, the RULES
request from the leader only): the launcher's Co-op page (step 2, "Boss tempo") writes
`aggressive` or `relentless` into `SHADPS4_BLOODBORNE_HOST_RULES`, and the team screen shows
"Host rules: ... Boss tempo: Relentless" to everyone.

For tuning, `SHADPS4_BLOODBORNE_BOSS_TEMPO=gap,odds,turn,anim` replaces a level's numbers while
that level is on (gap in (0, 1], odds in [0, 1], turn and anim in [0.5, 4]; `anim` is not used
yet). Vanilla always stays the game's.

## How the AI rests (01.09)

The enemies' AI is Lua 5.0 bytecode (`script/aicommon.luabnd.dcx` and each map's
`script/mXX_XX_00_00.luabnd.dcx`; `tools/re/lua50dis.py` disassembles them). A battle goal's
`Activate` picks an act by weight in `Common_Battle_Activate` (`common_battle_func.lua`); each act
adds its attack sub-goals and returns `GetWellSpace_Odds` (often 70). Right after the act,
`Common_Battle_Activate` rolls `ai:GetRandam_Int(1, 100) <= odds` and, on a hit, runs the act-after
function (`HumanCommon_ActAfter_AdjustSpace` or the boss's own), which adds the breather goals:

| Goal id | Name (`goal_list.lua`)    | What it does                 |
|---------|---------------------------|------------------------------|
| 2000    | `GOAL_COMMON_Wait`        | stands for its life time     |
| 2001    | `GOAL_COMMON_Turn`        | turns to the target          |
| 2016    | `GOAL_COMMON_LeaveTarget` | backs off                    |
| 2017    | `GOAL_COMMON_SidewayMove` | strafes                      |

Every sub-goal goes through one native function:

- The Lua binding is registered under the name `AddSubGoal` at 0x0127FBD4 (the name string's
  only reference), one wrapper per overload (0x0127EC30, 0x0127ECB0, 0x0127ED20, ...). Each
  zeroes the parameters it lacks and calls **AddSubGoal 0x0124CC60**: `(goal rdi, id esi, life
  xmm0, seven float parameters xmm1-xmm7, more on the stack)`. 42 places call it, the Lua
  wrappers and the native goals (combos, approach).
- Its prologue stores the life at `[rbp - 0xBC]` (0x0124CCA4); the hook goes right after, over
  `mov r14, rdi; xor ecx, ecx` (0x0124CCAC), and rewrites that float before the function copies
  it into the new sub-goal (`[slot + 0x28]`, 0x0124CD29). The new goal's id lands at
  `[slot + 0xA0]` (0x0124B8E1).
- The goal's `[+0]` is its AI object: the binding `GetNpcThinkParamID` (0x0125BC00) reads
  `[ai + 0x28]`. The AI's `[+0]` is its manipulator (`IsNpcPlayer`, 0x0125B6F0, calls its
  virtual +0x118), and the AI keeps its **character at `ai + 0x558`**, which it takes from the
  manipulator's virtual +0x30 at 0x0128EDC3 (a rig probe 2026-10-28 found the same pointer there
  for six enemies, each with its own handle; the manipulator's `[+8]` is not the character). AI
  targets resolve at 0x01291E10 (`TARGET_SELF` -1 is `ai + 0x568`). The AI runs on the
  `CSChrThread` workers, several at once.

So the rule needs no script edits: the hook (on the game that runs the boss: an AI runs on its
runner's game only) checks the goal id and whether the AI's character is one of the bosses on the
map (found once a second on the game's thread, `TickBossTempo`), and then:

- scales the life of the four breather goals by `gap`;
- cuts a Wait, LeaveTarget or SidewayMove to 0.05 s unless a roll (0-99) is under `odds`; that
  is the act-after chance times `odds`, and the same for the breathers inside an act. A Turn is
  never cut (the boss would swing facing away).

Lever B could also be the act-after roll itself, but that roll is an ordinary
`ai:GetRandam_Int(1, 100)` in Lua, the same call every other roll makes; the goals it adds are
the cleaner place.

## Turn speed

NpcParam `turnVellocity` (+0x0C, f32, degrees a second; from the game's own paramdef). Boss rows
range from 15 (row 251000) to 180 (212700); Father Gascoigne's row 271000 is 120. The boss rows'
value is multiplied by `turn` in the loaded table, in the same step as the harder enemies (one
kept copy of the table's own values, put back at Vanilla). The step runs on every member's game,
so the guests' copies of a boss turn the same. A boss already on the map when the level changes
keeps the turn speed it was made with until the map loads again (as the harder enemies' HP).

## Animation speed (not in this round)

BB's SpEffect paramdef has no animation-speed field (only `motionInterval` and `animIdOffset`),
so a faster boss animation would need the character's own playback rate at run time (not found
yet).

## Rig

`boss-tempo-vanilla`, `boss-tempo-aggressive`, `boss-tempo-relentless` (Gascoigne fights the
host alone for 90 s; each breather is logged as `goal 2000 Wait life 1.20 -> 0.60 (Aggressive)`)
and `boss-tempo-party` (only the leader picks Relentless; the guest's game turns the bosses
faster too).
