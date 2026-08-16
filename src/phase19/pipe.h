/**
 * @file pipe.h
 * @brief Inter-Process Communication (IPC) Ring-Buffer Pipe Interface
 */

#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PIPE_BUFFER_SIZE 4096

typedef struct {
    uint8_t buffer[PIPE_BUFFER_SIZE];
    size_t head;
    size_t tail;
    size_t count;
} pipe_t;

/**
 * @brief Allocate and initialize a new IPC ring-buffer pipe.
 * @return Pointer to newly allocated pipe structure.
 */
pipe_t *pipe_create(void);

/**
 * @brief Read bytes from an IPC pipe.
 * @param pipe Pointer to target pipe.
 * @param buf Output destination buffer.
 * @param count Maximum number of bytes to read.
 * @return Number of bytes successfully read.
 */
size_t pipe_read(pipe_t *pipe, void *buf, size_t count);

/**
 * @brief Write bytes to an IPC pipe.
 * @param pipe Pointer to target pipe.
 * @param buf Source data buffer.
 * @param count Number of bytes to write.
 * @return Number of bytes successfully written.
 */
size_t pipe_write(pipe_t *pipe, const void *buf, size_t count);

/**
 * @brief Free pipe resource allocations.
 * @param pipe Pipe to destroy.
 */
void pipe_destroy(pipe_t *pipe);

#endif // PIPE_H
