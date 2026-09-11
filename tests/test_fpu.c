#include "test.h"
#include "fpu.h"
#include "process.h"
#include "sched.h"
#include "kstring.h"

TEST(fpu_clean_state) {
    // The template every program starts from: default control words
    uint8_t state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    memset(state, 0xAA, sizeof(state));
    fpu_state_init(state);

    uint16_t fcw;
    uint32_t mxcsr;
    memcpy(&fcw, state + 0, sizeof(fcw));      // x87 control word
    memcpy(&mxcsr, state + 24, sizeof(mxcsr)); // SSE control/status
    EXPECT_EQ(fcw, 0x037F);
    EXPECT_EQ(mxcsr, 0x1F80u);

    // XMM0..XMM15 (offset 160, 16 bytes each) start as zero
    int nonzero = 0;
    for (int i = 160; i < 160 + 16 * 16; i++) nonzero |= state[i];
    EXPECT_EQ(nonzero, 0);
}

// /bin/fptest runs floating point loops in several processes at once and
// fails if any of them sees another's register contents.
TEST(fpu_state_is_per_process) {
    pid_t pid = process_spawn("/bin/fptest", NULL);
    EXPECT_TRUE(pid > 0);

    int status = -1;
    EXPECT_EQ(process_wait(pid, &status), pid);
    EXPECT_EQ(status, 0);
}

void test_suite_fpu(void) {
    kprintf("Suite: FPU and SSE State\n");
    RUN_TEST(fpu_clean_state);
    RUN_TEST(fpu_state_is_per_process);
}
