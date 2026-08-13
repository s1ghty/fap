#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "fap.h"

/*
 * cli.c — command implementations
 *
 * Each cmd_* function receives:
 *   argv[0] = the command name (e.g. "install")
 *   argv[1..] = arguments to the command
 *   argc = count including argv[0]
 *
 * Returns 0 on success, -1 on error (fap_err is set).
 *
 * FapIndex (~6.9 MB, FAP_MAX_PKGS slots) and FapLock (~1.7 MB,
 * FAP_MAX_DEPS slots) are too big for the stack, especially stacked
 * across nested calls — every one of them here is heap-allocated.
 */

/* Channel indexes are fetched at most once per command invocation. */
typedef struct {
    FapIndex *idx[2];
} IndexCache;

static int cache_get(IndexCache *c, FapChannel ch, FapIndex **out)
{
    if (!c->idx[ch]) {
        FapIndex *idx = malloc(sizeof(FapIndex));
        if (!idx)
            return fap_error("out of memory fetching channel index");
        if (fap_index_fetch(ch, idx) < 0) {
            free(idx);
            return -1;
        }
        c->idx[ch] = idx;
    }
    *out = c->idx[ch];
    return 0;
}

static void cache_free(IndexCache *c)
{
    free(c->idx[0]);
    free(c->idx[1]);
}

static int lock_path(char *buf, size_t bufsz)
{
    return fap_root_path(FAP_LOCK, buf, bufsz);
}

static int manifest_path(char *buf, size_t bufsz)
{
    return fap_root_path(FAP_MANIFEST, buf, bufsz);
}

/* fap.lock not existing yet is not an error — it just means nothing
 * is tracked so far. */
static int load_lock(FapLock *lock)
{
    char path[FAP_MAX_PATH];
    if (lock_path(path, sizeof(path)) < 0)
        return -1;
    struct stat st;
    if (stat(path, &st) < 0) {
        memset(lock, 0, sizeof(*lock));
        return 0;
    }
    return fap_lock_load(path, lock);
}

static int save_lock(const FapLock *lock)
{
    char path[FAP_MAX_PATH];
    if (lock_path(path, sizeof(path)) < 0)
        return -1;
    return fap_lock_save(path, lock);
}

static int lock_upsert(FapLock *lock, const FapPackage *pkg)
{
    for (int i = 0; i < lock->count; i++) {
        if (strcmp(lock->entries[i].pkg.name, pkg->name) == 0) {
            lock->entries[i].pkg = *pkg;
            return 0;
        }
    }
    if (lock->count >= FAP_MAX_DEPS)
        return fap_error("lock: too many packages (max %d)", FAP_MAX_DEPS);
    lock->entries[lock->count++].pkg = *pkg;
    return 0;
}

/* Removes by name if present; not found is not an error (best-effort
 * bookkeeping after fap_remove() has already done the real work). */
static void lock_remove(FapLock *lock, const char *name)
{
    for (int i = 0; i < lock->count; i++) {
        if (strcmp(lock->entries[i].pkg.name, name) == 0) {
            lock->entries[i] = lock->entries[lock->count - 1];
            lock->count--;
            return;
        }
    }
}

static int pkg_install_path(const FapPackage *pkg, char *buf, size_t bufsz)
{
    char root[FAP_MAX_PATH];
    if (fap_root_path(FAP_PKGS, root, sizeof(root)) < 0)
        return -1;
    int n = snprintf(buf, bufsz, "%s/%s-%s", root, pkg->name, pkg->version);
    if (n < 0 || (size_t)n >= bufsz)
        return fap_error("path too long");
    return 0;
}

static int is_installed(const FapPackage *pkg)
{
    char path[FAP_MAX_PATH];
    if (pkg_install_path(pkg, path, sizeof(path)) < 0)
        return 0;
    struct stat st;
    return stat(path, &st) == 0;
}

