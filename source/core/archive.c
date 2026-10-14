#include "archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../third_party/miniz/miniz_config.h"
#include "../third_party/miniz/miniz.h"
#include "savefile.h"
#include "util.h"

#define EXPORT_DIR "_DSavE/export"
#define ZIP_LEVEL 1 // Fast: the DS's CPU is slow, and padding compresses well at any level

// miniz writes the zip through this, at the offsets it chooses.
static size_t zip_write(void *opaque, mz_uint64 ofs, const void *buf, size_t n)
{
    FILE *f = opaque;
    if (fseek(f, (long)ofs, SEEK_SET) != 0)
        return 0;
    return fwrite(buf, 1, n, f);
}

static size_t zip_read(void *opaque, mz_uint64 ofs, void *buf, size_t n)
{
    FILE *f = opaque;
    if (fseek(f, (long)ofs, SEEK_SET) != 0)
        return 0;
    return fread(buf, 1, n, f);
}

// A backup's save data, fed to miniz in order.
typedef struct {
    SaveReader r;
    long pos;
} Feed;

static size_t feed_read(void *opaque, mz_uint64 ofs, void *buf, size_t n)
{
    Feed *fd = opaque;
    if ((long)ofs != fd->pos)
        return 0;
    size_t got = savefile_read(&fd->r, buf, n);
    fd->pos += (long)got;
    return got;
}

static bool add_save(mz_zip_archive *zip, const char *name, const Backup *b)
{
    long size = savefile_size(b->file);
    Feed fd = { .pos = 0 };
    if (size < 0 || !savefile_open(&fd.r, b->file))
        return false;
    MZ_TIME_T now = time(NULL);
    bool ok = mz_zip_writer_add_read_buf_callback(zip, name, feed_read, &fd, (mz_uint64)size, &now, NULL, 0,
                                                  ZIP_LEVEL, NULL, 0, NULL, 0);
    savefile_close(&fd.r);
    return ok && fd.pos == size;
}

// A small file (backup.ini) as it is on the card.
static bool add_small_file(mz_zip_archive *zip, const char *name, const char *path)
{
    long size = file_size(path);
    if (size < 0 || size > 16 * 1024)
        return false;
    char *buf = malloc((size_t)size + 1);
    FILE *f = buf ? fopen(path, "rb") : NULL;
    bool ok = f && fread(buf, 1, (size_t)size, f) == (size_t)size;
    if (f)
        fclose(f);
    ok = ok && mz_zip_writer_add_mem(zip, name, buf, (size_t)size, ZIP_LEVEL);
    free(buf);
    return ok;
}

// A zip entry name not used yet: "name.sav", then "name_2.sav"...
static bool unique_entry(mz_zip_archive *zip, const char *stem, const char *ext, char *out, size_t size)
{
    for (int i = 1; i < 1000; i++)
    {
        if (i == 1)
            snprintf(out, size, "%s%s", stem, ext);
        else
            snprintf(out, size, "%s_%d%s", stem, i, ext);
        if (mz_zip_reader_locate_file(zip, out, NULL, 0) < 0)
            return true;
    }
    return false;
}

// Add one game's backups. Returns false only when writing fails.
static bool add_game(mz_zip_archive *zip, const Library *lib, const Game *g, const char *folder, ArchiveMode mode,
                     ArchiveStats *st)
{
    static Backup list[BACKUP_LIST_MAX];
    int n = backup_list(lib, g, list, BACKUP_LIST_MAX);
    int added = 0;
    for (int i = 0; i < n; i++) // Newest first
    {
        const Backup *b = &list[i];
        if (backup_validate(b) != DSAVE_OK)
        {
            st->skipped++;
            continue;
        }
        char name[DSAVE_PATH_MAX];
        if (mode == ARCHIVE_NEWEST)
        {
            char stem[DSAVE_PATH_MAX];
            backup_export_stem(g, stem, sizeof(stem));
            if (!unique_entry(zip, stem, ".sav", name, sizeof(name)) || !add_save(zip, name, b))
                return false;
        }
        else
        {
            char file[DSAVE_PATH_MAX], ini[DSAVE_PATH_MAX];
            path_basename(file, sizeof(file), b->file);
            if (b->compressed && has_ext(file, SAVEFILE_GZ_EXT))
                file[strlen(file) - strlen(SAVEFILE_GZ_EXT)] = '\0';
            snprintf(name, sizeof(name), "saves/%s/%s/%s", folder, b->name, file);
            if (!add_save(zip, name, b))
                return false;
            if (b->has_meta && path_join(ini, sizeof(ini), b->dir, "backup.ini"))
            {
                snprintf(name, sizeof(name), "saves/%s/%s/backup.ini", folder, b->name);
                if (!add_small_file(zip, name, ini))
                    return false;
            }
        }
        st->backups++;
        added++;
        if (mode == ARCHIVE_NEWEST)
            break;
    }
    if (added)
        st->games++;
    return true;
}

