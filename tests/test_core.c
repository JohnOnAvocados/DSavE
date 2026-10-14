// Host-side tests for the save engine. Each test builds a fake SD card in a
// temp folder, laid out the way a kernel would leave it, and drives the same
// code the DS runs.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>

#include "../source/core/activity.h"
#include "../source/core/backup.h"
#include "../source/core/cardsave.h"
#include "../source/core/ini.h"
#include "../source/core/kernel.h"
#include "../source/core/library.h"
#include "../source/core/rom.h"
#include "../source/core/savefile.h"
#include "../source/core/sync.h"
#include "../source/core/util.h"

#include "fake_card.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define KB 1024L

static char base[DSAVE_PATH_MAX];
static char root[DSAVE_PATH_MAX];
static Library lib;
static int card_no = 0;

// ---------------------------------------------------------------------------
// Fake SD card helpers

static void new_card(void)
{
    snprintf(root, sizeof(root), "%s/card%d", base, ++card_no);
    mkdir_p(root);
}

static void p(char *out, const char *rel)
{
    path_join(out, DSAVE_PATH_MAX, root, rel);
}

static void make_dir(const char *rel)
{
    char path[DSAVE_PATH_MAX];
    p(path, rel);
    mkdir_p(path);
}

static void make_text(const char *rel, const char *text)
{
    char path[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    p(path, rel);
    path_dirname(dir, sizeof(dir), path);
    mkdir_p(dir);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

// Write a minimal .nds: header with title and code, and a banner whose
// English title is banner_title (NULL for no banner).
static void make_rom(const char *rel, const char *title, const char *code,
                     const char *banner_title)
{
    char path[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    p(path, rel);
    path_dirname(dir, sizeof(dir), path);
    mkdir_p(dir);

    static uint8_t rom[0x200 + 0xA00];
    memset(rom, 0, sizeof(rom));
    memcpy(rom, title, strlen(title) > 12 ? 12 : strlen(title));
    memcpy(rom + 0x0C, code, 4);
    size_t len = 0x200;
    if (banner_title)
    {
        uint32_t banner = 0x200;
        rom[0x68] = banner & 0xFF;
        rom[0x69] = (banner >> 8) & 0xFF;
        uint8_t *en = rom + banner + 0x240 + 0x100;
        for (size_t i = 0; banner_title[i]; i++)
            en[i * 2] = (uint8_t)banner_title[i];
        len = sizeof(rom);
    }
    FILE *f = fopen(path, "wb");
    fwrite(rom, 1, len, f);
    fclose(f);
}

// A save of `size` bytes: `data` bytes of a pattern, then 0xFF padding.
static void make_save(const char *rel, long size, long data, uint8_t seed)
{
    char path[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    p(path, rel);
    path_dirname(dir, sizeof(dir), path);
    mkdir_p(dir);
    FILE *f = fopen(path, "wb");
    for (long i = 0; i < size; i++)
        fputc(i < data ? (uint8_t)(seed + i * 7 + (i >> 8)) : 0xFF, f);
    fclose(f);
}

static void make_file(const char *rel, long size, uint8_t seed)
{
    make_save(rel, size, size, seed);
}

static void make_blank(const char *rel, long size, uint8_t value)
{
    char path[DSAVE_PATH_MAX];
    p(path, rel);
    FILE *f = fopen(path, "wb");
    for (long i = 0; i < size; i++)
        fputc(value, f);
    fclose(f);
}

// Pico's savelist.bin: 8-byte entries {u32 code, u8 type, u8 log2 size, 2 pad}.
static void make_savelist(const char *const codes[], const uint8_t log2_sizes[], int n)
{
    char path[DSAVE_PATH_MAX];
    p(path, "_pico/savelist.bin");
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < n; i++)
    {
        uint8_t e[8] = { (uint8_t)codes[i][0], (uint8_t)codes[i][1], (uint8_t)codes[i][2],
                         (uint8_t)codes[i][3], 2, log2_sizes[i], 0, 0 };
        fwrite(e, 1, 8, f);
    }
    fclose(f);
}

static Game *find_game(const char *code)
{
    for (int i = 0; i < lib.count; i++)
    {
        if (strcmp(lib.games[i].info.game_code, code) == 0)
            return &lib.games[i];
    }
    return NULL;
}

static uint32_t crc_of(const char *path)
{
    uint32_t c = 0;
    file_crc32(path, &c, NULL);
    return c;
}

static uint32_t crc_of_rel(const char *rel)
{
    char path[DSAVE_PATH_MAX];
    p(path, rel);
    return crc_of(path);
}

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void put_le32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(v >> (8 * i));
}

static void write_bytes(const char *rel, const uint8_t *data, long n)
{
    char path[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    p(path, rel);
    path_dirname(dir, sizeof(dir), path);
    mkdir_p(dir);
    FILE *f = fopen(path, "wb");
    fwrite(data, 1, (size_t)n, f);
    fclose(f);
}

// CRC of the first n bytes of path.
static uint32_t crc_prefix(const char *path, long n)
{
    FILE *f = fopen(path, "rb");
    uint32_t c = 0;
    uint8_t buf[4096];
    while (f && n > 0)
    {
        size_t want = (size_t)(n < 4096 ? n : 4096);
        size_t got = fread(buf, 1, want, f);
        if (!got)
            break;
        c = crc32_update(c, buf, got);
        n -= (long)got;
    }
    if (f)
        fclose(f);
    return c;
}

static bool path_is(const char *got, const char *rel)
{
    char expect[DSAVE_PATH_MAX];
    p(expect, rel);
    if (strcmp(got, expect) != 0)
    {
        fprintf(stderr, "  path: got %s\n        want %s\n", got, expect);
        return false;
    }
    return true;
}

static void set_mtime(const char *rel, time_t t)
{
    char path[DSAVE_PATH_MAX], cmd[DSAVE_PATH_MAX + 64];
    p(path, rel);
    struct tm *tm = gmtime(&t);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d%H%M.%S", tm);
    snprintf(cmd, sizeof(cmd), "TZ=UTC touch -t %s '%s'", stamp, path);
    CHECK(system(cmd) == 0);
}

// ---------------------------------------------------------------------------
// Tests

static void test_util(void)
{
    char out[64];
    sanitize_name(out, sizeof(out), "  Pok\xe9mon: Soul/Silver?  ", 40);
    CHECK(strcmp(out, "Pok_mon_ Soul_Silver_") == 0);
    sanitize_name(out, sizeof(out), "Pok\xc3\xa9mon \xc3\x9c \xe3\x81\x82!", 40);
    CHECK(strcmp(out, "Pokemon U _!") == 0);
    sanitize_name(out, sizeof(out), "...", 40);
    CHECK(strcmp(out, "unnamed") == 0);
    sanitize_name(out, sizeof(out), "abcdefghij", 4);
    CHECK(strcmp(out, "abcd") == 0);

    path_dirname(out, sizeof(out), "sd:/game.nds");
    CHECK(strcmp(out, "sd:/") == 0);
    path_dirname(out, sizeof(out), "sd:/roms/game.nds");
    CHECK(strcmp(out, "sd:/roms") == 0);

    CHECK(is_valid_save_size(512));
    CHECK(is_valid_save_size(64 * KB));
    CHECK(is_valid_save_size(8 * KB * KB));
    CHECK(!is_valid_save_size(500));
    CHECK(!is_valid_save_size(520 * KB));
    CHECK(!is_valid_save_size(16 * KB * KB));

    CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u);
}

static void test_ini(void)
{
    new_card();
    char path[DSAVE_PATH_MAX], val[32];
    p(path, "test.ini");
    make_text("test.ini", "; comment\n[SRLOADER]\nTHEME = 1\r\nSAVE_LOCATION=2\n\n[OTHER]\nX = y\n");

    CHECK(ini_get_int(path, "srloader", "save_location", -1) == 2);
    CHECK(ini_get_int(path, "SRLOADER", "THEME", -1) == 1);
    CHECK(ini_get_int(path, "SRLOADER", "MISSING", 7) == 7);
    CHECK(ini_get(path, "OTHER", "X", val, sizeof(val)) && strcmp(val, "y") == 0);

    // Change one key, add one to an earlier section, add a new section.
    CHECK(ini_set(path, "SRLOADER", "SAVE_LOCATION", "0"));
    CHECK(ini_set_int(path, "SRLOADER", "NEW_KEY", 5));
    CHECK(ini_set(path, "GAMESETTINGS", "SAVE_NUMBER", "3"));
    CHECK(ini_get_int(path, "SRLOADER", "SAVE_LOCATION", -1) == 0);
    CHECK(ini_get_int(path, "SRLOADER", "NEW_KEY", -1) == 5);
    CHECK(ini_get_int(path, "SRLOADER", "THEME", -1) == 1);
    CHECK(ini_get_int(path, "GAMESETTINGS", "SAVE_NUMBER", -1) == 3);
    CHECK(ini_get(path, "OTHER", "X", val, sizeof(val)) && strcmp(val, "y") == 0);

    // Comments survive a rewrite.
    FILE *f = fopen(path, "r");
    char line[128];
    CHECK(f && fgets(line, sizeof(line), f) && strncmp(line, "; comment", 9) == 0);
    if (f)
        fclose(f);

    // A new file is created.
    p(path, "new.ini");
    CHECK(ini_set(path, "dsave", "kernel", "pico"));
    CHECK(ini_get(path, "dsave", "kernel", val, sizeof(val)) && strcmp(val, "pico") == 0);
}

static void test_kernel_detection(void)
{
    KernelSet ks;

    new_card();
    kernels_detect(&ks, root);
    CHECK(ks.installed == KERNEL_BIT(KERNEL_GENERIC));
    CHECK(ks.active == KERNEL_GENERIC);
    kernels_free(&ks);

    // A DSpico card with TWiLight added: Pico is preferred by default.
    new_card();
    make_dir("_pico");
    make_dir("_nds/TWiLightMenu");
    kernels_detect(&ks, root);
    CHECK(ks.installed == (KERNEL_BIT(KERNEL_PICO) | KERNEL_BIT(KERNEL_TWILIGHT)));
    CHECK(ks.active == KERNEL_PICO);

    // The choice is remembered.
    CHECK(kernels_set_active(&ks, KERNEL_TWILIGHT));
    CHECK(!kernels_set_active(&ks, KERNEL_WOOD)); // not installed
    kernels_free(&ks);
    kernels_detect(&ks, root);
    CHECK(ks.active == KERNEL_TWILIGHT);
    CHECK(kernels_cycle(&ks, 1) == KERNEL_PICO);
    CHECK(kernels_cycle(&ks, -1) == KERNEL_PICO);
    kernels_free(&ks);

    // An Ace3DS X card: YSMenu, akmenu-next and TWiLight.
    new_card();
    make_dir("TTMenu");
    make_dir("_nds/akmenunext");
    make_dir("_nds/TWiLightMenu");
    kernels_detect(&ks, root);
    CHECK(ks.installed == (KERNEL_BIT(KERNEL_YSMENU) | KERNEL_BIT(KERNEL_AKMENUNEXT) |
                           KERNEL_BIT(KERNEL_TWILIGHT)));
    CHECK(ks.active == KERNEL_TWILIGHT);
    kernels_free(&ks);

    new_card();
    make_dir("__rpg");
    make_dir("__aio");
    kernels_detect(&ks, root);
    CHECK(ks.installed == (KERNEL_BIT(KERNEL_WOOD) | KERNEL_BIT(KERNEL_AKAIO)));
    KernelId k;
    CHECK(kernel_from_key("wood", &k) && k == KERNEL_WOOD);
    CHECK(!kernel_from_key("nope", &k));
    kernels_free(&ks);
}

static void test_kernel_paths(void)
{
    KernelSet ks;
    char out[DSAVE_PATH_MAX], rom[DSAVE_PATH_MAX];

    new_card();
    make_dir("_pico");
    make_dir("__rpg");
    make_dir("TTMenu");
    make_dir("__aio");
    make_text("_nds/TWiLightMenu/settings.ini", "[SRLOADER]\nSAVE_LOCATION = 0\n");
    make_text("_nds/akmenunext/globalsettings.ini", "[system]\nsaveext = .sav\nsavedir = 0\n");
    p(rom, "roms/Mario Kart DS.nds");
    kernels_detect(&ks, root);

    // Pico Loader: ROM extension replaced by .sav, beside the ROM.
    CHECK(kernel_save_path(&ks, KERNEL_PICO, rom, 0, out, sizeof(out)));
    CHECK(path_is(out, "roms/Mario Kart DS.sav"));
    CHECK(!kernel_save_path(&ks, KERNEL_PICO, rom, 1, out, sizeof(out)));

    // TWiLight: saves/ folder, slots as .savN.
    CHECK(kernel_save_path(&ks, KERNEL_TWILIGHT, rom, 0, out, sizeof(out)));
    CHECK(path_is(out, "roms/saves/Mario Kart DS.sav"));
    CHECK(kernel_save_path(&ks, KERNEL_TWILIGHT, rom, 3, out, sizeof(out)));
    CHECK(path_is(out, "roms/saves/Mario Kart DS.sav3"));
    CHECK(kernel_current_slot(&ks, KERNEL_TWILIGHT, rom) == 0);
    make_text("_nds/TWiLightMenu/gamesettings/Mario Kart DS.nds.ini",
              "[GAMESETTINGS]\nSAVE_NUMBER = 2\n");
    CHECK(kernel_current_slot(&ks, KERNEL_TWILIGHT, rom) == 2);

    // akmenu-next: .sav beside the ROM, slots as .N.sav.
    CHECK(kernel_save_path(&ks, KERNEL_AKMENUNEXT, rom, 0, out, sizeof(out)));
    CHECK(path_is(out, "roms/Mario Kart DS.sav"));
    CHECK(kernel_save_path(&ks, KERNEL_AKMENUNEXT, rom, 1, out, sizeof(out)));
    CHECK(path_is(out, "roms/Mario Kart DS.1.sav"));

    // Wood, YSMenu, AKAIO defaults.
    CHECK(kernel_save_path(&ks, KERNEL_WOOD, rom, 0, out, sizeof(out)));
    CHECK(path_is(out, "roms/Mario Kart DS.sav"));
    CHECK(kernel_save_path(&ks, KERNEL_YSMENU, rom, 0, out, sizeof(out)));
    CHECK(path_is(out, "roms/Mario Kart DS.sav"));
    CHECK(kernel_save_path(&ks, KERNEL_AKAIO, rom, 0, out, sizeof(out)));
    CHECK(path_is(out, "roms/Mario Kart DS.sav"));
    kernels_free(&ks);

    // Settings change the rules.
    make_text("_nds/TWiLightMenu/settings.ini", "[SRLOADER]\nSAVE_LOCATION = 2\n");
    make_text("_nds/akmenunext/globalsettings.ini", "[system]\nsaveext = .nds.sav\nsavedir = 1\n");
    make_text("__rpg/globalsettings.ini", "[system]\nsaveext = .nds.sav\n");
    kernels_detect(&ks, root);
    CHECK(kernel_save_path(&ks, KERNEL_TWILIGHT, rom, 2, out, sizeof(out)));
    CHECK(path_is(out, "_nds/TWiLightMenu/saves/Mario Kart DS.sav2"));
    CHECK(kernel_save_path(&ks, KERNEL_AKMENUNEXT, rom, 0, out, sizeof(out)));
    CHECK(path_is(out, "roms/saves/Mario Kart DS.nds.sav"));
    CHECK(kernel_save_path(&ks, KERNEL_WOOD, rom, 0, out, sizeof(out)));
    CHECK(path_is(out, "roms/Mario Kart DS.nds.sav"));
    kernels_free(&ks);

    // TWiLight "games folder" mode ignores the save number.
    make_text("_nds/TWiLightMenu/settings.ini", "[SRLOADER]\nSAVE_LOCATION = 1\n");
    kernels_detect(&ks, root);
    CHECK(kernel_current_slot(&ks, KERNEL_TWILIGHT, rom) == 0);
    CHECK(kernel_save_path(&ks, KERNEL_TWILIGHT, rom, 0, out, sizeof(out)));
    CHECK(path_is(out, "roms/Mario Kart DS.sav"));
    CHECK(!kernel_save_path(&ks, KERNEL_TWILIGHT, rom, 2, out, sizeof(out)));
    kernels_free(&ks);
}

static void test_savelist(void)
{
    new_card();
    make_dir("_pico");
    static const char *const codes[] = { "AMCE", "YQUE", "IPGE" };
    static const uint8_t sizes[] = { 19, 16, 0 };
    make_savelist(codes, sizes, 3);
    KernelSet ks;
    kernels_detect(&ks, root);
    CHECK(kernel_chip_size(&ks, "AMCE") == 512 * KB);
    CHECK(kernel_chip_size(&ks, "YQUE") == 64 * KB);
    CHECK(kernel_chip_size(&ks, "IPGE") == 0);
    CHECK(kernel_chip_size(&ks, "ZZZZ") == -1);
    kernels_free(&ks);
}

static void test_scan(void)
{
    // A Wood R4 card: saves sit beside ROMs.
    new_card();
    make_dir("__rpg");
    make_rom("Chrono Trigger.nds", "CHRONOTRIGGE", "YQUE", "CHRONO TRIGGER");
    make_file("Chrono Trigger.sav", 64 * KB, 1);
    make_rom("roms/rpg/Castlevania_DoS.nds", "CASTLEVANIA1", "ACVE", NULL);
    make_file("roms/rpg/Castlevania_DoS.sav", 64 * KB, 2);
    make_rom("roms/Pokemon_SoulSilver.nds", "POKEMON SS", "IPGE", "POKEMON SS");
    make_rom("homebrew/GodMode9i.nds", "GODMODE9I", "####", NULL);
    make_rom("_nds/hidden.nds", "HIDDEN", "HIDE", NULL);

    library_scan(&lib, root);
    CHECK(lib.kernels.active == KERNEL_WOOD);
    CHECK(lib.count == 3); // homebrew and _nds skipped
    CHECK(strcmp(lib.games[0].title, "Castlevania_DoS") == 0); // No banner: file name
    CHECK(strcmp(lib.games[1].title, "CHRONO TRIGGER") == 0);
    CHECK(strcmp(lib.games[2].title, "POKEMON SS") == 0);

    Game *ct = find_game("YQUE");
    CHECK(ct && ct->has_save && ct->save_size == 64 * KB && ct->loc_count == 1);
    Game *pk = find_game("IPGE");
    CHECK(pk && !pk->has_save && pk->save_size == -1);
    CHECK(pk && path_is(pk->save_path, "roms/Pokemon_SoulSilver.sav"));

    // Banner titles keep accents as UTF-8; backup folders fold them.
    new_card();
    make_dir("__rpg");
    make_rom("hg.nds", "POKEMON HG", "IPKE", "POK\xe9MON HeartGold\nNintendo");
    library_scan(&lib, root);
    CHECK(lib.count == 1 && strcmp(lib.games[0].title, "POK\xc3\xa9MON HeartGold") == 0);
    char dir[DSAVE_PATH_MAX];
    CHECK(lib.count == 1 && backup_game_dir(&lib, &lib.games[0], dir, sizeof(dir)) &&
          path_is(dir, "_DSavE/saves/IPKE POKeMON HeartGold"));
}

static void test_multi_kernel_card(void)
{
    // DSpico with Pico Launcher and TWiLight. The same game has a Pico save
    // beside the ROM and a TWiLight save in saves/.
    new_card();
    make_dir("_pico");
    make_dir("_nds/TWiLightMenu");
    make_rom("Mario Kart DS.nds", "MARIOKART DS", "AMCE", NULL);
    make_save("Mario Kart DS.sav", 512 * KB, 32 * KB, 1);
    make_save("saves/Mario Kart DS.sav", 512 * KB, 32 * KB, 2);
    set_mtime("Mario Kart DS.sav", 1000000000);
    set_mtime("saves/Mario Kart DS.sav", 1100000000);

    library_scan(&lib, root);
    Game *mk = find_game("AMCE");
    CHECK(mk && mk->loc_count == 2);
    if (!mk)
        return;
    int pico = library_loc_for_kernel(mk, KERNEL_PICO);
    int twl = library_loc_for_kernel(mk, KERNEL_TWILIGHT);
    CHECK(pico >= 0 && twl >= 0 && pico != twl);

    // Active kernel (Pico) wins even though TWiLight's copy is newer.
    CHECK(mk->primary == pico && !mk->primary_is_fallback);
    CHECK(path_is(mk->save_path, "Mario Kart DS.sav"));

    // Switching kernels switches the primary save.
    library_set_kernel(&lib, KERNEL_TWILIGHT);
    mk = find_game("AMCE");
    CHECK(mk && path_is(mk->save_path, "saves/Mario Kart DS.sav"));

    // If the active kernel has no save yet, fall back to the newest one.
    library_set_kernel(&lib, KERNEL_PICO);
    char path[DSAVE_PATH_MAX];
    p(path, "Mario Kart DS.sav");
    remove(path);
    library_refresh_game(&lib, mk);
    CHECK(mk->primary_is_fallback && path_is(mk->save_path, "saves/Mario Kart DS.sav"));

    // Kernels that share a path share an entry (Pico and akmenu-next).
    make_dir("_nds/akmenunext");
    library_scan(&lib, root);
    mk = find_game("AMCE");
    CHECK(mk && mk->loc_count == 2);
    if (mk)
    {
        CHECK(library_loc_for_kernel(mk, KERNEL_PICO) ==
              library_loc_for_kernel(mk, KERNEL_AKMENUNEXT));
    }
}

static void test_backup_cycle(void)
{
    new_card();
    make_dir("__rpg");
    make_rom("Chrono Trigger.nds", "CHRONOTRIGGE", "YQUE", "CHRONO TRIGGER");
    make_file("Chrono Trigger.sav", 64 * KB, 1);
    library_scan(&lib, root);
    Game *ct = find_game("YQUE");
    CHECK(ct != NULL);
    if (!ct)
        return;
    uint32_t original = crc_of(ct->save_path);

    Backup b1;
    CHECK(backup_create(&lib, ct, "Before final boss", false, &b1) == DSAVE_OK);
    CHECK(strcmp(b1.name, "Before final boss") == 0);
    CHECK(crc_of(b1.file) == original);
    CHECK(path_is(b1.file, "_DSavE/saves/YQUE CHRONO TRIGGER/Before final boss/CHRONO TRIGGER.sav"));

    // Same name again gets a suffix instead of overwriting.
    Backup b2;
    CHECK(backup_create(&lib, ct, "Before final boss", false, &b2) == DSAVE_OK);
    CHECK(strcmp(b2.name, "Before final boss_2") == 0);

    // Timestamped backup.
    Backup b3;
    CHECK(backup_create(&lib, ct, NULL, false, &b3) == DSAVE_OK);
    CHECK(strlen(b3.name) == 19 && b3.name[4] == '-' && b3.name[10] == '_');

    Backup list[BACKUP_LIST_MAX];
    CHECK(backup_list(&lib, ct, list, BACKUP_LIST_MAX) == 3);

    // Change the live save, then restore b1 and check it came back.
    make_file("Chrono Trigger.sav", 64 * KB, 99);
    library_refresh_game(&lib, ct);
    uint32_t changed = crc_of(ct->save_path);
    CHECK(changed != original);

    Backup safety;
    CHECK(backup_restore(&lib, ct, &b1, &safety) == DSAVE_OK);
    CHECK(crc_of(ct->save_path) == original);
    CHECK(safety.is_auto && crc_of(safety.file) == changed);
    CHECK(backup_list(&lib, ct, list, BACKUP_LIST_MAX) == 4);

    // Rename.
    CHECK(backup_rename(&lib, ct, &b2, "Clean copy") == DSAVE_OK);
    CHECK(backup_rename(&lib, ct, &b3, "Clean copy") == DSAVE_ERR_EXISTS);
    int n = backup_list(&lib, ct, list, BACKUP_LIST_MAX);
    bool found = false;
    for (int i = 0; i < n; i++)
        found |= strcmp(list[i].name, "Clean copy") == 0;
    CHECK(found);

    // Export writes an exact-size raw copy named after the ROM, replacing
    // an earlier export.
    char exported[DSAVE_PATH_MAX];
    CHECK(backup_export(&lib, ct, &b1, EXPORT_SAV, exported, sizeof(exported)) == DSAVE_OK);
    CHECK(backup_export(&lib, ct, &b1, EXPORT_SAV, exported, sizeof(exported)) == DSAVE_OK);
    CHECK(path_is(exported, "_DSavE/export/Chrono Trigger.sav"));
    CHECK(file_size(exported) == 64 * KB && crc_of(exported) == original);

    // .dsv: the same data, then DeSmuME's footer for EEPROM 512kbit.
    CHECK(backup_export(&lib, ct, &b1, EXPORT_DSV, exported, sizeof(exported)) == DSAVE_OK);
    CHECK(path_is(exported, "_DSavE/export/Chrono Trigger.dsv"));
    CHECK(crc_prefix(exported, 64 * KB) == original);
    {
        static const char marker[] =
            "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
        long footer = (long)strlen(marker) + 24 + 16;
        CHECK(file_size(exported) == 64 * KB + footer);
        uint8_t tail[256];
        FILE *f = fopen(exported, "rb");
        CHECK(f && fseek(f, 64 * KB, SEEK_SET) == 0 && fread(tail, 1, (size_t)footer, f) == (size_t)footer);
        if (f)
            fclose(f);
        const uint8_t *fields = tail + strlen(marker);
        CHECK(memcmp(tail, marker, strlen(marker)) == 0);
        CHECK(le32(fields) == 64 * KB && le32(fields + 4) == 64 * KB); // Size, padded size
        CHECK(le32(fields + 8) == 3 && le32(fields + 12) == 2);        // EEPROM 512kbit, 2 address bytes
        CHECK(le32(fields + 16) == 64 * KB && le32(fields + 20) == 0); // Memory size, version
        CHECK(memcmp(fields + 24, "|-DESMUME SAVE-|", 16) == 0);
    }
    char leftover[DSAVE_PATH_MAX];
    p(leftover, "_DSavE/export/.export.tmp");
    CHECK(!file_exists(leftover));

    // Trash moves instead of deleting.
    CHECK(backup_trash(&lib, ct, &b3) == DSAVE_OK);
    CHECK(!dir_exists(b3.dir));
    char trashed[DSAVE_PATH_MAX], leaf[96];
    snprintf(leaf, sizeof(leaf), "_DSavE/_TRASH_/YQUE_%s", b3.name);
    p(trashed, leaf);
    CHECK(dir_exists(trashed));
    CHECK(backup_list(&lib, ct, list, BACKUP_LIST_MAX) == 3);
}

static void test_restore_guards(void)
{
    new_card();
    make_rom("Chrono Trigger.nds", "CHRONOTRIGGE", "YQUE", "CHRONO TRIGGER");
    make_file("Chrono Trigger.sav", 64 * KB, 1);
    make_rom("Castlevania.nds", "CASTLEVANIA1", "ACVE", NULL);
    make_file("Castlevania.sav", 64 * KB, 2);
    library_scan(&lib, root);
    Game *ct = find_game("YQUE");
    Game *cv = find_game("ACVE");
    CHECK(ct && cv);
    if (!ct || !cv)
        return;

    Backup good;
    CHECK(backup_create(&lib, ct, "guard test", false, &good) == DSAVE_OK);
    uint32_t live = crc_of(ct->save_path);

    // Corrupted backup: flip one byte; restore must refuse and leave the
    // live save alone.
    FILE *f = fopen(good.file, "r+b");
    fseek(f, 100, SEEK_SET);
    int c = fgetc(f);
    fseek(f, 100, SEEK_SET);
    fputc(c ^ 0xFF, f);
    fclose(f);
    CHECK(backup_validate(&good) == DSAVE_ERR_CORRUPT);
    CHECK(backup_restore(&lib, ct, &good, NULL) == DSAVE_ERR_CORRUPT);
    CHECK(crc_of(ct->save_path) == live);

    // Truncated backup: not a valid DS save size.
    Backup trunc;
    CHECK(backup_create(&lib, ct, "truncated", false, &trunc) == DSAVE_OK);
    FILE *t = fopen(trunc.file, "r+b");
    CHECK(ftruncate(fileno(t), 1000) == 0);
    fclose(t);
    CHECK(backup_restore(&lib, ct, &trunc, NULL) == DSAVE_ERR_BAD_SIZE);

    // Backup made for another game is refused.
    Backup cvb;
    CHECK(backup_create(&lib, cv, "castlevania", false, &cvb) == DSAVE_OK);
    CHECK(backup_restore(&lib, ct, &cvb, NULL) == DSAVE_ERR_WRONG_GAME);
    CHECK(crc_of(ct->save_path) == live);
}

static void test_restore_sizes(void)
{
    // Pico pads saves to the save list size (512 KB by default); exact-size
    // backups from a PC or another kernel must still restore cleanly.
    new_card();
    make_dir("_pico");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_save("Game.sav", 512 * KB, 512 * KB, 9);
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    CHECK(g != NULL);
    if (!g)
        return;

    // A 64 KB backup onto a 512 KB file: data first, 0xFF after, size kept.
    make_file("exact.sav", 64 * KB, 3);
    char game_dir[DSAVE_PATH_MAX], bdir[DSAVE_PATH_MAX], bfile[DSAVE_PATH_MAX], src[DSAVE_PATH_MAX];
    backup_game_dir(&lib, g, game_dir, sizeof(game_dir));
    path_join(bdir, sizeof(bdir), game_dir, "from pc");
    mkdir_p(bdir);
    path_join(bfile, sizeof(bfile), bdir, "exact.sav");
    p(src, "exact.sav");
    copy_file(src, bfile);

    Backup list[BACKUP_LIST_MAX];
    CHECK(backup_list(&lib, g, list, BACKUP_LIST_MAX) == 1);
    CHECK(backup_restore(&lib, g, &list[0], NULL) == DSAVE_OK);
    CHECK(g->save_size == 512 * KB);
    CHECK(crc_prefix(g->save_path, 64 * KB) == crc_of(src));
    char padded[DSAVE_PATH_MAX];
    p(padded, "padded.sav");
    make_save("padded.sav", 512 * KB, 64 * KB, 3);
    CHECK(crc_of(g->save_path) == crc_of(padded));

    // The other way: a padded 512 KB backup onto an exact 64 KB save keeps
    // the file at 64 KB because the extra is only padding.
    CHECK(backup_restore_length(padded, 512 * KB, 64 * KB) == 64 * KB);
    // ...but real data past the end is never cut off.
    make_save("data.sav", 512 * KB, 128 * KB, 4);
    char data[DSAVE_PATH_MAX];
    p(data, "data.sav");
    CHECK(backup_restore_length(data, 512 * KB, 64 * KB) == 512 * KB);
    CHECK(backup_restore_length(data, 512 * KB, -1) == 512 * KB);

    new_card();
    make_rom("Small.nds", "SMALL", "SMLE", NULL);
    make_file("Small.sav", 64 * KB, 5);
    make_save("big.sav", 512 * KB, 64 * KB, 6);
    library_scan(&lib, root);
    g = find_game("SMLE");
    backup_game_dir(&lib, g, game_dir, sizeof(game_dir));
    path_join(bdir, sizeof(bdir), game_dir, "padded");
    mkdir_p(bdir);
    path_join(bfile, sizeof(bfile), bdir, "big.sav");
    p(src, "big.sav");
    copy_file(src, bfile);
    CHECK(backup_list(&lib, g, list, BACKUP_LIST_MAX) == 1);
    CHECK(backup_restore(&lib, g, &list[0], NULL) == DSAVE_OK);
    CHECK(g->save_size == 64 * KB);
    CHECK(crc_of(g->save_path) == crc_prefix(src, 64 * KB));
}

static void test_export_trim(void)
{
    // With Pico's save list, exports are trimmed to the real chip size.
    new_card();
    make_dir("_pico");
    static const char *const codes[] = { "YQUE" };
    static const uint8_t sizes[] = { 16 }; // 64 KB
    make_savelist(codes, sizes, 1);
    make_rom("Chrono Trigger.nds", "CHRONOTRIGGE", "YQUE", "CHRONO TRIGGER");
    make_save("Chrono Trigger.sav", 512 * KB, 64 * KB, 7);
    library_scan(&lib, root);
    Game *ct = find_game("YQUE");
    CHECK(ct && ct->chip_size == 64 * KB);
    if (!ct)
        return;
    Backup b;
    CHECK(backup_create(&lib, ct, "padded", false, &b) == DSAVE_OK);
    CHECK(b.size == 512 * KB); // Backups keep the file exactly as it was
    char out[DSAVE_PATH_MAX];
    CHECK(backup_export(&lib, ct, &b, EXPORT_SAV, out, sizeof(out)) == DSAVE_OK);
    CHECK(file_size(out) == 64 * KB);
    CHECK(crc_of(out) == crc_prefix(ct->save_path, 64 * KB));

    // Data past the chip size is kept rather than cut off.
    make_save("Chrono Trigger.sav", 512 * KB, 100 * KB, 7);
    library_refresh_game(&lib, ct);
    CHECK(backup_create(&lib, ct, "odd", false, &b) == DSAVE_OK);
    CHECK(backup_export(&lib, ct, &b, EXPORT_SAV, out, sizeof(out)) == DSAVE_OK);
    CHECK(file_size(out) == 512 * KB);
}

static void test_restore_to_other_kernel(void)
{
    // Copy a Pico save into TWiLight's location on the same card.
    new_card();
    make_dir("_pico");
    make_dir("_nds/TWiLightMenu");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_save("Game.sav", 512 * KB, 32 * KB, 11);
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    CHECK(g && g->primary == library_loc_for_kernel(g, KERNEL_PICO));
    if (!g)
        return;

    Backup b;
    CHECK(backup_create(&lib, g, "pico", false, &b) == DSAVE_OK);
    int twl = library_loc_for_kernel(g, KERNEL_TWILIGHT);
    CHECK(twl >= 0 && !g->locs[twl].exists);
    CHECK(backup_restore_to(&lib, g, &b, twl, NULL) == DSAVE_OK);
    CHECK(g->locs[twl].exists);
    CHECK(crc_of_rel("saves/Game.sav") == crc_of_rel("Game.sav"));
}

static void test_blank_and_first_save(void)
{
    new_card();
    make_rom("Castlevania.nds", "CASTLEVANIA1", "ACVE", NULL);
    make_rom("Pokemon.nds", "POKEMON SS", "IPGE", NULL);
    make_blank("Castlevania.sav", 64 * KB, 0xFF);
    library_scan(&lib, root);
    Game *cv = find_game("ACVE");
    Game *pk = find_game("IPGE");
    CHECK(cv && pk);
    if (!cv || !pk)
        return;

    Backup b;
    CHECK(backup_create(&lib, cv, "blank", false, &b) == DSAVE_WARN_BLANK);
    CHECK(backup_create(&lib, cv, "blank", true, &b) == DSAVE_OK);

    // No save: backup is refused, restore creates the file.
    CHECK(backup_create(&lib, pk, NULL, false, &b) == DSAVE_ERR_NO_SAVE);

    make_file("import.sav", 512 * KB, 7);
    char game_dir[DSAVE_PATH_MAX], bdir[DSAVE_PATH_MAX], bfile[DSAVE_PATH_MAX], src[DSAVE_PATH_MAX];
    backup_game_dir(&lib, pk, game_dir, sizeof(game_dir));
    path_join(bdir, sizeof(bdir), game_dir, "from pc");
    mkdir_p(bdir);
    path_join(bfile, sizeof(bfile), bdir, "anything.sav");
    p(src, "import.sav");
    copy_file(src, bfile);

    // A hand-copied backup with no backup.ini is still listed and usable.
    Backup list[BACKUP_LIST_MAX];
    CHECK(backup_list(&lib, pk, list, BACKUP_LIST_MAX) == 1);
    CHECK(!list[0].has_meta && list[0].size == 512 * KB);
    CHECK(backup_restore(&lib, pk, &list[0], NULL) == DSAVE_OK);
    CHECK(pk->has_save && pk->save_size == 512 * KB);
    CHECK(crc_of(pk->save_path) == crc_of(src));
}

static void test_sync_check(void)
{
    // DSpico card: Pico keeps a 512 KB padded save beside the ROM; TWiLight
    // has an exact 32 KB copy of the same data in saves/.
    new_card();
    make_dir("_pico");
    make_dir("_nds/TWiLightMenu");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_save("Game.sav", 512 * KB, 32 * KB, 21);
    make_save("saves/Game.sav", 32 * KB, 32 * KB, 21);
    set_mtime("Game.sav", 1000000000);
    set_mtime("saves/Game.sav", 1100000000);
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    CHECK(g && g->loc_count == 2);
    if (!g)
        return;
    int pico = library_loc_for_kernel(g, KERNEL_PICO);
    int twl = library_loc_for_kernel(g, KERNEL_TWILIGHT);

    SyncInfo si;
    sync_check(g, &si);
    CHECK(si.state == SYNC_MATCH && si.existing == 2);
    CHECK(si.newest == twl && si.loc[twl].newest && !si.loc[pico].newest);
    CHECK(si.loc[pico].data_size == 32 * KB && si.loc[twl].data_size == 32 * KB);

    // Change TWiLight's copy: now they differ.
    make_save("saves/Game.sav", 32 * KB, 32 * KB, 22);
    library_refresh_game(&lib, g);
    sync_check(g, &si);
    CHECK(si.state == SYNC_DIFFERENT);
    CHECK(si.loc[pico].same_as_primary && !si.loc[twl].same_as_primary);

    // Only one copy: nothing to compare.
    char path[DSAVE_PATH_MAX];
    p(path, "saves/Game.sav");
    remove(path);
    library_refresh_game(&lib, g);
    sync_check(g, &si);
    CHECK(si.state == SYNC_SINGLE && si.existing == 1);
}

static void test_sync_padding_edge(void)
{
    // 0xFF bytes inside the data, including across a 4 KB chunk boundary,
    // are data, not padding.
    new_card();
    make_dir("_pico");
    make_dir("_nds/TWiLightMenu");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    char a[DSAVE_PATH_MAX], b[DSAVE_PATH_MAX];
    p(a, "Game.sav");
    p(b, "saves/Game.sav");
    make_dir("saves");
    FILE *fa = fopen(a, "wb"), *fb = fopen(b, "wb");
    for (long i = 0; i < 64 * KB; i++)
    {
        uint8_t v = (i >= 4000 && i < 9000) ? 0xFF : (uint8_t)(i & 0x7F);
        if (i >= 16 * KB)
            v = 0xFF;
        fputc(v, fa);
        if (i < 16 * KB)
            fputc(v, fb);
    }
    fclose(fa);
    fclose(fb);
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    SyncInfo si;
    sync_check(g, &si);
    // The 0xFF run at 4000-8999 sits inside the data; only the tail from
    // 16 KB on is padding.
    CHECK(si.loc[library_loc_for_kernel(g, KERNEL_PICO)].data_size == 16 * KB);
    CHECK(si.state == SYNC_MATCH);
}

static void test_sync_copy_and_all(void)
{
    new_card();
    make_dir("_pico");
    make_dir("_nds/TWiLightMenu");
    make_text("_nds/akmenunext/globalsettings.ini", "[system]\nsavedir = 1\nsaveext = .nds.sav\n");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_save("Game.sav", 512 * KB, 32 * KB, 31);
    make_save("saves/Game.sav", 512 * KB, 32 * KB, 32);
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    CHECK(g && g->loc_count == 3);
    if (!g)
        return;
    int pico = library_loc_for_kernel(g, KERNEL_PICO);
    int twl = library_loc_for_kernel(g, KERNEL_TWILIGHT);
    int ak = library_loc_for_kernel(g, KERNEL_AKMENUNEXT);
    CHECK(path_is(g->locs[ak].path, "saves/Game.nds.sav"));

    // Copy TWiLight's save over Pico's; Pico's old save is kept as auto_.
    uint32_t old_pico = crc_of_rel("Game.sav");
    Backup safety;
    CHECK(sync_copy(&lib, g, twl, pico, &safety) == DSAVE_OK);
    CHECK(crc_of_rel("Game.sav") == crc_of_rel("saves/Game.sav"));
    CHECK(safety.is_auto && crc_of(safety.file) == old_pico);
    CHECK(sync_copy(&lib, g, ak, pico, NULL) == DSAVE_ERR_NO_SAVE);

    // Sync all: the primary (Pico) goes everywhere, including akmenu-next's
    // missing file.
    make_save("Game.sav", 512 * KB, 40 * KB, 33);
    library_refresh_game(&lib, g);
    int copied = 0;
    CHECK(sync_all(&lib, g, &copied) == DSAVE_OK && copied == 2);
    CHECK(g->locs[ak].exists);
    SyncInfo si;
    sync_check(g, &si);
    CHECK(si.state == SYNC_MATCH && si.existing == 3);
}

static void test_slots(void)
{
    new_card();
    make_dir("_nds/TWiLightMenu");
    make_dir("_pico");
    make_rom("roms/Game.nds", "GAME", "ABCE", NULL);
    make_save("roms/saves/Game.sav", 64 * KB, 64 * KB, 41);
    make_save("roms/saves/Game.sav2", 64 * KB, 64 * KB, 42);
    library_scan(&lib, root);
    library_set_kernel(&lib, KERNEL_TWILIGHT);
    Game *g = find_game("ABCE");
    CHECK(g != NULL);
    if (!g)
        return;

    SaveSlot slots[SLOT_MAX];
    int n = slots_list(&lib, g, KERNEL_TWILIGHT, slots, SLOT_MAX);
    CHECK(n == 10);
    CHECK(slots[0].exists && slots[0].current);
    CHECK(!slots[1].exists && slots[2].exists && !slots[2].current);
    CHECK(path_is(slots[2].path, "roms/saves/Game.sav2"));
    CHECK(slots_list(&lib, g, KERNEL_PICO, slots, SLOT_MAX) == 1);
    CHECK(slots_list(&lib, g, KERNEL_WOOD, slots, SLOT_MAX) == 0);

    // Switch TWiLight to slot 2: the per-game ini is written and the
    // primary save follows.
    CHECK(slots_set_current(&lib, g, KERNEL_TWILIGHT, 2) == DSAVE_OK);
    CHECK(path_is(g->save_path, "roms/saves/Game.sav2"));
    char ini[DSAVE_PATH_MAX];
    p(ini, "_nds/TWiLightMenu/gamesettings/Game.nds.ini");
    CHECK(ini_get_int(ini, "GAMESETTINGS", "SAVE_NUMBER", -1) == 2);
    n = slots_list(&lib, g, KERNEL_TWILIGHT, slots, SLOT_MAX);
    CHECK(slots[2].current && !slots[0].current);
    CHECK(slots_set_current(&lib, g, KERNEL_PICO, 1) == DSAVE_ERR_UNSUPPORTED);
    CHECK(slots_set_current(&lib, g, KERNEL_TWILIGHT, 10) == DSAVE_ERR_PATH);

    // Restore a backup into an empty slot file.
    Backup b;
    CHECK(backup_create(&lib, g, "slot2", false, &b) == DSAVE_OK);
    CHECK(backup_restore_path(&lib, g, &b, slots[5].path, NULL) == DSAVE_OK);
    CHECK(crc_of(slots[5].path) == crc_of_rel("roms/saves/Game.sav2"));
}

static void test_compression(void)
{
    new_card();
    make_dir("_pico");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_save("Game.sav", 512 * KB, 48 * KB, 51);
    library_scan(&lib, root);
    CHECK(!lib.compress);
    CHECK(library_set_compress(&lib, true));
    library_scan(&lib, root);
    CHECK(lib.compress); // Remembered in settings.ini
    Game *g = find_game("ABCE");
    if (!g)
        return;
    uint32_t live = crc_of(g->save_path);

    Backup b;
    CHECK(backup_create(&lib, g, "packed", false, &b) == DSAVE_OK);
    CHECK(b.compressed && has_ext(b.file, ".sav.gz"));
    CHECK(file_size(b.file) < 64 * KB); // 512 KB of mostly padding
    CHECK(b.size == 512 * KB && b.crc32 == live);
    CHECK(backup_validate(&b) == DSAVE_OK);

    // It is a standard gzip file.
    char cmd[DSAVE_PATH_MAX * 2 + 64], out[DSAVE_PATH_MAX];
    p(out, "gunzipped.sav");
    snprintf(cmd, sizeof(cmd), "gzip -dc '%s' > '%s'", b.file, out);
    CHECK(system(cmd) == 0);
    CHECK(crc_of(out) == live);

    // Listed, restored and exported like a raw backup.
    Backup list[BACKUP_LIST_MAX];
    CHECK(backup_list(&lib, g, list, BACKUP_LIST_MAX) == 1 && list[0].compressed);
    make_save("Game.sav", 512 * KB, 48 * KB, 52);
    library_refresh_game(&lib, g);
    Backup safety;
    CHECK(backup_restore(&lib, g, &list[0], &safety) == DSAVE_OK);
    CHECK(crc_of(g->save_path) == live && safety.compressed);
    char exported[DSAVE_PATH_MAX];
    CHECK(backup_export(&lib, g, &list[0], EXPORT_SAV, exported, sizeof(exported)) == DSAVE_OK);
    CHECK(has_ext(exported, ".sav") && crc_of(exported) == live);

    // Decompress and compress again in place.
    Backup raw, packed;
    CHECK(backup_set_compressed(&list[0], false, &raw) == DSAVE_OK);
    CHECK(!raw.compressed && has_ext(raw.file, ".sav") && !file_exists(list[0].file));
    CHECK(crc_of(raw.file) == live);
    CHECK(backup_set_compressed(&raw, true, &packed) == DSAVE_OK);
    CHECK(packed.compressed && !file_exists(raw.file) && backup_validate(&packed) == DSAVE_OK);

    // A corrupt .gz is refused and the live save is left alone.
    FILE *f = fopen(packed.file, "r+b");
    fseek(f, 20, SEEK_SET);
    fputc(0x00, f);
    fputc(0x11, f);
    fclose(f);
    CHECK(backup_validate(&packed) == DSAVE_ERR_CORRUPT);
    CHECK(backup_restore(&lib, g, &packed, NULL) == DSAVE_ERR_CORRUPT);
    CHECK(crc_of(g->save_path) == live);

    // A .sav.gz made by gzip on a PC (with a file name in the header) works.
    char game_dir[DSAVE_PATH_MAX], bdir[DSAVE_PATH_MAX], src[DSAVE_PATH_MAX];
    backup_game_dir(&lib, g, game_dir, sizeof(game_dir));
    path_join(bdir, sizeof(bdir), game_dir, "from pc");
    mkdir_p(bdir);
    make_save("pc.sav", 64 * KB, 64 * KB, 53);
    p(src, "pc.sav");
    snprintf(cmd, sizeof(cmd), "cd '%s' && gzip -k pc.sav && mv pc.sav.gz '%s/'", root, bdir);
    CHECK(system(cmd) == 0);
    int n = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    bool ok = false;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(list[i].name, "from pc") == 0)
            ok = list[i].compressed && list[i].size == 64 * KB &&
                 backup_restore(&lib, g, &list[i], NULL) == DSAVE_OK;
    }
    CHECK(ok);
    CHECK(crc_prefix(g->save_path, 64 * KB) == crc_of(src));
    CHECK(savefile_tail_is_ff(g->save_path, 64 * KB));

    // Saves over 1 MB stay raw.
    make_save("Game.sav", 2 * KB * KB, 2 * KB * KB, 54);
    library_refresh_game(&lib, g);
    CHECK(backup_create(&lib, g, "big", false, &b) == DSAVE_OK);
    CHECK(!b.compressed);
    CHECK(backup_set_compressed(&b, true, NULL) == DSAVE_ERR_UNSUPPORTED);
}

