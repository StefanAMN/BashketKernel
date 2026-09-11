/**
 * @file auth.c
 * @brief User ids and file permissions (see auth.h)
 */

#include "auth.h"
#include "sched.h"

bool auth_permits(uint32_t uid, uint32_t gid, const vnode_t *node, int want) {
    return auth_permits_groups(uid, gid, NULL, 0, node, want);
}

// Is `gid` the process's group or among its `count` other `groups`?
// (GID_NONE is in no list: it means "no group at all".)
static bool group_matches(uint32_t gid, uint32_t node_gid, const uint32_t *groups, uint32_t count) {
    if (node_gid == GID_NONE) return false;
    if (gid == node_gid) return true;
    for (uint32_t i = 0; i < count && groups; i++) {
        if (groups[i] == node_gid) return true;
    }
    return false;
}

bool auth_permits_groups(uint32_t uid, uint32_t gid, const uint32_t *groups, uint32_t count,
                         const vnode_t *node, int want) {
    if (!node) return false;
    if (uid == UID_ROOT) {
        // Root reads and writes anything; "execute" still needs the file to
        // be executable by someone, so that a data file is not run by slip
        if (want != PERM_EXEC) return true;
        return node->type == VFS_DIRECTORY || (node->mode & 0111) != 0;
    }

    // One digit of the mode applies: the owner's, the group's or the rest's
    int shift = (uid == node->uid) ? 6 : group_matches(gid, node->gid, groups, count) ? 3 : 0;
    return ((node->mode >> shift) & want) != 0;
}

// May `uid` do `want` to `node`, with the groups of the calling process if
// that is who is asking? (Kernel code that asks on behalf of some other
// user has only the number, and so no group.) Only the task itself changes
// its groups, so reading them here needs no lock.
static bool permits_for(uint32_t uid, const vnode_t *node, int want) {
    task_t *task = sched_get_current_task();
    if (task && task->is_user && task->uid == uid) {
        return auth_permits_groups(uid, task->gid, task->groups, task->group_count, node, want);
    }
    return auth_permits(uid, GID_NONE, node, want);
}

bool auth_may_access(uint32_t uid, const vnode_t *node, bool write) {
    return permits_for(uid, node, write ? PERM_WRITE : PERM_READ);
}

bool auth_may_execute(uint32_t uid, const vnode_t *node) {
    return permits_for(uid, node, PERM_EXEC);
}

bool auth_may_search(uint32_t uid, const vnode_t *dir) {
    return permits_for(uid, dir, PERM_EXEC);
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

bool auth_current_in_group(uint32_t gid) {
    task_t *task = sched_get_current_task();
    if (!task || !task->is_user) return gid == GID_ROOT;
    return group_matches(task->gid, gid, task->groups, task->group_count);
}

uint16_t auth_current_umask(void) {
    task_t *task = sched_get_current_task();
    return (task && task->is_user) ? task->umask : UMASK_DEFAULT;
}
