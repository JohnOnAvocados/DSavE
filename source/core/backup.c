#include "backup.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ini.h"
#include "savefile.h"

#define APP_DIR    "_DSavE"
#define SAVES_DIR  "_DSavE/saves"
#define TRASH_DIR  "_DSavE/_TRASH_"
#define EXPORT_DIR "_DSavE/export"
#define META_FILE  "backup.ini"
#define TMP_PREFIX ".tmp_"
#define AUTO_PREFIX "auto_"

#define TITLE_MAX 32
#define IO_CHUNK  4096

const char *dsave_result_str(DsaveResult r)
{
    switch (r)
    {
        case DSAVE_OK:                return "Done";
        case DSAVE_WARN_BLANK:        return "Save is blank (all 00 or FF)";
        case DSAVE_ERR_NO_SAVE:       return "This game has no save file yet";
        case DSAVE_ERR_IO:            return "SD card read/write failed";
        case DSAVE_ERR_PATH:          return "Path too long";
        case DSAVE_ERR_VERIFY:        return "Copy did not verify";
        case DSAVE_ERR_CORRUPT:       return "Backup is corrupt (CRC/size)";
        case DSAVE_ERR_BAD_SIZE:      return "Not a valid DS save size";
        case DSAVE_ERR_WRONG_GAME:    return "Backup is for another game";
        case DSAVE_ERR_EXISTS:        return "That name is already used";
        case DSAVE_ERR_ROLLED_BACK:   return "Restore failed; old save kept";
        case DSAVE_ERR_UNSUPPORTED:   return "Not supported by this kernel";
        case DSAVE_ERR_CARD_CHANGED:  return "The game card was removed or changed";
        case DSAVE_ERR_CARD:          return "The game card's save chip did not answer";
    }
    return "Unknown error";
}

// ---------------------------------------------------------------------------
// Paths

static void save_file_name(const Game *game, char *out, size_t size)
{
    char title[TITLE_MAX + 1];
    sanitize_name(title, sizeof(title), game->title, TITLE_MAX);
    snprintf(out, size, "%s.sav", title);
}

bool backup_game_dir(const Library *lib, const Game *game, char *out, size_t size)
{
    char title[TITLE_MAX + 1], folder[TITLE_MAX + 8], saves[DSAVE_PATH_MAX];
    sanitize_name(title, sizeof(title), game->title, TITLE_MAX);
    // Game code first, so two games with the same title never share a folder.
    snprintf(folder, sizeof(folder), "%s %s", game->info.game_code, title);
    return path_join(saves, sizeof(saves), lib->root, SAVES_DIR) &&
           path_join(out, size, saves, folder);
}

// Find "<parent>/<name>", or "<name>_2", "<name>_3"... that does not exist.
static bool unique_path(char *out, size_t size, const char *parent, const char *name,
                        const char *ext)
{
    for (int i = 1; i < 1000; i++)
    {
        char leaf[BACKUP_NAME_MAX + 32];
        if (i == 1)
            snprintf(leaf, sizeof(leaf), "%s%s", name, ext);
        else
            snprintf(leaf, sizeof(leaf), "%s_%d%s", name, i, ext);
        if (!path_join(out, size, parent, leaf))
            return false;
        if (!file_exists(out) && !dir_exists(out))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// backup.ini

static bool write_meta(const char *dir, const Game *game, const char *src,
                       const char *created, long size, uint32_t crc)
{
    char path[DSAVE_PATH_MAX];
    if (!path_join(path, sizeof(path), dir, META_FILE))
        return false;
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "code=%s\n", game->info.game_code);
    fprintf(f, "title=%s\n", game->title);
    fprintf(f, "size=%ld\n", size);
    fprintf(f, "crc32=%08" PRIX32 "\n", crc);
    fprintf(f, "created=%s\n", created);
    fprintf(f, "source=%s\n", src);
    return fclose(f) == 0;
}

static bool read_meta(Backup *b)
{
    char path[DSAVE_PATH_MAX];
    if (!path_join(path, sizeof(path), b->dir, META_FILE))
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    bool got_size = false, got_crc = false;
    char line[DSAVE_PATH_MAX + 16];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = line, *val = eq + 1;
        if (strcmp(key, "code") == 0)
            snprintf(b->game_code, sizeof(b->game_code), "%s", val);
        else if (strcmp(key, "created") == 0)
            snprintf(b->created, sizeof(b->created), "%s", val);
        else if (strcmp(key, "size") == 0)
            b->size = strtol(val, NULL, 10), got_size = true;
        else if (strcmp(key, "crc32") == 0)
            b->crc32 = (uint32_t)strtoul(val, NULL, 16), got_crc = true;
    }
    fclose(f);
    return got_size && got_crc;
}