// Writes a ROM whose banner titles are UTF-16 code units, one array per
// language slot (JP, EN, FR, DE, IT, ES); NULL leaves a slot empty.
static void make_rom_titles(const char *rel, const char *code, const uint16_t *const titles[6])
{
    char path[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    p(path, rel);
    path_dirname(dir, sizeof(dir), path);
    mkdir_p(dir);
    static uint8_t rom[0x200 + 0xA00];
    memset(rom, 0, sizeof(rom));
    memcpy(rom, "JPGAME", 6);
    memcpy(rom + 0x0C, code, 4);
    rom[0x69] = 0x02;
    for (int l = 0; l < 6; l++)
    {
        for (int i = 0; titles[l] && titles[l][i]; i++)
        {
            rom[0x200 + 0x240 + l * 0x100 + i * 2] = (uint8_t)titles[l][i];
            rom[0x200 + 0x240 + l * 0x100 + i * 2 + 1] = (uint8_t)(titles[l][i] >> 8);
        }
    }
    FILE *f = fopen(path, "wb");
    fwrite(rom, 1, sizeof(rom), f);
    fclose(f);
}

static void test_titles(void)
{
    static const uint16_t jp[] = { 0x30DD, 0x30B1, 0x30E2, 0x30F3, 0 }; // ポケモン
    static const uint16_t fr[] = { 'P', 'o', 'k', 0xE9, 'm', 'o', 'n', 0 };
    static const uint16_t quote[] = { 'K', 'i', 'r', 'b', 'y', 0x2019, 's', 0 };

    new_card();
    make_dir("_pico");
    // Japanese in every slot: the file name is used.
    const uint16_t *const all_jp[6] = { jp, jp, jp, jp, jp, jp };
    make_rom_titles("Pokemon Diamond (Japan).nds", "ADAJ", all_jp);
    // Japanese English slot, French slot in Latin: French wins.
    const uint16_t *const jp_fr[6] = { jp, jp, fr, NULL, NULL, NULL };
    make_rom_titles("x.nds", "IPKF", jp_fr);
    // Curly apostrophes are drawable.
    const uint16_t *const en_q[6] = { NULL, quote, NULL, NULL, NULL, NULL };
    make_rom_titles("k.nds", "AKBE", en_q);
    // Japanese file name and banner: the header title.
    make_rom_titles("\xe3\x83\x9d.nds", "AJPJ", all_jp);
    library_scan(&lib, root);

    Game *g = find_game("ADAJ");
    CHECK(g && strcmp(g->title, "Pokemon Diamond (Japan)") == 0);
    CHECK(g && !g->info.banner_latin);
    g = find_game("IPKF");
    CHECK(g && strcmp(g->title, "Pok\xc3\xa9mon") == 0 && g->info.banner_latin);
    g = find_game("AKBE");
    CHECK(g && strcmp(g->title, "Kirby\xe2\x80\x99s") == 0);
    g = find_game("AJPJ");
    CHECK(g && strcmp(g->title, "JPGAME") == 0);
    CHECK(text_is_latin("Caf\xc3\xa9 \xe2\x80\x94 ok") && !text_is_latin("\xe3\x83\x9d"));
}

static void test_import(void)
{
    new_card();
    make_dir("_pico");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    if (!g)
        return;

    ImportFile files[IMPORT_LIST_MAX];
    CHECK(backup_import_list(&lib, files, IMPORT_LIST_MAX) == 0);
    char dir[DSAVE_PATH_MAX];
    p(dir, "_DSavE/import");
    CHECK(dir_exists(dir)); // Created for the user to fill

    // A raw save, a DeSmuME .dsv with its footer, an odd size, a gz,
    // a no$gba save and an unrelated file.
    make_save("_DSavE/import/From PC.sav", 64 * KB, 10 * KB, 61);
    uint32_t raw = crc_of_rel("_DSavE/import/From PC.sav");
    make_save("_DSavE/import/emu.dsv", 8 * KB, 8 * KB, 62);
    uint32_t dsv_data = crc_of_rel("_DSavE/import/emu.dsv");
    {
        char path[DSAVE_PATH_MAX];
        p(path, "_DSavE/import/emu.dsv");
        FILE *f = fopen(path, "ab");
        fputs("|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:", f);
        uint8_t fields[24] = { 0 };
        fwrite(fields, 1, sizeof(fields), f);
        fputs("|-DESMUME SAVE-|", f);
        fclose(f);
    }
    make_file("_DSavE/import/odd.sav", 1000, 63);
    make_save("_DSavE/import/packed.sav", 512 * KB, 4 * KB, 64);
    uint32_t packed = crc_of_rel("_DSavE/import/packed.sav");
    {
        char cmd[DSAVE_PATH_MAX + 32], path[DSAVE_PATH_MAX];
        p(path, "_DSavE/import/packed.sav");
        snprintf(cmd, sizeof(cmd), "gzip '%s'", path);
        CHECK(system(cmd) == 0);
        p(path, "_DSavE/import/nocash.sav");
        FILE *f = fopen(path, "wb");
        fputs("NocashGbaBackupMediaSavDataFile", f);
        for (int i = 0; i < 4096; i++)
            fputc(0, f);
        fclose(f);
    }
    make_text("_DSavE/import/readme.txt", "hi");

    int n = backup_import_list(&lib, files, IMPORT_LIST_MAX);
    CHECK(n == 5); // Not the .txt
    CHECK(n == 5 && strcmp(files[0].name, "emu.dsv") == 0 && files[0].size == 8 * KB && files[0].valid);
    CHECK(n == 5 && strcmp(files[1].name, "From PC.sav") == 0 && files[1].valid);
    CHECK(n == 5 && strcmp(files[2].name, "nocash.sav") == 0 && !files[2].valid && files[2].format == EMU_NOCASH);
    CHECK(n == 5 && strcmp(files[3].name, "odd.sav") == 0 && !files[3].valid);
    CHECK(n == 5 && strcmp(files[4].name, "packed.sav.gz") == 0 && files[4].size == 512 * KB);
    if (n != 5)
        return;

    Backup b;
    CHECK(backup_import(&lib, g, files[1].path, &b) == DSAVE_OK);
    CHECK(strcmp(b.name, "import_From PC") == 0 && b.crc32 == raw && b.size == 64 * KB);
    CHECK(file_exists(files[1].path)); // Left in place
    CHECK(backup_import(&lib, g, files[0].path, &b) == DSAVE_OK);
    CHECK(b.size == 8 * KB && b.crc32 == dsv_data && backup_validate(&b) == DSAVE_OK);
    char tmp[DSAVE_PATH_MAX];
    p(tmp, "_DSavE/.import.tmp");
    CHECK(!file_exists(tmp));
    CHECK(backup_import(&lib, g, files[4].path, &b) == DSAVE_OK);
    CHECK(strcmp(b.name, "import_packed") == 0 && b.crc32 == packed);
    CHECK(backup_import(&lib, g, files[2].path, &b) == DSAVE_ERR_DAMAGED); // Header cut short
    CHECK(backup_import(&lib, g, files[3].path, &b) == DSAVE_ERR_BAD_SIZE);

    // backup.ini names the original file, and the backup restores.
    Backup list[BACKUP_LIST_MAX];
    CHECK(backup_list(&lib, g, list, BACKUP_LIST_MAX) == 3);
    char ini[DSAVE_PATH_MAX], source[DSAVE_PATH_MAX] = "";
    path_join(ini, sizeof(ini), list[0].dir, "backup.ini");
    FILE *mf = fopen(ini, "r");
    char line[DSAVE_PATH_MAX + 16];
    while (mf && fgets(line, sizeof(line), mf))
    {
        if (strncmp(line, "source=", 7) == 0)
            snprintf(source, sizeof(source), "%s", line + 7);
    }
    if (mf)
        fclose(mf);
    CHECK(strstr(source, "_DSavE/import/") != NULL);
    for (int i = 0; i < 3; i++)
    {
        if (strcmp(list[i].name, "import_emu") == 0)
        {
            CHECK(backup_restore(&lib, g, &list[i], NULL) == DSAVE_OK);
            CHECK(crc_of(g->save_path) == dsv_data && g->save_size == 8 * KB);
        }
    }
}

static void test_prune_auto(void)
{
    new_card();
    make_dir("_pico");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_save("Game.sav", 64 * KB, 16 * KB, 71);
    library_scan(&lib, root);
    CHECK(lib.keep_auto == LIBRARY_KEEP_AUTO_DEFAULT);
    CHECK(library_set_keep_auto(&lib, 2));
    library_scan(&lib, root);
    CHECK(lib.keep_auto == 2); // Remembered
    Game *g = find_game("ABCE");
    if (!g)
        return;

    Backup keep;
    CHECK(backup_create(&lib, g, "mine", false, &keep) == DSAVE_OK);
    for (int i = 0; i < 4; i++)
    {
        make_save("Game.sav", 64 * KB, 16 * KB, (uint8_t)(80 + i));
        library_refresh_game(&lib, g);
        CHECK(backup_restore(&lib, g, &keep, NULL) == DSAVE_OK);
    }
    Backup list[BACKUP_LIST_MAX];
    int n = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    int autos = 0;
    for (int i = 0; i < n; i++)
        autos += list[i].is_auto;
    CHECK(n == 3 && autos == 2); // "mine" is never pruned
    // The two newest autos hold the last two saves that were replaced.
    // Each auto holds the save from before that restore: seeds 82 and 83.
    bool seen82 = false, seen83 = false;
    for (int i = 0; i < n; i++)
    {
        if (!list[i].is_auto)
            continue;
        char want[DSAVE_PATH_MAX];
        make_save("probe.sav", 64 * KB, 16 * KB, 82);
        p(want, "probe.sav");
        seen82 |= list[i].crc32 == crc_of(want);
        make_save("probe.sav", 64 * KB, 16 * KB, 83);
        seen83 |= list[i].crc32 == crc_of(want);
    }
    CHECK(seen82 && seen83);
    // Pruned ones are in the trash, not deleted.
    char trash[DSAVE_PATH_MAX];
    p(trash, "_DSavE/_TRASH_");
    DIR *d = opendir(trash);
    int in_trash = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL)
        in_trash += strncmp(e->d_name, "ABCE_auto_", 10) == 0;
    if (d)
        closedir(d);
    CHECK(in_trash == 2);

    // 0 keeps everything.
    CHECK(library_set_keep_auto(&lib, 0));
    CHECK(backup_prune_auto(&lib, g, 0, NULL) == 0);
}

