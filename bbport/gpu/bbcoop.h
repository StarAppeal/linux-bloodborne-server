/* SPDX-License-Identifier: GPL-2.0-or-later */
/* bbport co-op: the emulator's seamless co-op (../emulator/src/core/bloodborne_*.cpp) in the
 * native port. What the loader and the C runtime call; gpu/bbcoop/ implements it. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* A segment of the game's image (its address, size, flags: 1 execute, 2 write, 4 read), for the
 * co-op code's memory checks. Called for each segment before bbcoop_install. */
void bbcoop_note_segment(uint64_t address, uint64_t size, unsigned flags);

/* The co-op patches and hooks go into the eboot (image offset 0, eboot_size bytes) when
 * SHADPS4_BLOODBORNE_SEAMLESS_COOP=1 (SHADPS4_BLOODBORNE_RE_TRACE=1: the trace hooks). Called
 * after relocation and the image patches, while the image is still writable. The hooks'
 * trampolines go to `code`, executable memory within 2 GiB of the image. */
void bbcoop_install(unsigned char *image, uint64_t eboot_size, void *code, uint64_t code_size,
                    const char *app0);

/* A fatal exception (Windows: its EXCEPTION_POINTERS) is ending the process: the crash report
 * (user/log/crash-*.txt and .dmp). */
void bbcoop_crash_report(void *exception_pointers);

/* The window is closing: a game whose thread has stopped ticking writes a freeze report
 * (user/log/freeze-*.txt and .dmp) first. */
void bbcoop_window_closing(void);

/* Autoplay (SHADPS4_BLOODBORNE_AUTOPLAY, the rig's scripted tests): the PS4 pad buttons and the
 * sticks (lx, ly, rx, ry; 128 is centred) it holds now; 0 when it is off. */
int bbcoop_autoplay_pad(uint32_t *buttons, uint8_t sticks[4]);

/* Party death (always on in the port): sees what the game will
 * read from the pad (the give-up hold, attacks that revive a downed member) and, while the player
 * is down, lets go of all but Options and the camera stick. */
void bbcoop_party_death_pad(uint32_t *buttons, uint8_t sticks[4], uint8_t *l2, uint8_t *r2);

/* Party death's panel, for the overlay: the other party members playing it (online id, state,
 * HP, revive progress while down; `here` on this player's map), whom an attack would revive now
 * (an index into members, or -1) and this player's own down. */
enum { BBCOOP_VITAL_OFF, BBCOOP_VITAL_UP, BBCOOP_VITAL_DOWN, BBCOOP_VITAL_DEAD };
enum { BBCOOP_PANEL_MEMBERS = 4 };
typedef struct bbcoop_panel_member {
    char name[20]; /* "" when unknown */
    int vital;     /* BBCOOP_VITAL_* */
    int here;
    int32_t hp, hp_max;
    uint32_t landed, needed;
    uint32_t vials, bullets; /* Kaniel 2026-10-18: beside the name */
} bbcoop_panel_member;
typedef struct bbcoop_panel {
    int member_count;
    bbcoop_panel_member members[BBCOOP_PANEL_MEMBERS];
    int revive_target;
    int vital; /* this player's */
    uint32_t landed, needed;
    float give_up; /* the give-up hold, 0 to 1 */
    int native_list; /* the game's HUD shows the party list (the party HUD's CoopParty tags) */
    int native_prompt; /* the game's HUD shows the prompt (CoopPrompt): the overlay draws its glyph only */
    int native_supplies; /* the game's HUD shows the vials and bullets (CoopSupply): the overlay draws none */
} bbcoop_panel;
/* Fills `panel`; 0 when there is nothing to draw (party death off, no party session, nobody else
 * on the vitals board and this player not down). */
int bbcoop_party_panel(bbcoop_panel *panel);
/* Kaniel 2026-10-17: the teammate compass. The party members on this player's map as the camera
 * sees them: bearing in radians (0 ahead, positive to the right), distance in metres, `x` the
 * place on the strip (-1 its left end, 1 its right end; `pinned`: past the strip's span, at its
 * end) and `member` the index into the party panel's members (its colour). 0 when there is
 * nothing to draw: no party session, nobody here, the camera not found, the game's HUD not up (a
 * load, a cutscene). */
