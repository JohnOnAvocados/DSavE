// Builds the vendored miniz 3.0.2 (MIT, see LICENSE): the raw DEFLATE
// compressor and decompressor that DSavE's gzip backups use, and the zip
// archive code that "Export all" uses.
// miniz.inc is the upstream miniz.c, renamed so the build does not pick it
// up twice.

#include "miniz_config.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "miniz.inc"
#pragma GCC diagnostic pop
