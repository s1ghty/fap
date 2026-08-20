#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utime.h>
#include <sys/stat.h>
#include "fap.h"

/* declared in registry.c, not part of the public fap.h API — exposed
 * so the JSON parsing can be tested without a network fetch */
int fap_index_parse(const char *json, size_t len, FapChannel channel, FapIndex *out);

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); pass++; } \
    else       { printf("  FAIL  %s  [%s]\n", msg, fap_err); fail++; } \
} while(0)

static const char *sample =
    "{\n"
    "  \"channel\": \"stable\",\n"
    "  \"packages\": [\n"
    "    {\n"
    "      \"name\": \"curl\",\n"
    "      \"version\": \"8.6.0\",\n"
    "      \"url\": \"https://example.com/curl-8.6.0.tar.zst\",\n"
    "      \"sha256\": \"abc123\",\n"
    "      \"description\": \"a URL transfer library\",\n"
    "      \"bin\": [\"curl\"],\n"
    "      \"libs\": [\"libcurl.so.4\"],\n"
    "      \"deps\": [\"openssl\", \"zlib\"]\n"
    "    },\n"
    "    {\n"
    "      \"name\": \"jq\",\n"
    "      \"version\": \"1.7\",\n"
    "      \"url\": \"https://example.com/jq-1.7.tar.zst\",\n"
    "      \"sha256\": \"def456\"\n"
    "    }\n"
    "  ]\n"
    "}\n";

/* FapIndex is ~11 MB (FAP_MAX_PKGS slots) — always heap-allocate it,
 * never a plain local, or this overflows the default stack. */
static FapIndex *alloc_index(void)
{
    FapIndex *idx = malloc(sizeof(FapIndex));
    if (!idx) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return idx;
}

static void test_index_parse(void)
{
    FapIndex *idx = alloc_index();
    CHECK(fap_index_parse(sample, strlen(sample), FAP_CHANNEL_STABLE, idx) == 0,
          "index_parse succeeds");
    CHECK(idx->count == 2, "index_parse finds 2 packages");
    CHECK(idx->channel == FAP_CHANNEL_STABLE, "index channel set");

    CHECK(strcmp(idx->pkgs[0].name, "curl") == 0, "pkg 0 name");
    CHECK(strcmp(idx->pkgs[0].version, "8.6.0") == 0, "pkg 0 version");
    CHECK(strcmp(idx->pkgs[0].url, "https://example.com/curl-8.6.0.tar.zst") == 0, "pkg 0 url");
    CHECK(strcmp(idx->pkgs[0].sha256, "abc123") == 0, "pkg 0 sha256");
    CHECK(strcmp(idx->pkgs[0].description, "a URL transfer library") == 0, "pkg 0 description");
    CHECK(idx->pkgs[0].bins_count == 1 && strcmp(idx->pkgs[0].bins[0], "curl") == 0, "pkg 0 bin");
    CHECK(idx->pkgs[0].libs_count == 1 && strcmp(idx->pkgs[0].libs[0], "libcurl.so.4") == 0, "pkg 0 libs");
    CHECK(idx->pkgs[0].channel == FAP_CHANNEL_STABLE, "pkg 0 channel tagged from fetch");
    CHECK(idx->pkgs[0].deps_count == 2 &&
          strcmp(idx->pkgs[0].deps[0], "openssl") == 0 &&
          strcmp(idx->pkgs[0].deps[1], "zlib") == 0,
          "pkg 0 deps");

    CHECK(strcmp(idx->pkgs[1].name, "jq") == 0, "pkg 1 name");
    CHECK(idx->pkgs[1].bins_count == 0, "pkg 1 has no bin array, defaults empty");
    CHECK(idx->pkgs[1].libs_count == 0, "pkg 1 has no libs array, defaults empty");
    CHECK(idx->pkgs[1].deps_count == 0, "pkg 1 has no deps array, defaults empty");

    free(idx);
}

static void test_index_find(void)
{
    FapIndex *idx = alloc_index();
    fap_index_parse(sample, strlen(sample), FAP_CHANNEL_STABLE, idx);

    FapPackage pkg;
    CHECK(fap_index_find(idx, "curl", NULL, &pkg) == 0, "index_find by name only");
    CHECK(strcmp(pkg.version, "8.6.0") == 0, "index_find returns right package");
    CHECK(pkg.deps_count == 2, "index_find'd package carries its deps");

    CHECK(fap_index_find(idx, "curl", "8.6.0", &pkg) == 0, "index_find with matching version");
    CHECK(fap_index_find(idx, "curl", "9.9.9", &pkg) < 0, "index_find rejects wrong version");
    CHECK(fap_index_find(idx, "nope", NULL, &pkg) < 0, "index_find rejects unknown package");

    free(idx);
}

static void test_index_parse_missing_packages(void)
{
    const char *bad = "{\"channel\": \"stable\"}";
    FapIndex *idx = alloc_index();
    CHECK(fap_index_parse(bad, strlen(bad), FAP_CHANNEL_STABLE, idx) < 0,
          "index_parse rejects document without \"packages\"");
    free(idx);
}

static void test_index_parse_missing_field(void)
{
    const char *bad = "{\"packages\": [{\"name\": \"curl\"}]}";
    FapIndex *idx = alloc_index();
    CHECK(fap_index_parse(bad, strlen(bad), FAP_CHANNEL_STABLE, idx) < 0,
          "index_parse rejects package missing required fields");
    free(idx);
}

