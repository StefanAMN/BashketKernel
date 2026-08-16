/**
 * @file auth.h
 * @brief User Account Authentication Subsystem Interface
 */

#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>

typedef struct {
    uint32_t uid;
    uint32_t gid;
    char username[32];
    char shell[64];
} user_t;

/**
 * @brief Initialize user account subsystem.
 */
void auth_init(void);

/**
 * @brief Authenticate credentials against account table.
 * @param user Username string.
 * @param pass Password string.
 * @return Pointer to user_t profile or NULL on authentication failure.
 */
const user_t *auth_login(const char *user, const char *pass);

/**
 * @brief Retrieve current active user profile context.
 * @return Pointer to active user_t object.
 */
const user_t *auth_get_current_user(void);

#endif // AUTH_H
