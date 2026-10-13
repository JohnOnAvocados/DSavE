#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Chunk size for file copies and CRCs. Small enough to sit on the stack of
// the DS ARM9 comfortably, big enough that FAT overhead stays low.
#define IO_CHUNK 4096

bool path_join(char *out, size_t out_size, const char *a, const char *b)
{
    size_t la = strlen(a);
    bool need_slash = la > 0 && a[la - 1] != '/';
    while (*b == '/')
        b++;
    int n = snprintf(out, out_size, "%s%s%s", a, need_slash ? "/" : "", b);
    return n >= 0 && (size_t)n < out_size;
}

void path_basename(char *out, size_t out_size, const char *path)
{
    const char *slash = strrchr(path, '/');
    snprintf(out, out_size, "%s", slash ? slash + 1 : path);
}

void path_dirname(char *out, size_t out_size, const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
    {
        snprintf(out, out_size, ".");
        return;
    }
    size_t len = (size_t)(slash - path);
    // Keep the slash for a drive root such as "sd:/" or "/".
    if (len == 0 || path[len - 1] == ':')
        len++;
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

void path_strip_ext(char *name)
{
    char *dot = strrchr(name, '.');
    if (dot && dot != name)
        *dot = '\0';
}

bool has_ext(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    if (ln < le)
        return false;
    const char *tail = name + ln - le;
    for (size_t i = 0; i < le; i++)
    {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i]))
            return false;
    }
    return true;
}

void sanitize_name(char *out, size_t out_size, const char *in, size_t max_len)
{
    static const char reserved[] = "\\/:*?\"<>|";
    size_t n = 0;
    if (max_len >= out_size)
        max_len = out_size - 1;

    // Accented Latin-1 letters (U+00C0-U+00FF) fold to their base letter,
    // so "Pok\u00e9mon" becomes "Pokemon".
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo_ouuuuyty";

    for (const unsigned char *p = (const unsigned char *)in; *p && n < max_len; p++)
    {
        unsigned char c = *p;
        if ((c == 0xC3) && (p[1] & 0xC0) == 0x80)
        {
            // Two-byte UTF-8 for U+00C0-U+00FF.
            c = (unsigned char)latin1[(p[1] & 0x3F)];
            p++;
        }
        else if (c >= 0xC0)
        {
            // Any other UTF-8 character becomes one '_'.
            while ((p[1] & 0xC0) == 0x80)
                p++;
            c = '_';
        }
        if (c < 0x20 || c > 0x7E || strchr(reserved, c))
            c = '_';
        // Collapse runs of spaces/underscores so titles stay readable.
        if ((c == ' ' || c == '_') && n > 0 && out[n - 1] == c)
            continue;
        out[n++] = (char)c;
    }
    out[n] = '\0';

    // Trim leading/trailing spaces and dots, which FAT and some kernels reject.
    size_t start = 0;
    while (out[start] == ' ' || out[start] == '.')
        start++;
    if (start)
        memmove(out, out + start, strlen(out + start) + 1);
    n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.'))
        out[--n] = '\0';

    if (n == 0)
        snprintf(out, out_size, "unnamed");
}

bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    return (long)st.st_size;
}

bool mkdir_p(const char *path)
{
    char buf[DSAVE_PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
        return false;

    // Skip past a drive prefix ("sd:/") so we never try to mkdir "sd:".
    char *p = strstr(buf, ":/");
    p = p ? p + 2 : buf + (buf[0] == '/' ? 1 : 0);

    for (; *p; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (!dir_exists(buf) && mkdir(buf, 0777) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    if (!dir_exists(buf) && mkdir(buf, 0777) != 0 && errno != EEXIST)
        return false;
    return dir_exists(buf);
}

bool remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return remove(path) == 0;

    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[DSAVE_PATH_MAX];
        if (!path_join(child, sizeof(child), path, e->d_name))
        {
            ok = false;
            continue;
        }
        if (dir_exists(child))
            ok = remove_tree(child) && ok;
        else
            ok = (remove(child) == 0) && ok;
    }
    closedir(d);
    return rmdir(path) == 0 && ok;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }

    const uint8_t *p = data;
    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool file_crc32(const char *path, uint32_t *crc_out, long *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    uint8_t buf[IO_CHUNK];
    uint32_t crc = 0;
    long total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        crc = crc32_update(crc, buf, n);
        total += (long)n;
    }
    bool ok = !ferror(f);
    fclose(f);

    if (ok)
    {
        if (crc_out)
            *crc_out = crc;
        if (size_out)
            *size_out = total;
    }
    return ok;
}

bool copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "wb");
    if (!out)
    {
        fclose(in);
        return false;
    }

    uint8_t buf[IO_CHUNK];
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        if (fwrite(buf, 1, n, out) != n)
        {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    fclose(in);
    if (fclose(out) != 0)
        ok = false;
    return ok;
}

bool file_is_blank(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    uint8_t buf[IO_CHUNK];
    int first = -1;
    bool blank = true;
    size_t n;
    while (blank && (n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (first < 0)
        {
            first = buf[0];
            if (first != 0x00 && first != 0xFF)
                blank = false;
        }
        for (size_t i = 0; blank && i < n; i++)
        {
            if (buf[i] != first)
                blank = false;
        }
    }
    fclose(f);
    // An empty file is not a save, but it is also not "blank data".
    return blank && first >= 0;
}

void timestamp_now(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t)
    {
        snprintf(out, out_size, "backup");
        return;
    }
    strftime(out, out_size, "%Y-%m-%d_%H-%M-%S", t);
}

bool is_valid_save_size(long size)
{
    if (size < 512 || size > 8L * 1024 * 1024)
        return false;
    return (size & (size - 1)) == 0;
}
