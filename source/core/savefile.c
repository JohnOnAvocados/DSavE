#include "savefile.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

#include "../third_party/miniz/miniz_config.h"
#include "../third_party/miniz/miniz.h"

#define IO_CHUNK 4096

// gzip header (RFC 1952)
#define GZ_ID1 0x1F
#define GZ_ID2 0x8B
#define GZ_CM_DEFLATE 8
#define GZ_FHCRC 0x02
#define GZ_FEXTRA 0x04
#define GZ_FNAME 0x08
#define GZ_FCOMMENT 0x10
#define GZ_OS_UNKNOWN 0xFF

bool savefile_is_gz(const char *path)
{
    return has_ext(path, SAVEFILE_GZ_EXT);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Read a whole small file into memory.
static uint8_t *read_all(const char *path, long *len_out, long max)
{
    long len = file_size(path);
    if (len < 0 || len > max)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = malloc(len ? (size_t)len : 1);
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        *len_out = len;
    return buf;
}

// Decompress a gzip file into a new buffer.
static uint8_t *gunzip_file(const char *path, long *len_out)
{
    long in_len;
    // A compressed save is never bigger than its data plus a little.
    uint8_t *in = read_all(path, &in_len, SAVEFILE_GZ_MAX + 64 * 1024);
    if (!in)
        return NULL;

    uint8_t *out = NULL;
    if (in_len < 18 || in[0] != GZ_ID1 || in[1] != GZ_ID2 || in[2] != GZ_CM_DEFLATE)
        goto done;

    uint8_t flags = in[3];
    long pos = 10;
    if (flags & GZ_FEXTRA)
    {
        if (pos + 2 > in_len)
            goto done;
        pos += 2 + (in[pos] | (in[pos + 1] << 8));
    }
    if (flags & GZ_FNAME)
    {
        while (pos < in_len && in[pos])
            pos++;
        pos++;
    }
    if (flags & GZ_FCOMMENT)
    {
        while (pos < in_len && in[pos])
            pos++;
        pos++;
    }
    if (flags & GZ_FHCRC)
        pos += 2;
    if (pos + 8 > in_len)
        goto done;

    uint32_t want_crc = le32(in + in_len - 8);
    uint32_t want_len = le32(in + in_len - 4);
    if (want_len > (uint32_t)SAVEFILE_GZ_MAX)
        goto done;

    out = malloc(want_len ? want_len : 1);
    if (!out)
        goto done;
    size_t got = tinfl_decompress_mem_to_mem(out, want_len, in + pos, (size_t)(in_len - 8 - pos), 0);
    if (got != want_len || crc32_update(0, out, got) != want_crc)
    {
        free(out);
        out = NULL;
        goto done;
    }
    *len_out = (long)got;

done:
    free(in);
    return out;
}

bool savefile_open(SaveReader *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    if (savefile_is_gz(path))
    {
        r->mem = gunzip_file(path, &r->mem_len);
        return r->mem != NULL;
    }
    r->f = fopen(path, "rb");
    return r->f != NULL;
}

size_t savefile_read(SaveReader *r, void *buf, size_t n)
{
    if (r->f)
        return fread(buf, 1, n, r->f);
    long left = r->mem_len - r->pos;
    if (left <= 0)
        return 0;
    if ((long)n > left)
        n = (size_t)left;
    memcpy(buf, r->mem + r->pos, n);
    r->pos += (long)n;
    return n;
}

bool savefile_seek(SaveReader *r, long offset)
{
    if (r->f)
        return fseek(r->f, offset, SEEK_SET) == 0;
    if (offset < 0 || offset > r->mem_len)
        return false;
    r->pos = offset;
    return true;
}

void savefile_close(SaveReader *r)
{
    if (r->f)
        fclose(r->f);
    free(r->mem);
    memset(r, 0, sizeof(*r));
}

long savefile_size(const char *path)
{
    if (!savefile_is_gz(path))
        return file_size(path);
    // The gzip trailer holds the length; check it by decompressing, since a
    // backup that does not decompress has no usable size.
    SaveReader r;
    if (!savefile_open(&r, path))
        return -1;
    long len = r.mem_len;
    savefile_close(&r);
    return len;
}

bool savefile_crc32(const char *path, uint32_t *crc_out, long *size_out)
{
    if (!savefile_is_gz(path))
        return file_crc32(path, crc_out, size_out);
    SaveReader r;
    if (!savefile_open(&r, path))
        return false;
    if (crc_out)
        *crc_out = crc32_update(0, r.mem, (size_t)r.mem_len);
    if (size_out)
        *size_out = r.mem_len;
    savefile_close(&r);
    return true;
}

bool savefile_tail_is_ff(const char *path, long offset)
{
    SaveReader r;
    if (!savefile_open(&r, path))
        return false;
    bool ff = savefile_seek(&r, offset);
    uint8_t buf[IO_CHUNK];
    size_t n;
    while (ff && (n = savefile_read(&r, buf, sizeof(buf))) > 0)
    {
        for (size_t i = 0; i < n && ff; i++)
            ff = buf[i] == 0xFF;
    }
    if (r.f && ferror(r.f))
        ff = false;
    savefile_close(&r);
    return ff;
}

bool savefile_copy_raw(const char *src, const char *dst)
{
    if (!savefile_is_gz(src))
        return copy_file(src, dst);
    SaveReader r;
    if (!savefile_open(&r, src))
        return false;
    FILE *out = fopen(dst, "wb");
    bool ok = out && fwrite(r.mem, 1, (size_t)r.mem_len, out) == (size_t)r.mem_len;
    if (out && fclose(out) != 0)
        ok = false;
    savefile_close(&r);
    return ok;
}

bool savefile_write_gz(const char *src, const char *dst, uint32_t *crc_out, long *size_out)
{
    SaveReader r;
    if (!savefile_open(&r, src))
        return false;

    // Raw sources are loaded whole; gz sources already are.
    uint8_t *data = r.mem;
    long len = r.mem_len;
    bool own = false;
    if (r.f)
    {
        savefile_close(&r);
        data = read_all(src, &len, SAVEFILE_GZ_MAX);
        if (!data)
            return false;
        own = true;
    }

    // Raw DEFLATE at the default level; the gzip wrapper is added here.
    int flags = (int)tdefl_create_comp_flags_from_zip_params(MZ_DEFAULT_LEVEL, -MZ_DEFAULT_WINDOW_BITS,
                                                             MZ_DEFAULT_STRATEGY);
    size_t comp_len = 0;
    void *comp = tdefl_compress_mem_to_heap(data, (size_t)len, &comp_len, flags);
    uint32_t crc = crc32_update(0, data, (size_t)len);
    if (own)
        free(data);
    else
        savefile_close(&r);
    if (!comp)
        return false;

    uint8_t header[10] = { GZ_ID1, GZ_ID2, GZ_CM_DEFLATE, 0, 0, 0, 0, 0, 0, GZ_OS_UNKNOWN };
    uint8_t trailer[8];
    put_le32(trailer, crc);
    put_le32(trailer + 4, (uint32_t)len);

    FILE *out = fopen(dst, "wb");
    bool ok = out && fwrite(header, 1, sizeof(header), out) == sizeof(header) &&
              fwrite(comp, 1, comp_len, out) == comp_len &&
              fwrite(trailer, 1, sizeof(trailer), out) == sizeof(trailer);
    if (out && fclose(out) != 0)
        ok = false;
    mz_free(comp);

    if (ok && crc_out)
        *crc_out = crc;
    if (ok && size_out)
        *size_out = len;
    return ok;
}
