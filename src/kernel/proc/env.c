/**
 * @file env.c
 * @brief Implementation of Environment Variables & User Stack Serialization for Execve
 */

#include "env.h"
#include "kprintf.h"
#include "string_util.h"

#define MAX_ENV_ENTRIES 32

static env_entry_t env_table[MAX_ENV_ENTRIES];
static size_t env_count = 0;

void env_init(void) {
    memset(env_table, 0, sizeof(env_table));
    env_count = 0;

    // Default system environment variables
    env_set("PATH", "/bin:/usr/bin");
    env_set("USER", "root");
    env_set("HOME", "/root");
    env_set("SHELL", "/bin/sh");
    env_set("OS", "BashketOS");

    kprintf("env: one global variable table (not per process)\n");
}

int env_set(const char *key, const char *val) {
    if (!key || !val) return -1;

    for (size_t i = 0; i < env_count; i++) {
        if (strcmp(env_table[i].key, key) == 0) {
            size_t j = 0;
            for (; val[j] != '\0' && j < sizeof(env_table[i].value) - 1; j++) {
                env_table[i].value[j] = val[j];
            }
            env_table[i].value[j] = '\0';
            return 0;
        }
    }

    if (env_count >= MAX_ENV_ENTRIES) return -1;

    size_t k = 0;
    for (; key[k] != '\0' && k < sizeof(env_table[env_count].key) - 1; k++) {
        env_table[env_count].key[k] = key[k];
    }
    env_table[env_count].key[k] = '\0';

    size_t v = 0;
    for (; val[v] != '\0' && v < sizeof(env_table[env_count].value) - 1; v++) {
        env_table[env_count].value[v] = val[v];
    }
    env_table[env_count].value[v] = '\0';

    env_count++;
    return 0;
}

const char *env_get(const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < env_count; i++) {
        if (strcmp(env_table[i].key, key) == 0) {
            return env_table[i].value;
        }
    }
    return NULL;
}

int env_unset(const char *key) {
    if (!key) return -1;
    for (size_t i = 0; i < env_count; i++) {
        if (strcmp(env_table[i].key, key) == 0) {
            for (size_t j = i; j < env_count - 1; j++) {
                env_table[j] = env_table[j + 1];
            }
            env_count--;
            memset(&env_table[env_count], 0, sizeof(env_entry_t));
            return 0;
        }
    }
    return -1;
}

void env_print_all(void) {
    for (size_t i = 0; i < env_count; i++) {
        kprintf("%s=%s\n", env_table[i].key, env_table[i].value);
    }
}

int env_setup_user_stack(uint64_t user_stack_top, char *const custom_envp[], uint64_t *out_user_rsp, uint64_t *out_envp_ptr) {
    if (!user_stack_top || !out_user_rsp) return -1;

    uint64_t sp = user_stack_top;
    uint64_t env_ptrs[MAX_ENV_ENTRIES + 16];
    size_t total_env_count = 0;

    // 1. Push environment strings onto top of stack
    if (custom_envp && custom_envp[0] != NULL) {
        for (size_t i = 0; custom_envp[i] != NULL && total_env_count < (MAX_ENV_ENTRIES + 15); i++) {
            size_t len = strlen(custom_envp[i]);
            sp -= (len + 1);
            memcpy((void *)sp, custom_envp[i], len + 1);
            env_ptrs[total_env_count++] = sp;
        }
    } else {
        // Serialize kernel default env_table: "KEY=VAL"
        for (size_t i = 0; i < env_count && total_env_count < MAX_ENV_ENTRIES; i++) {
            char env_str[256];
            memset(env_str, 0, sizeof(env_str));
            strcpy(env_str, env_table[i].key);
            strcat(env_str, "=");
            strcat(env_str, env_table[i].value);

            size_t len = strlen(env_str);
            sp -= (len + 1);
            memcpy((void *)sp, env_str, len + 1);
            env_ptrs[total_env_count++] = sp;
        }
    }

    // 2. Align stack pointer to 16-byte boundary (System V AMD64 ABI)
    sp &= ~((uint64_t)15);

    // 3. Push NULL sentinel pointer
    sp -= sizeof(uint64_t);
    *(uint64_t *)sp = 0;

    // 4. Push envp pointer array in reverse order
    for (int i = (int)total_env_count - 1; i >= 0; i--) {
        sp -= sizeof(uint64_t);
        *(uint64_t *)sp = env_ptrs[i];
    }

    uint64_t envp_vaddr = sp;

    if (out_envp_ptr) *out_envp_ptr = envp_vaddr;
    *out_user_rsp = sp;

    kprintf("Env Stack Setup: Serialized %d environment variables onto user stack at %p (RSP: %p)\n",
            (int)total_env_count, (void *)envp_vaddr, (void *)sp);

    return 0;
}
