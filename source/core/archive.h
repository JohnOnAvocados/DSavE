// "Export all": every game's backups in one .zip in _DSavE/export, to copy
// to a PC or to move a whole library to another card.

#ifndef DSAVE_ARCHIVE_H
#define DSAVE_ARCHIVE_H

#include <stdbool.h>

#include "backup.h"
#include "library.h"

typedef enum {
    ARCHIVE_NEWEST, // Each game's newest backup as "<ROM name>.sav", ready for emulators
    ARCHIVE_ALL,    // Every backup, laid out as _DSavE/saves is ("saves/<game>/<backup>/...")
} ArchiveMode;

typedef struct {
    int games;    // Games with at least one backup in the zip
    int backups;  // Backups written
    int skipped;  // Damaged backups left out
} ArchiveStats;

// Write <root>/_DSavE/export/DSavE_<timestamp>.zip. Compressed backups are
// stored as plain .sav files inside it. Each backup is checked first, as for
// a restore, and a damaged one is left out. The zip is written under a
// temporary name, read back and checked, and only then renamed into place,
// so a full SD card or a crash never leaves a half zip. A DSiWare
// backup's newest is "<ROM name>.pub", ".prv" and ".bnr"; with every
// backup, its public.sav, private.sav and banner.sav go in its folder. out_path receives
// its path. progress, when given, is called with (ctx, games done, total).
DsaveResult archive_export(const Library *lib, ArchiveMode mode, char *out_path, size_t out_size,
                           ArchiveStats *stats, void (*progress)(void *ctx, long done, long total), void *ctx);

#endif
