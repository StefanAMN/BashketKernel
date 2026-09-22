/**
 * @file string.c
 * @brief Implementation of Standard Memory and String Library Routines
 */

#include "string.h"

// memset, memcpy and memmove use the CPU's string instructions: eight bytes
// at a time with "rep stosq/movsq", then the last few bytes one at a time.
// A plain C byte loop here was the slowest code in the kernel: every page
// clear, page copy and screen scroll goes through these three functions.
// (The direction flag is clear on entry, as the C ABI guarantees.)

void *memset(void *dest, int val, size_t count) {
    void *d = dest;
    uint64_t pattern = 0x0101010101010101ULL * (uint8_t)val; // The byte, eight times
    size_t qwords = count / 8, bytes = count % 8;

    asm volatile("rep stosq" : "+D"(d), "+c"(qwords) : "a"(pattern) : "memory");
    asm volatile("rep stosb" : "+D"(d), "+c"(bytes) : "a"(pattern) : "memory");
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
    void *d = dest;
    const void *s = src;
    size_t qwords = count / 8, bytes = count % 8;

    asm volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(qwords) : : "memory");
    asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(bytes) : : "memory");
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    // Copying forwards is safe unless the destination starts inside the
    // source, in which case it would overwrite bytes it has yet to read.
    if (d <= s || d >= s + count) return memcpy(dest, src, count);

    // Overlap with the destination higher: copy backwards, from the last
    // byte down. "std" makes the string instructions count downwards; it must
    // be cleared again before any other code runs.
    const uint8_t *s_last = s + count - 1;
    uint8_t *d_last = d + count - 1;
    asm volatile("std\n\trep movsb\n\tcld" : "+D"(d_last), "+S"(s_last), "+c"(count) : : "memory", "cc");
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t count) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < count; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

size_t strlen(const char *str) {
    size_t len = 0;
    while (str && str[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    return 0;
}

char *strcpy(char *dest, const char *src) {
    char *orig = dest;
    while ((*dest++ = *src++) != '\0');
    return orig;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *orig = dest;
    while (*dest != '\0') {
        dest++;
    }
    while ((*dest++ = *src++) != '\0');
    return orig;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s; // Also finds the terminator when c is 0
        if (*s == '\0') return NULL;
    }
}

char *strstr(const char *haystack, const char *needle) {
    if (*needle == '\0') return (char *)haystack;

    for (; *haystack != '\0'; haystack++) {
        const char *h = haystack, *n = needle;
        while (*n != '\0' && *h == *n) { h++; n++; }
        if (*n == '\0') return (char *)haystack;
    }
    return NULL;
}
