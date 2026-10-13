// Options for the vendored miniz: no zip archives, no stdio, no time, and no
// zlib-compatible names (which would clash with nothing here, but keep the
// symbol table small).
#ifndef DSAVE_MINIZ_CONFIG_H
#define DSAVE_MINIZ_CONFIG_H

#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

#endif
