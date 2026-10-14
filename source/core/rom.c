#include "rom.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Cartridge header offsets (GBATEK "DS Cartridge Header").
#define HDR_TITLE        0x000
#define HDR_GAME_CODE    0x00C
#define HDR_UNIT_CODE    0x012
#define HDR_BANNER_OFFS  0x068
#define HDR_SIZE         0x200

// DSi header fields (see rom_parse_dsi)
#define HDR_DSI_FLAGS    0x1BF // Bit 2: has a banner save
#define HDR_TITLE_ID_HI  0x234
#define HDR_PUBLIC_SIZE  0x238
#define HDR_PRIVATE_SIZE 0x23C

// Banner layout (GBATEK "DS Cartridge Icon/Title"): six or more 0x100-byte
// UTF-16LE titles starting at 0x240, Japanese first, English second.
#define BANNER_ICON      0x020
#define BANNER_PALETTE   0x220
#define BANNER_TITLES    0x240
#define BANNER_TITLE_LEN 0x100
#define BANNER_LANGS     6

static void copy_ascii(char *out, const uint8_t *in, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len && in[i]; i++)
        out[n++] = (in[i] >= 0x20 && in[i] <= 0x7E) ? (char)in[i] : '_';
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

// Characters the UI fonts can draw besides Latin-1.
static bool is_latin_extra(uint32_t cp)
{
    static const uint16_t extra[] = { 0x2013, 0x2014, 0x2018, 0x2019, 0x201C,
                                      0x201D, 0x2022, 0x2026, 0x2122 };
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
    {
        if (cp == extra[i])
            return true;
    }
    return false;
}

bool text_is_latin(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        uint32_t cp;
        if (p[0] < 0x80)
            cp = *p++;
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
        {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        }
        else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
        {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        }
        else
            return false;
        if (cp >= 0x100 && !is_latin_extra(cp))
            return false;
    }
    return true;
}

