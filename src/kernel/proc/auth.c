/**
 * @file auth.c
 * @brief User ids and file permissions (see auth.h)
 */

#include "auth.h"
#include "sched.h"

bool auth_permits(uint32_t uid, uint32_t gid, const vnode_t *node, int want) {
    if (!node) return false;
    if (uid == UID_ROOT) {
        // Root reads and writes anything; "execute" still needs the file to
        // be executable by someone, so that a data file is not run by slip
        if (want != PERM_EXEC) return true;
        return node->type == VFS_DIRECTORY || (node->mode & 0111) != 0;
    }

    // One digit of the mode applies: the owner's, the group's or the rest's
    int shift = (uid == node->uid) ? 6 : (gid != GID_NONE && gid == node->gid) ? 3 : 0;
    return ((node->mode >> shift) & want) != 0;
}

// The group that goes with a user id: the calling process's, if that is who
// is asking. (Kernel code that asks on behalf of some other user has only
// the number.)
static uint32_t group_for(uint32_t uid) {
    task_t *task = sched_get_current_task();
    return (task && task->is_user && task->uid == uid) ? task->gid : GID_NONE;
}

bool auth_may_access(uint32_t uid, const vnode_t *node, bool write) {
    return auth_permits(uid, group_for(uid), node, write ? PERM_WRITE : PERM_READ);
}

bool auth_may_execute(uint32_t uid, const vnode_t *node) {
    return auth_permits(uid, group_for(uid), node, PERM_EXEC);
}

bool auth_may_search(uint32_t uid, const vnode_t *dir) {
    return auth_permits(uid, group_for(uid), dir, PERM_EXEC);
}

bool auth_may_remove(uint32_t uid, const vnode_t *dir, const vnode_t *child) {
    if (!dir || !child) return false;
    if (uid == UID_ROOT) return true;
    if (!auth_may_access(uid, dir, true)) return false;
    return child->uid == uid || dir->uid == uid;
}

bool auth_may_signal(uint32_t uid, uint32_t target_uid) {
    return uid == UID_ROOT || uid == target_uid;
}

uint32_t auth_current_uid(void) {
    task_t *task = sched_get_current_task();
    return (task && task->is_user) ? task->uid : UID_ROOT;
}

uint32_t auth_current_gid(void) {
    task_t *task = sched_get_current_task();
    return (task && task->is_user) ? task->gid : GID_ROOT;
}

uint16_t auth_current_umask(void) {
    task_t *task = sched_get_current_task();
    return (task && task->is_user) ? task->umask : UMASK_DEFAULT;
}
