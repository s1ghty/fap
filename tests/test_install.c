#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zstd.h>
#include "fap.h"

/* fap_sha256_verify lives in hash.c, which this test doesn't link
 * (kept dependency-free of openssl, same as test_package.c). It's
 * called internally by fap_package_download, which fap_install uses. */
int fap_sha256_verify(const char *path, const char *expected_hex)
{
    (void)path; (void)expected_hex;
    return 0;
}

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); pass++; } \
    else       { printf("  FAIL  %s  [%s]\n", msg, fap_err); fail++; } \
} while(0)

#define BLK 512

static void put_field(unsigned char *hdr, size_t off, size_t len, const char *s)
{
    memset(hdr + off, 0, len);
    memcpy(hdr + off, s, strlen(s) < len ? strlen(s) : len);
}

static void put_octal(unsigned char *hdr, size_t off, size_t len, unsigned long val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%0*lo", (int)len - 1, val);
    memcpy(hdr + off, buf, len - 1);
    hdr[off + len - 1] = '\0';
}

/* Builds a single-file ustar archive ("bin/<binname>", mode 0755,
 * content "#!/bin/sh\necho hi\n"), zstd-compresses it, and writes it
 * to out_path. */
static void write_sample_tarball(const char *out_path, const char *binname)
{
    static const char *src = "#!/bin/sh\necho hi\n";
    unsigned char tar[2 * BLK];
    char name[128];
    snprintf(name, sizeof(name), "bin/%s", binname);

    memset(tar, 0, sizeof(tar));
    put_field(tar, 0, 100, name);
    put_octal(tar, 100, 8, 0755);
    put_octal(tar, 124, 12, strlen(src));
    tar[156] = '0';
    memcpy(tar + 257, "ustar", 5);
    memcpy(tar + BLK, src, strlen(src));

    size_t bound = ZSTD_compressBound(sizeof(tar));
    char *compressed = malloc(bound);
    size_t clen = ZSTD_compress(compressed, bound, tar, sizeof(tar), 3);

    FILE *f = fopen(out_path, "wb");
    fwrite(compressed, 1, clen, f);
    fclose(f);
    free(compressed);
}

/* Builds a two-entry ustar archive: "bin/<binname>" (mode 0755,
 * content "#!/bin/sh\necho hi\n") and "lib/<libname>" (mode 0644,
 * content lib_content). Compresses and writes it to out_path. */
static void write_tarball_with_lib(const char *out_path, const char *binname,
                                    const char *libname, const char *lib_content)
{
    static const char *bin_src = "#!/bin/sh\necho hi\n";
    unsigned char tar[4 * BLK];
    memset(tar, 0, sizeof(tar));
    size_t off = 0;

    char bin_path[128];
    snprintf(bin_path, sizeof(bin_path), "bin/%s", binname);
    put_field(tar + off, 0, 100, bin_path);
    put_octal(tar + off, 100, 8, 0755);
    put_octal(tar + off, 124, 12, strlen(bin_src));
    tar[off + 156] = '0';
    memcpy(tar + off + 257, "ustar", 5);
    off += BLK;
    memcpy(tar + off, bin_src, strlen(bin_src));
    off += BLK;

    char lib_path[128];
    snprintf(lib_path, sizeof(lib_path), "lib/%s", libname);
    put_field(tar + off, 0, 100, lib_path);
    put_octal(tar + off, 100, 8, 0644);
    put_octal(tar + off, 124, 12, strlen(lib_content));
    tar[off + 156] = '0';
    memcpy(tar + off + 257, "ustar", 5);
    off += BLK;
    memcpy(tar + off, lib_content, strlen(lib_content));
    off += BLK;

    size_t bound = ZSTD_compressBound(off);
    char *compressed = malloc(bound);
    size_t clen = ZSTD_compress(compressed, bound, tar, off, 3);

    FILE *f = fopen(out_path, "wb");
    fwrite(compressed, 1, clen, f);
    fclose(f);
    free(compressed);
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int link_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int is_symlink(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static int is_regular_executable(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
}

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char buf[4096] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return strstr(buf, needle) != NULL;
}

static int run_and_capture(const char *path, char *out, size_t outsz)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s 2>&1", path);
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    size_t n = fread(out, 1, outsz - 1, p);
    out[n] = '\0';
    return pclose(p);
}

