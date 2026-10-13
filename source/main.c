// DSavE: a save manager for DS flashcarts, TWiLight Menu++ and Pico Launcher.
//
// This file is the DS side only: it sets both screens up as 16-bit bitmaps,
// reads the buttons and stylus, and copies what source/ui draws to the
// screens. Everything else is plain C that also runs on a PC (see tests/).

#include <nds.h>
#include <fat.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "core/library.h"
#include "ui/app.h"

#define SAME_KEY(k) ((unsigned)UI_##k == (unsigned)k)
_Static_assert(SAME_KEY(KEY_A) && SAME_KEY(KEY_B) && SAME_KEY(KEY_X) && SAME_KEY(KEY_Y) && SAME_KEY(KEY_L) &&
                   SAME_KEY(KEY_R) && SAME_KEY(KEY_START) && SAME_KEY(KEY_SELECT) && SAME_KEY(KEY_UP) &&
                   SAME_KEY(KEY_DOWN) && SAME_KEY(KEY_LEFT) && SAME_KEY(KEY_RIGHT),
               "UI key bits must match libnds");

static Library lib;
static App app;

// Back buffers, copied to VRAM after drawing so the screens never show a
// half-drawn frame.
static Color top_px[SCREEN_W * SCREEN_H] __attribute__((aligned(32)));
static Color bottom_px[SCREEN_W * SCREEN_H] __attribute__((aligned(32)));
static Canvas top, bottom;
static u16 *vram_top, *vram_bottom;

static void present(void)
{
    DC_FlushRange(top_px, sizeof(top_px));
    DC_FlushRange(bottom_px, sizeof(bottom_px));
    swiWaitForVBlank();
    dmaCopy(top_px, vram_top, sizeof(top_px));
    dmaCopy(bottom_px, vram_bottom, sizeof(bottom_px));
}

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

static void fatal(const char *title, const char *text)
{
    gfx_clear(&top, GFX_RGB(18, 20, 26));
    gfx_clear(&bottom, GFX_RGB(18, 20, 26));
    gfx_text(&top, &font_title, 8, 8, "DSavE " APP_VERSION, GFX_RGB(88, 182, 255));
    gfx_text(&top, &font_bold, 8, 40, title, GFX_RGB(240, 100, 100));
    gfx_text(&top, &font_small, 8, 60, text, GFX_RGB(233, 237, 242));
    present();
    while (true)
        swiWaitForVBlank();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    int bg_top = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    int bg_bottom = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    vram_top = bgGetGfxPtr(bg_top);
    vram_bottom = bgGetGfxPtr(bg_bottom);
    lcdMainOnTop();

    gfx_init(&top, top_px, SCREEN_W, SCREEN_H);
    gfx_init(&bottom, bottom_px, SCREEN_W, SCREEN_H);
    keysSetRepeat(20, 4);

    if (!fatInitDefault())
        fatal("Could not open the SD card", "Check the card is in and your loader patched DLDI.");

    char root[DSAVE_PATH_MAX];
    find_root(root, sizeof(root));
    app_init(&app, &lib, root);

    while (true)
    {
        scanKeys();
        UiInput in = { 0 };
        in.down = keysDown();
        in.held = keysHeld();
        in.repeat = keysDownRepeat();
        in.touching = (in.held & KEY_TOUCH) != 0;
        if (in.touching)
        {
            touchPosition tp;
            touchRead(&tp);
            in.tx = tp.px;
            in.ty = tp.py;
        }

        // Draw before running queued work, so the "Working" box shows.
        if (app_render(&app, &top, &bottom))
            present();
        else
            swiWaitForVBlank();
        app_update(&app, &in);
    }
    return 0;
}
