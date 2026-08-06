#include <stdio.h>
#include <string.h>
#include "fap.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); pass++; } \
    else       { printf("  FAIL  %s  [%s]\n", msg, fap_err); fail++; } \
} while(0)

static void test_sha256_known(void)
{
    /* SHA256 of the empty string is well-known */
    const char *tmpfile = "/tmp/fap_test_empty";
    FILE *f = fopen(tmpfile, "wb");
    if (!f) { printf("  SKIP  cannot write tmpfile\n"); return; }
    fclose(f);

    char hex[FAP_SHA256_HEX];
    int r = fap_sha256_file(tmpfile, hex);
    CHECK(r == 0, "sha256 of empty file returns 0");
    CHECK(strcmp(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
        "sha256 of empty file is correct");
}

static void test_sha256_verify_ok(void)
{
    const char *tmpfile = "/tmp/fap_test_hello";
    FILE *f = fopen(tmpfile, "wb");
    if (!f) { printf("  SKIP  cannot write tmpfile\n"); return; }
    fputs("hello\n", f);
    fclose(f);

    /* sha256("hello\n") */
    int r = fap_sha256_verify(tmpfile,
        "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03");
    CHECK(r == 0, "sha256_verify passes on correct hash");
}

static void test_sha256_verify_fail(void)
{
    const char *tmpfile = "/tmp/fap_test_hello";
    int r = fap_sha256_verify(tmpfile, "deadbeef");
    CHECK(r < 0, "sha256_verify fails on wrong hash");
}

static void test_util_channel(void)
{
    char buf[32];
    FapChannel ch;

    fap_channel_str(FAP_CHANNEL_STABLE, buf, sizeof(buf));
    CHECK(strcmp(buf, "stable") == 0, "channel_str stable");

    fap_channel_str(FAP_CHANNEL_EDGE, buf, sizeof(buf));
    CHECK(strcmp(buf, "edge") == 0, "channel_str edge");

    CHECK(fap_channel_parse("stable", &ch) == 0 && ch == FAP_CHANNEL_STABLE,
          "channel_parse stable");
    CHECK(fap_channel_parse("edge", &ch) == 0 && ch == FAP_CHANNEL_EDGE,
          "channel_parse edge");
    CHECK(fap_channel_parse("bogus", &ch) < 0,
          "channel_parse bogus returns error");
}

int main(void)
{
    printf("fap test suite\n\n");

    printf("hash:\n");
    test_sha256_known();
    test_sha256_verify_ok();
    test_sha256_verify_fail();

    printf("\nutil:\n");
    test_util_channel();

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
