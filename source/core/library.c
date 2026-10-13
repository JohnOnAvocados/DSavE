#include "library.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Deep enough for "roms/nds/genre/game.nds", shallow enough to stay quick on
// a large card.
#define SCAN_MAX_DEPTH 4

static bool path_in_root(char *out, size_t size, const char *root, const char *rel)
{
    return path_join(out, size, root, rel);
}

static bool root_has(const char *root, const char *rel)
{
    char p[DSAVE_PATH_MAX];
    return path_in_root(p, sizeof(p), root, rel) && dir_exists(p);
}

LoaderKind library_detect_loader(const char *root)
{
    if (root_has(root, "_nds/TWiLightMenu"))
        return LOADER_TWILIGHT;
    if (root_has(root, "__rpg"))
        return LOADER_WOOD;
    if (root_has(root, "TTMenu"))
        return LOADER_YSMENU;
    if (root_has(root, "__aio"))
        return LOADER_AKAIO;
    return LOADER_GENERIC;
}

const char *library_loader_name(LoaderKind kind)
{
    switch (kind)
    {
        case LOADER_TWILIGHT: return "TWiLight Menu++";
        case LOADER_WOOD:     return "Wood R4";
        case LOADER_YSMENU:   return "YSMenu";
        case LOADER_AKAIO:    return "AKAIO";
        default:              return "Generic flashcart";
    }
}

bool library_resolve_save(LoaderKind loader, const char *rom_path,
                          char *out, size_t out_size)
{
    char dir[DSAVE_PATH_MAX], base[DSAVE_PATH_MAX], name[DSAVE_PATH_MAX];
    path_dirname(dir, sizeof(dir), rom_path);
    path_basename(base, sizeof(base), rom_path);
    snprintf(name, sizeof(name), "%s", base);
    path_strip_ext(name);

    char beside[DSAVE_PATH_MAX], in_saves[DSAVE_PATH_MAX], saves_dir[DSAVE_PATH_MAX];
    char file[DSAVE_PATH_MAX];
    snprintf(file, sizeof(file), "%s.sav", name);
    path_join(beside, sizeof(beside), dir, file);
    path_join(saves_dir, sizeof(saves_dir), dir, "saves");
    path_join(in_saves, sizeof(in_saves), saves_dir, file);

    // Some kernels keep the full ROM name: "Game.nds.sav".
    char nds_sav[DSAVE_PATH_MAX];
    snprintf(nds_sav, sizeof(nds_sav), "%s.sav", rom_path);

    const char *order[3];
    if (loader == LOADER_TWILIGHT)
    {
        order[0] = in_saves;
        order[1] = beside;
    }
    else
    {
        order[0] = beside;
        order[1] = in_saves;
    }
    order[2] = nds_sav;

    for (int i = 0; i < 3; i++)
    {
        if (file_exists(order[i]))
        {
            snprintf(out, out_size, "%s", order[i]);
            return true;
        }
    }
    snprintf(out, out_size, "%s", order[0]);
    return false;
}

static bool skip_dir(const char *name)
{
    static const char *const skipped[] = {
        "saves", "TTMenu", "Nintendo 3DS", "System Volume Information",
        "LOST.DIR", "gm9i", "photo", "private", NULL,
    };
    // Covers _nds, _DSavE, __rpg, __aio and hidden folders.
    if (name[0] == '_' || name[0] == '.')
        return true;
    for (int i = 0; skipped[i]; i++)
    {
        if (strcasecmp(name, skipped[i]) == 0)
            return true;
    }
    return false;
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
    snprintf(lib->root, sizeof(lib->root), "%s", root);
    lib->count = 0;
    lib->truncated = false;
    lib->loader = library_detect_loader(root);

    scan_dir(lib, root, 0);
    qsort(lib->games, (size_t)lib->count, sizeof(Game), compare_games);
    return lib->count;
}

void library_refresh_game(const Library *lib, Game *game)
{
    game->has_save = library_resolve_save(lib->loader, game->rom_path,
                                          game->save_path, sizeof(game->save_path));
    game->save_size = game->has_save ? file_size(game->save_path) : -1;
}
