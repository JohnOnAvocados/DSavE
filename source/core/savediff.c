#include "savediff.h"

#include <string.h>

#include "savefile.h"

#define CHUNK 4096

bool save_diff(const char *a, const char *b, SaveDiff *out)
{
    memset(out, 0, sizeof(*out));
    out->size_a = savefile_size(a);
    out->size_b = savefile_size(b);
    if (out->size_a < 0 || out->size_b < 0)
        return false;
    SaveReader ra, rb;
    if (!savefile_open(&ra, a))
        return false;
    if (!savefile_open(&rb, b))
    {
        savefile_close(&ra);
        return false;
    }

    long len = out->size_a > out->size_b ? out->size_a : out->size_b;
    static uint8_t ba[CHUNK], bb[CHUNK];
    bool ok = true;
    long cur_start = -1, cur_end = -1; // The range being built, [start, end)
    for (long pos = 0; ok && pos < len; pos += CHUNK)
    {
        size_t want = (size_t)(len - pos < CHUNK ? len - pos : CHUNK);
        memset(ba, 0xFF, want);
        memset(bb, 0xFF, want);
        long ha = out->size_a - pos, hb = out->size_b - pos;
        size_t na = ha <= 0 ? 0 : (size_t)ha < want ? (size_t)ha : want;
        size_t nb = hb <= 0 ? 0 : (size_t)hb < want ? (size_t)hb : want;
        if (savefile_read(&ra, ba, na) != na || savefile_read(&rb, bb, nb) != nb)
        {
            ok = false;
            break;
        }
        for (size_t i = 0; i < want; i++)
        {
            if (ba[i] == bb[i])
                continue;
            long at = pos + (long)i;
            out->changed++;
            if (cur_start >= 0 && at - cur_end < DIFF_MERGE_GAP)
            {
                cur_end = at + 1;
                continue;
            }
            if (cur_start >= 0)
            {
                if (out->count < DIFF_MAX)
                    out->range[out->count] = (DiffRange){ cur_start, cur_end - cur_start };
                out->count++;
            }
            cur_start = at;
            cur_end = at + 1;
        }
    }
    if (ok && cur_start >= 0)
    {
        if (out->count < DIFF_MAX)
            out->range[out->count] = (DiffRange){ cur_start, cur_end - cur_start };
        out->count++;
    }
    savefile_close(&ra);
    savefile_close(&rb);
    return ok;
}

int save_diff_find(const SaveDiff *d, long offset)
{
    int kept = d->count < DIFF_MAX ? d->count : DIFF_MAX;
    for (int i = 0; i < kept; i++)
    {
        if (d->range[i].start + d->range[i].len > offset)
            return i;
    }
    return kept;
}