int main(void)
{
    const char *home = "/tmp/fap_test_install_home";
    system("rm -rf /tmp/fap_test_install_home /tmp/fap_test_install_pkg.tar.zst");
    system("mkdir -p /tmp/fap_test_install_home");
    setenv("HOME", home, 1);

    const char *tarball = "/tmp/fap_test_install_pkg.tar.zst";
    write_sample_tarball(tarball, "tool");

    char url[256];
    snprintf(url, sizeof(url), "file://%s", tarball);

    FapPackage pkg;
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.name, "tool");
    strcpy(pkg.version, "1.0");
    strcpy(pkg.url, url);
    strcpy(pkg.sha256, "unused-stubbed-out");
    pkg.bins_count = 1;
    strcpy(pkg.bins[0], "tool");

    printf("install:\n");

    CHECK(fap_install(&pkg) == 0, "install succeeds");
    CHECK(exists("/tmp/fap_test_install_home/.local/fap/pkgs/tool-1.0/bin/tool"),
          "binary placed under pkgs/<name>-<version>/bin/");

    char linktarget[256] = {0};
    ssize_t n = readlink("/tmp/fap_test_install_home/.local/bin/tool", linktarget, sizeof(linktarget) - 1);
    CHECK(n > 0, "bin/ symlink created");
    CHECK(n > 0 && strcmp(linktarget, "../fap/pkgs/tool-1.0/bin/tool") == 0,
          "symlink points at ../fap/pkgs/<name>-<version>/bin/<bin>");

    FILE *f = fopen("/tmp/fap_test_install_home/.local/bin/tool", "rb");
    char buf[128] = {0};
    if (f) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    CHECK(f != NULL && strcmp(buf, "#!/bin/sh\necho hi\n") == 0,
          "symlink resolves to correct extracted content");

    CHECK(!exists("/tmp/fap_test_install_home/.local/fap/staging/tool-1.0"),
          "staging dir cleaned up after successful install");

    CHECK(fap_install(&pkg) == 0, "reinstalling same name-version succeeds");
    CHECK(exists("/tmp/fap_test_install_home/.local/fap/pkgs/tool-1.0/bin/tool"),
          "binary still present after reinstall");

    FapPackage bad = pkg;
    strcpy(bad.name, "brokenpkg");
    strcpy(bad.bins[0], "doesnotexist");
    CHECK(fap_install(&bad) < 0, "install fails when expected binary is missing");
    CHECK(!exists("/tmp/fap_test_install_home/.local/fap/pkgs/brokenpkg-1.0"),
          "failed install leaves no package dir behind");
    CHECK(!exists("/tmp/fap_test_install_home/.local/fap/staging/brokenpkg-1.0"),
          "failed install cleans up its staging dir");

    CHECK(fap_remove("tool") == 0, "remove succeeds");
    CHECK(!exists("/tmp/fap_test_install_home/.local/fap/pkgs/tool-1.0"),
          "package dir removed");
    CHECK(!link_exists("/tmp/fap_test_install_home/.local/bin/tool"), "bin symlink removed");

    CHECK(fap_remove("tool") < 0, "removing an uninstalled package fails");
    CHECK(fap_remove("nope") < 0, "removing an unknown package fails");

    /* ── packages with bundled libs get wrapper scripts, not symlinks ── */

    const char *libtarball = "/tmp/fap_test_install_libpkg.tar.zst";
    write_tarball_with_lib(libtarball, "libbin", "libfake.so.1", "FAKE SHARED OBJECT CONTENT");
    char liburl[256];
    snprintf(liburl, sizeof(liburl), "file://%s", libtarball);

    FapPackage libpkg;
    memset(&libpkg, 0, sizeof(libpkg));
    strcpy(libpkg.name, "libpkg");
    strcpy(libpkg.version, "1.0");
    strcpy(libpkg.url, liburl);
    strcpy(libpkg.sha256, "unused-stubbed-out");
    libpkg.bins_count = 1;
    strcpy(libpkg.bins[0], "libbin");
    libpkg.libs_count = 1;
    strcpy(libpkg.libs[0], "libfake.so.1");

    CHECK(fap_install(&libpkg) == 0, "install succeeds for a package with libs");

    const char *libs_dst = "/tmp/fap_test_install_home/.local/fap/libs/libfake.so.1";
    CHECK(exists(libs_dst), "bundled .so copied into ~/.local/fap/libs/");
    CHECK(file_contains(libs_dst, "FAKE SHARED OBJECT CONTENT"), "copied .so has correct content");

    const char *libbin_link = "/tmp/fap_test_install_home/.local/bin/libbin";
    CHECK(!is_symlink(libbin_link), "binary from a libs package is NOT a plain symlink");
    CHECK(is_regular_executable(libbin_link), "it's an executable regular file (wrapper script) instead");
    CHECK(file_contains(libbin_link, "/.local/fap/libs"),
          "wrapper script references the shared libs dir");
    CHECK(file_contains(libbin_link, "/.local/fap/pkgs/libpkg-1.0/bin/libbin"),
          "wrapper script execs the real installed binary");

    char output[256] = {0};
    int run_rc = run_and_capture(libbin_link, output, sizeof(output));
    CHECK(run_rc == 0 && strcmp(output, "hi\n") == 0,
          "running the wrapper script actually executes the real binary");

    FapPackage bad_lib = libpkg;
    strcpy(bad_lib.name, "brokenlibpkg");
    strcpy(bad_lib.libs[0], "doesnotexist.so");
    CHECK(fap_install(&bad_lib) < 0, "install fails when a declared lib is missing from the package");
    CHECK(!exists("/tmp/fap_test_install_home/.local/fap/pkgs/brokenlibpkg-1.0"),
          "failed lib install leaves no package dir behind");

    CHECK(fap_remove("libpkg") == 0, "remove succeeds for a libs package");
    CHECK(!exists("/tmp/fap_test_install_home/.local/fap/pkgs/libpkg-1.0"),
          "libpkg package dir removed");
    CHECK(!link_exists(libbin_link),
          "wrapper script removed too (fap_remove isn't just readlink-based)");
    CHECK(exists(libs_dst),
          "shared libs dir is NOT cleaned up on remove (known simplification, see install.c)");

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
