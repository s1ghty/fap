#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>
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
