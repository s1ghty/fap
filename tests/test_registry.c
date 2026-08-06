#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(void)
{
    printf("registry:\n");
    test_index_parse();
    test_index_find();
    test_index_parse_missing_packages();
    test_index_parse_missing_field();
    test_index_parse_too_many_deps();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
