#include <stdio.h>
#include <string.h>
#include "fap.h"

/*
 * resolver.c — recursive dependency resolution + topological sort.
 *
 * Pure over an already-fetched FapIndex (no network, no fap.lock) —
 * classic three-color DFS: UNVISITED -> VISITING -> DONE. A DONE node
 * revisited via another path is a shared dependency (fine, skip it).
 * A VISITING node revisited is a cycle. Packages are appended to the
 * output in post-order, so every dependency lands before whatever
 * depends on it — that's exactly the install order.
 */

enum { UNVISITED = 0, VISITING, DONE };

static int find_pkg_index(const FapIndex *idx, const char *name)
{
    for (int i = 0; i < idx->count; i++) {
        if (strcmp(idx->pkgs[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void describe_cycle(const char *path[], int path_len, const char *closing_name,
                            char *buf, size_t bufsz)
{
    buf[0] = '\0';
    for (int i = 0; i < path_len; i++) {
        strncat(buf, path[i], bufsz - strlen(buf) - 1);
        strncat(buf, " -> ", bufsz - strlen(buf) - 1);
    }
    strncat(buf, closing_name, bufsz - strlen(buf) - 1);
}

static int visit(const FapIndex *idx, int pkg_idx, int *state,
                  const char *path[], int path_len, FapIndex *out)
{
    if (state[pkg_idx] == DONE)
        return 0;

    if (state[pkg_idx] == VISITING) {
        char cycle[2048];
        describe_cycle(path, path_len, idx->pkgs[pkg_idx].name, cycle, sizeof(cycle));
        return fap_error("resolver: circular dependency: %s", cycle);
    }

    if (path_len >= FAP_MAX_PKGS)
        return fap_error("resolver: dependency chain too deep (max %d)", FAP_MAX_PKGS);

    state[pkg_idx] = VISITING;
    path[path_len] = idx->pkgs[pkg_idx].name;

    const FapPackage *pkg = &idx->pkgs[pkg_idx];
    for (int i = 0; i < pkg->deps_count; i++) {
        int dep_idx = find_pkg_index(idx, pkg->deps[i]);
        if (dep_idx < 0)
            return fap_error("resolver: dependency \"%s\" of \"%s\" not found in index",
                             pkg->deps[i], pkg->name);
        if (visit(idx, dep_idx, state, path, path_len + 1, out) < 0)
            return -1;
    }

    state[pkg_idx] = DONE;
    if (out->count >= FAP_MAX_PKGS)
        return fap_error("resolver: too many packages in resolution (max %d)", FAP_MAX_PKGS);
    out->pkgs[out->count++] = *pkg;
    return 0;
}

int fap_resolve(const FapIndex *idx, const char *name, FapIndex *out)
{
    memset(out, 0, sizeof(*out));
    out->channel = idx->channel;

    int root_idx = find_pkg_index(idx, name);
    if (root_idx < 0)
        return fap_error("resolver: package \"%s\" not found in index", name);

    int state[FAP_MAX_PKGS];
    memset(state, 0, sizeof(state));
    const char *path[FAP_MAX_PKGS];

    return visit(idx, root_idx, state, path, 0, out);
}
