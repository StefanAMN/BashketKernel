/* Does something a user program must not be able to do, chosen by argv[1].
 * In every case the kernel should kill this process and carry on. */
#include "ulib.h"

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "";

    if (strcmp(what, "cli") == 0) {
        asm volatile("cli");                       /* Privileged: #GP */
    } else if (strcmp(what, "kread") == 0) {
        volatile long *kernel = (long *)0xffffffff80000000;
        printf("kernel memory: %lx\n", *kernel);   /* Supervisor page: #PF */
    } else if (strcmp(what, "kwrite") == 0) {
        volatile long *kernel = (long *)0xffffffff80000000;
        *kernel = 0;                               /* #PF */
    } else if (strcmp(what, "null") == 0) {
        volatile int *null = (int *)0;
        *null = 1;                                 /* Page 0 is never mapped: #PF */
    } else if (strcmp(what, "text") == 0) {
        volatile char *code = (char *)(unsigned long)main;
        *code = 0;                                 /* Text is read-only: #PF */
    } else if (strcmp(what, "exec-stack") == 0) {
        unsigned char ret = 0xc3;                  /* A "ret" on the stack */
        ((void (*)(void))(unsigned long)&ret)();   /* Stack is no-execute: #PF */
    } else if (strcmp(what, "div0") == 0) {
        volatile int zero = 0, one = 1;
        printf("%d\n", one / zero);                /* #DE */
    } else if (strcmp(what, "sgdt") == 0) {
        /* Reveals where the kernel's GDT is. Blocked only on CPUs with UMIP;
         * on others this is allowed and the program exits 1. */
        unsigned char gdtr[10];
        asm volatile("sgdt [%0]" : : "r"(gdtr) : "memory");
    } else if (strcmp(what, "int") == 0) {
        asm volatile("int 0x20");                  /* IDT gates are DPL 0: #GP */
    } else {
        printf("usage: fault cli|kread|kwrite|null|text|exec-stack|div0|sgdt|int\n");
        return 2;
    }

    printf("fault: '%s' was NOT stopped by the kernel\n", what);
    return 1;
}
