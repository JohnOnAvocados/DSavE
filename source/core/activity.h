// A log of what DSavE did to the saves on the card, one line per action, in
// <root>/_DSavE/log.txt. It is plain text (tab separated) so it can be read
// on a PC, and it gives a restore a one-step undo: the line records the auto
// backup the restore made, which activity_undo() puts back.

#ifndef DSAVE_ACTIVITY_H
#define DSAVE_ACTIVITY_H

#include <stdbool.h>
#include <stdint.h>

#include "backup.h"
#include "library.h"

#define ACTIVITY_FILE "_DSavE/log.txt"
#define ACTIVITY_OLD_FILE "_DSavE/log.old.txt"
// When log.txt grows past this it becomes log.old.txt and a new one starts.
#define ACTIVITY_ROTATE_BYTES (64 * 1024)
#define ACTIVITY_LIST_MAX 64

typedef enum {
    ACT_BACKUP,
    ACT_RESTORE,  // A backup written over a save
    ACT_COPY,     // One kernel's copy of a save written over another's
    ACT_IMPORT,
    ACT_TRASH,
    ACT_EXPORT,
    ACT_UNDO,     // A restore or copy undone
    ACT_UNKNOWN,
} ActivityKind;

typedef struct {
    char time[24];                       // "YYYY-MM-DD_HH-MM-SS"
    ActivityKind kind;
    char game_code[5];
    char title[64];
    char backup[BACKUP_NAME_MAX + 1];    // The backup made, restored or trashed
    uint32_t crc32;                      // Its CRC-32, 0 when not known
    char safety[BACKUP_NAME_MAX + 1];    // Auto backup made first ("" for none)
    char path[DSAVE_PATH_MAX];           // Save written, or file exported
    bool card;                           // The game in slot 1
} Activity;

const char *activity_kind_name(ActivityKind kind); // "Backup", "Restore", ...

// Append one line. Tabs and line breaks in the fields become spaces. A
// failed write is ignored by callers: the log never blocks the work.
bool activity_log(const Library *lib, ActivityKind kind, const Game *game, const char *backup,
                  uint32_t crc32, const char *safety, const char *path);

// The newest entries first, from log.txt and then log.old.txt. Lines that
// can't be read are skipped. Returns the count.
int activity_list(const Library *lib, Activity *out, int max);

// Whether activity_undo can work on this entry: a restore, copy or undo
// onto an SD card save that made an auto backup.
bool activity_can_undo(const Activity *a);

// Put back the save from before a restore or copy: the auto backup it made
// is restored onto the same file (which makes a new auto backup first, so
// the undo can be undone too). game_out receives the index of the game.
DsaveResult activity_undo(Library *lib, const Activity *a, int *game_out, Backup *auto_backup);

#endif
