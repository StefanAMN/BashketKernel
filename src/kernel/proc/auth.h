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
 * Besides its own group a process may be in up to TASK_MAX_GROUPS more,
 * its supplementary groups (setgroups, which only root may call: /bin/login
 * and /bin/su set them from /etc/group). The group digit applies when the
 * file's group is any one of them. They are copied by fork and kept by
 * execve; a set-uid program keeps its caller's.
 *
 * Set-gid (02000) does for the group what set-uid does for the user: the
 * program runs with the file's group as its effective group (the one every
 * check uses), and the real group (getgid) says who started it. Both bits
 * are taken away when anyone but root writes to the file or it changes
 * owner or group.
 *
 * The sticky bit (01000) on a directory restricts removing: anyone may
 * create in /tmp (mode 01777), but only the owner of an entry (or of the
 * directory, or root) may delete or rename it away. Without the bit,
 * permission to write in the directory is enough, as in Unix.
 *
 * Set-gid on a directory makes it a group's directory: a file, directory
 * or link made in it takes the directory's group rather than its creator's,
 * and a new subdirectory gets the bit too, so the rule reaches the whole
 * tree (auth_new_entry_group). A file system that cannot store owners
 * (FAT) shows root for everything regardless.
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
#define PERM_SETGID 02000        // Run with the file's group as the effective group
#define PERM_STICKY 01000        // On a directory: only an entry's owner may remove it
#define PERM_SET_IDS (PERM_SETUID | PERM_SETGID) // Taken away when the file is changed or given away
#define PERM_ALL    07777        // Every bit chmod may set

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
 * @brief The same for a process that is also in the `count` groups of
 *        `groups` (supplementary groups; NULL with count 0 for none): the
 *        group digit applies if the file's group is `gid` or any of them.
 */
bool auth_permits_groups(uint32_t uid, uint32_t gid, const uint32_t *groups, uint32_t count,
                         const vnode_t *node, int want);

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
 * @brief May `uid` remove `child` from `dir` (unlink, or rename it away)?
 *        Write access to the directory; and if the directory is sticky
 *        (PERM_STICKY, as /tmp is), owning the entry or the directory.
 *        Root may remove anything.
 */
bool auth_may_remove(uint32_t uid, const vnode_t *dir, const vnode_t *child);

/**
 * @brief The group a new entry in `dir` belongs to: the directory's own if
 *        it has the set-gid bit, else `creator_gid` (the creator's group).
 */
uint32_t auth_new_entry_group(const vnode_t *dir, uint32_t creator_gid);

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
 * @brief Is the calling process in group `gid`, as its own group or one of
 *        its supplementary groups? (Kernel threads: only in root's.)
 */
bool auth_current_in_group(uint32_t gid);

/**
 * @brief The permission bits the calling process takes away from what it
 *        creates (its umask; the default for kernel threads).
 */
uint16_t auth_current_umask(void);

#endif // AUTH_H
