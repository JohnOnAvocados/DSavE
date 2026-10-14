// Byte-level comparison of two saves (raw or .sav.gz), for the hex viewer:
// which ranges differ, so it can highlight them and jump between them.

#ifndef DSAVE_SAVEDIFF_H
#define DSAVE_SAVEDIFF_H

#include <stdbool.h>

#define DIFF_MAX 64
// Differences closer than this are reported as one range.
#define DIFF_MERGE_GAP 16

typedef struct {
    long start, len;
} DiffRange;

typedef struct {
    long size_a, size_b;  // Save data sizes
    long changed;         // Bytes that differ, in all ranges
    int count;            // Ranges found (only the first DIFF_MAX are kept)
    DiffRange range[DIFF_MAX];
} SaveDiff;

// Compare a and b. Where one is shorter, the missing bytes count as 0xFF,
// the padding kernels add, so a 64 KB save and the same save padded to
// 512 KB are equal. Returns false if either can't be read.
bool save_diff(const char *a, const char *b, SaveDiff *out);

// Index of the first range that ends after offset, or count when none.
int save_diff_find(const SaveDiff *d, long offset);

#endif
