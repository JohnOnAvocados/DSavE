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

static bool stat_file(const char *path, long *size, time_t *mtime)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    *size = (long)st.st_size;
    *mtime = st.st_mtime;
    return true;
}

bool library_stat_save(const Game *game, const char *path, long *size, time_t *mtime)
{
    long total = 0, s;
    time_t newest = 0, t;
    bool exists = false;
    if (!game->info.dsiware)
        exists = stat_file(path, &total, &newest);
    for (int part = 0; game->info.dsiware && part < DSI_PARTS; part++)
    {
        char p[DSAVE_PATH_MAX];
        if (!library_dsi_part_path(game, path, (DsiPart)part, p, sizeof(p)) || !stat_file(p, &s, &t))
            continue;
        exists = true;
        total += s;
        if (t > newest)
            newest = t;
    }
    if (size)
        *size = exists ? total : -1;
    if (mtime)
        *mtime = exists ? newest : 0;
    return exists;
}

DsiPart library_dsi_main_part(const Game *game)
{
    for (int part = 0; part < DSI_PARTS; part++)
    {
        if (rom_dsi_has_part(&game->info, (DsiPart)part))
            return (DsiPart)part;
    }
    return DSI_PARTS;
}

// The slot number in a DSiWare file's extension: ".pu3" or ".sav3" is
// slot 3, ".pub" or ".sav" slot 0. *sav says the file is a .sav.
static int dsi_slot(const char *path, bool *sav)
{
    const char *dot = strrchr(path, '.');
    size_t n = dot ? strlen(dot) : 0;
    *sav = dot && strncasecmp(dot, ".sav", 4) == 0 && (n == 4 || (n == 5 && dot[4] >= '0' && dot[4] <= '9'));
    char last = n ? dot[n - 1] : 0;
    return (n == (*sav ? 5u : 4u) && last >= '1' && last <= '9') ? last - '0' : 0;
}

bool library_dsi_part_path(const Game *game, const char *main_path, DsiPart part, char *out, size_t size)
{
    if (!game->info.dsiware || part >= DSI_PARTS || !rom_dsi_has_part(&game->info, part))
        return false;
    bool sav;
    int slot = dsi_slot(main_path, &sav);
    const char *dot = strrchr(main_path, '.');
    if (!dot)
        return false;
    if (sav && part != DSI_BANNER)
    {
        // The SAV layout's one file holds the main part only.
        if (part != library_dsi_main_part(game))
            return false;
        int n = snprintf(out, size, "%s", main_path);
        return n >= 0 && (size_t)n < size;
    }
    static const char *const ext[DSI_PARTS] = { "pub", "prv", "bnr" };
    int len = (int)(dot - main_path);
    int n = slot ? snprintf(out, size, "%.*s.%.2s%d", len, main_path, ext[part], slot)
                 : snprintf(out, size, "%.*s.%s", len, main_path, ext[part]);
    return n >= 0 && (size_t)n < size;
}

// The layout kernel id keeps this DSiWare title's save in.
static DsiLayout dsi_layout(const Library *lib, const Game *game, KernelId id, int slot)
{
    if (!kernel_runs_dsiware(id))
        return DSI_LAYOUT_NONE;
    if (id != KERNEL_TWILIGHT)
        return DSI_LAYOUT_SIDECAR;
    // TWiLight picks the layout at launch from the console and settings;
    // the files it made show which one it uses here. (The banner save is
    // the same file in both.)
    const KernelSet *ks = &lib->kernels;
    char path[DSAVE_PATH_MAX];
    for (int part = 0; part < DSI_BANNER; part++)
    {
        if (rom_dsi_has_part(&game->info, (DsiPart)part) &&
            kernel_dsiware_path(ks, id, game->rom_path, slot, DSI_LAYOUT_SIDECAR, (DsiPart)part, path,
                                sizeof(path)) &&
            file_exists(path))
            return DSI_LAYOUT_SIDECAR;
    }
    if (kernel_save_path(ks, id, game->rom_path, slot, path, sizeof(path)) && file_exists(path))
        return DSI_LAYOUT_SAV;
    return lib->dsi_mode ? DSI_LAYOUT_SIDECAR : DSI_LAYOUT_SAV;
}

bool library_kernel_path(const Library *lib, const Game *game, KernelId id, int slot, char *out,
                         size_t size)
{
    if (!game->info.dsiware)
        return kernel_save_path(&lib->kernels, id, game->rom_path, slot, out, size);
    DsiPart part = library_dsi_main_part(game);
    return part < DSI_PARTS && kernel_dsiware_path(&lib->kernels, id, game->rom_path, slot,
                                                   dsi_layout(lib, game, id, slot), part, out, size);
}

static void stat_loc(const Game *game, SaveLoc *loc)
{
    loc->exists = library_stat_save(game, loc->path, &loc->size, &loc->mtime);
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
    if (game->is_card)
        return;
    const KernelSet *ks = &lib->kernels;
    game->loc_count = 0;

    for (int k = 0; k < KERNEL_COUNT; k++)
    {
        if (!ks->cfg[k].installed)
            continue;
        char path[DSAVE_PATH_MAX];
        int slot = kernel_current_slot(ks, (KernelId)k, game->rom_path);
        if (!library_kernel_path(lib, game, (KernelId)k, slot, path, sizeof(path)))
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
            stat_loc(game, loc);
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
    lib->keep_auto = ini_get_int(settings, "dsave", "keep_auto", LIBRARY_KEEP_AUTO_DEFAULT);
    if (lib->keep_auto < 0)
        lib->keep_auto = 0;

    scan_dir(lib, root, 0);
    qsort(lib->games, (size_t)lib->count, sizeof(Game), compare_games);
    return lib->count;
}

int library_rom_count(const Library *lib)
{
    return lib->count && lib->games[0].is_card ? lib->count - 1 : lib->count;
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

bool library_set_keep_auto(Library *lib, int keep)
{
    char settings[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    kernels_settings_path(lib->root, settings, sizeof(settings));
    path_dirname(dir, sizeof(dir), settings);
    lib->keep_auto = keep < 0 ? 0 : keep;
    return mkdir_p(dir) && ini_set_int(settings, "dsave", "keep_auto", lib->keep_auto);
}
