#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zstd.h>
#include "fap.h"

/* fap_sha256_verify lives in hash.c, which this test doesn't link
 * (it only exercises fap_package_extract, the network/hash-free half
 * of package.c — hashing itself is covered by test_hash.c). Provide
 * a definition so fap_package_download, which is still in the same
 * translation unit as fap_package_extract, links cleanly even though
 * this test never calls it. */
int fap_sha256_verify(const char *path, const char *expected_hex)
{
    (void)path; (void)expected_hex;
    return 0;
}

/* declared in package.c, not part of the public fap.h API — exposed
 * so tar/zstd extraction can be tested without a network fetch */
int fap_package_extract(const char *compressed, size_t compressed_len, const char *dest_path);

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

/* Appends one ustar entry (header + padded data) to buf at *off, and
 * advances *off past it. buf must be large enough. */
static void add_entry(unsigned char *buf, size_t *off, char type, const char *name,
                       const char *linkname, unsigned long mode,
                       const char *data, size_t size)
{
    unsigned char *hdr = buf + *off;
    memset(hdr, 0, BLK);
    put_field(hdr, 0, 100, name);
    put_octal(hdr, 100, 8, mode);
    put_octal(hdr, 124, 12, size);
    hdr[156] = type;
    if (linkname)
        put_field(hdr, 157, 100, linkname);
    memcpy(hdr + 257, "ustar", 5);
    *off += BLK;
    if (size > 0) {
        memcpy(buf + *off, data, size);
        *off += ((size + BLK - 1) / BLK) * BLK;
    }
}

/* Builds a small ustar archive (dir, regular file, symlink), zstd
 * compresses it, and returns the compressed buffer (caller frees). */
static char *build_sample_package(size_t *out_len)
{
    static const char *tool_src = "#!/bin/sh\necho hi\n";
    static const char *note_src = "hello\n";

    unsigned char tar[8 * BLK];
    size_t off = 0;
    add_entry(tar, &off, '5', "share/", NULL, 0755, NULL, 0);
    add_entry(tar, &off, '0', "bin/tool", NULL, 0755, tool_src, strlen(tool_src));
    add_entry(tar, &off, '0', "share/note.txt", NULL, 0644, note_src, strlen(note_src));
    add_entry(tar, &off, '2', "bin/tool-link", "tool", 0777, NULL, 0);

    size_t bound = ZSTD_compressBound(off);
    char *compressed = malloc(bound);
    size_t clen = ZSTD_compress(compressed, bound, tar, off, 3);
    if (ZSTD_isError(clen)) {
        free(compressed);
        return NULL;
    }
    *out_len = clen;
    return compressed;
}

static int file_mode(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    return st.st_mode & 0777;
}

static void test_extract_sample(void)
{
    const char *dest = "/tmp/fap_test_pkg_extract";
    system("rm -rf /tmp/fap_test_pkg_extract");

    size_t clen;
    char *compressed = build_sample_package(&clen);
    CHECK(compressed != NULL, "sample package compresses");

    CHECK(fap_package_extract(compressed, clen, dest) == 0, "extract succeeds");

    char path[512];
    snprintf(path, sizeof(path), "%s/bin/tool", dest);
    FILE *f = fopen(path, "rb");
    char buf[128] = {0};
    if (f) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    CHECK(f != NULL, "extracted bin/tool exists");
    CHECK(strcmp(buf, "#!/bin/sh\necho hi\n") == 0, "extracted file content matches");
    CHECK(file_mode(path) == 0755, "extracted file preserves mode 0755");

    snprintf(path, sizeof(path), "%s/share/note.txt", dest);
    CHECK(file_mode(path) == 0644, "second file preserves mode 0644");

    snprintf(path, sizeof(path), "%s/share", dest);
    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISDIR(st.st_mode), "directory entry created");

    snprintf(path, sizeof(path), "%s/bin/tool-link", dest);
    char linktarget[64] = {0};
    ssize_t n = readlink(path, linktarget, sizeof(linktarget) - 1);
    CHECK(n > 0 && strcmp(linktarget, "tool") == 0, "symlink entry created with correct target");

    free(compressed);
}

static void test_rejects_path_traversal(void)
{
    const char *dest = "/tmp/fap_test_pkg_traversal";
    system("rm -rf /tmp/fap_test_pkg_traversal");

    unsigned char tar[4 * BLK];
    size_t off = 0;
    add_entry(tar, &off, '0', "../escaped.txt", NULL, 0644, "evil", 4);

    size_t bound = ZSTD_compressBound(off);
    char *compressed = malloc(bound);
    size_t clen = ZSTD_compress(compressed, bound, tar, off, 3);

    int r = fap_package_extract(compressed, clen, dest);
    CHECK(r < 0, "extract rejects \"..\" path traversal");
    CHECK(access("/tmp/fap_test_pkg_traversal/../escaped.txt", 0) != 0 &&
          access("/tmp/escaped.txt", 0) != 0,
          "no file written outside dest_path");

    free(compressed);
}

static void test_rejects_garbage_zstd(void)
{
    const char *dest = "/tmp/fap_test_pkg_garbage";
    const char garbage[] = "not a zstd frame at all";
    int r = fap_package_extract(garbage, sizeof(garbage), dest);
    CHECK(r < 0, "extract rejects non-zstd data");
}

static void test_rejects_truncated_tar(void)
{
    const char *dest = "/tmp/fap_test_pkg_truncated";

    unsigned char tar[4 * BLK];
    size_t off = 0;
    /* header claims 4096 bytes of content but we only provide one block */
    add_entry(tar, &off, '0', "bin/tool", NULL, 0755, "x", 1);
    /* corrupt the size field to claim far more data than exists */
    put_octal(tar, 124, 12, 1UL << 20);

    size_t bound = ZSTD_compressBound(off);
    char *compressed = malloc(bound);
    size_t clen = ZSTD_compress(compressed, bound, tar, off, 3);

    int r = fap_package_extract(compressed, clen, dest);
    CHECK(r < 0, "extract rejects a tar entry that overruns the archive");

    free(compressed);
}

int main(void)
{
    printf("package:\n");
    test_extract_sample();
    test_rejects_path_traversal();
    test_rejects_garbage_zstd();
    test_rejects_truncated_tar();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