// ---------------------------------------------------------------------------
// Create

DsaveResult backup_create(const Library *lib, const Game *game, const char *name,
                          bool allow_blank, Backup *out)
{
    if (!game->has_save)
        return DSAVE_ERR_NO_SAVE;
    return backup_create_from(lib, game, game->save_path, name, allow_blank, out);
}

DsaveResult backup_create_from(const Library *lib, const Game *game, const char *src,
                               const char *name, bool allow_blank, Backup *out)
{
    return backup_create_labeled(lib, game, src, src, name, allow_blank, out);
}

DsaveResult backup_create_labeled(const Library *lib, const Game *game, const char *src,
                                  const char *source_label, const char *name, bool allow_blank,
                                  Backup *out)
{
    if (!file_exists(src))
        return DSAVE_ERR_NO_SAVE;
    if (!allow_blank && file_is_blank(src))
        return DSAVE_WARN_BLANK;

    char game_dir[DSAVE_PATH_MAX];
    if (!backup_game_dir(lib, game, game_dir, sizeof(game_dir)))
        return DSAVE_ERR_PATH;
    if (!mkdir_p(game_dir))
        return DSAVE_ERR_IO;

    char created[24], clean[BACKUP_NAME_MAX + 1];
    timestamp_now(created, sizeof(created));
    sanitize_name(clean, sizeof(clean), (name && *name) ? name : created, BACKUP_NAME_MAX);

    // Work in a hidden temp folder; a crash leaves only a ".tmp_" leftover
    // that backup_list ignores.
    char tmp_leaf[BACKUP_NAME_MAX + 8], tmp_dir[DSAVE_PATH_MAX];
    snprintf(tmp_leaf, sizeof(tmp_leaf), TMP_PREFIX "%s", clean);
    if (!path_join(tmp_dir, sizeof(tmp_dir), game_dir, tmp_leaf))
        return DSAVE_ERR_PATH;
    if (dir_exists(tmp_dir))
        remove_tree(tmp_dir);
    if (!mkdir_p(tmp_dir))
        return DSAVE_ERR_IO;

    uint32_t src_crc, dst_crc;
    long src_size, dst_size;
    if (!savefile_crc32(src, &src_crc, &src_size))
        return DSAVE_ERR_IO;

    // Compress when the setting is on and the save fits in memory.
    bool compress = lib->compress && src_size <= SAVEFILE_GZ_MAX;
    char sav_name[TITLE_MAX + 16], tmp_file[DSAVE_PATH_MAX];
    save_file_name(game, sav_name, sizeof(sav_name));
    if (compress)
        strcat(sav_name, SAVEFILE_GZ_EXT);
    if (!path_join(tmp_file, sizeof(tmp_file), tmp_dir, sav_name))
        return DSAVE_ERR_PATH;

    DsaveResult r = DSAVE_OK;
    bool written = compress ? savefile_write_gz(src, tmp_file, NULL, NULL)
                            : savefile_copy_raw(src, tmp_file);
    if (!written)
        r = DSAVE_ERR_IO;
    else if (!savefile_crc32(tmp_file, &dst_crc, &dst_size) ||
             dst_crc != src_crc || dst_size != src_size)
        r = DSAVE_ERR_VERIFY;
    else if (!write_meta(tmp_dir, game, source_label, created, src_size, src_crc))
        r = DSAVE_ERR_IO;

    char final_dir[DSAVE_PATH_MAX];
    if (r == DSAVE_OK && !unique_path(final_dir, sizeof(final_dir), game_dir, clean, ""))
        r = DSAVE_ERR_PATH;
    if (r == DSAVE_OK && rename(tmp_dir, final_dir) != 0)
        r = DSAVE_ERR_IO;

    if (r != DSAVE_OK)
    {
        remove_tree(tmp_dir);
        return r;
    }

    if (out)
    {
        memset(out, 0, sizeof(*out));
        path_basename(out->name, sizeof(out->name), final_dir);
        snprintf(out->dir, sizeof(out->dir), "%s", final_dir);
        path_join(out->file, sizeof(out->file), final_dir, sav_name);
        snprintf(out->game_code, sizeof(out->game_code), "%s", game->info.game_code);
        snprintf(out->created, sizeof(out->created), "%s", created);
        out->size = src_size;
        out->crc32 = src_crc;
        out->has_meta = true;
        out->is_auto = strncmp(out->name, AUTO_PREFIX, strlen(AUTO_PREFIX)) == 0;
        out->compressed = compress;
    }
    return DSAVE_OK;
}

