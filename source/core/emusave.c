#include "emusave.h"

#include <stdio.h>
#include <string.h>

#include "util.h"

#define DSV_MARKER "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:"
#define DSV_END "|-DESMUME SAVE-|"
#define DSV_FIELDS 24

#define NOCASH_MAGIC "NocashGbaBackupMediaSavDataFile"
#define NOCASH_HEADER 0x50

#define AR1_MAGIC "ARDS000000000001"
#define AR1_HEADER 500
#define AR2_HEADER 0xA4

#define MAX_SAVE (8L * 1024 * 1024)
#define BUF_SIZE 4096

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// no$gba: "NocashGbaBackupMediaSavDataFile", 0x1A, "SRAM" at 0x40, then the
// packing method at 0x44: 0 is raw (size at 0x48, data at 0x4C), 1 is
// run-length packed (packed size at 0x48, size at 0x4C, data at 0x50).
static void probe_nocash(const uint8_t *h, long got, long file, EmuSave *out)
{
    out->format = EMU_NOCASH;
    out->length = -1;
    if (got < 0x4C || h[0x1F] != 0x1A || memcmp(h + 0x40, "SRAM", 4) != 0)
        return;
    uint32_t method = get32(h + 0x44);
    if (method == 0)
    {
        uint32_t length = get32(h + 0x48);
        if (length == 0 || length > (uint32_t)MAX_SAVE)
            return;
        out->offset = 0x4C;
        out->packed_size = (long)length;
        out->length = out->packed_size;
    }
    else if (method == 1 && got >= NOCASH_HEADER)
    {
        out->packed = true;
        out->offset = NOCASH_HEADER;
        uint32_t packed = get32(h + 0x48), length = get32(h + 0x4C);
        // Range-check before any arithmetic: long is 32 bits on the DS.
        if (length == 0 || length > (uint32_t)MAX_SAVE || packed == 0 || packed > length * 2 + 1)
            return;
        out->packed_size = (long)packed;
        out->length = (long)length;
    }
    if (out->length <= 0 || out->length > MAX_SAVE || out->packed_size > file - out->offset)
        out->length = -1;
}

// Action Replay version 1 starts with "ARDS000000000001" and has a 500 byte
// header. Version 2 starts with four zero bytes and has 0xC0 at 0xA3, with
// the data size in 256 byte units at 0xA1.
static void probe_action_replay(const uint8_t *h, long got, long file, EmuSave *out)
{
    out->format = EMU_ACTION_REPLAY;
    out->length = -1;
    if (got >= 16 && memcmp(h, AR1_MAGIC, 16) == 0 && file > AR1_HEADER)
    {
        out->offset = AR1_HEADER;
        out->length = file - AR1_HEADER;
    }
    else if (got >= AR2_HEADER && get32(h) == 0 && h[0xA3] == 0xC0)
    {
        long size = ((long)h[0xA1] << 8) | ((long)h[0xA2] << 16);
        if (size > 0 && size == file - AR2_HEADER)
        {
            out->offset = AR2_HEADER;
            out->length = size;
        }
    }
}

// DeSmuME's footer: the marker text, six 32-bit fields and DSV_END. The data
// is everything before the marker.
static long dsv_data_length(FILE *f, long file)
{
    uint8_t tail[256];
    long n = file < (long)sizeof(tail) ? file : (long)sizeof(tail);
    long end = (long)strlen(DSV_END), marker = (long)strlen(DSV_MARKER);
    if (n < end + marker || fseek(f, file - n, SEEK_SET) != 0 || fread(tail, 1, (size_t)n, f) != (size_t)n ||
        memcmp(tail + n - end, DSV_END, (size_t)end) != 0)
        return -1;
    for (long i = n - end - marker; i >= 0; i--)
    {
        if (memcmp(tail + i, DSV_MARKER, (size_t)marker) == 0)
            return file - n + i;
    }
    return -1;
}

bool emusave_probe(const char *path, EmuSave *out)
{
    memset(out, 0, sizeof(*out));
    long file = file_size(path);
    FILE *f = file >= 0 ? fopen(path, "rb") : NULL;
    if (!f)
        return false;

    uint8_t h[AR2_HEADER];
    long got = (long)fread(h, 1, sizeof(h), f);
    out->format = EMU_RAW;
    out->length = file;
    long dsv;
    if (got >= (long)strlen(NOCASH_MAGIC) && memcmp(h, NOCASH_MAGIC, strlen(NOCASH_MAGIC)) == 0)
        probe_nocash(h, got, file, out);
    else if (has_ext(path, ".duc") || has_ext(path, ".dss"))
        probe_action_replay(h, got, file, out);
    else if ((dsv = dsv_data_length(f, file)) >= 0)
    {
        out->format = EMU_DESMUME;
        out->length = dsv;
    }
    fclose(f);
    return true;
}

const char *emusave_format_name(EmuFormat format)
{
    switch (format)
    {
        case EMU_DESMUME: return "DeSmuME";
        case EMU_NOCASH: return "no$gba";
        case EMU_ACTION_REPLAY: return "Action Replay";
        case EMU_RAW:
        default: return "";
    }
}

typedef struct {
    FILE *f;
    uint8_t buf[BUF_SIZE];
    int len;
    long written;
    bool ok;
} Out;