static void test_note_and_keep(void)
{
    new_card();
    make_dir("_pico");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_save("Game.sav", 64 * KB, 16 * KB, 71);
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    if (!g)
        return;

    Backup b, u;
    CHECK(backup_create(&lib, g, "boss", false, &b) == DSAVE_OK);
    CHECK(b.note[0] == '\0' && !b.keep);

    // A note survives listing, a rename and compression.
    CHECK(backup_set_note(&b, "before the\nfinal boss", &u) == DSAVE_OK);
    CHECK(strcmp(u.note, "before the final boss") == 0);
    CHECK(backup_rename(&lib, g, &b, "boss2") == DSAVE_OK);
    Backup list[BACKUP_LIST_MAX];
    int n = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    CHECK(n == 1 && strcmp(list[0].note, "before the final boss") == 0);
    CHECK(backup_validate(&list[0]) == DSAVE_OK); // Size and CRC still read
    CHECK(backup_set_compressed(&list[0], true, &u) == DSAVE_OK);
    n = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    CHECK(n == 1 && list[0].compressed && strcmp(list[0].note, "before the final boss") == 0);

    // Editing replaces the note; an empty one removes it.
    CHECK(backup_set_note(&list[0], "100%", NULL) == DSAVE_OK);
    n = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    CHECK(strcmp(list[0].note, "100%") == 0);
    CHECK(backup_set_note(&list[0], "", NULL) == DSAVE_OK);
    n = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    CHECK(list[0].note[0] == '\0' && list[0].has_meta);
    char ini[DSAVE_PATH_MAX], line[256];
    path_join(ini, sizeof(ini), list[0].dir, "backup.ini");
    FILE *f = fopen(ini, "r");
    bool note_line = false;
    while (f && fgets(line, sizeof(line), f))
        note_line |= strncmp(line, "note=", 5) == 0;
    if (f)
        fclose(f);
    CHECK(!note_line);

    // Long notes are cut, never in the middle of a UTF-8 character.
    char longnote[64];
    memset(longnote, 'a', sizeof(longnote));
    memcpy(longnote + BACKUP_NOTE_MAX - 1, "\xc3\xa9", 2); // "é" across the limit
    longnote[sizeof(longnote) - 1] = '\0';
    CHECK(backup_set_note(&list[0], longnote, &u) == DSAVE_OK);
    CHECK(strlen(u.note) == BACKUP_NOTE_MAX - 1);

    // A kept backup can't be trashed until the mark is cleared.
    CHECK(backup_set_keep(&list[0], true, &u) == DSAVE_OK && u.keep);
    n = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    CHECK(list[0].keep);
    CHECK(backup_trash(&lib, g, &list[0]) == DSAVE_ERR_KEPT);
    CHECK(backup_list(&lib, g, list, BACKUP_LIST_MAX) == 1);

    // Kept auto backups are never pruned and don't count towards the limit.
    CHECK(library_set_keep_auto(&lib, 1));
    Backup src = list[0];
    for (int i = 0; i < 2; i++)
    {
        make_save("Game.sav", 64 * KB, 16 * KB, (uint8_t)(90 + i));
        library_refresh_game(&lib, g);
        Backup made;
        CHECK(backup_restore(&lib, g, &src, &made) == DSAVE_OK);
        if (i == 0)
            CHECK(backup_set_keep(&made, true, NULL) == DSAVE_OK);
    }
    n = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    int autos = 0, kept_autos = 0;
    for (int i = 0; i < n; i++)
    {
        autos += list[i].is_auto;
        kept_autos += list[i].is_auto && list[i].keep;
    }
    CHECK(autos == 2 && kept_autos == 1);
    make_save("Game.sav", 64 * KB, 16 * KB, 95);
    library_refresh_game(&lib, g);
    CHECK(backup_restore(&lib, g, &src, NULL) == DSAVE_OK);
    n = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    autos = kept_autos = 0;
    for (int i = 0; i < n; i++)
    {
        autos += list[i].is_auto;
        kept_autos += list[i].is_auto && list[i].keep;
    }
    CHECK(autos == 2 && kept_autos == 1); // The kept one plus the newest

    // Clearing the mark allows trashing again.
    for (int i = 0; i < n; i++)
    {
        if (list[i].keep)
        {
            CHECK(backup_set_keep(&list[i], false, &u) == DSAVE_OK && !u.keep);
            CHECK(backup_trash(&lib, g, &u) == DSAVE_OK);
        }
    }
    CHECK(library_set_keep_auto(&lib, LIBRARY_KEEP_AUTO_DEFAULT));
}

