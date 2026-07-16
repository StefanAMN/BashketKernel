#include "test.h"
#include "env.h"
#include "string_util.h"

TEST(env_set_get_unset_test) {
    env_init();

    EXPECT_TRUE(env_set("MY_VAR", "12345") == 0);
    const char *val = env_get("MY_VAR");
    EXPECT_TRUE(val != NULL && strcmp(val, "12345") == 0);

    EXPECT_TRUE(env_unset("MY_VAR") == 0);
    EXPECT_TRUE(env_get("MY_VAR") == NULL);
}

void test_suite_env(void) {
    kprintf("Suite: Phase 18 Process Environment Variables\n");
    RUN_TEST(env_set_get_unset_test);
}
