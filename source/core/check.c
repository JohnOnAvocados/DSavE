#include "check.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define SAVES_DIR "_DSavE/saves"
#define TMP_PREFIX ".tmp_"

const char *check_kind_name(CheckKind kind)
{
    switch (kind)
    {
        case CHECK_DAMAGED:  return "Damaged";
        case CHECK_NO_META:  return "Can't verify";
        case CHECK_NO_SAVE:  return "Empty folder";
        case CHECK_LEFTOVER: return "Unfinished";
        case CHECK_ORPHAN:   return "No game";
    }
    return "?";
}

static void add_issue(CheckReport *r, CheckKind kind, int game, const char *name, DsaveResult result)
{
    if (r->issue_count < CHECK_ISSUE_MAX)
    {
        CheckIssue *i = &r->issues[r->issue_count];
        i->kind = kind;
        i->game = game;
        snprintf(i->name, sizeof(i->name), "%s", name);
        i->result = result;
    }
    r->issue_count++;
}

static bool listed(const Backup *list, int n, const char *name)
{
    for (int i = 0; i < n; i++)
    {
        if (strcmp(list[i].name, name) == 0)
            return true;
    }
    return false;
}

// Check one game's folder.
static void check_game(const Library *lib, int gi, const char *dir, CheckReport *r)
{
    static Backup list[BACKUP_LIST_MAX];
    const Game *g = &lib->games[gi];
    int n = backup_list(lib, g, list, BACKUP_LIST_MAX);
    r->games++;
    for (int i = 0; i < n; i++)
    {
        r->backups++;
        if (!list[i].has_meta)
        {
            add_issue(r, CHECK_NO_META, gi, list[i].name, DSAVE_OK);
            continue;
        }
        DsaveResult v = backup_validate(&list[i]);
        if (v == DSAVE_OK)
            r->ok++;
        else
            add_issue(r, CHECK_DAMAGED, gi, list[i].name, v);
    }

    // Folders backup_list passed over.
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char path[DSAVE_PATH_MAX];
        if (!path_join(path, sizeof(path), dir, e->d_name) || !dir_exists(path))
            continue;
        if (strncmp(e->d_name, TMP_PREFIX, strlen(TMP_PREFIX)) == 0)
            add_issue(r, CHECK_LEFTOVER, gi, e->d_name, DSAVE_OK);
        else if (e->d_name[0] != '.' && !listed(list, n, e->d_name) && n < BACKUP_LIST_MAX)
            add_issue(r, CHECK_NO_SAVE, gi, e->d_name, DSAVE_OK);
    }
    closedir(d);
}

void check_all(const Library *lib, CheckReport *report,
               void (*progress)(void *ctx, long done, long total), void *ctx)
{
    memset(report, 0, sizeof(*report));
    // Game folders already checked: games that share one (the same game in
    // two ROM folders, or the game card and its ROM) are checked once.
    // Folder names only: they all sit in _DSavE/saves.
    typedef char Folder[64];
    Folder *seen = malloc(sizeof(Folder) * (size_t)(lib->count ? lib->count : 1));
    if (!seen)
        return;
    int seen_n = 0;

    for (int gi = 0; gi < lib->count; gi++)
    {
        if (progress)
            progress(ctx, gi, lib->count);
        char dir[DSAVE_PATH_MAX];
        if (!lib->games[gi].info.game_code[0] || !backup_game_dir(lib, &lib->games[gi], dir, sizeof(dir)) ||
            !dir_exists(dir))
            continue;
        char leaf[sizeof(Folder)];
        path_basename(leaf, sizeof(leaf), dir);
        bool dup = false;
        for (int s = 0; s < seen_n && !dup; s++)
            dup = strcmp(seen[s], leaf) == 0;
        if (dup)
            continue;
        snprintf(seen[seen_n++], sizeof(Folder), "%s", leaf);
        check_game(lib, gi, dir, report);
    }

    // Game folders that no game on the card uses.
    if (lib->truncated)
        report->orphans_skipped = true;
    else
    {
        char saves[DSAVE_PATH_MAX];
        DIR *d = path_join(saves, sizeof(saves), lib->root, SAVES_DIR) ? opendir(saves) : NULL;
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL)
        {
            if (e->d_name[0] == '.')
                continue;
            char path[DSAVE_PATH_MAX];
            if (!path_join(path, sizeof(path), saves, e->d_name) || !dir_exists(path))
                continue;
            bool used = false;
            for (int s = 0; s < seen_n && !used; s++)
                used = strcmp(seen[s], e->d_name) == 0;
            if (!used)
                add_issue(report, CHECK_ORPHAN, -1, e->d_name, DSAVE_OK);
        }
        if (d)
            closedir(d);
    }
    free(seen);
    if (progress)
        progress(ctx, lib->count, lib->count);
}
