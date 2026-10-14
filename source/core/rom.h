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

    // DSiWare (see rom_parse_dsi): a title installed to the DSi's NAND
    // rather than sold on a game card. It has no save chip; it keeps up to
    // three save files, each of a size fixed by its header.
    bool dsiware;
    uint32_t dsi_public_size;  // public.sav, 0 for none
    uint32_t dsi_private_size; // private.sav, 0 for none
    bool dsi_banner_save;      // Has a banner.sav
} RomInfo;

#define ROM_HEADER_SIZE 0x200
// The DSi header goes on past the DS one; DSavE reads up to the save sizes.
#define ROM_DSI_HEADER_SIZE 0x240

// The save files of a DSiWare title. On a DSi they are public.sav,
// private.sav and banner.sav in the title's NAND folder; loaders on an SD
// card keep them beside the ROM instead (see kernel.h).
typedef enum {
    DSI_PUBLIC,  // FAT12 image, size from the header
    DSI_PRIVATE, // FAT12 image, size from the header
    DSI_BANNER,  // 16 KB, an animated icon the title can change
    DSI_PARTS
} DsiPart;

#define DSI_BANNER_SAVE_SIZE 0x4000

// Fill the DSiWare fields of info from a header of ROM_DSI_HEADER_SIZE
// bytes. A title is DSiWare when its unit code (0x012) says it runs in DSi
// mode and its title ID's high word (0x234) is 0003xxxx with bit 2 set, the
// "installed to NAND" bit (00030004 DSiWare, 00030005, 00030015 and
// 00030017 system titles; 00030000 is a game card). The save sizes are at
// 0x238 (public) and 0x23C (private), and bit 2 of 0x1BF means it has a
// banner save. Offsets as TWiLight Menu++ (sNDSHeaderExt), nds-bootstrap
// and Pico Loader (nds_header_twl_t) read them; Pico Loader's IsDsiWare
// uses the same title ID bit.
void rom_parse_dsi(RomInfo *info, const uint8_t *header);

// Whether a DSiWare title uses this save file.
bool rom_dsi_has_part(const RomInfo *info, DsiPart part);

// The size of a save file the title uses, or 0.
long rom_dsi_part_size(const RomInfo *info, DsiPart part);
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
// draw. The DSiWare fields come from the DSi header (rom_parse_dsi).
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
