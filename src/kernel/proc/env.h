/**
 * @file env.h
 * @brief Environment Variables Subsystem Interface (Stack-Based Execve Loading)
 */

#ifndef ENV_H
#define ENV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    char key[64];
    char value[128];
} env_entry_t;

/**
 * @brief Initialize system default environment variables.
 */
void env_init(void);

/**
 * @brief Set or update an environment variable in kernel table.
 * @param key Variable key.
 * @param val Variable string value.
 * @return 0 on success, -1 on failure.
 */
int env_set(const char *key, const char *val);

/**
 * @brief Get value of an environment variable from kernel table.
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

/**
 * @brief Serialize environment variables and load them onto user process stack frame for execve.
 * @param user_stack_top Top virtual address of allocated user stack.
 * @param custom_envp Optional NULL-terminated array of "KEY=VAL" strings (or NULL to use kernel defaults).
 * @param out_user_rsp Pointer to receive updated user stack pointer (RSP).
 * @param out_envp_ptr Pointer to receive virtual address of envp pointer array on user stack.
 * @return 0 on success, -1 on failure.
 */
int env_setup_user_stack(uint64_t user_stack_top, char *const custom_envp[], uint64_t *out_user_rsp, uint64_t *out_envp_ptr);

#endif // ENV_H
