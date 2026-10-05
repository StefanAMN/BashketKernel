/**
 * @file auth.c
 * @brief User ids and file permissions (see auth.h)
 */

#include "auth.h"
#include "sched.h"

bool auth_may_access(uint32_t uid, const vnode_t *node, bool write) {
    if (!node) return false;
    if (uid == UID_ROOT) return true;

    if (uid == node->uid) return (node->mode & (write ? PERM_OWNER_WRITE : PERM_OWNER_READ)) != 0;
    return (node->mode & (write ? PERM_OTHER_WRITE : PERM_OTHER_READ)) != 0;
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
