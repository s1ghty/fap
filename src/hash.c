#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include "fap.h"

#define CHUNK 65536

int fap_sha256_file(const char *path, char hex_out[FAP_SHA256_HEX])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return fap_error("cannot open %s for hashing", path);

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { fclose(f); return fap_error("EVP_MD_CTX_new failed"); }
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);

    unsigned char buf[CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        EVP_DigestUpdate(ctx, buf, n);

    fclose(f);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    for (unsigned int i = 0; i < digest_len; i++)
        snprintf(hex_out + i * 2, 3, "%02x", digest[i]);
    hex_out[digest_len * 2] = '\0';

    return 0;
}

int fap_sha256_verify(const char *path, const char *expected_hex)
{
    char actual[FAP_SHA256_HEX];
    if (fap_sha256_file(path, actual) < 0)
        return -1;
    if (strcmp(actual, expected_hex) != 0)
        return fap_error("SHA256 mismatch for %s\n  got:      %s\n  expected: %s",
                         path, actual, expected_hex);
    return 0;
}
