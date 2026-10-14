// Drives the UI (source/ui/app.c) on a PC against fake SD cards: scripted
// button presses and stylus taps, checks on the result, and a PNG of both
// screens after each step in tests/screenshots/.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/core/util.h"
#include "../source/ui/app.h"

#include "fake_card.h"

#include "../source/third_party/miniz/miniz_config.h"
#include "../source/third_party/miniz/miniz.h"

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
static int card_no = 0;

static Library lib;
static App app;
static Color top_px[SCREEN_W * SCREEN_H], bottom_px[SCREEN_W * SCREEN_H];
static Canvas top, bottom;
static CardSlot *card_slot; // NULL: DS mode, no slot 1

// ---------------------------------------------------------------------------
// Fake SD card

static void new_card(void)
{
    snprintf(root, sizeof(root), "%s/card%d/", base, ++card_no);
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

static FILE *create(const char *rel)
{
    char path[DSAVE_PATH_MAX], dir[DSAVE_PATH_MAX];
    p(path, rel);
    path_dirname(dir, sizeof(dir), path);
    mkdir_p(dir);
    return fopen(path, "wb");
}

// A banner at rom + offset: the English title (Latin-1, so "\xe9" is é)
// and an icon, a ring in a colour picked from the game code on a dark
// square.
static void make_banner(uint8_t *rom, uint32_t offset, const char *code, const char *banner)
{
    rom[0x68] = (uint8_t)offset;
    rom[0x69] = (uint8_t)(offset >> 8);
    uint8_t *en = rom + offset + 0x240 + 0x100;
    for (size_t i = 0; banner[i]; i++)
        en[i * 2] = (uint8_t)banner[i];

    uint8_t *bitmap = rom + offset + 0x20, *pal = rom + offset + 0x220;
    unsigned hue = (unsigned)(code[0] * 7 + code[1] * 13 + code[2] * 3) % 6;
    static const uint16_t rings[6] = { 0x001F, 0x03E0, 0x7C00, 0x03FF, 0x7C1F, 0x7FE0 };
    uint16_t colours[4] = { 0, 0x1084, rings[hue], 0x7FFF };
    for (int i = 0; i < 4; i++)
    {
        pal[i * 2] = (uint8_t)colours[i];
        pal[i * 2 + 1] = (uint8_t)(colours[i] >> 8);
    }
    for (int y = 0; y < 32; y++)
    {
        for (int x = 0; x < 32; x++)
        {
            int dx = x * 2 - 31, dy = y * 2 - 31, d = dx * dx + dy * dy;
            int v = (x < 2 || y < 2 || x > 29 || y > 29) ? 0 : d < 300 ? 3 : d < 700 ? 2 : 1;
            int in = (y % 8) * 8 + x % 8, tile = (y / 8) * 4 + x / 8;
            bitmap[tile * 32 + in / 2] |= (uint8_t)((in & 1) ? v << 4 : v);
        }
    }
}

static void make_rom(const char *rel, const char *code, const char *banner)
{
    static uint8_t rom[0x200 + 0xA00];
    memset(rom, 0, sizeof(rom));
    memcpy(rom, "GAME", 4);
    memcpy(rom + 0x0C, code, 4);
    make_banner(rom, 0x200, code, banner);
    FILE *f = create(rel);
    fwrite(rom, 1, sizeof(rom), f);
    fclose(f);
}

static void put_le32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(v >> (8 * i));
}

// A DSiWare ROM: unit code 3, title ID 00030004, the save sizes and the
// banner save flag in its DSi header.
static void make_dsi_rom(const char *rel, const char *code, const char *banner, uint32_t pub, uint32_t prv)
{
    static uint8_t rom[0x1000 + 0xA00];
    memset(rom, 0, sizeof(rom));
    memcpy(rom, "DSIWARE", 7);
    memcpy(rom + 0x0C, code, 4);
    rom[0x12] = 0x03;
    rom[0x1BF] = 0x04;
    put_le32(rom + 0x234, 0x00030004);
    put_le32(rom + 0x238, pub);
    put_le32(rom + 0x23C, prv);
    make_banner(rom, 0x1000, code, banner);
    FILE *f = create(rel);
    fwrite(rom, 1, sizeof(rom), f);
    fclose(f);
}