enum { BBCOOP_COMPASS_MARKERS = 4 };
typedef struct bbcoop_compass_marker {
    char name[20]; /* the panel's name, "" when unknown */
    int member;
    float bearing, distance, x;
    int pinned;
    /* Kaniel 2026-10-17: its board name (bbcoop_member_portrait's key), BBCOOP_VITAL_* and its
     * revive's progress (0 to 1) while down. Downed markers come last (drawn over the others). */
    char board[40];
    int vital;
    float revive;
} bbcoop_compass_marker;
/* Kaniel 2026-10-20: this player's own bloodstain on this map, placed as the markers (only its
 * own: Kaniel after v1.0.2). */
typedef struct bbcoop_compass_stain {
    float bearing, distance, x;
    int pinned;
} bbcoop_compass_stain;
typedef struct bbcoop_compass {
    float heading; /* the camera's, for the strip's ticks */
    int count;
    bbcoop_compass_marker markers[BBCOOP_COMPASS_MARKERS];
    int has_stain;
    bbcoop_compass_stain stain;
} bbcoop_compass;
int bbcoop_compass_view(bbcoop_compass *compass);
/* Kaniel 2026-10-17: the map screen (an Elden Ring style map). What it shows as the game thread
 * read it last while `open` (the screen is up; the game is read only then): this player's map and
 * where it stands (the game's metres on that map: x east, y up, z north; the camera's heading as
 * the compass's), the party members (on this map: where they stand), every lamp (`lit` for this
 * character; names by the game's text ids: menu FMG 30, the area's item FMG 19) and what would
 * stop a trip now (BBCOOP_MAP_*: GO when nothing would). 0 before the first reading. */
enum { BBCOOP_MAP_LAMPS = 80, BBCOOP_MAP_MEMBERS = 4 };
enum { BBCOOP_MAP_GO, BBCOOP_MAP_NOT_LIT, BBCOOP_MAP_NOT_IN_WORLD, BBCOOP_MAP_DOWN, BBCOOP_MAP_BOSS,
       BBCOOP_MAP_TRAVELLING };
typedef struct bbcoop_map_lamp {
    int32_t warp_point;
    uint32_t map;
    float x, y, z;
    int lit;
    int32_t name_id;
} bbcoop_map_lamp;
typedef struct bbcoop_map_member {
    char name[20];  /* the panel's name */
    char board[40]; /* bbcoop_member_portrait's key */
    uint32_t map;   /* 0 when not known */
    int has_at;
    float x, y, z;
    int vital; /* BBCOOP_VITAL_* */
} bbcoop_map_member;
/* Kaniel 2026-10-20: this player's own bloodstain on its map (any map), with its echoes (only its
 * own: Kaniel after v1.0.2). */
typedef struct bbcoop_map_stain {
    uint32_t map;
    float x, y, z;
    int32_t echoes;
} bbcoop_map_stain;
typedef struct bbcoop_map {
    int in_world;
    uint32_t map;
    int32_t area_name_id;
    int has_me;
    float x, y, z;
    int has_heading;
    float heading;
    int block; /* BBCOOP_MAP_*, for a lit lamp */
    int member_count;
    bbcoop_map_member members[BBCOOP_MAP_MEMBERS];
    int lamp_count;
    bbcoop_map_lamp lamps[BBCOOP_MAP_LAMPS];
    /* Kaniel 2026-10-18: the areas whose main bosses are down (the map shows all of them). */
    int cleared_count;
    uint32_t cleared[24];
    int has_stain;
    bbcoop_map_stain stain;
} bbcoop_map;
int bbcoop_map_view(bbcoop_map *map, int open);
/* A trip from the map to the lamp (a warp point of the view's lamps), started by the game thread
 * if nothing stops it. */
void bbcoop_map_travel(int32_t warp_point);
/* Kaniel 2026-10-18 (fog of war): the cells this character has walked on a map, BBCOOP_MAP_CELL
 * metres square, as (x, z) cell pairs into `xz` (2 * `max` values); returns how many. */
enum { BBCOOP_MAP_CELL = 8 };
int bbcoop_map_explored(uint32_t map, int16_t *xz, int max);
/* An area's name id (item FMG 19) by its packed map. */
int32_t bbcoop_map_area_name_id(uint32_t map);
/* The map screen opens (1 from the game's System menu, 2 from the port's Insert menu or its key)
 * and closes (0); opened from the System menu, its page closes with it (bbcoop_team_menu_back
 * presses Circle a moment later). bbcoop_map_screen: 1 while it is open. */
