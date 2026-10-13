// Host-side tests for the save engine. They build a fake SD card in a temp
// folder and drive the same code the DS runs.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../source/core/backup.h"
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

static char root[DSAVE_PATH_MAX];
static Library lib;

static void p(char *out, const char *rel)
{
    path_join(out, DSAVE_PATH_MAX, root, rel);
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

static void make_file(const char *rel, long size, uint8_t seed)
{
    char path[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    p(path, rel);
    path_dirname(dir, sizeof(dir), path);
    mkdir_p(dir);
    FILE *f = fopen(path, "wb");
    for (long i = 0; i < size; i++)
        fputc((uint8_t)(seed + i * 7), f);
    fclose(f);
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
    CHECK(is_valid_save_size(64 * 1024));
    CHECK(is_valid_save_size(8 * 1024 * 1024));
    CHECK(!is_valid_save_size(500));
    CHECK(!is_valid_save_size(520 * 1024));
    CHECK(!is_valid_save_size(16 * 1024 * 1024));

    CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u);
}

static void test_scan_generic(void)
{
    // A Wood R4 card: saves sit beside ROMs.
    mkdir_p(root);
    char d[DSAVE_PATH_MAX];
    p(d, "__rpg");
    mkdir_p(d);
    make_rom("Chrono Trigger.nds", "CHRONOTRIGGE", "YQUE", "CHRONO TRIGGER");
    make_file("Chrono Trigger.sav", 64 * 1024, 1);
    make_rom("roms/rpg/Castlevania_DoS.nds", "CASTLEVANIA1", "ACVE", NULL);
    make_file("roms/rpg/Castlevania_DoS.sav", 64 * 1024, 2);
    make_rom("roms/Pokemon_SoulSilver.nds", "POKEMON SS", "IPGE", "POKEMON SS");
    make_rom("homebrew/GodMode9i.nds", "GODMODE9I", "####", NULL);
    make_rom("_nds/hidden.nds", "HIDDEN", "HIDE", NULL);

    library_scan(&lib, root);
    CHECK(lib.loader == LOADER_WOOD);
    CHECK(lib.count == 3); // homebrew and _nds skipped

    // Sorted by title.
    CHECK(strcmp(lib.games[0].title, "CASTLEVANIA1") == 0);
    CHECK(strcmp(lib.games[1].title, "CHRONO TRIGGER") == 0);
    CHECK(strcmp(lib.games[2].title, "POKEMON SS") == 0);

    Game *ct = find_game("YQUE");
    CHECK(ct && ct->has_save && ct->save_size == 64 * 1024);
    Game *pk = find_game("IPGE");
    CHECK(pk && !pk->has_save && pk->save_size == -1);
    char expect[DSAVE_PATH_MAX];
    p(expect, "roms/Pokemon_SoulSilver.sav");
    CHECK(pk && strcmp(pk->save_path, expect) == 0);
}

static void test_twilight_paths(void)
{
    char tw[DSAVE_PATH_MAX], rom[DSAVE_PATH_MAX], out[DSAVE_PATH_MAX], expect[DSAVE_PATH_MAX];
    p(tw, "_nds/TWiLightMenu");
    mkdir_p(tw);
    CHECK(library_detect_loader(root) == LOADER_TWILIGHT);

    // No save yet: TWiLight would create it under saves/.
    make_rom("roms/nds/Mario Kart DS.nds", "MARIOKART DS", "AMCE", NULL);
    p(rom, "roms/nds/Mario Kart DS.nds");
    CHECK(!library_resolve_save(LOADER_TWILIGHT, rom, out, sizeof(out)));
    p(expect, "roms/nds/saves/Mario Kart DS.sav");
    CHECK(strcmp(out, expect) == 0);

    // A save left beside the ROM by an older kernel is still found.
    make_file("roms/nds/Mario Kart DS.sav", 512 * 1024, 3);
    CHECK(library_resolve_save(LOADER_TWILIGHT, rom, out, sizeof(out)));
    p(expect, "roms/nds/Mario Kart DS.sav");
    CHECK(strcmp(out, expect) == 0);

    // When both exist, TWiLight's own saves/ copy wins.
    make_file("roms/nds/saves/Mario Kart DS.sav", 512 * 1024, 4);
    CHECK(library_resolve_save(LOADER_TWILIGHT, rom, out, sizeof(out)));
    p(expect, "roms/nds/saves/Mario Kart DS.sav");
    CHECK(strcmp(out, expect) == 0);

    remove_tree(tw);
    char nds[DSAVE_PATH_MAX];
    p(nds, "_nds");
    remove_tree(nds);
}

static void test_backup_cycle(void)
{
    library_scan(&lib, root);
    Game *ct = find_game("YQUE");
    CHECK(ct != NULL);
    if (!ct)
        return;
    uint32_t original = crc_of(ct->save_path);

    // Named backup.
    Backup b1;
    CHECK(backup_create(&lib, ct, "Before final boss", false, &b1) == DSAVE_OK);
    CHECK(strcmp(b1.name, "Before final boss") == 0);
    CHECK(file_exists(b1.file));
    CHECK(crc_of(b1.file) == original);
    char expect[DSAVE_PATH_MAX];
    p(expect, "_DSavE/saves/YQUE CHRONO TRIGGER/Before final boss/CHRONO TRIGGER.sav");
    CHECK(strcmp(b1.file, expect) == 0);

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
    make_file("Chrono Trigger.sav", 64 * 1024, 99);
    library_refresh_game(&lib, ct);
    CHECK(crc_of(ct->save_path) != original);
    uint32_t changed = crc_of(ct->save_path);

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
    p(expect, "_DSavE/export/CHRONO TRIGGER_YQUE_Before final boss.sav");
    CHECK(strcmp(exported, expect) == 0);
    CHECK(file_size(exported) == 64 * 1024 && crc_of(exported) == original);

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

    // Size mismatch is refused.
    make_file("Chrono Trigger.sav", 512 * 1024, 5);
    library_refresh_game(&lib, ct);
    Backup list[BACKUP_LIST_MAX];
    int n = backup_list(&lib, ct, list, BACKUP_LIST_MAX);
    bool refused = false;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(list[i].name, "Before final boss") == 0)
            refused = backup_restore(&lib, ct, &list[i], NULL) == DSAVE_ERR_SIZE_MISMATCH;
    }
    CHECK(refused);
    CHECK(file_size(ct->save_path) == 512 * 1024);
}