/* fap_install() only replaces an exact name-version dir; on an actual
 * version bump the old one is orphaned unless something removes it.
 * That's the resolved-install loop's call to make, not install.c's.
 * Best-effort: the new version is already correctly installed either way. */
static void remove_old_version(const char *name, const char *old_version)
{
    char root[FAP_MAX_PATH], dir[FAP_MAX_PATH];
    if (fap_root_path(FAP_PKGS, root, sizeof(root)) < 0)
        return;
    if (snprintf(dir, sizeof(dir), "%s/%s-%s", root, name, old_version) >= (int)sizeof(dir))
        return;
    fap_rm_rf(dir);
}

/* Installs every package in a resolved dependency chain, in order
 * (fap_resolve already put dependencies before whatever needs them),
 * upserting each into lock. A package already installed at the exact
 * resolved version is skipped rather than redundantly reinstalled;
 * one that changed version has its old on-disk copy cleaned up. Used
 * by install, sync, and update alike — same shape once you have a
 * resolved chain instead of one package. */
static int install_resolved(const FapIndex *resolved, FapLock *lock)
{
    int rc = 0;
    for (int i = 0; rc == 0 && i < resolved->count; i++) {
        const FapPackage *pkg = &resolved->pkgs[i];

        char old_version[FAP_MAX_VERSION] = "";
        for (int j = 0; j < lock->count; j++) {
            if (strcmp(lock->entries[j].pkg.name, pkg->name) == 0) {
                snprintf(old_version, sizeof(old_version), "%s", lock->entries[j].pkg.version);
                break;
            }
        }

        if (strcmp(old_version, pkg->version) == 0 && is_installed(pkg)) {
            printf("%s %s already installed\n", pkg->name, pkg->version);
        } else {
            char chan_str[16];
            fap_channel_str(pkg->channel, chan_str, sizeof(chan_str));
            printf("installing %s %s (%s)...\n", pkg->name, pkg->version, chan_str);
            rc = fap_install(pkg);
        }

        if (rc == 0)
            rc = lock_upsert(lock, pkg);
        if (rc == 0 && old_version[0] && strcmp(old_version, pkg->version) != 0)
            remove_old_version(pkg->name, old_version);
    }
    return rc;
}

/* ── install ──────────────────────────────────────────────────── */

/* Fetches name's channel index, resolves its full dependency chain,
 * and installs everything in order. pinned_version, if given, must
 * match what the registry resolved name to — fap.toml can pin an
 * exact version for a top-level requested package (not for transitive
 * deps: those have no per-dep channel/version in this model), and
 * fap_resolve() finds packages by name only, so that pin is checked
 * here against the resolved root. Shared by install and sync. */
static int resolve_and_install(const char *name, FapChannel channel, const char *pinned_version,
                                IndexCache *cache, FapLock *lock)
{
    FapIndex *idx;
    if (cache_get(cache, channel, &idx) < 0)
        return -1;

    FapIndex *resolved = malloc(sizeof(FapIndex));
    if (!resolved)
        return fap_error("out of memory resolving %s", name);

    int rc = fap_resolve(idx, name, resolved);

    if (rc == 0 && pinned_version) {
        const FapPackage *root = &resolved->pkgs[resolved->count - 1];
        if (strcmp(root->version, pinned_version) != 0)
            rc = fap_error("\"%s\" is pinned to version \"%s\" in fap.toml, but the registry has \"%s\"",
                           name, pinned_version, root->version);
    }

    if (rc == 0 && resolved->count > 1)
        printf("resolved %d package%s to install for %s\n",
               resolved->count, resolved->count == 1 ? "" : "s", name);

    if (rc == 0)
        rc = install_resolved(resolved, lock);

    free(resolved);
    return rc;
}

