#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include "fap.h"
#include "json.h"

/*
 * registry.c — fetch + parse a channel index (see CLAUDE.md's "Channel
 * index format") via libcurl. Parsing reuses json.c's flat scanning
 * helpers, same as lock.c.
 *
 * stable defaults to fap's own registry (FAP_DEFAULT_STABLE_INDEX_URL)
 * so fap works with zero configuration; FAP_STABLE_INDEX_URL still
 * overrides it for a private/custom registry. edge has no default —
 * no edge.json exists in the registry yet, so defaulting it would
 * just point at a 404 — FAP_EDGE_INDEX_URL is required until one does.
 *
 * Every fetch is cached locally under <root>/index-cache/<channel>.json
 * (mtime doubling as the "fetched at" timestamp — no separate metadata
 * file needed). A fetch within FAP_INDEX_TTL seconds (default 1 hour,
 * override with FAP_INDEX_TTL) reads the cache instead of hitting the
 * network at all — every command that touches the index (search,
 * install, info, sync, ...) was re-downloading the whole thing from
 * scratch every single time before this, dwarfing actual work for
 * anything but a large package download. A failed network fetch with
 * a cache present (even a stale one) falls back to it rather than
 * hard-failing — better a slightly outdated package list than no
 * result at all for a transient network hiccup, same reasoning apt
 * keeps working from its last successful `apt update` for.
 */

#define DEFAULT_INDEX_TTL_SECONDS 3600

static int index_ttl_seconds(void)
{
    const char *env = getenv("FAP_INDEX_TTL");
    if (env && *env) {
        char *end;
        long v = strtol(env, &end, 10);
        if (*end == '\0' && v >= 0)
            return (int)v;
    }
    return DEFAULT_INDEX_TTL_SECONDS;
}

static int cache_path(FapChannel channel, char *buf, size_t bufsz)
{
    char sub[64];
    snprintf(sub, sizeof(sub), "index-cache/%s.json",
              channel == FAP_CHANNEL_EDGE ? "edge" : "stable");
    return fap_root_path(sub, buf, bufsz);
}

/* 1 if path exists and was modified within ttl_seconds of now, 0
 * otherwise (missing file, stat failure, or genuinely stale) —
 * never an error condition on its own, just "don't use the cache". */
static int cache_is_fresh(const char *path, int ttl_seconds)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return 0;
    return (time(NULL) - st.st_mtime) < ttl_seconds;
}

static int read_whole_file(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return fap_error("registry: open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) < 0) {
        fclose(f);
        return fap_error("registry: cannot read %s", path);
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) < 0) {
        fclose(f);
        return fap_error("registry: cannot read %s", path);
    }
    char *buf = malloc((size_t)sz > 0 ? (size_t)sz + 1 : 1);
    if (!buf) {
        fclose(f);
        return fap_error("registry: out of memory reading %s", path);
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return fap_error("registry: short read on %s", path);
    }
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return 0;
}

/* Atomic write (temp file + rename, same pattern as everywhere else
 * in this codebase) so a crash or concurrent fap invocation never
 * sees a half-written cache file. Best-effort: a failure here doesn't
 * fail the caller — the freshly-fetched data it already has is still
 * good, this would just mean the next command hits the network again
 * too, no worse than caching having never existed. */
static void write_cache(const char *path, const char *data, size_t len)
{
    char dir[FAP_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (fap_mkdir_p(dir) < 0)
            return;
    }
    char tmp[FAP_MAX_PATH];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    size_t written = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || written != len) {
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) < 0)
        unlink(tmp);
}

