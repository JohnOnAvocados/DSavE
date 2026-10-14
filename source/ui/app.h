// The DSavE user interface: state, input handling and drawing for both
// screens. It has no DS dependencies: main.c feeds it the buttons and stylus
// each frame and copies the two canvases to the screens, and the host tests
// drive it the same way to check behaviour and render screenshots.
//
//   Top screen:    the selected game, its save, and its backup history.
//   Bottom screen: the touch game list and the action bar
//                  (A) Backup  (B) Restore  (X) Compress  (Y) Rename/Delete.

#ifndef DSAVE_APP_H
#define DSAVE_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/backup.h"
#include "../core/cardsave.h"
#include "../core/library.h"
#include "../core/sync.h"
#include "gfx.h"

#define APP_VERSION "0.3.0"

#define SCREEN_W 256
#define SCREEN_H 192

// Same bit values as libnds' KEY_*, so main.c passes keysDown() through.
enum {
    UI_KEY_A = 1 << 0,
    UI_KEY_B = 1 << 1,
    UI_KEY_SELECT = 1 << 2,
    UI_KEY_START = 1 << 3,
    UI_KEY_RIGHT = 1 << 4,
    UI_KEY_LEFT = 1 << 5,
    UI_KEY_UP = 1 << 6,
    UI_KEY_DOWN = 1 << 7,
    UI_KEY_R = 1 << 8,
    UI_KEY_L = 1 << 9,
    UI_KEY_X = 1 << 10,
    UI_KEY_Y = 1 << 11,
};

typedef struct {
    uint32_t down;   // Pressed this frame
    uint32_t held;   // Held down
    uint32_t repeat; // Pressed this frame, plus auto-repeat while held
    bool touching;   // Stylus on the bottom screen
    int tx, ty;
} UiInput;

typedef enum {
    SCREEN_MAIN,
    SCREEN_COPIES, // Every kernel's copy of the save, plus save slots
} UiScreen;

typedef enum {
    MODAL_NONE,
    MODAL_CONFIRM,
    MODAL_MENU,
    MODAL_MORE,     // Rename / Note / Keep / Delete / Export popup
    MODAL_KEYBOARD,
    MODAL_ABOUT,
    MODAL_IMPORT,   // Files in _DSavE/import
} UiModal;

// Work that touches the SD card. It is queued, the UI draws one frame saying
// what it is doing, and then it runs.
typedef enum {
    OP_NONE,
    OP_SCAN,
    OP_BACKUP,
    OP_BACKUP_BLANK,  // Back up a save that is all 00/FF
    OP_BACKUP_NAMED,  // Name from the keyboard
    OP_RESTORE,
    OP_COMPRESS,      // Toggle the selected backup between .sav and .sav.gz
    OP_RENAME,
    OP_TRASH,
    OP_EXPORT,
    OP_EXPORT_DSV,
    OP_SET_KERNEL,
    OP_OPEN_COPIES,
    OP_COPY_HERE,
    OP_COPY_ALL,
    OP_BACKUP_COPY,   // Back up the selected copy instead of the primary save
    OP_USE_SLOT,
    OP_IMPORT,
    OP_CARD_CHECK,    // Look at slot 1 again
    OP_NOTE,          // Set the selected backup's note from the keyboard
    OP_KEEP,          // Toggle the selected backup's Keep mark
} UiOp;

typedef enum {
    STATUS_HINT,
    STATUS_OK,
    STATUS_ERROR,
} StatusKind;

typedef struct {
    Library *lib;
    UiScreen screen;
    UiModal modal;
    bool dirty;       // Needs a redraw

    int game_sel, game_scroll;

    Backup backups[BACKUP_LIST_MAX];
    long backup_disk[BACKUP_LIST_MAX]; // Bytes used on the card
    int backup_count, hist_sel, hist_scroll;
    int backups_game; // Game whose backups are loaded, -1 if none

    SyncInfo sync;
    SaveSlot slots[SLOT_MAX];
    int slot_count, copy_sel, copy_scroll;

    UiOp pending;
    char busy[64];
    KernelId pending_kernel;

    char dlg_title[48], dlg_text[192];
    bool dlg_hold;    // Confirm by holding A, for anything that overwrites
    int dlg_progress;
    UiOp dlg_op;

    int menu_sel, more_sel;

    ImportFile imports[IMPORT_LIST_MAX];
    int import_count, import_sel, import_scroll;
    int import_game;      // Game the chosen import file goes into

    char kb_text[BACKUP_NAME_MAX + 1];
    int kb_row, kb_col;
    bool kb_shift;
    UiOp kb_op;

    // Icon of the selected game, and the ROM it came from ("" for none).
    RomIcon icon;
    char icon_rom[DSAVE_PATH_MAX];

    char status[96];
    StatusKind status_kind;

    // Stylus tracking
    bool touch_was;
    bool touch_drag;
    int touch_x0, touch_y0, touch_xlast, touch_ylast, touch_scroll0;
    int touch_hit, touch_now_hit;

    // Slot 1, or NULL where it can't be used. The game card, when there is
    // one, is lib->games[0] (is_card), and card holds what was found.
    CardSlot *card_slot;
    CardInfo card;

    // Long work (game card reads and writes) shows a progress bar. The
    // platform sets present to draw and show a frame mid-operation.
    bool running;
    int progress;     // 0-1000, or -1 for none
    void (*present)(void *user);
    void *present_user;
} App;

// Set up and queue the first scan of root. card_slot may be NULL.
void app_init(App *app, Library *lib, const char *root, CardSlot *card_slot);

// One frame of input. Runs queued work first.
void app_update(App *app, const UiInput *in);

// Draw both screens. Returns false (and draws nothing) if nothing changed.
bool app_render(App *app, Canvas *top, Canvas *bottom);

#endif