// ---------------------------------------------------------------------------
// List

// The save inside a backup folder: the first *.sav or *.sav.gz found.
static bool find_sav(const char *dir, char *out, size_t size)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL)
    {
        if (e->d_name[0] != '.' && (has_ext(e->d_name, ".sav") || has_ext(e->d_name, ".sav.gz")))
            found = path_join(out, size, dir, e->d_name);
    }
    closedir(d);
    return found;
}

static int compare_backups(const void *a, const void *b)
{
    const Backup *ba = a, *bb = b;
    int c = strcmp(bb->created, ba->created); // Newest first
    if (c)
        return c;
    // Same second: a clash suffix ("_2", "_10") marks the later one.
    size_t la = strlen(ba->name), lb = strlen(bb->name);
    if (la != lb)
        return la < lb ? 1 : -1;
    return strcmp(bb->name, ba->name);
}

int backup_list(const Library *lib, const Game *game, Backup *out, int max)
{
    char game_dir[DSAVE_PATH_MAX];
    if (!backup_game_dir(lib, game, game_dir, sizeof(game_dir)))
        return 0;
    DIR *d = opendir(game_dir);
    if (!d)
        return 0;

    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue; // ".", "..", and unfinished ".tmp_" folders

        Backup *b = &out[n];
        memset(b, 0, sizeof(*b));
        if (!path_join(b->dir, sizeof(b->dir), game_dir, e->d_name) || !dir_exists(b->dir))
            continue;
        if (!find_sav(b->dir, b->file, sizeof(b->file)))
            continue;

        snprintf(b->name, sizeof(b->name), "%s", e->d_name);
        b->has_meta = read_meta(b);
        if (!b->has_meta)
        {
            // Hand-copied backup: fall back to what the file itself says.
            b->size = savefile_size(b->file);
            snprintf(b->created, sizeof(b->created), "%s", b->name);
        }
        b->is_auto = strncmp(b->name, AUTO_PREFIX, strlen(AUTO_PREFIX)) == 0;
        b->compressed = savefile_is_gz(b->file);
        n++;
    }
    closedir(d);

    qsort(out, (size_t)n, sizeof(Backup), compare_backups);
    return n;
}

// ---------------------------------------------------------------------------
// Validate and restore

DsaveResult backup_validate(const Backup *b)
{
    uint32_t crc;
    long size;
    if (!savefile_crc32(b->file, &crc, &size))
        return b->compressed || savefile_is_gz(b->file) ? DSAVE_ERR_CORRUPT : DSAVE_ERR_IO;
    if (!is_valid_save_size(size))
        return DSAVE_ERR_BAD_SIZE;
    if (b->has_meta && (size != b->size || crc != b->crc32))
        return DSAVE_ERR_CORRUPT;
    return DSAVE_OK;
}

