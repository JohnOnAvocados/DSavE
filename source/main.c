// DSavE: a save manager for DS flashcarts and TWiLight Menu++.
//
// This is the step 1 front end: a plain text console on both screens that
// drives the save engine in source/core. The dual-screen tile UI replaces it
// in a later step; the engine API stays the same.

#include <nds.h>
#include <fat.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "core/backup.h"
#include "core/library.h"

#define APP_VERSION "0.1.0"

#define COLS 32
#define LIST_ROWS 17 // Bottom screen rows available for the list

typedef enum {
    VIEW_GAMES,
    VIEW_BACKUPS,
} View;

static PrintConsole top_screen, bottom_screen;

static Library lib;
static Backup backups[BACKUP_LIST_MAX];
static int backup_count;

static View view = VIEW_GAMES;
static int game_sel, game_scroll;
static int backup_sel, backup_scroll; // 0 is the "New backup" row

static char status[COLS * 2 + 1];
static bool status_error;

// ---------------------------------------------------------------------------
// Drawing helpers

static void set_status(bool error, const char *msg)
{
    snprintf(status, sizeof(status), "%s", msg);
    status_error = error;
}

static void result_status(DsaveResult r, const char *ok_msg)
{
    if (r == DSAVE_OK)
        set_status(false, ok_msg);
    else
        set_status(true, dsave_result_str(r));
}

// Print s cut to width so long names never wrap and break the layout.
static void put_clipped(const char *s, int width)
{
    char buf[COLS + 1];
    if (width > COLS)
        width = COLS;
    snprintf(buf, (size_t)width + 1, "%s", s);
    printf("%s", buf);
}

static void format_size(long size, char *out, size_t n)
{
    if (size < 0)
        snprintf(out, n, "none");
    else if (size >= 1024 * 1024)
        snprintf(out, n, "%ld MB", size / (1024 * 1024));
    else if (size >= 1024)
        snprintf(out, n, "%ld KB", size / 1024);
    else
        snprintf(out, n, "%ld B", size);
}

static Game *current_game(void)
{
    return lib.count ? &lib.games[game_sel] : NULL;
}

static void reload_backups(void)
{
    Game *g = current_game();
    backup_count = g ? backup_list(&lib, g, backups, BACKUP_LIST_MAX) : 0;
    int rows = backup_count + 1;
    if (backup_sel >= rows)
        backup_sel = rows - 1;
}

static void keep_visible(int sel, int *scroll)
{
    if (sel < *scroll)
        *scroll = sel;
    if (sel >= *scroll + LIST_ROWS)
        *scroll = sel - LIST_ROWS + 1;
}

static void draw_top(void)
{
    consoleSelect(&top_screen);
    consoleClear();
    KernelId k = lib.kernels.active;
    printf("\x1b[36mDSavE " APP_VERSION "\x1b[39m  %s%s\n", kernel_name(k),
           kernel_hw_tested(k) ? "" : " \x1b[37m(untested)\x1b[39m");
    printf("--------------------------------");

    Game *g = current_game();
    if (!g)
    {
        printf("\nNo DS games found on %s\n\nPut your .nds files on the SD\ncard and press START to rescan.", lib.root);
        return;
    }

    char size[16];
    format_size(g->save_size, size, sizeof(size));
    printf("\n");
    put_clipped(g->title, COLS);
    printf("\nGame code: %s\n", g->info.game_code);
    printf("Save: %s%s\n", size, g->has_save ? "" : " (not created yet)");
    if (g->chip_size > 0 && g->save_size > g->chip_size)
    {
        char chip[16];
        format_size(g->chip_size, chip, sizeof(chip));
        printf("\x1b[37m(chip is %s, rest is padding)\x1b[39m\n", chip);
    }
    printf("\x1b[37m");
    put_clipped(g->save_path, COLS);
    printf("\x1b[39m\n");
    if (g->primary_is_fallback)
        printf("\x1b[33mUsing another kernel's save\x1b[39m\n");

    if (view == VIEW_BACKUPS && backup_sel > 0)
    {
        Backup *b = &backups[backup_sel - 1];
        format_size(b->size, size, sizeof(size));
        printf("\n\x1b[33mBackup\x1b[39m\n");
        put_clipped(b->name, COLS);
        printf("\nMade:  %s\n", b->created[0] ? b->created : "unknown");
        printf("Size:  %s\n", size);
        if (b->has_meta)
            printf("CRC32: %08lX\n", (unsigned long)b->crc32);
        else
            printf("CRC32: not recorded\n");
        if (b->is_auto)
            printf("\x1b[37m(auto copy made before a restore)\x1b[39m\n");
    }
    else if (view == VIEW_BACKUPS)
    {
        printf("\n%d backup%s\n", backup_count, backup_count == 1 ? "" : "s");
    }

    // Status line at the bottom of the top screen.
    printf("\x1b[23;0H%s", status_error ? "\x1b[31m" : "\x1b[32m");
    put_clipped(status, COLS);
    printf("\x1b[39m");
}