static void test_activity(void)
{
    new_card();
    make_dir("_pico");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_save("Game.sav", 64 * KB, 16 * KB, 71);
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    if (!g)
        return;
    Activity acts[ACTIVITY_LIST_MAX];
    CHECK(activity_list(&lib, acts, ACTIVITY_LIST_MAX) == 0);

    Backup b, safety;
    CHECK(backup_create(&lib, g, "start", false, &b) == DSAVE_OK);
    CHECK(activity_log(&lib, ACT_BACKUP, g, b.name, b.crc32, "", g->save_path));
    char save[DSAVE_PATH_MAX];
    snprintf(save, sizeof(save), "%s", g->save_path);
    uint32_t before = crc_of(save);

    // Restore over a newer save, then undo it.
    make_save("Game.sav", 64 * KB, 16 * KB, 72);
    library_refresh_game(&lib, g);
    uint32_t newer = crc_of(save);
    CHECK(newer != before);
    CHECK(backup_restore(&lib, g, &b, &safety) == DSAVE_OK && safety.name[0]);
    CHECK(activity_log(&lib, ACT_RESTORE, g, b.name, b.crc32, safety.name, g->save_path));
    CHECK(crc_of(save) == before);

    int n = activity_list(&lib, acts, ACTIVITY_LIST_MAX);
    CHECK(n == 2 && acts[0].kind == ACT_RESTORE && acts[1].kind == ACT_BACKUP);
    CHECK(strcmp(acts[0].backup, "start") == 0 && strcmp(acts[0].safety, safety.name) == 0);
    CHECK(strcmp(acts[0].game_code, "ABCE") == 0 && strcmp(acts[0].path, save) == 0);
    CHECK(acts[0].crc32 == b.crc32 && !acts[0].card);
    CHECK(activity_can_undo(&acts[0]) && !activity_can_undo(&acts[1]));

    int gi = -1;
    Backup undo_safety;
    CHECK(activity_undo(&lib, &acts[0], &gi, &undo_safety) == DSAVE_OK);
    CHECK(gi >= 0 && &lib.games[gi] == g && undo_safety.name[0]);
    CHECK(crc_of(save) == newer);

    // The undo is logged, and can itself be undone.
    n = activity_list(&lib, acts, ACTIVITY_LIST_MAX);
    CHECK(n == 3 && acts[0].kind == ACT_UNDO && activity_can_undo(&acts[0]));
    CHECK(activity_undo(&lib, &acts[0], NULL, NULL) == DSAVE_OK);
    CHECK(crc_of(save) == before);

    // An auto backup that's gone can't be undone to.
    n = activity_list(&lib, acts, ACTIVITY_LIST_MAX);
    Activity gone = acts[0];
    snprintf(gone.safety, sizeof(gone.safety), "auto_nope");
    CHECK(activity_undo(&lib, &gone, NULL, NULL) == DSAVE_ERR_GONE);

    // Tabs and line breaks in a title don't break the line.
    char title[sizeof(g->title)];
    snprintf(title, sizeof(title), "%s", g->title);
    snprintf(g->title, sizeof(g->title), "Tab\there\nnewline");
    CHECK(activity_log(&lib, ACT_EXPORT, g, "start", 0, "", "x"));
    snprintf(g->title, sizeof(g->title), "%s", title);
    n = activity_list(&lib, acts, ACTIVITY_LIST_MAX);
    CHECK(acts[0].kind == ACT_EXPORT && strcmp(acts[0].title, "Tab here newline") == 0);

    // Junk lines are skipped; the log rotates and keeps the newest.
    char logp[DSAVE_PATH_MAX];
    p(logp, ACTIVITY_FILE);
    FILE *f = fopen(logp, "a");
    fputs("garbage line\n\n2026\tbackup\n", f);
    fclose(f);
    int before_junk = n;
    CHECK(activity_list(&lib, acts, ACTIVITY_LIST_MAX) == before_junk);
    for (int i = 0; i < 700; i++)
        activity_log(&lib, ACT_BACKUP, g, "spam", (uint32_t)i, "", g->save_path);
    CHECK(file_size(logp) <= ACTIVITY_ROTATE_BYTES + 512);
    n = activity_list(&lib, acts, ACTIVITY_LIST_MAX);
    CHECK(n == ACTIVITY_LIST_MAX && acts[0].crc32 == 699 && acts[ACTIVITY_LIST_MAX - 1].crc32 == 699 - (ACTIVITY_LIST_MAX - 1));
}