static void make_save(const char *rel, long size, long data, uint8_t seed)
{
    FILE *f = create(rel);
    for (long i = 0; i < size; i++)
        fputc(i < data ? (uint8_t)(seed + i * 7 + (i >> 8)) : 0xFF, f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Driving the UI

static void frame(uint32_t down, uint32_t held, bool touching, int tx, int ty)
{
    UiInput in = { down, held, down, touching, tx, ty };
    app_update(&app, &in);
    app_render(&app, &top, &bottom);
}

// Let queued work run.
static void settle(void)
{
    for (int i = 0; i < 4; i++)
        frame(0, 0, false, 0, 0);
}

static void press(uint32_t keys)
{
    frame(keys, keys, false, 0, 0);
    frame(0, 0, false, 0, 0);
    settle();
}

static void hold(uint32_t keys, int frames)
{
    frame(keys, keys, false, 0, 0);
    for (int i = 1; i < frames; i++)
        frame(0, keys, false, 0, 0);
    frame(0, 0, false, 0, 0);
    settle();
}

static void tap(int x, int y)
{
    frame(0, 0, true, x, y);
    frame(0, 0, true, x, y);
    frame(0, 0, false, 0, 0);
    settle();
}

static void drag(int x, int y0, int y1)
{
    frame(0, 0, true, x, y0);
    for (int i = 1; i <= 8; i++)
        frame(0, 0, true, x, y0 + (y1 - y0) * i / 8);
    frame(0, 0, false, 0, 0);
    settle();
}

static void tap_key(char ch)
{
    static const char *const rows[4] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_()" };
    for (int r = 0; r < 4; r++)
    {
        const char *pos = strchr(rows[r], ch);
        if (pos)
        {
            tap(3 + (int)(pos - rows[r]) * 25 + 12, 30 + r * 27 + 12);
            return;
        }
    }
}

static void type(const char *s)
{
    for (; *s; s++)
        tap_key(*s);
}

static void start_app(void)
{
    app_init(&app, &lib, root, card_slot);
    app.dirty = true;
    app_render(&app, &top, &bottom); // The "Scanning" frame
    settle();
}

static Game *game(void)
{
    return lib.count ? &lib.games[app.game_sel] : NULL;
}

static void select_game(const char *code)
{
    for (int i = 0; i < lib.count; i++)
    {
        if (strcmp(lib.games[i].info.game_code, code) == 0)
        {
            while (app.game_sel != i)
                press(app.game_sel < i ? UI_KEY_DOWN : UI_KEY_UP);
            return;
        }
    }
    CHECK(!"game not found");
}

static uint32_t crc_of(const char *path)
{
    uint32_t crc = 0;
    long size;
    file_crc32(path, &crc, &size);
    return crc;
}

static bool status_ok(void)
{
    return app.status_kind == STATUS_OK;
}

// ---------------------------------------------------------------------------
// Screenshots: both screens stacked, as on the console.

static const char *shot_dir = "screenshots";

static void shot(const char *name)
{
    app.dirty = true;
    app_render(&app, &top, &bottom);

    enum { GAP = 8, W = SCREEN_W, H = SCREEN_H * 2 + GAP };
    static uint8_t rgb[W * H * 3];
    memset(rgb, 0x40, sizeof(rgb));
    for (int s = 0; s < 2; s++)
    {
        const Color *px = s ? bottom_px : top_px;
        for (int y = 0; y < SCREEN_H; y++)
        {
            for (int x = 0; x < SCREEN_W; x++)
            {
                Color c = px[y * SCREEN_W + x];
                uint8_t *o = &rgb[((y + s * (SCREEN_H + GAP)) * W + x) * 3];
                o[0] = (uint8_t)(((c & 31) * 255) / 31);
                o[1] = (uint8_t)((((c >> 5) & 31) * 255) / 31);
                o[2] = (uint8_t)((((c >> 10) & 31) * 255) / 31);
            }
        }
    }
    size_t len = 0;
    void *png = tdefl_write_image_to_png_file_in_memory(rgb, W, H, 3, &len);
    char path[DSAVE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.png", shot_dir, name);
    FILE *f = fopen(path, "wb");
    if (f && png)
    {
        fwrite(png, 1, len, f);
        fclose(f);
    }
    mz_free(png);
}

// ---------------------------------------------------------------------------

// DSpico with Pico Launcher and TWiLight, a few games.
static void make_pico_card(void)
{
    new_card();
    make_dir("_pico");
    make_dir("_nds/TWiLightMenu");
    make_rom("Mario Kart DS.nds", "AMCE", "Mario Kart DS");
    make_save("Mario Kart DS.sav", 512 * KB, 20 * KB, 1);
    make_save("saves/Mario Kart DS.sav", 512 * KB, 20 * KB, 1);
    make_rom("roms/HeartGold.nds", "IPKE", "POK\xe9MON HeartGold Version\nNintendo");
    make_save("roms/HeartGold.sav", 512 * KB, 400 * KB, 3);
    make_rom("roms/Zelda PH.nds", "AZEE", "The Legend of Zelda: Phantom Hourglass");
    make_save("roms/saves/Zelda PH.sav", 512 * KB, 8 * KB, 4); // TWiLight's only
    make_rom("roms/Tetris DS.nds", "ATRE", "Tetris DS");
    make_rom("roms/Advance Wars DS.nds", "AWRE", "Advance Wars: Dual Strike");
    make_save("roms/Advance Wars DS.sav", 64 * KB, 64 * KB, 5);
    make_rom("roms/Kirby.nds", "ATKE", "Kirby: Squeak Squad");
    make_save("roms/Kirby.sav", 512 * KB, 2 * KB, 6);
    make_rom("roms/Professor Layton.nds", "A5FE", "Professor Layton and the Curious Village");
    make_save("roms/Professor Layton.sav", 512 * KB, 8 * KB, 7);
    make_rom("roms/Picross.nds", "APRE", "Picross DS");
}

static void test_main_flow(void)
{
    make_pico_card();
    start_app();
    CHECK(lib.count == 8);
    CHECK(lib.kernels.active == KERNEL_PICO);
    CHECK(app.backups_game == app.game_sel && app.backup_count == 0);
    CHECK(app.icon.valid && strcmp(app.icon_rom, game()->rom_path) == 0);
    shot("01_games");

    // Accented title, sorted by its UTF-8 bytes.
    select_game("IPKE");
    CHECK(strcmp(game()->title, "POK\xc3\xa9MON HeartGold Version") == 0);

    // A: quick backup.
    press(UI_KEY_A);
    CHECK(status_ok() && app.backup_count == 1 && !app.backups[0].compressed);
    shot("02_backed_up");

    // Turn compression on from the menu, then back up again.
    press(UI_KEY_START);
    CHECK(app.modal == MODAL_MENU);
    press(UI_KEY_DOWN);
    press(UI_KEY_A);
    CHECK(lib.compress);
    shot("03_menu");
    press(UI_KEY_B);
    CHECK(app.modal == MODAL_NONE);
    press(UI_KEY_A);
    CHECK(status_ok() && app.backup_count >= 1);

    // X compresses the older raw backup.
    int raw = app.backups[0].compressed ? 1 : 0;
    CHECK(app.backup_count == 2 && !app.backups[raw].compressed && app.backups[1 - raw].compressed);
    while (app.hist_sel != raw)
        press(app.hist_sel < raw ? UI_KEY_R : UI_KEY_L);
    CHECK(app.hist_sel == raw);
    press(UI_KEY_X);
    CHECK(status_ok() && app.backups[app.hist_sel].compressed);
    CHECK(app.backup_disk[app.hist_sel] < app.backups[app.hist_sel].size);
    shot("04_history");

    // Play a little, so the restore changes the save.
    FILE *played = fopen(game()->save_path, "r+b");
    CHECK(played && fputc(0x42, played) == 0x42);
    if (played)
        fclose(played);
    uint32_t played_crc = crc_of(game()->save_path);

    // B: restore needs A held; a short press does nothing.
    press(UI_KEY_B);
    CHECK(app.modal == MODAL_CONFIRM && app.dlg_hold);
    shot("05_restore_confirm");
    hold(UI_KEY_A, 20);
    CHECK(app.modal == MODAL_CONFIRM && app.dlg_progress == 0);
    frame(UI_KEY_A, UI_KEY_A, false, 0, 0);
    for (int i = 0; i < 30; i++)
        frame(0, UI_KEY_A, false, 0, 0);
    shot("06_holding");
    for (int i = 0; i < 40; i++)
        frame(0, UI_KEY_A, false, 0, 0);
    frame(0, 0, false, 0, 0);
    settle();
    CHECK(app.modal == MODAL_NONE && status_ok());
    int with_auto = app.backup_count;
    bool has_auto = false;
    for (int i = 0; i < app.backup_count; i++)
        has_auto |= app.backups[i].is_auto;
    CHECK(has_auto && with_auto >= 2);
    shot("07_restored");

    // Y: rename with the touch keyboard.
    press(UI_KEY_L);
    press(UI_KEY_Y);
    CHECK(app.modal == MODAL_MORE);
    shot("08_more");
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_KEYBOARD);
    for (int i = 0; i < 30; i++)
        press(UI_KEY_B); // Delete the old name
    CHECK(app.kb_text[0] == '\0');
    tap(3 + 12, 30 + 4 * 27 + 12); // Shift
    type("before");
    tap(3 + 2 * 25 + 30, 30 + 4 * 27 + 12); // Space
    type("boss");
    CHECK(strcmp(app.kb_text, "Before boss") == 0);
    shot("09_keyboard");
    press(UI_KEY_START);
    CHECK(app.modal == MODAL_NONE && status_ok());
    Backup *b = &app.backups[app.hist_sel];
    CHECK(strcmp(b->name, "Before boss") == 0);

    // Add a note: it shows under the history list.
    press(UI_KEY_Y);
    press(UI_KEY_DOWN);
    CHECK(app.more_sel == 1);
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_KEYBOARD && app.kb_op == OP_NOTE && app.kb_text[0] == '\0');
    type("all");
    tap(3 + 2 * 25 + 30, 30 + 4 * 27 + 12); // Space
    type("stars");
    shot("09b_note");
    press(UI_KEY_START);
    CHECK(app.modal == MODAL_NONE && status_ok());
    b = &app.backups[app.hist_sel];
    CHECK(strcmp(b->name, "Before boss") == 0 && strcmp(b->note, "all stars") == 0);

    // Keep it: trash is disabled until the mark is cleared.
    press(UI_KEY_Y);
    press(UI_KEY_DOWN);
    press(UI_KEY_DOWN);
    CHECK(app.more_sel == 2);
    press(UI_KEY_A);
    CHECK(status_ok() && app.backups[app.hist_sel].keep);
    shot("09c_kept");
    press(UI_KEY_Y);
    shot("09d_more_kept");
    for (int i = 0; i < 3; i++)
        press(UI_KEY_DOWN); // Skips the disabled trash item
    CHECK(app.more_sel == 4);
    press(UI_KEY_B);
    CHECK(app.modal == MODAL_NONE);

    // View it in hex, then compare: first with the current save, then with
    // each other backup until one differs (the auto backup of the save that
    // was played on).
    press(UI_KEY_Y);
    while (app.more_sel != 4)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    settle();
    CHECK(app.screen == SCREEN_HEX && app.hex_got == HEX_PAGE && app.hex_off == 0 && !app.hex_cmp_open);
    shot("09e_hex");
    press(UI_KEY_R);
    CHECK(app.hex_off == HEX_PAGE);
    press(UI_KEY_UP);
    CHECK(app.hex_off == HEX_PAGE - HEX_COLS);
    press(UI_KEY_X);
    settle();
    CHECK(app.hex_cmp == HEX_CMP_LIVE && app.hex_cmp_open);
    bool found_diff = app.diff.count > 0;
    for (int i = 0; i < 6 && !found_diff && app.hex_cmp != HEX_CMP_NONE; i++)
    {
        press(UI_KEY_X);
        settle();
        found_diff = app.diff.count > 0;
    }
    CHECK(found_diff && app.diff.range[0].start == 0);
    CHECK(app.hex_off == 0); // Shows the first difference
    bool marked = false;
    for (int i = 0; i < HEX_COLS; i++)
        marked |= app.hex_buf[i] != app.hex_cbuf[i];
    CHECK(marked);
    shot("09f_hex_compare");
    press(UI_KEY_B);
    CHECK(app.screen == SCREEN_MAIN);

    // Export, then clear Keep and trash it.
    press(UI_KEY_Y);
    while (app.more_sel != 5)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    CHECK(status_ok() && strstr(app.status, "export"));
    press(UI_KEY_Y);
    while (app.more_sel != 2)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    CHECK(status_ok() && !app.backups[app.hist_sel].keep);
    int before = app.backup_count;
    press(UI_KEY_Y);
    while (app.more_sel != 3)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_CONFIRM);
    hold(UI_KEY_A, 70);
    CHECK(status_ok() && app.backup_count == before - 1);

    // The activity history lists all of that, newest first, and undoes the
    // restore: the save goes back to what it was before.
    char save[DSAVE_PATH_MAX];
    snprintf(save, sizeof(save), "%s", game()->save_path);
    uint32_t restored_crc = crc_of(save);
    press(UI_KEY_START);
    while (app.menu_sel != MENU_HISTORY)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_HISTORY && app.act_count >= 5);
    CHECK(app.acts[0].kind == ACT_TRASH && app.acts[1].kind == ACT_EXPORT);
    shot("09g_history");
    int r = -1;
    for (int i = 0; i < app.act_count && r < 0; i++)
        r = app.acts[i].kind == ACT_RESTORE ? i : -1;
    CHECK(r >= 0);
    press(UI_KEY_A); // The trash entry can't be undone
    CHECK(app.modal == MODAL_HISTORY);
    while (app.act_sel != r)
        press(UI_KEY_DOWN);
    shot("09h_history_restore");
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_CONFIRM && app.dlg_op == OP_UNDO);
    hold(UI_KEY_A, 70);
    CHECK(app.modal == MODAL_NONE && status_ok());
    CHECK(crc_of(save) != restored_crc && crc_of(save) == played_crc);
    char logp[DSAVE_PATH_MAX];
    p(logp, "_DSavE/log.txt");
    CHECK(file_size(logp) > 0);

    // B goes back to the menu.
    press(UI_KEY_START);
    while (app.menu_sel != MENU_HISTORY)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    CHECK(app.acts[0].kind == ACT_UNDO);
    press(UI_KEY_B);
    CHECK(app.modal == MODAL_MENU);

    // Check all backups: everything verifies.
    while (app.menu_sel != MENU_CHECK)
        press(UI_KEY_DOWN);
    shot("09i_menu_check");
    press(UI_KEY_A);
    settle();
    CHECK(app.modal == MODAL_CHECK && app.check.issue_count == 0 && app.check.backups > 0 && status_ok());
    shot("09j_check_ok");
    press(UI_KEY_B);
    CHECK(app.modal == MODAL_MENU);
    press(UI_KEY_B);

    // Damage one backup: the check finds it, and A shows it in the list.
    Backup victim = app.backups[app.backup_count - 1];
    FILE *vf = fopen(victim.file, "r+b");
    CHECK(vf != NULL);
    if (vf)
    {
        fseek(vf, 10, SEEK_SET);
        fputc(0x99, vf);
        fclose(vf);
    }
    app.hist_sel = 0;
    press(UI_KEY_START);
    while (app.menu_sel != MENU_CHECK)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    settle();
    CHECK(app.modal == MODAL_CHECK && app.check.issue_count == 1);
    CHECK(app.check.issues[0].kind == CHECK_DAMAGED && strcmp(app.check.issues[0].name, victim.name) == 0);
    shot("09k_check_damaged");
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_NONE && app.status_kind == STATUS_ERROR);
    CHECK(strcmp(app.backups[app.hist_sel].name, victim.name) == 0);

    // Export all to .zip: Left/Right picks newest or all; A writes it.
    press(UI_KEY_START);
    while (app.menu_sel != MENU_ZIP)
        press(UI_KEY_DOWN);
    CHECK(app.zip_mode == ARCHIVE_NEWEST);
    press(UI_KEY_RIGHT);
    CHECK(app.zip_mode == ARCHIVE_ALL);
    shot("09l_menu_zip");
    press(UI_KEY_A);
    settle();
    CHECK(app.status_kind == STATUS_ERROR && strstr(app.status, "1 damaged backup left out")); // Damaged above
    char zips[DSAVE_PATH_MAX];
    p(zips, "_DSavE/export");
    CHECK(dir_exists(zips));
    shot("09m_zipped");
    press(UI_KEY_B);
}

