#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/savefile.h"

// ---------------------------------------------------------------------------
// Look

#define C_BG GFX_RGB(18, 20, 26)
#define C_PANEL GFX_RGB(30, 34, 43)
#define C_PANEL2 GFX_RGB(43, 48, 60)
#define C_PRESSED GFX_RGB(62, 70, 88)
#define C_LINE GFX_RGB(54, 60, 74)
#define C_TEXT GFX_RGB(233, 237, 242)
#define C_MUTED GFX_RGB(142, 151, 168)
#define C_FAINT GFX_RGB(96, 104, 120)
#define C_ACCENT GFX_RGB(88, 182, 255)
#define C_SELECT GFX_RGB(30, 64, 99)
#define C_OK GFX_RGB(92, 212, 138)
#define C_WARN GFX_RGB(242, 184, 75)
#define C_ERR GFX_RGB(240, 100, 100)
#define C_BLACK GFX_RGB(0, 0, 0)
#define C_WHITE GFX_RGB(255, 255, 255)
#define C_KEY_A GFX_RGB(222, 92, 90)
#define C_KEY_B GFX_RGB(226, 170, 60)
#define C_KEY_X GFX_RGB(84, 134, 236)
#define C_KEY_Y GFX_RGB(70, 178, 108)
#define C_KEY_OTHER GFX_RGB(110, 118, 136)

// Bottom screen layout
#define HEADER_H 22
#define LIST_Y 24
#define GAME_ROW_H 20
#define GAME_ROWS 6
#define COPY_ROW_H 24
#define COPY_ROWS 5
#define BAR_Y 147
#define BTN_W 122
#define BTN_H 21

// Top screen layout
#define HIST_Y 88
#define HIST_ROW_H 15
#define HIST_ROWS 4
#define STATUS_Y 178

#define MENU_ITEMS 5
#define MENU_Y 28
#define MENU_PITCH 24
#define MORE_ITEMS 6
#define MORE_BOX_Y 12
#define MORE_BOX_H 168
#define MORE_Y 40
#define MORE_PITCH 23

// "More" popup items
enum { MORE_RENAME, MORE_TRASH, MORE_EXPORT_SAV, MORE_EXPORT_DSV, MORE_NEW, MORE_IMPORT };
#define IMPORT_ROWS 5

// Choices for "Keep auto backups"; 0 keeps all.
static const int keep_choices[] = { 5, 10, 20, 50, 0 };
#define KEEP_CHOICES ((int)(sizeof(keep_choices) / sizeof(keep_choices[0])))

#define HOLD_FRAMES 60
#define DRAG_START 6

// Keyboard
#define KB_ROWS 5
#define KB_COLS 10
#define KB_Y 30
#define KB_KEY_W 24
#define KB_KEY_H 25
#define KB_PITCH_X 25
#define KB_PITCH_Y 27

static const char *const kb_chars[4] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_()" };
enum { KB_SHIFT, KB_SPACE, KB_DELETE, KB_OK };
static const int kb_special_col[4] = { 0, 2, 6, 8 };   // First unit of each special key
static const int kb_special_units[4] = { 2, 4, 2, 2 };

// ---------------------------------------------------------------------------
// Touch controls. Each frame the visible controls are laid out once; drawing
// and hit testing both use that layout.

enum {
    ID_NONE = 0,
    ID_MENU,
    ID_COPIES,
    ID_BACK,
    ID_ACT_A,
    ID_ACT_B,
    ID_ACT_X,
    ID_ACT_Y,
    ID_ACT_START,
    ID_ACT_BACK,
    ID_DLG_OK,
    ID_DLG_CANCEL,
    ID_KB_CANCEL,
    ID_OUTSIDE,
    ID_ROW = 100,  // + visible row
    ID_ITEM = 200, // + menu item
    ID_KEY = 300,  // + row * 16 + column
};

typedef struct {
    int x, y, w, h, id;
} Ctl;

#define MAX_CTLS 64

typedef struct {
    Ctl c[MAX_CTLS];
    int n;
} Ctls;

static void add_ctl(Ctls *ctls, int x, int y, int w, int h, int id)
{
    if (ctls->n < MAX_CTLS)
        ctls->c[ctls->n++] = (Ctl){ x, y, w, h, id };
}

static const Ctl *find_ctl(const Ctls *ctls, int id)
{
    for (int i = 0; i < ctls->n; i++)
    {
        if (ctls->c[i].id == id)
            return &ctls->c[i];
    }
    return NULL;
}

static int hit_test(const Ctls *ctls, int x, int y)
{
    // Later controls sit on top.
    for (int i = ctls->n - 1; i >= 0; i--)
    {
        const Ctl *c = &ctls->c[i];
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h)
            return c->id;
    }
    return ID_NONE;
}

// ---------------------------------------------------------------------------
// Helpers

static Game *cur_game(App *app)
{
    return app->lib->count ? &app->lib->games[app->game_sel] : NULL;
}

static Backup *cur_backup(App *app)
{
    if (app->backup_count == 0 || app->hist_sel >= app->backup_count)
        return NULL;
    return &app->backups[app->hist_sel];
}

static bool on_card(App *app)
{
    Game *g = cur_game(app);
    return g && g->is_card;
}

// Why the game card's save can't be used, or "".
static const char *card_note(const App *app)
{
    switch (app->card.status)
    {
        case CARD_EMPTY:      return "No game card in slot 1. Insert one and press A";
        case CARD_UNREADABLE: return "Couldn't read the game card. Reinsert it and press A";
        case CARD_NO_CHIP:    return "This game card has no save chip DSavE can find";
        case CARD_NAND:       return "This card keeps its save in NAND, which DSavE can't read yet";
        default:              return "";
    }
}

static void set_status(App *app, StatusKind kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, ap);
    va_end(ap);
    app->status_kind = kind;
    app->dirty = true;
}

static void result_status(App *app, DsaveResult r, const char *ok_msg)
{
    if (r == DSAVE_OK)
        set_status(app, STATUS_OK, "%s", ok_msg);
    else
        set_status(app, STATUS_ERROR, "%s", dsave_result_str(r));
}

static void fmt_size(long size, char *out, size_t n)
{
    const long kb = 1024, mb = 1024 * 1024;
    if (size < 0)
        snprintf(out, n, "none");
    else if (size < kb)
        snprintf(out, n, "%ld B", size);
    else if (size < mb)
    {
        if (size % kb == 0 || size >= 10 * kb)
            snprintf(out, n, "%ld KB", (size + kb / 2) / kb);
        else
            snprintf(out, n, "%ld.%ld KB", size / kb, (size % kb) * 10 / kb);
    }
    else if (size % mb == 0)
        snprintf(out, n, "%ld MB", size / mb);
    else
        snprintf(out, n, "%ld.%ld MB", size / mb, (size % mb) * 10 / mb);
}

static bool is_digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

// "2026-10-13_14-22-01" -> "13 Oct 2026 14:22". Anything after the
// timestamp (a "_2" from a name clash) is kept.
static bool pretty_time(const char *s, char *out, size_t n)
{
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (strlen(s) < 19 || !is_digits(s, 4) || s[4] != '-' || !is_digits(s + 5, 2) || s[7] != '-' ||
        !is_digits(s + 8, 2) || s[10] != '_' || !is_digits(s + 11, 2) || s[13] != '-' ||
        !is_digits(s + 14, 2) || s[16] != '-' || !is_digits(s + 17, 2))
        return false;
    int month = (s[5] - '0') * 10 + (s[6] - '0');
    if (month < 1 || month > 12)
        return false;
    int day = (s[8] - '0') * 10 + (s[9] - '0');
    snprintf(out, n, "%d %s %.4s %.2s:%.2s%s", day, months[month - 1], s, s + 11, s + 14, s + 19);
    return true;
}

static void backup_label(const Backup *b, char *out, size_t n)
{
    char t[48];
    if (b->is_auto && pretty_time(b->name + 5, t, sizeof(t)))
        snprintf(out, n, "Auto, %s", t);
    else if (pretty_time(b->name, t, sizeof(t)))
        snprintf(out, n, "%s", t);
    else
        snprintf(out, n, "%s", b->name);
}

// "Pico Launcher, akmenu-next" for the kernels in mask.
static void kernel_names(KernelMask mask, char *out, size_t size)
{
    out[0] = '\0';
    for (int k = 0; k < KERNEL_COUNT; k++)
    {
        if (!(mask & KERNEL_BIT(k)))
            continue;
        size_t len = strlen(out);
        snprintf(out + len, size - len, "%s%s", len ? ", " : "", kernel_name((KernelId)k));
    }
}

static void keep_visible(int sel, int *scroll, int rows, int count)
{
    if (sel < *scroll)
        *scroll = sel;
    if (sel >= *scroll + rows)
        *scroll = sel - rows + 1;
    int max = count > rows ? count - rows : 0;
    if (*scroll > max)
        *scroll = max;
    if (*scroll < 0)
        *scroll = 0;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int copy_rows(const App *app)
{
    const Game *g = app->lib->count ? &app->lib->games[app->game_sel] : NULL;
    return g ? g->loc_count + app->slot_count : 0;
}

// ---------------------------------------------------------------------------
// Loading

static void load_backups(App *app)
{
    Game *g = cur_game(app);
    app->backup_count = g ? backup_list(app->lib, g, app->backups, BACKUP_LIST_MAX) : 0;
    for (int i = 0; i < app->backup_count; i++)
        app->backup_disk[i] = file_size(app->backups[i].file);
    app->backups_game = app->game_sel;
    app->hist_sel = clampi(app->hist_sel, 0, app->backup_count ? app->backup_count - 1 : 0);
    keep_visible(app->hist_sel, &app->hist_scroll, HIST_ROWS, app->backup_count);
    app->dirty = true;
}

static void ensure_backups(App *app)
{
    if (app->backups_game != app->game_sel)
    {
        app->hist_sel = app->hist_scroll = 0;
        load_backups(app);
    }
}

// The ROM whose banner gives the selected game's icon: its own, or for a
// game card, a ROM on the SD card with the same game code.
static const char *icon_rom(App *app)
{
    Game *g = cur_game(app);
    if (!g)
        return "";
    if (!g->is_card)
        return g->rom_path;
    for (int i = 0; g->info.game_code[0] && i < app->lib->count; i++)
    {
        const Game *o = &app->lib->games[i];
        if (!o->is_card && strcmp(o->info.game_code, g->info.game_code) == 0)
            return o->rom_path;
    }
    return "";
}

// Load the selected game's icon when the selection changes. Only one icon
// is kept, so a card with hundreds of games costs no extra memory.
static void ensure_icon(App *app)
{
    const char *rom = icon_rom(app);
    if (strcmp(rom, app->icon_rom) == 0)
        return;
    snprintf(app->icon_rom, sizeof(app->icon_rom), "%s", rom);
    if (!rom[0] || !rom_read_icon(rom, &app->icon))
        app->icon.valid = false;
    app->dirty = true;
}

static void select_backup_named(App *app, const char *name)
{
    for (int i = 0; i < app->backup_count; i++)
    {
        if (strcmp(app->backups[i].name, name) == 0)
        {
            app->hist_sel = i;
            keep_visible(i, &app->hist_scroll, HIST_ROWS, app->backup_count);
            return;
        }
    }
}

static void load_copies(App *app)
{
    Game *g = cur_game(app);
    app->slot_count = 0;
    if (!g)
        return;
    library_refresh_game(app->lib, g);
    sync_check(g, &app->sync);
    const KernelConfig *twl = &app->lib->kernels.cfg[KERNEL_TWILIGHT];
    if (twl->installed && twl->twl_location != TWL_GAMES_FOLDER)
        app->slot_count = slots_list(app->lib, g, KERNEL_TWILIGHT, app->slots, SLOT_MAX);
    int rows = copy_rows(app);
    app->copy_sel = clampi(app->copy_sel, 0, rows ? rows - 1 : 0);
    keep_visible(app->copy_sel, &app->copy_scroll, COPY_ROWS, rows);
}

// ---------------------------------------------------------------------------
// Queued work

static void queue(App *app, UiOp op, const char *busy)
{
    app->pending = op;
    snprintf(app->busy, sizeof(app->busy), "%s", busy);
    app->modal = MODAL_NONE;
    app->dirty = true;
}

static void confirm(App *app, UiOp op, bool hold, const char *title, const char *fmt, ...)
{
    snprintf(app->dlg_title, sizeof(app->dlg_title), "%s", title);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->dlg_text, sizeof(app->dlg_text), fmt, ap);
    va_end(ap);
    app->dlg_hold = hold;
    app->dlg_progress = 0;
    app->dlg_op = op;
    app->modal = MODAL_CONFIRM;
    app->dirty = true;
}

