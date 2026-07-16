#include "test.h"
#include "auth.h"
#include "string_util.h"

TEST(auth_login_test) {
    auth_init();

    const user_t *u = auth_get_current_user();
    EXPECT_TRUE(u != NULL && u->uid == 0);
    EXPECT_TRUE(strcmp(u->username, "root") == 0);

    const user_t *login_u = auth_login("root", "root");
    EXPECT_TRUE(login_u != NULL);
}

void test_suite_auth(void) {
    kprintf("Suite: Phase 31 User Account Authentication\n");
    RUN_TEST(auth_login_test);
}
