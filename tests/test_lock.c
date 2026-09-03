#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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

/* Sets just what fap_lock_orphans() actually looks at (name + deps) —
 * this is pure in-memory graph logic, never round-tripped through
 * fap_lock_load()/fap_validate_package(), so no other field needs to
 * be a realistic value. */
static void set_pkg(FapPackage *pkg, const char *name, ...)
{
    memset(pkg, 0, sizeof(*pkg));
    strcpy(pkg->name, name);
    strcpy(pkg->version, "1.0");

    va_list ap;
    va_start(ap, name);
    const char *dep;
    while ((dep = va_arg(ap, const char *)) != NULL)
        strcpy(pkg->deps[pkg->deps_count++], dep);
    va_end(ap);
}

static void set_manifest(FapManifest *m, ...)
{
    memset(m, 0, sizeof(*m));
    va_list ap;
    va_start(ap, m);
    const char *name;
    while ((name = va_arg(ap, const char *)) != NULL)
        strcpy(m->deps[m->deps_count++].name, name);
    va_end(ap);
}

static int orphans_contains(const FapLock *orphans, const char *name)
{
    for (int i = 0; i < orphans->count; i++)
        if (strcmp(orphans->entries[i].pkg.name, name) == 0)
            return 1;
    return 0;
}

static void test_orphans_direct_dep(void)
{
    /* a (explicit) -> b. b is only needed via a. */
    FapLock *lock = alloc_lock();
    memset(lock, 0, sizeof(*lock));
    set_pkg(&lock->entries[0].pkg, "a", "b", (char *)NULL);
    set_pkg(&lock->entries[1].pkg, "b", (char *)NULL);
    lock->count = 2;

    FapManifest m;
    set_manifest(&m, "a", (char *)NULL);

    FapLock *orphans = alloc_lock();
    CHECK(fap_lock_orphans(lock, &m, orphans) == 0, "orphans computes with a explicit, b its dep");
    CHECK(orphans->count == 0, "b is reachable via a — not an orphan");

    /* a no longer explicit (e.g. after `fap remove a`'s manifest entry
     * is gone) — b has nothing left keeping it around. */
    FapManifest empty_m;
    set_manifest(&empty_m, (char *)NULL);
    CHECK(fap_lock_orphans(lock, &empty_m, orphans) == 0, "orphans computes with nothing explicit");
    CHECK(orphans->count == 2, "both a and b are orphans once nothing requests a");
    CHECK(orphans_contains(orphans, "a") && orphans_contains(orphans, "b"),
          "both a and b are named in the orphan set");

    free(lock);
    free(orphans);
}

static void test_orphans_shared_dep(void)
{
    /* a (explicit) -> shared; d (explicit) -> shared. Removing a alone
     * shouldn't orphan shared — d still needs it. */
    FapLock *lock = alloc_lock();
    memset(lock, 0, sizeof(*lock));
    set_pkg(&lock->entries[0].pkg, "a", "shared", (char *)NULL);
    set_pkg(&lock->entries[1].pkg, "d", "shared", (char *)NULL);
    set_pkg(&lock->entries[2].pkg, "shared", (char *)NULL);
    lock->count = 3;

    FapManifest m;
    set_manifest(&m, "d", (char *)NULL); /* a already "removed" from fap.toml */

    FapLock *orphans = alloc_lock();
    CHECK(fap_lock_orphans(lock, &m, orphans) == 0, "orphans computes with only d explicit");
    CHECK(orphans->count == 1 && orphans_contains(orphans, "a"),
          "a is orphaned (nothing explicit needs it), shared is not (d still needs it)");

    free(lock);
    free(orphans);
}

static void test_orphans_chain(void)
{
    /* a (explicit) -> b -> c. Dropping a from the manifest should
     * orphan b AND c in one pass, without recomputing after each
     * removal — c is only reachable through b, and b is only
     * reachable through a. */
    FapLock *lock = alloc_lock();
    memset(lock, 0, sizeof(*lock));
    set_pkg(&lock->entries[0].pkg, "a", "b", (char *)NULL);
    set_pkg(&lock->entries[1].pkg, "b", "c", (char *)NULL);
    set_pkg(&lock->entries[2].pkg, "c", (char *)NULL);
    lock->count = 3;

    FapManifest empty_m;
    set_manifest(&empty_m, (char *)NULL);

    FapLock *orphans = alloc_lock();
    CHECK(fap_lock_orphans(lock, &empty_m, orphans) == 0, "orphans computes on a chain");
    CHECK(orphans->count == 3, "the whole chain (a, b, c) comes back as orphans in one pass");

    free(lock);
    free(orphans);
}

static void test_orphans_none(void)
{
    FapLock *lock = alloc_lock();
    memset(lock, 0, sizeof(*lock));
    set_pkg(&lock->entries[0].pkg, "solo", (char *)NULL);
    lock->count = 1;

    FapManifest m;
    set_manifest(&m, "solo", (char *)NULL);

    FapLock *orphans = alloc_lock();
    CHECK(fap_lock_orphans(lock, &m, orphans) == 0, "orphans computes with nothing orphaned");
    CHECK(orphans->count == 0, "an explicitly-requested package with no deps is never an orphan");

    free(lock);
    free(orphans);
}

int main(void)
{
    printf("lock:\n");
    test_lock_roundtrip();
    test_lock_missing_field();
    test_lock_empty();

    printf("\norphans:\n");
    test_orphans_direct_dep();
    test_orphans_shared_dep();
    test_orphans_chain();
    test_orphans_none();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