static void open_keyboard(App *app, UiOp op, const char *text)
{
    snprintf(app->kb_text, sizeof(app->kb_text), "%s", text);
    app->kb_op = op;
    app->kb_row = 1;
    app->kb_col = 0;
    app->kb_shift = false;
    app->modal = MODAL_KEYBOARD;
    app->dirty = true;
}

// Look at slot 1 again and update the card's entry at the top of the list.
static void check_card(App *app)
{
    Library *lib = app->lib;
    bool had = lib->count && lib->games[0].is_card;
    card_probe(app->card_slot, &app->card);
    card_add_to_library(lib, &app->card);
    bool has = lib->count && lib->games[0].is_card;
    if (!had && has && app->game_sel > 0)
        app->game_sel++;
    else if (had && !has && app->game_sel > 0)
        app->game_sel--;
    app->game_sel = clampi(app->game_sel, 0, lib->count ? lib->count - 1 : 0);
    keep_visible(app->game_sel, &app->game_scroll, GAME_ROWS, lib->count);
    app->backups_game = -1;
}

static void card_found_status(App *app)
{
    if (app->card.status == CARD_READY)
    {
        char desc[32];
        card_chip_describe(&app->card.chip, desc, sizeof(desc));
        set_status(app, STATUS_OK, "Game card: %s, %s", app->lib->games[0].title, desc);
    }
    else
        set_status(app, STATUS_ERROR, "%s", card_note(app));
}

// Redraw the progress bar mid-operation, at most every 1%.
static void on_progress(void *ctx, long done, long total)
{
    App *app = ctx;
    int p = total > 0 ? (int)(done * 1000 / total) : 0;
    if (p < app->progress + 10 && p < 1000)
        return;
    app->progress = p;
    app->dirty = true;
    if (app->present)
        app->present(app->present_user);
}

static void finish_backup(App *app, DsaveResult r, const Backup *made)
{
    if (r == DSAVE_WARN_BLANK)
    {
        confirm(app, OP_BACKUP_BLANK, false, "This save is empty",
                "It holds no game data yet (every byte is 00 or FF). Back it up anyway?");
        return;
    }
    if (r != DSAVE_OK)
    {
        result_status(app, r, "");
        return;
    }
    load_backups(app);
    select_backup_named(app, made->name);
    char label[64];
    backup_label(made, label, sizeof(label));
    if (made->compressed)
    {
        char raw[16], disk[16];
        fmt_size(made->size, raw, sizeof(raw));
        fmt_size(file_size(made->file), disk, sizeof(disk));
        set_status(app, STATUS_OK, "Backed up %s, %s \xe2\x86\x92 %s", label, raw, disk);
    }
    else
        set_status(app, STATUS_OK, "Backed up: %s", label);
}

static void run_op(App *app, UiOp op)
{
    Library *lib = app->lib;
    Game *g = cur_game(app);
    Backup *b = cur_backup(app);
    Backup made, safety;
    memset(&made, 0, sizeof(made));
    memset(&safety, 0, sizeof(safety));
    char msg[96];
    CardProgress progress = { app, on_progress };

    switch (op)
    {
        case OP_NONE:
            break;

        case OP_SCAN:
        {
            char root[DSAVE_PATH_MAX];
            snprintf(root, sizeof(root), "%s", lib->root);
            library_scan(lib, root);
        }
            card_probe(app->card_slot, &app->card);
            card_add_to_library(lib, &app->card);
            app->game_sel = app->game_scroll = 0;
            app->backups_game = -1;
            {
                int roms = library_rom_count(lib);
                const char *card = app->card.status == CARD_READY ? ", and a game card" : "";
                if (roms)
                    set_status(app, STATUS_OK, "Found %d game%s%s%s", roms, roms == 1 ? "" : "s", card,
                               lib->truncated ? " (list is full)" : "");
                else if (app->card.status == CARD_READY)
                    card_found_status(app);
                else
                    set_status(app, STATUS_ERROR, "No DS games found on the card");
            }
            break;

        case OP_CARD_CHECK:
            check_card(app);
            card_found_status(app);
            break;

        case OP_BACKUP:
            app->kb_text[0] = '\0';
            // fall through
        case OP_BACKUP_NAMED:
        case OP_BACKUP_BLANK:
            if (!g)
                break;
        {
            DsaveResult r = g->is_card ? card_backup(lib, app->card_slot, g, app->kb_text, op == OP_BACKUP_BLANK,
                                                     &made, &progress)
                                       : backup_create(lib, g, app->kb_text, op == OP_BACKUP_BLANK, &made);
            if (r == DSAVE_ERR_CARD_CHANGED)
                check_card(app);
            finish_backup(app, r, &made);
            break;
        }

        case OP_BACKUP_COPY:
        {
            if (!g)
                break;
            int rows = copy_rows(app);
            if (app->copy_sel >= rows)
                break;
            const char *src = app->copy_sel < g->loc_count ? g->locs[app->copy_sel].path
                                                           : app->slots[app->copy_sel - g->loc_count].path;
            DsaveResult r = backup_create_from(lib, g, src, NULL, true, &made);
            app->backups_game = -1;
            if (r == DSAVE_OK)
            {
                char label[64];
                backup_label(&made, label, sizeof(label));
                set_status(app, STATUS_OK, "Backed up this copy: %s", label);
            }
            else
                result_status(app, r, "");
            break;
        }

        case OP_RESTORE:
            if (!g || !b)
                break;
            snprintf(msg, sizeof(msg), "%s", b->name);
            {
                bool card = g->is_card;
                DsaveResult r = card ? card_restore(lib, app->card_slot, g, b, &safety, &progress)
                                     : backup_restore(lib, g, b, &safety);
                if (r == DSAVE_ERR_CARD_CHANGED)
                    check_card(app);
                load_backups(app);
                select_backup_named(app, msg);
                if (r == DSAVE_ERR_UNSUPPORTED && card)
                    set_status(app, STATUS_ERROR, "DSavE can't write this card's save chip yet");
                else
                    result_status(app, r, card ? "Written to the game card. Its old save is an Auto backup"
                                          : safety.name[0] ? "Restored. The old save was kept as an Auto backup"
                                                           : "Restored");
            }
            break;

        case OP_COMPRESS:
            if (!b)
                break;
            {
                bool to_gz = !b->compressed;
                DsaveResult r = backup_set_compressed(b, to_gz, &made);
                snprintf(msg, sizeof(msg), "%s", b->name);
                load_backups(app);
                select_backup_named(app, msg);
                if (r == DSAVE_ERR_UNSUPPORTED)
                    set_status(app, STATUS_ERROR, "Saves over 1 MB are kept uncompressed");
                else if (r != DSAVE_OK)
                    result_status(app, r, "");
                else
                {
                    char raw[16], disk[16];
                    fmt_size(made.size, raw, sizeof(raw));
                    fmt_size(file_size(made.file), disk, sizeof(disk));
                    if (to_gz)
                        set_status(app, STATUS_OK, "Compressed: %s \xe2\x86\x92 %s", raw, disk);
                    else
                        set_status(app, STATUS_OK, "Decompressed to a plain .sav (%s)", raw);
                }
            }
            break;

        case OP_RENAME:
            if (!g || !b)
                break;
            {
                DsaveResult r = backup_rename(lib, g, b, app->kb_text);
                char clean[BACKUP_NAME_MAX + 1];
                sanitize_name(clean, sizeof(clean), app->kb_text, BACKUP_NAME_MAX);
                load_backups(app);
                if (r == DSAVE_OK)
                    select_backup_named(app, clean);
                result_status(app, r, "Renamed");
            }
            break;

        case OP_TRASH:
            if (!g || !b)
                break;
            result_status(app, backup_trash(lib, g, b), "Moved to _DSavE/_TRASH_");
            load_backups(app);
            break;

        case OP_EXPORT:
        case OP_EXPORT_DSV:
            if (!g || !b)
                break;
            {
                char path[DSAVE_PATH_MAX], base[DSAVE_PATH_MAX];
                DsaveResult r = backup_export(lib, g, b, op == OP_EXPORT_DSV ? EXPORT_DSV : EXPORT_SAV, path,
                                              sizeof(path));
                path_basename(base, sizeof(base), path);
                snprintf(msg, sizeof(msg), "Exported to _DSavE/export/%s", base);
                result_status(app, r, msg);
            }
            break;

        case OP_SET_KERNEL:
            library_set_kernel(lib, app->pending_kernel);
            set_status(app, STATUS_OK, "Now using %s save paths", kernel_name(lib->kernels.active));
            break;

        case OP_OPEN_COPIES:
            if (!g)
                break;
            app->copy_sel = g->primary;
            app->copy_scroll = 0;
            load_copies(app);
            app->screen = SCREEN_COPIES;
            app->status[0] = '\0';
            break;

        case OP_COPY_HERE:
            if (!g || app->copy_sel >= g->loc_count)
                break;
            {
                DsaveResult r = sync_copy(lib, g, g->primary, app->copy_sel, &safety);
                app->backups_game = -1;
                load_copies(app);
                result_status(app, r, safety.name[0] ? "Copied. The old file was kept as an Auto backup"
                                                     : "Copied");
            }
            break;

        case OP_COPY_ALL:
            if (!g)
                break;
            {
                int copied = 0;
                DsaveResult r = sync_all(lib, g, &copied);
                app->backups_game = -1;
                load_copies(app);
                snprintf(msg, sizeof(msg), "Copied to %d place%s", copied, copied == 1 ? "" : "s");
                result_status(app, r, msg);
            }
            break;

        case OP_IMPORT:
            if (app->import_sel >= app->import_count || app->import_game < 0 || app->import_game >= lib->count)
                break;
            {
                Game *into = &lib->games[app->import_game];
                DsaveResult r = backup_import(lib, into, app->imports[app->import_sel].path, &made);
                if (r == DSAVE_OK)
                {
                    // Show the game it went into, which may not be the one
                    // that was selected.
                    app->game_sel = app->import_game;
                    keep_visible(app->game_sel, &app->game_scroll, GAME_ROWS, lib->count);
                    load_backups(app);
                    select_backup_named(app, made.name);
                    set_status(app, STATUS_OK, "Imported as %s", made.name);
                }
                else
                    result_status(app, r, "");
            }
            break;

        case OP_USE_SLOT:
            if (!g || app->copy_sel < g->loc_count)
                break;
            {
                int n = app->slots[app->copy_sel - g->loc_count].number;
                DsaveResult r = slots_set_current(lib, g, KERNEL_TWILIGHT, n);
                app->backups_game = -1;
                load_copies(app);
                snprintf(msg, sizeof(msg), "TWiLight now loads slot %d", n);
                result_status(app, r, msg);
            }
            break;
    }
}

// ---------------------------------------------------------------------------
// Actions shared by buttons and touch

static void act_backup(App *app)
{
    Game *g = cur_game(app);
    if (!g)
        return;
    if (g->is_card)
    {
        if (g->has_save)
            queue(app, OP_BACKUP, "Reading the game card");
        else
            queue(app, OP_CARD_CHECK, "Checking the game card");
        return;
    }
    if (!g->has_save)
    {
        set_status(app, STATUS_ERROR, "No save to back up yet. Play the game first");
        return;
    }
    queue(app, OP_BACKUP, "Backing up the save");
}

