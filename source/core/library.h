// Finds the games on the SD card, works out which loader (flashcart kernel or
// TWiLight Menu++) is in charge, and pairs each ROM with its live .sav file.

#ifndef DSAVE_LIBRARY_H
#define DSAVE_LIBRARY_H

#include <stdbool.h>

#include "rom.h"
#include "util.h"

typedef enum {
    LOADER_GENERIC,  // Unknown kernel: save next to the ROM
    LOADER_TWILIGHT, // TWiLight Menu++ / nds-bootstrap: <rom dir>/saves/
    LOADER_WOOD,     // Wood R4 / R4iLS family (__rpg)
    LOADER_YSMENU,   // YSMenu (TTMenu)
    LOADER_AKAIO,    // AKAIO (__aio)
} LoaderKind;

typedef struct {
    char rom_path[DSAVE_PATH_MAX];
    char save_path[DSAVE_PATH_MAX]; // Existing save, or where it would go
    bool has_save;
    long save_size;                 // -1 when there is no save yet
    RomInfo info;
    char title[64];                 // Display title
} Game;

#define LIBRARY_MAX_GAMES 512

typedef struct {
    char root[DSAVE_PATH_MAX];      // Drive root, e.g. "sd:/" or "fat:/"
    LoaderKind loader;
    Game games[LIBRARY_MAX_GAMES];
    int count;
    bool truncated;                 // More ROMs than LIBRARY_MAX_GAMES
} Library;

// Look for each loader's system folder under root. TWiLight wins when both it
// and a kernel are present, since it then launches the games and owns saves.
LoaderKind library_detect_loader(const char *root);

const char *library_loader_name(LoaderKind kind);

// Work out where the live save for rom_path is. Checks every known layout and
// returns the first that exists; if none exists, returns the path the
// detected loader would create. Returns true if the save exists.
bool library_resolve_save(LoaderKind loader, const char *rom_path,
                          char *out, size_t out_size);

// Scan root for .nds files (skipping system folders and homebrew), sorted by
// title. Returns the number of games found.
int library_scan(Library *lib, const char *root);

// Re-check one game's save after a restore or an external change.
void library_refresh_game(const Library *lib, Game *game);

#endif
