#define _XOPEN_SOURCE 700
/* flock() (fap_acquire_lock() below) isn't part of XOPEN/POSIX — it's
 * a glibc extension gated behind this feature-test macro, not a GCC
 * language extension (CLAUDE.md's "no GCC extensions" is about the
 * compiler, not glibc's optional POSIX-adjacent library surface). Its
 * simpler per-open-file-description semantics (vs. fcntl(F_SETLK)'s
 * surprising per-process locking, where a second lock from the same
 * process doesn't conflict and closing *any* fd drops *all* locks the
 * process holds on the file) are what make "is another fap already
 * running" straightforward to both implement and test. Fine to rely on
 * a Linux-glibc-specific function here — CLAUDE.md already scopes fap
 * to Linux x86_64 only. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <ftw.h>
#include <pwd.h>
#include "fap.h"

char fap_err[512];

int fap_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fap_err, sizeof(fap_err), fmt, ap);
    va_end(ap);
    return -1;
}

int fap_home_path(const char *rel, char *buf, size_t bufsz)
{
    const char *home = getenv("HOME");
    if (!home)
        return fap_error("HOME not set");
    int n = snprintf(buf, bufsz, "%s/%s", home, rel);
    if (n < 0 || (size_t)n >= bufsz)
        return fap_error("path too long: %s/%s", home, rel);
    return 0;
}

int fap_is_system_mode(void)
{
    return geteuid() == 0;
}

int fap_root_path(const char *sub, char *buf, size_t bufsz)
{
    if (fap_is_system_mode()) {
        int n = (sub && *sub)
            ? snprintf(buf, bufsz, "%s/%s", FAP_SYSTEM_ROOT, sub)
            : snprintf(buf, bufsz, "%s", FAP_SYSTEM_ROOT);
        if (n < 0 || (size_t)n >= bufsz)
            return fap_error("path too long: %s/%s", FAP_SYSTEM_ROOT, sub ? sub : "");
        return 0;
    }

    char rel[FAP_MAX_PATH];
    int n = (sub && *sub)
        ? snprintf(rel, sizeof(rel), "%s/%s", FAP_USER_ROOT, sub)
        : snprintf(rel, sizeof(rel), "%s", FAP_USER_ROOT);
    if (n < 0 || (size_t)n >= sizeof(rel))
        return fap_error("path too long: %s/%s", FAP_USER_ROOT, sub ? sub : "");
    return fap_home_path(rel, buf, bufsz);
}

/* Resolves the invoking user's real home directory: SUDO_USER's
 * passwd entry if running under sudo, else plain $HOME. sudo resets
 * $HOME to the target user's (/root) by default, not the original
 * invoking user's — needed so fap_other_root_path() checks the
 * actual person's ~/.local/fap, not root's. */
static int real_user_home(char *buf, size_t bufsz)
{
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && *sudo_user) {
        struct passwd *pw = getpwnam(sudo_user);
        if (pw && pw->pw_dir) {
            int n = snprintf(buf, bufsz, "%s", pw->pw_dir);
            if (n < 0 || (size_t)n >= bufsz)
                return fap_error("path too long: %s", pw->pw_dir);
            return 0;
        }
    }
    const char *home = getenv("HOME");
    if (!home)
        return fap_error("HOME not set");
    int n = snprintf(buf, bufsz, "%s", home);
    if (n < 0 || (size_t)n >= bufsz)
        return fap_error("path too long: %s", home);
    return 0;
}