static void test_copies(void)
{
    make_pico_card();
    start_app();

    // Zelda only has a TWiLight save: Pico falls back to it.
    select_game("AZEE");
    CHECK(game()->primary_is_fallback);
    shot("10_fallback");

    // SELECT opens the copies screen.
    press(UI_KEY_SELECT);
    CHECK(app.screen == SCREEN_COPIES);
    CHECK(app.sync.state == SYNC_SINGLE);
    shot("11_copies");

    // Copy the TWiLight save to Pico's place.
    int pico = library_loc_for_kernel(game(), KERNEL_PICO);
    while (app.copy_sel != pico)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_CONFIRM);
    hold(UI_KEY_A, 70);
    CHECK(status_ok() && app.sync.state == SYNC_MATCH);
    shot("12_copied");

    // Slots: pick TWiLight slot 2 by touch.
    int slot_row = game()->loc_count + 2;
    while (app.copy_sel != slot_row)
        press(UI_KEY_DOWN);
    shot("13_slot");
    tap(60, 147 + 10); // A: Use slot
    CHECK(status_ok() && app.slots[2].current);

    press(UI_KEY_B);
    CHECK(app.screen == SCREEN_MAIN);
}

static void test_touch(void)
{
    make_pico_card();
    start_app();

    // Tap the third row.
    tap(100, 24 + 2 * 20 + 10);
    CHECK(app.game_sel == 2);

    // Drag the list up by two rows: scrolls, does not select.
    drag(100, 120, 120 - 2 * 20 - 5);
    CHECK(app.game_scroll == 2 && app.game_sel == 2);

    // Tap the Backup button.
    select_game("ATKE");
    tap(60, 147 + 10);
    CHECK(status_ok() && app.backup_count == 1);

    // Tap Rename/Delete, then outside the popup to close it.
    tap(190, 170 + 10);
    CHECK(app.modal == MODAL_MORE);
    tap(5, 185);
    CHECK(app.modal == MODAL_NONE);

    // A game with no save: Backup is refused.
    select_game("ATRE");
    press(UI_KEY_A);
    CHECK(app.status_kind == STATUS_ERROR && app.backup_count == 0);
    shot("14_no_save");

    // Kernel switch from the menu.
    press(UI_KEY_START);
    press(UI_KEY_RIGHT);
    CHECK(lib.kernels.active == KERNEL_TWILIGHT);
    shot("15_kernel");
    press(UI_KEY_B);
}

