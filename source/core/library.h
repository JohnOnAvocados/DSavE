// Finds the games on the SD card and pairs each ROM with its live save, using
// the rules of every kernel installed on the card (see kernel.h).

#ifndef DSAVE_LIBRARY_H
#define DSAVE_LIBRARY_H

#include <stdbool.h>
#include <time.h>

#include "kernel.h"
#include "rom.h"
#include "util.h"

// One place a game's save lives (or would live) for one or more kernels.
// Kernels that agree on a path share one entry.
//
// A DSiWare title's save is up to three files (rom.h, DsiPart). path is then
// its main file: the public save, or the first part the title has. The
// others sit beside it (library_dsi_part_path). exists means any of them
// exists, size is their total and mtime the newest.
typedef struct {
    char path[DSAVE_PATH_MAX];
    KernelMask kernels;
    bool exists;
    long size;    // -1 when missing
    time_t mtime; // 0 when missing
} SaveLoc;

#define GAME_MAX_LOCS KERNEL_COUNT

typedef struct {
    char rom_path[DSAVE_PATH_MAX];
    RomInfo info;
    char title[128];  // Display title, UTF-8
    long chip_size;   // Real chip size from Pico's save list, -1 unknown

    SaveLoc locs[GAME_MAX_LOCS];
    int loc_count;
    int primary;      // Index into locs of the save DSavE works on

    // Copies of locs[primary], kept for convenience.
    char save_path[DSAVE_PATH_MAX];
    bool has_save;
    long save_size;

    // True when the active kernel's own save is missing and the primary is
    // another kernel's file.
    bool primary_is_fallback;

    // The game card in slot 1 rather than a ROM on the SD card (see
    // cardsave.h). It has no save locations; has_save means its save chip
    // can be read, and save_size is the chip's size.
    bool is_card;
} Game;

#define LIBRARY_MAX_GAMES 512

typedef struct {
    char root[DSAVE_PATH_MAX]; // Drive root, e.g. "sd:/" or "fat:/"
    KernelSet kernels;
    Game games[LIBRARY_MAX_GAMES];
    int count;
    bool truncated;            // More ROMs than LIBRARY_MAX_GAMES
    bool compress;             // Store new backups as .sav.gz
    int keep_auto;             // Auto backups kept per game, 0 = all
    // DSavE runs in DSi mode. Where TWiLight has no DSiWare save yet, this
    // picks the layout it will make (kernel.h, DsiLayout). Set by main.c
    // before the first scan; library_scan leaves it alone.
    bool dsi_mode;
} Library;

#define LIBRARY_KEEP_AUTO_DEFAULT 10

// Detect kernels, then scan root for .nds/.dsi files (skipping system
// folders and homebrew), sorted by title. Returns the number of games.
int library_scan(Library *lib, const char *root);

void library_free(Library *lib);

// Re-check one game's save locations, e.g. after a restore or a kernel
// change. Picks the primary save:
//   1. the active kernel's save in its current slot, if it exists;
//   2. otherwise the most recently modified existing save of any kernel;
//   3. otherwise the active kernel's path, where a restore will create it.
void library_refresh_game(const Library *lib, Game *game);

// Number of ROMs found, not counting a game card entry.
int library_rom_count(const Library *lib);

// Change the active kernel and refresh every game.
void library_set_kernel(Library *lib, KernelId id);

// Turn compression of new backups on or off; remembered in
// _DSavE/settings.ini.
bool library_set_compress(Library *lib, bool on);

// How many auto backups (made before a restore or copy) to keep per game;
// older ones move to the trash. 0 keeps them all. Remembered in
// _DSavE/settings.ini.
bool library_set_keep_auto(Library *lib, int keep);

// Find the location entry used by a kernel, or -1.
int library_loc_for_kernel(const Game *game, KernelId id);

// The path kernel id uses for this game's save in a slot: kernel_save_path,
// or for DSiWare the main file in the layout the kernel uses (for TWiLight,
// the layout of the files already there, else the one lib->dsi_mode
// implies). Returns false if the kernel has no save for this game there.
bool library_kernel_path(const Library *lib, const Game *game, KernelId id, int slot, char *out,
                         size_t size);

// DSiWare: the path of one of the title's save files at the location whose
// main file is main_path. Returns false if the title doesn't use that part,
// or the layout has no file for it (the SAV layout keeps no private save
// when there is a public one).
bool library_dsi_part_path(const Game *game, const char *main_path, DsiPart part, char *out, size_t size);

// The part a DSiWare title's main file holds, or DSI_PARTS if it has none.
DsiPart library_dsi_main_part(const Game *game);

// Whether a save exists at path (for DSiWare, any of its files beside the
// main file path), with its size (their total) and newest mtime. size and
// mtime may be NULL; they get -1 and 0 when it is missing.
bool library_stat_save(const Game *game, const char *path, long *size, time_t *mtime);

#endif
