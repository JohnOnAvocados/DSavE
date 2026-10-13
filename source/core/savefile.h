// Reading save data from either a raw .sav or a gzip-compressed .sav.gz.
//
// Compressed backups are standard gzip files, so `gunzip` on a PC turns them
// back into raw saves. They are only used for saves up to SAVEFILE_GZ_MAX,
// because the DS decompresses a whole save in memory (4 MB of RAM on a DS
// Lite) and the few larger saves (Band Brothers DX's 8 MB) stay raw.

#ifndef DSAVE_SAVEFILE_H
#define DSAVE_SAVEFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SAVEFILE_GZ_MAX (1024L * 1024)
#define SAVEFILE_GZ_EXT ".gz"

typedef struct {
    FILE *f;           // Raw file, or NULL when reading from mem
    uint8_t *mem;      // Decompressed data of a .gz
    long mem_len;
    long pos;
} SaveReader;

// True if path ends in ".gz".
bool savefile_is_gz(const char *path);

// Open a save for reading; .gz files are decompressed and checked against
// their gzip CRC and length. Returns false on any error.
bool savefile_open(SaveReader *r, const char *path);
size_t savefile_read(SaveReader *r, void *buf, size_t n);
bool savefile_seek(SaveReader *r, long offset);
void savefile_close(SaveReader *r);

// Length of the save data (decompressed length for .gz), or -1.
long savefile_size(const char *path);

// CRC-32 and length of the save data.
bool savefile_crc32(const char *path, uint32_t *crc_out, long *size_out);

// True if every byte of the save data from offset on is 0xFF.
bool savefile_tail_is_ff(const char *path, long offset);

// Copy save data from src (raw or .gz) to dst as raw bytes.
bool savefile_copy_raw(const char *src, const char *dst);

// Compress src's save data into a gzip file at dst. Fails for data larger
// than SAVEFILE_GZ_MAX. crc_out/size_out describe the uncompressed data.
bool savefile_write_gz(const char *src, const char *dst, uint32_t *crc_out, long *size_out);

#endif
