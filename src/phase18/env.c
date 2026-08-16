/**
 * @file env.c
 * @brief Implementation of Environment Variables Table Management
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

    // Set default standard environment variables
    env_set("PATH", "/bin:/usr/bin");
    env_set("USER", "root");
    env_set("HOME", "/root");
    env_set("SHELL", "/bin/sh");
    env_set("OS", "BashketOS");

    kprintf("Env Init: Default process environment variables registered.\n");
}

int env_set(const char *key, const char *val) {
    if (!key || !val) return -1;

    // Check if key already exists in table
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

    if (env_count >= MAX_ENV_ENTRIES) return -1; // Table full

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
            // Shift remaining entries left
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
