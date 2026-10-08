#ifndef ORBIT_BROWSER_MATCH_H
#define ORBIT_BROWSER_MATCH_H
#include "orbit.h"
typedef struct {
    const Release *release;
    char url[3 * sizeof(((Release *)0)->filename) + 64];
} BrowserMatch;
/* Extract only a complete, filename-bound Vikingfile route. Never retain bytes
 * from other pages, credentials, cookies or storage signatures. */
bool browser_match(void *context, const unsigned char *bytes, size_t length);
#endif
