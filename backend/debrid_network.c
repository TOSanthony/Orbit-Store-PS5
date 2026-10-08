/* TorBox download URL policy, shared by resolution and redirect checks. */
#include "debrid.h"
#include "provider_network.h"
#include <string.h>
#include <strings.h>

#ifndef ORBIT_TEST
static bool host_suffix(const char *host, const char *suffix) {
    size_t a = strlen(host), b = strlen(suffix);
    return a > b && !strcasecmp(host + a - b, suffix);
}
static bool download_host(const char *host) {
    /* TorBox's public /v1/api/speedtest lists these Hyperdrive domains.
     * Match a complete DNS suffix, never arbitrary tb-cdn.* hosts or CDN
     * providers' shared domains. Retain the older storage addresses too. */
    static const char *suffixes[] = {
        ".tb-cdn.pw", ".tb-cdn.to", ".tb-cdn.earth",
        ".tb-cdn.st", ".tb-cdn.io", ".tb-cdn.cx"
    };
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++)
        if (host_suffix(host, suffixes[i])) return true;
    return host_suffix(host, ".torbox.app") &&
        (!strcasecmp(host, "storage.torbox.app") || !strncasecmp(host, "storage-", 8));
}
#endif

bool debrid_transfer_url_allowed(const char *url) {
#ifdef ORBIT_TEST
    return provider_transfer_url_allowed(url);
#else
    CURLU *u = curl_url(); if (!u || !url || strlen(url) > 8192) { curl_url_cleanup(u); return false; }
    char *scheme = NULL, *host = NULL, *port = NULL, *user = NULL, *password = NULL, *fragment = NULL;
    bool ok = !curl_url_set(u, CURLUPART_URL, url, 0) && !curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) && !curl_url_get(u, CURLUPART_HOST, &host, 0);
    curl_url_get(u, CURLUPART_PORT, &port, 0); curl_url_get(u, CURLUPART_USER, &user, 0); curl_url_get(u, CURLUPART_PASSWORD, &password, 0); curl_url_get(u, CURLUPART_FRAGMENT, &fragment, 0);
    ok = ok && !strcmp(scheme, "https") && (!port || !strcmp(port, "443")) && !user && !password && !fragment &&
         download_host(host);
    curl_free(scheme); curl_free(host); curl_free(port); curl_free(user); curl_free(password); curl_free(fragment); curl_url_cleanup(u); return ok;
#endif
}