void bbcoop_map_menu(int open);
int bbcoop_map_screen(void);
/* Kaniel 2026-10-21 ("another 3D map mode that uses the game's own camera"): while asked (on 1),
 * every frame is drawn from the game's camera moved `distance` metres from the spot (x, y, z: the
 * game's metres on the player's map), with the compass's `heading` and `pitch` radians down from
 * level; asked off (or not asked again for a moment), the game's own camera again. */
void bbcoop_map_overhead(int on, float x, float y, float z, float distance, float heading, float pitch);
/* The camera the last frame was drawn with (the overhead one while it is on): its rows (the x
 * axis, the y axis, the z axis it looks along, its position), vertical field of view (radians),
 * width / height, near and far planes; 1 once there is one. */
typedef struct bbcoop_camera {
    float rows[4][4];
    float fov, aspect, near_plane, far_plane;
} bbcoop_camera;
int bbcoop_map_camera(bbcoop_camera *camera);
/* Where a point (the game's metres) lies in the camera's picture: x from -1 (left) to 1 (right),
 * y from -1 (bottom) to 1 (top) into `at`; 0 behind the camera. */
int bbcoop_camera_project(const bbcoop_camera *camera, float x, float y, float z, float at[2]);
/* Kaniel 2026-10-18: the pad's touchpad held: the map opens (as from the Insert menu) while the
 * player is in the world. */
void bbcoop_map_from_pad(void);
/* A member's portrait by its board name: round, BBCOOP_PORTRAIT_SIDE square RGBA into `rgba`
 * (`size` bytes). Its id (changes with the picture, 0 none) into `id`; 1 when it has one. */
enum { BBCOOP_PORTRAIT_SIDE = 128 };
int bbcoop_member_portrait(const char *member, uint8_t *rgba, size_t size, uint32_t *id);
/* A revive's progress, 0 to 1: the share of the damage it needs that has landed (`landed`
 * and `needed` from the panel). */
float bbcoop_revive_share(uint32_t landed, uint32_t needed);
/* Where a file of the player's game folder (app0) is on this PC, `relative` like
 * "dvdroot_ps4/menu/common.tpf.dcx"; 0 before the game is installed or when `out` is too small. */
int bbcoop_game_file(const char *relative, char *out, size_t size);

/* Party portraits (the co-op code's bloodborne_portrait.h). The texture cache read back the status
 * page's model (its render target, `width` x `height` RGBA rows): this character's portrait once
 * the page closes. */
void bbcoop_portrait_model(const uint8_t *rgba, int width, int height);
/* The party list's portrait slot `slot` (0-2, the party HUD's CoopPartyFace): its version, which
 * changes whenever its picture does; with the picture (64x64 RGBA into `rgba`, `size` bytes) 1, 0
 * when it shows none. */
uint32_t bbcoop_portrait_version(int slot);
/* The off-screen status model is drawn (TickPortraits): read the 560x768 model target back as it
 * is drawn into, not only when the status page samples it. */
int bbcoop_portrait_offscreen_wanted(void);
int bbcoop_portrait_slot(int slot, uint8_t *rgba, size_t size, uint32_t *version);

/* The game opens a file (its guest path): the party code notes the character's save slot. */
void bbcoop_file_opened(const char *guest_path);

/* The game unlocked a trophy itself; party members get it too. */
void bbcoop_trophy_unlocked(int32_t trophy_id);

/* Phase 5, the team lobby (always on with co-op), for the game's own menu
 * (the team screen, bbport_overlay.cpp). Names are UTF-8; `id` is what a request names (a
 * player's lobby member, a team's id). */
enum { BBCOOP_LOBBY_TEAM = 4, BBCOOP_LOBBY_PLAYERS = 16, BBCOOP_LOBBY_INVITES = 8,
       BBCOOP_LOBBY_WORLDS = 16 };
enum { BBCOOP_LOBBY_OFFLINE, BBCOOP_LOBBY_TITLE, BBCOOP_LOBBY_PLAYING };
typedef struct bbcoop_lobby_entry {
    char name[40];
    char id[40];
    int state; /* BBCOOP_LOBBY_*: a player's or a member's */
    int ready; /* a member's */
} bbcoop_lobby_entry;
/* Kaniel 2026-10-17: one of the server's worlds (each kept apart on the server; any team plays
 * any of them). */
