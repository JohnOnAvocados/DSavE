#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ini.h"

typedef struct {
    const char *name;
    const char *key;
    const char *marker; // Folder whose presence means the kernel is installed
    bool hw_tested;
} KernelInfo;

// Order matters: it is the default preference when several are installed.
static const KernelInfo kernels[KERNEL_COUNT] = {
    [KERNEL_PICO]       = { "Pico Launcher",   "pico",       "_pico",             false },
    [KERNEL_TWILIGHT]   = { "TWiLight Menu++", "twilight",   "_nds/TWiLightMenu", false },
    [KERNEL_AKMENUNEXT] = { "akmenu-next",     "akmenunext", "_nds/akmenunext",   false },
    [KERNEL_WOOD]       = { "Wood R4",         "wood",       "__rpg",             false },
    [KERNEL_YSMENU]     = { "YSMenu",          "ysmenu",     "TTMenu",            false },
    [KERNEL_AKAIO]      = { "AKAIO",           "akaio",      "__aio",             false },
    [KERNEL_GENERIC]    = { "Generic",         "generic",    NULL,                false },
};

#define SETTINGS_SECTION "dsave"
#define SETTINGS_KEY_KERNEL "kernel"

const char *kernel_name(KernelId id)
{
    return id < KERNEL_COUNT ? kernels[id].name : "?";
}

const char *kernel_key(KernelId id)
{
    return id < KERNEL_COUNT ? kernels[id].key : "?";
}

bool kernel_from_key(const char *key, KernelId *out)
{
    for (int i = 0; i < KERNEL_COUNT; i++)
    {
        if (strcasecmp(key, kernels[i].key) == 0)
        {
            *out = (KernelId)i;
            return true;
        }
    }
    return false;
}

bool kernel_hw_tested(KernelId id)
{
    return id < KERNEL_COUNT && kernels[id].hw_tested;
}

void kernels_settings_path(const char *root, char *out, size_t size)
{
    path_join(out, size, root, "_DSavE/settings.ini");
}

// ---------------------------------------------------------------------------
// Detection and settings

static bool root_dir_exists(const char *root, const char *rel)
{
    char p[DSAVE_PATH_MAX];
    return path_join(p, sizeof(p), root, rel) && dir_exists(p);
}

static void read_akmenu_settings(KernelConfig *cfg, const char *root, const char *dir)
{
    char path[DSAVE_PATH_MAX], rel[64], val[32];
    snprintf(rel, sizeof(rel), "%s/globalsettings.ini", dir);
    if (!path_join(path, sizeof(path), root, rel))
        return;
    // akmenu writes ".sav" or ".nds.sav"; older builds wrote 1/0.
    if (ini_get(path, "system", "saveext", val, sizeof(val)))
        cfg->nds_sav_ext = strcasecmp(val, ".nds.sav") == 0 || strcmp(val, "0") == 0;
    cfg->saves_dir = ini_get_int(path, "system", "savedir", 0) != 0;
}

static void load_savelist(KernelSet *ks)
{
    char path[DSAVE_PATH_MAX];
    if (!path_join(path, sizeof(path), ks->root, "_pico/savelist.bin"))
        return;
    long size = file_size(path);
    if (size < (long)sizeof(SaveListEntry) || size > 1024 * 1024)
        return;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    int count = (int)(size / (long)sizeof(SaveListEntry));
    ks->savelist = malloc((size_t)count * sizeof(SaveListEntry));
    if (ks->savelist && fread(ks->savelist, sizeof(SaveListEntry), (size_t)count, f) == (size_t)count)
        ks->savelist_count = count;
    else
    {
        free(ks->savelist);
        ks->savelist = NULL;
    }
    fclose(f);
}

