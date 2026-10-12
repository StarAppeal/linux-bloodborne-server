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

/* Party death (on in the port unless SHADPS4_BLOODBORNE_PARTY_DEATH=0): sees what the game will
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
} bbcoop_panel_member;
typedef struct bbcoop_panel {
    int member_count;
    bbcoop_panel_member members[BBCOOP_PANEL_MEMBERS];
    int revive_target;
    int vital; /* this player's */
    uint32_t landed, needed;
    float give_up; /* the give-up hold, 0 to 1 */
    int native_list; /* the game's HUD shows the party list (the party HUD's CoopParty tags) */
} bbcoop_panel;
/* Fills `panel`; 0 when there is nothing to draw (party death off, no party session, nobody else
 * on the vitals board and this player not down). */
int bbcoop_party_panel(bbcoop_panel *panel);
/* A revive's progress, 0 to 1: the share of the damage it needs that has landed (`landed`
 * and `needed` from the panel). */
float bbcoop_revive_share(uint32_t landed, uint32_t needed);
/* Where a file of the player's game folder (app0) is on this PC, `relative` like
 * "dvdroot_ps4/menu/common.tpf.dcx"; 0 before the game is installed or when `out` is too small. */
int bbcoop_game_file(const char *relative, char *out, size_t size);

/* The game opens a file (its guest path): the party code notes the character's save slot. */
void bbcoop_file_opened(const char *guest_path);

/* The game unlocked a trophy itself; party members get it too. */
void bbcoop_trophy_unlocked(int32_t trophy_id);

#ifdef __cplusplus
}
#endif
