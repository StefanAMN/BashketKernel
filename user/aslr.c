/* aslr: show where this program's code, stack and mapped memory are, and
 * check that they move from one execution to the next.
 *
 *   aslr          print the addresses, then run itself again and compare:
 *                 exit 0 if all three differ
 *
 * Address-space layout randomisation makes an attacker who has found a way to
 * redirect a program guess where anything is.
 */
#include "ulib.h"

/* Lower-case hex digits as a number; 0 if anything else is in the way */
static unsigned long parse_hex(const char *s) {
    unsigned long value = 0;
    for (; *s; s++) {
        unsigned digit = (*s >= '0' && *s <= '9') ? (unsigned)(*s - '0')
                       : (*s >= 'a' && *s <= 'f') ? (unsigned)(*s - 'a' + 10) : 16;
        if (digit == 16) return 0;
        value = value * 16 + digit;
    }
    return value;
}

/* A number as lower-case hex digits and a NUL (out has room for 17) */
static void to_hex(unsigned long value, char *out) {
    char digits[17];
    int n = 0;
    do { digits[n++] = "0123456789abcdef"[value & 15]; value >>= 4; } while (value);
    while (n) *out++ = digits[--n];
    *out = '\0';
}

int main(int argc, char **argv) {
    int on_stack = 0;
    unsigned long code = (unsigned long)main;
    unsigned long stack = (unsigned long)&on_stack;
    unsigned long mapped = (unsigned long)mmap_anon(4096);

    if (argc == 4) {
        /* Second execution: compare with what the first one saw */
        unsigned long first_code = parse_hex(argv[1]), first_stack = parse_hex(argv[2]), first_mapped = parse_hex(argv[3]);
        printf("second run: code %lx  stack %lx  mmap %lx\n", code, stack, mapped);
        int moved = (code != first_code) + (stack != first_stack) + (mapped != first_mapped);
        printf("%d of 3 regions moved\n", moved);
        /* The offset within a page cannot change (placement is by whole pages) */
        int same_offsets = ((code & 0xFFF) == (first_code & 0xFFF));
        return (moved == 3 && same_offsets) ? 0 : 1;
    }

    printf("first run:  code %lx  stack %lx  mmap %lx\n", code, stack, mapped);

    char a[20], b[20], c[20];
    to_hex(code, a);
    to_hex(stack, b);
    to_hex(mapped, c);
    char *args[] = { "/bin/aslr", a, b, c, NULL };
    long err = execv("/bin/aslr", args);
    fprintf(2, "aslr: cannot run myself again: %s\n", strerror(err));
    return 2;
}
