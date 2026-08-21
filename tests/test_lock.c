#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fap.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); pass++; } \
    else       { printf("  FAIL  %s  [%s]\n", msg, fap_err); fail++; } \
} while(0)

/* FapLock is ~2.7 MB (FAP_MAX_DEPS slots) — always heap-allocate it,
 * never a plain local, especially with more than one in scope at once. */
static FapLock *alloc_lock(void)
{
    FapLock *lock = malloc(sizeof(FapLock));
    if (!lock) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return lock;
}

static void test_lock_roundtrip(void)
{
    const char *tmpfile = "/tmp/fap_test.lock";

    FapLock *lock = alloc_lock();
    memset(lock, 0, sizeof(*lock));
    lock->count = 2;
    strcpy(lock->entries[0].pkg.name, "curl");
    strcpy(lock->entries[0].pkg.version, "8.6.0");
    lock->entries[0].pkg.channel = FAP_CHANNEL_STABLE;
    strcpy(lock->entries[0].pkg.url, "https://example.com/curl-8.6.0.tar.zst");
    strcpy(lock->entries[0].pkg.sha256, "abc123");
    lock->entries[0].pkg.bins_count = 1;
    strcpy(lock->entries[0].pkg.bins[0], "curl");
    lock->entries[0].pkg.libs_count = 1;
    strcpy(lock->entries[0].pkg.libs[0], "libcurl.so.4");
    lock->entries[0].pkg.deps_count = 2;
    strcpy(lock->entries[0].pkg.deps[0], "openssl");
    strcpy(lock->entries[0].pkg.deps[1], "zlib");

    /* name/version are validated on load (fap_validate_package) and
     * restricted to a safe charset — a package name can never
     * legitimately need quotes or backslashes. url isn't subject to
     * that restriction (it's only ever handed to curl, never turned
     * into a path or embedded in a generated script), so that's what
     * exercises write_json_string's escaping here instead. */
    strcpy(lock->entries[1].pkg.name, "weird-pkg");
    strcpy(lock->entries[1].pkg.version, "1.0");
    lock->entries[1].pkg.channel = FAP_CHANNEL_EDGE;
    strcpy(lock->entries[1].pkg.url, "https://example.com/we\"ird \\path.tar.zst");
    strcpy(lock->entries[1].pkg.sha256, "def456");

    CHECK(fap_lock_save(tmpfile, lock) == 0, "lock_save succeeds");
    free(lock);

    FapLock *loaded = alloc_lock();
    memset(loaded, 0, sizeof(*loaded));
    CHECK(fap_lock_load(tmpfile, loaded) == 0, "lock_load succeeds");
    CHECK(loaded->count == 2, "lock_load recovers package count");
    CHECK(strcmp(loaded->entries[0].pkg.name, "curl") == 0, "name round-trips");
    CHECK(strcmp(loaded->entries[0].pkg.version, "8.6.0") == 0, "version round-trips");
    CHECK(loaded->entries[0].pkg.channel == FAP_CHANNEL_STABLE, "channel round-trips");
    CHECK(strcmp(loaded->entries[0].pkg.url, "https://example.com/curl-8.6.0.tar.zst") == 0,
          "url round-trips");
    CHECK(strcmp(loaded->entries[0].pkg.sha256, "abc123") == 0, "sha256 round-trips");
    CHECK(loaded->entries[0].pkg.bins_count == 1 &&
          strcmp(loaded->entries[0].pkg.bins[0], "curl") == 0,
          "bin round-trips");
    CHECK(loaded->entries[0].pkg.libs_count == 1 &&
          strcmp(loaded->entries[0].pkg.libs[0], "libcurl.so.4") == 0,
          "libs round-trip");
    CHECK(loaded->entries[0].pkg.deps_count == 2 &&
          strcmp(loaded->entries[0].pkg.deps[0], "openssl") == 0 &&
          strcmp(loaded->entries[0].pkg.deps[1], "zlib") == 0,
          "deps round-trip");
    CHECK(strcmp(loaded->entries[1].pkg.url, "https://example.com/we\"ird \\path.tar.zst") == 0,
          "escaped url round-trips");
    CHECK(loaded->entries[1].pkg.channel == FAP_CHANNEL_EDGE, "edge channel round-trips");
    CHECK(loaded->entries[1].pkg.deps_count == 0, "package with no deps round-trips as empty");
    CHECK(loaded->entries[1].pkg.libs_count == 0, "package with no libs round-trips as empty");
    free(loaded);
}

static void test_lock_missing_field(void)
{
    const char *tmpfile = "/tmp/fap_test_bad.lock";
    FILE *f = fopen(tmpfile, "wb");
    if (!f) { printf("  SKIP  cannot write tmpfile\n"); return; }
    fputs("{\"lockfile_version\":1,\"packages\":[{\"name\":\"curl\"}]}", f);
    fclose(f);

    FapLock *lock = alloc_lock();
    int r = fap_lock_load(tmpfile, lock);
    CHECK(r < 0, "lock_load rejects package missing required fields");
    free(lock);
}

static void test_lock_empty(void)
{
    const char *tmpfile = "/tmp/fap_test_empty.lock";
    FILE *f = fopen(tmpfile, "wb");
    if (!f) { printf("  SKIP  cannot write tmpfile\n"); return; }
    fputs("{\"lockfile_version\":1,\"packages\":[]}", f);
    fclose(f);

    FapLock *lock = alloc_lock();
    int r = fap_lock_load(tmpfile, lock);
    CHECK(r == 0, "lock_load succeeds on empty packages array");
    CHECK(lock->count == 0, "lock_load reports zero packages");
    free(lock);
}

int main(void)
{
    printf("lock:\n");
    test_lock_roundtrip();
    test_lock_missing_field();
    test_lock_empty();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