// Write len bytes to dst: src's save data (raw or .gz) (up to src_len bytes), then 0xFF
// padding. An existing dst is overwritten in place, never truncated or
// recreated, so a flashcart's pre-allocated, contiguous file keeps its
// clusters (Wood R4 refuses fragmented saves). crc_out receives the CRC-32
// of exactly what was written.
static bool write_save_stream(const char *src, long src_len, const char *dst, long len,
                              uint32_t *crc_out)
{
    SaveReader in;
    if (!savefile_open(&in, src))
        return false;
    FILE *out = fopen(dst, file_exists(dst) ? "r+b" : "wb");
    if (!out)
    {
        savefile_close(&in);
        return false;
    }

    uint8_t buf[IO_CHUNK];
    uint32_t crc = 0;
    long done = 0;
    bool ok = true;
    while (ok && done < len)
    {
        size_t want = (size_t)(len - done < IO_CHUNK ? len - done : IO_CHUNK);
        size_t got = 0;
        if (done < src_len)
        {
            size_t from_src = (size_t)(src_len - done) < want ? (size_t)(src_len - done) : want;
            got = savefile_read(&in, buf, from_src);
            if (got != from_src)
                ok = false;
        }
        memset(buf + got, 0xFF, want - got);
        if (ok && fwrite(buf, 1, want, out) != want)
            ok = false;
        crc = crc32_update(crc, buf, want);
        done += (long)want;
    }
    savefile_close(&in);
    if (fclose(out) != 0)
        ok = false;
    if (crc_out)
        *crc_out = crc;
    return ok;
}

static bool matches(const char *path, uint32_t crc, long size)
{
    uint32_t c;
    long s;
    return file_crc32(path, &c, &s) && c == crc && s == size;
}

long backup_restore_length(const char *backup_file, long backup_size, long live_size)
{
    if (live_size < 0)
        return backup_size;      // New file: exactly the backup
    if (backup_size <= live_size)
        return live_size;        // Keep the kernel's size; pad with 0xFF
    // Backup is bigger (e.g. a 512 KB padded file going onto an exact-size
    // save). Drop the extra only if it is padding; never lose data.
    return savefile_tail_is_ff(backup_file, live_size) ? live_size : backup_size;
}

DsaveResult backup_restore_to(const Library *lib, Game *game, const Backup *b, int loc,
                              Backup *auto_backup)
{
    library_refresh_game(lib, game);
    if (loc < 0)
        loc = game->primary;
    if (loc >= game->loc_count)
    {
        if (auto_backup)
            memset(auto_backup, 0, sizeof(*auto_backup));
        return DSAVE_ERR_PATH;
    }
    char path[DSAVE_PATH_MAX];
    snprintf(path, sizeof(path), "%s", game->locs[loc].path);
    return backup_restore_path(lib, game, b, path, auto_backup);
}

DsaveResult backup_restore_path(const Library *lib, Game *game, const Backup *b,
                                const char *path, Backup *auto_backup)
{
    if (auto_backup)
        memset(auto_backup, 0, sizeof(*auto_backup));

    DsaveResult r = backup_validate(b);
    if (r != DSAVE_OK)
        return r;
    if (b->game_code[0] && strcmp(b->game_code, game->info.game_code) != 0)
        return DSAVE_ERR_WRONG_GAME;

    struct {
        const char *path;
        bool exists;
        long size;
    } target = { path, file_exists(path), file_size(path) };

    long backup_size = savefile_size(b->file);
    long len = backup_restore_length(b->file, backup_size, target.exists ? target.size : -1);
    uint32_t crc;

    if (!target.exists)
    {
        // First save at this location: create it where the kernel looks.
        char dir[DSAVE_PATH_MAX];
        path_dirname(dir, sizeof(dir), target.path);
        if (!mkdir_p(dir) || !write_save_stream(b->file, backup_size, target.path, len, &crc))
            return DSAVE_ERR_IO;
        library_refresh_game(lib, game);
        return matches(target.path, crc, len) ? DSAVE_OK : DSAVE_ERR_VERIFY;
    }

    // Safety copy of what is there now. Blank saves are kept too: losing a
    // blank save costs nothing, but refusing here would block the restore.
    char auto_name[BACKUP_NAME_MAX + 1];
    backup_auto_name(auto_name, sizeof(auto_name));
    Backup safety;
    r = backup_create_from(lib, game, target.path, auto_name, true, &safety);
    if (r != DSAVE_OK)
        return r;
    if (auto_backup)
        *auto_backup = safety;
    backup_prune_auto(lib, game, lib->keep_auto, safety.name);

    if (write_save_stream(b->file, backup_size, target.path, len, &crc) &&
        matches(target.path, crc, len))
    {
        library_refresh_game(lib, game);
        return DSAVE_OK;
    }

    // Roll back from the safety copy.
    write_save_stream(safety.file, safety.size, target.path, safety.size, NULL);
    library_refresh_game(lib, game);
    return DSAVE_ERR_ROLLED_BACK;
}

