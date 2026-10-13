/**
 * @file random.c
 * @brief The kernel's random number generator (see random.h)
 */

#include "random.h"
#include "cpu.h"
#include "spinlock.h"
#include "kstring.h"
#include "kprintf.h"
#include "crypto/chacha20.h"
#include "crypto/sha256.h"

#define RESEED_AFTER_EVENTS 32   // Remake the key once this many new timings have been collected
#define EXPAND_BLOCK 256         // Keystream per step: 32 bytes of next key, 224 of output

static uint8_t key[32];
static uint64_t requests = 0;    // Makes every step's keystream different even under one key
static bool seeded = false;
static uint64_t reseeds = 0;
static spinlock_t rng_lock = SPINLOCK_RANKED("rng", LOCK_RANK_TIME);

// Timings waiting to be folded into the key. Written from interrupt
// handlers without a lock: two CPUs mixing at once lose nothing that
// matters, and no reader depends on a consistent view of it.
static uint64_t pool[8];
static volatile uint32_t pool_position = 0;
static volatile uint32_t pool_events = 0;

static const char *source = "timing only";

static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

// The CPU's own generators (Intel SDM Vol. 1, 7.3.17). Both report failure
// in the carry flag (the generator can run dry), so each is tried a few
// times. RDSEED gives raw entropy; RDRAND the output of a generator the CPU
// seeds from it.
static bool cpu_has_rdseed(void) {
    uint32_t r[4];
    cpuid(0, 0, r);
    if (r[0] < 7) return false;
    cpuid(7, 0, r);
    return (r[1] >> 18) & 1;     // EBX bit 18
}

static bool cpu_has_rdrand(void) {
    uint32_t r[4];
    cpuid(1, 0, r);
    return (r[2] >> 30) & 1;     // ECX bit 30
}

static bool rdseed64(uint64_t *out) {
    for (int tries = 0; tries < 32; tries++) {
        uint8_t ok;
        asm volatile("rdseed %0\n\tsetc %1" : "=r"(*out), "=qm"(ok) : : "cc");
        if (ok) return true;
        asm volatile("pause");
    }
    return false;
}

static bool rdrand64(uint64_t *out) {
    for (int tries = 0; tries < 16; tries++) {
        uint8_t ok;
        asm volatile("rdrand %0\n\tsetc %1" : "=r"(*out), "=qm"(ok) : : "cc");
        if (ok) return true;
    }
    return false;
}

void random_add_event(uint64_t value) {
    uint64_t now = rdtsc();
    uint32_t at = __atomic_fetch_add(&pool_position, 1, __ATOMIC_RELAXED) % 8;
    // Rotate what is there before adding, so that events landing on the
    // same word do not simply cancel
    pool[at] = ((pool[at] << 13) | (pool[at] >> 51)) ^ now ^ (value * 0x9E3779B97F4A7C15ULL);
    __atomic_fetch_add(&pool_events, 1, __ATOMIC_RELAXED);
}

// Remake the key from the old key, everything collected, the time, and the
// CPU's generator if it has one. Caller holds rng_lock.
static void reseed(void) {
    uint64_t hardware[4] = {0};
    bool have_seed = cpu_has_rdseed(), have_rand = cpu_has_rdrand();
    bool got = false;
    for (int i = 0; i < 4; i++) {
        if (have_seed && rdseed64(&hardware[i])) got = true;
        else if (have_rand && rdrand64(&hardware[i])) got = true;
    }
    if (got) source = have_seed ? "RDSEED" : "RDRAND";

    uint64_t now = rdtsc();
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, key, sizeof(key));
    sha256_update(&ctx, (const uint8_t *)pool, sizeof(pool));
    sha256_update(&ctx, (const uint8_t *)hardware, sizeof(hardware));
    sha256_update(&ctx, (const uint8_t *)&now, sizeof(now));
    sha256_final(&ctx, key);

    memset(hardware, 0, sizeof(hardware));
    __atomic_store_n(&pool_events, 0, __ATOMIC_RELAXED);
    seeded = true;
    reseeds++;
}

void random_expand(const uint8_t from_key[32], uint64_t request_number, uint8_t next_key[32], uint8_t *out, size_t count) {
    uint8_t zeros[EXPAND_BLOCK], stream[EXPAND_BLOCK];
    uint8_t nonce[12] = {0};
    memset(zeros, 0, sizeof(zeros));
    memcpy(nonce, &request_number, sizeof(request_number));
    if (count > EXPAND_BLOCK - 32) count = EXPAND_BLOCK - 32;

    // The keystream is the cipher applied to zeros. Its first 32 bytes
    // replace the key and are never given out; the rest is the output.
    chacha20_encrypt(from_key, nonce, 0, zeros, stream, sizeof(stream));
    memcpy(next_key, stream, 32);
    memcpy(out, stream + 32, count);
    memset(stream, 0, sizeof(stream));
}

void get_random_bytes(void *buf, size_t count) {
    uint8_t *out = buf;
    while (count > 0) {
        size_t chunk = count < EXPAND_BLOCK - 32 ? count : EXPAND_BLOCK - 32;
        uint8_t block[EXPAND_BLOCK - 32];

        uint64_t flags = spinlock_acquire_irqsave(&rng_lock);
        if (!seeded || pool_events >= RESEED_AFTER_EVENTS) reseed();
        random_expand(key, requests++, key, block, chunk);
        spinlock_release_irqrestore(&rng_lock, flags);

        // Copied out after the lock is dropped: `buf` is the caller's, and
        // the lock is taken from interrupt context elsewhere
        memcpy(out, block, chunk);
        memset(block, 0, sizeof(block));
        out += chunk;
        count -= chunk;
    }
}

uint32_t get_random_u32(void) {
    uint32_t value;
    get_random_bytes(&value, sizeof(value));
    return value;
}

void random_init(void) {
    uint64_t flags = spinlock_acquire_irqsave(&rng_lock);
    reseed();
    spinlock_release_irqrestore(&rng_lock, flags);
    kprintf("random: ChaCha20 generator seeded from %s%s\n", source,
            source[0] == 't' ? " (this CPU has no hardware generator: weak until interrupts have been seen)" : "");
}

const char *random_source(void) { return source; }
uint64_t random_reseed_count(void) { return reseeds; }
