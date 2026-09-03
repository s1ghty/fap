#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "fap.h"
#include "json.h"

/*
 * lock.c — read/write fap.lock (JSON), scoped to the fixed schema in
 * CLAUDE.md. See json.c for the underlying scanning helpers.
 */

/* Look up a required string field within [obj, obj_end) and copy it
 * into out. Sets fap_err and returns -1 if missing or malformed. */
static int req_field(const char *obj, const char *obj_end, const char *key,
                      char *out, size_t outsz)
{
    const char *v;
    if (fap_json_find(obj, obj_end, key, &v) < 0)
        return fap_error("lock: package missing \"%s\"", key);
    return fap_json_string(&v, out, outsz);
}

int fap_lock_load(const char *path, FapLock *out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return fap_error("lock: open %s: %s", path, strerror(errno));

    if (fseek(f, 0, SEEK_END) < 0) {
        fclose(f);
        return fap_error("lock: cannot read %s", path);
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) < 0) {
        fclose(f);
        return fap_error("lock: cannot read %s", path);
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return fap_error("lock: out of memory reading %s", path);
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    memset(out, 0, sizeof(*out));

    const char *arr;
    if (fap_json_find(buf, buf + n, "packages", &arr) < 0 || *arr != '[') {
        free(buf);
        return fap_error("lock: %s missing \"packages\" array", path);
    }
    const char *arr_end = fap_json_match(arr);
    if (!arr_end) {
        free(buf);
        return fap_error("lock: %s has unterminated \"packages\" array", path);
    }

    const char *p = arr + 1;
    fap_json_skip_ws(&p);
    while (p < arr_end && *p != ']') {
        if (*p != '{') {
            free(buf);
            return fap_error("lock: expected object in \"packages\"");
        }
        const char *obj_end = fap_json_match(p);
        if (!obj_end) {
            free(buf);
            return fap_error("lock: unterminated package object");
        }
        if (out->count >= FAP_MAX_DEPS) {
            free(buf);
            return fap_error("lock: too many packages (max %d)", FAP_MAX_DEPS);
        }

        FapPackage *pkg = &out->entries[out->count].pkg;
        memset(pkg, 0, sizeof(*pkg));

        char chan[16];
        if (req_field(p, obj_end, "name", pkg->name, sizeof(pkg->name)) < 0 ||
            req_field(p, obj_end, "version", pkg->version, sizeof(pkg->version)) < 0 ||
            req_field(p, obj_end, "channel", chan, sizeof(chan)) < 0 ||
            req_field(p, obj_end, "url", pkg->url, sizeof(pkg->url)) < 0 ||
            req_field(p, obj_end, "sha256", pkg->sha256, sizeof(pkg->sha256)) < 0 ||
            fap_channel_parse(chan, &pkg->channel) < 0 ||
            fap_json_optional_array(p, obj_end, "bin", pkg->bins, &pkg->bins_count, FAP_MAX_BINS) < 0 ||
            fap_json_optional_array(p, obj_end, "libs", pkg->libs, &pkg->libs_count, FAP_MAX_LIBS) < 0 ||
            fap_json_optional_array(p, obj_end, "deps", pkg->deps, &pkg->deps_count, FAP_MAX_PKG_DEPS) < 0 ||
            fap_validate_package(pkg) < 0) {
            free(buf);
            return -1;
        }

        out->count++;
        p = obj_end + 1;
        fap_json_skip_ws(&p);
        if (p < arr_end && *p == ',') {
            p++;
            fap_json_skip_ws(&p);
        }
    }

    free(buf);
    return 0;
}

static void write_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\t': fputs("\\t", f);  break;
        case '\r': fputs("\\r", f);  break;
        default:
            if ((unsigned char)*s < 0x20)
                fprintf(f, "\\u%04x", *s);
            else
                fputc(*s, f);
        }
    }
    fputc('"', f);
}

static void write_json_string_array(FILE *f, const char arr[][FAP_MAX_NAME], int count)
{
    fputc('[', f);
    for (int i = 0; i < count; i++) {
        if (i)
            fputc(',', f);
        write_json_string(f, arr[i]);
    }
    fputc(']', f);
}

int fap_lock_save(const char *path, const FapLock *lock)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return fap_error("lock: open %s for write: %s", path, strerror(errno));

    fprintf(f, "{\n  \"lockfile_version\": 1,\n  \"packages\": [\n");
    for (int i = 0; i < lock->count; i++) {
        const FapPackage *pkg = &lock->entries[i].pkg;
        char chan[16];
        fap_channel_str(pkg->channel, chan, sizeof(chan));

        fprintf(f, "    {\n      \"name\": ");
        write_json_string(f, pkg->name);
        fprintf(f, ",\n      \"version\": ");
        write_json_string(f, pkg->version);
        fprintf(f, ",\n      \"channel\": ");
        write_json_string(f, chan);
        fprintf(f, ",\n      \"url\": ");
        write_json_string(f, pkg->url);
        fprintf(f, ",\n      \"sha256\": ");
        write_json_string(f, pkg->sha256);
        fprintf(f, ",\n      \"bin\": ");
        write_json_string_array(f, pkg->bins, pkg->bins_count);
        fprintf(f, ",\n      \"libs\": ");
        write_json_string_array(f, pkg->libs, pkg->libs_count);
        fprintf(f, ",\n      \"deps\": ");
        write_json_string_array(f, pkg->deps, pkg->deps_count);
        fprintf(f, "\n    }%s\n", (i + 1 < lock->count) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");

    if (fclose(f) != 0)
        return fap_error("lock: write %s: %s", path, strerror(errno));
    return 0;
}

static int lock_index_of(const FapLock *lock, const char *name)
{
    for (int i = 0; i < lock->count; i++)
        if (strcmp(lock->entries[i].pkg.name, name) == 0)
            return i;
    return -1;
}

int fap_lock_orphans(const FapLock *lock, const FapManifest *manifest, FapLock *out)
{
    /* needed[i] tracks whether lock->entries[i] is reachable from the
     * explicit set — either directly requested, or a (transitive)
     * dependency of something that is. */
    unsigned char needed[FAP_MAX_DEPS] = {0};
    int stack[FAP_MAX_DEPS];
    int sp = 0;

    for (int i = 0; i < manifest->deps_count; i++) {
        int idx = lock_index_of(lock, manifest->deps[i].name);
        if (idx >= 0 && !needed[idx]) {
            needed[idx] = 1;
            stack[sp++] = idx;
        }
    }

    while (sp > 0) {
        const FapPackage *pkg = &lock->entries[stack[--sp]].pkg;
        for (int d = 0; d < pkg->deps_count; d++) {
            int idx = lock_index_of(lock, pkg->deps[d]);
            if (idx >= 0 && !needed[idx]) {
                needed[idx] = 1;
                stack[sp++] = idx;
            }
        }
    }

    out->count = 0;
    for (int i = 0; i < lock->count; i++) {
        if (!needed[i]) {
            if (out->count >= FAP_MAX_DEPS)
                return fap_error("lock: too many orphans (max %d)", FAP_MAX_DEPS);
            out->entries[out->count++] = lock->entries[i];
        }
    }
    return 0;
}