void backup_auto_name(char *out, size_t size)
{
    char stamp[24];
    timestamp_now(stamp, sizeof(stamp));
    snprintf(out, size, AUTO_PREFIX "%s", stamp);
}

DsaveResult backup_restore(const Library *lib, Game *game, const Backup *b,
                           Backup *auto_backup)
{
    return backup_restore_to(lib, game, b, -1, auto_backup);
}

// ---------------------------------------------------------------------------
// Rename, trash, export

DsaveResult backup_set_compressed(const Backup *b, bool compress, Backup *out)
{
    if (out)
        *out = *b;
    bool is_gz = savefile_is_gz(b->file);
    if (is_gz == compress)
        return DSAVE_OK;

    DsaveResult r = backup_validate(b);
    if (r != DSAVE_OK)
        return r;
    uint32_t crc, new_crc;
    long size, new_size;
    if (!savefile_crc32(b->file, &crc, &size))
        return DSAVE_ERR_IO;
    if (compress && size > SAVEFILE_GZ_MAX)
        return DSAVE_ERR_UNSUPPORTED;

    // "<title>.sav" <-> "<title>.sav.gz", next to the old file.
    char target[DSAVE_PATH_MAX];
    snprintf(target, sizeof(target), "%s", b->file);
    if (compress)
    {
        if (strlen(target) + strlen(SAVEFILE_GZ_EXT) >= sizeof(target))
            return DSAVE_ERR_PATH;
        strcat(target, SAVEFILE_GZ_EXT);
    }
    else
        target[strlen(target) - strlen(SAVEFILE_GZ_EXT)] = '\0';

    bool ok = compress ? savefile_write_gz(b->file, target, NULL, NULL)
                       : savefile_copy_raw(b->file, target);
    if (!ok)
    {
        remove(target);
        return DSAVE_ERR_IO;
    }
    if (!savefile_crc32(target, &new_crc, &new_size) || new_crc != crc || new_size != size)
    {
        remove(target);
        return DSAVE_ERR_VERIFY;
    }
    if (remove(b->file) != 0)
        return DSAVE_ERR_IO;

    if (out)
    {
        snprintf(out->file, sizeof(out->file), "%s", target);
        out->compressed = compress;
    }
    return DSAVE_OK;
}

DsaveResult backup_rename(const Library *lib, const Game *game, const Backup *b,
                          const char *new_name)
{
    char game_dir[DSAVE_PATH_MAX], clean[BACKUP_NAME_MAX + 1], target[DSAVE_PATH_MAX];
    if (!backup_game_dir(lib, game, game_dir, sizeof(game_dir)))
        return DSAVE_ERR_PATH;
    sanitize_name(clean, sizeof(clean), new_name, BACKUP_NAME_MAX);
    if (strcmp(clean, b->name) == 0)
        return DSAVE_OK;
    if (!path_join(target, sizeof(target), game_dir, clean))
        return DSAVE_ERR_PATH;
    if (dir_exists(target) || file_exists(target))
        return DSAVE_ERR_EXISTS;
    return rename(b->dir, target) == 0 ? DSAVE_OK : DSAVE_ERR_IO;
}

