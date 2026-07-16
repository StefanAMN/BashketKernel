/**
 * @file env.h
 * @brief Environment Variables Subsystem Interface
 */

#ifndef ENV_H
#define ENV_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char key[64];
    char value[128];
} env_entry_t;

/**
 * @brief Initialize process environment variable subsystem.
 */
void env_init(void);

/**
 * @brief Set or update an environment variable.
 * @param key Variable key.
 * @param val Variable string value.
 * @return 0 on success, -1 on failure.
 */
int env_set(const char *key, const char *val);

/**
 * @brief Get value of an environment variable.
 * @param key Variable key to look up.
 * @return Value string pointer or NULL if not set.
 */
const char *env_get(const char *key);

/**
 * @brief Remove an environment variable.
 * @param key Variable key to remove.
 * @return 0 on success, -1 on failure.
 */
int env_unset(const char *key);

/**
 * @brief Display all set environment variables.
 */
void env_print_all(void);

#endif // ENV_H
