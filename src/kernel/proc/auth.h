/**
 * @file auth.h
 * @brief Users, groups and file permissions
 *
 * The kernel knows users and groups only as numbers. A process has a user
 * id and a group id; a file has an owner, a group and twelve permission
 * bits. Names and passwords are the business of programs (/bin/login,
 * /bin/su, /bin/passwd, and the file /etc/passwd).
 *
 * The permission bits are Unix's, written in octal so that 0754 reads as
 * three digits: what the owner may do, what members of the file's group
 * may do, what everyone else may do. In each digit 4 is read, 2 is write
 * and 1 is execute. Exactly one digit applies to a process: the owner's
 * if it is the owner, else the group's if it is in the group, else the
 * last. (So an owner can be denied what others are allowed.)
 *
 * User 0, root, may read and write anything, and execute any file that
 * anyone at all may execute.
 *
 * A process really has two user ids. The effective one is what every check
 * uses. The real one says who started it. They differ only while it runs a
 * set-uid program: a file with the 04000 bit runs with its *owner's* rights
 * rather than its caller's. That is how an ordinary user changes a password
 * in a file only root may write: /bin/passwd belongs to root and has the
 * bit, and takes care to change only the caller's own line.
 *
 * Not done: supplementary groups (a process is in one group), the execute
 * bit on directories (anyone may look a name up in any directory), set-gid
 * and the sticky bit (/tmp instead has a rule of its own: see
 * auth_may_remove).
 */

#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>
#include <stdbool.h>
#include "vfs.h"

#define UID_ROOT 0
#define GID_ROOT 0
#define GID_NONE 0xFFFFFFFFu     // "In no group": only the owner's or everyone's bits can apply

// What is being asked for
#define PERM_READ  4
#define PERM_WRITE 2
#define PERM_EXEC  1

#define PERM_SETUID 04000        // Run with the file owner's user id
#define PERM_ALL    04777        // Every bit chmod may set

// New files and directories get these, less the creator's umask
#define PERM_NEW_FILE 0666
#define PERM_NEW_DIR  0777
#define UMASK_DEFAULT 022        // Take away write for group and others

/**
 * @brief May a process with this user and group do `want` (PERM_READ,
 *        PERM_WRITE or PERM_EXEC, one of them) to the node?
 */
bool auth_permits(uint32_t uid, uint32_t gid, const vnode_t *node, int want);

/**
 * @brief The same for reading or writing, taking the group from the
 *        calling process if `uid` is its own (else: no group).
 */
bool auth_may_access(uint32_t uid, const vnode_t *node, bool write);

/**
 * @brief May `uid` run `node` as a program (its execute bit for the owner,
 *        the group or others)? Root may run anything that has any execute
 *        bit at all.
 */
bool auth_may_execute(uint32_t uid, const vnode_t *node);

/**
 * @brief May `uid` look names up in the directory `dir` (its execute bit,
 *        "search": needed for every directory a path passes through)?
 *        Root may search any directory.
 */
bool auth_may_search(uint32_t uid, const vnode_t *dir);

/**
 * @brief May `uid` remove `child` from `dir`? Write access to the directory,
 *        and (so that /tmp can be writable by all) owning the entry or the
 *        directory.
 */
bool auth_may_remove(uint32_t uid, const vnode_t *dir, const vnode_t *child);

/**
 * @brief May `uid` send a signal to a process running as `target_uid`?
 *        Root may signal anyone; anyone else only their own processes.
 */
bool auth_may_signal(uint32_t uid, uint32_t target_uid);

/**
 * @brief The effective user of the calling process: whose rights it has
 *        (root for kernel threads).
 */
uint32_t auth_current_uid(void);

/**
 * @brief The group of the calling process (root's for kernel threads).
 */
uint32_t auth_current_gid(void);

/**
 * @brief The permission bits the calling process takes away from what it
 *        creates (its umask; the default for kernel threads).
 */
uint16_t auth_current_umask(void);

#endif // AUTH_H
