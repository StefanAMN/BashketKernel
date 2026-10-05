/**
 * @file auth.h
 * @brief Who may do what: user ids and file permissions
 *
 * Every process runs as a user, identified by a number (its uid). User 0 is
 * "root" and may do anything. Every file and directory has an owner and four
 * permission bits: may the owner read it, may the owner write it, may
 * everyone else read it, may everyone else write it. For a directory,
 * "write" means creating and removing entries.
 *
 * The kernel knows only numbers. Names and passwords are the business of
 * /bin/login, a program that runs as root, checks a password against
 * /etc/passwd, and then gives up root by switching to the user's uid. A
 * process that is not root cannot change its uid, so that step is one-way.
 *
 * Deliberately left out: groups, the execute bit (any readable file can be
 * run), set-uid programs (so there is no way back up to root, no `su` for
 * ordinary users), and permissions on anything but files (shared memory
 * segments and ports above 1023 are open to all).
 */

#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>
#include <stdbool.h>
#include "vfs.h"

#define UID_ROOT 0

// Permission bits, numbered as Unix does so that 0644 means what it usually
// means: owner may read and write, others may read
#define PERM_OWNER_READ  0400
#define PERM_OWNER_WRITE 0200
#define PERM_OTHER_READ  0004
#define PERM_OTHER_WRITE 0002
#define PERM_ALL (PERM_OWNER_READ | PERM_OWNER_WRITE | PERM_OTHER_READ | PERM_OTHER_WRITE)

/**
 * @brief May a process running as `uid` read (or, with `write`, change)
 *        this file or directory?
 */
bool auth_may_access(uint32_t uid, const vnode_t *node, bool write);

/**
 * @brief May `uid` remove `child` from `dir`? Needs write permission on the
 *        directory, and, so that a shared directory like /tmp does not let
 *        people delete each other's files, ownership of the file or of the
 *        directory.
 */
bool auth_may_remove(uint32_t uid, const vnode_t *dir, const vnode_t *child);

/**
 * @brief May a process running as `uid` send a signal to one running as
 *        `target_uid`?
 */
bool auth_may_signal(uint32_t uid, uint32_t target_uid);

/**
 * @brief The uid of the calling task (root for kernel threads).
 */
uint32_t auth_current_uid(void);

#endif // AUTH_H
