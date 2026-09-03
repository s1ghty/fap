#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <curl/curl.h>
#include <zstd.h>
#include "fap.h"

/*
 * package.c — download a package tarball, verify its SHA256, and
 * extract its zstd-compressed ustar payload into dest_path.
 *
 * No libarchive dependency (see CLAUDE.md's dependency list), so tar
 * extraction is a small hand-rolled ustar reader scoped to what fap's
 * own registry actually produces: regular files, directories, and
 * symlinks. GNU longname/pax extensions aren't supported.
 */

#define TAR_BLOCK 512

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag[1];
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

/* ponytail: 1 GiB heuristic cap on a decompressed package — this is a
 * CLI tool package manager, not a container registry; raise if a real
 * package ever needs more. */
#define MAX_DECOMPRESSED_SIZE (1UL << 30)

static size_t write_file_cb(char *ptr, size_t size, size_t nmemb, void *stream)
{
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

struct progress_ctx {
    const char *label;   /* e.g. "firefox 154.0" */
    int          tty;    /* only print to a real terminal — see download_to_file */
    double       last_print_time;
    int          printed_anything;
};

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Overwrites one line on stderr (curl's own convention for progress
 * meters) via \r, so it doesn't interleave with fap's normal stdout
 * output. curl calls this far more often than any human needs to see
 * it redraw — sometimes dozens of times before dltotal is even known
 * (still connecting) or after dlnow has already reached dltotal
 * (finishing up) — so this throttles to real time passing (~10
 * updates/sec) rather than trusting call frequency or percent deltas
 * to mean anything, and always lets the true final state (dlnow ==
 * dltotal) through regardless of the clock. */
static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;
    struct progress_ctx *ctx = clientp;
    if (!ctx->tty)
        return 0;

    int at_end = dltotal > 0 && dlnow >= dltotal;
    double now = monotonic_seconds();
    if (!at_end && ctx->printed_anything && now - ctx->last_print_time < 0.1)
        return 0;
    ctx->last_print_time = now;
    ctx->printed_anything = 1;

    if (dltotal <= 0) {
        /* server didn't report a size (e.g. no Content-Length) — show
         * bytes transferred instead of a percentage */
        fprintf(stderr, "\rdownloading %s... %.1f MB", ctx->label, dlnow / 1e6);
    } else {
        double pct = (double)dlnow * 100.0 / (double)dltotal;
        fprintf(stderr, "\rdownloading %s... %5.1f%%  (%.1f/%.1f MB)",
                ctx->label, pct, dlnow / 1e6, dltotal / 1e6);
    }
    return 0;
}

static int download_to_file(const char *url, const char *path, const char *progress_label)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return fap_error("package: open %s for write: %s", path, strerror(errno));

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(f);
        return fap_error("package: curl_easy_init failed");
    }

    struct progress_ctx ctx = { progress_label, isatty(STDERR_FILENO), 0.0, 0 };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fap/" FAP_VERSION);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    fclose(f);

    if (ctx.printed_anything)
        fprintf(stderr, "\n");

    if (res != CURLE_OK)
        return fap_error("package: download %s: %s", url, curl_easy_strerror(res));
    /* status is 0 for protocols without an HTTP-style status (e.g. file://) */
    if (status != 0 && status != 200)
        return fap_error("package: %s returned HTTP %ld", url, status);
    return 0;
}

/* Read a whole file into a malloc'd buffer. Caller frees *out. */
static int read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return fap_error("package: open %s: %s", path, strerror(errno));

    if (fseek(f, 0, SEEK_END) < 0) {
        fclose(f);
        return fap_error("package: cannot read %s", path);
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) < 0) {
        fclose(f);
        return fap_error("package: cannot read %s", path);
    }

    char *buf = malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        return fap_error("package: out of memory reading %s", path);
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return fap_error("package: short read on %s", path);
    }

    *out = buf;
    *out_len = n;
    return 0;
}

