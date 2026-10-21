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
 *   - A process only ever loses its own pages: at a page fault it took in
 *     user mode, at fork, and inside a system call that needs a frame for
 *     one of its buffers when none is free. In that last case the pages
 *     of every buffer the call has checked so far stay: the kernel may be
 *     about to use them (process_populate records them, and the system
 *     call entry forgets them when the call returns).
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
#include "vmm.h"

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
 * @brief The same, never taking a page that overlaps one of the
 *        `keep_count` ranges in `keep`.
 */
size_t swap_out_keeping(uint64_t *space, uint64_t *cursor, size_t want, const vmm_range_t *keep, size_t keep_count);

/**
 * @brief Bring the page containing `virt` back, if it is swapped out.
 *        May sleep.
 * @return 1 if it is now present, 0 if it was not a swapped page, -1 if it
 *         is and could not be read back (no free frame, or a disk error).
 */
int swap_in(uint64_t *space, uint64_t virt);

/**
 * @brief Called at a page fault from user mode, and before a system call
 *        gives one of its buffers a frame: if free memory is low, the
 *        current process gives up pages until it is not, keeping those of
 *        the buffers its current system call has checked. May sleep.
 */
void swap_relieve_pressure(void);

/**
 * @brief Called by a system call that could not get a frame for one of its
 *        buffers (process_populate): the current process gives up pages
 *        outside the buffers the call has checked, until SWAP_HIGH_FRAMES
 *        are free, or at least one page. Unlike swap_relieve_pressure it
 *        looks even after a search that found nothing. Gives nothing if
 *        the call has checked more buffers than it can keep track of
 *        (TASK_CALL_RANGES). May sleep.
 * @return How many pages went.
 */
size_t swap_make_room_for_call(void);

/**
 * @brief A page fault from user mode, or a system call giving its buffers
 *        their frames (process_populate), needed a frame and found none:
 *        the room made for it was taken by another process since. Should
 *        it try again? Yes after making more room from the process's own
 *        pages (swap_make_room_for_call); when it has none it may give (a
 *        fork child whose pages are all still shared), after waiting a
 *        tick for other processes, which make room at their own faults,
 *        for a quarter of a second at most. Never without a swap disk.
 * @param attempt Counts from 0.
 * Interrupts must be on and no lock held: this may sleep.
 */
bool swap_may_retry(int attempt);

/**
 * @brief Called by fork: if free memory is below SWAP_LOW_FRAMES, the page
 *        cache gives back what nobody maps; if that was not enough and
 *        there is a swap disk, the calling process gives up pages of its
 *        own until SWAP_HIGH_FRAMES are free (or it has none left to give).
 *        Unlike swap_relieve_pressure it always looks: fork happens rarely,
 *        and afterwards the pages are shared and can no longer go. May sleep.
 * @return How many pages went (to the disk, or out of the cache).
 */
size_t swap_make_room_for_fork(void);

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
