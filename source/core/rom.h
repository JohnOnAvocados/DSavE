// Reads the identifying fields out of an .nds file's header and banner.

#ifndef DSAVE_ROM_H
#define DSAVE_ROM_H

#include <stdbool.h>

typedef struct {
    char header_title[13]; // 12-byte ASCII title at 0x000, NUL-terminated
    char game_code[5];     // 4-character game code at 0x00C, e.g. "ACTE"
    char banner_title[64]; // English banner title (first line), ASCII-folded
} RomInfo;

// Fill info from the ROM at path. Returns false if the file is not readable
// or is too short to be an .nds. A missing or odd banner is not an error;
// banner_title is then left empty.
bool rom_read_info(const char *path, RomInfo *info);

// Best human-readable title: banner title if present, else header title,
// else the ROM file name.
const char *rom_display_title(const RomInfo *info, const char *fallback);

// True for game codes that are not real retail codes (homebrew uses
// "####" or zeros, and has no save to manage).
bool rom_is_homebrew(const RomInfo *info);

#endif