static int zstd_decompress(const char *src, size_t src_len, char **out, size_t *out_len)
{
    unsigned long long content_size = ZSTD_getFrameContentSize(src, src_len);
    if (content_size == ZSTD_CONTENTSIZE_ERROR || content_size == ZSTD_CONTENTSIZE_UNKNOWN)
        return fap_error("package: tarball is not a valid zstd frame with known size");
    if (content_size > MAX_DECOMPRESSED_SIZE)
        return fap_error("package: decompressed size %llu exceeds %lu byte limit",
                         content_size, MAX_DECOMPRESSED_SIZE);

    char *buf = malloc((size_t)content_size > 0 ? (size_t)content_size : 1);
    if (!buf)
        return fap_error("package: out of memory decompressing tarball");

    size_t r = ZSTD_decompress(buf, (size_t)content_size, src, src_len);
    if (ZSTD_isError(r)) {
        free(buf);
        return fap_error("package: zstd decompress failed: %s", ZSTD_getErrorName(r));
    }

    *out = buf;
    *out_len = r;
    return 0;
}

static unsigned long parse_octal(const char *field, size_t len)
{
    char buf[13];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, field, n);
    buf[n] = '\0';
    return strtoul(buf, NULL, 8);
}

/* Reject absolute paths and ".." components, so a malicious or
 * corrupt tarball can't write outside dest_path. */
static int path_is_safe(const char *name)
{
    if (name[0] == '\0' || name[0] == '/')
        return 0;
    const char *p = name;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        if (p - seg == 2 && seg[0] == '.' && seg[1] == '.')
            return 0;
        if (*p == '/')
            p++;
    }
    return 1;
}

static int build_entry_path(const struct tar_header *hdr, const char *dest_path,
                             char *out, size_t outsz)
{
    char name[257];
    int n;
    if (hdr->prefix[0] != '\0')
        n = snprintf(name, sizeof(name), "%.155s/%.100s", hdr->prefix, hdr->name);
    else
        n = snprintf(name, sizeof(name), "%.100s", hdr->name);
    if (n < 0 || (size_t)n >= sizeof(name))
        return fap_error("package: tar entry name too long");

    /* directories carry a trailing '/' in ustar; harmless for our checks */
    if (!path_is_safe(name))
        return fap_error("package: tar entry \"%s\" escapes package root", name);

    n = snprintf(out, outsz, "%s/%s", dest_path, name);
    if (n < 0 || (size_t)n >= outsz)
        return fap_error("package: extracted path too long: %s/%s", dest_path, name);
    return 0;
}

static int extract_dir(const char *out_path)
{
    return fap_mkdir_p(out_path);
}

static int extract_regular_file(const char *out_path, unsigned long mode,
                                 const char *data, size_t size)
{
    char parent[FAP_MAX_PATH];
    int n = snprintf(parent, sizeof(parent), "%s", out_path);
    if (n < 0 || (size_t)n >= sizeof(parent))
        return fap_error("package: path too long: %s", out_path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        if (fap_mkdir_p(parent) < 0)
            return -1;
    }

    FILE *f = fopen(out_path, "wb");
    if (!f)
        return fap_error("package: open %s for write: %s", out_path, strerror(errno));
    size_t written = size > 0 ? fwrite(data, 1, size, f) : 0;
    fclose(f);
    if (written != size)
        return fap_error("package: short write extracting %s", out_path);

    if (chmod(out_path, (mode_t)(mode & 07777)) < 0)
        return fap_error("package: chmod %s: %s", out_path, strerror(errno));
    return 0;
}

static int extract_symlink(const char *out_path, const char *linkname)
{
    /* Same traversal check as the entry name itself (path_is_safe) —
     * without it, a crafted symlink could point outside dest_path, and
     * a later archive entry written "through" it (e.g. a regular file
     * whose own name is safe but resolves via this symlink) would
     * escape the package root just as surely as a ".." entry name
     * would. This is the classic tar-symlink-traversal class of bug. */
    if (!path_is_safe(linkname))
        return fap_error("package: symlink target \"%s\" escapes package root", linkname);

    char parent[FAP_MAX_PATH];
    int n = snprintf(parent, sizeof(parent), "%s", out_path);
    if (n < 0 || (size_t)n >= sizeof(parent))
        return fap_error("package: path too long: %s", out_path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        if (fap_mkdir_p(parent) < 0)
            return -1;
    }
    return fap_symlink_force(linkname, out_path);
}

