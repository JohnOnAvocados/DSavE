// Host-side tests for the save engine. Each test builds a fake SD card in a
// temp folder, laid out the way a kernel would leave it, and drives the same
// code the DS runs.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../source/core/backup.h"
#include "../source/core/ini.h"
#include "../source/core/kernel.h"
#include "../source/core/library.h"
#include "../source/core/rom.h"
#include "../source/core/util.h"

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
    CHECK(strcmp(lib.games[0].title, "CASTLEVANIA1") == 0);
    CHECK(strcmp(lib.games[1].title, "CHRONO TRIGGER") == 0);
    CHECK(strcmp(lib.games[2].title, "POKEMON SS") == 0);

    Game *ct = find_game("YQUE");
    CHECK(ct && ct->has_save && ct->save_size == 64 * KB && ct->loc_count == 1);
    Game *pk = find_game("IPGE");
    CHECK(pk && !pk->has_save && pk->save_size == -1);
    CHECK(pk && path_is(pk->save_path, "roms/Pokemon_SoulSilver.sav"));
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

    // Export writes an exact-size raw copy.
    char exported[DSAVE_PATH_MAX];
    CHECK(backup_export(&lib, ct, &b1, exported, sizeof(exported)) == DSAVE_OK);
    CHECK(path_is(exported, "_DSavE/export/CHRONO TRIGGER_YQUE_Before final boss.sav"));
    CHECK(file_size(exported) == 64 * KB && crc_of(exported) == original);

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
    CHECK(backup_export(&lib, ct, &b, out, sizeof(out)) == DSAVE_OK);
    CHECK(file_size(out) == 64 * KB);
    CHECK(crc_of(out) == crc_prefix(ct->save_path, 64 * KB));

    // Data past the chip size is kept rather than cut off.
    make_save("Chrono Trigger.sav", 512 * KB, 100 * KB, 7);
    library_refresh_game(&lib, ct);
    CHECK(backup_create(&lib, ct, "odd", false, &b) == DSAVE_OK);
    CHECK(backup_export(&lib, ct, &b, out, sizeof(out)) == DSAVE_OK);
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
    test_restore_to_other_kernel();
    test_blank_and_first_save();

    library_free(&lib);
    remove_tree(tmpl);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
