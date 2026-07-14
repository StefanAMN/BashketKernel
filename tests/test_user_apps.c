#include "test.h"
#include "user_apps.h"
#include "vfs.h"

TEST(user_apps_registration_test) {
    user_apps_init();
    vnode_t *hello_binary = vfs_find(NULL, "bin/hello");
    EXPECT_TRUE(hello_binary != NULL);

    vnode_t *calc_binary = vfs_find(NULL, "bin/calc");
    EXPECT_TRUE(calc_binary != NULL);
}

void test_suite_user_apps(void) {
    kprintf("Suite: Phase 16 Userland Environment & ELF Programs\n");
    RUN_TEST(user_apps_registration_test);
}