// ---------------------------------------------------------------------------
// Game card saves, against a fake save chip

static FakeCard fc;
static CardSlot slot;

// Data that never repeats, so it can't be mistaken for address wrapping.
static uint8_t chip_byte(long i, uint8_t seed)
{
    return (uint8_t)((((uint32_t)i * 2654435761u) >> 13) ^ seed);
}

static void fill_chip(long n, uint8_t seed)
{
    for (long i = 0; i < n; i++)
        fc.mem[i] = chip_byte(i, seed);
}

static bool chip_is(long n, uint8_t seed)
{
    for (long i = 0; i < n; i++)
    {
        if (fc.mem[i] != chip_byte(i, seed))
            return false;
    }
    return true;
}

static void test_icon(void)
{
    // Pixel (x, y) holds (x + 2y) % 16, laid out in 8x8 tiles.
    uint8_t bitmap[0x200] = { 0 }, pal[0x20];
    for (int y = 0; y < 32; y++)
    {
        for (int x = 0; x < 32; x++)
        {
            int in = (y % 8) * 8 + x % 8, tile = (y / 8) * 4 + x / 8, v = (x + 2 * y) % 16;
            bitmap[tile * 32 + in / 2] |= (uint8_t)((in & 1) ? v << 4 : v);
        }
    }
    for (int i = 0; i < 16; i++)
    {
        pal[i * 2] = (uint8_t)(i * 0x111);
        pal[i * 2 + 1] = (uint8_t)((i * 0x111) >> 8) | 0x80; // Bit 15 is ignored
    }
    RomIcon icon;
    rom_decode_icon(bitmap, pal, &icon);
    bool right = icon.valid;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            right &= icon.px[y * 32 + x] == (x + 2 * y) % 16;
    CHECK(right);
    CHECK(icon.pal[5] == 0x555 && icon.pal[15] == 0xFFF);

    // From a ROM file: one with a banner, one without.
    new_card();
    make_rom("with.nds", "WITH", "ABCE", "With banner");
    make_rom("without.nds", "WITHOUT", "ABDE", NULL);
    char path[DSAVE_PATH_MAX];
    p(path, "with.nds");
    CHECK(rom_read_icon(path, &icon) && icon.valid);
    p(path, "without.nds");
    CHECK(!rom_read_icon(path, &icon) && !icon.valid);
    p(path, "missing.nds");
    CHECK(!rom_read_icon(path, &icon));
}

