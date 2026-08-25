/**
 * @file user_apps.h
 * @brief Userland Binary Application Suite Interface
 */

#ifndef USER_APPS_H
#define USER_APPS_H

#include <stdint.h>

/**
 * @brief Register built-in user ELF binaries into VFS /bin directory.
 */
void user_apps_init(void);

#endif // USER_APPS_H
