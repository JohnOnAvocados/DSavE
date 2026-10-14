// Save files from emulators and PC tools. DSavE keeps raw saves (the chip's
// bytes, as flashcarts use them); this reads the other formats a DS save
// turns up in and writes DeSmuME's .dsv for export.
//
//   - Raw .sav: melonDS, flashcarts, Checkpoint and JKSM on a 3DS.
//   - DeSmuME and DraStic .dsv: the raw data followed by a text marker, six
//     32-bit fields and "|-DESMUME SAVE-|".
//   - no$gba: a 0x4C or 0x50 byte header, then the data, raw or packed with
//     no$gba's run-length scheme.
//   - Action Replay .duc and .dss: a 500 byte (version 1) or 0xA4 byte
//     (version 2) header, then the raw data.
//
// Formats follow DeSmuME's importers (src/mc.cpp).

#ifndef DSAVE_EMUSAVE_H
#define DSAVE_EMUSAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EMU_RAW,
    EMU_DESMUME,
    EMU_NOCASH,
    EMU_ACTION_REPLAY,
} EmuFormat;

typedef struct {
    EmuFormat format;
    long offset;      // Where the save data starts in the file
    long length;      // Save data length, -1 when the header is damaged
    bool packed;      // no$gba run-length packing
    long packed_size; // Bytes of packed data after offset
} EmuSave;

// Work out a file's format from its contents. Action Replay files are only
// recognised by their .duc or .dss extension, since a version 2 header is
// too plain to tell apart from save data. Returns false when the file
// can't be read.
bool emusave_probe(const char *path, EmuSave *out);

// Short name for the format ("no$gba"), or "" for a raw save.
const char *emusave_format_name(EmuFormat format);

// Write the save data of a probed file to out_path as a raw save. Returns
// false when reading or writing fails, or the packed data is damaged.
bool emusave_extract(const char *path, const EmuSave *save, const char *out_path);

// DeSmuME's footer for a raw save of `size` bytes (a DS save size), as
// DeSmuME and DraStic expect after the data in a .dsv. Returns its length.
#define EMUSAVE_DSV_FOOTER_MAX 128
int emusave_dsv_footer(long size, uint8_t *out);

#endif