static void test_card_detect(void)
{
    static const struct {
        FakeChipType type;
        long size;
        uint32_t jedec;
        bool ir, blank;
        ChipType want;
        long want_size;
        bool guessed, writable;
    } cases[] = {
        { FAKE_EEPROM_TINY, 512, 0, false, false, CHIP_EEPROM_TINY, 512, false, true },
        { FAKE_EEPROM, 8 * KB, 0, false, false, CHIP_EEPROM, 8 * KB, false, true },
        { FAKE_EEPROM, 64 * KB, 0, false, false, CHIP_EEPROM, 64 * KB, false, true },
        { FAKE_EEPROM, 8 * KB, 0, false, true, CHIP_EEPROM, 64 * KB, true, true },
        { FAKE_EEPROM, 128 * KB, 0, false, false, CHIP_EEPROM, 128 * KB, false, true },
        { FAKE_FLASH, 256 * KB, 0x204012, false, false, CHIP_FLASH, 256 * KB, false, true },
        { FAKE_FLASH, 512 * KB, 0x621100, false, false, CHIP_FLASH, 512 * KB, false, true },
        { FAKE_FLASH, 1024 * KB, 0x204014, false, false, CHIP_FLASH, 1024 * KB, false, true },
        { FAKE_FLASH, 512 * KB, 0x204013, true, false, CHIP_FLASH, 512 * KB, false, true },
        { FAKE_FLASH, 512 * KB, 0x123456, false, false, CHIP_FLASH, 512 * KB, false, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        fake_card_init(&fc, "ABCE", "TEST", cases[i].type, cases[i].size, cases[i].jedec, cases[i].ir);
        fake_card_slot(&fc, &slot);
        if (!cases[i].blank)
            fill_chip(cases[i].size, (uint8_t)i);
        CardChip chip;
        // The IR hint is wrong on purpose for odd cases: detection retries.
        bool ok = card_chip_detect(&slot.bus, (i & 1) ? !cases[i].ir : cases[i].ir, &chip);
        CHECK(ok && chip.type == cases[i].want && chip.size == cases[i].want_size);
        if (!ok || chip.size != cases[i].want_size)
            fprintf(stderr, "  case %zu: type %d size %ld\n", i, chip.type, chip.size);
        CHECK(chip.size_guessed == cases[i].guessed && chip.writable == cases[i].writable);
        CHECK(chip.infrared == cases[i].ir);
        if (!ok || cases[i].blank)
            continue;
        // Reads across a 256-byte boundary come back right.
        uint8_t buf[300];
        long at = cases[i].size > 600 ? 250 : 100;
        card_chip_read(&slot.bus, &chip, (uint32_t)at, buf, 300);
        CHECK(memcmp(buf, fc.mem + at, 300) == 0);
    }

    char desc[32];
    fake_card_init(&fc, "IPKE", "POKEMON HG", FAKE_FLASH, 512 * KB, 0x204013, true);
    fake_card_slot(&fc, &slot);
    CardChip chip;
    CHECK(card_chip_detect(&slot.bus, true, &chip));
    card_chip_describe(&chip, desc, sizeof(desc));
    CHECK(strcmp(desc, "FLASH 512 KB + IR") == 0);

    fake_card_init(&fc, "ABCE", "TEST", FAKE_NONE, 0, 0, false);
    fake_card_slot(&fc, &slot);
    CHECK(!card_chip_detect(&slot.bus, false, &chip) && chip.type == CHIP_NONE);
}

static void test_card_write(void)
{
    static const struct { FakeChipType type; long size; uint32_t jedec; bool ir; } cases[] = {
        { FAKE_EEPROM_TINY, 512, 0, false },
        { FAKE_EEPROM, 64 * KB, 0, false },
        { FAKE_EEPROM, 128 * KB, 0, true },
        { FAKE_FLASH, 256 * KB, 0x204012, false },
        { FAKE_FLASH, 512 * KB, 0x204013, true },
    };
    static uint8_t data[256 * 1024];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        fake_card_init(&fc, "ABCE", "TEST", cases[i].type, cases[i].size, cases[i].jedec, cases[i].ir);
        fake_card_slot(&fc, &slot);
        fill_chip(cases[i].size, 1); // Old data, so FLASH must erase first
        CardChip chip;
        CHECK(card_chip_detect(&slot.bus, cases[i].ir, &chip));
        long n = cases[i].size < (long)sizeof(data) ? cases[i].size : (long)sizeof(data);
        for (long j = 0; j < n; j++)
            data[j] = (j >= 4096 && j < 8192) ? 0xFF : (uint8_t)(j * 31 + 7);
        CHECK(card_chip_write(&slot.bus, &chip, 0, data, (uint32_t)n));
        CHECK(memcmp(fc.mem, data, (size_t)n) == 0);
        if (cases[i].type == FAKE_FLASH)
        {
            CHECK(fc.erases == n / CARD_FLASH_SECTOR);
            CHECK(fc.page_writes == (n - 4096) / 256); // 0xFF pages skipped
            // FLASH writes must start on a sector.
            CHECK(!card_chip_write(&slot.bus, &chip, 256, data, 256));
        }
    }

    // A blank 8 KB EEPROM: the size test finds 8 KB and leaves it blank.
    fake_card_init(&fc, "ABCE", "TEST", FAKE_EEPROM, 8 * KB, 0, false);
    fake_card_slot(&fc, &slot);
    CardChip chip;
    CHECK(card_chip_detect(&slot.bus, false, &chip) && chip.size_guessed);
    CHECK(card_chip_test_size(&slot.bus, &chip) && chip.size == 8 * KB && !chip.size_guessed);
    CHECK(fc.mem[0] == 0xFF);
    fake_card_init(&fc, "ABCE", "TEST", FAKE_EEPROM, 64 * KB, 0, false);
    fake_card_slot(&fc, &slot);
    CHECK(card_chip_detect(&slot.bus, false, &chip) && chip.size_guessed);
    CHECK(card_chip_test_size(&slot.bus, &chip) && chip.size == 64 * KB);
}

