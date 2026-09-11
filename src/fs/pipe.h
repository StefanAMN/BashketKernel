/**
 * @file pipe.h
 * @brief Pipes: a one-way byte stream between processes
 */

#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PIPE_BUFFER_SIZE 4096

typedef struct pipe {
    uint8_t buffer[PIPE_BUFFER_SIZE]; // Ring buffer
    size_t head;                      // Next byte to write
    size_t tail;                      // Next byte to read
    size_t count;                     // Bytes currently buffered
    bool reader_open;                 // The read end has not been closed
    bool writer_open;                 // The write end has not been closed
} pipe_t;

/**
 * @brief Create a pipe with both ends open.
 */
pipe_t *pipe_create(void);

/**
 * @brief Read up to `count` bytes. Sleeps while the pipe is empty and the
 *        write end is open.
 * @return Bytes read; 0 means end-of-file (empty and the writer is gone).
 */
int64_t pipe_read(pipe_t *pipe, void *buf, size_t count);

/**
 * @brief Write all `count` bytes, sleeping whenever the pipe is full.
 * @return Bytes written, or -EPIPE if the read end is closed and nothing
 *         could be written.
 */
int64_t pipe_write(pipe_t *pipe, const void *buf, size_t count);

/**
 * @brief Close one end. Wakes the other side; frees the pipe once both ends
 *        are closed.
 */
void pipe_close_reader(pipe_t *pipe);
void pipe_close_writer(pipe_t *pipe);

#endif // PIPE_H
