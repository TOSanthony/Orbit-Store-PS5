#include "orbit.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>
static bool viking_file(const Release *r) {
    if (strcmp(r->source, "vikingfile") || strlen(r->title_id) != 9 ||
        strncmp(r->title_id, "PPSA", 4) || !r->filename[0] ||
        strchr(r->filename, '/') || strchr(r->filename, '\\') || strstr(r->filename, ".."))
        return false;
    for (size_t i = 4; i < 9; i++)
        if (!isdigit((unsigned char)r->title_id[i])) return false;
    for (const unsigned char *p = (const void *)r->filename; *p; p++)
        if (*p < 32 || *p == 127) return false;
    const char *suffix = !strcmp(r->format, "exFAT") ? ".exfat" :
                         !strcmp(r->format, "FFPFSC") ? ".ffpfsc" :
                         !strcmp(r->format, "FPKG") ? ".pkg" : NULL;
    size_t n = strlen(r->filename);
    if (!suffix || n <= strlen(suffix) || strcasecmp(r->filename + n - strlen(suffix), suffix))
        return false;
    /* A source may use a descriptive filename without its title ID. If an ID
     * is present it must agree with the catalogue; matching remains exact. */
    for (const char *p = strstr(r->filename, "PPSA"); p; p = strstr(p + 4, "PPSA"))
        if (strncmp(p, r->title_id, 9) || isdigit((unsigned char)p[9])) return false;
    return true;
}
static bool browser_page(const char *url) {
    const char *prefixes[] = {"https://vik1ngfile.site/f/", "https://vikingfile.com/f/"};
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(url, prefixes[i], n) || strlen(url + n) != 10) continue;
        for (const char *p = url + n; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9'))) return false;
        return true;
    }
    return false;
}
static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Provider boundary: never turn metadata/page URLs into unverified downloads. */
bool provider_url_supported(const Release *release) {
    if (!strcmp(release->id, "orbit-network-check"))
        return !strcmp(release->url,
                       "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/LICENSE");
    if (!strcmp(release->source, "archive"))
        return !strncmp(release->url, "https://archive.org/download/", 29) &&
               strlen(release->url) > 29 &&
               (!strcmp(release->format, "FFPFSC") || !strcmp(release->format, "exFAT") ||
                !strcmp(release->format, "FPKG"));
    /* Only exact filename-bound direct routes may enter the download queue.
     * Landing pages are a separate browser capability, never a transfer URL. */
    const char *prefix = "https://vikingfile.com/d/";
    if (!viking_file(release) || strncmp(release->url, prefix, strlen(prefix)))
        return false;
    const char *path = release->url + strlen(prefix);
    if (strlen(path) < 12 || path[10] != '/')
        return false;
    for (size_t i = 0; i < 10; i++)
        if (!isalnum((unsigned char)path[i])) return false;
    const unsigned char *expected = (const void *)release->filename;
    for (const unsigned char *p = (const void *)(path + 11); *p; p++) {
        unsigned char c = *p;
        if (c == '%') {
            if (!p[1] || !p[2] || hex(p[1]) < 0 || hex(p[2]) < 0) return false;
            c = (unsigned char)(hex(p[1]) * 16 + hex(p[2])); p += 2;
        } else if (c <= 32 || c >= 127 || c == '?' || c == '#' || c == '/' || c == '\\')
            return false;
        if (!*expected || c != *expected++) return false;
    }
    return !*expected;
}

bool provider_supported(const Release *release) {
#ifdef ORBIT_TEST
    /* Test binaries may parse the catalogue, but may only transfer fixtures. */
    return !strncmp(release->url, "http://127.0.0.1:", 17) &&
           (!strcmp(release->source, "archive") || !strcmp(release->source, "vikingfile"));
#else
    return provider_url_supported(release);
#endif
}

bool provider_browser_supported(const Release *release) {
    if (!release || strcmp(release->source, "vikingfile")) return false;
#ifdef ORBIT_TEST
    if (!strncmp(release->browser_url, "http://127.0.0.1:", 17)) return true;
#endif
    return viking_file(release) && browser_page(release->browser_url) &&
        (!strcmp(release->url, release->browser_url) || provider_url_supported(release));
}

bool provider_capture_url(const Release *release, const char *candidate) {
    if (!candidate || strlen(candidate) >= sizeof release->url) return false;
    Release resolved = *release;
    strcpy(resolved.url, candidate);
    return !strcmp(release->source, "vikingfile") && provider_supported(&resolved);
}
