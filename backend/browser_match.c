#include "browser_match.h"
#include "browser_scan.h"
#include <string.h>
_Static_assert(sizeof(((BrowserMatch *)0)->url) <= PROBE_MATCH_MAX, "Scanner overlap must cover encoded filenames");
static bool boundary(unsigned char c) {
    return c == 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '"' || c == '\'' || c == '<' || c == '>';
}
bool browser_match(void *context, const unsigned char *bytes, size_t length) {
    BrowserMatch *match = context;
    const char *prefix = "https://vikingfile.com/d/";
    const size_t plen = strlen(prefix);
    for (size_t step = 1; step <= 2; step++) {
        for (size_t i = 0; (plen + 1) * step <= length && i <= length - (plen + 1) * step; i++) {
            if (bytes[i] != 'h' || (i >= step && !boundary(bytes[i - step]))) continue;
            bool valid = true;
            for (size_t j = 0; j < plen; j++) {
                if (bytes[i + j * step] != (unsigned char)prefix[j] ||
                    (step == 2 && bytes[i + j * step + 1])) { valid = false; break; }
            }
            if (!valid) continue;
            char candidate[sizeof match->url];
            memcpy(candidate, prefix, plen);
            for (size_t j = plen; j < sizeof candidate && (j + 1) * step <= length - i; j++) {
                unsigned char c = bytes[i + j * step];
                if (step == 2 && bytes[i + j * step + 1]) break;
                if (boundary(c)) {
                    candidate[j] = 0;
                    if (provider_capture_url(match->release, candidate)) {
                        memcpy(match->url, candidate, j + 1);
                        return true;
                    }
                    if (c != '\'') break; /* Apostrophe may be inside an unescaped path. */
                }
                if (c < 33 || c > 126) break;
                candidate[j] = (char)c;
            }
        }
    }
    return false;
}
