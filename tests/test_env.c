/**
 * @file test_env.c
 * @brief Unit Test Suite for Process Environment Variables & User Stack Serialization
 */

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

TEST(env_stack_setup_test) {
    env_init();

    static uint8_t mock_user_stack[4096] __attribute__((aligned(16)));
    uint64_t stack_top = (uint64_t)mock_user_stack + sizeof(mock_user_stack);

    uint64_t out_rsp = 0;
    uint64_t out_envp_ptr = 0;

    int res = env_setup_user_stack(stack_top, NULL, &out_rsp, &out_envp_ptr);
    EXPECT_EQ(res, 0);
    EXPECT_TRUE(out_rsp != 0);
    EXPECT_TRUE(out_envp_ptr != 0);
    EXPECT_TRUE((out_rsp % 16) == 0); // 16-byte System V AMD64 ABI stack alignment check
}

void test_suite_env(void) {
    kprintf("Suite: Phase 18 Process Environment Variables\n");
    RUN_TEST(env_set_get_unset_test);
    RUN_TEST(env_stack_setup_test);
}