static int extract_tar(const char *data, size_t len, const char *dest_path)
{
    size_t off = 0;
    while (off + TAR_BLOCK <= len) {
        const struct tar_header *hdr = (const struct tar_header *)(data + off);
        if (hdr->name[0] == '\0')
            break; /* end-of-archive marker */

        if ((unsigned char)hdr->size[0] & 0x80)
            return fap_error("package: GNU base-256 tar size format not supported");

        unsigned long mode = parse_octal(hdr->mode, sizeof(hdr->mode));
        size_t size = (size_t)parse_octal(hdr->size, sizeof(hdr->size));
        size_t data_blocks = (size + TAR_BLOCK - 1) / TAR_BLOCK;
        off += TAR_BLOCK;

        if (off + data_blocks * TAR_BLOCK > len)
            return fap_error("package: corrupt tarball: entry \"%.100s\" overruns archive", hdr->name);

        char type = hdr->typeflag[0];
        if (type == 'x' || type == 'g') {
            /* PAX (extended/global) header: metadata we don't need */
        } else {
            char out_path[FAP_MAX_PATH];
            if (build_entry_path(hdr, dest_path, out_path, sizeof(out_path)) < 0)
                return -1;

            switch (type) {
            case '5':
                if (extract_dir(out_path) < 0)
                    return -1;
                break;
            case '0':
            case '\0':
                if (extract_regular_file(out_path, mode, data + off, size) < 0)
                    return -1;
                break;
            case '2': {
                char linkname[101];
                snprintf(linkname, sizeof(linkname), "%.100s", hdr->linkname);
                if (extract_symlink(out_path, linkname) < 0)
                    return -1;
                break;
            }
            default:
                return fap_error("package: unsupported tar entry type '%c' for \"%s\"", type, out_path);
            }
        }

        off += data_blocks * TAR_BLOCK;
    }
    return 0;
}

/* Decompress a zstd-compressed ustar tarball already in memory and
 * extract it into dest_path. Split out from fap_package_download so
 * this (the actual parsing logic) is testable without a network
 * fetch, mirroring fap_index_parse() in registry.c. */
int fap_package_extract(const char *compressed, size_t compressed_len, const char *dest_path)
{
    if (fap_mkdir_p(dest_path) < 0)
        return -1;

    char *decompressed = NULL;
    size_t decompressed_len = 0;
    int r = zstd_decompress(compressed, compressed_len, &decompressed, &decompressed_len);
    if (r < 0)
        return -1;

    r = extract_tar(decompressed, decompressed_len, dest_path);
    free(decompressed);
    return r;
}

int fap_package_download(const FapPackage *pkg, const char *dest_path)
{
    if (fap_mkdir_p(dest_path) < 0)
        return -1;

    char tmp[FAP_MAX_PATH];
    int n = snprintf(tmp, sizeof(tmp), "%s/.fap-download.tar.zst", dest_path);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return fap_error("package: path too long: %s", dest_path);

    char label[FAP_MAX_NAME + FAP_MAX_VERSION + 2];
    snprintf(label, sizeof(label), "%s %s", pkg->name, pkg->version);
    if (download_to_file(pkg->url, tmp, label) < 0)
        return -1;

    if (fap_sha256_verify(tmp, pkg->sha256) < 0) {
        unlink(tmp);
        return -1;
    }

    char *compressed = NULL;
    size_t compressed_len = 0;
    int r = read_file(tmp, &compressed, &compressed_len);
    unlink(tmp);
    if (r < 0)
        return -1;

    r = fap_package_extract(compressed, compressed_len, dest_path);
    free(compressed);
    return r;
}
