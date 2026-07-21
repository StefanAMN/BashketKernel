/* fptest: checks that each process has its own floating point registers.
 *
 * Several processes run the same long calculation at the same time, each from
 * a different starting value, and each keeps its running values in SSE
 * registers. The timer interrupts them many times. If the kernel failed to
 * save and restore those registers on a task switch, a process would continue
 * with another's numbers and get the wrong answer. */
#include "ulib.h"

#define WORKERS 4
#define ROUNDS  3000000

/* A calculation with no function calls, so the compiler keeps x, sum and
 * weight in XMM registers for the whole loop. The result is very sensitive to
 * every intermediate value, so one corrupted register changes the answer. */
__attribute__((noinline)) static double churn(double seed) {
    double x = seed, sum = 0.0, weight = 1.0;
    for (long i = 0; i < ROUNDS; i++) {
        x = 3.9 * x * (1.0 - x);        /* Logistic map: stays in (0, 1), chaotic */
        sum += x * weight;
        weight = weight * 0.999999 + 1e-7;
    }
    return sum + x;
}

/* Bit-exact comparison: the same inputs must give the same bits every time */
static int same(double x, double y) {
    return memcmp(&x, &y, sizeof(double)) == 0;
}

int main(void) {
    /* A fresh program starts with clean registers: default rounding, all
     * floating point exceptions masked (so 1.0/0.0 is infinity, not a fault). */
    unsigned int mxcsr = 0;
    asm volatile("stmxcsr [%0]" : : "r"(&mxcsr) : "memory");
    if ((mxcsr & 0xFFC0) != 0x1F80) {
        printf("fptest: FAIL unexpected MXCSR %x at start\n", mxcsr);
        return 1;
    }

    /* Reference answers, computed with nothing else using the FPU */
    double expected[WORKERS];
    for (int w = 0; w < WORKERS; w++) expected[w] = churn(0.1 + 0.1 * w);
    if (same(expected[0], expected[1])) {
        printf("fptest: FAIL the calculation does not depend on its seed\n");
        return 1;
    }

    /* A register value that must survive fork into the child */
    volatile double carried = 1234.5;

    long pids[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        pids[w] = fork();
        if (pids[w] == 0) {
            if (carried != 1234.5) exit(2);
            /* Twice, so every worker is still running while others start */
            int ok = same(churn(0.1 + 0.1 * w), expected[w]) && same(churn(0.1 + 0.1 * w), expected[w]);
            exit(ok ? 0 : 1);
        }
        if (pids[w] < 0) {
            printf("fptest: FAIL fork: %s\n", strerror(pids[w]));
            return 1;
        }
    }

    /* The parent computes too, so five processes are sharing the CPU */
    int failures = same(churn(0.77), churn(0.77)) ? 0 : 1;

    for (int w = 0; w < WORKERS; w++) {
        int status = -1;
        waitpid(pids[w], &status);
        if (status != 0) {
            printf("fptest: FAIL worker %d got a wrong result (status %d)\n", w, status);
            failures++;
        }
    }

    if (failures == 0) printf("fptest: all workers computed the right values\n");
    return failures ? 1 : 0;
}
