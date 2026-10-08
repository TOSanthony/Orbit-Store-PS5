#ifndef ORBIT_TRANSFER_WRITER_H
#define ORBIT_TRANSFER_WRITER_H
#include "orbit.h"

#define TRANSFER_BUFFER_BYTES (8U * 1024 * 1024)
#define TRANSFER_WRITE_BYTES (256U * 1024)
typedef struct TransferWriter TransferWriter;
/* Owns neither fd nor job. The caller must join before closing/reusing either. */
TransferWriter *transfer_writer_start(Job *job, int fd, double started, char *error, size_t cap);
bool transfer_writer_failed(const TransferWriter *writer);
int transfer_writer_enqueue(TransferWriter *writer, unsigned range, const void *data, size_t size);
/* Stops accepting input, drains accepted bytes and saves a final durable checkpoint. */
int transfer_writer_finish(TransferWriter *writer, char *error, size_t cap);
#endif
