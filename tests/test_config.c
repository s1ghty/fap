#include <stdio.h>
#include <string.h>
#include "fap.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); pass++; } \
    else       { printf("  FAIL  %s  [%s]\n", msg, fap_err); fail++; } \
} while(0)

static void write_file(const char *path, const char *contents)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  SKIP  cannot write %s\n", path); return; }
    fputs(contents, f);
    fclose(f);
}

static void test_manifest_sample(void)
{
    const char *tmpfile = "/tmp/fap_test.toml";
    write_file(tmpfile,
        "[package]\n"
        "name = \"myproject\"\n"
        "\n"
        "[dependencies]\n"
        "curl = { version = \"8.6.0\", channel = \"stable\" }\n"
        "jq   = { version = \"1.7\",   channel = \"stable\" }\n"
        "eza  = { channel = \"edge\" }\n");

    FapManifest m;
    CHECK(fap_manifest_load(tmpfile, &m) == 0, "manifest_load succeeds");
    CHECK(strcmp(m.project_name, "myproject") == 0, "project name parsed");
    CHECK(m.deps_count == 3, "dependency count is 3");

    CHECK(strcmp(m.deps[0].name, "curl") == 0, "dep 0 name is curl");
    CHECK(strcmp(m.deps[0].version, "8.6.0") == 0, "dep 0 version parsed");
    CHECK(m.deps[0].channel == FAP_CHANNEL_STABLE, "dep 0 channel stable");

    CHECK(strcmp(m.deps[2].name, "eza") == 0, "dep 2 name is eza");
    CHECK(m.deps[2].version[0] == '\0', "dep 2 version empty (latest)");
    CHECK(m.deps[2].channel == FAP_CHANNEL_EDGE, "dep 2 channel edge");
}

static void test_manifest_no_deps(void)
{
    const char *tmpfile = "/tmp/fap_test_nodeps.toml";
    write_file(tmpfile, "[package]\nname = \"bare\"\n");

    FapManifest m;
    CHECK(fap_manifest_load(tmpfile, &m) == 0, "manifest_load succeeds with no [dependencies]");
    CHECK(m.deps_count == 0, "dependency count is 0");
}

static void test_manifest_missing_package(void)
{
    const char *tmpfile = "/tmp/fap_test_nopkg.toml";
    write_file(tmpfile, "[dependencies]\ncurl = { version = \"1.0\" }\n");

    FapManifest m;
    CHECK(fap_manifest_load(tmpfile, &m) < 0, "manifest_load rejects missing [package]");
}

static void test_manifest_bad_dep_shape(void)
{
    const char *tmpfile = "/tmp/fap_test_baddep.toml";
    write_file(tmpfile, "[package]\nname = \"x\"\n\n[dependencies]\ncurl = \"8.6.0\"\n");

    FapManifest m;
    CHECK(fap_manifest_load(tmpfile, &m) < 0, "manifest_load rejects non-table dependency");
}

static void test_manifest_add_remove_dep(void)
{
    const char *tmpfile = "/tmp/fap_test_addrm.toml";
    remove(tmpfile); /* start from a clean slate, unlike the read-only tests above */

    CHECK(fap_manifest_add_dep(tmpfile, "jq", FAP_CHANNEL_STABLE) == 0,
          "add_dep creates a manifest that didn't exist");

    FapManifest m;
    CHECK(fap_manifest_load(tmpfile, &m) == 0, "created manifest loads back");
    CHECK(strcmp(m.project_name, "system") == 0, "default project name is \"system\"");
    CHECK(m.deps_count == 1 && strcmp(m.deps[0].name, "jq") == 0, "jq dep present");
    CHECK(m.deps[0].version[0] == '\0', "jq dep is unpinned (bare channel)");
    CHECK(m.deps[0].channel == FAP_CHANNEL_STABLE, "jq dep channel is stable");

    CHECK(fap_manifest_add_dep(tmpfile, "fastfetch", FAP_CHANNEL_STABLE) == 0,
          "add_dep adds a second package");
    CHECK(fap_manifest_load(tmpfile, &m) == 0 && m.deps_count == 2,
          "both packages present after second add");

    CHECK(fap_manifest_add_dep(tmpfile, "jq", FAP_CHANNEL_EDGE) == 0,
          "re-adding an existing dep succeeds (upsert)");
    CHECK(fap_manifest_load(tmpfile, &m) == 0 && m.deps_count == 2,
          "upsert doesn't duplicate the entry");
    for (int i = 0; i < m.deps_count; i++)
        if (strcmp(m.deps[i].name, "jq") == 0)
            CHECK(m.deps[i].channel == FAP_CHANNEL_EDGE, "upsert updated jq's channel to edge");

    CHECK(fap_manifest_remove_dep(tmpfile, "jq") == 0, "remove_dep removes jq");
    CHECK(fap_manifest_load(tmpfile, &m) == 0 && m.deps_count == 1
          && strcmp(m.deps[0].name, "fastfetch") == 0,
          "only fastfetch remains after removing jq");

    CHECK(fap_manifest_remove_dep(tmpfile, "doesnotexist") == 0,
          "removing a name that isn't in the manifest is a no-op, not an error");

    remove(tmpfile);
    CHECK(fap_manifest_remove_dep(tmpfile, "jq") == 0,
          "removing from a manifest that doesn't exist at all is also a no-op");
}

int main(void)
{
    printf("config:\n");
    test_manifest_sample();
    test_manifest_no_deps();
    test_manifest_missing_package();
    test_manifest_bad_dep_shape();
    test_manifest_add_remove_dep();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
