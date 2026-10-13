// Reads the identifying fields out of an .nds file's header and banner.

#ifndef DSAVE_ROM_H
#define DSAVE_ROM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char header_title[13]; // 12-byte ASCII title at 0x000, NUL-terminated
    char game_code[5];     // 4-character game code at 0x00C, e.g. "ACTE"
    char banner_title[128]; // Banner title (first line), UTF-8
    bool banner_latin;     // banner_title uses only characters the UI can draw
} RomInfo;

#define ROM_HEADER_SIZE 0x200
#define ROM_BANNER_TITLES_SIZE 0x600 // Six 0x100-byte titles from banner+0x240

// Fill info from a header and, if not NULL, the banner's six titles. Used
// for ROM files and for game cards read from slot 1.
void rom_parse(RomInfo *info, const uint8_t *header, const uint8_t *titles);

// True if the header's CRC-16 (at 0x15E, over 0x000-0x15D) is right. A
// header read from a dirty or half-inserted game card fails this.
bool rom_header_crc_ok(const uint8_t *header);

// A game's 32x32 banner icon, decoded from the DS's 4x4 tiles of 8x8 4-bit
// pixels. DSi animated icons are shown by their first frame, which every
// banner also keeps in the DS-format slot.
#define ROM_ICON_SIZE 32

typedef struct {
    bool valid;
    uint8_t px[ROM_ICON_SIZE * ROM_ICON_SIZE]; // Palette index per pixel; 0 is transparent
    uint16_t pal[16];                          // BGR555
} RomIcon;

// Decode the 0x200-byte bitmap and 0x20-byte palette at banner+0x20.
void rom_decode_icon(const uint8_t *bitmap, const uint8_t *palette, RomIcon *icon);

// Read the icon of the ROM at path. Returns false (icon->valid false) if the
// ROM has no banner.
bool rom_read_icon(const char *path, RomIcon *icon);

// Fill info from the ROM at path. Returns false if the file is not readable
// or is too short to be an .nds. A missing or odd banner is not an error;
// banner_title is then left empty. The banner title is the first of
// English, French, German, Italian, Spanish and Japanese that the UI can
// draw.
bool rom_read_info(const char *path, RomInfo *info);

// True if every character of the UTF-8 text is Latin-1 or one of the few
// punctuation marks the UI fonts include.
bool text_is_latin(const char *utf8);

// Best human-readable title: a drawable banner title, else the ROM file name
// (fallback) if drawable, else the header title.
const char *rom_display_title(const RomInfo *info, const char *fallback);

// True for game codes that are not real retail codes (homebrew uses
// "####" or zeros, and has no save to manage).
bool rom_is_homebrew(const RomInfo *info);

#endif