static void draw_bottom(void)
{
    consoleSelect(&bottom_screen);
    consoleClear();

    if (view == VIEW_GAMES)
    {
        printf("Games (%d)%s\n", lib.count, lib.truncated ? " - list full" : "");
        keep_visible(game_sel, &game_scroll);
        for (int i = game_scroll; i < lib.count && i < game_scroll + LIST_ROWS; i++)
        {
            Game *g = &lib.games[i];
            printf("%s%c ", i == game_sel ? "\x1b[33m" : "", i == game_sel ? '>' : ' ');
            printf("%c ", g->has_save ? '*' : ' ');
            put_clipped(g->title, COLS - 4);
            printf("\x1b[39m\n");
        }
        printf("\x1b[21;0H--------------------------------");
        printf("A: backups   X: quick backup\n");
        printf("L/R: kernel  START: rescan");
    }
    else
    {
        Game *g = current_game();
        printf("Backups: ");
        put_clipped(g ? g->title : "", COLS - 9);
        printf("\n");
        int rows = backup_count + 1;
        keep_visible(backup_sel, &backup_scroll);
        for (int i = backup_scroll; i < rows && i < backup_scroll + LIST_ROWS; i++)
        {
            printf("%s%c ", i == backup_sel ? "\x1b[33m" : "", i == backup_sel ? '>' : ' ');
            if (i == 0)
                printf("[New backup...]");
            else
                put_clipped(backups[i - 1].name, COLS - 2);
            printf("\x1b[39m\n");
        }
        printf("\x1b[21;0H--------------------------------");
        if (backup_sel == 0)
            printf("A: backup now   Y: name it\nB: back");
        else
            printf("A: restore  Y: rename  X: trash\nSELECT: export     B: back");
    }
}

static void redraw(void)
{
    draw_top();
    draw_bottom();
}

// ---------------------------------------------------------------------------
// Modal prompts

// Ask a question on the bottom screen. Destructive actions need A held for
// about a second, so a stray press can never restore or delete.
static bool confirm(const char *question, bool hold)
{
    consoleSelect(&bottom_screen);
    consoleClear();
    printf("\n");
    put_clipped(question, COLS);
    printf("\n\n%s\nB: cancel", hold ? "Hold A to confirm" : "A: yes");

    int held = 0;
    while (true)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 down = keysDown(), now = keysHeld();
        if (down & KEY_B)
            return false;
        if (!hold && (down & KEY_A))
            return true;
        if (hold)
        {
            held = (now & KEY_A) ? held + 1 : 0;
            printf("\x1b[6;0H[");
            for (int i = 0; i < 30; i++)
                printf("%c", i < held / 2 ? '#' : ' ');
            printf("]");
            if (held >= 60)
                return true;
        }
    }
    return false;
}

