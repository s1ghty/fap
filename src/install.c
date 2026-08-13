#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "fap.h"

/*
 * install.c — atomic install via staging directory + symlinks
 *
 * Flow (paths below shown for user mode; fap_root_path()/fap_bin_path()
 * resolve to the system-wide root instead when running as root — see
 * fap.h):
 *   1. Extract tarball into ~/.local/fap/staging/<name>-<version>/
 *      (fap_package_download does download + SHA256 verify + extract)
 *   2. Verify all expected binaries and bundled libs are present
 *   3. rename() staging dir to ~/.local/fap/pkgs/<name>-<version>/
 *   4. Copy any bundled .so files (pkg->libs) into ~/.local/fap/libs/
 *   5. Install each binary into ~/.local/bin/ — a plain symlink, or if
 *      the package has libs, a wrapper script that points the dynamic
 *      linker at ~/.local/fap/libs/ first (see fap.h for why: no
 *      per-user hook into ld.so short of editing /etc/ld.so.conf.d,
 *      which needs root)
 *
 * rename() is atomic on the same filesystem, so a crash anywhere
 * before step 3 leaves ~/.local/fap/pkgs/ untouched.
 *
 * Symlink/wrapper targets are always absolute, never relative — in
 * system mode the bin dir (/usr/local/bin) and the package root
 * (/var/lib/fap) aren't siblings the way ~/.local/bin and
 * ~/.local/fap are, so a relative "../fap/pkgs/..." target would
 * resolve to the wrong place.
 */

static int pkg_dirname(const FapPackage *pkg, char *buf, size_t bufsz)
{
    int n = snprintf(buf, bufsz, "%s-%s", pkg->name, pkg->version);
    if (n < 0 || (size_t)n >= bufsz)
        return fap_error("install: package dir name too long");
    return 0;
}

static int verify_present(const char *staging_dir, const char *subdir,
                           const char *name, const char *kind)
{
    char path[FAP_MAX_PATH];
    int n = snprintf(path, sizeof(path), "%s/%s/%s", staging_dir, subdir, name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return fap_error("install: %s path too long", kind);
    struct stat st;
    if (stat(path, &st) < 0)
        return fap_error("install: expected %s \"%s\" missing from package", kind, name);
    return 0;
}

/* Copies each pkg->libs[i] from <pkg_dir>/lib/ into the single shared
 * libs_root, so every installed package's wrapper scripts can find
 * them via one LD_LIBRARY_PATH entry.
 *
 * ponytail: one flat shared dir, no per-package subdirs — two
 * packages bundling a same-named-but-different lib will clobber each
 * other here. Acceptable for now; the fix if it ever bites is
 * per-package lib dirs (like pkgs/) plus a search path built
 * per-binary instead of one global directory. Removal-side reference
 * counting (don't delete a lib another remaining package still
 * declares) is handled in cli.c's cleanup_orphaned_libs(), using the
 * libs[] each package already records in fap.lock — this comment is
 * only about the install-time same-name-different-content clobber
 * risk, which counting refs on removal doesn't touch. */
static int install_libs(const FapPackage *pkg, const char *pkg_dir, const char *libs_root)
{
    if (pkg->libs_count == 0)
        return 0;
    if (fap_mkdir_p(libs_root) < 0)
        return -1;

    for (int i = 0; i < pkg->libs_count; i++) {
        char src[FAP_MAX_PATH], dst[FAP_MAX_PATH];
        int n = snprintf(src, sizeof(src), "%s/lib/%s", pkg_dir, pkg->libs[i]);
        if (n < 0 || (size_t)n >= sizeof(src))
            return fap_error("install: lib path too long");
        n = snprintf(dst, sizeof(dst), "%s/%s", libs_root, pkg->libs[i]);
        if (n < 0 || (size_t)n >= sizeof(dst))
            return fap_error("install: lib path too long");
        if (fap_copy_file(src, dst) < 0)
            return -1;
    }
    return 0;
}

/* Writes an executable shell wrapper at link that exports
 * LD_LIBRARY_PATH (prepending libs_root) and execs real_bin, atomically
 * (temp file + rename, same pattern as fap_symlink_force). */
static int write_wrapper(const char *link, const char *real_bin, const char *libs_root)
{
    char tmp[FAP_MAX_PATH];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", link);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return fap_error("install: wrapper path too long");

    FILE *f = fopen(tmp, "w");
    if (!f)
        return fap_error("install: open %s for write: %s", tmp, strerror(errno));
    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "export LD_LIBRARY_PATH=\"%s${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n", libs_root);
    fprintf(f, "exec \"%s\" \"$@\"\n", real_bin);
    if (fclose(f) != 0) {
        unlink(tmp);
        return fap_error("install: write %s: %s", tmp, strerror(errno));
    }

    if (chmod(tmp, 0755) < 0) {
        unlink(tmp);
        return fap_error("install: chmod %s: %s", tmp, strerror(errno));
    }
    if (rename(tmp, link) < 0) {
        unlink(tmp);
        return fap_error("install: rename %s -> %s: %s", tmp, link, strerror(errno));
    }
    return 0;
}

