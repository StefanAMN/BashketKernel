/**
 * @file pipe.c
 * @brief Implementation of Inter-Process Communication (IPC) Ring-Buffer Pipe
 */

#include "pipe.h"
#include "heap.h"
#include "string_util.h"

pipe_t *pipe_create(void) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return NULL;
    memset(p, 0, sizeof(pipe_t));
    return p;
}

size_t pipe_write(pipe_t *pipe, const void *buf, size_t count) {
    if (!pipe || !buf || count == 0) return 0;
    const uint8_t *src = (const uint8_t *)buf;

    size_t written = 0;
    while (written < count && pipe->count < PIPE_BUFFER_SIZE) {
        pipe->buffer[pipe->head] = src[written];
        pipe->head = (pipe->head + 1) % PIPE_BUFFER_SIZE;
        pipe->count++;
        written++;
    }

    return written;
}

size_t pipe_read(pipe_t *pipe, void *buf, size_t count) {
    if (!pipe || !buf || count == 0) return 0;
    uint8_t *dst = (uint8_t *)buf;

    size_t read_bytes = 0;
    while (read_bytes < count && pipe->count > 0) {
        dst[read_bytes] = pipe->buffer[pipe->tail];
        pipe->tail = (pipe->tail + 1) % PIPE_BUFFER_SIZE;
        pipe->count--;
        read_bytes++;
    }

    return read_bytes;
}

void pipe_destroy(pipe_t *pipe) {
    if (pipe) {
        kfree(pipe);
    }
}
