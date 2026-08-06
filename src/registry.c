#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "fap.h"
#include "json.h"

/*
 * registry.c — fetch + parse a channel index (see CLAUDE.md's "Channel
 * index format") via libcurl. Parsing reuses json.c's flat scanning
 * helpers, same as lock.c.
 *
 * Index URLs aren't a settled part of this project yet (cli.c's
 * cmd_channels notes "URLs will come from a config file or compiled-in
 * defaults") — for now they're read from FAP_STABLE_INDEX_URL /
 * FAP_EDGE_INDEX_URL so nothing here hardcodes a registry that doesn't
 * exist.
 */

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

static int index_url(FapChannel channel, char *buf, size_t bufsz)
{
    const char *env = (channel == FAP_CHANNEL_EDGE)
        ? "FAP_EDGE_INDEX_URL" : "FAP_STABLE_INDEX_URL";
    const char *url = getenv(env);
    if (!url || !*url)
        return fap_error("registry: no index URL configured for this channel; set %s", env);
    if ((size_t)snprintf(buf, bufsz, "%s", url) >= bufsz)
        return fap_error("registry: %s is too long", env);
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

int fap_index_fetch(FapChannel channel, FapIndex *out)
{
    char url[FAP_MAX_URL];
    if (index_url(channel, url, sizeof(url)) < 0)
        return -1;

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

    if (status != 200) {
        free(buf.data);
        return fap_error("registry: %s returned HTTP %ld", url, status);
    }
    if (!buf.data) {
        return fap_error("registry: empty response from %s", url);
    }

    int r = fap_index_parse(buf.data, buf.len, channel, out);
    free(buf.data);
    return r;
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