static void test_index_parse_too_many_deps(void)
{
    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
        "{\"packages\": [{\"name\": \"x\", \"version\": \"1\", "
        "\"url\": \"u\", \"sha256\": \"s\", \"deps\": [");
    for (int i = 0; i < FAP_MAX_PKG_DEPS + 1 && n < (int)sizeof(buf); i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s\"d%d\"", i ? "," : "", i);
    snprintf(buf + n, sizeof(buf) - n, "]}]}");

    FapIndex *idx = alloc_index();
    CHECK(fap_index_parse(buf, strlen(buf), FAP_CHANNEL_STABLE, idx) < 0,
          "index_parse rejects a package with too many deps");
    free(idx);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fputs(content, f);
    fclose(f);
}

static const char *index_with_pkg(const char *pkgname)
{
    static char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"packages\": [{\"name\": \"%s\", \"version\": \"1\", "
        "\"url\": \"file:///dev/null\", \"sha256\": \"s\"}]}", pkgname);
    return buf;
}

/* fap_index_fetch caches every fetch under <HOME>/.local/fap/index-cache/
 * (see registry.c's file header comment) — a fetch within FAP_INDEX_TTL
 * seconds of the last one reads that cache instead of hitting the
 * network/file:// URL at all. Exercised with file:// sources (no real
 * network needed) and an artificially backdated cache mtime to force
 * staleness on demand, rather than actually waiting out a TTL. */
static void test_index_fetch_caching(void)
{
    const char *home = "/tmp/fap_test_registry_home";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", home, home);
    system(cmd);
    setenv("HOME", home, 1);

    const char *src = "/tmp/fap_test_registry_src.json";
    char srcurl[256];
    snprintf(srcurl, sizeof(srcurl), "file://%s", src);
    setenv("FAP_STABLE_INDEX_URL", srcurl, 1);
    unsetenv("FAP_INDEX_TTL");

    printf("\nregistry index caching:\n");

    write_file(src, index_with_pkg("v1pkg"));
    FapIndex *idx = alloc_index();
    CHECK(fap_index_fetch(FAP_CHANNEL_STABLE, idx) == 0, "first fetch succeeds");
    CHECK(idx->count == 1 && strcmp(idx->pkgs[0].name, "v1pkg") == 0,
          "first fetch returns the source's real content");

    char cache_file[FAP_MAX_PATH];
    snprintf(cache_file, sizeof(cache_file), "%s/.local/fap/index-cache/stable.json", home);
    struct stat st;
    CHECK(stat(cache_file, &st) == 0, "fetch wrote a local cache file");

    /* source changes, but the cache is still fresh (default 1h TTL) —
     * a second fetch should NOT reflect the change */
    write_file(src, index_with_pkg("v2pkg"));
    free(idx);
    idx = alloc_index();
    CHECK(fap_index_fetch(FAP_CHANNEL_STABLE, idx) == 0, "second fetch (within TTL) succeeds");
    CHECK(idx->count == 1 && strcmp(idx->pkgs[0].name, "v1pkg") == 0,
          "second fetch served from cache, not re-fetched (still v1pkg despite source now being v2pkg)");

    /* backdate the cache past the TTL window — next fetch must go back
     * to the (now-changed) source. 3600 matches registry.c's
     * DEFAULT_INDEX_TTL_SECONDS (not visible here, it's a static #define
     * in a different translation unit) — if that default ever changes,
     * this needs to move with it. */
    time_t old = time(NULL) - 3600 - 60;
    struct utimbuf times = { old, old };
    utime(cache_file, &times);

    free(idx);
    idx = alloc_index();
    CHECK(fap_index_fetch(FAP_CHANNEL_STABLE, idx) == 0, "third fetch (cache backdated stale) succeeds");
    CHECK(idx->count == 1 && strcmp(idx->pkgs[0].name, "v2pkg") == 0,
          "third fetch re-fetched from source once the cache was stale (now v2pkg)");

    /* FAP_INDEX_TTL=0 means nothing is ever fresh — always re-fetch */
    write_file(src, index_with_pkg("v3pkg"));
    setenv("FAP_INDEX_TTL", "0", 1);
    free(idx);
    idx = alloc_index();
    CHECK(fap_index_fetch(FAP_CHANNEL_STABLE, idx) == 0, "fourth fetch (TTL=0) succeeds");
    CHECK(idx->count == 1 && strcmp(idx->pkgs[0].name, "v3pkg") == 0,
          "FAP_INDEX_TTL=0 disables caching entirely (always current, v3pkg)");
    unsetenv("FAP_INDEX_TTL");

    /* network/source failure with a cache present (even stale) falls
     * back to the cache instead of hard-failing */
    utime(cache_file, &times); /* backdate again so it's eligible to be seen as stale */
    setenv("FAP_STABLE_INDEX_URL", "file:///tmp/fap_test_registry_does_not_exist.json", 1);
    free(idx);
    idx = alloc_index();
    CHECK(fap_index_fetch(FAP_CHANNEL_STABLE, idx) == 0,
          "fetch with an unreachable source but an existing cache still succeeds");
    CHECK(idx->count == 1 && strcmp(idx->pkgs[0].name, "v3pkg") == 0,
          "...by falling back to the last successfully cached content (v3pkg)");

    free(idx);
    unsetenv("FAP_STABLE_INDEX_URL");
    unsetenv("HOME");
}

int main(void)
{
    printf("registry:\n");
    test_index_parse();
    test_index_find();
    test_index_parse_missing_packages();
    test_index_parse_missing_field();
    test_index_parse_too_many_deps();
    test_index_fetch_caching();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
