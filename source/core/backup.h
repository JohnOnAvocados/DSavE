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

#include "emusave.h"
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
    DSAVE_ERR_CARD_CHANGED,   // The game card was removed or swapped
    DSAVE_ERR_CARD,           // The game card's save chip did not respond
    DSAVE_ERR_DAMAGED,        // An import file's header or packed data is damaged
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

// Same, recording source_label in backup.ini as where the save came from
// (when src is a temporary copy, e.g. of a game card's save chip).
DsaveResult backup_create_labeled(const Library *lib, const Game *game, const char *src,
                                  const char *source_label, const char *name, bool allow_blank,
                                  Backup *out);

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

typedef enum {
    EXPORT_SAV, // Raw: melonDS, flashcarts, Checkpoint and JKSM
    EXPORT_DSV, // Raw plus DeSmuME's footer: DeSmuME and DraStic
} ExportFormat;

// Copy a backup to <root>/_DSavE/export/<ROM file name>.sav or .dsv, the
// name emulators look for next to the ROM. An earlier export of the same
// game is replaced. When the chip size is known (Pico's save list, or a
// game card) and the rest of the file is only a kernel's 0xFF padding, the
// export is trimmed to it so emulators detect the right save type.
DsaveResult backup_export(const Library *lib, const Game *game, const Backup *b, ExportFormat format,
                          char *out_path, size_t out_size);

// Name for a safety copy made before a restore: "auto_<timestamp>".
void backup_auto_name(char *out, size_t size);

// Auto backups beyond the newest `keep` for this game move to the trash.
// protect (a backup name, or NULL) is always kept and counts towards keep.
// Restores and copies call this after making their safety copy, with
// lib->keep_auto. Returns how many were moved.
int backup_prune_auto(const Library *lib, const Game *game, int keep, const char *protect);

// Saves from a PC, an emulator or another save manager (Checkpoint, JKSM)
// are copied into <root>/_DSavE/import/ and imported as a backup. Raw .sav,
// DeSmuME and DraStic .dsv, no$gba, Action Replay .duc/.dss and .sav.gz
// files are read (see emusave.h).
#define IMPORT_DIR "_DSavE/import"
#define IMPORT_LIST_MAX 32

typedef struct {
    char name[64];
    char path[DSAVE_PATH_MAX];
    long size;        // Save data size, -1 if unreadable
    bool valid;       // size is a DS save size
    EmuFormat format;
    int game;         // Index of the game whose ROM has the same file name, or -1
} ImportFile;

// List the files in the import folder (creating it), sorted by name.
int backup_import_list(const Library *lib, ImportFile *out, int max);

// Make a backup named "import_<file name>" from an import file. The file
// is left where it is. A save's data doesn't say which game it is from, so
// the caller picks the game: the one named after it (ImportFile.game), or
// the user's choice.
DsaveResult backup_import(const Library *lib, const Game *game, const char *path, Backup *out);

#endif