int fap_other_root_path(const char *sub, char *buf, size_t bufsz)
{
    if (fap_is_system_mode()) {
        char home[FAP_MAX_PATH];
        if (real_user_home(home, sizeof(home)) < 0)
            return -1;
        int n = (sub && *sub)
            ? snprintf(buf, bufsz, "%s/%s/%s", home, FAP_USER_ROOT, sub)
            : snprintf(buf, bufsz, "%s/%s", home, FAP_USER_ROOT);
        if (n < 0 || (size_t)n >= bufsz)
            return fap_error("path too long: %s/%s/%s", home, FAP_USER_ROOT, sub ? sub : "");
        return 0;
    }

    int n = (sub && *sub)
        ? snprintf(buf, bufsz, "%s/%s", FAP_SYSTEM_ROOT, sub)
        : snprintf(buf, bufsz, "%s", FAP_SYSTEM_ROOT);
    if (n < 0 || (size_t)n >= bufsz)
        return fap_error("path too long: %s/%s", FAP_SYSTEM_ROOT, sub ? sub : "");
    return 0;
}

int fap_bin_path(char *buf, size_t bufsz)
{
    if (fap_is_system_mode()) {
        int n = snprintf(buf, bufsz, "%s", FAP_SYSTEM_BIN);
        if (n < 0 || (size_t)n >= bufsz)
            return fap_error("path too long: %s", FAP_SYSTEM_BIN);
        return 0;
    }
    return fap_home_path(FAP_USER_BIN, buf, bufsz);
}

int fap_desktop_dir(const char *desktop_type, char *buf, size_t bufsz)
{
    if (!desktop_type)
        return fap_error("desktop: no desktop_type given");

    if (strcmp(desktop_type, "application") == 0) {
        if (fap_is_system_mode()) {
            int n = snprintf(buf, bufsz, "%s", FAP_SYSTEM_APPLICATIONS);
            if (n < 0 || (size_t)n >= bufsz)
                return fap_error("path too long: %s", FAP_SYSTEM_APPLICATIONS);
            return 0;
        }
        return fap_home_path(FAP_USER_APPLICATIONS, buf, bufsz);
    }

    if (strcmp(desktop_type, "x11-session") == 0 || strcmp(desktop_type, "wayland-session") == 0) {
        if (!fap_is_system_mode())
            return fap_error("desktop: %s entries only apply in system mode (sudo) — "
                              "a login manager reads them before any user is authenticated, "
                              "so there's no per-user equivalent", desktop_type);
        const char *dir = strcmp(desktop_type, "x11-session") == 0 ? FAP_XSESSIONS : FAP_WAYLAND_SESSIONS;
        int n = snprintf(buf, bufsz, "%s", dir);
        if (n < 0 || (size_t)n >= bufsz)
            return fap_error("path too long: %s", dir);
        return 0;
    }

    return fap_error("desktop: unknown desktop_type \"%s\"", desktop_type);
}

/* mkdir -p: create all intermediate dirs */
int fap_mkdir_p(const char *path)
{
    char tmp[FAP_MAX_PATH];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return fap_error("path too long: %s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return fap_error("mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return fap_error("mkdir %s: %s", tmp, strerror(errno));
    return 0;
}

/* Create or replace a symlink atomically */
int fap_symlink_force(const char *target, const char *link)
{
    /* Use a temp name then rename for atomicity */
    char tmp[FAP_MAX_PATH];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", link);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return fap_error("link path too long");

    unlink(tmp);
    if (symlink(target, tmp) < 0)
        return fap_error("symlink %s -> %s: %s", tmp, target, strerror(errno));
    if (rename(tmp, link) < 0) {
        unlink(tmp);
        return fap_error("rename %s -> %s: %s", tmp, link, strerror(errno));
    }
    return 0;
}

/* Copy src to dst, preserving src's permission bits, atomically
 * (write to a temp file, then rename over dst). */
int fap_copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return fap_error("copy: open %s: %s", src, strerror(errno));

    struct stat st;
    if (fstat(fileno(in), &st) < 0) {
        fclose(in);
        return fap_error("copy: stat %s: %s", src, strerror(errno));
    }

    char tmp[FAP_MAX_PATH];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        fclose(in);
        return fap_error("copy: path too long: %s", dst);
    }

    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        return fap_error("copy: open %s for write: %s", tmp, strerror(errno));
    }

    char buf[65536];
    size_t n_read;
    while ((n_read = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n_read, out) != n_read) {
            fclose(in);
            fclose(out);
            unlink(tmp);
            return fap_error("copy: write %s: %s", tmp, strerror(errno));
        }
    }
    fclose(in);
    if (fclose(out) != 0) {
        unlink(tmp);
        return fap_error("copy: close %s: %s", tmp, strerror(errno));
    }

    if (chmod(tmp, st.st_mode & 07777) < 0) {
        unlink(tmp);
        return fap_error("copy: chmod %s: %s", tmp, strerror(errno));
    }
    if (rename(tmp, dst) < 0) {
        unlink(tmp);
        return fap_error("copy: rename %s -> %s: %s", tmp, dst, strerror(errno));
    }
    return 0;
}

