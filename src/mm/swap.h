/**
 * @file swap.h
 * @brief Pushing pages out to disk when memory runs short, and reading them
 *        back when they are touched
 *
 * A machine has only so many frames. Without swap, a program that asks for
 * more memory than is free is refused. With a swap disk attached, the pages
 * a process has not used for longest are written to the disk and their
 * frames handed out again; the page-table entry of such a page is marked
 * not present and remembers where on the disk the page went (its "slot").
 * Touching the page faults, and the fault handler reads it back.
 *
 * Rules this implementation keeps (PLAN.md, C4, has the reasons):
 *
 *   - A process only ever loses its own pages, and only at a page fault it
 *     took in user mode. While it is inside a system call, nothing of its
 *     is taken: the kernel may be about to use a buffer it has checked.
 *   - Pages shared with another process (shared memory, or copy-on-write
 *     pages nobody has written yet) stay in memory.
 *   - The victim is chosen by the clock algorithm: a hand goes round the
 *     process's pages; a page whose "accessed" bit is set gets the bit
 *     cleared and is passed over, one whose bit is still clear when the
 *     hand comes round again has not been used in between, and goes.
 *
 * The disk is one made by the host's `mkswap`: a header page with a
 * signature, then one slot per page.
 */

#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SWAP_LOW_FRAMES  256   // Below this many free frames, a faulting process gives pages up...
#define SWAP_HIGH_FRAMES 320   // ...until this many are free again

/**
 * @brief Check a swap disk's first page (the Linux swap header, version 1).
 * @param disk_pages Size of the disk in pages: the header's own idea of its
 *                   size is not believed beyond it.
 * @param slots      Receives the number of usable slots (pages 1 onwards).
 */
bool swap_parse_header(const uint8_t *page, uint64_t disk_pages, uint32_t *slots);

/**
 * @brief Use disk `disk` for swapping, if it carries a swap header.
 * @return 0, or -1 if it does not (or swap is already set up).
 */
int swap_init(unsigned disk);

bool swap_active(void);
size_t swap_total_pages(void);
size_t swap_free_pages(void);
uint64_t swap_pages_out(void);   // Pages written out since boot
uint64_t swap_pages_in(void);    // ...and read back

/**
 * @brief Write up to `want` pages of an address space out and free their
 *        frames. `cursor` is the clock hand: where the last search stopped.
 *        May sleep. The address space must not be in use on another CPU.
 * @return How many pages went.
 */
size_t swap_out(uint64_t *space, uint64_t *cursor, size_t want);

/**
 * @brief Bring the page containing `virt` back, if it is swapped out.
 *        May sleep.
 * @return 1 if it is now present, 0 if it was not a swapped page, -1 if it
 *         is and could not be read back (no free frame, or a disk error).
 */
int swap_in(uint64_t *space, uint64_t virt);

/**
 * @brief Called at a page fault from user mode: if free memory is low, the
 *        current process gives up pages until it is not. May sleep.
 */
void swap_relieve_pressure(void);

// Slots are counted, because fork gives parent and child the same one.
// Called by the address space code (vmm.c) as entries are copied and dropped.

/**
 * @brief One more page-table entry refers to `slot` (fork copied one).
 *        Numbers outside the swap disk are ignored.
 */
void swap_slot_ref(uint32_t slot);

/**
 * @brief One entry fewer refers to `slot` (it was read back, unmapped, or its
 *        address space destroyed); with the last, the slot is free again.
 *        Numbers outside the swap disk are ignored.
 */
void swap_slot_unref(uint32_t slot);

#endif // SWAP_H