static void test_card_probe(void)
{
    new_card();
    make_dir("_pico");
    make_rom("roms/HeartGold.nds", "POKEMON HG", "IPKE", "Pokemon HeartGold");
    make_dir("_DSavE/saves/AMCE Mario Kart DS");
    library_scan(&lib, root);
    int roms = lib.count;
    CardInfo card;

    // No slot (DS mode): no card entry.
    card_probe(NULL, &card);
    CHECK(card.status == CARD_UNAVAILABLE);
    card_add_to_library(&lib, &card);
    CHECK(lib.count == roms && !lib.games[0].is_card);

    // Empty slot: an entry that says so.
    fake_card_init(&fc, "IPKE", "POKEMON HG", FAKE_FLASH, 512 * KB, 0x204013, true);
    fake_card_slot(&fc, &slot);
    fc.slot_status = CARD_EMPTY;
    card_probe(&slot, &card);
    CHECK(card.status == CARD_EMPTY);
    card_add_to_library(&lib, &card);
    CHECK(lib.count == roms + 1 && lib.games[0].is_card && !lib.games[0].has_save);
    CHECK(library_rom_count(&lib) == roms);

    // A card whose ROM is on the SD card takes the ROM's title.
    fc.slot_status = CARD_READY;
    card_probe(&slot, &card);
    CHECK(card.status == CARD_READY && strcmp(card.info.game_code, "IPKE") == 0);
    CHECK(card.chip.infrared && card.chip.size == 512 * KB);
    card_add_to_library(&lib, &card);
    CHECK(lib.count == roms + 1); // Replaced, not added twice
    Game *g = &lib.games[0];
    CHECK(g->is_card && g->has_save && g->save_size == 512 * KB);
    CHECK(strcmp(g->title, "Pokemon HeartGold") == 0);
    library_refresh_game(&lib, g); // Leaves a card alone
    CHECK(g->has_save && g->loc_count == 0);

    // One with only a backup folder takes the folder's title; else the header's.
    fake_card_init(&fc, "AMCE", "MARIOKARTDS", FAKE_EEPROM, 64 * KB, 0, false);
    card_probe(&slot, &card);
    card_add_to_library(&lib, &card);
    CHECK(strcmp(lib.games[0].title, "Mario Kart DS") == 0);
    fake_card_init(&fc, "AZEE", "ZELDA DS", FAKE_FLASH, 256 * KB, 0x204012, false);
    card_probe(&slot, &card);
    card_add_to_library(&lib, &card);
    CHECK(strcmp(lib.games[0].title, "ZELDA DS") == 0);

    // A bad header, NAND saves and no chip.
    fc.header[0] ^= 1;
    card_probe(&slot, &card);
    CHECK(card.status == CARD_UNREADABLE);
    fake_card_init(&fc, "UORE", "WARIOWAREDIY", FAKE_NONE, 0, 0, false);
    card_probe(&slot, &card);
    CHECK(card.status == CARD_NAND);
    fake_card_init(&fc, "ABCE", "NOSAVE", FAKE_NONE, 0, 0, false);
    card_probe(&slot, &card);
    CHECK(card.status == CARD_NO_CHIP);

    card_add_to_library(&lib, NULL);
    CHECK(lib.count == roms && !lib.games[0].is_card);
}

static void count_progress(void *ctx, long done, long total)
{
    long *last = ctx;
    if (done < last[0] || done > total)
        last[1] = 1; // Went backwards or past the end
    last[0] = done;
    last[2] = total;
}

static void test_card_backup_restore(void)
{
    new_card();
    make_dir("_pico");
    make_rom("roms/HeartGold.nds", "POKEMON HG", "IPKE", "Pokemon HeartGold");
    make_save("roms/HeartGold.sav", 512 * KB, 300 * KB, 9); // Flashcart copy
    library_scan(&lib, root);
    fake_card_init(&fc, "IPKE", "POKEMON HG", FAKE_FLASH, 512 * KB, 0x204013, true);
    fake_card_slot(&fc, &slot);
    fill_chip(512 * KB, 5);
    CardInfo card;
    card_probe(&slot, &card);
    card_add_to_library(&lib, &card);
    Game *g = &lib.games[0];

    // Back up: the backup lands in the ROM's folder.
    long prog[3] = { 0, 0, 0 };
    CardProgress cp = { prog, count_progress };
    Backup b;
    CHECK(card_backup(&lib, &slot, g, NULL, false, &b, &cp) == DSAVE_OK);
    CHECK(b.size == 512 * KB && prog[0] == prog[2] && prog[1] == 0);
    char tmp[DSAVE_PATH_MAX];
    p(tmp, "_DSavE/.card.tmp");
    CHECK(!file_exists(tmp));
    Game *rom = find_game("IPKE");
    rom = rom == g ? &lib.games[1] : rom;
    Backup list[BACKUP_LIST_MAX];
    CHECK(backup_list(&lib, rom, list, BACKUP_LIST_MAX) == 1);
    CHECK(list[0].crc32 == crc32_update(0, fc.mem, 512 * KB));

    // Restore over changed data: only changed sectors are rewritten.
    fill_chip(64 * KB, 6);
    Backup safety;
    prog[0] = 0;
    CHECK(card_restore(&lib, &slot, g, &b, &safety, &cp) == DSAVE_OK);
    CHECK(chip_is(512 * KB, 5) && fc.erases == 1);
    CHECK(safety.is_auto && prog[0] == prog[2] && prog[1] == 0);
    CHECK(backup_list(&lib, g, list, BACKUP_LIST_MAX) == 2);
    int writes = fc.page_writes;
    CHECK(card_restore(&lib, &slot, g, &b, NULL, NULL) == DSAVE_OK);
    CHECK(fc.page_writes == writes); // Nothing to change

    // The flashcart's padded save goes onto the card too.
    Game *sd = &lib.games[1];
    Backup from_sd;
    CHECK(backup_create(&lib, sd, "flashcart", false, &from_sd) == DSAVE_OK);
    CHECK(card_restore(&lib, &slot, g, &from_sd, NULL, NULL) == DSAVE_OK);
    char sav[DSAVE_PATH_MAX];
    p(sav, "roms/HeartGold.sav");
    CHECK(crc32_update(0, fc.mem, 512 * KB) == crc_of(sav));

    // Swapped card, loose card, read-only chip, write failure.
    memcpy(fc.header + 0x0C, "IPGE", 4);
    fake_card_init(&fc, "IPGE", "POKEMON SS", FAKE_FLASH, 512 * KB, 0x204013, true);
    CHECK(card_backup(&lib, &slot, g, NULL, true, NULL, NULL) == DSAVE_ERR_CARD_CHANGED);
    CHECK(card_restore(&lib, &slot, g, &b, NULL, NULL) == DSAVE_ERR_CARD_CHANGED);
    fake_card_init(&fc, "IPKE", "POKEMON HG", FAKE_FLASH, 512 * KB, 0x204013, true);
    fill_chip(512 * KB, 5);
    fc.flaky_read = 9;
    CHECK(card_backup(&lib, &slot, g, NULL, true, NULL, NULL) == DSAVE_ERR_CARD);
    fc.flaky_read = 0;
    fc.protect = true;
    fill_chip(512 * KB, 7);
    CHECK(card_restore(&lib, &slot, g, &b, NULL, NULL) == DSAVE_ERR_ROLLED_BACK);
    CHECK(chip_is(512 * KB, 7));
    fake_card_init(&fc, "IPKE", "POKEMON HG", FAKE_FLASH, 512 * KB, 0x654321, true);
    CHECK(card_restore(&lib, &slot, g, &b, NULL, NULL) == DSAVE_ERR_UNSUPPORTED);

    // A 512 KB backup with data past 256 KB can't go on a 256 KB chip.
    fake_card_init(&fc, "IPKE", "POKEMON HG", FAKE_FLASH, 256 * KB, 0x204012, true);
    CHECK(card_restore(&lib, &slot, g, &b, NULL, NULL) == DSAVE_ERR_BAD_SIZE);

    // Blank 8 KB EEPROM: the restore settles the size first.
    new_card();
    library_scan(&lib, root);
    fake_card_init(&fc, "ABCE", "SMALL", FAKE_EEPROM, 8 * KB, 0, false);
    card_probe(&slot, &card);
    card_add_to_library(&lib, &card);
    g = &lib.games[0];
    CHECK(card_backup(&lib, &slot, g, NULL, false, NULL, NULL) == DSAVE_WARN_BLANK);
    make_dir("_DSavE/import");
    make_save("_DSavE/import/small.sav", 8 * KB, 8 * KB, 3);
    char imp[DSAVE_PATH_MAX];
    p(imp, "_DSavE/import/small.sav");
    CHECK(backup_import(&lib, g, imp, &b) == DSAVE_OK);
    CHECK(card_restore(&lib, &slot, g, &b, NULL, NULL) == DSAVE_OK);
    CHECK(crc32_update(0, fc.mem, 8 * KB) == crc_of(imp));
}

// no$gba's run-length packing, written the way no$gba does: long runs as
// 80 with a 16-bit count, short runs as 81..FF, the rest copied. count_first
// writes the 80 run's count before its byte, as older readers expect.
static long nocash_pack(const uint8_t *in, long n, uint8_t *out, bool count_first)
{
    long o = 0, i = 0, lit = -1;
    while (i < n)
    {
        long run = 1;
        while (i + run < n && in[i + run] == in[i] && run < 0xFFFF)
            run++;
        if (run >= 3)
        {
            lit = -1;
            if (run > 0x7F)
            {
                out[o++] = 0x80;
                if (count_first)
                {
                    out[o++] = (uint8_t)run;
                    out[o++] = (uint8_t)(run >> 8);
                    out[o++] = in[i];
                }
                else
                {
                    out[o++] = in[i];
                    out[o++] = (uint8_t)run;
                    out[o++] = (uint8_t)(run >> 8);
                }
            }
            else
            {
                out[o++] = (uint8_t)(0x80 + run);
                out[o++] = in[i];
            }
            i += run;
            continue;
        }
        if (lit < 0 || out[lit] == 0x7F)
        {
            lit = o;
            out[o++] = 0;
        }
        out[lit]++;
        out[o++] = in[i++];
    }
    out[o++] = 0;
    return o;
}

// A no$gba save file: the header, then the data packed or not.
static long nocash_file(const uint8_t *data, long n, int method, bool count_first, uint8_t *out)
{
    memset(out, 0, 0x50);
    memcpy(out, "NocashGbaBackupMediaSavDataFile", 31);
    out[0x1F] = 0x1A;
    memcpy(out + 0x40, "SRAM", 4);
    put_le32(out + 0x44, (uint32_t)method);
    if (method == 0)
    {
        put_le32(out + 0x48, (uint32_t)n);
        memcpy(out + 0x4C, data, (size_t)n);
        return 0x4C + n;
    }
    long packed = nocash_pack(data, n, out + 0x50, count_first);
    put_le32(out + 0x48, (uint32_t)packed);
    put_le32(out + 0x4C, (uint32_t)n);
    return 0x50 + packed;
}

// A card pulled out mid-operation: DSavE must stop, say so, and never claim
// the old save was put back when it wasn't.
static void test_card_pulled(void)
{
    new_card();
    make_dir("_pico");
    make_rom("roms/HG.nds", "POKEMON HG", "IPKE", "Pokemon HeartGold");
    library_scan(&lib, root);
    fake_card_init(&fc, "IPKE", "POKEMON HG", FAKE_FLASH, 512 * KB, 0x204013, true);
    fake_card_slot(&fc, &slot);
    fill_chip(512 * KB, 5);
    CardInfo card;
    card_probe(&slot, &card);
    card_add_to_library(&lib, &card);
    Game *g = &lib.games[0];
    Backup b;
    CHECK(card_backup(&lib, &slot, g, "keep", false, &b, NULL) == DSAVE_OK);

    // Pulled right after the first sector erase of a restore.
    fill_chip(512 * KB, 7);
    fc.waited_us = 0;
    fc.remove_after_erases = fc.erases + 1;
    Backup safety;
    CHECK(card_restore(&lib, &slot, g, &b, &safety, NULL) == DSAVE_ERR_ROLLBACK_FAILED);
    CHECK(fc.waited_us < 1000000); // Stops at once, not after the chip timeouts
    CHECK(safety.is_auto);
    fc.removed = false;
    fc.remove_after_erases = 0;
    fill_chip(512 * KB, 7);
    CHECK(safety.crc32 == crc32_update(0, fc.mem, 512 * KB)); // The auto backup holds the old save

    // Pulled after the slot was re-opened but before the save was read: not
    // a "blank save", and no backup is made.
    Backup list[BACKUP_LIST_MAX];
    int before = backup_list(&lib, g, list, BACKUP_LIST_MAX);
    fc.remove_after_reads = fc.reads + 1;
    CHECK(card_backup(&lib, &slot, g, "pulled", false, NULL, NULL) == DSAVE_ERR_CARD_CHANGED);
    fc.removed = false;
    fc.remove_after_reads = fc.reads + 1;
    CHECK(card_backup(&lib, &slot, g, "pulled", true, NULL, NULL) == DSAVE_ERR_CARD_CHANGED);
    fc.removed = false;
    fc.remove_after_reads = 0;
    CHECK(backup_list(&lib, g, list, BACKUP_LIST_MAX) == before);
    char tmp[DSAVE_PATH_MAX];
    p(tmp, "_DSavE/.card.tmp");
    CHECK(!file_exists(tmp));

    // A chip that ignores writes still holds its old save, and says so.
    fill_chip(512 * KB, 7);
    fc.protect = true;
    CHECK(card_restore(&lib, &slot, g, &b, NULL, NULL) == DSAVE_ERR_ROLLED_BACK);
    fc.protect = false;
}

static void limit_file_size(rlim_t n)
{
    struct rlimit rl;
    getrlimit(RLIMIT_FSIZE, &rl);
    rl.rlim_cur = n;
    setrlimit(RLIMIT_FSIZE, &rl);
}