static void act_restore(App *app)
{
    Game *g = cur_game(app);
    Backup *b = cur_backup(app);
    if (!g)
        return;
    if (!b)
    {
        set_status(app, STATUS_ERROR, "No backups to restore yet");
        return;
    }
    char label[64], file[DSAVE_PATH_MAX];
    backup_label(b, label, sizeof(label));
    path_basename(file, sizeof(file), g->save_path);
    if (g->is_card)
    {
        if (!g->has_save)
            set_status(app, STATUS_ERROR, "%s", card_note(app));
        else
            confirm(app, OP_RESTORE, true, "Write to the game card?",
                    "\"%s\" replaces the save on the card. Its current save is backed up first. "
                    "Keep the card in until it's done.",
                    label);
        return;
    }
    if (g->has_save)
        confirm(app, OP_RESTORE, true, "Restore this backup?",
                "\"%s\" replaces the save in %s. The current save is backed up first.", label, file);
    else
        confirm(app, OP_RESTORE, true, "Restore this backup?",
                "\"%s\" is written to %s, where %s looks for this game's save.", label, file,
                kernel_name(app->lib->kernels.active));
}

static void act_compress(App *app)
{
    Backup *b = cur_backup(app);
    if (!cur_game(app))
        return;
    if (!b)
    {
        set_status(app, STATUS_ERROR, "No backup selected");
        return;
    }
    if (!b->compressed && b->size > SAVEFILE_GZ_MAX)
    {
        set_status(app, STATUS_ERROR, "Saves over 1 MB are kept uncompressed");
        return;
    }
    queue(app, OP_COMPRESS, b->compressed ? "Decompressing the backup" : "Compressing the backup");
}

static void act_more(App *app)
{
    if (!cur_game(app))
        return;
    app->modal = MODAL_MORE;
    app->more_sel = cur_backup(app) ? MORE_RENAME : MORE_NEW;
    app->dirty = true;
}

static void act_open_copies(App *app)
{
    if (on_card(app))
        set_status(app, STATUS_ERROR, "Copies and slots are for saves on the SD card");
    else if (cur_game(app))
        queue(app, OP_OPEN_COPIES, "Comparing the copies of this save");
}

static void act_menu(App *app)
{
    app->modal = MODAL_MENU;
    app->menu_sel = 0;
    app->dirty = true;
}

static void act_close_copies(App *app)
{
    app->screen = SCREEN_MAIN;
    app->status[0] = '\0';
    app->dirty = true;
}

static void act_copy_here(App *app)
{
    Game *g = cur_game(app);
    if (!g)
        return;
    if (app->copy_sel >= g->loc_count)
    {
        const SaveSlot *sl = &app->slots[app->copy_sel - g->loc_count];
        if (sl->current)
            set_status(app, STATUS_HINT, "TWiLight already loads slot %d", sl->number);
        else
            queue(app, OP_USE_SLOT, "Switching save slot");
        return;
    }
    if (app->copy_sel == g->primary)
    {
        set_status(app, STATUS_HINT, "This is the save DSavE is using");
        return;
    }
    if (!g->has_save)
    {
        set_status(app, STATUS_ERROR, "There is no current save to copy");
        return;
    }
    char from[96], to[96];
    kernel_names(g->locs[g->primary].kernels, from, sizeof(from));
    kernel_names(g->locs[app->copy_sel].kernels, to, sizeof(to));
    if (g->locs[app->copy_sel].exists)
        confirm(app, OP_COPY_HERE, true, "Copy the save here?",
                "The %s save replaces the copy %s uses. That copy is backed up first.", from, to);
    else
        confirm(app, OP_COPY_HERE, true, "Copy the save here?",
                "The %s save is copied to where %s looks for it.", from, to);
}

static void act_copy_all(App *app)
{
    Game *g = cur_game(app);
    if (!g)
        return;
    if (g->loc_count < 2)
    {
        set_status(app, STATUS_HINT, "Only one kernel uses this save");
        return;
    }
    if (!g->has_save)
    {
        set_status(app, STATUS_ERROR, "There is no current save to copy");
        return;
    }
    confirm(app, OP_COPY_ALL, true, "Copy to every kernel?",
            "The current save replaces every other copy, so all kernels load the same save. "
            "Old copies are backed up first.");
}

static void act_backup_copy(App *app)
{
    Game *g = cur_game(app);
    if (!g || app->copy_sel >= copy_rows(app))
        return;
    bool exists = app->copy_sel < g->loc_count ? g->locs[app->copy_sel].exists
                                               : app->slots[app->copy_sel - g->loc_count].exists;
    if (!exists)
        set_status(app, STATUS_ERROR, "Nothing to back up there yet");
    else
        queue(app, OP_BACKUP_COPY, "Backing up this copy");
}

static void cycle_kernel(App *app, int dir)
{
    KernelId next = kernels_cycle(&app->lib->kernels, dir);
    if (next == app->lib->kernels.active)
    {
        set_status(app, STATUS_HINT, "%s is the only kernel on this card", kernel_name(next));
        return;
    }
    app->pending_kernel = next;
    UiModal keep = app->modal;
    queue(app, OP_SET_KERNEL, "Switching kernel");
    app->modal = keep;
}

static void cycle_keep(App *app, int dir)
{
    int i = 0;
    while (i < KEEP_CHOICES && keep_choices[i] != app->lib->keep_auto)
        i++;
    if (i == KEEP_CHOICES)
        i = 1; // An unlisted value from settings.ini: start from 10
    i = (i + dir + KEEP_CHOICES) % KEEP_CHOICES;
    int keep = keep_choices[i];
    if (!library_set_keep_auto(app->lib, keep))
        set_status(app, STATUS_ERROR, "Could not save the setting");
    else if (keep)
        set_status(app, STATUS_OK, "Keeping %d auto backups per game", keep);
    else
        set_status(app, STATUS_OK, "Keeping every auto backup");
}

static void menu_activate(App *app, int item)
{
    switch (item)
    {
        case 0:
            cycle_kernel(app, 1);
            break;
        case 1:
            if (library_set_compress(app->lib, !app->lib->compress))
                set_status(app, STATUS_OK, "New backups are %s", app->lib->compress ? "compressed" : "plain .sav files");
            else
                set_status(app, STATUS_ERROR, "Could not save the setting");
            break;
        case 2:
            cycle_keep(app, 1);
            break;
        case 3:
            queue(app, OP_SCAN, "Scanning the SD card");
            break;
        case 4:
            app->modal = MODAL_ABOUT;
            break;
    }
    app->dirty = true;
}

static bool more_enabled(App *app, int item)
{
    Game *g = cur_game(app);
    if (item == MORE_NEW)
        return g && g->has_save;
    if (item == MORE_IMPORT)
        return g && g->info.game_code[0]; // An empty card slot has no game to import for
    return cur_backup(app) != NULL;
}

static void open_import(App *app)
{
    app->import_count = backup_import_list(app->lib, app->imports, IMPORT_LIST_MAX);
    app->import_sel = app->import_scroll = 0;
    app->modal = MODAL_IMPORT;
    app->dirty = true;
}

// The game an import file goes into: the one whose ROM it is named after,
// otherwise the selected game.
static int import_target(const App *app, const ImportFile *f)
{
    if (f->game >= 0 && f->game < app->lib->count)
        return f->game;
    const Game *g = app->lib->count ? &app->lib->games[app->game_sel] : NULL;
    return g && g->info.game_code[0] ? app->game_sel : -1;
}

static void import_choose(App *app)
{
    if (app->import_sel >= app->import_count)
        return;
    const ImportFile *f = &app->imports[app->import_sel];
    if (!f->valid)
    {
        set_status(app, STATUS_ERROR, f->size < 0 ? "That save file is damaged" : "That file is not a DS save size");
        return;
    }
    app->import_game = import_target(app, f);
    if (app->import_game < 0)
        return;
    const Game *g = &app->lib->games[app->import_game];
    if (f->game >= 0)
        confirm(app, OP_IMPORT, false, "Import this save?",
                "\"%s\" is named after the ROM of %s, so it becomes a backup of that game.", f->name, g->title);
    else
        confirm(app, OP_IMPORT, false, "Import this save?",
                "\"%s\" becomes a backup of %s. No ROM has the same name, so check it's the right game.", f->name,
                g->title);
}

static void more_activate(App *app, int item)
{
    if (!more_enabled(app, item))
        return;
    Backup *b = cur_backup(app);
    char label[64];
    switch (item)
    {
        case MORE_RENAME:
            open_keyboard(app, OP_RENAME, b->name);
            break;
        case MORE_TRASH:
            backup_label(b, label, sizeof(label));
            confirm(app, OP_TRASH, true, "Move to trash?",
                    "\"%s\" moves to _DSavE/_TRASH_. Delete it there from a PC to free space.", label);
            break;
        case MORE_EXPORT_SAV:
            queue(app, OP_EXPORT, "Exporting the save");
            break;
        case MORE_EXPORT_DSV:
            queue(app, OP_EXPORT_DSV, "Exporting the save");
            break;
        case MORE_NEW:
            open_keyboard(app, OP_BACKUP_NAMED, "");
            break;
        case MORE_IMPORT:
            open_import(app);
            break;
    }
}

static void dialog_accept(App *app)
{
    UiOp op = app->dlg_op;
    static const char *const busy[] = {
        [OP_RESTORE] = "Restoring the backup",
        [OP_BACKUP_BLANK] = "Backing up the save",
        [OP_TRASH] = "Moving to trash",
        [OP_COPY_HERE] = "Copying the save",
        [OP_COPY_ALL] = "Copying to every kernel",
        [OP_IMPORT] = "Importing the save",
    };
    const char *text = (size_t)op < sizeof(busy) / sizeof(busy[0]) && busy[op] ? busy[op] : "Working";
    if (on_card(app) && op == OP_RESTORE)
        text = "Writing to the game card";
    else if (on_card(app) && op == OP_BACKUP_BLANK)
        text = "Reading the game card";
    queue(app, op, text);
}

