// Small portable helpers shared by the engine. Everything here is plain C99
// plus POSIX file calls, so it builds for the DS and for host-side tests.

#ifndef DSAVE_UTIL_H
#define DSAVE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSAVE_PATH_MAX 256

// Join a and b with exactly one '/' between them. Returns false on overflow.
bool path_join(char *out, size_t out_size, const char *a, const char *b);

// Copy the last path component of path (without directory) into out.
void path_basename(char *out, size_t out_size, const char *path);

// Copy the directory part of path (without trailing '/') into out.
void path_dirname(char *out, size_t out_size, const char *path);

// Strip a trailing extension (".nds") from name in place.
void path_strip_ext(char *name);

// Case-insensitive suffix test, e.g. has_ext("A.NDS", ".nds").
bool has_ext(const char *name, const char *ext);

// Make a name safe for FAT and for every flashcart kernel: printable ASCII
// only, no reserved characters, no leading/trailing spaces or dots, and at
// most max_len characters. Empty results become "unnamed".
void sanitize_name(char *out, size_t out_size, const char *in, size_t max_len);

bool file_exists(const char *path);
bool dir_exists(const char *path);

// Size of a regular file, or -1 if it cannot be read.
long file_size(const char *path);

// mkdir -p. Returns true if the directory exists afterwards.
bool mkdir_p(const char *path);

// Remove a directory and everything in it (one level of files is all the
// engine ever creates, but this handles nesting too).
bool remove_tree(const char *path);

// CRC-32 (IEEE 802.3), incremental: pass 0 as crc to start.
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

// CRC-32 of a whole file. Returns false if it cannot be read.
bool file_crc32(const char *path, uint32_t *crc_out, long *size_out);

// Copy src to dst in chunks, creating or truncating dst.
bool copy_file(const char *src, const char *dst);

// True if every byte of the file is the same value and that value is 0x00 or
// 0xFF, which is what an erased or never-written save chip looks like.
bool file_is_blank(const char *path);

// "YYYY-MM-DD_HH-MM-SS" for the current local time.
void timestamp_now(char *out, size_t out_size);

// True if size is a plausible DS save chip size: a power of two from 512 B
// (tiny EEPROM) up to 8 MB (Band Brothers DX FLASH).
bool is_valid_save_size(long size);

#endif
