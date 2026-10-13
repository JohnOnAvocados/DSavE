// Cross-kernel tools: compare the copies of one game's save that different
// kernels keep on the same card, copy a save into another kernel's layout,
// and manage save slots (TWiLight's .sav1-.sav9, akmenu-next's .1.sav-.9.sav).

#ifndef DSAVE_SYNC_H
#define DSAVE_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "backup.h"
#include "library.h"

typedef enum {
    SYNC_SINGLE,    // Zero or one copy exists: nothing to compare
    SYNC_MATCH,     // Every existing copy holds the same save data
    SYNC_DIFFERENT, // At least two copies differ
} SyncState;

typedef struct {
    bool exists;
    uint32_t crc32;   // CRC of the data with trailing 0xFF padding removed
    long data_size;   // Length of that data
    bool newest;      // Most recently modified existing copy
    bool same_as_primary;
} LocStatus;

typedef struct {
    SyncState state;
    int existing;     // Number of copies that exist
    int newest;       // Index into game->locs, or -1
    LocStatus loc[GAME_MAX_LOCS];
} SyncInfo;

// Compare every copy of the game's save. Copies that differ only in 0xFF
// padding (a 64 KB exact save and the same data padded to 512 KB) count as
// the same. Reads each file once.
void sync_check(const Game *game, SyncInfo *out);

// Copy the save at game->locs[from] to game->locs[to]. The target, if it
// exists, is first backed up as "auto_<timestamp>" and then overwritten in
// place with padding kept (same rules as a restore).
DsaveResult sync_copy(const Library *lib, Game *game, int from, int to, Backup *auto_backup);

// Copy the primary save to every other location, creating missing ones, so
// the game finds the same save whichever installed kernel launches it.
// Returns the first error, or DSAVE_OK. *copied counts locations written.
DsaveResult sync_all(const Library *lib, Game *game, int *copied);

#define SLOT_MAX 10

typedef struct {
    int number;     // 0 = the normal save
    char path[DSAVE_PATH_MAX];
    bool exists;
    long size;
    bool current;   // The slot the kernel loads now
} SaveSlot;

// List a kernel's slots for this game (0 up to kernel_max_slot). Returns the
// count, 0 if the kernel is not installed.
int slots_list(const Library *lib, const Game *game, KernelId kernel, SaveSlot *out, int max);

// Make TWiLight load slot n for this game by writing SAVE_NUMBER to its
// per-game settings file. Other kernels do not expose a writable slot
// setting, so this fails for them.
DsaveResult slots_set_current(Library *lib, Game *game, KernelId kernel, int n);

#endif
