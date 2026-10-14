// Options for the vendored miniz: zip archives through callbacks only (no
// stdio), and no zlib-compatible names (which would clash with nothing here,
// but keep the symbol table small). Zip entries are dated with time(), so
// they carry the console clock.
#ifndef DSAVE_MINIZ_CONFIG_H
#define DSAVE_MINIZ_CONFIG_H

#define MINIZ_NO_STDIO
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

#endif