void kernels_detect(KernelSet *ks, const char *root)
{
    memset(ks, 0, sizeof(*ks));
    snprintf(ks->root, sizeof(ks->root), "%s", root);

    for (int i = 0; i < KERNEL_COUNT; i++)
    {
        if (kernels[i].marker && root_dir_exists(root, kernels[i].marker))
        {
            ks->cfg[i].installed = true;
            ks->installed |= KERNEL_BIT(i);
        }
    }
    if (!ks->installed)
    {
        ks->cfg[KERNEL_GENERIC].installed = true;
        ks->installed = KERNEL_BIT(KERNEL_GENERIC);
    }

    if (ks->cfg[KERNEL_TWILIGHT].installed)
    {
        char path[DSAVE_PATH_MAX];
        path_join(path, sizeof(path), root, "_nds/TWiLightMenu/settings.ini");
        int loc = ini_get_int(path, "SRLOADER", "SAVE_LOCATION", TWL_SAVES_FOLDER);
        ks->cfg[KERNEL_TWILIGHT].twl_location =
            (loc >= TWL_SAVES_FOLDER && loc <= TWL_TWLM_FOLDER) ? (TwlSaveLocation)loc : TWL_SAVES_FOLDER;
    }
    if (ks->cfg[KERNEL_AKMENUNEXT].installed)
        read_akmenu_settings(&ks->cfg[KERNEL_AKMENUNEXT], root, "_nds/akmenunext");
    if (ks->cfg[KERNEL_WOOD].installed)
        read_akmenu_settings(&ks->cfg[KERNEL_WOOD], root, "__rpg");
    if (ks->cfg[KERNEL_AKAIO].installed)
        read_akmenu_settings(&ks->cfg[KERNEL_AKAIO], root, "__aio");
    // Only akmenu-next has the saves/ folder option.
    ks->cfg[KERNEL_WOOD].saves_dir = false;
    ks->cfg[KERNEL_AKAIO].saves_dir = false;

    if (ks->cfg[KERNEL_PICO].installed)
        load_savelist(ks);

    // Active kernel: remembered choice, else the first installed.
    ks->active = KERNEL_GENERIC;
    for (int i = KERNEL_COUNT - 1; i >= 0; i--)
    {
        if (ks->cfg[i].installed)
            ks->active = (KernelId)i;
    }
    char settings[DSAVE_PATH_MAX], key[32];
    kernels_settings_path(root, settings, sizeof(settings));
    KernelId saved;
    if (ini_get(settings, SETTINGS_SECTION, SETTINGS_KEY_KERNEL, key, sizeof(key)) &&
        kernel_from_key(key, &saved) && ks->cfg[saved].installed)
        ks->active = saved;
}

void kernels_free(KernelSet *ks)
{
    free(ks->savelist);
    ks->savelist = NULL;
    ks->savelist_count = 0;
}