typedef struct bbcoop_lobby_world {
    char id[40];
    char name[64]; /* "Kaniel's world" */
    char when[32]; /* when it was last played, local time ("16 Oct 19:12"), "" when not known */
    int lamps;     /* its lamps lit (how far it is) */
    /* Kaniel 2026-10-17 (the list as the game's Load Game): when it was made ("14 Oct 2026", ""
     * when not known), the characters that played in it ("Kaniel, Ada"), and 1 while another team
     * plays it (it cannot be picked then). */
    char created[32];
    char characters[160];
    int busy;
} bbcoop_lobby_world;
typedef struct bbcoop_lobby {
    int signed_in; /* the server answered */
    int in_team, leader, started;
    int member_count, player_count, invite_count;
    bbcoop_lobby_entry members[BBCOOP_LOBBY_TEAM];       /* the leader first */
    bbcoop_lobby_entry players[BBCOOP_LOBBY_PLAYERS];    /* others signed in, not in this team */
    bbcoop_lobby_entry invites[BBCOOP_LOBBY_INVITES];    /* name: who invited, id: the team */
    char notice[96];                                      /* the server's last refusal */
    char me[40];                                          /* this player's id */
    /* Kaniel 2026-10-16: the screen opened from the game's System menu (not the title's load),
     * a save playing, and playing in the team's world. */
    int menu, playing, in_hunt;
    /* Kaniel 2026-10-17: the server's worlds, the last played first, and the one the team's next
     * Start plays (before a team: the one its team will take): an index into them, -1 a new
     * world. */
    int world_count, world;
    bbcoop_lobby_world worlds[BBCOOP_LOBBY_WORLDS];
} bbcoop_lobby;
/* Fills `lobby`; 0 when the team lobby is off. */
int bbcoop_lobby_view(bbcoop_lobby *lobby);
/* The menu's requests (TeamRequest actions): create, leave, invite a player (`id`), accept or
 * decline an invite (`id`: its team), remove a member (`id`). */
enum { BBCOOP_TEAM_CREATE = 1, BBCOOP_TEAM_INVITE = 2, BBCOOP_TEAM_ACCEPT = 3,
       BBCOOP_TEAM_DECLINE = 4, BBCOOP_TEAM_LEAVE = 5, BBCOOP_TEAM_REMOVE = 6,
       BBCOOP_TEAM_WORLD = 10 /* the leader picks the world (`id`; "" a new one) */,
       BBCOOP_TEAM_FORGET_WORLD = 11 /* the leader forgets a world (`id`) */ };
void bbcoop_lobby_request(int action, const char *id);
/* The title holds a save's load for the team: the team screen is up (bbport_overlay.cpp). */
int bbcoop_team_screen(void);
/* Kaniel 2026-10-16: the team menu in game. The System menu's "Co-op Team" page opens the team
 * screen over the game (1) and its buttons close it (0); a moment after it closed the pad presses
 * Circle for the game (1 while held) so the page under it closes too. The port's Insert menu opens
 * it with 2: no page of the game under it, no Circle. "Join the hunt": this save
 * into its team's world where it stands (`team`: an invitation accepted first, or "" in a team). */
void bbcoop_team_menu(int open);
int bbcoop_team_menu_back(void);
/* Kaniel 2026-10-17: the rig's team-worlds verb asks the team screen to open its world list: 1
 * once. */
int bbcoop_team_world_list_asked(void);
void bbcoop_team_join_hunt(const char *team);
/* Kaniel 2026-10-14: the team loading screen (League of Legends): each hunter's stage and bar,
 * up from the team's Start until every member is in the team's session. 0 when it is not up. */
enum { BBCOOP_LOAD_WAITING, BBCOOP_LOAD_LOADING, BBCOOP_LOAD_JOINING, BBCOOP_LOAD_READY };
typedef struct bbcoop_team_load_row {
    char name[40];
    int stage; /* BBCOOP_LOAD_* */
    int percent;
    int self;
} bbcoop_team_load_row;
typedef struct bbcoop_team_load {
    int count;
    bbcoop_team_load_row rows[4];
} bbcoop_team_load;
int bbcoop_team_load_view(bbcoop_team_load *view);
/* Kaniel 2026-10-15: while that screen is up the game's sound is held (runtime_audio.c reads this
 * every buffer): 1 held. */