static int rm_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_DP)
        return rmdir(path);
    return unlink(path);
}

/* Recursively remove path (file, symlink, or directory tree). A
 * missing path is not an error. */
int fap_rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT)
            return 0;
        return fap_error("rm_rf: stat %s: %s", path, strerror(errno));
    }
    /* nftw() on a path that isn't a directory is unreliable across
     * platforms — confirmed failing outright with ENOTDIR on macOS's
     * libc, even though every prior caller of this function only ever
     * passed a directory and never hit it. unlink() directly for the
     * plain file/symlink case instead of routing it through a
     * tree-walk API that isn't guaranteed to support it. */
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) < 0)
            return fap_error("rm_rf: unlink %s: %s", path, strerror(errno));
        return 0;
    }
    if (nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS) < 0)
        return fap_error("rm_rf: %s: %s", path, strerror(errno));
    return 0;
}

void fap_channel_str(FapChannel ch, char *buf, size_t bufsz)
{
    switch (ch) {
    case FAP_CHANNEL_STABLE: snprintf(buf, bufsz, "stable"); break;
    case FAP_CHANNEL_EDGE:   snprintf(buf, bufsz, "edge");   break;
    default:                 snprintf(buf, bufsz, "unknown");
    }
}

int fap_channel_parse(const char *s, FapChannel *out)
{
    if (strcmp(s, "stable") == 0) { *out = FAP_CHANNEL_STABLE; return 0; }
    if (strcmp(s, "edge")   == 0) { *out = FAP_CHANNEL_EDGE;   return 0; }
    return fap_error("unknown channel: %s", s);
}

/* fap.toml/fap.lock are machine-wide state (see CLAUDE.md) but nothing
 * previously stopped two concurrent `fap install`/`remove`/`sync`/
 * `update` invocations from racing a read-modify-write on them — the
 * atomic rename each write already does only prevents a torn *read*,
 * not two processes each computing a new version from the same stale
 * snapshot and clobbering each other's update. main.c acquires this
 * around every command that writes either file, and only those —
 * `fap search`/`list`/`info`/`channels` never touch them, so they stay
 * unblocked. Non-blocking + fail-fast (not queued/blocking) on
 * purpose, same as dpkg: telling the user another fap is already
 * running is more useful than hanging indefinitely behind it. The fd
 * is owned by the caller (main.c), not held in file-scope state here —
 * this file's only global is fap_err (see CLAUDE.md's coding
 * conventions). */
int fap_acquire_lock(int *fd_out)
{
    char path[FAP_MAX_PATH], root[FAP_MAX_PATH];
    if (fap_root_path(NULL, root, sizeof(root)) < 0)
        return -1;
    if (fap_mkdir_p(root) < 0)
        return -1;
    if (fap_root_path(FAP_LOCKFILE, path, sizeof(path)) < 0)
        return -1;

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        return fap_error("lock: open %s: %s", path, strerror(errno));

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int saved_errno = errno;
        close(fd);
        if (saved_errno == EWOULDBLOCK)
            return fap_error("another fap process is already running (holding %s)", path);
        return fap_error("lock: flock %s: %s", path, strerror(saved_errno));
    }

    *fd_out = fd;
    return 0;
}

void fap_release_lock(int fd)
{
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}