static void kb_press(App *app, int row, int col)
{
    size_t len = strlen(app->kb_text);
    if (row < 4)
    {
        char ch = kb_chars[row][col];
        if (app->kb_shift && ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        if (len < BACKUP_NAME_MAX)
        {
            app->kb_text[len] = ch;
            app->kb_text[len + 1] = '\0';
        }
        app->kb_shift = false; // Shift applies to one letter
    }
    else if (col == KB_SHIFT)
        app->kb_shift = !app->kb_shift;
    else if (col == KB_SPACE)
    {
        if (len < BACKUP_NAME_MAX && len > 0)
        {
            app->kb_text[len] = ' ';
            app->kb_text[len + 1] = '\0';
        }
    }
    else if (col == KB_DELETE)
    {
        if (len > 0)
            app->kb_text[len - 1] = '\0';
    }
    else if (col == KB_OK)
    {
        if (app->kb_op == OP_RENAME && app->kb_text[0] == '\0')
        {
            set_status(app, STATUS_ERROR, "Type a name first");
            return;
        }
        queue(app, app->kb_op, app->kb_op == OP_RENAME ? "Renaming the backup" : "Backing up the save");
    }
    app->dirty = true;
}

// Map a column between the letter rows and the special-key row.
static int kb_col_to_special(int col)
{
    for (int k = 3; k >= 0; k--)
    {
        if (col >= kb_special_col[k])
            return k;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Layout

static int pill_width(const char *key, const char *text)
{
    return gfx_text_width(&font_small, key) + gfx_text_width(&font_small, text) + 16;
}

static void layout_bottom(App *app, Ctls *ctls)
{
    ctls->n = 0;
    switch (app->modal)
    {
        case MODAL_NONE:
            break;
        case MODAL_CONFIRM:
            add_ctl(ctls, 0, 0, SCREEN_W, SCREEN_H, ID_OUTSIDE);
            add_ctl(ctls, 14, 36, 228, 124, ID_NONE); // The box itself
            add_ctl(ctls, 22, 126, 132, 24, ID_DLG_OK);
            add_ctl(ctls, 158, 126, 76, 24, ID_DLG_CANCEL);
            return;
        case MODAL_MENU:
            add_ctl(ctls, 4, 3, pill_width("B", "Close"), 16, ID_BACK);
            for (int i = 0; i < MENU_ITEMS; i++)
                add_ctl(ctls, 6, MENU_Y + i * MENU_PITCH, SCREEN_W - 12, MENU_PITCH - 2, ID_ITEM + i);
            return;
        case MODAL_MORE:
            add_ctl(ctls, 0, 0, SCREEN_W, SCREEN_H, ID_OUTSIDE);
            add_ctl(ctls, 20, MORE_BOX_Y, 216, MORE_BOX_H, ID_NONE); // The box itself
            for (int i = 0; i < MORE_ITEMS; i++)
                add_ctl(ctls, 26, MORE_Y + i * MORE_PITCH, 204, MORE_PITCH - 2, ID_ITEM + i);
            return;
        case MODAL_IMPORT:
            add_ctl(ctls, 4, 3, pill_width("B", "Back"), 16, ID_BACK);
            for (int i = 0; i < IMPORT_ROWS && app->import_scroll + i < app->import_count; i++)
                add_ctl(ctls, 0, LIST_Y + i * COPY_ROW_H, SCREEN_W, COPY_ROW_H, ID_ITEM + i);
            add_ctl(ctls, 4, BAR_Y + 23, BTN_W, BTN_H, ID_DLG_OK);
            add_ctl(ctls, 130, BAR_Y + 23, BTN_W, BTN_H, ID_DLG_CANCEL);
            return;
        case MODAL_KEYBOARD:
            add_ctl(ctls, 200, 5, 52, 20, ID_KB_CANCEL);
            for (int r = 0; r < 4; r++)
            {
                for (int c = 0; c < KB_COLS; c++)
                    add_ctl(ctls, 3 + c * KB_PITCH_X, KB_Y + r * KB_PITCH_Y, KB_KEY_W, KB_KEY_H, ID_KEY + r * 16 + c);
            }
            for (int k = 0; k < 4; k++)
                add_ctl(ctls, 3 + kb_special_col[k] * KB_PITCH_X, KB_Y + 4 * KB_PITCH_Y,
                        kb_special_units[k] * KB_PITCH_X - 1, KB_KEY_H, ID_KEY + 4 * 16 + k);
            return;
        case MODAL_ABOUT:
            add_ctl(ctls, 0, 0, SCREEN_W, SCREEN_H, ID_OUTSIDE);
            return;
    }

    if (app->screen == SCREEN_MAIN)
    {
        add_ctl(ctls, 4, 3, pill_width("START", "Menu"), 16, ID_MENU);
        int w = pill_width("SELECT", "Copies");
        add_ctl(ctls, SCREEN_W - 4 - w, 3, w, 16, ID_COPIES);
        for (int i = 0; i < GAME_ROWS && app->game_scroll + i < app->lib->count; i++)
            add_ctl(ctls, 0, LIST_Y + i * GAME_ROW_H, SCREEN_W, GAME_ROW_H, ID_ROW + i);
    }
    else
    {
        add_ctl(ctls, 4, 3, pill_width("B", "Back"), 16, ID_BACK);
        int rows = copy_rows(app);
        for (int i = 0; i < COPY_ROWS && app->copy_scroll + i < rows; i++)
            add_ctl(ctls, 0, LIST_Y + i * COPY_ROW_H, SCREEN_W, COPY_ROW_H, ID_ROW + i);
    }
    add_ctl(ctls, 4, BAR_Y, BTN_W, BTN_H, ID_ACT_A);
    add_ctl(ctls, 130, BAR_Y, BTN_W, BTN_H, app->screen == SCREEN_MAIN ? ID_ACT_B : ID_ACT_START);
    add_ctl(ctls, 4, BAR_Y + 23, BTN_W, BTN_H, app->screen == SCREEN_MAIN ? ID_ACT_X : ID_ACT_Y);
    add_ctl(ctls, 130, BAR_Y + 23, BTN_W, BTN_H, app->screen == SCREEN_MAIN ? ID_ACT_Y : ID_ACT_BACK);
}

// ---------------------------------------------------------------------------
// Input

static void activate(App *app, int id)
{
    if (id >= ID_KEY)
    {
        int r = (id - ID_KEY) / 16, c = (id - ID_KEY) % 16;
        app->kb_row = r;
        app->kb_col = c;
        kb_press(app, r, c);
        return;
    }
    if (id >= ID_ITEM)
    {
        int item = id - ID_ITEM;
        if (app->modal == MODAL_MENU)
        {
            app->menu_sel = item;
            menu_activate(app, item);
        }
        else if (app->modal == MODAL_MORE)
        {
            app->more_sel = item;
            more_activate(app, item);
        }
        else if (app->modal == MODAL_IMPORT)
        {
            // First tap selects, a second tap imports.
            int idx = clampi(app->import_scroll + item, 0, app->import_count - 1);
            if (idx == app->import_sel)
                import_choose(app);
            app->import_sel = idx;
            app->dirty = true;
        }
        return;
    }
    if (id >= ID_ROW)
    {
        int row = id - ID_ROW;
        if (app->screen == SCREEN_MAIN)
            app->game_sel = clampi(app->game_scroll + row, 0, app->lib->count - 1);
        else
            app->copy_sel = clampi(app->copy_scroll + row, 0, copy_rows(app) - 1);
        app->dirty = true;
        return;
    }
    switch (id)
    {
        case ID_MENU: act_menu(app); break;
        case ID_COPIES: act_open_copies(app); break;
        case ID_BACK:
            if (app->modal == MODAL_MENU || app->modal == MODAL_IMPORT)
                app->modal = MODAL_NONE;
            else
                act_close_copies(app);
            app->dirty = true;
            break;
        case ID_ACT_A:
            if (app->screen == SCREEN_MAIN)
                act_backup(app);
            else
                act_copy_here(app);
            break;
        case ID_ACT_B: act_restore(app); break;
        case ID_ACT_X: act_compress(app); break;
        case ID_ACT_Y:
            if (app->screen == SCREEN_MAIN)
                act_more(app);
            else
                act_backup_copy(app);
            break;
        case ID_ACT_START: act_copy_all(app); break;
        case ID_ACT_BACK: act_close_copies(app); break;
        case ID_DLG_OK:
            if (app->modal == MODAL_IMPORT)
                import_choose(app);
            else if (!app->dlg_hold)
                dialog_accept(app);
            break;
        case ID_DLG_CANCEL:
        case ID_KB_CANCEL:
        case ID_OUTSIDE:
            app->modal = app->modal == MODAL_ABOUT ? MODAL_MENU : MODAL_NONE;
            app->dirty = true;
            break;
    }
}

static void handle_touch(App *app, const UiInput *in)
{
    Ctls ctls;
    layout_bottom(app, &ctls);
    bool list_screen = app->modal == MODAL_NONE;

    if (in->touching && !app->touch_was)
    {
        app->touch_x0 = app->touch_xlast = in->tx;
        app->touch_y0 = app->touch_ylast = in->ty;
        app->touch_hit = app->touch_now_hit = hit_test(&ctls, in->tx, in->ty);
        app->touch_drag = false;
        app->touch_scroll0 = app->screen == SCREEN_MAIN ? app->game_scroll : app->copy_scroll;
        app->dirty = true;
    }
    else if (in->touching)
    {
        app->touch_xlast = in->tx;
        app->touch_ylast = in->ty;
        int now = hit_test(&ctls, in->tx, in->ty);
        if (now != app->touch_now_hit)
            app->dirty = true;
        app->touch_now_hit = now;

        if (list_screen && app->touch_hit >= ID_ROW && app->touch_hit < ID_ITEM &&
            abs(in->ty - app->touch_y0) > DRAG_START)
            app->touch_drag = true;
        if (app->touch_drag)
        {
            // Drag the list: one row per row height moved.
            bool main = app->screen == SCREEN_MAIN;
            int row_h = main ? GAME_ROW_H : COPY_ROW_H;
            int rows = main ? GAME_ROWS : COPY_ROWS;
            int count = main ? app->lib->count : copy_rows(app);
            int max = count > rows ? count - rows : 0;
            int scroll = clampi(app->touch_scroll0 + (app->touch_y0 - in->ty) / row_h, 0, max);
            int *target = main ? &app->game_scroll : &app->copy_scroll;
            if (scroll != *target)
            {
                *target = scroll;
                app->dirty = true;
            }
        }
    }
    else if (app->touch_was)
    {
        int id = hit_test(&ctls, app->touch_xlast, app->touch_ylast);
        if (!app->touch_drag && id == app->touch_hit && id != ID_NONE)
            activate(app, id);
        if (app->touch_drag && list_screen)
        {
            // Keep the selection on screen after a drag.
            if (app->screen == SCREEN_MAIN)
                app->game_sel = clampi(app->game_sel, app->game_scroll, app->game_scroll + GAME_ROWS - 1);
            else
                app->copy_sel = clampi(app->copy_sel, app->copy_scroll, app->copy_scroll + COPY_ROWS - 1);
        }
        app->touch_hit = app->touch_now_hit = ID_NONE;
        app->touch_drag = false;
        app->dirty = true;
    }
    app->touch_was = in->touching;
}

static void input_main(App *app, uint32_t k)
{
    Library *lib = app->lib;
    int count = lib->count;
    int old = app->game_sel;
    if (count)
    {
        if (k & UI_KEY_UP)
            app->game_sel = app->game_sel > 0 ? app->game_sel - 1 : count - 1;
        if (k & UI_KEY_DOWN)
            app->game_sel = app->game_sel + 1 < count ? app->game_sel + 1 : 0;
        if (k & UI_KEY_LEFT)
            app->game_sel = clampi(app->game_sel - GAME_ROWS, 0, count - 1);
        if (k & UI_KEY_RIGHT)
            app->game_sel = clampi(app->game_sel + GAME_ROWS, 0, count - 1);
    }
    if (app->game_sel != old)
    {
        keep_visible(app->game_sel, &app->game_scroll, GAME_ROWS, count);
        app->status[0] = '\0';
        app->dirty = true;
    }
    if (k & UI_KEY_L && app->hist_sel > 0)
    {
        app->hist_sel--;
        app->dirty = true;
    }
    if (k & UI_KEY_R && app->hist_sel + 1 < app->backup_count)
    {
        app->hist_sel++;
        app->dirty = true;
    }
    keep_visible(app->hist_sel, &app->hist_scroll, HIST_ROWS, app->backup_count);

    if (k & UI_KEY_A)
        act_backup(app);
    else if (k & UI_KEY_B)
        act_restore(app);
    else if (k & UI_KEY_X)
        act_compress(app);
    else if (k & UI_KEY_Y)
        act_more(app);
    else if (k & UI_KEY_START)
        act_menu(app);
    else if (k & UI_KEY_SELECT)
        act_open_copies(app);
}

static void input_copies(App *app, uint32_t k)
{
    int rows = copy_rows(app);
    int old = app->copy_sel;
    if (rows)
    {
        if (k & UI_KEY_UP)
            app->copy_sel = app->copy_sel > 0 ? app->copy_sel - 1 : rows - 1;
        if (k & UI_KEY_DOWN)
            app->copy_sel = app->copy_sel + 1 < rows ? app->copy_sel + 1 : 0;
    }
    if (app->copy_sel != old)
    {
        keep_visible(app->copy_sel, &app->copy_scroll, COPY_ROWS, rows);
        app->status[0] = '\0';
        app->dirty = true;
    }
    if (k & UI_KEY_A)
        act_copy_here(app);
    else if (k & UI_KEY_START)
        act_copy_all(app);
    else if (k & UI_KEY_Y)
        act_backup_copy(app);
    else if (k & (UI_KEY_B | UI_KEY_SELECT))
        act_close_copies(app);
}

static void input_modal(App *app, const UiInput *in)
{
    uint32_t k = in->repeat;
    switch (app->modal)
    {
        case MODAL_NONE:
            break;

        case MODAL_CONFIRM:
            if (in->down & UI_KEY_B)
            {
                app->modal = MODAL_NONE;
                app->dirty = true;
            }
            else if (app->dlg_hold)
            {
                bool holding = (in->held & UI_KEY_A) || (in->touching && app->touch_hit == ID_DLG_OK &&
                                                         app->touch_now_hit == ID_DLG_OK);
                int before = app->dlg_progress;
                app->dlg_progress = holding ? app->dlg_progress + 1 : 0;
                if (app->dlg_progress != before)
                    app->dirty = true;
                if (app->dlg_progress >= HOLD_FRAMES)
                    dialog_accept(app);
            }
            else if (in->down & UI_KEY_A)
                dialog_accept(app);
            break;

        case MODAL_MENU:
            if (k & UI_KEY_UP)
                app->menu_sel = (app->menu_sel + MENU_ITEMS - 1) % MENU_ITEMS;
            if (k & UI_KEY_DOWN)
                app->menu_sel = (app->menu_sel + 1) % MENU_ITEMS;
            if (k & (UI_KEY_UP | UI_KEY_DOWN))
                app->dirty = true;
            if (app->menu_sel == 0 && (k & (UI_KEY_LEFT | UI_KEY_RIGHT)))
                cycle_kernel(app, (k & UI_KEY_LEFT) ? -1 : 1);
            else if (app->menu_sel == 1 && (k & (UI_KEY_LEFT | UI_KEY_RIGHT)))
                menu_activate(app, 1);
            else if (app->menu_sel == 2 && (k & (UI_KEY_LEFT | UI_KEY_RIGHT)))
                cycle_keep(app, (k & UI_KEY_LEFT) ? -1 : 1);
            else if (in->down & UI_KEY_A)
                menu_activate(app, app->menu_sel);
            else if (in->down & (UI_KEY_B | UI_KEY_START))
            {
                app->modal = MODAL_NONE;
                app->dirty = true;
            }
            break;

        case MODAL_MORE:
            for (int step = 0; step < MORE_ITEMS && (k & (UI_KEY_UP | UI_KEY_DOWN)); step++)
            {
                app->more_sel = (app->more_sel + ((k & UI_KEY_UP) ? MORE_ITEMS - 1 : 1)) % MORE_ITEMS;
                if (more_enabled(app, app->more_sel))
                    break;
            }
            if (k & (UI_KEY_UP | UI_KEY_DOWN))
                app->dirty = true;
            if (in->down & UI_KEY_A)
                more_activate(app, app->more_sel);
            else if (in->down & (UI_KEY_B | UI_KEY_Y))
            {
                app->modal = MODAL_NONE;
                app->dirty = true;
            }
            break;

        case MODAL_KEYBOARD:
        {
            int old_row = app->kb_row;
            if (k & UI_KEY_UP)
                app->kb_row = (app->kb_row + KB_ROWS - 1) % KB_ROWS;
            if (k & UI_KEY_DOWN)
                app->kb_row = (app->kb_row + 1) % KB_ROWS;
            if (app->kb_row != old_row)
            {
                if (app->kb_row == 4)
                    app->kb_col = kb_col_to_special(app->kb_col);
                else if (old_row == 4)
                    app->kb_col = kb_special_col[app->kb_col];
            }
            int cols = app->kb_row == 4 ? 4 : KB_COLS;
            if (k & UI_KEY_LEFT)
                app->kb_col = (app->kb_col + cols - 1) % cols;
            if (k & UI_KEY_RIGHT)
                app->kb_col = (app->kb_col + 1) % cols;
            if (k & (UI_KEY_UP | UI_KEY_DOWN | UI_KEY_LEFT | UI_KEY_RIGHT))
                app->dirty = true;
            if (k & UI_KEY_A)
                kb_press(app, app->kb_row, app->kb_col);
            else if (k & UI_KEY_B)
                kb_press(app, 4, KB_DELETE);
            else if (in->down & (UI_KEY_L | UI_KEY_R))
                kb_press(app, 4, KB_SHIFT);
            else if (in->down & UI_KEY_START)
                kb_press(app, 4, KB_OK);
            else if (in->down & UI_KEY_SELECT)
            {
                app->modal = MODAL_NONE;
                app->dirty = true;
            }
            break;
        }

        case MODAL_ABOUT:
            if (in->down & (UI_KEY_A | UI_KEY_B | UI_KEY_START))
            {
                app->modal = MODAL_MENU;
                app->dirty = true;
            }
            break;

        case MODAL_IMPORT:
        {
            int old = app->import_sel;
            if (app->import_count && (k & UI_KEY_UP))
                app->import_sel = (app->import_sel + app->import_count - 1) % app->import_count;
            if (app->import_count && (k & UI_KEY_DOWN))
                app->import_sel = (app->import_sel + 1) % app->import_count;
            if (app->import_sel != old)
            {
                keep_visible(app->import_sel, &app->import_scroll, IMPORT_ROWS, app->import_count);
                app->dirty = true;
            }
            if (in->down & UI_KEY_A)
                import_choose(app);
            else if (in->down & UI_KEY_B)
            {
                app->modal = MODAL_NONE;
                app->dirty = true;
            }
            break;
        }
    }
}

void app_init(App *app, Library *lib, const char *root, CardSlot *card_slot)
{
    memset(app, 0, sizeof(*app));
    app->lib = lib;
    app->card_slot = card_slot;
    app->progress = -1;
    app->backups_game = -1;
    snprintf(lib->root, sizeof(lib->root), "%s", root);
    queue(app, OP_SCAN, "Scanning the SD card");
}

void app_update(App *app, const UiInput *in)
{
    if (app->pending != OP_NONE)
    {
        UiOp op = app->pending;
        app->pending = OP_NONE;
        app->running = true;
        app->progress = -1;
        run_op(app, op);
        app->running = false;
        app->progress = -1;
        if (app->pending == OP_NONE)
            app->busy[0] = '\0';
        if (app->screen == SCREEN_MAIN)
        {
            ensure_backups(app);
            ensure_icon(app);
        }
        app->touch_was = in->touching;
        app->dirty = true;
        return;
    }

    handle_touch(app, in);
    if (app->pending != OP_NONE)
        return;

    if (app->modal != MODAL_NONE)
        input_modal(app, in);
    else
    {
        // Moving repeats while held; actions fire once per press.
        const uint32_t nav = UI_KEY_UP | UI_KEY_DOWN | UI_KEY_LEFT | UI_KEY_RIGHT | UI_KEY_L | UI_KEY_R;
        uint32_t k = (in->repeat & nav) | (in->down & ~nav);
        if (app->screen == SCREEN_MAIN)
            input_main(app, k);
        else
            input_copies(app, k);
    }

    if (app->screen == SCREEN_MAIN && app->pending == OP_NONE)
    {
        ensure_backups(app);
        ensure_icon(app);
    }
}

// ---------------------------------------------------------------------------
// Drawing

static void draw_wrapped(Canvas *c, const Font *f, int x, int y, int w, const char *text, Color col,
                         int max_lines)
{
    const char *p = text;
    for (int line = 0; *p && line < max_lines; line++)
    {
        // Take words while they fit.
        const char *end = p, *best = NULL;
        char buf[192];
        while (true)
        {
            const char *q = end;
            while (*q && *q != ' ' && *q != '\n')
                q++;
            size_t len = (size_t)(q - p);
            if (len >= sizeof(buf))
                break;
            memcpy(buf, p, len);
            buf[len] = '\0';
            if (gfx_text_width(f, buf) > w && best)
                break;
            best = q;
            if (*q != ' ')
                break;
            end = q + 1;
        }
        if (!best)
            best = end;
        size_t len = (size_t)(best - p);
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        if (line == max_lines - 1 && *best)
            gfx_text_fit(c, f, x, y, w, p, col);
        else
            gfx_text_fit(c, f, x, y, w, buf, col);
        p = best;
        while (*p == ' ' || *p == '\n')
            p++;
        y += f->height;
    }
}

// A path as seen from the card root: "fat:/roms/x.sav" -> "/roms/x.sav".
static const char *card_path(const Library *lib, const char *path)
{
    size_t n = strlen(lib->root);
    if (n && lib->root[n - 1] == '/' && strncmp(path, lib->root, n) == 0)
        return path + n - 1;
    const char *colon = strstr(path, ":/");
    return colon ? colon + 1 : path;
}

static void dim(Canvas *c)
{
    for (int i = 0; i < c->w * c->h; i++)
        c->px[i] = gfx_mix(c->px[i], C_BLACK, 150);
}

static Color key_color(const char *key)
{
    switch (key[0])
    {
        case 'A': return C_KEY_A;
        case 'B': return C_KEY_B;
        case 'X': return C_KEY_X;
        case 'Y': return C_KEY_Y;
    }
    return C_KEY_OTHER;
}

// A button badge: a disc for A/B/X/Y/L/R, a pill for START/SELECT.
// Returns its width.
static int draw_badge(Canvas *c, int x, int y, const char *key, bool enabled)
{
    Color col = enabled ? key_color(key) : C_FAINT;
    if (strlen(key) == 1)
    {
        gfx_disc(c, x, y, 15, col);
        gfx_text_center(c, &font_bold, x, y, 15, key, enabled ? C_WHITE : C_PANEL);
        return 15;
    }
    int w = gfx_text_width(&font_small, key) + 8;
    gfx_rrect(c, x, y + 1, w, 13, 6, col);
    gfx_text(c, &font_small, x + 4, y + 1, key, enabled ? C_WHITE : C_PANEL);
    return w;
}

static bool pressed(const App *app, int id)
{
    return app->touch_was && app->touch_hit == id && app->touch_now_hit == id && !app->touch_drag;
}

static void draw_button(Canvas *c, const App *app, const Ctl *ctl, const char *key, const char *label,
                        bool enabled)
{
    if (!ctl)
        return;
    Color bg = pressed(app, ctl->id) && enabled ? C_PRESSED : C_PANEL2;
    gfx_rrect(c, ctl->x, ctl->y, ctl->w, ctl->h, 6, bg);
    int bw = draw_badge(c, ctl->x + 4, ctl->y + (ctl->h - 15) / 2, key, enabled);
    int tx = ctl->x + 4 + bw + 6;
    gfx_text_fit(c, &font_body, tx, ctl->y + (ctl->h - font_body.height) / 2, ctl->x + ctl->w - tx - 4, label,
                 enabled ? C_TEXT : C_FAINT);
}

// Header pill: a muted key name, then the label.
static void draw_pill(Canvas *c, const App *app, const Ctl *ctl, const char *key, const char *label)
{
    if (!ctl)
        return;
    gfx_rrect(c, ctl->x, ctl->y, ctl->w, ctl->h, 8, pressed(app, ctl->id) ? C_PRESSED : C_PANEL2);
    int x = gfx_text(c, &font_small, ctl->x + 7, ctl->y + 1, key, C_MUTED);
    gfx_text(c, &font_small, x + 3, ctl->y + 1, label, C_TEXT);
}

static void draw_scrollbar(Canvas *c, int x, int y, int h, int first, int visible, int count)
{
    if (count <= visible)
        return;
    gfx_rrect(c, x, y, 3, h, 1, C_PANEL2);
    int th = h * visible / count;
    if (th < 8)
        th = 8;
    int ty = y + (h - th) * first / (count - visible);
    gfx_rrect(c, x, ty, 3, th, 1, C_MUTED);
}

static void draw_top_header(Canvas *c, const App *app)
{
    gfx_fill(c, 0, 0, SCREEN_W, 20, C_PANEL);
    int x = gfx_text(c, &font_title, 6, 1, "DSavE", C_ACCENT);
    gfx_text(c, &font_small, x + 4, 5, APP_VERSION, C_FAINT);

    KernelId k = app->lib->kernels.active;
    const char *name = kernel_name(k);
    int w = gfx_text_width(&font_small, name) + 12;
    int px = SCREEN_W - 5 - w;
    gfx_rrect(c, px, 3, w, 14, 7, C_SELECT);
    gfx_text(c, &font_small, px + 6, 3, name, C_TEXT);
    if (!kernel_hw_tested(k))
        gfx_text_right(c, &font_small, px - 5, 3, "untested", C_WARN);
}

static void draw_status(Canvas *c, const App *app)
{
    gfx_fill(c, 0, STATUS_Y, SCREEN_W, SCREEN_H - STATUS_Y, C_PANEL);
    const char *text = app->status;
    Color col = app->status_kind == STATUS_OK ? C_OK : app->status_kind == STATUS_ERROR ? C_ERR : C_MUTED;
    if (app->pending != OP_NONE || app->running)
    {
        text = app->busy;
        col = C_ACCENT;
    }
    if (!text[0])
    {
        text = app->screen == SCREEN_COPIES ? "Up/Down choose a copy  \xc2\xb7  B back"
                                            : "L/R choose a backup  \xc2\xb7  SELECT copies";
        col = C_FAINT;
    }
    if (col != C_FAINT)
        gfx_disc(c, 6, STATUS_Y + 4, 6, col);
    gfx_text_fit(c, &font_small, col != C_FAINT ? 16 : 6, STATUS_Y, SCREEN_W - 22, text,
                 col == C_MUTED ? C_TEXT : col == C_FAINT ? C_FAINT : col);
}

#define ICON_X 6
#define ICON_Y 24
#define INFO_X (ICON_X + ROM_ICON_SIZE + 6) // Text beside the icon

// A 32x32 game card, for a card whose game has no ROM on the SD card.
static void draw_card_glyph(Canvas *c, int x, int y, bool ok)
{
    Color col = ok ? C_ACCENT : C_FAINT;
    gfx_rrect(c, x + 3, y + 1, 26, 30, 3, col);
    gfx_fill(c, x + 21, y + 1, 8, 6, C_BG); // Notched corner
    gfx_rrect(c, x + 7, y + 6, 13, 12, 2, C_BG);
    for (int k = 0; k < 6; k++)
        gfx_fill(c, x + 6 + k * 4, y + 24, 2, 5, C_BG);
}

static void draw_icon(Canvas *c, const App *app, const Game *g)
{
    if (app->icon.valid)
        gfx_indexed(c, ICON_X, ICON_Y, ROM_ICON_SIZE, ROM_ICON_SIZE, app->icon.px, app->icon.pal, 1);
    else if (g->is_card)
        draw_card_glyph(c, ICON_X, ICON_Y, g->has_save);
    else
    {
        gfx_rrect(c, ICON_X, ICON_Y, ROM_ICON_SIZE, ROM_ICON_SIZE, 4, C_PANEL);
        gfx_text_center(c, &font_small, ICON_X, ICON_Y + 10, ROM_ICON_SIZE, "DS", C_FAINT);
    }
}

static void draw_card_info(Canvas *c, const App *app, const Game *g)
{
    draw_icon(c, app, g);
    gfx_text_fit(c, &font_bold, INFO_X, 23, SCREEN_W - 6 - INFO_X, g->title, C_TEXT);
    char line[96], desc[32];
    if (g->has_save)
    {
        card_chip_describe(&app->card.chip, desc, sizeof(desc));
        snprintf(line, sizeof(line), "%s  \xc2\xb7  Game card  \xc2\xb7  %s", g->info.game_code, desc);
    }
    else if (g->info.game_code[0])
        snprintf(line, sizeof(line), "%s  \xc2\xb7  Game card", g->info.game_code);
    else
        snprintf(line, sizeof(line), "Game card in slot 1");
    gfx_text_fit(c, &font_small, INFO_X, 40, SCREEN_W - 6 - INFO_X, line, C_MUTED);
    if (!g->has_save)
        draw_wrapped(c, &font_small, INFO_X, 53, SCREEN_W - 6 - INFO_X, card_note(app), C_WARN, 2);
    else if (!app->card.chip.writable)
        draw_wrapped(c, &font_small, INFO_X, 53, SCREEN_W - 6 - INFO_X,
                     "DSavE can back this chip up, but not write to it yet", C_WARN, 2);
    else
        gfx_text_fit(c, &font_small, INFO_X, 53, SCREEN_W - 6 - INFO_X, "Keep the card in while DSavE reads or writes it",
                     C_FAINT);
}

static void draw_game_info(Canvas *c, const App *app, const Game *g)
{
    if (g->is_card)
    {
        draw_card_info(c, app, g);
        return;
    }
    draw_icon(c, app, g);
    gfx_text_fit(c, &font_bold, INFO_X, 23, SCREEN_W - 6 - INFO_X, g->title, C_TEXT);

    char line[96], save[16], chip[16];
    fmt_size(g->save_size, save, sizeof(save));
    if (!g->has_save)
        snprintf(line, sizeof(line), "%s  \xc2\xb7  No save yet", g->info.game_code);
    else if (g->chip_size > 0 && g->chip_size != g->save_size)
    {
        fmt_size(g->chip_size, chip, sizeof(chip));
        snprintf(line, sizeof(line), "%s  \xc2\xb7  Save %s  \xc2\xb7  Chip %s", g->info.game_code, save, chip);
    }
    else
        snprintf(line, sizeof(line), "%s  \xc2\xb7  Save %s", g->info.game_code, save);
    gfx_text_fit(c, &font_small, INFO_X, 40, SCREEN_W - 6 - INFO_X, line, C_MUTED);

    gfx_text_fit_end(c, &font_small, INFO_X, 53, SCREEN_W - 6 - INFO_X, card_path(app->lib, g->save_path),
                     C_FAINT);

    if (g->primary_is_fallback)
    {
        char names[96];
        kernel_names(g->locs[g->primary].kernels, names, sizeof(names));
        snprintf(line, sizeof(line), "Using the %s copy of this save", names);
        gfx_text_fit(c, &font_small, 6, 67, SCREEN_W - 12, line, C_WARN);
    }
    else if (!g->has_save)
        gfx_text_fit(c, &font_small, 6, 67, SCREEN_W - 12, "Play the game once, or restore a backup",
                     C_FAINT);
}

static void draw_top_main(Canvas *c, const App *app)
{
    const Library *lib = app->lib;
    if (!lib->count)
    {
        if (app->pending == OP_SCAN)
            return;
        gfx_text(c, &font_bold, 6, 30, "No DS games found", C_TEXT);
        draw_wrapped(c, &font_small, 6, 50, SCREEN_W - 12,
                     "DSavE looks for .nds files on the whole card, except system folders. "
                     "Copy your games on, then open the menu with START and rescan.",
                     C_MUTED, 5);
        return;
    }
    const Game *g = &lib->games[app->game_sel];
    draw_game_info(c, app, g);

    // Backup history
    gfx_rrect(c, 4, HIST_Y - 2, SCREEN_W - 8, STATUS_Y - HIST_Y - 1, 6, C_PANEL);
    char head[48];
    snprintf(head, sizeof(head), "Backups (%d)", app->backup_count);
    gfx_text(c, &font_bold, 10, HIST_Y - 1, head, C_TEXT);
    int lx = SCREEN_W - 10 - 15;
    if (app->backup_count > 1)
    {
        draw_badge(c, lx, HIST_Y - 1, "R", true);
        draw_badge(c, lx - 18, HIST_Y - 1, "L", true);
    }
    gfx_text_right(c, &font_small, lx - 22, HIST_Y + 1, lib->compress ? "gzip on" : "", C_FAINT);

    if (app->backups_game != app->game_sel)
        return;
    if (app->backup_count == 0)
    {
        draw_wrapped(c, &font_small, 10, HIST_Y + 20, SCREEN_W - 20,
                     g->has_save ? "No backups yet. Press A to back up this save."
                                 : "No backups yet.",
                     C_FAINT, 3);
        return;
    }
    int y = HIST_Y + 16;
    for (int i = app->hist_scroll; i < app->backup_count && i < app->hist_scroll + HIST_ROWS; i++)
    {
        const Backup *b = &app->backups[i];
        bool sel = i == app->hist_sel;
        if (sel)
        {
            gfx_rrect(c, 7, y, SCREEN_W - 14, HIST_ROW_H, 4, C_SELECT);
            gfx_fill(c, 7, y + 2, 2, HIST_ROW_H - 4, C_ACCENT);
        }
        char size[40], raw[16], disk[16];
        fmt_size(b->size, raw, sizeof(raw));
        if (b->compressed)
        {
            fmt_size(app->backup_disk[i], disk, sizeof(disk));
            snprintf(size, sizeof(size), "%s \xe2\x86\x92 %s", raw, disk);
        }
        else
            snprintf(size, sizeof(size), "%s", raw);
        int sw = gfx_text_width(&font_small, size);
        int right = SCREEN_W - 12;
        if (b->compressed)
        {
            gfx_rrect(c, right - 15, y + 2, 15, 11, 3, C_KEY_Y);
            gfx_text_center(c, &font_small, right - 15, y + 0, 15, "gz", C_WHITE);
            right -= 19;
        }
        gfx_text_right(c, &font_small, right, y + 1, size, sel ? C_TEXT : C_MUTED);
        char label[64];
        backup_label(b, label, sizeof(label));
        gfx_text_fit(c, &font_small, 13, y + 1, right - sw - 8 - 13, label,
                     b->is_auto ? (sel ? C_WARN : gfx_mix(C_WARN, C_PANEL, 80)) : C_TEXT);
        y += HIST_ROW_H;
    }
    draw_scrollbar(c, SCREEN_W - 9, HIST_Y + 16, HIST_ROWS * HIST_ROW_H, app->hist_scroll, HIST_ROWS,
                   app->backup_count);

    const Backup *b = &app->backups[app->hist_sel];
    char detail[96];
    if (b->has_meta)
        snprintf(detail, sizeof(detail), "CRC %08lX  \xc2\xb7  %s", (unsigned long)b->crc32,
                 b->is_auto ? "kept automatically" : b->name);
    else
        snprintf(detail, sizeof(detail), "No backup.ini  \xc2\xb7  %s", b->name);
    gfx_text_fit(c, &font_small, 10, STATUS_Y - 15, SCREEN_W - 20, detail, C_FAINT);
}

static void draw_top_copies(Canvas *c, const App *app)
{
    const Game *g = cur_game((App *)app);
    if (!g)
        return;
    gfx_text_fit(c, &font_bold, 6, 23, SCREEN_W - 12, g->title, C_TEXT);

    static const char *const states[] = {
        [SYNC_SINGLE] = "Only one copy of this save exists",
        [SYNC_MATCH] = "\xe2\x9c\x93 Every copy holds the same save",
        [SYNC_DIFFERENT] = "\xe2\x89\xa0 The copies differ. Pick the one to keep",
    };
    Color sc = app->sync.state == SYNC_MATCH ? C_OK : app->sync.state == SYNC_DIFFERENT ? C_WARN : C_MUTED;
    gfx_text_fit(c, &font_small, 6, 40, SCREEN_W - 12, states[app->sync.state], sc);

    gfx_rrect(c, 4, 58, SCREEN_W - 8, STATUS_Y - 61, 6, C_PANEL);
    int y = 62;
    char line[128], size[16];
    if (app->copy_sel < g->loc_count)
    {
        const SaveLoc *loc = &g->locs[app->copy_sel];
        const LocStatus *st = &app->sync.loc[app->copy_sel];
        char names[96];
        kernel_names(loc->kernels, names, sizeof(names));
        gfx_text(c, &font_small, 10, y, "Used by", C_MUTED);
        gfx_text_fit(c, &font_bold, 10, y + 13, SCREEN_W - 20, names, C_TEXT);
        gfx_text_fit_end(c, &font_small, 10, y + 30, SCREEN_W - 20, card_path(app->lib, loc->path), C_FAINT);
        y += 48;
        if (st->exists)
        {
            fmt_size(loc->size, size, sizeof(size));
            char data[16];
            fmt_size(st->data_size, data, sizeof(data));
            snprintf(line, sizeof(line), "File %s  \xc2\xb7  data %s  \xc2\xb7  CRC %08lX", size, data,
                     (unsigned long)st->crc32);
            gfx_text_fit(c, &font_small, 10, y, SCREEN_W - 20, line, C_MUTED);
            const char *rel = app->copy_sel == g->primary ? "This is the save DSavE uses"
                              : st->same_as_primary         ? "Same data as the current save"
                                                            : "Different from the current save";
            gfx_text_fit(c, &font_small, 10, y + 14, SCREEN_W - 20, rel,
                         st->same_as_primary || app->copy_sel == g->primary ? C_OK : C_WARN);
            if (st->newest && app->sync.existing > 1)
                gfx_text(c, &font_small, 10, y + 28, "Most recently played copy", C_ACCENT);
        }
        else
            gfx_text(c, &font_small, 10, y, "No file yet. A copies the current save here.", C_FAINT);
    }
    else
    {
        const SaveSlot *sl = &app->slots[app->copy_sel - g->loc_count];
        snprintf(line, sizeof(line), "TWiLight Menu++ slot %d", sl->number);
        gfx_text(c, &font_bold, 10, y, line, C_TEXT);
        gfx_text_fit_end(c, &font_small, 10, y + 17, SCREEN_W - 20, card_path(app->lib, sl->path), C_FAINT);
        fmt_size(sl->size, size, sizeof(size));
        snprintf(line, sizeof(line), "%s%s", sl->exists ? "Holds a save, " : "Empty", sl->exists ? size : "");
        gfx_text(c, &font_small, 10, y + 31, line, C_MUTED);
        gfx_text(c, &font_small, 10, y + 45, sl->current ? "TWiLight loads this slot" : "A makes TWiLight load this slot",
                 sl->current ? C_OK : C_FAINT);
    }
}

static void draw_bottom_main(Canvas *c, const App *app, const Ctls *ctls)
{
    const Library *lib = app->lib;
    gfx_fill(c, 0, 0, SCREEN_W, HEADER_H, C_PANEL);
    draw_pill(c, app, find_ctl(ctls, ID_MENU), "START", "Menu");
    draw_pill(c, app, find_ctl(ctls, ID_COPIES), "SELECT", "Copies");
    char count[24];
    int roms = library_rom_count(lib);
    snprintf(count, sizeof(count), "%d game%s", roms, roms == 1 ? "" : "s");
    const Ctl *m = find_ctl(ctls, ID_MENU), *cp = find_ctl(ctls, ID_COPIES);
    if (m && cp)
        gfx_text_center(c, &font_small, m->x + m->w, 4, cp->x - m->x - m->w, count, C_MUTED);

    if (!lib->count)
    {
        gfx_text_center(c, &font_body, 0, 70, SCREEN_W, app->pending == OP_SCAN ? "" : "No games", C_FAINT);
    }
    for (int i = 0; i < GAME_ROWS && app->game_scroll + i < lib->count; i++)
    {
        int idx = app->game_scroll + i;
        const Game *g = &lib->games[idx];
        int y = LIST_Y + i * GAME_ROW_H;
        bool sel = idx == app->game_sel;
        if (sel)
            gfx_rrect(c, 3, y + 1, SCREEN_W - 10, GAME_ROW_H - 2, 5, C_SELECT);
        else if (pressed(app, ID_ROW + i))
            gfx_rrect(c, 3, y + 1, SCREEN_W - 10, GAME_ROW_H - 2, 5, C_PANEL2);
        if (g->is_card)
        {
            // A little game card: notched corner, contacts along the bottom.
            Color col = g->has_save ? C_ACCENT : C_FAINT;
            gfx_rrect(c, 7, y + 4, 12, 13, 2, col);
            gfx_fill(c, 15, y + 4, 4, 3, sel ? C_SELECT : C_BG);
            for (int k = 0; k < 4; k++)
                gfx_fill(c, 9 + k * 2, y + 13, 1, 3, sel ? C_SELECT : C_BG);
        }
        else if (g->has_save)
            gfx_disc(c, 9, y + 6, 8, g->primary_is_fallback ? C_WARN : C_OK);
        else
            gfx_ring(c, 9, y + 6, 8, 1, C_FAINT);
        char size[16];
        fmt_size(g->save_size, size, sizeof(size));
        const char *right = g->has_save ? size
                            : g->is_card ? (app->card.status == CARD_EMPTY ? "empty" : "card")
                                         : "no save";
        int rw = gfx_text_width(&font_small, right);
        gfx_text_right(c, &font_small, SCREEN_W - 12, y + 4, right, sel ? C_MUTED : C_FAINT);
        gfx_text_fit(c, &font_body, 23, y + 2, SCREEN_W - 12 - rw - 6 - 23, g->title, C_TEXT);
    }
    draw_scrollbar(c, SCREEN_W - 5, LIST_Y + 1, GAME_ROWS * GAME_ROW_H - 2, app->game_scroll, GAME_ROWS,
                   lib->count);

    gfx_fill(c, 0, BAR_Y - 3, SCREEN_W, SCREEN_H - BAR_Y + 3, C_PANEL);
    const Game *g = lib->count ? &lib->games[app->game_sel] : NULL;
    const Backup *b = app->backup_count && app->backups_game == app->game_sel ? &app->backups[app->hist_sel] : NULL;
    if (g && g->is_card && !g->has_save)
        draw_button(c, app, find_ctl(ctls, ID_ACT_A), "A", "Check card", true);
    else
        draw_button(c, app, find_ctl(ctls, ID_ACT_A), "A", "Backup", g && g->has_save);
    draw_button(c, app, find_ctl(ctls, ID_ACT_B), "B", "Restore", b != NULL);
    draw_button(c, app, find_ctl(ctls, ID_ACT_X), "X", b && b->compressed ? "Decompress" : "Compress",
                b && (b->compressed || b->size <= SAVEFILE_GZ_MAX));
    draw_button(c, app, find_ctl(ctls, ID_ACT_Y), "Y", "Rename/Delete", g != NULL);
}

static void draw_bottom_copies(Canvas *c, const App *app, const Ctls *ctls)
{
    const Game *g = cur_game((App *)app);
    gfx_fill(c, 0, 0, SCREEN_W, HEADER_H, C_PANEL);
    draw_pill(c, app, find_ctl(ctls, ID_BACK), "B", "Back");
    gfx_text_right(c, &font_small, SCREEN_W - 6, 4, "Copies and slots", C_MUTED);
    if (!g)
        return;

    int rows = copy_rows(app);
    for (int i = 0; i < COPY_ROWS && app->copy_scroll + i < rows; i++)
    {
        int idx = app->copy_scroll + i;
        int y = LIST_Y + i * COPY_ROW_H;
        bool sel = idx == app->copy_sel;
        if (sel)
            gfx_rrect(c, 3, y + 1, SCREEN_W - 10, COPY_ROW_H - 2, 5, C_SELECT);
        else if (pressed(app, ID_ROW + i))
            gfx_rrect(c, 3, y + 1, SCREEN_W - 10, COPY_ROW_H - 2, 5, C_PANEL2);

        char text[96], right[24];
        const char *icon;
        Color ic;
        if (idx < g->loc_count)
        {
            const SaveLoc *loc = &g->locs[idx];
            const LocStatus *st = &app->sync.loc[idx];
            kernel_names(loc->kernels, text, sizeof(text));
            if (idx == g->primary)
            {
                icon = "\xe2\x98\x85"; // ★
                ic = C_ACCENT;
            }
            else if (!st->exists)
            {
                icon = "\xe2\x80\x93"; // –
                ic = C_FAINT;
            }
            else if (st->same_as_primary)
            {
                icon = "\xe2\x9c\x93"; // ✓
                ic = C_OK;
            }
            else
            {
                icon = "\xe2\x89\xa0"; // ≠
                ic = C_WARN;
            }
            if (st->exists)
                fmt_size(loc->size, right, sizeof(right));
            else
                snprintf(right, sizeof(right), "no file");
        }
        else
        {
            const SaveSlot *sl = &app->slots[idx - g->loc_count];
            snprintf(text, sizeof(text), "TWiLight slot %d", sl->number);
            icon = sl->current ? "\xe2\x97\x8f" : "\xe2\x97\x8b"; // ● ○
            ic = sl->current ? C_ACCENT : C_FAINT;
            snprintf(right, sizeof(right), "%s", sl->current ? "in use" : sl->exists ? "saved" : "empty");
        }
        gfx_text_center(c, &font_bold, 6, y + 4, 14, icon, ic);
        int rw = gfx_text_width(&font_small, right);
        gfx_text_right(c, &font_small, SCREEN_W - 12, y + 6, right, sel ? C_MUTED : C_FAINT);
        gfx_text_fit(c, &font_body, 24, y + 4, SCREEN_W - 12 - rw - 6 - 24, text, C_TEXT);
    }
    draw_scrollbar(c, SCREEN_W - 5, LIST_Y + 1, COPY_ROWS * COPY_ROW_H - 2, app->copy_scroll, COPY_ROWS, rows);

    gfx_fill(c, 0, BAR_Y - 3, SCREEN_W, SCREEN_H - BAR_Y + 3, C_PANEL);
    bool slot = app->copy_sel >= g->loc_count;
    draw_button(c, app, find_ctl(ctls, ID_ACT_A), "A", slot ? "Use slot" : "Copy here",
                slot || (g->has_save && app->copy_sel != g->primary));
    draw_button(c, app, find_ctl(ctls, ID_ACT_START), "START", "Copy to all", g->loc_count > 1 && g->has_save);
    draw_button(c, app, find_ctl(ctls, ID_ACT_Y), "Y", "Back up copy", true);
    draw_button(c, app, find_ctl(ctls, ID_ACT_BACK), "B", "Back", true);
}

static void draw_box(Canvas *c, int x, int y, int w, int h)
{
    gfx_rrect_border(c, x, y, w, h, 8, C_LINE, C_PANEL);
}

static void draw_confirm(Canvas *c, const App *app, const Ctls *ctls)
{
    dim(c);
    draw_box(c, 14, 36, 228, 124);
    gfx_text_fit(c, &font_bold, 24, 44, 208, app->dlg_title, C_TEXT);
    draw_wrapped(c, &font_small, 24, 64, 208, app->dlg_text, C_MUTED, 4);

    const Ctl *ok = find_ctl(ctls, ID_DLG_OK);
    if (app->dlg_hold)
    {
        gfx_rrect(c, ok->x, ok->y, ok->w, ok->h, 6, C_PANEL2);
        int fill = (ok->w * app->dlg_progress) / HOLD_FRAMES;
        if (fill > 0)
        {
            gfx_clip(c, ok->x, ok->y, fill, ok->h);
            gfx_rrect(c, ok->x, ok->y, ok->w, ok->h, 6, gfx_mix(C_PANEL2, C_KEY_A, 140));
            gfx_unclip(c);
        }
        int bw = draw_badge(c, ok->x + 4, ok->y + 4, "A", true);
        gfx_text(c, &font_body, ok->x + bw + 10, ok->y + 4, app->dlg_progress ? "Keep holding" : "Hold to confirm",
                 C_TEXT);
    }
    else
        draw_button(c, app, ok, "A", "Yes", true);
    draw_button(c, app, find_ctl(ctls, ID_DLG_CANCEL), "B", app->dlg_hold ? "Cancel" : "No", true);
}

static void draw_switch(Canvas *c, int x, int y, bool on)
{
    gfx_rrect(c, x, y, 26, 14, 7, on ? C_OK : C_FAINT);
    gfx_disc(c, on ? x + 13 : x + 1, y + 1, 12, C_WHITE);
}

static void draw_menu(Canvas *c, const App *app, const Ctls *ctls)
{
    const Library *lib = app->lib;
    gfx_clear(c, C_BG);
    gfx_fill(c, 0, 0, SCREEN_W, HEADER_H, C_PANEL);
    draw_pill(c, app, find_ctl(ctls, ID_BACK), "B", "Close");
    gfx_text_right(c, &font_bold, SCREEN_W - 8, 3, "Menu", C_TEXT);

    static const char *const items[MENU_ITEMS] = { "Kernel", "Compress new backups", "Keep auto backups",
                                                   "Rescan SD card", "About DSavE" };
    for (int i = 0; i < MENU_ITEMS; i++)
    {
        const Ctl *ctl = find_ctl(ctls, ID_ITEM + i);
        bool sel = i == app->menu_sel;
        gfx_rrect(c, ctl->x, ctl->y, ctl->w, ctl->h, 6,
                  sel ? C_SELECT : pressed(app, ctl->id) ? C_PRESSED : C_PANEL);
        int ty = ctl->y + (ctl->h - font_body.height) / 2;
        gfx_text(c, &font_body, ctl->x + 8, ty, items[i], C_TEXT);
        int right = ctl->x + ctl->w - 8;
        char val[48];
        if (i == 0)
        {
            snprintf(val, sizeof(val), "\xe2\x86\x90 %s \xe2\x86\x92", kernel_name(lib->kernels.active));
            gfx_text_right(c, &font_small, right, ty + 1, val, C_ACCENT);
        }
        else if (i == 1)
            draw_switch(c, right - 26, ctl->y + (ctl->h - 14) / 2, lib->compress);
        else if (i == 2)
        {
            if (lib->keep_auto)
                snprintf(val, sizeof(val), "\xe2\x86\x90 Last %d \xe2\x86\x92", lib->keep_auto);
            else
                snprintf(val, sizeof(val), "\xe2\x86\x90 All \xe2\x86\x92");
            gfx_text_right(c, &font_small, right, ty + 1, val, C_ACCENT);
        }
    }

    char found[160], names[128];
    KernelMask mask = 0;
    for (int k = 0; k < KERNEL_COUNT; k++)
    {
        if (lib->kernels.cfg[k].installed)
            mask |= KERNEL_BIT(k);
    }
    kernel_names(mask, names, sizeof(names));
    snprintf(found, sizeof(found),
             "On this card: %s. The kernel decides where DSavE reads and writes each game's save.",
             names);
    draw_wrapped(c, &font_small, 8, MENU_Y + MENU_ITEMS * MENU_PITCH + 4, SCREEN_W - 16, found, C_FAINT, 3);
}

static void draw_more(Canvas *c, const App *app, const Ctls *ctls)
{
    dim(c);
    draw_box(c, 20, MORE_BOX_Y, 216, MORE_BOX_H);
    const Backup *b = cur_backup((App *)app);
    char label[64], title[80];
    if (b)
    {
        backup_label(b, label, sizeof(label));
        snprintf(title, sizeof(title), "Backup: %s", label);
    }
    else
        snprintf(title, sizeof(title), "No backup selected");
    gfx_text_fit(c, &font_bold, 30, MORE_BOX_Y + 7, 196, title, C_TEXT);

    static const char *const items[MORE_ITEMS] = {
        [MORE_RENAME] = "Rename backup",          [MORE_TRASH] = "Move backup to trash",
        [MORE_EXPORT_SAV] = "Export .sav",        [MORE_EXPORT_DSV] = "Export .dsv",
        [MORE_NEW] = "New backup with a name",    [MORE_IMPORT] = "Import a save\xe2\x80\xa6",
    };
    // Who reads each export format
    static const char *const hints[MORE_ITEMS] = {
        [MORE_EXPORT_SAV] = "melonDS, flashcarts",
        [MORE_EXPORT_DSV] = "DeSmuME, DraStic",
    };
    for (int i = 0; i < MORE_ITEMS; i++)
    {
        const Ctl *ctl = find_ctl(ctls, ID_ITEM + i);
        bool en = more_enabled((App *)app, i);
        bool sel = i == app->more_sel;
        gfx_rrect(c, ctl->x, ctl->y, ctl->w, ctl->h, 6,
                  sel && en ? C_SELECT : pressed(app, ctl->id) && en ? C_PRESSED : C_PANEL2);
        gfx_text(c, &font_body, ctl->x + 8, ctl->y + (ctl->h - font_body.height) / 2, items[i],
                 en ? (i == MORE_TRASH ? C_ERR : C_TEXT) : C_FAINT);
        if (hints[i])
            gfx_text_right(c, &font_small, ctl->x + ctl->w - 8, ctl->y + (ctl->h - font_small.height) / 2,
                           hints[i], en ? C_MUTED : C_FAINT);
    }
}

static void draw_import(Canvas *c, const App *app, const Ctls *ctls)
{
    gfx_clear(c, C_BG);
    gfx_fill(c, 0, 0, SCREEN_W, HEADER_H, C_PANEL);
    draw_pill(c, app, find_ctl(ctls, ID_BACK), "B", "Back");
    gfx_text_right(c, &font_small, SCREEN_W - 6, 4, "Import from _DSavE/import", C_MUTED);

    if (!app->import_count)
    {
        draw_wrapped(c, &font_small, 10, 34, SCREEN_W - 20,
                     "No saves to import. Copy saves into the _DSavE/import folder on the SD card, "
                     "then open this again. DSavE reads .sav (melonDS, Checkpoint, JKSM), .dsv "
                     "(DeSmuME, DraStic), no$gba, Action Replay .duc and .sav.gz files. A save "
                     "named after a ROM goes into that game.",
                     C_MUTED, 7);
    }
    for (int i = 0; i < IMPORT_ROWS && app->import_scroll + i < app->import_count; i++)
    {
        int idx = app->import_scroll + i;
        const ImportFile *f = &app->imports[idx];
        int y = LIST_Y + i * COPY_ROW_H;
        bool sel = idx == app->import_sel;
        if (sel)
            gfx_rrect(c, 3, y + 1, SCREEN_W - 10, COPY_ROW_H - 2, 5, C_SELECT);
        else if (pressed(app, ID_ITEM + i))
            gfx_rrect(c, 3, y + 1, SCREEN_W - 10, COPY_ROW_H - 2, 5, C_PANEL2);
        char right[40], size[16];
        if (f->valid)
        {
            fmt_size(f->size, size, sizeof(size));
            const char *fmt = emusave_format_name(f->format);
            snprintf(right, sizeof(right), "%s%s%s", fmt, fmt[0] ? " \xc2\xb7 " : "", size);
        }
        else
            snprintf(right, sizeof(right), "%s", f->size < 0 ? "damaged" : "wrong size");
        int rw = gfx_text_width(&font_small, right);
        gfx_text_right(c, &font_small, SCREEN_W - 12, y + 6, right, f->valid ? C_MUTED : C_ERR);
        gfx_text_fit(c, &font_body, 10, y + 4, SCREEN_W - 12 - rw - 6 - 10, f->name, f->valid ? C_TEXT : C_FAINT);
    }
    draw_scrollbar(c, SCREEN_W - 5, LIST_Y + 1, IMPORT_ROWS * COPY_ROW_H - 2, app->import_scroll, IMPORT_ROWS,
                   app->import_count);

    gfx_fill(c, 0, BAR_Y - 3, SCREEN_W, SCREEN_H - BAR_Y + 3, C_PANEL);
    int target = app->import_count ? import_target(app, &app->imports[app->import_sel]) : app->game_sel;
    const Game *g = target >= 0 && target < app->lib->count ? &app->lib->games[target] : NULL;
    char into[160];
    snprintf(into, sizeof(into), "Into: %s%s", g ? g->title : "",
             app->import_count && app->imports[app->import_sel].game >= 0 ? " (named after its ROM)" : "");
    gfx_text_fit(c, &font_small, 8, BAR_Y + 4, SCREEN_W - 16, into, C_FAINT);
    bool can = app->import_count && app->imports[app->import_sel].valid;
    draw_button(c, app, find_ctl(ctls, ID_DLG_OK), "A", "Import", can);
    draw_button(c, app, find_ctl(ctls, ID_DLG_CANCEL), "B", "Back", true);
}

static void draw_keyboard(Canvas *c, const App *app, const Ctls *ctls)
{
    gfx_clear(c, C_BG);
    gfx_rrect_border(c, 4, 4, 192, 22, 6, C_ACCENT, C_PANEL);
    int x = 10;
    if (app->kb_text[0])
        x += gfx_text_fit_end(c, &font_body, 10, 7, 176, app->kb_text, C_TEXT);
    else
        gfx_text(c, &font_body, 10, 7, app->kb_op == OP_RENAME ? "Name" : "Name (blank for date)", C_FAINT);
    if (app->kb_text[0])
        gfx_fill(c, x + 1, 8, 1, 15, C_ACCENT);
    const Ctl *cancel = find_ctl(ctls, ID_KB_CANCEL);
    gfx_rrect(c, cancel->x, cancel->y, cancel->w, cancel->h, 6, pressed(app, ID_KB_CANCEL) ? C_PRESSED : C_PANEL2);
    gfx_text_center(c, &font_small, cancel->x, cancel->y + 3, cancel->w, "Cancel", C_TEXT);

    for (int r = 0; r < KB_ROWS; r++)
    {
        int cols = r == 4 ? 4 : KB_COLS;
        for (int col = 0; col < cols; col++)
        {
            int id = ID_KEY + r * 16 + col;
            const Ctl *k = find_ctl(ctls, id);
            bool sel = r == app->kb_row && col == app->kb_col;
            Color bg = pressed(app, id) ? C_PRESSED : C_PANEL2;
            if (r == 4 && col == KB_OK)
                bg = gfx_mix(bg, C_ACCENT, 120);
            if (r == 4 && col == KB_SHIFT && app->kb_shift)
                bg = gfx_mix(bg, C_ACCENT, 90);
            gfx_rrect(c, k->x, k->y, k->w, k->h, 5, bg);
            if (sel)
            {
                gfx_rrect_border(c, k->x, k->y, k->w, k->h, 5, C_ACCENT, bg);
            }
            char label[8];
            const Font *f = &font_body;
            if (r < 4)
            {
                char ch = kb_chars[r][col];
                if (app->kb_shift && ch >= 'a' && ch <= 'z')
                    ch = (char)(ch - 'a' + 'A');
                label[0] = ch;
                label[1] = '\0';
            }
            else
            {
                static const char *const special[] = { "Shift", "Space", "\xe2\x8c\xab", "OK" };
                snprintf(label, sizeof(label), "%s", special[col]);
                f = col == KB_DELETE ? &font_title : &font_small;
            }
            gfx_text_center(c, f, k->x, k->y + (k->h - f->height) / 2, k->w, label, C_TEXT);
        }
    }
    gfx_text_center(c, &font_small, 0, 165, SCREEN_W, "A type  \xc2\xb7  B delete  \xc2\xb7  L/R shift",
                    C_FAINT);
    gfx_text_center(c, &font_small, 0, 178, SCREEN_W, "START done  \xc2\xb7  SELECT cancel", C_FAINT);
}

static void draw_about(Canvas *c)
{
    dim(c);
    draw_box(c, 14, 14, 228, 164);
    gfx_text(c, &font_title, 24, 20, "DSavE " APP_VERSION, C_ACCENT);
    draw_wrapped(c, &font_small, 24, 42, 208,
                 "Save manager for DS flashcarts, TWiLight Menu++ and Pico Launcher. "
                 "Backups live in _DSavE/saves on this card.",
                 C_TEXT, 4);
    draw_wrapped(c, &font_small, 24, 98, 208,
                 "GPLv3. Uses miniz (MIT) and DejaVu Sans. github.com/JohnOnAvocados/DSavE",
                 C_MUTED, 3);
    gfx_text(c, &font_small, 24, 160, "Press A or B to close", C_FAINT);
}

static void draw_busy(Canvas *c, const App *app)
{
    dim(c);
    draw_box(c, 28, 70, 200, 52);
    gfx_text_center(c, &font_bold, 28, 78, 200, "Working\xe2\x80\xa6", C_TEXT);
    gfx_text_center(c, &font_small, 28, 98, 200, app->busy, C_MUTED);
    if (app->running && app->progress >= 0)
    {
        gfx_rrect(c, 48, 113, 160, 4, 2, C_PANEL2);
        int w = app->progress * 160 / 1000;
        if (w > 3)
            gfx_rrect(c, 48, 113, w, 4, 2, C_ACCENT);
    }
}

bool app_render(App *app, Canvas *top, Canvas *bottom)
{
    if (!app->dirty)
        return false;
    app->dirty = false;

    // Top screen
    gfx_clear(top, C_BG);
    draw_top_header(top, app);
    if (app->screen == SCREEN_COPIES)
        draw_top_copies(top, app);
    else
        draw_top_main(top, app);
    draw_status(top, app);

    // Bottom screen: the base screen, then any modal over it.
    Ctls base, modal;
    UiModal m = app->modal;
    app->modal = MODAL_NONE;
    layout_bottom(app, &base);
    app->modal = m;
    layout_bottom(app, &modal);

    gfx_clear(bottom, C_BG);
    if (app->screen == SCREEN_COPIES)
        draw_bottom_copies(bottom, app, &base);
    else
        draw_bottom_main(bottom, app, &base);

    switch (app->modal)
    {
        case MODAL_NONE: break;
        case MODAL_CONFIRM: draw_confirm(bottom, app, &modal); break;
        case MODAL_MENU: draw_menu(bottom, app, &modal); break;
        case MODAL_MORE: draw_more(bottom, app, &modal); break;
        case MODAL_IMPORT: draw_import(bottom, app, &modal); break;
        case MODAL_KEYBOARD: draw_keyboard(bottom, app, &modal); break;
        case MODAL_ABOUT:
            app->modal = MODAL_MENU;
            layout_bottom(app, &modal);
            draw_menu(bottom, app, &modal);
            app->modal = MODAL_ABOUT;
            draw_about(bottom);
            break;
    }
    if (app->pending != OP_NONE || app->running)
        draw_busy(bottom, app);
    return true;
}
