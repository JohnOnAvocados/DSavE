#include "activity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static const char *const kind_keys[] = {
    [ACT_BACKUP] = "backup", [ACT_RESTORE] = "restore", [ACT_COPY] = "copy",  [ACT_IMPORT] = "import",
    [ACT_TRASH] = "trash",   [ACT_EXPORT] = "export",   [ACT_UNDO] = "undo",
};

const char *activity_kind_name(ActivityKind kind)
{
    static const char *const names[] = {
        [ACT_BACKUP] = "Backup", [ACT_RESTORE] = "Restore", [ACT_COPY] = "Copy",  [ACT_IMPORT] = "Import",
        [ACT_TRASH] = "Trash",   [ACT_EXPORT] = "Export",   [ACT_UNDO] = "Undo",
    };
    return kind < ACT_UNKNOWN ? names[kind] : "?";
}

// Copy a field, turning tabs and line breaks into spaces.
static void put_field(FILE *f, const char *s)
{
    for (; s && *s; s++)
        fputc(*s == '\t' || *s == '\r' || *s == '\n' ? ' ' : *s, f);
}

bool activity_log(const Library *lib, ActivityKind kind, const Game *game, const char *backup,
                  uint32_t crc32, const char *safety, const char *path)
{
    if (kind >= ACT_UNKNOWN || !game)
        return false;
    char file[DSAVE_PATH_MAX], old[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    if (!path_join(file, sizeof(file), lib->root, ACTIVITY_FILE) ||
        !path_join(old, sizeof(old), lib->root, ACTIVITY_OLD_FILE))
        return false;
    path_dirname(dir, sizeof(dir), file);
    if (!mkdir_p(dir))
        return false;
    if (file_size(file) > ACTIVITY_ROTATE_BYTES)
    {
        remove(old);
        rename(file, old);
    }

    FILE *f = fopen(file, "a");
    if (!f)
        return false;
    char now[24];
    timestamp_now(now, sizeof(now));
    fprintf(f, "%s\t%s\t", now, kind_keys[kind]);
    put_field(f, game->info.game_code);
    fputc('\t', f);
    put_field(f, game->title);
    fputc('\t', f);
    put_field(f, backup);
    fprintf(f, "\t%08lX\t", (unsigned long)crc32);
    put_field(f, safety);
    fputc('\t', f);
    put_field(f, game->is_card ? "card" : path);
    fputc('\n', f);
    return fclose(f) == 0;
}

// Split one line into an entry. False for anything that isn't one.
static bool parse_line(char *line, Activity *a)
{
    line[strcspn(line, "\r\n")] = '\0';
    char *fields[8];
    int n = 0;
    char *p = line;
    while (n < 8)
    {
        fields[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab)
            break;
        *tab = '\0';
        p = tab + 1;
    }
    if (n != 8 || strlen(fields[0]) < 19)
        return false;

    memset(a, 0, sizeof(*a));
    snprintf(a->time, sizeof(a->time), "%s", fields[0]);
    a->kind = ACT_UNKNOWN;
    for (int k = 0; k < ACT_UNKNOWN; k++)
    {
        if (strcmp(fields[1], kind_keys[k]) == 0)
            a->kind = (ActivityKind)k;
    }
    if (a->kind == ACT_UNKNOWN)
        return false;
    snprintf(a->game_code, sizeof(a->game_code), "%s", fields[2]);
    snprintf(a->title, sizeof(a->title), "%s", fields[3]);
    snprintf(a->backup, sizeof(a->backup), "%s", fields[4]);
    a->crc32 = (uint32_t)strtoul(fields[5], NULL, 16);
    snprintf(a->safety, sizeof(a->safety), "%s", fields[6]);
    a->card = strcmp(fields[7], "card") == 0;
    if (!a->card)
        snprintf(a->path, sizeof(a->path), "%s", fields[7]);
    return true;
}

// Read one log file into a ring of the last `max` entries.
static void read_file(const char *path, Activity *ring, int max, int *count)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[DSAVE_PATH_MAX * 2 + 256];
    while (fgets(line, sizeof(line), f))
    {
        if (!strchr(line, '\n') && !feof(f))
        {
            // Too long to be ours: skip the rest of it.
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;
            continue;
        }
        Activity a;
        if (parse_line(line, &a))
            ring[(*count)++ % max] = a;
    }
    fclose(f);
}

int activity_list(const Library *lib, Activity *out, int max)
{
    if (max <= 0)
        return 0;
    char file[DSAVE_PATH_MAX], old[DSAVE_PATH_MAX];
    if (!path_join(file, sizeof(file), lib->root, ACTIVITY_FILE) ||
        !path_join(old, sizeof(old), lib->root, ACTIVITY_OLD_FILE))
        return 0;
    Activity *ring = malloc(sizeof(Activity) * (size_t)max);
    if (!ring)
        return 0;
    int count = 0;
    read_file(old, ring, max, &count);
    read_file(file, ring, max, &count);

    // Newest first.
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++)
        out[i] = ring[(count - 1 - i) % max];
    free(ring);
    return n;
}

bool activity_can_undo(const Activity *a)
{
    return (a->kind == ACT_RESTORE || a->kind == ACT_COPY || a->kind == ACT_UNDO) && a->safety[0] && !a->card && a->path[0];
}

// The game the entry is about: one with the same game code that keeps a
// save at the entry's path, else the only game with that code.
static int find_game(const Library *lib, const Activity *a)
{
    int by_code = -1, matches = 0;
    for (int i = 0; i < lib->count; i++)
    {
        const Game *g = &lib->games[i];
        if (g->is_card || strcmp(g->info.game_code, a->game_code) != 0)
            continue;
        for (int l = 0; l < g->loc_count; l++)
        {
            if (strcmp(g->locs[l].path, a->path) == 0)
                return i;
        }
        by_code = i;
        matches++;
    }
    return matches == 1 ? by_code : -1;
}

DsaveResult activity_undo(Library *lib, const Activity *a, int *game_out, Backup *auto_backup)
{
    if (auto_backup)
        memset(auto_backup, 0, sizeof(*auto_backup));
    if (!activity_can_undo(a))
        return DSAVE_ERR_UNSUPPORTED;
    int gi = find_game(lib, a);
    if (gi < 0)
        return DSAVE_ERR_GONE;
    if (game_out)
        *game_out = gi;
    Game *game = &lib->games[gi];

    static Backup list[BACKUP_LIST_MAX];
    int n = backup_list(lib, game, list, BACKUP_LIST_MAX);
    for (int i = 0; i < n; i++)
    {
        if (strcmp(list[i].name, a->safety) != 0)
            continue;
        Backup safety = list[i], made;
        DsaveResult r = backup_restore_path(lib, game, &safety, a->path, &made);
        if (r == DSAVE_OK)
            activity_log(lib, ACT_UNDO, game, safety.name, safety.crc32, made.name, a->path);
        if (auto_backup)
            *auto_backup = made;
        return r;
    }
    return DSAVE_ERR_GONE;
}
