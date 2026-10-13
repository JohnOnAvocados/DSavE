#include "library.h"

#include "ini.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// Deep enough for "roms/nds/genre/game.nds", shallow enough to stay quick on
// a large card.
#define SCAN_MAX_DEPTH 4

static bool skip_dir(const char *name)
{
    static const char *const skipped[] = {
        "saves", "TTMenu", "Nintendo 3DS", "System Volume Information",
        "LOST.DIR", "gm9i", "photo", "private", "ramdisks", NULL,
    };
    // Covers _nds, _pico, _DSavE, __rpg, __aio and hidden folders.
    if (name[0] == '_' || name[0] == '.')
        return true;
    for (int i = 0; skipped[i]; i++)
    {
        if (strcasecmp(name, skipped[i]) == 0)
            return true;
    }
    return false;
}

static void stat_loc(SaveLoc *loc)
{
    struct stat st;
    loc->exists = stat(loc->path, &st) == 0 && S_ISREG(st.st_mode);
    loc->size = loc->exists ? (long)st.st_size : -1;
    loc->mtime = loc->exists ? st.st_mtime : 0;
}

int library_loc_for_kernel(const Game *game, KernelId id)
{
    for (int i = 0; i < game->loc_count; i++)
    {
        if (game->locs[i].kernels & KERNEL_BIT(id))
            return i;
    }
    return -1;
}

void library_refresh_game(const Library *lib, Game *game)
{
    const KernelSet *ks = &lib->kernels;
    game->loc_count = 0;

    for (int k = 0; k < KERNEL_COUNT; k++)
    {
        if (!ks->cfg[k].installed)
            continue;
        char path[DSAVE_PATH_MAX];
        int slot = kernel_current_slot(ks, (KernelId)k, game->rom_path);
        if (!kernel_save_path(ks, (KernelId)k, game->rom_path, slot, path, sizeof(path)))
            continue;

        // Merge kernels that use the same file.
        int found = -1;
        for (int i = 0; i < game->loc_count; i++)
        {
            if (strcmp(game->locs[i].path, path) == 0)
                found = i;
        }
        if (found < 0 && game->loc_count < GAME_MAX_LOCS)
        {
            found = game->loc_count++;
            SaveLoc *loc = &game->locs[found];
            memset(loc, 0, sizeof(*loc));
            snprintf(loc->path, sizeof(loc->path), "%s", path);
            stat_loc(loc);
        }
        if (found >= 0)
            game->locs[found].kernels |= KERNEL_BIT(k);
    }

    int active = library_loc_for_kernel(game, ks->active);
    int newest = -1;
    for (int i = 0; i < game->loc_count; i++)
    {
        if (game->locs[i].exists &&
            (newest < 0 || game->locs[i].mtime > game->locs[newest].mtime))
            newest = i;
    }

    game->primary_is_fallback = false;
    if (active >= 0 && game->locs[active].exists)
        game->primary = active;
    else if (newest >= 0)
    {
        game->primary = newest;
        game->primary_is_fallback = true;
    }
    else
        game->primary = active >= 0 ? active : 0;

    const SaveLoc *p = &game->locs[game->primary];
    snprintf(game->save_path, sizeof(game->save_path), "%s", game->loc_count ? p->path : "");
    game->has_save = game->loc_count && p->exists;
    game->save_size = game->has_save ? p->size : -1;
}

static void add_game(Library *lib, const char *rom_path)
{
    if (lib->count >= LIBRARY_MAX_GAMES)
    {
        lib->truncated = true;
        return;
    }

    Game *g = &lib->games[lib->count];
    memset(g, 0, sizeof(*g));
    if (!rom_read_info(rom_path, &g->info) || rom_is_homebrew(&g->info))
        return;

    snprintf(g->rom_path, sizeof(g->rom_path), "%s", rom_path);
    char fallback[DSAVE_PATH_MAX];
    path_basename(fallback, sizeof(fallback), rom_path);
    path_strip_ext(fallback);
    snprintf(g->title, sizeof(g->title), "%s", rom_display_title(&g->info, fallback));
    g->chip_size = kernel_chip_size(&lib->kernels, g->info.game_code);
    library_refresh_game(lib, g);
    lib->count++;
}

static void scan_dir(Library *lib, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && !lib->truncated)
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char path[DSAVE_PATH_MAX];
        if (!path_join(path, sizeof(path), dir, e->d_name))
            continue;

        if (dir_exists(path))
        {
            if (depth < SCAN_MAX_DEPTH && !skip_dir(e->d_name))
                scan_dir(lib, path, depth + 1);
        }
        else if (has_ext(e->d_name, ".nds") || has_ext(e->d_name, ".dsi"))
        {
            add_game(lib, path);
        }
    }
    closedir(d);
}

static int compare_games(const void *a, const void *b)
{
    const Game *ga = a, *gb = b;
    int c = strcasecmp(ga->title, gb->title);
    return c ? c : strcmp(ga->rom_path, gb->rom_path);
}

int library_scan(Library *lib, const char *root)
{
    kernels_free(&lib->kernels);
    snprintf(lib->root, sizeof(lib->root), "%s", root);
    lib->count = 0;
    lib->truncated = false;
    kernels_detect(&lib->kernels, root);
    char settings[DSAVE_PATH_MAX];
    kernels_settings_path(root, settings, sizeof(settings));
    lib->compress = ini_get_int(settings, "dsave", "compress", 0) != 0;

    scan_dir(lib, root, 0);
    qsort(lib->games, (size_t)lib->count, sizeof(Game), compare_games);
    return lib->count;
}

void library_free(Library *lib)
{
    kernels_free(&lib->kernels);
}

void library_set_kernel(Library *lib, KernelId id)
{
    if (!kernels_set_active(&lib->kernels, id))
        return;
    for (int i = 0; i < lib->count; i++)
        library_refresh_game(lib, &lib->games[i]);
}

bool library_set_compress(Library *lib, bool on)
{
    char settings[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    kernels_settings_path(lib->root, settings, sizeof(settings));
    path_dirname(dir, sizeof(dir), settings);
    lib->compress = on;
    return mkdir_p(dir) && ini_set_int(settings, "dsave", "compress", on ? 1 : 0);
}