static int install_one(const char *name, const FapManifest *manifest,
                        IndexCache *cache, FapLock *lock, FapChannel *channel_out)
{
    FapChannel channel = FAP_CHANNEL_STABLE;
    const char *pinned_version = NULL;
    for (int i = 0; i < manifest->deps_count; i++) {
        if (strcmp(manifest->deps[i].name, name) == 0) {
            channel = manifest->deps[i].channel;
            if (manifest->deps[i].version[0])
                pinned_version = manifest->deps[i].version;
            break;
        }
    }

    if (channel_out)
        *channel_out = channel;
    return resolve_and_install(name, channel, pinned_version, cache, lock);
}

int cmd_install(int argc, char **argv)
{
    if (argc < 2)
        return fap_error("install: specify at least one package");

    char manifest_p[FAP_MAX_PATH];
    if (manifest_path(manifest_p, sizeof(manifest_p)) < 0)
        return -1;

    FapManifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    struct stat st;
    if (stat(manifest_p, &st) == 0 && fap_manifest_load(manifest_p, &manifest) < 0)
        return -1;

    FapLock *lock = malloc(sizeof(FapLock));
    if (!lock)
        return fap_error("out of memory");
    int rc = load_lock(lock);

    IndexCache cache;
    memset(&cache, 0, sizeof(cache));

    for (int i = 1; rc == 0 && i < argc; i++) {
        FapChannel channel;
        rc = install_one(argv[i], &manifest, &cache, lock, &channel);
        /* Record only what was explicitly asked for, not the whole
         * resolved dependency chain — fap.toml tracks intent, the
         * resolver re-derives transitive deps from the registry every
         * time, same distinction pacman draws between "explicit" and
         * "installed as a dependency" packages. */
        if (rc == 0)
            rc = fap_manifest_add_dep(manifest_p, argv[i], channel);
    }

    if (rc == 0)
        rc = save_lock(lock);

    cache_free(&cache);
    free(lock);
    return rc;
}

/* ── remove ───────────────────────────────────────────────────── */

int cmd_remove(int argc, char **argv)
{
    if (argc < 2)
        return fap_error("remove: specify at least one package");

    char manifest_p[FAP_MAX_PATH];
    if (manifest_path(manifest_p, sizeof(manifest_p)) < 0)
        return -1;

    FapLock *lock = malloc(sizeof(FapLock));
    if (!lock)
        return fap_error("out of memory");
    int rc = load_lock(lock);

    for (int i = 1; rc == 0 && i < argc; i++) {
        printf("removing %s...\n", argv[i]);
        rc = fap_remove(argv[i]);
        if (rc == 0)
            lock_remove(lock, argv[i]);
        if (rc == 0)
            rc = fap_manifest_remove_dep(manifest_p, argv[i]);
    }

    if (rc == 0)
        rc = save_lock(lock);

    free(lock);
    return rc;
}

/* ── sync ─────────────────────────────────────────────────────── */

/* If dep is already locked and installed exactly as locked, this is a
 * pure no-op fast path: no registry fetch, so `fap sync` still works
 * offline once everything's in place. Otherwise resolve+install its
 * full dependency chain, same as install. */
static int sync_dep(const FapDep *dep, FapLock *lock, IndexCache *cache)
{
    for (int i = 0; i < lock->count; i++) {
        if (strcmp(lock->entries[i].pkg.name, dep->name) == 0) {
            if (is_installed(&lock->entries[i].pkg)) {
                printf("%s %s already installed\n",
                       lock->entries[i].pkg.name, lock->entries[i].pkg.version);
                return 0;
            }
            break;
        }
    }

    const char *pinned_version = dep->version[0] ? dep->version : NULL;
    return resolve_and_install(dep->name, dep->channel, pinned_version, cache, lock);
}

int cmd_sync(int argc, char **argv)
{
    (void)argc; (void)argv;

    char manifest_p[FAP_MAX_PATH];
    if (manifest_path(manifest_p, sizeof(manifest_p)) < 0)
        return -1;

    FapManifest manifest;
    if (fap_manifest_load(manifest_p, &manifest) < 0)
        return -1;

    FapLock *lock = malloc(sizeof(FapLock));
    if (!lock)
        return fap_error("out of memory");
    int rc = load_lock(lock);

    IndexCache cache;
    memset(&cache, 0, sizeof(cache));

    for (int i = 0; rc == 0 && i < manifest.deps_count; i++)
        rc = sync_dep(&manifest.deps[i], lock, &cache);

    if (rc == 0)
        rc = save_lock(lock);

    cache_free(&cache);
    free(lock);
    return rc;
}