static int install_bins(const FapPackage *pkg, const char *pkg_dir,
                         const char *bin_root, const char *libs_root)
{
    for (int i = 0; i < pkg->bins_count; i++) {
        char link[FAP_MAX_PATH];
        int n = snprintf(link, sizeof(link), "%s/%s", bin_root, pkg->bins[i]);
        if (n < 0 || (size_t)n >= sizeof(link))
            return fap_error("install: symlink path too long");

        char real_bin[FAP_MAX_PATH];
        n = snprintf(real_bin, sizeof(real_bin), "%s/bin/%s", pkg_dir, pkg->bins[i]);
        if (n < 0 || (size_t)n >= sizeof(real_bin))
            return fap_error("install: binary path too long");

        if (pkg->libs_count > 0) {
            if (write_wrapper(link, real_bin, libs_root) < 0)
                return -1;
        } else {
            if (fap_symlink_force(real_bin, link) < 0)
                return -1;
        }
    }
    return 0;
}

int fap_install(const FapPackage *pkg)
{
    char dirname[FAP_MAX_NAME + FAP_MAX_VERSION + 2];
    if (pkg_dirname(pkg, dirname, sizeof(dirname)) < 0)
        return -1;

    /* staging and pkgs must resolve under the same root for rename()
     * to be an atomic same-filesystem move — fap_root_path() guarantees
     * that in both user and system mode. */
    char staging_root[FAP_MAX_PATH], pkgs_root[FAP_MAX_PATH];
    char bin_root[FAP_MAX_PATH], libs_root[FAP_MAX_PATH];
    if (fap_root_path(FAP_STAGING, staging_root, sizeof(staging_root)) < 0 ||
        fap_root_path(FAP_PKGS, pkgs_root, sizeof(pkgs_root)) < 0 ||
        fap_bin_path(bin_root, sizeof(bin_root)) < 0 ||
        fap_root_path(FAP_LIBS, libs_root, sizeof(libs_root)) < 0)
        return -1;

    char staging_dir[FAP_MAX_PATH], pkg_dir[FAP_MAX_PATH];
    int n = snprintf(staging_dir, sizeof(staging_dir), "%s/%s", staging_root, dirname);
    if (n < 0 || (size_t)n >= sizeof(staging_dir))
        return fap_error("install: staging path too long");
    n = snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", pkgs_root, dirname);
    if (n < 0 || (size_t)n >= sizeof(pkg_dir))
        return fap_error("install: package path too long");

    /* clear any leftover staging dir from a previous crashed/failed install */
    if (fap_rm_rf(staging_dir) < 0)
        return -1;

    if (fap_package_download(pkg, staging_dir) < 0) {
        fap_rm_rf(staging_dir);
        return -1;
    }

    for (int i = 0; i < pkg->bins_count; i++) {
        if (verify_present(staging_dir, "bin", pkg->bins[i], "binary") < 0) {
            fap_rm_rf(staging_dir);
            return -1;
        }
    }
    for (int i = 0; i < pkg->libs_count; i++) {
        if (verify_present(staging_dir, "lib", pkg->libs[i], "library") < 0) {
            fap_rm_rf(staging_dir);
            return -1;
        }
    }

    if (fap_mkdir_p(pkgs_root) < 0) {
        fap_rm_rf(staging_dir);
        return -1;
    }
    /* replace any existing install of this exact name-version */
    if (fap_rm_rf(pkg_dir) < 0) {
        fap_rm_rf(staging_dir);
        return -1;
    }
    if (rename(staging_dir, pkg_dir) < 0) {
        fap_rm_rf(staging_dir);
        return fap_error("install: rename %s -> %s: %s", staging_dir, pkg_dir, strerror(errno));
    }

    if (install_libs(pkg, pkg_dir, libs_root) < 0)
        return -1;

    if (fap_mkdir_p(bin_root) < 0)
        return -1;

    return install_bins(pkg, pkg_dir, bin_root, libs_root);
}

