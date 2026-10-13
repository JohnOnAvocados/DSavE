#include "rom.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Cartridge header offsets (GBATEK "DS Cartridge Header").
#define HDR_TITLE        0x000
#define HDR_GAME_CODE    0x00C
#define HDR_BANNER_OFFS  0x068
#define HDR_SIZE         0x200

// Banner layout (GBATEK "DS Cartridge Icon/Title"): six or more 0x100-byte
// UTF-16LE titles starting at 0x240, Japanese first, English second.
#define BANNER_TITLES    0x240
#define BANNER_TITLE_LEN 0x100
#define BANNER_LANG_EN   1

static void copy_ascii(char *out, const uint8_t *in, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len && in[i]; i++)
        out[n++] = (in[i] >= 0x20 && in[i] <= 0x7E) ? (char)in[i] : '_';
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

bool rom_read_info(const char *path, RomInfo *info)
{
    memset(info, 0, sizeof(*info));

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    uint8_t hdr[HDR_SIZE];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
    {
        fclose(f);
        return false;
    }

    copy_ascii(info->header_title, hdr + HDR_TITLE, 12);
    copy_ascii(info->game_code, hdr + HDR_GAME_CODE, 4);

    uint32_t banner = (uint32_t)hdr[HDR_BANNER_OFFS] |
                      ((uint32_t)hdr[HDR_BANNER_OFFS + 1] << 8) |
                      ((uint32_t)hdr[HDR_BANNER_OFFS + 2] << 16) |
                      ((uint32_t)hdr[HDR_BANNER_OFFS + 3] << 24);

    uint8_t title[BANNER_TITLE_LEN];
    long pos = (long)banner + BANNER_TITLES + BANNER_LANG_EN * BANNER_TITLE_LEN;
    if (banner != 0 && fseek(f, pos, SEEK_SET) == 0 &&
        fread(title, 1, sizeof(title), f) == sizeof(title))
    {
        // Convert UTF-16 to UTF-8 and keep only the first line, which is the
        // game's name; later lines are the subtitle and publisher.
        size_t n = 0;
        for (size_t i = 0; i + 1 < sizeof(title) && n + 4 < sizeof(info->banner_title); i += 2)
        {
            uint16_t ch = (uint16_t)(title[i] | (title[i + 1] << 8));
            if (ch == 0 || ch == '\n')
                break;
            char *out = info->banner_title;
            if (ch < 0x20 || (ch >= 0x7F && ch < 0xA0) || (ch >= 0xD800 && ch <= 0xDFFF))
                out[n++] = '_';
            else if (ch < 0x80)
                out[n++] = (char)ch;
            else if (ch < 0x800)
            {
                out[n++] = (char)(0xC0 | (ch >> 6));
                out[n++] = (char)(0x80 | (ch & 0x3F));
            }
            else
            {
                out[n++] = (char)(0xE0 | (ch >> 12));
                out[n++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (ch & 0x3F));
            }
        }
        while (n > 0 && info->banner_title[n - 1] == ' ')
            n--;
        info->banner_title[n] = '\0';
    }

    fclose(f);
    return true;
}

const char *rom_display_title(const RomInfo *info, const char *fallback)
{
    if (info->banner_title[0])
        return info->banner_title;
    if (info->header_title[0])
        return info->header_title;
    return fallback;
}

bool rom_is_homebrew(const RomInfo *info)
{
    const char *c = info->game_code;
    if (strlen(c) != 4)
        return true;
    if (strcmp(c, "####") == 0 || strcmp(c, "0000") == 0)
        return true;
    return false;
}