struct fetch_buf {
    char   *data;
    size_t  len;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct fetch_buf *b = userdata;
    size_t add = size * nmemb;
    char *n = realloc(b->data, b->len + add + 1);
    if (!n)
        return 0; /* signals error to curl, aborts the transfer */
    b->data = n;
    memcpy(b->data + b->len, ptr, add);
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

const char *fap_channel_index_url(FapChannel channel)
{
    const char *env = getenv(channel == FAP_CHANNEL_EDGE
                              ? "FAP_EDGE_INDEX_URL" : "FAP_STABLE_INDEX_URL");
    if (env && *env)
        return env;
    return channel == FAP_CHANNEL_STABLE ? FAP_DEFAULT_STABLE_INDEX_URL : NULL;
}

static int index_url(FapChannel channel, char *buf, size_t bufsz)
{
    const char *url = fap_channel_index_url(channel);
    if (!url)
        return fap_error("registry: no index URL configured for this channel; set %s",
                          channel == FAP_CHANNEL_EDGE ? "FAP_EDGE_INDEX_URL" : "FAP_STABLE_INDEX_URL");
    if ((size_t)snprintf(buf, bufsz, "%s", url) >= bufsz)
        return fap_error("registry: index URL is too long");
    return 0;
}

static int req_field(const char *obj, const char *obj_end, const char *key,
                      char *out, size_t outsz)
{
    const char *v;
    if (fap_json_find(obj, obj_end, key, &v) < 0)
        return fap_error("registry: package missing \"%s\"", key);
    return fap_json_string(&v, out, outsz);
}

static int parse_package(const char *obj, const char *obj_end,
                          FapChannel channel, FapPackage *pkg)
{
    memset(pkg, 0, sizeof(*pkg));
    pkg->channel = channel;

    if (req_field(obj, obj_end, "name", pkg->name, sizeof(pkg->name)) < 0)
        return -1;
    if (req_field(obj, obj_end, "version", pkg->version, sizeof(pkg->version)) < 0)
        return -1;
    if (req_field(obj, obj_end, "url", pkg->url, sizeof(pkg->url)) < 0)
        return -1;
    if (req_field(obj, obj_end, "sha256", pkg->sha256, sizeof(pkg->sha256)) < 0)
        return -1;

    const char *v;
    if (fap_json_find(obj, obj_end, "description", &v) == 0)
        fap_json_string(&v, pkg->description, sizeof(pkg->description));
    if (fap_json_find(obj, obj_end, "desktop_type", &v) == 0)
        fap_json_string(&v, pkg->desktop_type, sizeof(pkg->desktop_type));
    if (fap_json_find(obj, obj_end, "desktop_name", &v) == 0)
        fap_json_string(&v, pkg->desktop_name, sizeof(pkg->desktop_name));
    if (fap_json_find(obj, obj_end, "icon", &v) == 0)
        fap_json_string(&v, pkg->icon, sizeof(pkg->icon));

    if (fap_json_optional_array(obj, obj_end, "bin", pkg->bins, &pkg->bins_count, FAP_MAX_BINS) < 0)
        return -1;
    if (fap_json_optional_array(obj, obj_end, "libs", pkg->libs, &pkg->libs_count, FAP_MAX_LIBS) < 0)
        return -1;
    if (fap_json_optional_array(obj, obj_end, "deps", pkg->deps, &pkg->deps_count, FAP_MAX_PKG_DEPS) < 0)
        return -1;

    return 0;
}

/* Parse a channel index document (see CLAUDE.md) into out. Exposed
 * (not static) so it can be exercised directly without a network
 * fetch, both by callers and by tests. */
int fap_index_parse(const char *json, size_t len, FapChannel channel, FapIndex *out)
{
    memset(out, 0, sizeof(*out));
    out->channel = channel;

    const char *arr;
    if (fap_json_find(json, json + len, "packages", &arr) < 0 || *arr != '[')
        return fap_error("registry: index missing \"packages\" array");
    const char *arr_end = fap_json_match(arr);
    if (!arr_end)
        return fap_error("registry: index has unterminated \"packages\" array");

    const char *p = arr + 1;
    fap_json_skip_ws(&p);
    while (p < arr_end && *p != ']') {
        if (*p != '{')
            return fap_error("registry: expected object in \"packages\"");
        const char *obj_end = fap_json_match(p);
        if (!obj_end)
            return fap_error("registry: unterminated package object");
        if (out->count >= FAP_MAX_PKGS)
            return fap_error("registry: too many packages (max %d)", FAP_MAX_PKGS);

        if (parse_package(p, obj_end, channel, &out->pkgs[out->count]) < 0)
            return -1;
        out->count++;

        p = obj_end + 1;
        fap_json_skip_ws(&p);
        if (p < arr_end && *p == ',') {
            p++;
            fap_json_skip_ws(&p);
        }
    }

    return 0;
}

/* The actual network round-trip, unchanged from before caching existed
 * except for the name — split out so fap_index_fetch can call it only
 * when the cache doesn't already answer the question. */
static int fetch_from_network(const char *url, char **out_data, size_t *out_len)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return fap_error("registry: curl_easy_init failed");

    struct fetch_buf buf = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fap/" FAP_VERSION);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        free(buf.data);
        return fap_error("registry: fetch %s: %s", url, curl_easy_strerror(res));
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (status != 0 && status != 200) {
        free(buf.data);
        return fap_error("registry: %s returned HTTP %ld", url, status);
    }
    if (!buf.data)
        return fap_error("registry: empty response from %s", url);

    *out_data = buf.data;
    *out_len = buf.len;
    return 0;
}

int fap_index_fetch(FapChannel channel, FapIndex *out)
{
    char url[FAP_MAX_URL];
    if (index_url(channel, url, sizeof(url)) < 0)
        return -1;

    char cfile[FAP_MAX_PATH];
    int have_cache_path = cache_path(channel, cfile, sizeof(cfile)) == 0;

    if (have_cache_path && cache_is_fresh(cfile, index_ttl_seconds())) {
        char *data; size_t len;
        if (read_whole_file(cfile, &data, &len) == 0) {
            int rc = fap_index_parse(data, len, channel, out);
            free(data);
            if (rc == 0)
                return 0;
            /* cache file corrupt/unparseable — fall through and refetch */
        }
    }

    char *data; size_t len;
    int rc = fetch_from_network(url, &data, &len);
    if (rc < 0) {
        /* Network fetch failed — a stale cache is still better than
         * nothing for a transient hiccup. Only if one actually exists;
         * otherwise the original network error is the right thing to
         * report. */
        if (have_cache_path) {
            char *cached; size_t clen;
            if (read_whole_file(cfile, &cached, &clen) == 0) {
                int crc = fap_index_parse(cached, clen, channel, out);
                free(cached);
                if (crc == 0)
                    return 0;
            }
        }
        return rc;
    }

    rc = fap_index_parse(data, len, channel, out);
    if (rc == 0 && have_cache_path)
        write_cache(cfile, data, len);
    free(data);
    return rc;
}

int fap_index_find(const FapIndex *idx, const char *name,
                   const char *version, FapPackage *out)
{
    for (int i = 0; i < idx->count; i++) {
        const FapPackage *pkg = &idx->pkgs[i];
        if (strcmp(pkg->name, name) != 0)
            continue;
        if (version && *version && strcmp(pkg->version, version) != 0)
            continue;
        *out = *pkg;
        return 0;
    }
    if (version && *version)
        return fap_error("registry: package \"%s\" version \"%s\" not found in index", name, version);
    return fap_error("registry: package \"%s\" not found in index", name);
}
