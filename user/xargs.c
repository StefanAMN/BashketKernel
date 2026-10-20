/* xargs: run a command with arguments read from standard input.
 *   xargs [-n N] [COMMAND [ARG...]]
 * Reads line_text (separated by blanks and newlines) and runs COMMAND ARG...
 * followed by as many of them as fit, again and again until the input
 * ends; with -n, at most N line_text each time. COMMAND is echo if not given,
 * and is looked up in $PATH as the shell does.
 *
 * "As many as fit": execve takes at most PROCESS_MAX_ARGS (32) arguments
 * and PROCESS_ARG_BYTES (4096) bytes of argument and environment strings
 * together (src/kernel/proc/process.h), so each command line is cut to
 * stay inside both. A word that cannot fit even on its own is an error.
 * Quotes and backslashes in the input are ordinary characters (POSIX
 * xargs gives them a meaning; this one does not). Nothing is run when the
 * input holds no line_text (as BSD's xargs does, and GNU's with -r).
 *
 * Status: 0 if every run succeeded; 123 if one ended with another
 * status; 126 or 127 (and no more runs) if COMMAND could not be run or
 * was not found; 1 for an over-long word or an error reading the input.
 */
#include "ulib.h"

#define MAX_ARGS  32          /* PROCESS_MAX_ARGS: argv[0] included */
#define ARG_BYTES 4096        /* PROCESS_ARG_BYTES: argument and environment strings, NULs included */

extern char **environ;

static char *argv_out[MAX_ARGS + 1];   /* The command line being built */
static int fixed_count;                /* COMMAND and its ARGs: the start of every line */
static int count;                      /* Words in argv_out now (fixed ones included) */
static char line_text[ARG_BYTES];      /* The words read for this line, each with its NUL */
static size_t line_used;
static size_t room;                    /* Bytes a line's words may use, after the fixed part and the environment */
static int max_words = MAX_ARGS;       /* -n */
static char pending[ARG_BYTES];        /* The word being read (not "word": with -masm=intel that is WORD, a size keyword) */
static size_t pending_len;
static int status;                     /* What xargs will exit with (see the top) */

/* In the child: run argv_out, looked up in $PATH unless it holds a '/'.
 * Never returns: exits 127 if it was not found, 126 if it could not run. */
static void run_child(void) {
    const char *name = argv_out[0];
    long err = -ENOENT;
    if (strchr(name, '/')) {
        err = execv(name, argv_out);
    } else {
        const char *dirs = getenv("PATH");
        if (!dirs) dirs = "/bin";
        char path[160];
        while (*dirs) {
            size_t len = 0;
            while (dirs[len] && dirs[len] != ':') len++;
            if (len > 0 && len + 1 + strlen(name) < sizeof(path)) {
                memcpy(path, dirs, len);
                path[len] = '/';
                strcpy(path + len + 1, name);
                long e = execv(path, argv_out);      /* Only returns on failure */
                if (e != -ENOENT) err = e;
            }
            dirs += len;
            if (*dirs == ':') dirs++;
        }
    }
    fprintf(2, "xargs: %s: %s\n", name, err == -ENOENT ? "not found" : strerror(err));
    exit(err == -ENOENT ? 127 : 126);
}

/* Run the command line built so far, wait for it, and start a new one.
 * Its status: what the command exited with (126 or 127 if it could not be
 * run at all), or -1 if fork failed. */
static int run_line(void) {
    argv_out[count] = NULL;
    long pid = fork();
    if (pid == 0) run_child();
    count = fixed_count;
    line_used = 0;
    if (pid < 0) {
        fprintf(2, "xargs: cannot fork: %s\n", strerror(pid));
        return -1;
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

/* Run the line built so far (run_line) and fold its result into
 * `status`. 0 if no more lines may be run (the command could not be
 * started: status is then 126, 127 or 1), 1 to go on. */
static int finish_line(void) {
    int s = run_line();
    if (s == 126 || s == 127 || s < 0) {
        status = s < 0 ? 1 : s;
        return 0;
    }
    if (s != 0) status = 123;
    return 1;
}

/* The word in pending[] has ended (at a blank or at the end of the input):
 * add it to the line, first running the line if it is full. Nothing
 * happens if no word was being read. 0 if no more lines may be run, as
 * for finish_line. */
static int end_word(void) {
    if (pending_len == 0) return 1;
    if (count - fixed_count >= max_words || count >= MAX_ARGS || line_used + pending_len + 1 > room) {
        if (!finish_line()) return 0;
    }
    memcpy(line_text + line_used, pending, pending_len);
    line_text[line_used + pending_len] = '\0';
    argv_out[count++] = line_text + line_used;
    line_used += pending_len + 1;
    pending_len = 0;
    return 1;
}

int main(int argc, char **argv) {
    int arg = 1;
    if (arg + 1 < argc && strcmp(argv[arg], "-n") == 0) {
        max_words = (int)atoi(argv[arg + 1]);
        arg += 2;
        if (max_words < 1) {
            fprintf(2, "usage: xargs [-n N] [COMMAND [ARG...]] (N at least 1)\n");
            return 2;
        }
    }

    /* The fixed part, and what it leaves of the byte limit */
    static char *echo_argv[] = { "echo", NULL };
    char **fixed = arg < argc ? argv + arg : echo_argv;
    size_t used = 0;
    for (char **e = environ; e && *e; e++) used += strlen(*e) + 1;
    for (fixed_count = 0; fixed[fixed_count]; fixed_count++) {
        if (fixed_count >= MAX_ARGS - 1) break;
        argv_out[fixed_count] = fixed[fixed_count];
        used += strlen(fixed[fixed_count]) + 1;
    }
    if (fixed[fixed_count] || used >= ARG_BYTES) {
        fprintf(2, "xargs: the command and its arguments leave no room for more\n");
        return 1;
    }
    room = ARG_BYTES - used;
    count = fixed_count;

    static char buf[1024];
    long n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
                if (!end_word()) return status;
            } else if (pending_len + 2 > room) {            /* The word, one more byte and its NUL */
                fprintf(2, "xargs: a word of more than %ld bytes cannot be passed on\n", (long)room - 1);
                return 1;
            } else {
                pending[pending_len++] = buf[i];
            }
        }
    }
    if (n < 0) {
        fprintf(2, "xargs: %s\n", strerror(n));
        return 1;
    }
    if (!end_word()) return status;

    if (count > fixed_count) finish_line();
    return status;
}
