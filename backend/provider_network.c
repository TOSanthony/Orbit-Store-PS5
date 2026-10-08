#include "provider_network.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#ifndef ORBIT_TEST
static bool suffix(const char *host, const char *ending) {
    size_t a = strlen(host), b = strlen(ending);
    return a > b && !strcmp(host + a - b, ending);
}
#endif
bool provider_transfer_url_allowed(const char *url) {
    if (!url || strlen(url) > 8192) return false;
    CURLU *parsed = curl_url();
    if (!parsed) return false;
    char *host = NULL, *scheme = NULL, *port = NULL, *user = NULL, *password = NULL, *fragment = NULL;
    bool ok = !curl_url_set(parsed, CURLUPART_URL, url, 0) &&
              !curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) &&
              !curl_url_get(parsed, CURLUPART_HOST, &host, 0);
    curl_url_get(parsed, CURLUPART_PORT, &port, 0);
    curl_url_get(parsed, CURLUPART_USER, &user, 0);
    curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0);
    curl_url_get(parsed, CURLUPART_FRAGMENT, &fragment, 0);
    ok = ok && !user && !password && !fragment;
#ifdef ORBIT_TEST
    ok = ok && !strcmp(scheme, "http") && !strcmp(host, "127.0.0.1");
#else
    ok = ok && !strcmp(scheme, "https") && (!port || !strcmp(port, "443")) &&
        (!strcmp(host, "vikingfile.com") || !strcmp(host, "ar.vikingfile.com") ||
         suffix(host, ".s3.bhs.io.cloud.ovh.net") || suffix(host, ".r2.cloudflarestorage.com"));
#endif
    curl_free(host); curl_free(scheme); curl_free(port); curl_free(user);
    curl_free(password); curl_free(fragment); curl_url_cleanup(parsed);
    return ok;
}

curl_socket_t provider_open_socket(void *context, curlsocktype purpose, struct curl_sockaddr *address) {
    (void)context;
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    bool ok = false;
    if (address->family == AF_INET && address->addrlen >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)&address->addr;
        uint32_t ip = ntohl(a->sin_addr.s_addr);
#ifdef ORBIT_TEST
        ok = ip == 0x7f000001U;
#else
        ok = (ip >> 24) != 0 && (ip >> 24) != 10 && (ip >> 24) != 127 &&
             (ip >> 24) < 224 && (ip >> 22) != (0x64400000U >> 22) &&
             (ip >> 16) != 0xa9fe && (ip >> 20) != (0xac100000U >> 20) &&
             (ip >> 16) != 0xc0a8 && (ip >> 8) != 0xc00000 && (ip >> 8) != 0xc00002 &&
             (ip >> 8) != 0xc05863 && (ip >> 15) != (0xc6120000U >> 15) &&
             (ip >> 8) != 0xc63364 && (ip >> 8) != 0xcb0071;
#endif
    }
#ifndef ORBIT_TEST
    if (address->family == AF_INET6 && address->addrlen >= sizeof(struct sockaddr_in6)) {
        const unsigned char *ip = ((const struct sockaddr_in6 *)&address->addr)->sin6_addr.s6_addr;
        ok = (ip[0] & 0xe0) == 0x20 && !(ip[0] == 0x20 && ip[1] == 2) &&
             !(ip[0] == 0x20 && ip[1] == 1 && ip[2] == 0 && ip[3] == 0) &&
             !(ip[0] == 0x20 && ip[1] == 1 && ip[2] == 0x0d && ip[3] == 0xb8);
    }
#endif
    return ok ? socket(address->family, address->socktype, address->protocol) : CURL_SOCKET_BAD;
}