DsaveResult backup_trash(const Library *lib, const Game *game, const Backup *b)
{
    char trash[DSAVE_PATH_MAX], leaf[BACKUP_NAME_MAX + 8], target[DSAVE_PATH_MAX];
    if (!path_join(trash, sizeof(trash), lib->root, TRASH_DIR))
        return DSAVE_ERR_PATH;
    if (!mkdir_p(trash))
        return DSAVE_ERR_IO;
    snprintf(leaf, sizeof(leaf), "%s_%s", game->info.game_code, b->name);
    if (!unique_path(target, sizeof(target), trash, leaf, ""))
        return DSAVE_ERR_PATH;
    return rename(b->dir, target) == 0 ? DSAVE_OK : DSAVE_ERR_IO;
}

DsaveResult backup_export(const Library *lib, const Game *game, const Backup *b,
                          char *out_path, size_t out_size)
{
    DsaveResult r = backup_validate(b);
    if (r != DSAVE_OK)
        return r;

    char dir[DSAVE_PATH_MAX], title[TITLE_MAX + 1], leaf[TITLE_MAX + BACKUP_NAME_MAX + 16];
    char target[DSAVE_PATH_MAX];
    if (!path_join(dir, sizeof(dir), lib->root, EXPORT_DIR))
        return DSAVE_ERR_PATH;
    if (!mkdir_p(dir))
        return DSAVE_ERR_IO;
    sanitize_name(title, sizeof(title), game->title, TITLE_MAX);
    snprintf(leaf, sizeof(leaf), "%s_%s_%s", title, game->info.game_code, b->name);
    if (!unique_path(target, sizeof(target), dir, leaf, ".sav"))
        return DSAVE_ERR_PATH;

    // Export the real chip image when the chip size is known and the rest of
    // the file is only a kernel's 0xFF padding.
    long size = savefile_size(b->file);
    long len = size;
    if (game->chip_size > 0 && size > game->chip_size && savefile_tail_is_ff(b->file, game->chip_size))
        len = game->chip_size;

    uint32_t crc;
    if (size < 0 || !write_save_stream(b->file, size, target, len, &crc))
        return DSAVE_ERR_IO;
    if (!matches(target, crc, len))
        return DSAVE_ERR_VERIFY;

    if (out_path)
        snprintf(out_path, out_size, "%s", target);
    return DSAVE_OK;
}

// ---------------------------------------------------------------------------
// Auto backup pruning

int backup_prune_auto(const Library *lib, const Game *game, int keep, const char *protect)
{
    if (keep <= 0)
        return 0;
    static Backup list[BACKUP_LIST_MAX];
    int n = backup_list(lib, game, list, BACKUP_LIST_MAX);
    int autos = 0, trashed = 0;
    bool protected_seen = false;
    for (int i = 0; i < n; i++)
    {
        if (list[i].is_auto && protect && strcmp(list[i].name, protect) == 0)
            protected_seen = true;
    }
    if (protected_seen)
        autos = 1;
    for (int i = 0; i < n; i++) // Newest first
    {
        if (!list[i].is_auto || (protected_seen && strcmp(list[i].name, protect) == 0) ||
            ++autos <= keep)
            continue;
        if (backup_trash(lib, game, &list[i]) == DSAVE_OK)
            trashed++;
    }
    return trashed;
}

// ---------------------------------------------------------------------------
// Import

#define DSV_MARKER "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:"
#define DSV_END "|-DESMUME SAVE-|"
#define NOCASH_MAGIC "NocashGbaBackupMediaSavDataFile"