bool kernels_set_active(KernelSet *ks, KernelId id)
{
    if (id >= KERNEL_COUNT || !ks->cfg[id].installed)
        return false;
    ks->active = id;
    char settings[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    kernels_settings_path(ks->root, settings, sizeof(settings));
    path_dirname(dir, sizeof(dir), settings);
    return mkdir_p(dir) && ini_set(settings, SETTINGS_SECTION, SETTINGS_KEY_KERNEL, kernel_key(id));
}

KernelId kernels_cycle(const KernelSet *ks, int direction)
{
    int id = (int)ks->active;
    for (int n = 0; n < KERNEL_COUNT; n++)
    {
        id = (id + (direction < 0 ? KERNEL_COUNT - 1 : 1)) % KERNEL_COUNT;
        if (ks->cfg[id].installed)
            return (KernelId)id;
    }
    return ks->active;
}

// ---------------------------------------------------------------------------
// Save paths

// Split a ROM path into its folder, file name and file name without the
// last extension.
static void split_rom(const char *rom_path, char *dir, char *file, char *stem)
{
    path_dirname(dir, DSAVE_PATH_MAX, rom_path);
    path_basename(file, DSAVE_PATH_MAX, rom_path);
    snprintf(stem, DSAVE_PATH_MAX, "%s", file);
    path_strip_ext(stem);
}

bool kernel_twl_gamesettings_path(const KernelSet *ks, const char *rom_path,
                                  char *out, size_t size)
{
    char dir[DSAVE_PATH_MAX], file[DSAVE_PATH_MAX], stem[DSAVE_PATH_MAX], rel[DSAVE_PATH_MAX];
    split_rom(rom_path, dir, file, stem);
    snprintf(rel, sizeof(rel), "_nds/TWiLightMenu/gamesettings/%s.ini", file);
    return path_join(out, size, ks->root, rel);
}

int kernel_twl_save_number(const KernelSet *ks, const char *rom_path)
{
    char path[DSAVE_PATH_MAX];
    if (!kernel_twl_gamesettings_path(ks, rom_path, path, sizeof(path)))
        return 0;
    int n = ini_get_int(path, "GAMESETTINGS", "SAVE_NUMBER", 0);
    return (n >= 0 && n <= 9) ? n : 0;
}

int kernel_max_slot(KernelId id)
{
    return (id == KERNEL_TWILIGHT || id == KERNEL_AKMENUNEXT) ? 9 : 0;
}

int kernel_current_slot(const KernelSet *ks, KernelId id, const char *rom_path)
{
    if (id == KERNEL_TWILIGHT && ks->cfg[id].twl_location != TWL_GAMES_FOLDER)
        return kernel_twl_save_number(ks, rom_path);
    // akmenu-next keeps its per-game slot in a binary gamedata file that is
    // not documented; DSavE treats slot 0 as current.
    return 0;
}

bool kernel_save_path(const KernelSet *ks, KernelId id, const char *rom_path,
                      int slot, char *out, size_t size)
{
    if (id >= KERNEL_COUNT || slot < 0 || slot > kernel_max_slot(id))
        return false;

    char dir[DSAVE_PATH_MAX], file[DSAVE_PATH_MAX], stem[DSAVE_PATH_MAX];
    char folder[DSAVE_PATH_MAX], name[DSAVE_PATH_MAX];
    split_rom(rom_path, dir, file, stem);
    const KernelConfig *cfg = &ks->cfg[id];

    switch (id)
    {
        case KERNEL_TWILIGHT:
            // Slots only apply when saves are not kept beside the ROM.
            if (cfg->twl_location == TWL_GAMES_FOLDER)
            {
                if (slot != 0)
                    return false;
                snprintf(folder, sizeof(folder), "%s", dir);
            }
            else if (cfg->twl_location == TWL_TWLM_FOLDER)
                path_join(folder, sizeof(folder), ks->root, "_nds/TWiLightMenu/saves");
            else
                path_join(folder, sizeof(folder), dir, "saves");
            if (slot)
                snprintf(name, sizeof(name), "%s.sav%d", stem, slot);
            else
                snprintf(name, sizeof(name), "%s.sav", stem);
            break;

        case KERNEL_AKMENUNEXT:
        case KERNEL_WOOD:
        case KERNEL_AKAIO:
        {
            // akmenu's generateSaveName: drop a 3-letter extension unless the
            // ".nds.sav" option is on, then add ".<slot>" and ".sav".
            char base[DSAVE_PATH_MAX];
            snprintf(base, sizeof(base), "%s", file);
            size_t n = strlen(base);
            if (!cfg->nds_sav_ext && n > 3 && base[n - 4] == '.')
                base[n - 4] = '\0';
            if (slot)
                snprintf(name, sizeof(name), "%s.%d.sav", base, slot);
            else
                snprintf(name, sizeof(name), "%s.sav", base);
            if (cfg->saves_dir)
                path_join(folder, sizeof(folder), dir, "saves");
            else
                snprintf(folder, sizeof(folder), "%s", dir);
            break;
        }

        case KERNEL_PICO:
        case KERNEL_YSMENU:
        case KERNEL_GENERIC:
        default:
            // Pico Loader replaces the ROM's extension with ".sav".
            snprintf(name, sizeof(name), "%s.sav", stem);
            snprintf(folder, sizeof(folder), "%s", dir);
            break;
    }
    return path_join(out, size, folder, name);
}

bool kernel_runs_dsiware(KernelId id)
{
    return id == KERNEL_PICO || id == KERNEL_TWILIGHT || id == KERNEL_GENERIC;
}

bool kernel_dsiware_path(const KernelSet *ks, KernelId id, const char *rom_path, int slot,
                         DsiLayout layout, DsiPart part, char *out, size_t size)
{
    if (!kernel_runs_dsiware(id) || layout == DSI_LAYOUT_NONE || part >= DSI_PARTS ||
        (layout == DSI_LAYOUT_SAV && id != KERNEL_TWILIGHT))
        return false;
    // Same folder and slot rules as the kernel's .sav, which these kernels
    // all name "<rom name>.sav" or "<rom name>.savN".
    char path[DSAVE_PATH_MAX];
    if (!kernel_save_path(ks, id, rom_path, slot, path, sizeof(path)))
        return false;
    if (layout == DSI_LAYOUT_SIDECAR || part == DSI_BANNER)
    {
        // TWiLight's getPubExtension and friends; Pico Loader's
        // DsiWareSaveArranger swaps the ROM's extension the same way.
        static const char *const ext[DSI_PARTS] = { "pub", "prv", "bnr" };
        char *dot = strrchr(path, '.');
        size_t room = sizeof(path) - (size_t)(dot - path);
        int n = slot ? snprintf(dot, room, ".%.2s%d", ext[part], slot) : snprintf(dot, room, ".%s", ext[part]);
        if (n < 0 || (size_t)n >= room)
            return false;
    }
    int n = snprintf(out, size, "%s", path);
    return n >= 0 && (size_t)n < size;
}

long kernel_chip_size(const KernelSet *ks, const char *game_code)
{
    if (!ks->savelist || strlen(game_code) != 4)
        return -1;
    const uint8_t *c = (const uint8_t *)game_code;
    uint32_t code = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) |
                    ((uint32_t)c[3] << 24);
    for (int i = 0; i < ks->savelist_count; i++)
    {
        const SaveListEntry *e = &ks->savelist[i];
        if (e->game_code == code)
            return e->log2_size ? (1L << e->log2_size) : 0;
    }
    return -1;
}