static void test_empty_card(void)
{
    new_card();
    start_app();
    CHECK(lib.count == 0 && app.status_kind == STATUS_ERROR);
    press(UI_KEY_A);
    press(UI_KEY_B);
    press(UI_KEY_SELECT);
    CHECK(app.screen == SCREEN_MAIN && app.modal == MODAL_NONE);
    shot("16_empty");
    press(UI_KEY_START);
    while (app.menu_sel != MENU_ABOUT)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_ABOUT);
    shot("17_about");
}

static void test_import(void)
{
    make_pico_card();
    make_save("_DSavE/import/Kirby from PC.sav", 512 * KB, 3 * KB, 9);
    make_save("_DSavE/import/broken.sav", 1000, 1000, 9);
    // DeSmuME's save for a ROM on the card.
    make_save("_DSavE/import/Tetris DS.dsv", 8 * KB, 8 * KB, 10);
    {
        char path[DSAVE_PATH_MAX];
        p(path, "_DSavE/import/Tetris DS.dsv");
        uint8_t footer[EMUSAVE_DSV_FOOTER_MAX];
        int n = emusave_dsv_footer(8 * KB, footer);
        FILE *f = fopen(path, "ab");
        fwrite(footer, 1, (size_t)n, f);
        fclose(f);
    }
    start_app();

    // Keep auto backups: Right from 10 goes to 20.
    press(UI_KEY_START);
    press(UI_KEY_DOWN);
    press(UI_KEY_DOWN);
    CHECK(lib.keep_auto == LIBRARY_KEEP_AUTO_DEFAULT);
    press(UI_KEY_RIGHT);
    CHECK(lib.keep_auto == 20 && status_ok());
    shot("18_keep_auto");
    press(UI_KEY_B);

    // Y, "Import a save", lists the import folder.
    select_game("ATKE");
    press(UI_KEY_Y);
    for (int i = 0; i < 9 && app.more_sel != 8; i++)
        press(UI_KEY_DOWN); // Skips the items that need a backup
    CHECK(app.more_sel == 8);
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_IMPORT && app.import_count == 3);
    shot("19_import");
    if (app.import_count != 3)
        return;

    // The bad size is refused; the good one becomes a backup.
    int good = strcmp(app.imports[0].name, "Kirby from PC.sav") == 0 ? 0 : 1;
    CHECK(app.imports[good].valid && !app.imports[1 - good].valid);
    while (app.import_sel != 1 - good)
        press(app.import_sel < 1 - good ? UI_KEY_DOWN : UI_KEY_UP);
    press(UI_KEY_A);
    CHECK(app.backup_count == 0);
    while (app.import_sel != good)
        press(app.import_sel < good ? UI_KEY_DOWN : UI_KEY_UP);
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_CONFIRM && !app.dlg_hold);
    shot("20_import_confirm");
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_NONE && status_ok() && app.backup_count == 1);
    CHECK(strncmp(app.backups[0].name, "import_", 7) == 0);
    shot("21_imported");

    // A .dsv named after Tetris goes into Tetris, though Kirby is selected.
    int kirby = app.game_sel;
    press(UI_KEY_Y);
    for (int i = 0; i < 9 && app.more_sel != 8; i++)
        press(UI_KEY_DOWN);
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_IMPORT && app.import_count == 3);
    while (app.import_sel != 2)
        press(UI_KEY_DOWN);
    CHECK(app.imports[2].valid && app.imports[2].format == EMU_DESMUME && app.imports[2].size == 8 * KB);
    CHECK(app.imports[2].game >= 0 && strcmp(lib.games[app.imports[2].game].info.game_code, "ATRE") == 0);
    shot("21b_import_matched");
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_CONFIRM && strstr(app.dlg_text, "Tetris DS"));
    press(UI_KEY_A);
    CHECK(app.modal == MODAL_NONE && status_ok() && app.game_sel != kirby);
    CHECK(strcmp(lib.games[app.game_sel].info.game_code, "ATRE") == 0);
    CHECK(app.backup_count == 1 && strcmp(app.backups[0].name, "import_Tetris DS") == 0);
    CHECK(app.backups[0].size == 8 * KB);

    // Export it for DeSmuME: named after the ROM, footer added.
    press(UI_KEY_Y);
    CHECK(app.modal == MODAL_MORE);
    for (int i = 0; i < 9 && app.more_sel != 6; i++)
        press(UI_KEY_DOWN);
    shot("21c_more_export");
    press(UI_KEY_A);
    settle();
    CHECK(status_ok() && strstr(app.status, "Tetris DS.dsv"));
    char out[DSAVE_PATH_MAX];
    p(out, "_DSavE/export/Tetris DS.dsv");
    CHECK(file_size(out) > 8 * KB);
}

