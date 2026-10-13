// Minimal INI reader/writer for the settings files kernels keep on the SD
// card (TWiLight's settings.ini, akmenu's globalsettings.ini) and for
// DSavE's own settings. Sections and keys match case-insensitively; values
// are trimmed. Lines that are not "key = value" are preserved on write.

#ifndef DSAVE_INI_H
#define DSAVE_INI_H

#include <stdbool.h>
#include <stddef.h>

// Copy the value of [section] key into out. Returns false (and leaves out
// untouched) when the file, section or key is missing.
bool ini_get(const char *path, const char *section, const char *key,
             char *out, size_t out_size);

// Integer value, or fallback when missing or not a number.
int ini_get_int(const char *path, const char *section, const char *key, int fallback);

// Set [section] key = value, creating the file, section or key as needed.
// Everything else in the file is kept as it was.
bool ini_set(const char *path, const char *section, const char *key, const char *value);

bool ini_set_int(const char *path, const char *section, const char *key, int value);

#endif
