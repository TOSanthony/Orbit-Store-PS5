#include "catalog_crypto.h"
#include "catalog_key.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

/* magic[8], format=1, key-id=1, purpose, reserved=0, nonce[12], ciphertext,
 * tag[16]. The entire 24-byte header is authenticated as additional data. */
static const unsigned char prefix[10] = {'O','R','B','I','T','E','N','C',1,1};
enum { HEADER = 24, TAG = 16 };

static int valid_purpose(unsigned purpose) {
    return purpose >= CATALOG_PURPOSE_FEED && purpose <= CATALOG_PURPOSE_RELEASES;
}

int catalog_seal(const void *data, size_t size, unsigned purpose,
                 unsigned char **out, size_t *length) {
    *out = NULL;
    *length = 0;
    if (!data || !size || size > CATALOG_SEAL_LIMIT || !valid_purpose(purpose)) return -1;
    unsigned char *buf = malloc(size + CATALOG_SEAL_OVERHEAD);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, final = 0, ignored = 0, ok = 0;
    if (!buf || !ctx) goto done;
    memcpy(buf, prefix, sizeof prefix);
    buf[10] = (unsigned char)purpose;
    buf[11] = 0;
    if (RAND_bytes(buf + 12, 12) != 1 ||
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, catalog_key, buf + 12) != 1 ||
        EVP_EncryptUpdate(ctx, NULL, &ignored, buf, HEADER) != 1 ||
        EVP_EncryptUpdate(ctx, buf + HEADER, &n, data, (int)size) != 1 ||
        EVP_EncryptFinal_ex(ctx, buf + HEADER + n, &final) != 1 ||
        (size_t)(n + final) != size ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG, buf + HEADER + size) != 1)
        goto done;
    *out = buf;
    *length = size + CATALOG_SEAL_OVERHEAD;
    ok = 1;
done:
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) free(buf);
    return ok ? 0 : -1;
}

int catalog_open(const void *data, size_t size, unsigned purpose,
                 unsigned char **out, size_t *length) {
    *out = NULL;
    *length = 0;
    const unsigned char *buf = data;
    if (!buf || size <= CATALOG_SEAL_OVERHEAD ||
        size > CATALOG_SEAL_LIMIT + CATALOG_SEAL_OVERHEAD || !valid_purpose(purpose) ||
        memcmp(buf, prefix, sizeof prefix) || buf[10] != purpose || buf[11]) return -1;
    size_t plain_size = size - CATALOG_SEAL_OVERHEAD;
    unsigned char *plain = malloc(plain_size + 1);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, final = 0, ignored = 0, ok = 0;
    if (!plain || !ctx) goto done;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, catalog_key, buf + 12) != 1 ||
        EVP_DecryptUpdate(ctx, NULL, &ignored, buf, HEADER) != 1 ||
        EVP_DecryptUpdate(ctx, plain, &n, buf + HEADER, (int)plain_size) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG, (void *)(buf + HEADER + plain_size)) != 1 ||
        EVP_DecryptFinal_ex(ctx, plain + n, &final) != 1 || (size_t)(n + final) != plain_size)
        goto done;
    plain[plain_size] = 0;
    *out = plain;
    *length = plain_size;
    ok = 1;
done:
    EVP_CIPHER_CTX_free(ctx);
    if (!ok && plain) {
        OPENSSL_cleanse(plain, plain_size + 1);
        free(plain);
    }
    return ok ? 0 : -1;
}