static void test_fonts(void)
{
    CHECK(gfx_text_width(&font_body, "") == 0);
    CHECK(gfx_text_width(&font_body, "Hi") > 0);
    // Unknown characters draw as '?'.
    CHECK(gfx_text_width(&font_body, "\xe3\x81\x82") == gfx_text_width(&font_body, "?"));
    const char *s = "\xc3\xa9x";
    CHECK(utf8_next(&s) == 0xE9 && utf8_next(&s) == 'x' && *s == '\0');
    s = "\xff";
    CHECK(utf8_next(&s) == '?' && *s == '\0');
}

static FakeCard fc;
static CardSlot slot;
static int presents;
static bool progress_shot;

static void count_present(void *user)
{
    (void)user;
    presents++;
    app_render(&app, &top, &bottom);
    if (!progress_shot && app.progress >= 400)
    {
        progress_shot = true;
        shot("23_card_progress");
    }
}

static void test_game_card(void)
{
    make_pico_card();
    fake_card_init(&fc, "IPKE", "POKEMON HG", FAKE_FLASH, 512 * KB, 0x204013, true);
    for (long i = 0; i < 512 * KB; i++)
        fc.mem[i] = (uint8_t)(((uint32_t)i * 2654435761u) >> 13);
    fake_card_slot(&fc, &slot);
    card_slot = &slot;
    start_app();
    app.present = count_present;

    // The card is the first row, named after the ROM with its game code.
    CHECK(lib.count == 9 && lib.games[0].is_card && app.game_sel == 0);
    CHECK(strcmp(game()->title, "POK\xc3\xa9MON HeartGold Version") == 0);
    CHECK(status_ok() && strstr(app.status, "game card"));
    CHECK(app.icon.valid && strstr(app.icon_rom, "HeartGold.nds")); // The ROM's icon
    shot("22_card");

    // A reads the card into a backup, with a progress bar.
    press(UI_KEY_A);
    CHECK(status_ok() && app.backup_count == 1 && presents > 0);
    uint32_t want = crc32_update(0, fc.mem, 512 * KB);
    CHECK(app.backups[0].crc32 == want);

    // B writes it back after a hold.
    fc.mem[100] ^= 0x55;
    press(UI_KEY_B);
    CHECK(app.modal == MODAL_CONFIRM && app.dlg_hold);
    shot("24_card_restore");
    hold(UI_KEY_A, 70);
    CHECK(status_ok() && crc32_update(0, fc.mem, 512 * KB) == want);
    CHECK(app.backup_count == 2 && progress_shot);
    shot("25_card_written");

    // The ROM on the SD card shares the card's backups.
    select_game("IPKE");
    CHECK(app.game_sel == 0); // The card comes first
    while (app.game_sel == 0 || strcmp(game()->info.game_code, "IPKE") != 0)
        press(UI_KEY_DOWN);
    CHECK(!game()->is_card && app.backup_count == 2);
    while (app.game_sel != 0)
        press(UI_KEY_UP);

    // Copies are for SD saves.
    press(UI_KEY_SELECT);
    CHECK(app.screen == SCREEN_MAIN && app.status_kind == STATUS_ERROR);

    // Take the card out: the next backup notices, A checks again.
    fc.slot_status = CARD_EMPTY;
    press(UI_KEY_A);
    CHECK(app.status_kind == STATUS_ERROR && game()->is_card && !game()->has_save);
    press(UI_KEY_A);
    CHECK(app.status_kind == STATUS_ERROR && strstr(app.status, "No game card"));
    CHECK(!app.icon.valid && !app.icon_rom[0]);
    shot("26_card_empty");
    fc.slot_status = CARD_READY;
    press(UI_KEY_A);
    CHECK(status_ok() && game()->has_save);

    card_slot = NULL;
}

