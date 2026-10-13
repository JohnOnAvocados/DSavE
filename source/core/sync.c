#include "sync.h"

#include <stdio.h>
#include <string.h>

#include "ini.h"
#include "kernel.h"

#define IO_CHUNK 4096

// CRC-32 and length of a file's data, ignoring trailing 0xFF padding.
// Streams the file once: runs of 0xFF are held back and only folded into the
// CRC when non-0xFF data follows them.
static bool data_crc(const char *path, uint32_t *crc_out, long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    static const uint8_t ff[IO_CHUNK] = { [0 ... IO_CHUNK - 1] = 0xFF };
    uint8_t buf[IO_CHUNK];
    uint32_t crc = 0;
    long len = 0, pending_ff = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        size_t start = 0;
        for (size_t i = 0; i < n; i++)
        {
            if (buf[i] == 0xFF)
                continue;
            // Flush held-back padding, then this run of data up to i.
            while (pending_ff > 0)
            {
                size_t chunk = (size_t)(pending_ff < IO_CHUNK ? pending_ff : IO_CHUNK);
                crc = crc32_update(crc, ff, chunk);
                len += (long)chunk;
                pending_ff -= (long)chunk;
            }
            // Bytes between start and i are 0xFF that are now known to be data.
            crc = crc32_update(crc, buf + start, i + 1 - start);
            len += (long)(i + 1 - start);
            start = i + 1;
        }
        pending_ff += (long)(n - start);
    }
    bool ok = !ferror(f);
    fclose(f);
    if (ok)
    {
        *crc_out = crc;
        *len_out = len;
    }
    return ok;
}

void sync_check(const Game *game, SyncInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->newest = -1;

    for (int i = 0; i < game->loc_count; i++)
    {
        const SaveLoc *loc = &game->locs[i];
        LocStatus *st = &out->loc[i];
        st->exists = loc->exists && data_crc(loc->path, &st->crc32, &st->data_size);
        if (!st->exists)
            continue;
        out->existing++;
        if (out->newest < 0 || loc->mtime > game->locs[out->newest].mtime)
            out->newest = i;
    }
    if (out->newest >= 0)
        out->loc[out->newest].newest = true;

    const LocStatus *primary = &out->loc[game->primary];
    out->state = out->existing < 2 ? SYNC_SINGLE : SYNC_MATCH;
    for (int i = 0; i < game->loc_count; i++)
    {
        LocStatus *st = &out->loc[i];
        if (!st->exists)
            continue;
        st->same_as_primary = primary->exists && st->crc32 == primary->crc32 &&
                              st->data_size == primary->data_size;
        for (int j = 0; j < i; j++)
        {
            const LocStatus *other = &out->loc[j];
            if (other->exists && (other->crc32 != st->crc32 || other->data_size != st->data_size))
                out->state = SYNC_DIFFERENT;
        }
    }
}

DsaveResult sync_copy(const Library *lib, Game *game, int from, int to, Backup *auto_backup)
{
    if (auto_backup)
        memset(auto_backup, 0, sizeof(*auto_backup));
    library_refresh_game(lib, game);
    if (from < 0 || from >= game->loc_count || to < 0 || to >= game->loc_count)
        return DSAVE_ERR_PATH;
    if (from == to)
        return DSAVE_OK;
    if (!game->locs[from].exists)
        return DSAVE_ERR_NO_SAVE;

    // Treat the source file as a backup with no backup.ini: restore checks
    // its size, keeps the target's padding and makes the safety copy.
    Backup src;
    memset(&src, 0, sizeof(src));
    snprintf(src.file, sizeof(src.file), "%s", game->locs[from].path);
    path_dirname(src.dir, sizeof(src.dir), src.file);
    src.size = game->locs[from].size;
    return backup_restore_to(lib, game, &src, to, auto_backup);
}

DsaveResult sync_all(const Library *lib, Game *game, int *copied)
{
    if (copied)
        *copied = 0;
    library_refresh_game(lib, game);
    if (!game->has_save)
        return DSAVE_ERR_NO_SAVE;

    int from = game->primary;
    for (int to = 0; to < game->loc_count; to++)
    {
        if (to == from)
            continue;
        DsaveResult r = sync_copy(lib, game, from, to, NULL);
        if (r != DSAVE_OK)
            return r;
        if (copied)
            (*copied)++;
        // sync_copy refreshes the game; the primary index is stable because
        // the source still exists and the active kernel did not change.
        from = game->primary;
    }
    return DSAVE_OK;
}

int slots_list(const Library *lib, const Game *game, KernelId kernel, SaveSlot *out, int max)
{
    const KernelSet *ks = &lib->kernels;
    if (kernel >= KERNEL_COUNT || !ks->cfg[kernel].installed)
        return 0;

    int current = kernel_current_slot(ks, kernel, game->rom_path);
    int n = 0;
    for (int slot = 0; slot <= kernel_max_slot(kernel) && n < max; slot++)
    {
        SaveSlot *s = &out[n];
        memset(s, 0, sizeof(*s));
        if (!kernel_save_path(ks, kernel, game->rom_path, slot, s->path, sizeof(s->path)))
            continue;
        s->number = slot;
        s->exists = file_exists(s->path);
        s->size = s->exists ? file_size(s->path) : -1;
        s->current = slot == current;
        n++;
    }
    return n;
}

DsaveResult slots_set_current(Library *lib, Game *game, KernelId kernel, int n)
{
    const KernelSet *ks = &lib->kernels;
    if (kernel != KERNEL_TWILIGHT || !ks->cfg[kernel].installed ||
        ks->cfg[kernel].twl_location == TWL_GAMES_FOLDER)
        return DSAVE_ERR_UNSUPPORTED;
    if (n < 0 || n > kernel_max_slot(kernel))
        return DSAVE_ERR_PATH;

    char path[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    if (!kernel_twl_gamesettings_path(ks, game->rom_path, path, sizeof(path)))
        return DSAVE_ERR_PATH;
    path_dirname(dir, sizeof(dir), path);
    if (!mkdir_p(dir) || !ini_set_int(path, "GAMESETTINGS", "SAVE_NUMBER", n))
        return DSAVE_ERR_IO;
    library_refresh_game(lib, game);
    return DSAVE_OK;
}
