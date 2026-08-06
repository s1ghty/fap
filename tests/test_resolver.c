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

/* FapIndex is ~11 MB (FAP_MAX_PKGS slots) — always heap-allocate it. */
static FapIndex *alloc_index(void)
{
    FapIndex *idx = malloc(sizeof(FapIndex));
    if (!idx) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memset(idx, 0, sizeof(*idx));
    return idx;
}

/* Appends a package with the given deps (NULL-terminated name list). */
static void add_pkg(FapIndex *idx, const char *name, ...)
{
    FapPackage *pkg = &idx->pkgs[idx->count++];
    memset(pkg, 0, sizeof(*pkg));
    snprintf(pkg->name, sizeof(pkg->name), "%s", name);
    snprintf(pkg->version, sizeof(pkg->version), "1.0");

    va_list ap;
    va_start(ap, name);
    const char *dep;
    while ((dep = va_arg(ap, const char *)) != NULL)
        snprintf(pkg->deps[pkg->deps_count++], FAP_MAX_NAME, "%s", dep);
    va_end(ap);
}

static int index_of(const FapIndex *out, const char *name)
{
    for (int i = 0; i < out->count; i++)
        if (strcmp(out->pkgs[i].name, name) == 0)
            return i;
    return -1;
}

static void test_leaf_package(void)
{
    FapIndex *idx = alloc_index();
    add_pkg(idx, "curl", NULL);

    FapIndex *out = alloc_index();
    CHECK(fap_resolve(idx, "curl", out) == 0, "resolve succeeds for a dep-free package");
    CHECK(out->count == 1, "output has exactly the root package");
    CHECK(strcmp(out->pkgs[0].name, "curl") == 0, "output is the root package");

    free(idx);
    free(out);
}

static void test_linear_chain(void)
{
    FapIndex *idx = alloc_index();
    add_pkg(idx, "c", NULL);
    add_pkg(idx, "b", "c", NULL);
    add_pkg(idx, "a", "b", NULL);

    FapIndex *out = alloc_index();
    CHECK(fap_resolve(idx, "a", out) == 0, "resolve succeeds on a linear chain a->b->c");
    CHECK(out->count == 3, "all three packages are in the output");

    int ia = index_of(out, "a"), ib = index_of(out, "b"), ic = index_of(out, "c");
    CHECK(ia >= 0 && ib >= 0 && ic >= 0, "a, b, and c all appear in the output");
    CHECK(ic < ib && ib < ia, "install order is c, then b, then a (deps before dependents)");

    free(idx);
    free(out);
}

static void test_diamond_dependency(void)
{
    /* a depends on b and c; b and c both depend on d */
    FapIndex *idx = alloc_index();
    add_pkg(idx, "d", NULL);
    add_pkg(idx, "b", "d", NULL);
    add_pkg(idx, "c", "d", NULL);
    add_pkg(idx, "a", "b", "c", NULL);

    FapIndex *out = alloc_index();
    CHECK(fap_resolve(idx, "a", out) == 0, "resolve succeeds on a diamond dependency");
    CHECK(out->count == 4, "d is not duplicated despite two paths to it");

    int id = index_of(out, "d"), ib = index_of(out, "b"), ic = index_of(out, "c"), ia = index_of(out, "a");
    CHECK(id < ib && id < ic, "d (the shared dependency) comes before both b and c");
    CHECK(ib < ia && ic < ia, "a (the root) comes last");

    free(idx);
    free(out);
}

static void test_self_cycle(void)
{
    FapIndex *idx = alloc_index();
    add_pkg(idx, "a", "a", NULL);

    FapIndex *out = alloc_index();
    int r = fap_resolve(idx, "a", out);
    CHECK(r < 0, "resolve rejects a package depending on itself");
    CHECK(strstr(fap_err, "circular") != NULL, "error mentions a circular dependency");

    free(idx);
    free(out);
}

static void test_indirect_cycle(void)
{
    FapIndex *idx = alloc_index();
    add_pkg(idx, "a", "b", NULL);
    add_pkg(idx, "b", "c", NULL);
    add_pkg(idx, "c", "a", NULL);

    FapIndex *out = alloc_index();
    int r = fap_resolve(idx, "a", out);
    CHECK(r < 0, "resolve rejects a->b->c->a");
    CHECK(strstr(fap_err, "a") && strstr(fap_err, "b") && strstr(fap_err, "c"),
          "cycle error names all three packages involved");

    free(idx);
    free(out);
}

static void test_missing_dependency(void)
{
    FapIndex *idx = alloc_index();
    add_pkg(idx, "a", "ghost", NULL);

    FapIndex *out = alloc_index();
    int r = fap_resolve(idx, "a", out);
    CHECK(r < 0, "resolve rejects a dependency that isn't in the index");
    CHECK(strstr(fap_err, "ghost") != NULL, "error names the missing dependency");

    free(idx);
    free(out);
}

static void test_root_not_found(void)
{
    FapIndex *idx = alloc_index();
    add_pkg(idx, "curl", NULL);

    FapIndex *out = alloc_index();
    CHECK(fap_resolve(idx, "nope", out) < 0, "resolve rejects a root package not in the index");

    free(idx);
    free(out);
}

int main(void)
{
    printf("resolver:\n");
    test_leaf_package();
    test_linear_chain();
    test_diamond_dependency();
    test_self_cycle();
    test_indirect_cycle();
    test_missing_dependency();
    test_root_not_found();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
