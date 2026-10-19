/**
 * @file pipe.h
 * @brief Pipes: a one-way byte stream between processes
 */

#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"

#define PIPE_BUFFER_SIZE 4096

typedef struct pipe {
    uint8_t buffer[PIPE_BUFFER_SIZE]; // Ring buffer
    size_t head;                      // Next byte to write
    size_t tail;                      // Next byte to read
    size_t count;                     // Bytes currently buffered
    bool reader_open;                 // The read end has not been closed
    bool writer_open;                 // The write end has not been closed
    spinlock_t lock;                  // Protects everything above
} pipe_t;

/**
 * @brief Create an empty pipe with both ends open (NULL if memory ran out).
 */
pipe_t *pipe_create(void);

/**
 * @brief Read up to `count` bytes into the kernel buffer `buf`: whatever is
 *        there, without waiting for more. Sleeps while the pipe is empty and
 *        the write end is open.
 * @return Bytes read; 0 means end-of-file (empty and the writer is gone);
 *         -EINTR if a signal arrived while it waited.
 */
int64_t pipe_read(pipe_t *pipe, void *buf, size_t count);

/**
 * @brief Write all `count` bytes from the kernel buffer `buf`, sleeping
 *        whenever the pipe is full.
 * @return Bytes written (fewer if the reader closed its end, or a signal
 *         arrived, part way); -EPIPE if the read end is closed and nothing
 *         could be written; -EINTR if a signal arrived before anything was.
 */
int64_t pipe_write(pipe_t *pipe, const void *buf, size_t count);

/**
 * @brief Close the read end. Wakes the writer (whose writes now fail with
 *        EPIPE); frees the pipe once both ends are closed.
 */
void pipe_close_reader(pipe_t *pipe);

/**
 * @brief Close the write end. Wakes the reader (who reads end-of-file once
 *        the pipe is empty); frees the pipe once both ends are closed.
 */
void pipe_close_writer(pipe_t *pipe);

#endif // PIPE_H
