/**
 * @file auth.c
 * @brief Implementation of User Account Authentication
 */

#include "auth.h"
#include "kprintf.h"
#include "string_util.h"

static user_t root_user = { .uid = 0, .gid = 0, .username = "root", .shell = "/bin/sh" };
static user_t current_user;

void auth_init(void) {
    current_user = root_user;
    kprintf("Auth Init: Registered default accounts (root:0).\n");
}

const user_t *auth_login(const char *user, const char *pass) {
    (void)pass;
    if (!user) return NULL;

    if (strcmp(user, "root") == 0) {
        current_user = root_user;
        return &current_user;
    }
    return NULL;
}

const user_t *auth_get_current_user(void) {
    return &current_user;
}
