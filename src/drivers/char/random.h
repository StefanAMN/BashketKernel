/**
 * @file random.h
 * @brief The kernel's random number generator
 *
 * Address-space layout, TCP sequence numbers, DNS query ids and password
 * salts all depend on numbers nobody else can predict. A generator has two
 * parts, and each can fail on its own:
 *
 *  - Entropy: something an outsider really cannot know. Where the CPU has a
 *    hardware generator (RDSEED, or RDRAND) that is used. Otherwise all there
 *    is are the exact times at which interrupts arrive (keys, disk, network),
 *    measured with the timestamp counter. That is thin, and thinnest right
 *    after boot: on such a machine the first numbers are only as good as the
 *    boot time's unpredictability. The boot log says which case applies.
 *
 *  - Expansion: turning a little entropy into as many bytes as are asked
 *    for, such that seeing some output says nothing about the rest. Here
 *    that is ChaCha20, used so that the key is replaced after every request
 *    ("fast key erasure"): someone who learns the generator's state later
 *    still cannot work out what it produced earlier.
 *
 * New timing measurements keep being mixed in, and the key is rekeyed from
 * them (through SHA-256) now and then.
 */

#ifndef RANDOM_H
#define RANDOM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Fill `buf` (kernel memory) with `count` random bytes. Safe from
 *        any context, interrupt handlers included; never sleeps; seeds
 *        itself on first use.
 */
void get_random_bytes(void *buf, size_t count);

/**
 * @brief Four random bytes as a number (get_random_bytes).
 */
uint32_t get_random_u32(void);

/**
 * @brief Note that something happened just now whose timing is not under
 *        anyone's control: called from device interrupt handlers. `value`
 *        is anything else worth mixing in (a scancode, a byte count).
 */
void random_add_event(uint64_t value);

/**
 * @brief Seed now and report what the seed is made of. Called once at boot;
 *        harmless to call again.
 */
void random_init(void);

/**
 * @brief "RDSEED", "RDRAND" or "timing only": where the entropy comes from.
 */
const char *random_source(void);

/**
 * @brief How often the key has been remade from collected entropy.
 */
uint64_t random_reseed_count(void);

/**
 * @brief The expansion step on its own, for tests: from `key`, the next
 *        key and `count` bytes of output (at most 224).
 */
void random_expand(const uint8_t key[32], uint64_t request_number, uint8_t next_key[32], uint8_t *out, size_t count);

#endif // RANDOM_H