// Length of the save data in a raw file: everything before a DeSmuME
// footer, or the whole file. Returns -1 for a no$gba save, whose data is
// wrapped (and often compressed) in its own format.
static long raw_data_length(const char *path)
{
    long size = file_size(path);
    if (size < 0)
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    char head[sizeof(NOCASH_MAGIC)] = "";
    if (fread(head, 1, sizeof(head) - 1, f) == sizeof(head) - 1 && strcmp(head, NOCASH_MAGIC) == 0)
    {
        fclose(f);
        return -1;
    }

    // The DeSmuME footer is a marker, a few fields and DSV_END at the end.
    char tail[256 + 1];
    long n = size < 256 ? size : 256;
    long len = size;
    if (fseek(f, size - n, SEEK_SET) == 0 && fread(tail, 1, (size_t)n, f) == (size_t)n)
    {
        long end = (long)strlen(DSV_END);
        if (n >= end && memcmp(tail + n - end, DSV_END, (size_t)end) == 0)
        {
            long marker = (long)strlen(DSV_MARKER);
            for (long i = n - end - marker; i >= 0; i--)
            {
                if (memcmp(tail + i, DSV_MARKER, (size_t)marker) == 0)
                {
                    len = size - n + i;
                    break;
                }
            }
        }
    }
    fclose(f);
    return len;
}

static bool importable(const char *name)
{
    return has_ext(name, ".sav") || has_ext(name, ".dsv") || has_ext(name, ".sav.gz");
}

static int compare_imports(const void *a, const void *b)
{
    return strcasecmp(((const ImportFile *)a)->name, ((const ImportFile *)b)->name);
}

int backup_import_list(const Library *lib, ImportFile *out, int max)
{
    char dir[DSAVE_PATH_MAX];
    if (!path_join(dir, sizeof(dir), lib->root, IMPORT_DIR))
        return 0;
    mkdir_p(dir); // So it is there to copy files into
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.' || !importable(e->d_name))
            continue;
        ImportFile *f = &out[n];
        snprintf(f->name, sizeof(f->name), "%s", e->d_name);
        if (!path_join(f->path, sizeof(f->path), dir, e->d_name) || !file_exists(f->path))
            continue;
        f->size = savefile_is_gz(f->path) ? savefile_size(f->path) : raw_data_length(f->path);
        f->valid = is_valid_save_size(f->size);
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(ImportFile), compare_imports);
    return n;
}

DsaveResult backup_import(const Library *lib, const Game *game, const char *path, Backup *out)
{
    if (!file_exists(path))
        return DSAVE_ERR_NO_SAVE;
    bool gz = savefile_is_gz(path);
    long len = gz ? savefile_size(path) : raw_data_length(path);
    if (!gz && len < 0 && file_size(path) >= 0)
        return DSAVE_ERR_UNSUPPORTED; // no$gba
    if (len < 0)
        return gz ? DSAVE_ERR_CORRUPT : DSAVE_ERR_IO;
    if (!is_valid_save_size(len))
        return DSAVE_ERR_BAD_SIZE;

    char stem[DSAVE_PATH_MAX], name[BACKUP_NAME_MAX + 1];
    path_basename(stem, sizeof(stem), path);
    if (gz)
        path_strip_ext(stem);
    path_strip_ext(stem);
    snprintf(name, sizeof(name), "import_%s", stem);

    if (gz || len == file_size(path))
        return backup_create_labeled(lib, game, path, path, name, true, out);

    // Trim the emulator footer into a temporary file first.
    char tmp[DSAVE_PATH_MAX];
    if (!path_join(tmp, sizeof(tmp), lib->root, APP_DIR "/.import.tmp"))
        return DSAVE_ERR_PATH;
    FILE *in = fopen(path, "rb"), *o = fopen(tmp, "wb");
    bool ok = in && o;
    uint8_t buf[IO_CHUNK];
    for (long done = 0; ok && done < len;)
    {
        size_t want = (size_t)(len - done < IO_CHUNK ? len - done : IO_CHUNK);
        ok = fread(buf, 1, want, in) == want && fwrite(buf, 1, want, o) == want;
        done += (long)want;
    }
    if (in)
        fclose(in);
    if (o && fclose(o) != 0)
        ok = false;
    DsaveResult r = ok ? backup_create_labeled(lib, game, tmp, path, name, true, out) : DSAVE_ERR_IO;
    remove(tmp);
    return r;
}