// Convert one banner title from UTF-16 to UTF-8, keeping only the first
// line, which is the game's name; later lines are the subtitle and publisher.
static void banner_line(char *out, size_t size, const uint8_t *title)
{
    size_t n = 0;
    for (size_t i = 0; i + 1 < BANNER_TITLE_LEN && n + 4 < size; i += 2)
    {
        uint16_t ch = (uint16_t)(title[i] | (title[i + 1] << 8));
        if (ch == 0 || ch == '\n')
            break;
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
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

void rom_parse(RomInfo *info, const uint8_t *hdr, const uint8_t *titles)
{
    memset(info, 0, sizeof(*info));
    copy_ascii(info->header_title, hdr + HDR_TITLE, 12);
    copy_ascii(info->game_code, hdr + HDR_GAME_CODE, 4);
    if (!titles)
        return;

    // English first, then the other European languages, then Japanese: the
    // first title the fonts can draw wins. Japanese-only games have Japanese
    // in every slot; their title is kept but marked as not Latin.
    static const int order[] = { 1, 2, 3, 4, 5, 0 };
    char first[sizeof(info->banner_title)] = "";
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
    {
        char line[sizeof(info->banner_title)];
        banner_line(line, sizeof(line), titles + order[i] * BANNER_TITLE_LEN);
        if (!line[0])
            continue;
        if (!first[0])
            snprintf(first, sizeof(first), "%s", line);
        if (text_is_latin(line))
        {
            snprintf(info->banner_title, sizeof(info->banner_title), "%s", line);
            info->banner_latin = true;
            return;
        }
    }
    snprintf(info->banner_title, sizeof(info->banner_title), "%s", first);
}

void rom_decode_icon(const uint8_t *bitmap, const uint8_t *palette, RomIcon *icon)
{
    for (int y = 0; y < ROM_ICON_SIZE; y++)
    {
        for (int x = 0; x < ROM_ICON_SIZE; x++)
        {
            int tile = (y / 8) * 4 + x / 8;
            int in = (y % 8) * 8 + x % 8;
            uint8_t b = bitmap[tile * 32 + in / 2];
            icon->px[y * ROM_ICON_SIZE + x] = (uint8_t)((in & 1) ? b >> 4 : b & 0x0F);
        }
    }
    for (int i = 0; i < 16; i++)
        icon->pal[i] = (uint16_t)((palette[i * 2] | (palette[i * 2 + 1] << 8)) & 0x7FFF);
    icon->valid = true;
}

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint32_t banner_offset(const uint8_t *hdr)
{
    return le32(hdr + HDR_BANNER_OFFS);
}

void rom_parse_dsi(RomInfo *info, const uint8_t *hdr)
{
    uint32_t tid_hi = le32(hdr + HDR_TITLE_ID_HI);
    info->dsiware = (hdr[HDR_UNIT_CODE] & 0x02) && (tid_hi >> 16) == 0x0003 && (tid_hi & 0x04);
    if (!info->dsiware)
        return;
    info->dsi_public_size = le32(hdr + HDR_PUBLIC_SIZE);
    info->dsi_private_size = le32(hdr + HDR_PRIVATE_SIZE);
    info->dsi_banner_save = (hdr[HDR_DSI_FLAGS] & 0x04) != 0;
}

long rom_dsi_part_size(const RomInfo *info, DsiPart part)
{
    if (!info->dsiware)
        return 0;
    // A size past 16 MB is not a real save; treat the header as damaged.
    const uint32_t max = 16u * 1024 * 1024;
    switch (part)
    {
        case DSI_PUBLIC:  return info->dsi_public_size <= max ? (long)info->dsi_public_size : 0;
        case DSI_PRIVATE: return info->dsi_private_size <= max ? (long)info->dsi_private_size : 0;
        case DSI_BANNER:  return info->dsi_banner_save ? DSI_BANNER_SAVE_SIZE : 0;
        default:          return 0;
    }
}

bool rom_dsi_has_part(const RomInfo *info, DsiPart part)
{
    return rom_dsi_part_size(info, part) > 0;
}

bool rom_read_icon(const char *path, RomIcon *icon)
{
    memset(icon, 0, sizeof(*icon));
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t hdr[HDR_SIZE];
    static uint8_t data[BANNER_TITLES - BANNER_ICON]; // Bitmap, then palette
    uint32_t banner = 0;
    bool ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && (banner = banner_offset(hdr)) != 0 &&
              fseek(f, (long)banner + BANNER_ICON, SEEK_SET) == 0 &&
              fread(data, 1, sizeof(data), f) == sizeof(data);
    fclose(f);
    if (ok)
        rom_decode_icon(data, data + (BANNER_PALETTE - BANNER_ICON), icon);
    return ok;
}

bool rom_header_crc_ok(const uint8_t *hdr)
{
    // CRC-16/MODBUS, as the DS BIOS's swiCRC16 computes it.
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 0x15E; i++)
    {
        crc ^= hdr[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc == (uint16_t)(hdr[0x15E] | (hdr[0x15F] << 8));
}

bool rom_read_info(const char *path, RomInfo *info)
{
    memset(info, 0, sizeof(*info));

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    uint8_t hdr[ROM_DSI_HEADER_SIZE];
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    if (got < HDR_SIZE)
    {
        fclose(f);
        return false;
    }

    uint32_t banner = banner_offset(hdr);

    static uint8_t titles[BANNER_LANGS * BANNER_TITLE_LEN];
    bool have = banner != 0 && fseek(f, (long)banner + BANNER_TITLES, SEEK_SET) == 0 &&
                fread(titles, 1, sizeof(titles), f) == sizeof(titles);
    fclose(f);
    rom_parse(info, hdr, have ? titles : NULL);
    if (got == sizeof(hdr))
        rom_parse_dsi(info, hdr);
    return true;
}

const char *rom_display_title(const RomInfo *info, const char *fallback)
{
    if (info->banner_title[0] && info->banner_latin)
        return info->banner_title;
    // A Japanese-only banner cannot be drawn: the ROM's file name usually
    // can, and the header title always can.
    if (fallback && fallback[0] && text_is_latin(fallback))
        return fallback;
    if (info->header_title[0])
        return info->header_title;
    if (info->banner_title[0])
        return info->banner_title;
    return fallback ? fallback : "";
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
