// Check every backup on the SD card at once: each backup's size and CRC-32
// against its backup.ini (as a restore would), plus the folders under
// _DSavE/saves that hold no usable backup or belong to no game on the card.
// It only reads; nothing is changed or moved.

#ifndef DSAVE_CHECK_H
#define DSAVE_CHECK_H

#include <stdbool.h>

#include "backup.h"
#include "library.h"

typedef enum {
    CHECK_DAMAGED,    // Size or CRC doesn't match backup.ini, or unreadable
    CHECK_NO_META,    // No readable backup.ini, so it can't be verified
    CHECK_NO_SAVE,    // A backup folder with no .sav or .sav.gz in it
    CHECK_LEFTOVER,   // A ".tmp_" folder left by an interrupted backup
    CHECK_ORPHAN,     // A game folder whose game isn't on the card
} CheckKind;

#define CHECK_ISSUE_MAX 48

typedef struct {
    CheckKind kind;
    int game;                       // Index into lib->games, -1 for an orphan
    char name[BACKUP_NAME_MAX + 1]; // The backup, or the orphan folder's name
    DsaveResult result;             // For CHECK_DAMAGED: what validation said
} CheckIssue;

typedef struct {
    int games;          // Games with a backup folder
    int backups;        // Backups checked
    int ok;             // ...that verified
    int issue_count;    // Issues found (only the first CHECK_ISSUE_MAX are kept)
    CheckIssue issues[CHECK_ISSUE_MAX];
    bool orphans_skipped; // The game list is full, so orphans weren't looked for
} CheckReport;

// progress, when given, is called with (ctx, games done, games total).
void check_all(const Library *lib, CheckReport *report,
               void (*progress)(void *ctx, long done, long total), void *ctx);

// "Damaged", "Not verifiable", ...
const char *check_kind_name(CheckKind kind);

#endif
