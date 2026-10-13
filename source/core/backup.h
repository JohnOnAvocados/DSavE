// Versioned backups of a game's save, stored on the SD card as:
//
//   <root>/_DSavE/saves/<GAMECODE> <title>/<backup name>/<title>.sav
//                                                       /backup.ini
//
// The .sav is a raw image of the save file, so PC tools and emulators can
// open it directly. With compression on it is <title>.sav.gz instead, a
// standard gzip file. backup.ini records its size and CRC-32, which restore
// checks before it touches the live save.

#ifndef DSAVE_BACKUP_H
#define DSAVE_BACKUP_H

#include <stdbool.h>
#include <stdint.h>

#include "library.h"

#define BACKUP_NAME_MAX 40
#define BACKUP_LIST_MAX 64

typedef enum {
    DSAVE_OK = 0,
    DSAVE_WARN_BLANK,         // Save is all 0x00/0xFF; pass allow_blank to keep it
    DSAVE_ERR_NO_SAVE,        // Game has no live save to back up
    DSAVE_ERR_IO,             // A read, write, mkdir or rename failed
    DSAVE_ERR_PATH,           // A path did not fit in DSAVE_PATH_MAX
    DSAVE_ERR_VERIFY,         // Copy did not match its source after writing
    DSAVE_ERR_CORRUPT,        // Backup's size or CRC does not match backup.ini
    DSAVE_ERR_BAD_SIZE,       // Backup is not a valid DS save size
    DSAVE_ERR_WRONG_GAME,     // Backup was made for a different game code
    DSAVE_ERR_EXISTS,         // Target name is already taken
    DSAVE_ERR_ROLLED_BACK,    // Restore failed verification; old save put back
    DSAVE_ERR_UNSUPPORTED,    // The kernel has no such feature
} DsaveResult;

const char *dsave_result_str(DsaveResult r);

typedef struct {
    char name[BACKUP_NAME_MAX + 1];
    char dir[DSAVE_PATH_MAX];     // Folder holding this backup
    char file[DSAVE_PATH_MAX];    // The raw .sav inside it
    char game_code[5];
    char created[24];             // "YYYY-MM-DD_HH-MM-SS"
    long size;
    uint32_t crc32;
    bool has_meta;                // backup.ini was present and readable
    bool is_auto;                 // Made automatically before a restore
    bool compressed;              // Stored as <title>.sav.gz
} Backup;

// Paths under <root>/_DSavE.
bool backup_game_dir(const Library *lib, const Game *game, char *out, size_t size);

// Copy the game's live save into a new backup. name may be NULL or empty for
// a timestamp. The copy is written to a temporary folder, verified by CRC,
// and only then renamed into place, so a failure never leaves a half backup.
DsaveResult backup_create(const Library *lib, const Game *game, const char *name,
                          bool allow_blank, Backup *out);

// Same, from any save file of this game (e.g. another kernel's copy).
DsaveResult backup_create_from(const Library *lib, const Game *game, const char *src,
                               const char *name, bool allow_blank, Backup *out);

// List a game's backups, newest first. Returns the count.
int backup_list(const Library *lib, const Game *game, Backup *out, int max);

// Check a backup against its backup.ini before using it.
DsaveResult backup_validate(const Backup *b);

// Restore a backup over the game's primary save:
//   1. validate the backup (size, CRC-32, game code),
//   2. back up the current save automatically ("auto_<timestamp>"),
//   3. overwrite the save in place, so a flashcart's pre-allocated,
//      contiguous file keeps its clusters, padding with 0xFF to the size the
//      kernel made (see backup_restore_length),
//   4. read it back and compare CRCs; on mismatch, put the old save back.
// If the game has no save yet, a new file is created where the active kernel
// expects it. auto_backup receives the safety copy when one was made.
DsaveResult backup_restore(const Library *lib, Game *game, const Backup *b,
                           Backup *auto_backup);

// Same, onto game->locs[loc] (another kernel's save path). loc < 0 means the
// primary save.
DsaveResult backup_restore_to(const Library *lib, Game *game, const Backup *b, int loc,
                              Backup *auto_backup);

// Same, onto any save file path of this game (e.g. a TWiLight slot file).
DsaveResult backup_restore_path(const Library *lib, Game *game, const Backup *b,
                                const char *path, Backup *auto_backup);

// How long the restored file will be. Kernels pad saves (TWiLight and Pico
// both default to 512 KB filled with 0xFF), so:
//   - no save yet: the backup's size;
//   - backup smaller than or equal to the save: the save's size (0xFF pad);
//   - backup larger: the save's size if the extra is all 0xFF, else the
//     backup's size. Data is never cut off.
long backup_restore_length(const char *backup_file, long backup_size, long live_size);

// Compress a backup to .sav.gz, or decompress it back to a raw .sav. The new
// file is written and verified before the old one is removed. Saves larger
// than SAVEFILE_GZ_MAX are left raw (DSAVE_ERR_UNSUPPORTED).
DsaveResult backup_set_compressed(const Backup *b, bool compress, Backup *out);

// Rename a backup's folder.
DsaveResult backup_rename(const Library *lib, const Game *game, const Backup *b,
                          const char *new_name);

// Move a backup to <root>/_DSavE/_TRASH_/ instead of deleting it.
DsaveResult backup_trash(const Library *lib, const Game *game, const Backup *b);

// Copy a backup's raw .sav to <root>/_DSavE/export/<title>_<CODE>_<name>.sav.
// When Pico's save list gives the real chip size and the rest of the file is
// 0xFF padding, the export is trimmed to the chip size so emulators detect
// the right save type.
DsaveResult backup_export(const Library *lib, const Game *game, const Backup *b,
                          char *out_path, size_t out_size);

#endif