// A DSiWare title on a DSpico: its .pub, .prv and .bnr back up as one,
// restore together, and export as the files Pico looks for.
static void test_dsiware(void)
{
    new_card();
    make_dir("_pico");
    make_rom("Mario Kart DS.nds", "AMCE", "Mario Kart DS");
    make_dsi_rom("dsiware/Flipnote Studio.nds", "KGUE", "Flipnote Studio", 192 * KB, 16 * KB);
    make_save("dsiware/Flipnote Studio.pub", 192 * KB, 192 * KB, 30);
    make_save("dsiware/Flipnote Studio.prv", 16 * KB, 16 * KB, 31);
    make_save("dsiware/Flipnote Studio.bnr", 16 * KB, 16 * KB, 32);
    start_app();
    select_game("KGUE");
    Game *g = game();
    CHECK(g->info.dsiware && g->has_save && g->save_size == 224 * KB);
    shot("30_dsiware");

    // A: one backup of all three files.
    press(UI_KEY_A);
    CHECK(status_ok() && app.backup_count == 1 && app.backups[0].dsiware);
    CHECK(backup_data_size(&app.backups[0]) == 224 * KB);
    shot("31_dsiware_backed_up");

    // X: DSiWare backups stay raw.
    press(UI_KEY_X);
    CHECK(app.status_kind == STATUS_ERROR && strstr(app.status, "DSiWare"));

    // Y: there is no .dsv for DSiWare, so Down skips it.
    press(UI_KEY_Y);
    while (app.more_sel != 5)
        press(UI_KEY_DOWN);
    shot("32_dsiware_more");
    press(UI_KEY_DOWN);
    CHECK(app.more_sel == 7);
    press(UI_KEY_UP);
    CHECK(app.more_sel == 5);
    press(UI_KEY_A);
    CHECK(status_ok() && strstr(app.status, ".pub, .prv, .bnr"));
    char out[DSAVE_PATH_MAX];
    p(out, "_DSavE/export/Flipnote Studio.prv");
    CHECK(file_size(out) == 16 * KB);

    // Play on, then B: every file comes back.
    char pub[DSAVE_PATH_MAX], prv[DSAVE_PATH_MAX];
    p(pub, "dsiware/Flipnote Studio.pub");
    p(prv, "dsiware/Flipnote Studio.prv");
    uint32_t pub_crc = crc_of(pub), prv_crc = crc_of(prv);
    make_save("dsiware/Flipnote Studio.pub", 192 * KB, 192 * KB, 40);
    make_save("dsiware/Flipnote Studio.prv", 16 * KB, 16 * KB, 41);
    press(UI_KEY_B);
    CHECK(app.modal == MODAL_CONFIRM && strstr(app.dlg_text, "DSiWare"));
    shot("33_dsiware_restore");
    hold(UI_KEY_A, 70);
    CHECK(app.modal == MODAL_NONE && status_ok());
    CHECK(crc_of(pub) == pub_crc && crc_of(prv) == prv_crc);
    CHECK(app.backup_count == 2 && app.backups[0].is_auto && app.backups[0].dsiware);
    shot("34_dsiware_restored");
}

int main(int argc, char **argv)
{
    if (argc > 1)
        shot_dir = argv[1];
    mkdir_p(shot_dir);
    char tmpl[] = "/tmp/dsave-ui-XXXXXX";
    if (!mkdtemp(tmpl))
        return 2;
    snprintf(base, sizeof(base), "%s", tmpl);
    gfx_init(&top, top_px, SCREEN_W, SCREEN_H);
    gfx_init(&bottom, bottom_px, SCREEN_W, SCREEN_H);

    test_fonts();
    test_main_flow();
    test_copies();
    test_touch();
    test_empty_card();
    test_import();
    test_game_card();
    test_dsiware();

    remove_tree(base);
    library_free(&lib);
    printf("%d UI checks, %d failures (screenshots in %s/)\n", checks, failures, shot_dir);
    return failures ? 1 : 0;
}
