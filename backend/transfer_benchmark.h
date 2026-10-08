#ifndef ORBIT_TRANSFER_BENCHMARK_H
#define ORBIT_TRANSFER_BENCHMARK_H
#include "orbit.h"
#ifdef ORBIT_BENCHMARK
cJSON *transfer_benchmark(Job *job, int fd, unsigned connections, int64_t bytes,
                         unsigned duration, long socket_buffer, long curl_buffer);
#endif
#endif