// Read the zip back: every entry's data must match its CRC.
static bool verify(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    zip.m_pRead = zip_read;
    zip.m_pIO_opaque = f;
    long size = file_size(path);
    bool ok = size > 0 && mz_zip_reader_init(&zip, (mz_uint64)size, 0) && mz_zip_validate_archive(&zip, 0);
    mz_zip_reader_end(&zip);
    fclose(f);
    return ok;
}

DsaveResult archive_export(const Library *lib, ArchiveMode mode, char *out_path, size_t out_size,
                           ArchiveStats *stats, void (*progress)(void *ctx, long done, long total), void *ctx)
{
    ArchiveStats st = { 0 };
    char dir[DSAVE_PATH_MAX], tmp[DSAVE_PATH_MAX], target[DSAVE_PATH_MAX], stamp[24], leaf[48];
    if (!path_join(dir, sizeof(dir), lib->root, EXPORT_DIR) || !path_join(tmp, sizeof(tmp), dir, ".zip.tmp"))
        return DSAVE_ERR_PATH;
    if (!mkdir_p(dir))
        return DSAVE_ERR_IO;
    timestamp_now(stamp, sizeof(stamp));
    snprintf(leaf, sizeof(leaf), "DSavE_%s.zip", stamp);
    if (!path_join(target, sizeof(target), dir, leaf))
        return DSAVE_ERR_PATH;

    remove(tmp);
    FILE *f = fopen(tmp, "w+b");
    if (!f)
        return DSAVE_ERR_IO;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    zip.m_pWrite = zip_write;
    zip.m_pIO_opaque = f;
    bool ok = mz_zip_writer_init(&zip, 0);

    // Games that share a backup folder (one game in two ROM folders, or the
    // game card and its ROM) go in once.
    typedef char Folder[64];
    Folder *seen = malloc(sizeof(Folder) * (size_t)(lib->count ? lib->count : 1));
    int seen_n = 0;
    ok = ok && seen;
    for (int gi = 0; ok && gi < lib->count; gi++)
    {
        if (progress)
            progress(ctx, gi, lib->count);
        const Game *g = &lib->games[gi];
        char gdir[DSAVE_PATH_MAX], folder[sizeof(Folder)];
        if (!g->info.game_code[0] || !backup_game_dir(lib, g, gdir, sizeof(gdir)) || !dir_exists(gdir))
            continue;
        path_basename(folder, sizeof(folder), gdir);
        bool dup = false;
        for (int s = 0; s < seen_n && !dup; s++)
            dup = strcmp(seen[s], folder) == 0;
        if (dup)
            continue;
        snprintf(seen[seen_n++], sizeof(Folder), "%s", folder);
        ok = add_game(&zip, lib, g, folder, mode, &st);
    }
    free(seen);

    ok = ok && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (fclose(f) != 0)
        ok = false;
    if (stats)
        *stats = st;

    DsaveResult r = DSAVE_OK;
    if (!ok)
        r = DSAVE_ERR_IO;
    else if (!st.backups)
        r = DSAVE_ERR_NO_SAVE;
    else if (!verify(tmp))
        r = DSAVE_ERR_VERIFY;
    else if (rename(tmp, target) != 0)
        r = DSAVE_ERR_IO;
    if (r != DSAVE_OK)
    {
        remove(tmp);
        return r;
    }
    if (progress)
        progress(ctx, lib->count, lib->count);
    if (out_path)
        snprintf(out_path, out_size, "%s", target);
    return DSAVE_OK;
}