// The SD card fills up partway through a restore.
static void test_restore_disk_full(void)
{
    signal(SIGXFSZ, SIG_IGN);
    new_card();
    make_dir("_pico");
    make_rom("roms/G.nds", "GAME", "ABCE", "Game");
    make_rom("roms/H.nds", "GAME2", "ABDE", "Game 2");
    make_save("roms/G.sav", 256 * KB, 256 * KB, 1);
    make_save("other/big.sav", 512 * KB, 512 * KB, 2);
    library_scan(&lib, root);
    char big[DSAVE_PATH_MAX], live[DSAVE_PATH_MAX], fresh[DSAVE_PATH_MAX];
    p(big, "other/big.sav");
    p(live, "roms/G.sav");
    p(fresh, "roms/H.sav");
    uint32_t old_crc = crc_of(live);

    // A bigger backup over a smaller save: the old save comes back at its
    // old size, not with the new data hanging off the end.
    Game *g = find_game("ABCE");
    Backup b, safety;
    CHECK(backup_create_from(&lib, g, big, "big", false, &b) == DSAVE_OK);
    limit_file_size(400 * KB);
    CHECK(backup_restore(&lib, g, &b, &safety) == DSAVE_ERR_ROLLED_BACK);
    limit_file_size(RLIM_INFINITY);
    CHECK(file_size(live) == 256 * KB && crc_of(live) == old_crc);

    // A first save that doesn't fit leaves nothing for the kernel to load.
    Game *h = find_game("ABDE");
    Backup hb;
    CHECK(backup_create_from(&lib, h, big, "big", false, &hb) == DSAVE_OK);
    limit_file_size(100 * KB);
    CHECK(backup_restore(&lib, h, &hb, &safety) == DSAVE_ERR_IO);
    limit_file_size(RLIM_INFINITY);
    CHECK(!file_exists(fresh) && !h->has_save);
    CHECK(backup_restore(&lib, h, &hb, &safety) == DSAVE_OK && crc_of(fresh) == crc_of(big));
}

// More backups than a list holds: the list keeps the newest, whatever order
// the folders come back in.
static void test_backup_list_newest(void)
{
    new_card();
    make_dir("_pico");
    make_rom("roms/G.nds", "GAME", "ABCE", "Game");
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    for (int i = 0; i < BACKUP_LIST_MAX + 16; i++)
    {
        // Names run against the dates, so name order is no help either.
        char rel[DSAVE_PATH_MAX], ini[160];
        snprintf(rel, sizeof(rel), "_DSavE/saves/ABCE Game/b%03d/Game.sav", 999 - i);
        make_file(rel, 8 * KB, (uint8_t)i);
        snprintf(rel, sizeof(rel), "_DSavE/saves/ABCE Game/b%03d/backup.ini", 999 - i);
        snprintf(ini, sizeof(ini), "code=ABCE\nsize=8192\ncrc32=0\ncreated=2026-01-%02d_%02d-00-00\n", 1 + i / 24, i % 24);
        make_text(rel, ini);
    }
    static Backup list[BACKUP_LIST_MAX];
    CHECK(backup_list(&lib, g, list, BACKUP_LIST_MAX) == BACKUP_LIST_MAX);
    CHECK(strcmp(list[0].name, "b920") == 0); // The newest, i = 79
    CHECK(strcmp(list[BACKUP_LIST_MAX - 1].name, "b983") == 0); // i = 16
    Backup few[3];
    CHECK(backup_list(&lib, g, few, 3) == 3 && strcmp(few[2].name, "b922") == 0);
}

// no$gba headers with absurd sizes are refused before any size arithmetic.
static void test_nocash_huge_header(void)
{
    new_card();
    static uint8_t file[0x60], data[16];
    long n = nocash_file(data, sizeof(data), 1, false, file);
    put_le32(file + 0x4C, 0x7FFFFFFF); // Unpacked size
    write_bytes("huge.sav", file, n);
    char path[DSAVE_PATH_MAX];
    p(path, "huge.sav");
    EmuSave es;
    CHECK(emusave_probe(path, &es) && es.format == EMU_NOCASH && es.length == -1);
    put_le32(file + 0x44, 0);
    put_le32(file + 0x48, 0xFFFFFFF0); // Raw size
    write_bytes("huge.sav", file, n);
    CHECK(emusave_probe(path, &es) && es.length == -1);
}

static void test_emulator_formats(void)
{
    new_card();
    make_dir("_pico");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_rom("roms/Other.nds", "OTHER", "XYZE", NULL);
    make_rom("a/Twin.nds", "TWIN", "TW1E", NULL);
    make_rom("b/Twin.nds", "TWIN", "TW2E", NULL);
    library_scan(&lib, root);
    Game *g = find_game("ABCE");
    if (!g)
        return;

    // 64 KB with copied bytes, short runs, long runs and 0xFF padding.
    enum { SIZE = 64 * KB };
    static uint8_t data[SIZE], file[SIZE * 2 + 0x100];
    for (long i = 0; i < SIZE; i++)
        data[i] = i < 900 ? (uint8_t)(i * 37 + (i >> 3)) : i < 1300 ? 0x00 : i < 1305 ? 0xAA
                : i < 3000 ? (uint8_t)(i ^ (i >> 5)) : 0xFF;
    uint32_t want = crc32_update(0, data, SIZE);

    long n = nocash_file(data, SIZE, 0, false, file);
    write_bytes("_DSavE/import/a raw nocash.sav", file, n);
    n = nocash_file(data, SIZE, 1, false, file);
    CHECK(n < SIZE / 4); // Really packed
    write_bytes("_DSavE/import/b packed.sav", file, n);
    n = nocash_file(data, SIZE, 1, true, file);
    write_bytes("_DSavE/import/c packed old.sav", file, n);
    // Damaged: the end marker and the last runs are lost.
    write_bytes("_DSavE/import/d damaged.sav", file, n - 4);
    n = nocash_file(data, SIZE, 1, false, file);
    file[n - 1] = 0x05; // End marker replaced by a copy past the end
    write_bytes("_DSavE/import/e bad end.sav", file, n);

    // Action Replay version 1 (.duc) and 2 (.dss).
    memset(file, 0, 500);
    memcpy(file, "ARDS000000000001", 16);
    memcpy(file + 500, data, SIZE);
    write_bytes("_DSavE/import/f ar.duc", file, 500 + SIZE);
    memset(file, 0, 0xA4);
    file[0xA1] = (uint8_t)(SIZE >> 8);
    file[0xA2] = (uint8_t)(SIZE >> 16);
    file[0xA3] = 0xC0;
    memcpy(file + 0xA4, data, SIZE);
    write_bytes("_DSavE/import/g ar.dss", file, 0xA4 + SIZE);
    file[0xA1] = 1; // Size no longer matches the file
    write_bytes("_DSavE/import/h ar bad.dss", file, 0xA4 + SIZE);
    // The same bytes as a .sav are just a raw file of an odd size.
    write_bytes("_DSavE/import/i not ar.sav", file, 0xA4 + SIZE);

    ImportFile files[IMPORT_LIST_MAX];
    int count = backup_import_list(&lib, files, IMPORT_LIST_MAX);
    CHECK(count == 9);
    if (count != 9)
        return;
    static const struct {
        EmuFormat format;
        bool valid;
        DsaveResult result;
    } expect[9] = {
        { EMU_NOCASH, true, DSAVE_OK },
        { EMU_NOCASH, true, DSAVE_OK },
        { EMU_NOCASH, true, DSAVE_OK },
        { EMU_NOCASH, false, DSAVE_ERR_DAMAGED },
        { EMU_NOCASH, true, DSAVE_ERR_DAMAGED },
        { EMU_ACTION_REPLAY, true, DSAVE_OK },
        { EMU_ACTION_REPLAY, true, DSAVE_OK },
        { EMU_ACTION_REPLAY, false, DSAVE_ERR_DAMAGED },
        { EMU_RAW, false, DSAVE_ERR_BAD_SIZE },
    };
    for (int i = 0; i < count; i++)
    {
        Backup b;
        DsaveResult r = backup_import(&lib, g, files[i].path, &b);
        CHECK(files[i].format == expect[i].format);
        CHECK(files[i].valid == expect[i].valid);
        CHECK(files[i].game == -1);
        CHECK(r == expect[i].result);
        if (r == DSAVE_OK)
            CHECK(b.size == SIZE && b.crc32 == want && backup_validate(&b) == DSAVE_OK);
        if (r != expect[i].result)
            printf("  (%s gave %s)\n", files[i].name, dsave_result_str(r));
    }
    CHECK(strcmp(emusave_format_name(EMU_NOCASH), "no$gba") == 0 && !emusave_format_name(EMU_RAW)[0]);
    char tmp[DSAVE_PATH_MAX];
    p(tmp, "_DSavE/.import.tmp");
    CHECK(!file_exists(tmp));

    // .dsv for each save size DeSmuME has a type for, and 128 KB, which it
    // detects from the address size.
    static const struct {
        long size;
        uint32_t type, addr;
    } dsv[] = { { 512, 1, 1 }, { 8 * KB, 2, 2 }, { 32 * KB, 4, 2 }, { 64 * KB, 3, 2 }, { 128 * KB, 0, 3 },
                { 256 * KB, 5, 3 }, { 512 * KB, 6, 3 }, { 1024 * KB, 7, 3 }, { 8192 * KB, 10, 3 } };
    for (size_t i = 0; i < sizeof(dsv) / sizeof(dsv[0]); i++)
    {
        uint8_t footer[EMUSAVE_DSV_FOOTER_MAX];
        int len = emusave_dsv_footer(dsv[i].size, footer);
        CHECK(len == 82 + 24 + 16);
        CHECK(le32(footer + 82 + 8) == dsv[i].type && le32(footer + 82 + 12) == dsv[i].addr);
    }

    // Exported .dsv files import back to the same data.
    make_save("Game.sav", 512 * KB, 9 * KB, 33);
    library_refresh_game(&lib, g);
    Backup b, back;
    CHECK(backup_create(&lib, g, "for desmume", false, &b) == DSAVE_OK);
    char out[DSAVE_PATH_MAX];
    CHECK(backup_export(&lib, g, &b, EXPORT_DSV, out, sizeof(out)) == DSAVE_OK);
    CHECK(backup_import(&lib, g, out, &back) == DSAVE_OK);
    CHECK(back.size == b.size && back.crc32 == b.crc32);

    // A save named after a ROM is matched to it: case, ".nds.sav" and .dsv
    // all count, a name two ROMs share does not.
    new_card();
    make_dir("_pico");
    make_rom("Game.nds", "GAME", "ABCE", NULL);
    make_rom("roms/Other.nds", "OTHER", "XYZE", NULL);
    make_rom("a/Twin.nds", "TWIN", "TW1E", NULL);
    make_rom("b/Twin.nds", "TWIN", "TW2E", NULL);
    library_scan(&lib, root);
    make_save("_DSavE/import/game.nds.sav", 8 * KB, 8 * KB, 1);
    make_save("_DSavE/import/Other.dsv", 8 * KB, 8 * KB, 2);
    make_save("_DSavE/import/Other.sav.gz.txt", 8 * KB, 8 * KB, 2);
    make_save("_DSavE/import/Twin.sav", 8 * KB, 8 * KB, 3);
    make_save("_DSavE/import/Unknown.duc", 8 * KB, 8 * KB, 4);
    count = backup_import_list(&lib, files, IMPORT_LIST_MAX);
    CHECK(count == 4);
    if (count != 4)
        return;
    Game *other = find_game("XYZE");
    CHECK(strcmp(files[0].name, "game.nds.sav") == 0 && files[0].game == find_game("ABCE") - lib.games);
    CHECK(strcmp(files[1].name, "Other.dsv") == 0 && files[1].game == other - lib.games);
    CHECK(strcmp(files[2].name, "Twin.sav") == 0 && files[2].game == -1);
    // Not really an Action Replay file, so it reads as damaged.
    CHECK(strcmp(files[3].name, "Unknown.duc") == 0 && files[3].game == -1 && !files[3].valid);
    CHECK(backup_import(&lib, other, files[1].path, &b) == DSAVE_OK && strcmp(b.name, "import_Other") == 0);
    CHECK(backup_import(&lib, other, files[0].path, &b) == DSAVE_OK && strcmp(b.name, "import_game") == 0);
}

int main(void)
{
    char tmpl[] = "/tmp/dsave-test-XXXXXX";
    if (!mkdtemp(tmpl))
        return 2;
    snprintf(base, sizeof(base), "%s", tmpl);

    test_util();
    test_ini();
    test_kernel_detection();
    test_kernel_paths();
    test_savelist();
    test_scan();
    test_multi_kernel_card();
    test_backup_cycle();
    test_restore_guards();
    test_restore_sizes();
    test_export_trim();
    test_emulator_formats();
    test_restore_to_other_kernel();
    test_blank_and_first_save();
    test_sync_check();
    test_sync_padding_edge();
    test_sync_copy_and_all();
    test_slots();
    test_compression();
    test_titles();
    test_import();
    test_prune_auto();
    test_note_and_keep();
    test_activity();
    test_icon();
    test_card_detect();
    test_card_write();
    test_card_probe();
    test_card_backup_restore();
    test_card_pulled();
    test_backup_list_newest();
    test_nocash_huge_header();
    test_restore_disk_full();

    library_free(&lib);
    remove_tree(tmpl);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