/* ── update ───────────────────────────────────────────────────── */

/* Re-resolves name's full dependency chain fresh from the registry
 * (no lock fast-path here — that's the point of update) and installs
 * whatever actually changed; install_resolved() skips anything
 * already at the resolved version, so an update where only the root
 * moved doesn't needlessly reinstall unchanged deps. */
static int update_one(const char *name, FapLock *lock, IndexCache *cache)
{
    int found = -1;
    for (int i = 0; i < lock->count; i++) {
        if (strcmp(lock->entries[i].pkg.name, name) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0)
        return fap_error("update: package \"%s\" is not installed", name);

    FapChannel channel = lock->entries[found].pkg.channel;
    return resolve_and_install(name, channel, NULL, cache, lock);
}

int cmd_update(int argc, char **argv)
{
    FapLock *lock = malloc(sizeof(FapLock));
    if (!lock)
        return fap_error("out of memory");
    int rc = load_lock(lock);

    IndexCache cache;
    memset(&cache, 0, sizeof(cache));

    if (rc == 0 && argc < 2) {
        for (int i = 0; rc == 0 && i < lock->count; i++) {
            char name[FAP_MAX_NAME];
            snprintf(name, sizeof(name), "%s", lock->entries[i].pkg.name);
            rc = update_one(name, lock, &cache);
        }
    } else {
        for (int i = 1; rc == 0 && i < argc; i++)
            rc = update_one(argv[i], lock, &cache);
    }

    if (rc == 0)
        rc = save_lock(lock);

    cache_free(&cache);
    free(lock);
    return rc;
}

/* ── search ───────────────────────────────────────────────────── */

static void print_match(const FapPackage *pkg)
{
    char chan[16];
    fap_channel_str(pkg->channel, chan, sizeof(chan));
    printf("%-20s %-12s %-8s %s\n", pkg->name, pkg->version, chan, pkg->description);
}

static int search_index(const FapIndex *idx, const char *query)
{
    int found = 0;
    for (int i = 0; i < idx->count; i++) {
        const FapPackage *pkg = &idx->pkgs[i];
        if (strstr(pkg->name, query) || strstr(pkg->description, query)) {
            print_match(pkg);
            found++;
        }
    }
    return found;
}

int cmd_search(int argc, char **argv)
{
    if (argc < 2)
        return fap_error("search: provide a query");

    const char *stable_url = fap_channel_index_url(FAP_CHANNEL_STABLE);
    const char *edge_url = fap_channel_index_url(FAP_CHANNEL_EDGE);
    if (!stable_url && !edge_url)
        return fap_error("search: no channels configured; set FAP_STABLE_INDEX_URL and/or FAP_EDGE_INDEX_URL");

    FapIndex *stable = malloc(sizeof(FapIndex));
    FapIndex *edge = malloc(sizeof(FapIndex));
    if (!stable || !edge) {
        free(stable);
        free(edge);
        return fap_error("out of memory");
    }

    /* Search whichever channels are configured; a channel with no
     * index URL set is skipped, not treated as an error (fap_channels
     * shows the same "not configured" state as normal). stable always
     * has one — see FAP_DEFAULT_STABLE_INDEX_URL. */
    int rc = 0;
    int found = 0;
    if (stable_url) {
        rc = fap_index_fetch(FAP_CHANNEL_STABLE, stable);
        if (rc == 0)
            found += search_index(stable, argv[1]);
    }
    if (rc == 0 && edge_url) {
        rc = fap_index_fetch(FAP_CHANNEL_EDGE, edge);
        if (rc == 0)
            found += search_index(edge, argv[1]);
    }

    if (rc == 0 && found == 0)
        printf("no packages found matching \"%s\"\n", argv[1]);

    free(stable);
    free(edge);
    return rc;
}

/* ── list ─────────────────────────────────────────────────────── */

int cmd_list(int argc, char **argv)
{
    (void)argc; (void)argv;

    FapLock *lock = malloc(sizeof(FapLock));
    if (!lock)
        return fap_error("out of memory");
    int rc = load_lock(lock);

    if (rc == 0) {
        if (lock->count == 0) {
            printf("no packages installed\n");
        } else {
            for (int i = 0; i < lock->count; i++) {
                const FapPackage *pkg = &lock->entries[i].pkg;
                char chan[16];
                fap_channel_str(pkg->channel, chan, sizeof(chan));
                printf("%-20s %-12s %-8s%s\n", pkg->name, pkg->version, chan,
                       is_installed(pkg) ? "" : "  (missing on disk!)");
            }
        }
    }

    free(lock);
    return rc;
}

/* ── info ─────────────────────────────────────────────────────── */

static void print_info(const FapPackage *pkg)
{
    char chan[16];
    fap_channel_str(pkg->channel, chan, sizeof(chan));
    printf("name:        %s\n", pkg->name);
    printf("version:     %s\n", pkg->version);
    printf("channel:     %s\n", chan);
    printf("url:         %s\n", pkg->url);
    printf("sha256:      %s\n", pkg->sha256);
    printf("description: %s\n", pkg->description);
    printf("binaries:    ");
    for (int i = 0; i < pkg->bins_count; i++)
        printf("%s%s", i ? ", " : "", pkg->bins[i]);
    printf("\n");
}

static void print_install_state(const char *name)
{
    FapLock *lock = malloc(sizeof(FapLock));
    if (!lock)
        return;
    if (load_lock(lock) == 0) {
        for (int i = 0; i < lock->count; i++) {
            if (strcmp(lock->entries[i].pkg.name, name) != 0)
                continue;
            char path[FAP_MAX_PATH];
            if (pkg_install_path(&lock->entries[i].pkg, path, sizeof(path)) == 0)
                printf("installed:   %s (%s)\n", lock->entries[i].pkg.version, path);
            break;
        }
    }
    free(lock);
}

int cmd_info(int argc, char **argv)
{
    if (argc < 2)
        return fap_error("info: specify a package name");
    const char *name = argv[1];

    static const FapChannel channels[2] = { FAP_CHANNEL_STABLE, FAP_CHANNEL_EDGE };
    FapPackage pkg;
    int resolved = 0, any_fetch_ok = 0;

    FapIndex *idx = malloc(sizeof(FapIndex));
    if (!idx)
        return fap_error("out of memory");

    for (int i = 0; i < 2 && !resolved; i++) {
        if (fap_index_fetch(channels[i], idx) < 0)
            continue;
        any_fetch_ok = 1;
        if (fap_index_find(idx, name, NULL, &pkg) == 0)
            resolved = 1;
    }
    free(idx);

    if (!resolved) {
        if (!any_fetch_ok)
            return -1; /* fap_err already set by the failed fetch */
        return fap_error("info: package \"%s\" not found in any channel", name);
    }

    print_info(&pkg);
    print_install_state(name);
    return 0;
}

/* ── channels ─────────────────────────────────────────────────── */

int cmd_channels(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *stable_env = getenv("FAP_STABLE_INDEX_URL");
    printf("stable  %s%s\n", fap_channel_index_url(FAP_CHANNEL_STABLE),
           (stable_env && *stable_env) ? "" : "  (default — override with FAP_STABLE_INDEX_URL)");
    const char *edge_url = fap_channel_index_url(FAP_CHANNEL_EDGE);
    printf("edge    %s\n", edge_url ? edge_url : "(not configured, set FAP_EDGE_INDEX_URL)");
    return 0;
}
