#include "ini.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

#define INI_LINE_MAX 512
// Settings files are small; this bounds the rewrite buffer on the DS.
#define INI_FILE_MAX (64 * 1024)

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

// If line is "[name]", copy name into out and return true.
static bool parse_section(char *line, char *out, size_t out_size)
{
    char *t = trim(line);
    size_t n = strlen(t);
    if (n < 2 || t[0] != '[' || t[n - 1] != ']')
        return false;
    t[n - 1] = '\0';
    snprintf(out, out_size, "%s", trim(t + 1));
    return true;
}

// If line is "key = value", point *key and *value into it and return true.
static bool parse_pair(char *line, char **key, char **value)
{
    char *t = trim(line);
    if (*t == ';' || *t == '#' || *t == '\0')
        return false;
    char *eq = strchr(t, '=');
    if (!eq)
        return false;
    *eq = '\0';
    *key = trim(t);
    *value = trim(eq + 1);
    return **key != '\0';
}

bool ini_get(const char *path, const char *section, const char *key,
             char *out, size_t out_size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    char line[INI_LINE_MAX], current[64] = "";
    bool found = false;
    while (!found && fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char copy[INI_LINE_MAX];
        snprintf(copy, sizeof(copy), "%s", line);
        if (parse_section(copy, current, sizeof(current)))
            continue;
        char *k, *v;
        if (strcasecmp(current, section) == 0 && parse_pair(line, &k, &v) &&
            strcasecmp(k, key) == 0)
        {
            snprintf(out, out_size, "%s", v);
            found = true;
        }
    }
    fclose(f);
    return found;
}

int ini_get_int(const char *path, const char *section, const char *key, int fallback)
{
    char buf[32];
    if (!ini_get(path, section, key, buf, sizeof(buf)))
        return fallback;
    char *end;
    long v = strtol(buf, &end, 10);
    return (end == buf) ? fallback : (int)v;
}

bool ini_set(const char *path, const char *section, const char *key, const char *value)
{
    // Read the whole file, then write it back with the one line changed.
    char *src = NULL;
    long len = 0;
    FILE *f = fopen(path, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len < 0 || len > INI_FILE_MAX)
        {
            fclose(f);
            return false;
        }
        src = malloc((size_t)len + 1);
        if (!src || fread(src, 1, (size_t)len, f) != (size_t)len)
        {
            free(src);
            fclose(f);
            return false;
        }
        src[len] = '\0';
        fclose(f);
    }

    size_t cap = (size_t)len + strlen(section) + strlen(key) + strlen(value) + 16;
    char *dst = malloc(cap);
    if (!dst)
    {
        free(src);
        return false;
    }
    size_t used = 0;
#define EMIT(...) used += (size_t)snprintf(dst + used, cap - used, __VA_ARGS__)

    char current[64] = "";
    bool in_section = false, section_seen = false, done = false;
    const char *p = src ? src : "";
    while (*p)
    {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[INI_LINE_MAX];
        snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) ? n : sizeof(line) - 1), p);
        line[strcspn(line, "\r")] = '\0';
        p += n + (nl ? 1 : 0);

        char copy[INI_LINE_MAX];
        snprintf(copy, sizeof(copy), "%s", line);
        if (parse_section(copy, current, sizeof(current)))
        {
            // Leaving our section without finding the key: add it here.
            if (in_section && !done)
            {
                EMIT("%s = %s\n", key, value);
                done = true;
            }
            in_section = strcasecmp(current, section) == 0;
            section_seen |= in_section;
            EMIT("%s\n", line);
            continue;
        }

        snprintf(copy, sizeof(copy), "%s", line);
        char *k, *v;
        if (in_section && !done && parse_pair(copy, &k, &v) && strcasecmp(k, key) == 0)
        {
            EMIT("%s = %s\n", k, value);
            done = true;
            continue;
        }
        EMIT("%s\n", line);
    }
    if (!done)
    {
        if (!section_seen)
            EMIT("%s[%s]\n", used ? "\n" : "", section);
        EMIT("%s = %s\n", key, value);
    }
#undef EMIT
    free(src);

    bool ok = false;
    if (used < cap)
    {
        f = fopen(path, "wb");
        if (f)
        {
            ok = fwrite(dst, 1, used, f) == used;
            ok = (fclose(f) == 0) && ok;
        }
    }
    free(dst);
    return ok;
}

bool ini_set_int(const char *path, const char *section, const char *key, int value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    return ini_set(path, section, key, buf);
}
