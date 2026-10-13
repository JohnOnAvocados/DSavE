#include "backup.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        case DSAVE_ERR_SIZE_MISMATCH: return "Backup size differs from save";
        case DSAVE_ERR_WRONG_GAME:    return "Backup is for another game";
        case DSAVE_ERR_EXISTS:        return "That name is already used";
        case DSAVE_ERR_ROLLED_BACK:   return "Restore failed; old save kept";
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

static bool write_meta(const char *dir, const Game *game, const char *created,
                       long size, uint32_t crc)
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
    fprintf(f, "source=%s\n", game->save_path);
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
    if (!game->has_save || !file_exists(game->save_path))
        return DSAVE_ERR_NO_SAVE;
    if (!allow_blank && file_is_blank(game->save_path))
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

    char sav_name[TITLE_MAX + 8], tmp_file[DSAVE_PATH_MAX];
    save_file_name(game, sav_name, sizeof(sav_name));
    if (!path_join(tmp_file, sizeof(tmp_file), tmp_dir, sav_name))
        return DSAVE_ERR_PATH;

    uint32_t src_crc, dst_crc;
    long src_size, dst_size;
    DsaveResult r = DSAVE_OK;
    if (!file_crc32(game->save_path, &src_crc, &src_size) ||
        !copy_file(game->save_path, tmp_file))
        r = DSAVE_ERR_IO;
    else if (!file_crc32(tmp_file, &dst_crc, &dst_size) ||
             dst_crc != src_crc || dst_size != src_size)
        r = DSAVE_ERR_VERIFY;
    else if (!write_meta(tmp_dir, game, created, src_size, src_crc))
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
    }
    return DSAVE_OK;
}

// ---------------------------------------------------------------------------
// List

// The .sav inside a backup folder: the first *.sav file found.
static bool find_sav(const char *dir, char *out, size_t size)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL)
    {
        if (e->d_name[0] != '.' && has_ext(e->d_name, ".sav"))
            found = path_join(out, size, dir, e->d_name);
    }
    closedir(d);
    return found;
}

static int compare_backups(const void *a, const void *b)
{
    const Backup *ba = a, *bb = b;
    int c = strcmp(bb->created, ba->created); // Newest first
    return c ? c : strcmp(ba->name, bb->name);
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
            b->size = file_size(b->file);
            snprintf(b->created, sizeof(b->created), "%s", b->name);
        }
        b->is_auto = strncmp(b->name, AUTO_PREFIX, strlen(AUTO_PREFIX)) == 0;
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
    if (!file_crc32(b->file, &crc, &size))
        return DSAVE_ERR_IO;
    if (!is_valid_save_size(size))
        return DSAVE_ERR_BAD_SIZE;
    if (b->has_meta && (size != b->size || crc != b->crc32))
        return DSAVE_ERR_CORRUPT;
    return DSAVE_OK;
}

// Overwrite dst with src without truncating or recreating dst. Flashcart
// kernels such as Wood R4 need the save to stay one contiguous file, and an
// in-place write keeps the clusters the kernel already knows about.
static bool write_in_place(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "r+b");
    if (!out)
    {
        fclose(in);
        return false;
    }

    uint8_t buf[IO_CHUNK];
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        if (fwrite(buf, 1, n, out) != n)
        {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    fclose(in);
    if (fclose(out) != 0)
        ok = false;
    return ok;
}

static bool matches(const char *path, uint32_t crc, long size)
{
    uint32_t c;
    long s;
    return file_crc32(path, &c, &s) && c == crc && s == size;
}

DsaveResult backup_restore(const Library *lib, Game *game, const Backup *b,
                           Backup *auto_backup)
{
    if (auto_backup)
        memset(auto_backup, 0, sizeof(*auto_backup));

    DsaveResult r = backup_validate(b);
    if (r != DSAVE_OK)
        return r;
    if (b->game_code[0] && strcmp(b->game_code, game->info.game_code) != 0)
        return DSAVE_ERR_WRONG_GAME;

    uint32_t crc;
    long size;
    if (!file_crc32(b->file, &crc, &size))
        return DSAVE_ERR_IO;

    library_refresh_game(lib, game);

    if (!game->has_save)
    {
        // First save for this game: create it where the loader looks.
        char dir[DSAVE_PATH_MAX];
        path_dirname(dir, sizeof(dir), game->save_path);
        if (!mkdir_p(dir) || !copy_file(b->file, game->save_path))
            return DSAVE_ERR_IO;
        library_refresh_game(lib, game);
        return matches(game->save_path, crc, size) ? DSAVE_OK : DSAVE_ERR_VERIFY;
    }

    if (game->save_size != size)
        return DSAVE_ERR_SIZE_MISMATCH;

    // Safety copy of what is there now. Blank saves are kept too: losing a
    // blank save costs nothing, but refusing here would block the restore.
    char auto_name[BACKUP_NAME_MAX + 1], stamp[24];
    timestamp_now(stamp, sizeof(stamp));
    snprintf(auto_name, sizeof(auto_name), AUTO_PREFIX "%s", stamp);
    Backup safety;
    r = backup_create(lib, game, auto_name, true, &safety);
    if (r != DSAVE_OK)
        return r;
    if (auto_backup)
        *auto_backup = safety;

    if (write_in_place(b->file, game->save_path) && matches(game->save_path, crc, size))
    {
        library_refresh_game(lib, game);
        return DSAVE_OK;
    }

    // Roll back from the safety copy.
    write_in_place(safety.file, game->save_path);
    library_refresh_game(lib, game);
    return DSAVE_ERR_ROLLED_BACK;
}

// ---------------------------------------------------------------------------
// Rename, trash, export

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

    uint32_t crc;
    long size;
    if (!file_crc32(b->file, &crc, &size) || !copy_file(b->file, target))
        return DSAVE_ERR_IO;
    if (!matches(target, crc, size))
        return DSAVE_ERR_VERIFY;

    if (out_path)
        snprintf(out_path, out_size, "%s", target);
    return DSAVE_OK;
}
