/**
 * @file string.h
 * @brief The C library's memory and string functions, freestanding
 *
 * The kernel has no C library; these are the parts it uses, with the
 * standard's meaning (the compiler may also call memset, memcpy, memmove and
 * memcmp on its own, for structure copies and the like). Kernel code
 * includes "kstring.h".
 */

#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Set `count` bytes at `dest` to `val` (as a byte). Returns `dest`.
 */
void *memset(void *dest, int val, size_t count);

/**
 * @brief Copy `count` bytes from `src` to `dest`, which must not overlap.
 *        Returns `dest`.
 */
void *memcpy(void *dest, const void *src, size_t count);

/**
 * @brief Copy `count` bytes from `src` to `dest`, which may overlap.
 *        Returns `dest`.
 */
void *memmove(void *dest, const void *src, size_t count);

/**
 * @brief Compare `count` bytes: 0 if equal, else the difference of the
 *        first unequal pair (as unsigned bytes).
 */
int memcmp(const void *s1, const void *s2, size_t count);

/**
 * @brief Length of a NUL-terminated string (0 for NULL).
 */
size_t strlen(const char *str);

/**
 * @brief Compare two strings: 0 if equal, negative or positive as `s1`
 *        sorts before or after `s2` (by unsigned bytes).
 */
int strcmp(const char *s1, const char *s2);

/**
 * @brief strcmp of at most the first `n` characters.
 */
int strncmp(const char *s1, const char *s2, size_t n);

/**
 * @brief The first `c` in `s` (the terminator, for c == 0), or NULL.
 */
char *strchr(const char *s, int c);

/**
 * @brief The first `needle` in `haystack` (`haystack` itself for an empty
 *        needle), or NULL.
 */
char *strstr(const char *haystack, const char *needle);

/**
 * @brief Copy at most `n` characters of `src` into `dest` and fill the rest
 *        of the `n` with NULs. As in C, `dest` is not terminated if `src`
 *        is `n` or longer: callers give n one less than the buffer.
 */
char *strncpy(char *dest, const char *src, size_t n);

#endif // LIB_STRING_H
