# Event flags in a party: whose are they?

The game keeps its progress in event flags (numbers up to 2 billion, each on or off). In a party
each flag has one owner, and the co-op code reads it from one table:
`FlagTable` in `emulator/src/core/bloodborne_flags.h` (`RuleFor(flag)`). The older helpers
(`CarriesToGuestWorld`, `KeepsLocalFlag`, `TakesHostMapFlag`, `SharedWithParty`,
`IsMapPickupFlag`, `IsLampTravelFlag`, `IsHeadstoneTravelFlag`, `IsBossFogEntryFlag`,
`GuestProgressFlags`, `ShopLineFlags`) all read that table now. The bugs of 2026-10-11 (Insight
twice, Dream doors shut, the bell, a boss's "found" Insight) were each a flag in the wrong group,
so a new flag rule is a new table row, nothing else.

## The owners

- **Host**: the host's world decides. A guest's copy follows the host while in the session, and
  the changes the guest saw are kept and set in its own world once it is home ("carried home").
  Map flags (12xxxxxx-13xxxxxx) are also set to the host's values when a guest loads the map.
- **Per player**: each player keeps their own. When the other player's game sends "set this
  flag" or "this event ended", it is ignored.
- **Shared**: the game does not sync these, so the party does: a member that sees one turn on
  posts it to the party's flag board and every other member sets it.
- **Session**: only for this session or this map visit. Never carried home, never shared.

Every flag not in the table is **Host, carried home** (quest steps, NPCs, the Dream).

## The table

| Flags | Owner | What else | Why |
|---|---|---|---|
| 0 and below | Session | | not an event flag |
| 4000-4999 | Session | | death and multiplayer bookkeeping (4000 a boss fight on, 4047 a player on the way out) |
| 6630-6633, 6640-6649 | Per player | carried home; follow the player's own items | "holds this workshop tool / hunter badge" (`HeldItemFlags`, set by the Dream's 12100450): the badges' bosses, NPC gifts and map objects read them. As the host's (a team world wrote them into every save) a guest never got its own Sword Hunter Badge from the Cleric Beast (Kaniel 2026-10-18) |
| 9180-9199 | Session | | the bell's bookkeeping (common events 9181-9198); carried home it gave Insight again |
| 9300-9307, 9336, 9338-9344, 9346-9348 | Per player | carried home | a boss's "found" flag: each player gets the Insight for finding a boss (`EncounterInsights`) |
| 9401 | Host | carried home; a guest out in the world sets it itself once its own character has been to the Dream | Central Yharnam reached: the Dream's first visit and its shop (set early, it showed the clinic's lamp and kept a first death from waking in the Dream) |
| 9403 | Host | carried home; a guest out in the world sets it itself once the session's world has 12410900 | the workshop's doors open (only set for a live player) |
| 12101000-12101009 | Per player | carried home; cleared once per save slot | the messengers' weapon lines: each player's follow its own hunter badges, as in the base game (a broken patch once set them all) |
| 12101020-12101029 | Per player | carried home | the Dream messengers' gifts: what one player got is not given to the other |
| 21 boss fog "host entered" flags (12414700 Gascoigne, ...) | Shared | | a guest's fog event sets it to start the boss; holds for one map visit |
| the flag after each of those | Session | | "a phantom entered" (local music) |
| the maps' door flags (`DoorFlags`, 390: each object act's flag, its event's "done" flag, the flags it sets on; listed by `tools/re/door_flags.py`) | Shared | carried home, host's map value | the game only syncs an object act (a door, gate, lever, kicked ladder) to players on the same map: a door a teammate opened elsewhere stayed shut (Kaniel 2026-10-19) |
| a lamp's active flag and the one after (10007810 + map x 10000 + lamp x 20, maps 210-399) | Shared | carried home, host's map value | the game does not sync a lamp lit by another player |
| 12000000-13999999 (the rest) | Host | carried home, host's map value | the maps' flags: switches, bosses, enemies |
| 50000000-52899999, 53000000-53999999 | Host | the guest's own pickup list | map item pickups (kept in their own file, not carried with the rest; never in a team's world: each character keeps its own, as it keeps the items) |
| 52900000-52999999 | Host | carried home | chalice dungeon item flags: not pickups |
| 72101000-72101099 | Per player | carried home | the pick in the Dream messengers' talk |
| 72102000-72103999 | Session | headstone trip | a Dream headstone's "travel to <lamp>": a request, not progress |
| 7MMM01LL, map codes 211-399 | Session | lamp trip | a lamp's "travel to the Hunter's Dream": a request, not progress |
| 94k00000-94k05999, altars k 0-9 | Shared | carried home; clears shared too | a ritual altar's dungeon record: a ritual or a join at one member's altar puts the dungeon on every member's (team altars) |

Rows are looked up in order, most specific first: a flag takes the first row that holds it. Two
rows only overlap when the later one holds the earlier one whole (the maps' flags hold the boss
fogs, the doors and the lamps' active flags); `RowsNestOrStayApart` checks that.

## Tests

`emulator/tests/bloodborne/test_bloodborne_flag_rules.cpp`:

- `DecisionsMatchTheRecordedOnes`: what the hand-written helpers decided before the table, over
  about half a million flags (every row's edges, every flag number the co-op code names, a sweep
  of the whole range, each map's boss fog and lamp windows), is in `flag_rules_expected.txt`. The
  table must decide the same. A deliberate rule change rewrites that file with
  `BLOODBORNE_FLAG_RULES_WRITE=1` (and the diff shows exactly which flags changed).
- `PredicatesFollowTheTable`, `BossFoundFlagsArePerPlayer`, `ListsComeFromTheTable`,
  `SessionFlagsAreNeitherKeptNorShared`, `EveryRowDecidesSomeFlag`.
