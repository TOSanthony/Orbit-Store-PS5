#ifndef ORBIT_CATALOG_CRYPTO_H
#define ORBIT_CATALOG_CRYPTO_H
#include <stddef.h>

/* Scraping resistance only: the key is distributed with the application and
 * corresponding source. This is not a signing key or an access-control boundary. */
#define CATALOG_SEAL_OVERHEAD 40u
#define CATALOG_SEAL_LIMIT (8u * 1024u * 1024u)
#define CATALOG_PURPOSE_FEED 1u
#define CATALOG_PURPOSE_GAMES 2u
#define CATALOG_PURPOSE_RELEASES 3u

/* Allocated results belong to the caller. Open authenticates before returning
 * plaintext and appends a NUL outside its reported length for JSON parsing. */
int catalog_seal(const void *data, size_t size, unsigned purpose,
                 unsigned char **out, size_t *length);
int catalog_open(const void *data, size_t size, unsigned purpose,
                 unsigned char **out, size_t *length);
#endif