static void out_put(Out *o, const uint8_t *data, long n)
{
    while (o->ok && n > 0)
    {
        int take = BUF_SIZE - o->len < n ? BUF_SIZE - o->len : (int)n;
        memcpy(o->buf + o->len, data, (size_t)take);
        o->len += take;
        data += take;
        n -= take;
        o->written += take;
        if (o->len == BUF_SIZE)
        {
            o->ok = fwrite(o->buf, 1, BUF_SIZE, o->f) == BUF_SIZE;
            o->len = 0;
        }
    }
}

static void out_fill(Out *o, uint8_t v, long n)
{
    uint8_t run[256];
    memset(run, v, sizeof(run));
    for (; n > 0; n -= (long)sizeof(run))
        out_put(o, run, n < (long)sizeof(run) ? n : (long)sizeof(run));
}

static bool out_flush(Out *o)
{
    if (o->ok && o->len)
        o->ok = fwrite(o->buf, 1, (size_t)o->len, o->f) == (size_t)o->len;
    o->len = 0;
    return o->ok;
}

// no$gba's run-length packing. Each control byte is followed by its data:
//   00        end
//   01..7F    that many bytes, copied
//   80        a byte and a 16-bit count: the byte, repeated
//   81..FF    a byte, repeated (control - 80) times
// DeSmuME reads the 80 run as byte then count; older readers took the
// count first. count_first picks that order, tried when the first fails.
static bool unpack_nocash(FILE *in, const EmuSave *s, Out *o, bool count_first)
{
    long left = s->packed_size;
    while (left-- > 0)
    {
        int control = fgetc(in);
        if (control < 0)
            return false;
        long count;
        if (control == 0)
            return o->written == s->length;
        if (control == 0x80)
        {
            uint8_t r[3];
            if (left < 3 || fread(r, 1, 3, in) != 3)
                return false;
            left -= 3;
            count = count_first ? (r[0] | (r[1] << 8)) : (r[1] | (r[2] << 8));
            if (count == 0 || count > s->length - o->written)
                return false;
            out_fill(o, count_first ? r[2] : r[0], count);
        }
        else if (control > 0x80)
        {
            count = control - 0x80;
            int v = left > 0 ? fgetc(in) : -1;
            if (v < 0 || count > s->length - o->written)
                return false;
            left--;
            out_fill(o, (uint8_t)v, count);
        }
        else
        {
            count = control;
            uint8_t lit[0x80];
            if (count > left || count > s->length - o->written || fread(lit, 1, (size_t)count, in) != (size_t)count)
                return false;
            left -= count;
            out_put(o, lit, count);
        }
        if (!o->ok)
            return false;
    }
    return false; // No end marker
}

static bool copy_data(FILE *in, long n, Out *o)
{
    uint8_t buf[BUF_SIZE];
    while (o->ok && n > 0)
    {
        size_t want = n < BUF_SIZE ? (size_t)n : BUF_SIZE;
        if (fread(buf, 1, want, in) != want)
            return false;
        out_put(o, buf, (long)want);
        n -= (long)want;
    }
    return o->ok;
}

bool emusave_extract(const char *path, const EmuSave *save, const char *out_path)
{
    if (save->length <= 0)
        return false;
    FILE *in = fopen(path, "rb");
    if (!in)
        return false;
    static Out o;
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++)
    {
        memset(&o, 0, sizeof(o));
        o.ok = true;
        o.f = fopen(out_path, "wb");
        if (!o.f || fseek(in, save->offset, SEEK_SET) != 0)
        {
            if (o.f)
                fclose(o.f);
            break;
        }
        if (save->packed)
            ok = unpack_nocash(in, save, &o, attempt == 1);
        else
            ok = copy_data(in, save->length, &o);
        ok = out_flush(&o) && ok && o.written == save->length;
        if (fclose(o.f) != 0)
            ok = false;
        if (!save->packed)
            break; // Only packed data has a second reading
    }
    fclose(in);
    return ok;
}

int emusave_dsv_footer(long size, uint8_t *out)
{
    // DeSmuME's save types, by size: the type is the index in its list, with
    // 0 meaning "detect". Sizes it has no type for (128 KB EEPROM) keep 0 and
    // say how many address bytes the chip takes.
    static const long sizes[] = { 0, 512, 8 * 1024, 64 * 1024, 32 * 1024, 256 * 1024, 512 * 1024,
                                  1024 * 1024, 2 * 1024 * 1024, 4 * 1024 * 1024, 8 * 1024 * 1024 };
    uint32_t type = 0;
    for (uint32_t i = 1; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        if (sizes[i] == size)
            type = i;
    }
    uint32_t addr = size <= 512 ? 1 : size <= 64 * 1024 ? 2 : 3;

    int n = (int)strlen(DSV_MARKER);
    memcpy(out, DSV_MARKER, (size_t)n);
    put32(out + n, (uint32_t)size);      // Bytes written
    put32(out + n + 4, (uint32_t)size);  // Padded size
    put32(out + n + 8, type);
    put32(out + n + 12, addr);
    put32(out + n + 16, (uint32_t)size); // Memory size
    put32(out + n + 20, 0);              // Version
    n += DSV_FIELDS;
    memcpy(out + n, DSV_END, strlen(DSV_END));
    return n + (int)strlen(DSV_END);
}