static void test_blank_and_first_save(void)
{
    library_scan(&lib, root);
    Game *cv = find_game("ACVE");
    Game *pk = find_game("IPGE");
    CHECK(cv && pk);
    if (!cv || !pk)
        return;

    make_blank("roms/rpg/Castlevania_DoS.sav", 64 * 1024, 0xFF);
    Backup b;
    CHECK(backup_create(&lib, cv, "blank", false, &b) == DSAVE_WARN_BLANK);
    CHECK(backup_create(&lib, cv, "blank", true, &b) == DSAVE_OK);

    // No save: backup is refused, restore creates the file.
    CHECK(backup_create(&lib, pk, NULL, false, &b) == DSAVE_ERR_NO_SAVE);

    make_file("import.sav", 512 * 1024, 7);
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
    CHECK(!list[0].has_meta && list[0].size == 512 * 1024);
    CHECK(backup_restore(&lib, pk, &list[0], NULL) == DSAVE_OK);
    CHECK(pk->has_save && pk->save_size == 512 * 1024);
    CHECK(crc_of(pk->save_path) == crc_of(src));
}

int main(void)
{
    char tmpl[] = "/tmp/dsave-test-XXXXXX";
    if (!mkdtemp(tmpl))
        return 2;
    snprintf(root, sizeof(root), "%s/sd", tmpl);

    test_util();
    test_scan_generic();
    test_twilight_paths();
    test_backup_cycle();
    test_restore_guards();
    test_blank_and_first_save();

    remove_tree(tmpl);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
