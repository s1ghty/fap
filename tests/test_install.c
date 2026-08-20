#define _POSIX_C_SOURCE 200809L
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

typedef struct {
    const char *path;
    const char *content;
    unsigned    mode;
} TarEntry;

/* General multi-entry ustar archive builder, for tree-shaped packages
 * (a binary nested under a nonstandard path, plus sibling resource
 * files) that the fixed-shape helpers above can't express. */
static void write_tarball(const char *out_path, const TarEntry *entries, int count)
{
    size_t total_blocks = 0;
    for (int i = 0; i < count; i++)
        total_blocks += 1 + (strlen(entries[i].content) + BLK - 1) / BLK;
    size_t buf_size = (total_blocks + 2) * BLK; /* +2 zero blocks: end-of-archive marker */

    unsigned char *tar = calloc(buf_size, 1);
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        size_t clen = strlen(entries[i].content);
        put_field(tar, off, 100, entries[i].path);
        put_octal(tar, off + 100, 8, entries[i].mode);
        put_octal(tar, off + 124, 12, clen);
        tar[off + 156] = '0';
        memcpy(tar + off + 257, "ustar", 5);
        off += BLK;
        memcpy(tar + off, entries[i].content, clen);
        off += ((clen + BLK - 1) / BLK) * BLK;
    }

    size_t bound = ZSTD_compressBound(buf_size);
    char *compressed = malloc(bound);
    size_t clen = ZSTD_compress(compressed, bound, tar, buf_size, 3);

    FILE *f = fopen(out_path, "wb");
    fwrite(compressed, 1, clen, f);
    fclose(f);
    free(compressed);
    free(tar);
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
    CHECK(n > 0 && strcmp(linktarget, "/tmp/fap_test_install_home/.local/fap/pkgs/tool-1.0/bin/tool") == 0,
          "symlink target is absolute (not a relative ../fap/pkgs/... path — "
          "system mode's bin dir and pkgs root aren't siblings)");

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

    /* ── tree-shaped packages: a bin entry with a '/' is an explicit
     * path, not a bare name — for a package that ships a real resource
     * tree alongside its binary (neovim's share/nvim/runtime/,
     * Firefox's bundled locale/plugin files), not just bin/<name> ── */

    const char *treetarball = "/tmp/fap_test_install_tree.tar.zst";
    TarEntry tree_entries[] = {
        { "myapp/bin/myapp", "#!/bin/sh\necho hello from myapp\n", 0755 },
        { "myapp/share/myapp/runtime/colors.txt", "some colorscheme data\n", 0644 },
        { "myapp/share/myapp/runtime/syntax/c.txt", "c syntax rules\n", 0644 },
        { "myapp/README.md", "just a doc file, not bin or lib\n", 0644 },
    };
    write_tarball(treetarball, tree_entries, 4);
    char treeurl[256];
    snprintf(treeurl, sizeof(treeurl), "file://%s", treetarball);

    FapPackage treepkg;
    memset(&treepkg, 0, sizeof(treepkg));
    strcpy(treepkg.name, "myapp");
    strcpy(treepkg.version, "1.0");
    strcpy(treepkg.url, treeurl);
    strcpy(treepkg.sha256, "unused-stubbed-out");
    treepkg.bins_count = 1;
    strcpy(treepkg.bins[0], "myapp/bin/myapp");

    printf("\ntree-shaped packages:\n");

    CHECK(fap_install(&treepkg) == 0, "install succeeds for an explicit-path bin entry");
    CHECK(exists("/tmp/fap_test_install_home/.local/fap/pkgs/myapp-1.0/myapp/bin/myapp"),
          "binary placed at its declared path, not forced under bin/");
    CHECK(exists("/tmp/fap_test_install_home/.local/fap/pkgs/myapp-1.0/myapp/share/myapp/runtime/colors.txt"),
          "sibling resource file preserved");
    CHECK(exists("/tmp/fap_test_install_home/.local/fap/pkgs/myapp-1.0/myapp/share/myapp/runtime/syntax/c.txt"),
          "nested resource file (2 dirs deep) preserved");
    CHECK(exists("/tmp/fap_test_install_home/.local/fap/pkgs/myapp-1.0/myapp/README.md"),
          "a file that's neither bin/ nor lib/ is preserved too");

    const char *myapp_link = "/tmp/fap_test_install_home/.local/bin/myapp";
    char treelinktarget[256] = {0};
    ssize_t tn = readlink(myapp_link, treelinktarget, sizeof(treelinktarget) - 1);
    CHECK(tn > 0, "installed command uses the basename, not the full declared path");
    CHECK(tn > 0 && strcmp(treelinktarget,
          "/tmp/fap_test_install_home/.local/fap/pkgs/myapp-1.0/myapp/bin/myapp") == 0,
          "symlink target is the exact declared path, absolute");

    char treeoutput[256] = {0};
    int tree_run_rc = run_and_capture(myapp_link, treeoutput, sizeof(treeoutput));
    CHECK(tree_run_rc == 0 && strcmp(treeoutput, "hello from myapp\n") == 0,
          "running the installed command actually executes the real binary");

    CHECK(fap_remove("myapp") == 0, "remove succeeds for a tree-shaped package");
    CHECK(!exists("/tmp/fap_test_install_home/.local/fap/pkgs/myapp-1.0"),
          "whole package tree removed, including the resource files");
    CHECK(!link_exists(myapp_link), "bin symlink removed");

    /* ── desktop entries: most packages never set desktop_type (no
     * .desktop file at all, the default/common case); one that does
     * gets a real XDG entry written and cleaned up on remove ── */

    printf("\ndesktop entries:\n");

    const char *nodesktop_desktop_file =
        "/tmp/fap_test_install_home/.local/share/applications/tool.desktop";

    FapPackage plain = pkg; /* the "tool" package from way above, no desktop_type */
    CHECK(fap_install(&plain) == 0, "install succeeds for a package with no desktop_type");
    CHECK(!exists(nodesktop_desktop_file),
          "no .desktop file written when desktop_type isn't set (the common/default case)");
    fap_remove("tool");

    FapPackage app = pkg;
    strcpy(app.desktop_type, "application");
    strcpy(app.desktop_name, "My Tool");
    strcpy(app.description, "a test tool");

    CHECK(fap_install(&app) == 0, "install succeeds for a package with desktop_type=application");
    CHECK(exists(nodesktop_desktop_file), "a .desktop file was written to ~/.local/share/applications/");
    CHECK(file_contains(nodesktop_desktop_file, "Name=My Tool"), "Name= uses desktop_name");
    CHECK(file_contains(nodesktop_desktop_file, "Comment=a test tool"), "Comment= uses description");
    CHECK(file_contains(nodesktop_desktop_file, "Type=Application"), "Type=Application present");
    CHECK(file_contains(nodesktop_desktop_file, "Exec=/tmp/fap_test_install_home/.local/bin/tool"),
          "Exec= points at the real, absolute installed binary path");
    CHECK(file_contains(nodesktop_desktop_file, "TryExec=/tmp/fap_test_install_home/.local/bin/tool"),
          "TryExec= matches Exec=");
    CHECK(!file_contains(nodesktop_desktop_file, "DesktopNames"),
          "no DesktopNames= for a plain application entry (only wayland-session needs it)");

    CHECK(fap_remove("tool") == 0, "remove succeeds for a package with a desktop entry");
    CHECK(!exists(nodesktop_desktop_file),
          "the .desktop file is cleaned up on remove (deterministic filename, no manifest needed)");

    /* x11-session/wayland-session only make sense in system mode — a
     * login manager reads them before any user is authenticated, so
     * there's no per-user equivalent. Tests run as a regular user, so
     * this exercises the real rejection path, not a hypothetical one.
     * install_desktop_entry() is fap_install()'s last step, running
     * after the package dir is already renamed into place and its
     * bins already symlinked — same as install_libs()/install_bins()
     * failing, this doesn't roll back what already succeeded (an
     * existing behavior, not new here): the core install is still
     * good, only the optional desktop-entry step failed. */
    FapPackage waysession = pkg;
    strcpy(waysession.desktop_type, "wayland-session");
    CHECK(fap_install(&waysession) < 0,
          "install fails for a wayland-session entry when not running as root");
    CHECK(exists("/tmp/fap_test_install_home/.local/fap/pkgs/tool-1.0"),
          "the core install (package dir) still succeeded despite the desktop-entry step failing");
    fap_remove("tool");

    FapPackage nodesktopbin = pkg;
    strcpy(nodesktopbin.desktop_type, "application");
    nodesktopbin.bins_count = 0;
    CHECK(fap_install(&nodesktopbin) < 0,
          "install fails when desktop_type is set but the package has no bin entries");

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