/* A bin/ entry belongs to abs_pkg_dir's package if it's a plain
 * symlink pointing inside it, or — for a libs wrapper script, which
 * isn't a symlink — a regular file whose content references it.
 * Both symlink targets and wrapper script content are always
 * absolute (see the file header comment on why), so one prefix/substring
 * check covers both cases. */
static int bin_belongs_to(const char *link_path, const char *abs_pkg_dir)
{
    struct stat st;
    if (lstat(link_path, &st) < 0)
        return 0;

    if (S_ISLNK(st.st_mode)) {
        char target[FAP_MAX_PATH];
        ssize_t r = readlink(link_path, target, sizeof(target) - 1);
        if (r < 0)
            return 0;
        target[r] = '\0';
        return strncmp(target, abs_pkg_dir, strlen(abs_pkg_dir)) == 0;
    }

    if (S_ISREG(st.st_mode)) {
        FILE *f = fopen(link_path, "r");
        if (!f)
            return 0;
        char content[FAP_MAX_PATH * 2];
        size_t rd = fread(content, 1, sizeof(content) - 1, f);
        fclose(f);
        content[rd] = '\0';
        return strstr(content, abs_pkg_dir) != NULL;
    }

    return 0;
}

/* Scans pkgs_root for a directory named "<name>-<anything>"; copies
 * it into dirname_out if found. A missing/unreadable pkgs_root just
 * means not found, same as no matching entry. */
static int find_pkg_dirname(const char *pkgs_root, const char *name,
                             char *dirname_out, size_t dirname_sz)
{
    DIR *d = opendir(pkgs_root);
    if (!d)
        return 0;
    size_t namelen = strlen(name);
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        if (strncmp(ent->d_name, name, namelen) == 0 && ent->d_name[namelen] == '-') {
            snprintf(dirname_out, dirname_sz, "%s", ent->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

int fap_remove(const char *name)
{
    char pkgs_root[FAP_MAX_PATH], bin_root[FAP_MAX_PATH];
    if (fap_root_path(FAP_PKGS, pkgs_root, sizeof(pkgs_root)) < 0 ||
        fap_bin_path(bin_root, sizeof(bin_root)) < 0)
        return -1;

    char dirname[FAP_MAX_NAME + FAP_MAX_VERSION + 2] = { 0 };

    if (!find_pkg_dirname(pkgs_root, name, dirname, sizeof(dirname))) {
        /* Not installed at this privilege level — but maybe it's
         * installed at the other one, which is a much more useful
         * thing to tell you than a flat "not installed" you know is
         * wrong because you just installed it a minute ago. */
        char other_pkgs_root[FAP_MAX_PATH], other_dirname[FAP_MAX_NAME + FAP_MAX_VERSION + 2] = { 0 };
        if (fap_other_root_path(FAP_PKGS, other_pkgs_root, sizeof(other_pkgs_root)) == 0 &&
            find_pkg_dirname(other_pkgs_root, name, other_dirname, sizeof(other_dirname))) {
            return fap_is_system_mode()
                ? fap_error("remove: \"%s\" is installed for a regular user, not system-wide — run without sudo to remove it", name)
                : fap_error("remove: \"%s\" is installed system-wide, not for your user — run with sudo to remove it", name);
        }
        return fap_error("remove: package \"%s\" not installed", name);
    }

    /* remove only the bin/ entries that belong to this package dir */
    char pkg_dir[FAP_MAX_PATH];
    int n = snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", pkgs_root, dirname);
    if (n < 0 || (size_t)n >= sizeof(pkg_dir))
        return fap_error("remove: path too long");

    struct dirent *ent;
    DIR *bd = opendir(bin_root);
    if (bd) {
        while ((ent = readdir(bd)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;
            char link_path[FAP_MAX_PATH];
            if (snprintf(link_path, sizeof(link_path), "%s/%s", bin_root, ent->d_name) >= (int)sizeof(link_path))
                continue;
            if (bin_belongs_to(link_path, pkg_dir))
                unlink(link_path);
        }
        closedir(bd);
    }

    /* Shared libs cleanup (deleting libs_root/<lib> if no other
     * remaining package still needs it) happens one layer up, in
     * cli.c's cmd_remove — this function only knows the package's
     * name, not its libs[] list, which lives in fap.lock, not on
     * disk. */
    return fap_rm_rf(pkg_dir);
}
