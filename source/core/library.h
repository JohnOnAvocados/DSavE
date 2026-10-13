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
    char title[64];   // Display title
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
} Game;

#define LIBRARY_MAX_GAMES 512

typedef struct {
    char root[DSAVE_PATH_MAX]; // Drive root, e.g. "sd:/" or "fat:/"
    KernelSet kernels;
    Game games[LIBRARY_MAX_GAMES];
    int count;
    bool truncated;            // More ROMs than LIBRARY_MAX_GAMES
    bool compress;             // Store new backups as .sav.gz
} Library;

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

// Change the active kernel and refresh every game.
void library_set_kernel(Library *lib, KernelId id);

// Turn compression of new backups on or off; remembered in
// _DSavE/settings.ini.
bool library_set_compress(Library *lib, bool on);

// Find the location entry used by a kernel, or -1.
int library_loc_for_kernel(const Game *game, KernelId id);

#endif
