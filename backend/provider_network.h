#ifndef ORBIT_PROVIDER_NETWORK_H
#define ORBIT_PROVIDER_NETWORK_H
#include <stdbool.h>
#include <curl/curl.h>
bool provider_transfer_url_allowed(const char *url);
curl_socket_t provider_open_socket(void *context, curlsocktype purpose, struct curl_sockaddr *address);
#endif