// D-pad name editor: Up/Down change the letter, Left/Right move, X deletes,
// A or START accepts, B cancels. Works without the stylus or a keyboard.
static bool edit_name(char *name, size_t size)
{
    static const char charset[] =
        " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_()";
    const int nchars = (int)sizeof(charset) - 1;
    size_t max = size - 1 < BACKUP_NAME_MAX ? size - 1 : BACKUP_NAME_MAX;
    int cur = (int)strlen(name);
    if (cur > 0)
        cur--;

    while (true)
    {
        consoleSelect(&bottom_screen);
        consoleClear();
        printf("Name this backup:\n\n");
        int len = (int)strlen(name);
        for (int i = 0; i <= len && i < (int)max; i++)
        {
            char c = i < len ? name[i] : ' ';
            printf(i == cur ? "\x1b[30;43m%c\x1b[39;49m" : "%c", c);
            if (i % COLS == COLS - 1)
                printf("\n");
        }
        printf("\x1b[19;0HUp/Down: letter  Left/Right: move");
        printf("X: delete letter   A: save\nB: cancel");

        swiWaitForVBlank();
        scanKeys();
        u32 k = keysDownRepeat();
        len = (int)strlen(name);

        if (k & (KEY_A | KEY_START))
        {
            // Trailing spaces are trimmed by sanitize_name later.
            return name[0] != '\0';
        }
        if (k & KEY_B)
            return false;
        if (k & KEY_LEFT && cur > 0)
            cur--;
        if (k & KEY_RIGHT && cur < len && cur + 1 < (int)max)
            cur++;
        if (k & (KEY_UP | KEY_DOWN))
        {
            if (cur == len)
            {
                name[len] = ' ';
                name[len + 1] = '\0';
            }
            const char *pos = strchr(charset, name[cur]);
            int idx = pos ? (int)(pos - charset) : 0;
            idx = (idx + ((k & KEY_UP) ? 1 : nchars - 1)) % nchars;
            name[cur] = charset[idx];
        }
        if (k & KEY_X && cur < len)
        {
            memmove(name + cur, name + cur + 1, (size_t)(len - cur));
            if (cur > 0 && cur >= (int)strlen(name))
                cur--;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Actions

static void do_backup(const char *name)
{
    Game *g = current_game();
    if (!g)
        return;
    set_status(false, "Backing up...");
    draw_top();

    Backup made;
    DsaveResult r = backup_create(&lib, g, name, false, &made);
    if (r == DSAVE_WARN_BLANK)
    {
        if (!confirm("Save is blank (empty). Keep it?", false))
        {
            set_status(false, "Backup cancelled");
            return;
        }
        r = backup_create(&lib, g, name, true, &made);
    }
    char msg[COLS * 2 + 1];
    snprintf(msg, sizeof(msg), "Backed up: %s", made.name);
    result_status(r, msg);
    reload_backups();
}

static void do_restore(Backup *b)
{
    Game *g = current_game();
    char q[96];
    snprintf(q, sizeof(q), "Restore \"%.20s\"?", b->name);
    if (!confirm(q, true))
        return;

    set_status(false, "Restoring...");
    draw_top();
    Backup safety;
    DsaveResult r = backup_restore(&lib, g, b, &safety);
    result_status(r, safety.name[0] ? "Restored (old save kept as auto_)" : "Restored");
    reload_backups();
}

static void do_rename(Backup *b)
{
    char name[BACKUP_NAME_MAX + 2];
    snprintf(name, sizeof(name), "%s", b->name);
    if (!edit_name(name, sizeof(name)))
        return;
    result_status(backup_rename(&lib, current_game(), b, name), "Renamed");
    reload_backups();
}

static void do_trash(Backup *b)
{
    char q[96];
    snprintf(q, sizeof(q), "Move \"%.20s\" to trash?", b->name);
    if (!confirm(q, true))
        return;
    result_status(backup_trash(&lib, current_game(), b), "Moved to _DSavE/_TRASH_");
    reload_backups();
}

static void do_export(Backup *b)
{
    char path[DSAVE_PATH_MAX];
    result_status(backup_export(&lib, current_game(), b, path, sizeof(path)),
                  "Exported to _DSavE/export");
}

static void rescan(void)
{
    consoleSelect(&bottom_screen);
    consoleClear();
    printf("\n  Scanning SD card...");
    library_scan(&lib, lib.root);
    game_sel = game_scroll = 0;
    char msg[COLS * 2 + 1];
    snprintf(msg, sizeof(msg), "Found %d game%s", lib.count, lib.count == 1 ? "" : "s");
    set_status(false, msg);
}

// ---------------------------------------------------------------------------

// The drive the app was launched from: "sd:/" in DSi mode, "fat:/" on a
// DS-mode flashcart. Falls back on the console mode if argv gave no path.
static void find_root(char *out, size_t size)
{
    char cwd[DSAVE_PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
    {
        char *colon = strstr(cwd, ":/");
        if (colon)
        {
            colon[2] = '\0';
            snprintf(out, size, "%s", cwd);
            return;
        }
    }
    snprintf(out, size, "%s", isDSiMode() ? "sd:/" : "fat:/");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&top_screen, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottom_screen, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
    keysSetRepeat(20, 5);

    if (!fatInitDefault())
    {
        consoleSelect(&top_screen);
        printf("DSavE " APP_VERSION "\n\n\x1b[31mCould not open the SD card.\x1b[39m\n\n"
               "Check that the card is in and\nthat your loader patched DLDI.");
        while (true)
            swiWaitForVBlank();
        return 0;
    }

    find_root(lib.root, sizeof(lib.root));
    rescan();
    redraw();

    while (true)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 k = keysDownRepeat();
        if (!k)
            continue;

        if (view == VIEW_GAMES)
        {
            if (k & KEY_UP && game_sel > 0)
                game_sel--;
            if (k & KEY_DOWN && game_sel + 1 < lib.count)
                game_sel++;
            if (k & KEY_LEFT)
                game_sel = game_sel > LIST_ROWS ? game_sel - LIST_ROWS : 0;
            if (k & KEY_RIGHT && lib.count)
                game_sel = game_sel + LIST_ROWS < lib.count ? game_sel + LIST_ROWS : lib.count - 1;
            if (k & KEY_A && current_game())
            {
                view = VIEW_BACKUPS;
                backup_sel = backup_scroll = 0;
                status[0] = '\0';
                reload_backups();
            }
            if (k & KEY_X)
                do_backup(NULL);
            if (k & KEY_START)
                rescan();
            if (k & (KEY_L | KEY_R))
            {
                KernelId next = kernels_cycle(&lib.kernels, (k & KEY_L) ? -1 : 1);
                if (next != lib.kernels.active)
                {
                    library_set_kernel(&lib, next);
                    char msg[COLS * 2 + 1];
                    snprintf(msg, sizeof(msg), "Kernel: %s", kernel_name(next));
                    set_status(false, msg);
                }
            }
        }
        else
        {
            int rows = backup_count + 1;
            if (k & KEY_UP && backup_sel > 0)
                backup_sel--;
            if (k & KEY_DOWN && backup_sel + 1 < rows)
                backup_sel++;
            if (k & KEY_B)
            {
                view = VIEW_GAMES;
                status[0] = '\0';
            }
            else if (backup_sel == 0)
            {
                if (k & KEY_A)
                    do_backup(NULL);
                if (k & KEY_Y)
                {
                    char name[BACKUP_NAME_MAX + 2];
                    timestamp_now(name, sizeof(name));
                    if (edit_name(name, sizeof(name)))
                        do_backup(name);
                }
            }
            else
            {
                Backup *b = &backups[backup_sel - 1];
                if (k & KEY_A)
                    do_restore(b);
                else if (k & KEY_Y)
                    do_rename(b);
                else if (k & KEY_X)
                    do_trash(b);
                else if (k & KEY_SELECT)
                    do_export(b);
            }
        }
        redraw();
    }
    return 0;
}