int bbcoop_sound_held(void);
/* And a cutscene under it is skipped: 1 while Options is to be held down for the game's skip
 * (runtime_pad.c, even while the screen takes the pad). */
int bbcoop_cutscene_skip(void);
/* Kaniel 2026-10-17: under a cutscene every party member on the map sees, Options is a vote
 * (runtime_pad.c, `options`: the player holds it): 0 the player's own, 1 held back (others still
 * watch), 2 pressed for the game's skip (everyone watching voted). */
int bbcoop_cutscene_vote_pad(int options);
/* The vote as the overlay shows it: 1 while some of those watching want to skip. */
int bbcoop_cutscene_vote(int *want, int *watching);
/* Its buttons: the leader's start (a member is ready by itself, Kaniel 2026-10-20), or, in no
 * team, this save plays alone. */
enum { BBCOOP_TEAM_GO_START = 1, BBCOOP_TEAM_GO_ALONE = 2 };
void bbcoop_team_choose(int what);

/* Trading between party members (the co-op code's bloodborne_trade.h, with
 * always on in the port with the server world): what the port's trade menus (shim/bbport_trade_menu.cpp) need. An
 * item is a kind (1 Blood Echoes with id 0, 2 goods by EquipParamGoods id, 3 an item of its own:
 * a weapon, armor, gem or rune by its full item id, category | id); texts are UTF-8 and `size`
 * bytes at most. Calls from the game's thread. */
int bbcoop_trade_on(void);
/* Whether the item may be given (Kaniel 2026-10-15: any item; not the bell items). */
int bbcoop_trade_tradeable(uint32_t kind, uint32_t id);
uint32_t bbcoop_trade_held(uint32_t kind, uint32_t id);
/* The quantity picker's line for giving the item to the nearest member on this map; 0 when
 * nobody is there to give to. */
int bbcoop_trade_give_question(uint32_t kind, uint32_t id, char *text, size_t size);
/* The Blood Echoes picker's steps for what the player holds now: Up/Down (also its first and
 * lowest number) and the game's other step. */
void bbcoop_trade_echoes_picker(uint32_t *step, uint32_t *big_step);
/* Offers `count` of the item to the nearest member (taken out at once; an item of its own: the
 * inventory entry with `handle`, else the first): 1 when sent. */
int bbcoop_trade_offer(uint32_t kind, uint32_t id, uint32_t count, uint32_t handle);
/* Kaniel 2026-10-16: the giver picks who gets it. The members a trade may go to now (on this map,
 * standing, the nearest first): their count, and member `index`'s id (board name) and name; the
 * question and the offer for one of them (`member`: its id). */
int bbcoop_trade_partner_count(void);
int bbcoop_trade_partner(int index, char *member, size_t member_size, char *name,
                         size_t name_size);
int bbcoop_trade_give_question_to(const char *member, uint32_t kind, uint32_t id, char *text,
                                  size_t size);
int bbcoop_trade_offer_to(const char *member, uint32_t kind, uint32_t id, uint32_t count,
                          uint32_t handle);
/* The oldest offer to this player waiting for an answer: its id and question; 0 when none. */
int bbcoop_trade_ask(uint64_t *id, char *text, size_t size);
/* Whether offer `id` still waits for this player's answer. */
int bbcoop_trade_ask_open(uint64_t id);
/* Accepting takes what was asked for out first: 1 when it was a yes that went. */
int bbcoop_trade_answer(uint64_t id, int accept);
/* The next notice to show, skipping those with nothing to say; 0 when none. */
int bbcoop_trade_notice(char *text, size_t size);
/* Kaniel 2026-10-16 (the save editor's Give): items of the player's own (weapons, armor, gems,
 * runes: full item id, `count` of them) put in as a pickup does, with the game's popup (the
 * game's thread). 0 when trading's game functions are not known. */
int bbcoop_give_own_item(uint32_t item_id, uint32_t count);
/* The menus' tick: the co-op code runs it on the game's thread with its trades. */
void bbcoop_trade_set_ui_tick(void (*tick)(void));
/* Items' names for the boxes (the save editor's, from the game's text): by full item id (goods
 * 0x40000000 | id), 1 when it knows the item. */
void bbcoop_trade_set_namer(int (*namer)(uint32_t item_id, char *text, size_t size));

#ifdef __cplusplus
}
#endif
