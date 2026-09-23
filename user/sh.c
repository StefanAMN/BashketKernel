/*
 * sh: a small command interpreter.
 *
 *   sh                 read commands from standard input until "exit"
 *   sh -c "command"    run one command line and exit with its status
 *   sh FILE            run the commands in FILE ('#' starts a comment)
 *
 * A command line is one or more pipelines separated by ';' or newlines (one
 * after the other), '&&' (the next only if this one succeeded) or '||'
 * (only if it failed). A pipeline is one or more programs joined by '|'.
 * Each may redirect its input with "< file" and its output with "> file"
 * or ">> file"; a digit in front names another descriptor ("2> errors",
 * "3< file"), and "N>&M" (or "N<&M") makes N a copy of M, so "2>&1"
 * sends errors where the output goes. Redirections are applied left to
 * right, after the pipe: "a 2>&1 | b" sends both to b, while
 * "a > f 2>&1" and "a 2>&1 > f" differ. A program name without a '/' is looked up in the
 * directories of $PATH.
 *
 * Compound commands, on one line or over several (the prompt asks for the
 * rest with "> "), nested as deep as wanted:
 *   if LIST; then LIST; [elif LIST; then LIST;] [else LIST;] fi
 *   while LIST; do LIST; done      (until LIST: as long as it fails)
 *   for NAME in WORDS; do LIST; done
 *   case WORD in PATTERN|PATTERN) LIST;; ... esac
 *   { LIST; }
 * with break [N] and continue [N] inside loops, and the built-in test
 * EXPR (or [ EXPR ]) for conditions: -e -f -d PATH, -z -n STRING, a = b,
 * a != b, x -eq -ne -lt -le -gt -ge y, and ! to turn one round. Ctrl-C
 * ends a loop as well as the command it was running. A case runs the list
 * of the first pattern (wildcards as for file names, but "*" also matches
 * ".x") that matches the word; quoted parts of a pattern match themselves.
 * A compound command may be redirected and piped like a program
 * ("for ...; done > f 2>&1", "{ a; b; } | sort", "echo x | while ...");
 * it then runs in a child of the shell, so what it sets stays there. At
 * the terminal it may also end with '&' ("while ...; done &"): a job.
 *
 * Here-documents: "cat <<WORD" gives the command the lines after its own
 * line, up to one that is WORD alone, as its input ("<<-WORD" takes the
 * tabs off the start of each). They are expanded ($NAME, $(...), $((...)),
 * and \ before $, `, \ and a newline) unless some part of WORD is quoted
 * ('EOF', "EOF", \EOF). At the prompt the lines are asked for with "> ".
 * Several may follow one line, in the order of their "<<".
 *
 * Functions: "NAME() { LIST; }" defines one, and NAME then runs as a
 * command, with its words as $1 .. $9 (${10} on), $# and $@ ("$@" gives
 * one word per argument); "return [N]" ends it. A function belongs to the
 * shell (programs do not see it); alone on its line it runs in the shell
 * itself and can set its variables, in a pipeline or with a redirection in
 * a child. Calls may nest 64 deep; deeper ones abandon the command.
 *
 * A word may join plain, "double quoted" ($ expanded) and 'single quoted'
 * (taken as it is) parts. $(command) is replaced by what the command
 * prints: one word inside double quotes, split at blanks outside them. A
 * word with *, ? or [...] and no quoted part is replaced by the names it
 * matches, sorted (user/lib/glob.h); one that matches nothing stays as
 * typed.
 * $NAME (or ${NAME}, to put letters after it) expands to a variable and
 * $? to the last exit status; $0 is "sh" or the script. "NAME=value"
 * on its own sets a variable of the shell, which programs do not see;
 * "export NAME=value" (or "export NAME" for one already set) puts it in
 * the environment, which the shell and everything it starts share; "unset
 * NAME" forgets it. "NAME=value program" sets it in that program's
 * environment only. A value is expanded like any word, but is neither
 * split nor matched against file names: X=$(ls) keeps the newlines, X=*
 * a star. A for loop's variable is the shell's own.
 *
 * $((EXPRESSION)) is replaced by its value: 64-bit integers with + - * / %,
 * < <= > >= == !=, && || !, parentheses, and names of variables holding
 * numbers (unset: 0); see "Arithmetic" below. "read [-r] NAME..." sets
 * the names from one line of input, split at blanks (the last takes the
 * rest); its status is 1 at the end of the input, so
 * "while read line; do ...; done < file" goes through a file.
 *
 * Ctrl-C ends the running programs and returns to the prompt; Ctrl-D at an
 * empty prompt leaves the shell. At the terminal the line can be edited
 * (the arrow keys, Home, End, Delete, Ctrl-A, -E, -K, -U, -W) and Up and
 * Down go through the lines entered before. Tab completes a program name
 * or a file name; a second Tab lists the choices.
 *
 * Jobs (only when reading commands from the terminal): a line ending in '&'
 * runs in the background, and Ctrl-Z stops the running command and returns
 * to the prompt. "jobs" lists them, "fg [N]" brings one back to the
 * terminal, "bg [N]" lets a stopped one carry on in the background.
 *
 * Everything here is done with fork, execve, waitpid, pipe, dup2, open and
 * close, plus, for jobs, process groups: each command line runs in a group
 * of its own, and the shell tells the terminal which group Ctrl-C and
 * Ctrl-Z are meant for.
 */
#include "ulib.h"
#include "lineedit.h"
#include "glob.h"

#define MAX_ARGS      64
#define MAX_STAGES    8
#define MAX_REDIRECTS 8
#define MAX_ASSIGNMENTS 8

enum { REDIRECT_IN, REDIRECT_OUT, REDIRECT_APPEND, REDIRECT_COPY, REDIRECT_HEREDOC };

typedef struct {
    int kind;           /* REDIRECT_* */
    int fd;             /* The descriptor it changes */
    int from;           /* REDIRECT_COPY: the descriptor copied */
    char *path;         /* REDIRECT_HEREDOC: the body (see heredoc_open); the others: the file opened */
} redirect_t;

typedef struct {
    char *argv[MAX_ARGS + 1];
    int argc;
    redirect_t redirects[MAX_REDIRECTS];  /* In the order written: the order matters */
    int redirect_count;
    char *assignments[MAX_ASSIGNMENTS];   /* "NAME=value" words before the program's name */
    int assignment_count;
} stage_t;

/* Words are copied here while parsing, each followed by a NUL. Copying
 * (instead of cutting the input line up in place) keeps "a|b" simple: there
 * is no room in the line itself for a terminator between 'a' and '|'. */
static char word_storage[4096];
static int last_status = 0;   /* Exit status of the previous command, for $? */
static int script_fd = -1;    /* The script being run, or -1 when reading the terminal */

/* --- Jobs ------------------------------------------------------------------- */

#define MAX_JOBS 8

typedef struct {
    int used;
    long pids[8];               /* One per program; the first is also the number of the process group */
    int alive[8];
    int count;
    int stopped;
    char text[64];              /* The command line, for "jobs" */
} job_t;

static job_t jobs[MAX_JOBS];
static int interactive = 0;     /* Reading the terminal: job control is on */
static long shell_group = 0;

/* Record a job (a stopped or background pipeline) of `count` processes,
 * `text` being its command line. Returns its job number, or 0 if the table
 * is full. */
static int job_add(const long *pids, const int *alive, int count, const char *text, int stopped) {
    for (int n = 0; n < MAX_JOBS; n++) {
        if (jobs[n].used) continue;
        memset(&jobs[n], 0, sizeof(jobs[n]));
        jobs[n].used = 1;
        jobs[n].count = count;
        jobs[n].stopped = stopped;
        for (int i = 0; i < count; i++) {
            jobs[n].pids[i] = pids[i];
            jobs[n].alive[i] = alive ? alive[i] : 1;
        }
        size_t len = strlen(text);
        if (len >= sizeof(jobs[n].text)) len = sizeof(jobs[n].text) - 1;
        memcpy(jobs[n].text, text, len);
        while (len > 0 && (jobs[n].text[len - 1] == ' ' || jobs[n].text[len - 1] == '\n')) len--;
        jobs[n].text[len] = '\0';
        return n + 1;
    }
    return 0;
}

/* Wait for a command that has the terminal: until every process of it has
 * exited, or it has been stopped. Returns the last program's status. */
static int wait_foreground(const long *pids, int *alive, int count, int *stopped) {
    int last = 1;
    *stopped = 0;
    for (int i = 0; i < count; i++) {
        if (!alive[i]) continue;
        int status = 0;
        long r;
        while ((r = waitpid(pids[i], &status, interactive ? WUNTRACED : 0)) == -EINTR) { } /* Interrupted is not finished */
        if (r == pids[i] && WIFSTOPPED(status)) {
            *stopped = 1;
            continue;
        }
        alive[i] = 0;
        if (i == count - 1) last = status;
    }
    return last;
}

/* Before each prompt: see what has become of the background jobs */
static void check_jobs(void) {
    for (int n = 0; n < MAX_JOBS; n++) {
        job_t *job = &jobs[n];
        if (!job->used) continue;
        int living = 0;
        for (int i = 0; i < job->count; i++) {
            if (!job->alive[i]) continue;
            int status = 0;
            long r = waitpid(job->pids[i], &status, WNOHANG | WUNTRACED);
            if (r == job->pids[i] && WIFSTOPPED(status)) {
                if (!job->stopped) printf("[%d] Stopped   %s\n", n + 1, job->text);
                job->stopped = 1;
            } else if (r != 0) {
                job->alive[i] = 0;                /* Exited (or no longer ours to wait for) */
            }
            living += job->alive[i];
        }
        if (living == 0) {
            printf("[%d] Done      %s\n", n + 1, job->text);
            job->used = 0;
        }
    }
}

/* The job a "fg" or "bg" means: the one numbered, or the newest */
static job_t *pick_job(const char *arg, int *number) {
    int n = arg ? (int)atoi(arg[0] == '%' ? arg + 1 : arg) : 0;
    if (!arg) {
        for (int i = MAX_JOBS; i >= 1 && n == 0; i--) if (jobs[i - 1].used) n = i;
    }
    if (n < 1 || n > MAX_JOBS || !jobs[n - 1].used) return NULL;
    *number = n;
    return &jobs[n - 1];
}

/* Give a command the terminal and wait for it. If it gets stopped it
 * becomes (or stays) a job; returns the last program's status. */
static int run_in_foreground(long *pids, int *alive, int count, const char *text, job_t *existing) {
    int stopped = 0;
    if (interactive) tcsetpgrp(pids[0]);
    int status = wait_foreground(pids, alive, count, &stopped);
    if (interactive) tcsetpgrp(shell_group);

    if (stopped) {
        int n;
        if (existing) {
            existing->stopped = 1;
            n = (int)(existing - jobs) + 1;
        } else {
            n = job_add(pids, alive, count, text, 1);
        }
        if (n > 0) {
            printf("\n[%d] Stopped   %s\n", n, jobs[n - 1].text);
        } else {
            /* No room to remember it: it can not be left stopped for ever */
            fprintf(2, "\nsh: too many jobs: ending %s\n", text);
            kill(-pids[0], SIGKILL);
            wait_foreground(pids, alive, count, &stopped);
        }
        return 128 + SIGTSTP;
    }
    if (existing) existing->used = 0;
    return status;
}

/* Character classes of the command line */
static int is_operator(char c) { return c == '|' || c == '<' || c == '>'; }
static int is_blank(char c)    { return c == ' ' || c == '\t'; }

/* Copy the next word from *cursor into word_storage. Handles "double quotes".
 * Returns the copy, or NULL if there is no word here or no room left. */
/* (A character of a variable's name, for $NAME) */
static int is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* --- Variables ---------------------------------------------------------------
 *
 * A variable is either the shell's own (set by NAME=value, seen by $NAME
 * and nobody else) or in the environment (export, and passed to every
 * program). A name is in one place at most: setting an exported variable
 * changes the environment, and export moves a variable there. */

#define MAX_VARIABLES 64
static char *variables[MAX_VARIABLES];   /* "NAME=value", malloc'd; NULL: a free entry */

/* Whether `text` up to `end` (or its NUL, whichever is first) is a variable
 * name: a letter or '_', then letters, digits and '_'. ("1x=y" is a
 * command, not an assignment.) */
static int is_name(const char *text, const char *end) {
    if (text == end || !((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z') || *text == '_')) return 0;
    for (; *text && text != end; text++) {
        if (!is_name_char(*text)) return 0;
    }
    return 1;
}

/* The shell's own entry for `name`, or NULL */
static char **variable_entry(const char *name) {
    size_t len = strlen(name);
    for (int i = 0; i < MAX_VARIABLES; i++) {
        if (variables[i] && strncmp(variables[i], name, len) == 0 && variables[i][len] == '=') return &variables[i];
    }
    return NULL;
}

/* The value of `name`: the shell's own variable, else the environment's;
 * NULL if it is set in neither */
static const char *get_variable(const char *name) {
    char **entry = variable_entry(name);
    if (entry) return *entry + strlen(name) + 1;
    return getenv(name);
}

/* Set `name` to `value`: in the environment if it is exported, else as the
 * shell's own. 0, or -1 (a message printed) if out of memory or room. */
static int set_variable(const char *name, const char *value) {
    if (getenv(name)) return setenv(name, value);
    size_t name_len = strlen(name), value_len = strlen(value);
    char *text = malloc(name_len + value_len + 2);
    if (!text) return -1;
    memcpy(text, name, name_len);
    text[name_len] = '=';
    memcpy(text + name_len + 1, value, value_len + 1);
    char **entry = variable_entry(name);
    for (int i = 0; !entry && i < MAX_VARIABLES; i++) if (!variables[i]) entry = &variables[i];
    if (!entry) {
        free(text);
        fprintf(2, "sh: %s: too many variables\n", name);
        return -1;
    }
    free(*entry);
    *entry = text;
    return 0;
}

/* Forget `name`, both the shell's own and the environment's */
static void unset_variable(const char *name) {
    char **entry = variable_entry(name);
    if (entry) {
        free(*entry);
        *entry = NULL;
    }
    unsetenv(name);
}

/* Move the shell's own variable `name` into the environment (nothing to
 * do if it has none). 0, or -1 if out of memory. */
static int export_variable(const char *name) {
    char **entry = variable_entry(name);
    if (!entry) return 0;
    if (setenv(name, *entry + strlen(name) + 1) != 0) return -1;
    free(*entry);
    *entry = NULL;
    return 0;
}

/* The arguments of the function being run ($1 .. $9, $#, $@; at the top
 * level none, or a script's own with "sh FILE ARGS"), and $0 */
static char **arguments;
static int argument_count;
static const char *shell_name = "sh";

#define FIELD_BREAK '\x1f'    /* Where unquoted $(...) output had blanks (0x1f in output counts as one) */

/* Set while next_word reads NAME=value: the value is one word whatever a
 * $(...) in it prints, as in other Bourne shells (X=$(ls) keeps the
 * newlines) */
static int reading_assignment = 0;
/* Set while next_word reads a case pattern: what was quoted must match
 * itself, so a quoted *, ? or [ is written in brackets ("[*]") */
static int reading_pattern = 0;

/* Add character `c` to `word` (length *len, in word_storage from `used`);
 * if `literal` (it was quoted) while a pattern is read, a wildcard
 * character goes in as "[c]". 0, or -1 if it does not fit. */
static int put_char(char *word, size_t *len, size_t used, char c, int literal) {
    int bracket = literal && reading_pattern && (c == '*' || c == '?' || c == '[');
    if (used + *len + (bracket ? 3 : 1) >= sizeof(word_storage)) return -1;
    if (bracket) word[(*len)++] = '[';
    word[(*len)++] = c;
    if (bracket) word[(*len)++] = ']';
    return 0;
}

/* `v` (not negative) in decimal into text[12] */
static void number_text(int v, char text[12]) {
    char tmp[12];
    int digits = 0;
    do { tmp[digits++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
    for (int i = 0; i < digits; i++) text[i] = tmp[digits - 1 - i];
    text[digits] = '\0';
}

/* Append the value of the variable at *p (*p just after the '$') to `word`
 * (length *len, in word_storage from `used`), and move *p past it: $NAME,
 * ${NAME}, $? (the last status), $1 .. $9 and ${N} (the arguments; $0 the
 * shell), $# (how many) and $@ (all of them). `quote` is the quote the
 * '$' is inside (0 if none). $@ gives one word per argument, quoted or
 * not, by putting FIELD_BREAKs between them and setting *split (in an
 * assignment or a pattern, a blank instead). An unset variable is
 * nothing. 0; -1 if it does not fit, or "${" has no proper name and '}'. */
static int append_variable(const char **p, char *word, size_t *len, size_t used, char quote, int *split) {
    char name[32], text[12];
    const char *value = NULL;
    size_t n = 0;
    int braced = (**p == '{');
    if (braced) (*p)++;
    char special = 0;
    if (**p == '?' || **p == '#' || **p == '@') {
        special = *(*p)++;
    } else if (**p >= '0' && **p <= '9') {
        special = '1';                     /* $1: one digit; ${12}: all of them */
        do name[n++] = *(*p)++; while (braced && **p >= '0' && **p <= '9' && n < sizeof(name) - 1);
    } else {
        while (is_name_char(**p) && n < sizeof(name) - 1) name[n++] = *(*p)++;
    }
    name[n] = '\0';
    if (braced) {
        if (**p != '}' || (!special && n == 0)) return -1;   /* "${}", "${a-b}", no '}' */
        (*p)++;
    }

    if (special == '?') {
        number_text(last_status, text);
        value = text;
    } else if (special == '#') {
        number_text(argument_count, text);
        value = text;
    } else if (special == '1') {
        int index = atoi(name);
        if (index == 0) value = shell_name;
        else if (index <= argument_count) value = arguments[index - 1];
    } else if (special == '@') {
        int joined = reading_assignment || reading_pattern;
        if (!joined) *split = 1;
        for (int i = 0; i < argument_count; i++) {
            if (i > 0 && put_char(word, len, used, joined ? ' ' : FIELD_BREAK, 0) < 0) return -1;
            for (const char *c = arguments[i]; *c; c++) {
                if (put_char(word, len, used, *c, quote != 0) < 0) return -1;
            }
        }
        return 0;
    } else {
        value = get_variable(name);
    }
    for (; value && *value; value++) {
        if (put_char(word, len, used, *value, quote != 0) < 0) return -1;
    }
    return 0;
}

static int run_line(const char *line, int *should_exit);
static int is_function(const char *name);
static int call_function(int argc, char **argv, int *should_exit);
static int read_builtin(int argc, char **argv);

#define WORD_QUOTED 1         /* next_word: some part was in quotes */
#define WORD_SPLIT  2         /* next_word: holds FIELD_BREAKs, to be split into words */
#define SUBST_MAX   4096      /* Most output a $(...) may give */

static unsigned substitutions_run = 0;   /* $(...)s run so far: did a command line have one? */

/* The ')' that closes the "$(" just before `p`, skipping nested ones and
 * quoted text; NULL if there is none */
static const char *closing_paren(const char *p) {
    int depth = 1;
    char quote = 0;
    for (; *p; p++) {
        if (quote) { if (*p == quote) quote = 0; continue; }
        if (*p == '"' || *p == '\'') quote = *p;
        else if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) return p;
    }
    return NULL;
}

/* $(command): run the command line in a child of this shell, its output
 * into a pipe, and collect it into out[size] without the trailing newlines.
 * $? becomes its status. Returns the length, or -1 (a message printed) if
 * it cannot be run or prints more than size - 1 bytes. */
static long substitute(const char *command, char *out, size_t size) {
    int p[2];
    if (pipe(p) != 0) return -1;
    long pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        interactive = 0;                      /* The child runs the command, not jobs */
        reading_assignment = reading_pattern = 0;   /* (Its own words split as usual) */
        int should_exit = 0;
        exit(run_line(command, &should_exit));
    }
    close(p[1]);
    size_t used = 0;
    int too_long = 0;
    for (;;) {
        char buf[256];
        long n = read(p[0], buf, sizeof(buf));
        if (n == -EINTR) continue;
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {        /* (Read to the end even when too long, so the child finishes) */
            if (used + 1 < size) out[used++] = buf[i];
            else too_long = 1;
        }
    }
    close(p[0]);
    int status = -1;
    while (waitpid(pid, &status, 0) == -EINTR) { }
    last_status = status;
    substitutions_run++;
    if (too_long) {
        fprintf(2, "sh: $(%s): more than %lu bytes of output\n", command, (unsigned long)(size - 1));
        return -1;
    }
    while (used > 0 && out[used - 1] == '\n') used--;
    out[used] = '\0';
    return (long)used;
}

/* --- Arithmetic: $((...)) ---------------------------------------------------
 *
 * 64-bit signed integers, with C's operators and precedence for the ones
 * there are: unary ! - +, then * / %, + -, < <= > >=, == !=, &&, ||, and
 * parentheses. A name is a variable holding a number (unset or empty: 0);
 * $NAME, ${NAME}, $?, $# and $1.. work too. Constants are decimal, octal
 * (a leading 0) or hexadecimal (0x), as in C. Overflow wraps around as
 * two's complement (unlike C, where it is undefined); division by zero is
 * an error. && and || do not evaluate their right side when the left
 * decides, so "x != 0 && 10 / x" is safe. Not done: assignments (=, +=,
 * ++), the bit operators, ?:, and $(...) inside. */

typedef struct {
    const char *p;            /* Where the parser is */
    const char *error;        /* The first error met, or NULL */
} arith_t;

/* Skip blanks and newlines in the expression */
static void arith_blanks(arith_t *a) {
    while (is_blank(*a->p) || *a->p == '\n') a->p++;
}

/* Read an unsigned integer constant at *p (C's forms: decimal, 0 octal, 0x
 * hex; 2^63 and above wrap to negative) into *value, and move *p past it.
 * 0, or -1 if there is none, it has a wrong digit or letters after it, or
 * it does not fit in 64 bits. */
static int arith_constant(const char **p, long *value) {
    const char *c = *p;
    unsigned long base = 10, v = 0;
    if (c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) { base = 16; c += 2; }
    else if (c[0] == '0') base = 8;
    const char *digits = c;
    for (;; c++) {
        unsigned long d;
        if (*c >= '0' && *c <= '9') d = (unsigned long)(*c - '0');
        else if (*c >= 'a' && *c <= 'f') d = (unsigned long)(*c - 'a' + 10);
        else if (*c >= 'A' && *c <= 'F') d = (unsigned long)(*c - 'A' + 10);
        else break;
        if (d >= base) return -1;                         /* "08", "1a" */
        if (v > (~0UL - d) / base) return -1;             /* More than 64 bits */
        v = v * base + d;
    }
    if (c == digits && base != 8) return -1;              /* "0x" alone; nothing */
    if (is_name_char(*c)) return -1;                      /* "12abc" */
    *p = c;
    *value = (long)v;
    return 0;
}

/* The number a variable holds, `text` (blanks around it allowed, a sign
 * before it; empty or NULL is 0); 0 and an error in `a` naming `what` if
 * it is not a number */
static long arith_variable(arith_t *a, const char *text, const char *what) {
    if (!text) return 0;
    while (is_blank(*text)) text++;
    if (*text == '\0') return 0;
    int negative = (*text == '-');
    if (*text == '-' || *text == '+') text++;
    long v;
    if (arith_constant(&text, &v) == 0) {
        while (is_blank(*text)) text++;
        if (*text == '\0') return negative ? (long)(0UL - (unsigned long)v) : v;
    }
    if (!a->error) {
        static char message[64];
        size_t n = strlen(what);
        if (n > 40) n = 40;
        memcpy(message, what, n);
        strcpy(message + n, ": not a number");
        a->error = message;
    }
    return 0;
}

static long arith_or(arith_t *a, int live);

/* A number, a variable, or ( expression ). With `live` 0 (the side of an
 * && or || that does not count) nothing is reported. */
static long arith_primary(arith_t *a, int live) {
    arith_blanks(a);
    const char *c = a->p;
    if (*c == '(') {
        a->p++;
        long v = arith_or(a, live);
        arith_blanks(a);
        if (*a->p != ')') { if (!a->error) a->error = "')' expected"; return 0; }
        a->p++;
        return v;
    }
    if (*c >= '0' && *c <= '9') {
        long v;
        if (arith_constant(&a->p, &v) < 0) { if (!a->error) a->error = "bad number"; return 0; }
        return v;
    }
    char name[32];
    size_t n = 0;
    if (*c == '$') {
        c++;
        int braced = (*c == '{');
        if (braced) c++;
        long special = -1;
        if (*c == '?') { special = last_status; c++; }
        else if (*c == '#') { special = argument_count; c++; }
        else if (*c >= '0' && *c <= '9') {
            int index = *c++ - '0';
            while (braced && *c >= '0' && *c <= '9' && index < 1000) index = index * 10 + (*c++ - '0');
            const char *text = index >= 1 && index <= argument_count ? arguments[index - 1] : NULL;
            if (braced && *c++ != '}') { if (!a->error) a->error = "bad ${"; return 0; }
            a->p = c;
            return index == 0 || !live ? 0 : arith_variable(a, text, "argument");
        }
        if (special >= 0) {
            if (braced && *c++ != '}') { if (!a->error) a->error = "bad ${"; return 0; }
            a->p = c;
            return special;
        }
        while (is_name_char(*c) && n < sizeof(name) - 1) name[n++] = *c++;
        if (n == 0 || (braced && *c++ != '}')) {
            if (!a->error) a->error = *c == '(' ? "$(...) is not supported inside $((...))" : "bad $";
            return 0;
        }
    } else if (is_name_char(*c)) {
        while (is_name_char(*c) && n < sizeof(name) - 1) name[n++] = *c++;
    } else {
        if (!a->error) a->error = *c ? "a number or a name expected" : "the expression ends too early";
        return 0;
    }
    name[n] = '\0';
    a->p = c;
    if (!is_name(name, NULL)) { if (!a->error) a->error = "bad name"; return 0; }
    return live ? arith_variable(a, get_variable(name), name) : 0;   /* (Not a number: only an error where it counts) */
}

/* ! - + in front (any number of them) */
static long arith_unary(arith_t *a, int live) {
    arith_blanks(a);
    char op = *a->p;
    if (op == '!' && a->p[1] != '=') { a->p++; return !arith_unary(a, live); }
    if (op == '-' || op == '+') {
        a->p++;
        unsigned long v = (unsigned long)arith_unary(a, live);
        return op == '-' ? (long)(0UL - v) : (long)v;
    }
    return arith_primary(a, live);
}

/* * / % (left to right). Division by zero is an error where it counts;
 * the one quotient that overflows (the most negative number / -1) wraps. */
static long arith_mul(arith_t *a, int live) {
    long v = arith_unary(a, live);
    for (;;) {
        arith_blanks(a);
        char op = *a->p;
        if (op != '*' && op != '/' && op != '%') return v;
        a->p++;
        long w = arith_unary(a, live);
        if (op == '*') {
            v = (long)((unsigned long)v * (unsigned long)w);
        } else if (w == 0) {
            if (live && !a->error) a->error = "division by zero";
            v = 0;
        } else if (w == -1) {
            v = op == '/' ? (long)(0UL - (unsigned long)v) : 0;   /* (No trap for LONG_MIN / -1) */
        } else {
            v = op == '/' ? v / w : v % w;
        }
    }
}

/* + - (left to right, wrapping) */
static long arith_add(arith_t *a, int live) {
    long v = arith_mul(a, live);
    for (;;) {
        arith_blanks(a);
        char op = *a->p;
        if (op != '+' && op != '-') return v;
        a->p++;
        unsigned long w = (unsigned long)arith_mul(a, live);
        v = (long)(op == '+' ? (unsigned long)v + w : (unsigned long)v - w);
    }
}

/* < <= > >= : 1 or 0 */
static long arith_compare(arith_t *a, int live) {
    long v = arith_add(a, live);
    for (;;) {
        arith_blanks(a);
        if (*a->p != '<' && *a->p != '>') return v;
        int less = (*a->p == '<'), equal = (a->p[1] == '=');
        a->p += equal ? 2 : 1;
        long w = arith_add(a, live);
        v = less ? (equal ? v <= w : v < w) : (equal ? v >= w : v > w);
    }
}

/* == != : 1 or 0 */
static long arith_equal(arith_t *a, int live) {
    long v = arith_compare(a, live);
    for (;;) {
        arith_blanks(a);
        if (!((a->p[0] == '=' || a->p[0] == '!') && a->p[1] == '=')) return v;
        int same = (a->p[0] == '=');
        a->p += 2;
        long w = arith_compare(a, live);
        v = same ? v == w : v != w;
    }
}

/* && : 1 or 0; the right side counts only if the left is not 0 */
static long arith_and(arith_t *a, int live) {
    long v = arith_equal(a, live);
    for (;;) {
        arith_blanks(a);
        if (!(a->p[0] == '&' && a->p[1] == '&')) return v;
        a->p += 2;
        long w = arith_equal(a, live && v != 0);
        v = v != 0 && w != 0;
    }
}

/* || : 1 or 0; the right side counts only if the left is 0 */
static long arith_or(arith_t *a, int live) {
    long v = arith_and(a, live);
    for (;;) {
        arith_blanks(a);
        if (!(a->p[0] == '|' && a->p[1] == '|')) return v;
        a->p += 2;
        long w = arith_and(a, live && v == 0);
        v = v != 0 || w != 0;
    }
}

static int arith_failed;      /* A $((...)) failed and said why: no "syntax error" on top */

/* Evaluate `expr` into *value. 0, or -1 with a message printed (and
 * arith_failed set). */
static int arith_eval(const char *expr, long *value) {
    arith_t a = { expr, NULL };
    *value = arith_or(&a, 1);
    arith_blanks(&a);
    if (!a.error && *a.p) a.error = "unexpected text";
    if (a.error) {
        fprintf(2, "sh: $((%s)): %s\n", expr, a.error);
        arith_failed = 1;
        return -1;
    }
    return 0;
}

/* The "))" that ends the "$((" just before `p`: the first ')' outside
 * parentheses opened since, which must be followed by another. NULL if
 * there is none. */
static const char *arith_end(const char *p) {
    int depth = 0;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')' && depth > 0) depth--;
        else if (*p == ')') return p[1] == ')' ? p : NULL;
    }
    return NULL;
}

/* `v` in decimal (with a '-' if negative) into text[24] */
static void long_text(long v, char text[24]) {
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    char tmp[24];
    int digits = 0;
    do { tmp[digits++] = (char)('0' + u % 10); u /= 10; } while (u > 0);
    int n = 0;
    if (v < 0) text[n++] = '-';
    while (digits > 0) text[n++] = tmp[--digits];
    text[n] = '\0';
}

/* The next word of the line at *cursor, stored in word_storage from *used
 * on; moves *cursor past it. A word runs to a blank or an operator outside
 * quotes, and may be made of parts: plain text, "double quoted" ($NAME and
 * $? expanded, blanks and operators kept) and 'single quoted' (everything
 * kept as it is): `"a b"c` is one word, `a b c`. NULL if no word starts
 * here (the end of the line, an operator), a quote is not closed, or the
 * line's words do not fit in word_storage (or a "${" is malformed).
 * Reading a case pattern, quoted wildcard characters are bracketed (see
 * put_char). $(command) is replaced by the
 * command's output: as it is inside double quotes, and outside them with
 * its blanks as FIELD_BREAKs, at which the caller splits the word. *flags
 * (if not NULL) receives WORD_QUOTED (any part was in quotes: not a
 * wildcard pattern) and WORD_SPLIT. */
static char *next_word(const char **cursor, size_t *used, int *flags) {
    const char *p = *cursor;
    while (is_blank(*p)) p++;

    char *word = word_storage + *used;
    size_t len = 0;
    char quote = 0;                   /* The quote we are inside, or 0 */
    int quoted = 0, split = 0, any = 0;
    for (;;) {
        char c = *p;
        if (c == '\0') {
            if (quote) return NULL;   /* Missing closing quote */
            break;
        }
        if (!quote && (is_blank(c) || is_operator(c))) break;
        any = 1;
        if (!quote && (c == '"' || c == '\'')) {
            quote = c;
            quoted = 1;
            p++;
        } else if (quote && c == quote) {
            quote = 0;
            p++;
        } else if (c == '$' && quote != '\'' && p[1] == '(' && p[2] == '(') {
            /* $((expression)): its value, one word (a number has no blanks) */
            const char *end = arith_end(p + 3);
            char expr[256];
            if (!end || (size_t)(end - (p + 3)) >= sizeof(expr)) return NULL;
            memcpy(expr, p + 3, (size_t)(end - (p + 3)));
            expr[end - (p + 3)] = '\0';
            p = end + 2;
            long value;
            char text[24];
            if (arith_eval(expr, &value) < 0) return NULL;
            long_text(value, text);
            for (const char *t = text; *t; t++) {
                if (put_char(word, &len, *used, *t, quote != 0) < 0) return NULL;
            }
        } else if (c == '$' && quote != '\'' && p[1] == '(') {
            const char *end = closing_paren(p + 2);
            char command[256];
            if (!end || (size_t)(end - (p + 2)) >= sizeof(command)) return NULL;
            memcpy(command, p + 2, (size_t)(end - (p + 2)));
            command[end - (p + 2)] = '\0';
            p = end + 1;
            static char output[SUBST_MAX + 1];
            long n = substitute(command, output, sizeof(output));
            if (n < 0) return NULL;
            int whole = quote || reading_assignment;   /* Taken as it is: no splitting */
            for (long i = 0; i < n; i++) {
                char o = output[i];
                if (!whole && (is_blank(o) || o == '\n' || o == FIELD_BREAK)) o = FIELD_BREAK;
                if (put_char(word, &len, *used, o, quote != 0) < 0) return NULL;
            }
            if (!whole) split = 1;
        } else if (c == '$' && quote != '\'' &&
                   (is_name_char(p[1]) || p[1] == '?' || p[1] == '#' || p[1] == '@' || p[1] == '{')) {
            p++;
            if (append_variable(&p, word, &len, *used, quote, &split) < 0) return NULL;
        } else {
            if (put_char(word, &len, *used, c, quote != 0) < 0) return NULL;
            p++;
        }
    }
    if (!any) return NULL;            /* No word here at all ("" is one: empty) */
    if (flags) *flags = (quoted ? WORD_QUOTED : 0) | (split ? WORD_SPLIT : 0);

    word[len] = '\0';
    *used += len + 1;
    *cursor = p;
    return word;
}

/* Replace the wildcard word `pattern` (the last word in word_storage,
 * ending at *used) by the names it matches, sorted, appended to the
 * stage's arguments. Only the last part of a path may have wildcards:
 * "/tmp/notes?" looks in /tmp. No match, or wildcards in the directory part:
 * the word stays as it is (as in POSIX sh). Returns 0, or -1 if the names
 * do not fit (MAX_ARGS, word_storage). */
static int expand_wildcard(char *pattern, stage_t *stage, size_t *used) {
    char *slash = NULL;
    for (char *c = pattern; *c; c++) if (*c == '/') slash = c;
    const char *base = slash ? slash + 1 : pattern;
    size_t dir_len = slash ? (size_t)(slash - pattern) + 1 : 0;  /* Kept with its '/' */
    char dir[256];
    if (dir_len >= sizeof(dir)) return -1;
    memcpy(dir, pattern, dir_len);
    dir[dir_len] = '\0';
    if (glob_has_wildcard(dir)) {
        stage->argv[stage->argc++] = pattern;
        return 0;
    }

    /* The pattern stays where it is while the names are added after it;
     * then the names move down over it */
    size_t pattern_at = (size_t)(pattern - word_storage);
    int first = stage->argc;
    long fd = open(dir_len ? dir : ".", O_RDONLY);
    struct dirent entry;
    while (fd >= 0 && readdir((int)fd, &entry) == 1) {
        if (!glob_match(base, entry.name)) continue;
        size_t n = strlen(entry.name);
        if (stage->argc >= MAX_ARGS || *used + dir_len + n + 1 > sizeof(word_storage)) {
            close((int)fd);
            return -1;
        }
        char *word = word_storage + *used;
        memcpy(word, dir, dir_len);
        memcpy(word + dir_len, entry.name, n + 1);
        *used += dir_len + n + 1;
        /* Sorted as they come: insert before the first that sorts after it */
        int at = stage->argc++;
        while (at > first && strcmp(stage->argv[at - 1], word) > 0) {
            stage->argv[at] = stage->argv[at - 1];
            at--;
        }
        stage->argv[at] = word;
    }
    if (fd >= 0) close((int)fd);
    if (stage->argc == first) {                      /* Nothing matched */
        stage->argv[stage->argc++] = pattern;
        return 0;
    }
    /* Close the gap left by the pattern: every name after it moves back */
    size_t gap = strlen(pattern) + 1;
    for (size_t i = pattern_at; i + gap < *used; i++) word_storage[i] = word_storage[i + gap]; /* (Forwards: safe) */
    *used -= gap;
    for (int i = first; i < stage->argc; i++) stage->argv[i] -= gap;
    return 0;
}

/* Add the parts of `word` between FIELD_BREAKs to the stage's arguments,
 * leaving out empty ones (an unquoted $(...) that printed nothing gives no
 * argument at all). 0, or -1 if there are more than MAX_ARGS. */
static int add_fields(char *word, stage_t *stage) {
    char *start = word;
    for (char *c = word;; c++) {
        if (*c != FIELD_BREAK && *c != '\0') continue;
        int last = (*c == '\0');
        *c = '\0';
        if (*start) {
            if (stage->argc >= MAX_ARGS) return -1;
            stage->argv[stage->argc++] = start;
        }
        if (last) return 0;
        start = c + 1;
    }
}

/* --- Here-documents --------------------------------------------------------
 *
 * "cat <<WORD" takes the lines that follow the command, up to a line that
 * is WORD alone, as the command's input ("<<-WORD": leading tabs are taken
 * off every line, WORD's included). Unless some part of WORD was quoted,
 * the lines are expanded when the command runs: $NAME and the other
 * variables, $(...), $((...)), and a backslash before $, `, \ or a
 * newline. cut_segments takes the lines out of the command text while it
 * cuts it, leaving "<\x02" and a letter in place of the redirection; the
 * node made from the text keeps the bodies (node->docs), and the letter
 * says which. A body is stored with a first character saying how it is
 * taken: '\'' as it is, '"' expanded. */

#define HEREDOC_MARK   '\x02'
#define HEREDOC_DIRECT 4096    /* The kernel's pipe holds 4 KiB (src/fs/pipe.h): a body this size is written at once */

/* The bodies of the node whose command is being parsed and run
 * (parse_redirect looks the letters up here), or NULL */
static char *const *active_docs;
static int active_doc_count;

/* Text being built, malloc'd and growing */
typedef struct {
    char *s;
    size_t len, cap;
} text_t;

/* Add `n` bytes at `from` to `t`. 0, or -1 if memory ran out. */
static int text_add(text_t *t, const char *from, size_t n) {
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 256;
        while (t->len + n + 1 > cap) cap *= 2;
        char *grown = malloc(cap);
        if (!grown) return -1;
        if (t->s) memcpy(grown, t->s, t->len);
        free(t->s);
        t->s = grown;
        t->cap = cap;
    }
    memcpy(t->s + t->len, from, n);
    t->len += n;
    t->s[t->len] = '\0';
    return 0;
}

/* The text a here-document's body gives: a malloc'd copy, expanded unless
 * it was marked as taken as it is. NULL (a message printed) if a $(...)
 * or $((...)) in it fails, a "${" is malformed, or memory ran out. */
static char *heredoc_text(const char *body) {
    text_t out = { 0 };
    if (text_add(&out, "", 0) < 0) return NULL;
    if (body[0] == '\'') {
        if (text_add(&out, body + 1, strlen(body + 1)) < 0) { free(out.s); return NULL; }
        return out.s;
    }
    static char value[sizeof(word_storage)];      /* A variable's or a $(...)'s text */
    for (const char *t = body + 1; *t;) {
        int failed = 0;
        if (t[0] == '\\' && t[1] && strchr("$`\\\n", t[1])) {
            if (t[1] != '\n') failed = text_add(&out, t + 1, 1);   /* (A backslash and a newline join two lines) */
            t += 2;
        } else if (t[0] == '$' && t[1] == '(' && t[2] == '(') {
            const char *end = arith_end(t + 3);
            char expr[256], number[24];
            long v;
            if (!end || (size_t)(end - (t + 3)) >= sizeof(expr)) {
                fprintf(2, "sh: here-document: a $(( is not closed\n");
                failed = 1;
            } else {
                memcpy(expr, t + 3, (size_t)(end - (t + 3)));
                expr[end - (t + 3)] = '\0';
                if (arith_eval(expr, &v) < 0) {
                    failed = 1;
                } else {
                    long_text(v, number);
                    failed = text_add(&out, number, strlen(number));
                }
                t = end + 2;
            }
        } else if (t[0] == '$' && t[1] == '(') {
            const char *end = closing_paren(t + 2);
            char command[256];
            if (!end || (size_t)(end - (t + 2)) >= sizeof(command)) {
                fprintf(2, "sh: here-document: a $( is not closed\n");
                failed = 1;
            } else {
                memcpy(command, t + 2, (size_t)(end - (t + 2)));
                command[end - (t + 2)] = '\0';
                long n = substitute(command, value, sizeof(value));
                failed = n < 0 || text_add(&out, value, (size_t)n) < 0;
                t = end + 1;
            }
        } else if (t[0] == '$' && (is_name_char(t[1]) || t[1] == '?' || t[1] == '#' || t[1] == '@' || t[1] == '{')) {
            /* As inside double quotes: append_variable writes into `value`
             * (bounded by word_storage's size, as for a word), $@ with
             * FIELD_BREAKs between the arguments, made blanks here */
            const char *q = t + 1;
            size_t len = 0;
            int split = 0;
            if (append_variable(&q, value, &len, 0, '"', &split) < 0) {
                fprintf(2, "sh: here-document: a ${ is malformed\n");
                failed = 1;
            } else {
                for (size_t i = 0; i < len; i++) if (value[i] == FIELD_BREAK) value[i] = ' ';
                failed = text_add(&out, value, len);
                t = q;
            }
        } else {
            failed = text_add(&out, t, 1);
            t++;
        }
        if (failed) { free(out.s); return NULL; }
    }
    return out.s;
}

/* Write all `n` bytes at `text` to `fd` (as much as a closed reader allows) */
static void write_all(int fd, const char *text, size_t n) {
    while (n > 0) {
        long done = write(fd, text, n);
        if (done == -EINTR) continue;
        if (done <= 0) return;
        text += done;
        n -= (size_t)done;
    }
}

/* A descriptor to read a here-document's text from: the read end of a
 * pipe. A text the pipe can hold is written into it at once; a longer one
 * by a process of its own, or the command would wait for a reader that is
 * itself. (That process is the child of a child that exits at once, so
 * nobody has to wait for it: init does.) -1 (a message printed) if it
 * cannot be made. */
static long heredoc_open(const char *body) {
    char *text = heredoc_text(body);
    if (!text) return -1;
    size_t n = strlen(text);
    int p[2];
    if (pipe(p) != 0) {
        fprintf(2, "sh: cannot create pipe\n");
        free(text);
        return -1;
    }
    if (n <= HEREDOC_DIRECT) {
        write_all(p[1], text, n);
    } else {
        long pid = fork();
        if (pid == 0) {
            if (fork() == 0) {
                close(p[0]);
                write_all(p[1], text, n);
                exit(0);
            }
            exit(0);
        }
        int status;
        if (pid > 0) while (waitpid(pid, &status, 0) == -EINTR) { }
        else fprintf(2, "sh: fork failed: %s\n", strerror(pid));
    }
    close(p[1]);
    free(text);
    return p[0];
}

/* The word after '<' or '>': one name, even from a $(...) (whose output
 * may not be several words, nor none). NULL if there is no such word. */
static char *redirect_target(const char **p, size_t *used) {
    int flags;
    char *word = next_word(p, used, &flags);
    if (!word || !(flags & WORD_SPLIT)) return word;
    char *start = word;
    while (*start == FIELD_BREAK) start++;
    size_t n = strlen(start);
    while (n > 0 && start[n - 1] == FIELD_BREAK) start[--n] = '\0';
    for (size_t i = 0; i < n; i++) if (start[i] == FIELD_BREAK) return NULL;
    return n > 0 ? start : NULL;
}

/* Parse one redirection at *p, which is at '<' or '>' (or at the digit
 * naming a descriptor before one), and add it to the stage, moving *p past
 * it. 0, or -1 if it is malformed or the stage has too many. */
static int parse_redirect(const char **p, size_t *used, stage_t *stage) {
    const char *c = *p;
    int fd = -1;
    if (*c >= '0' && *c <= '9') fd = *c++ - '0';
    if (stage->redirect_count >= MAX_REDIRECTS) return -1;
    redirect_t *r = &stage->redirects[stage->redirect_count];

    if (c[0] == '<' && c[1] == HEREDOC_MARK) {
        /* A here-document cut_segments took out: its body, by letter */
        int k = c[2] - 'A';
        if (!active_docs || k < 0 || k >= active_doc_count || !active_docs[k]) return -1;
        r->kind = REDIRECT_HEREDOC;
        r->fd = fd < 0 ? 0 : fd;
        r->path = active_docs[k];
        *p = c + 3;
        stage->redirect_count++;
        return 0;
    }
    if (*c == '<') {
        r->kind = REDIRECT_IN;
        if (fd < 0) fd = 0;
        c++;
    } else {
        r->kind = REDIRECT_OUT;
        if (fd < 0) fd = 1;
        c++;
        if (*c == '>') { r->kind = REDIRECT_APPEND; c++; }
    }
    r->fd = fd;

    if (*c == '&' && r->kind != REDIRECT_APPEND) {
        /* "N>&M": one digit, and the word must end there ("2>&1x" is wrong) */
        c++;
        if (*c < '0' || *c > '9') return -1;
        r->kind = REDIRECT_COPY;
        r->from = *c++ - '0';
        if (*c && !is_blank(*c) && !is_operator(*c) && *c != '\n') return -1;
    } else {
        if (!(r->path = redirect_target(&c, used))) return -1;
    }
    *p = c;
    stage->redirect_count++;
    return 0;
}

/* Whether the text at `p` starts an assignment word: NAME=, the name
 * unquoted */
static int is_assignment(const char *p) {
    const char *equals = p;
    while (is_name_char(*equals)) equals++;
    return *equals == '=' && is_name(p, equals);
}

/* Split `line` into pipeline stages. Returns the number of stages, 0 for an
 * empty line, -1 on a syntax error. With `assignments`, NAME=value words
 * before a stage's program name go to its assignments; a line of nothing
 * else is one stage with no arguments (argc 0). Without (the words of a
 * for), they are words like any other. */
static int parse(const char *line, stage_t *stages, int assignments) {
    size_t used = 0;
    int count = 0;
    stage_t *stage = &stages[0];
    memset(stage, 0, sizeof(*stage));

    const char *p = line;
    for (;;) {
        while (is_blank(*p)) p++;
        if (*p == '\0') break;

        if (*p == '|') {
            if (stage->argc == 0 || count + 1 >= MAX_STAGES) return -1;
            stage = &stages[++count];
            memset(stage, 0, sizeof(*stage));
            p++;
        } else if (*p == '<' || *p == '>' ||
                   (*p >= '0' && *p <= '9' && (p[1] == '<' || p[1] == '>'))) {
            /* (A digit right before '<' or '>' names a descriptor, as in
             * every Bourne shell: "echo 2>f" writes nothing to f) */
            if (parse_redirect(&p, &used, stage) < 0) return -1;
        } else if (assignments && stage->argc == 0 && is_assignment(p)) {
            /* The value is expanded but neither split nor matched as a
             * wildcard: X=* stores a star */
            if (stage->assignment_count >= MAX_ASSIGNMENTS) return -1;
            reading_assignment = 1;
            char *word = next_word(&p, &used, NULL);
            reading_assignment = 0;
            if (!word) return -1;
            stage->assignments[stage->assignment_count++] = word;
        } else {
            if (stage->argc >= MAX_ARGS) return -1;
            int flags;
            char *word = next_word(&p, &used, &flags);
            if (!word) return -1;
            if (flags & WORD_SPLIT) {
                if (add_fields(word, stage) < 0) return -1;
            } else if (!(flags & WORD_QUOTED) && glob_has_wildcard(word)) {
                if (expand_wildcard(word, stage, &used) < 0) return -1;
            } else {
                stage->argv[stage->argc++] = word;
            }
        }
    }

    if (stage->argc == 0 && stage->assignment_count > 0) {
        /* Only assignments: they change the shell. A pipe or a redirection
         * would have nothing to apply to. */
        return count == 0 && stage->redirect_count == 0 ? 1 : -1;
    }
    if (stage->argc == 0) return count == 0 ? 0 : -1; /* Empty line, or "a |" */
    return count + 1; /* argv arrays are NULL-terminated by the memset */
}

/* In the child: point fd `target` at `path` (opened with `flags`), or exit
 * with status 1. */
static void redirect_or_die(const char *path, int flags, int target) {
    long fd = open(path, flags);
    if (fd < 0) {
        fprintf(2, "sh: %s: %s\n", path, strerror(fd));
        exit(1);
    }
    if (fd == target) return;   /* ("3> f" with 3 free: open chose 3 itself) */
    dup2((int)fd, target);
    close((int)fd);
}

/* In a child: apply a stage's redirections, in the order written, or exit
 * with status 1 (a message printed) if one cannot be done. */
static void apply_redirects(stage_t *stage) {
    for (int i = 0; i < stage->redirect_count; i++) {
        redirect_t *r = &stage->redirects[i];
        if (r->kind == REDIRECT_IN) {
            redirect_or_die(r->path, O_RDONLY, r->fd);
        } else if (r->kind == REDIRECT_OUT) {
            redirect_or_die(r->path, O_WRONLY | O_CREAT | O_TRUNC, r->fd);
        } else if (r->kind == REDIRECT_APPEND) {
            redirect_or_die(r->path, O_WRONLY | O_CREAT | O_APPEND, r->fd);
        } else if (r->kind == REDIRECT_HEREDOC) {
            long fd = heredoc_open(r->path);
            if (fd < 0) exit(1);
            if (fd != r->fd) {
                dup2((int)fd, r->fd);
                close((int)fd);
            }
        } else if (dup2(r->from, r->fd) < 0) {
            fprintf(2, "sh: %d: bad file descriptor\n", r->from);
            exit(1);
        }
    }
}

/* In a child: apply a stage's redirections and run its program. Never
 * returns (exits 127 if the program could not be run). */
static void exec_stage(stage_t *stage) {
    ignore_interrupt(0); /* Unlike the shell, the program should die on Ctrl-C */
    if (script_fd >= 0) close(script_fd); /* The script is the shell's business, not the program's */
    apply_redirects(stage);

    /* NAME=value before the program: in its environment only (the child's
     * copy of it; the shell's is untouched). PATH set so is used below. */
    for (int i = 0; i < stage->assignment_count; i++) {
        char *equals = strchr(stage->assignments[i], '=');
        *equals = '\0';
        if (setenv(stage->assignments[i], equals + 1) != 0) {
            fprintf(2, "sh: out of memory\n");
            exit(1);
        }
    }

    /* A function, here in a pipeline or with redirections: run by this
     * child, which is a copy of the shell */
    if (is_function(stage->argv[0])) {
        int should_exit = 0;
        interactive = 0;                          /* (The child runs commands, not jobs) */
        exit(call_function(stage->argc, stage->argv, &should_exit));
    }
    /* read in a pipeline ("echo a | read x"): it reads, and the variables
     * it sets go with the child, as in other Bourne shells */
    if (strcmp(stage->argv[0], "read") == 0) exit(read_builtin(stage->argc, stage->argv));

    const char *name = stage->argv[0];
    long err = -ENOENT;
    char path[160];

    if (strchr(name, '/')) {
        /* A name with a slash is a path: run exactly that */
        err = execv(name, stage->argv);
    } else {
        /* Otherwise try each directory in PATH (colon-separated) in turn */
        const char *dirs = getenv("PATH");
        if (!dirs) dirs = "/bin";
        while (*dirs) {
            size_t dir_len = 0;
            while (dirs[dir_len] && dirs[dir_len] != ':') dir_len++;

            if (dir_len > 0 && dir_len + 1 + strlen(name) < sizeof(path)) {
                memcpy(path, dirs, dir_len);
                path[dir_len] = '/';
                strcpy(path + dir_len + 1, name);
                long e = execv(path, stage->argv); /* Only returns on failure */
                if (e != -ENOENT) err = e;         /* Remember a more telling error */
            }
            dirs += dir_len;
            if (*dirs == ':') dirs++;
        }
    }

    fprintf(2, "sh: %s: %s\n", name, err == -ENOENT ? "command not found" : strerror(err));
    exit(127);
}

/* Run a parsed pipeline and return the exit status of its last program (0
 * at once for one started in the background). */
static int run_pipeline(stage_t *stages, int count, int background, const char *text) {
    long pids[MAX_STAGES];
    int input = -1; /* Read end of the pipe from the previous stage */

    for (int i = 0; i < count; i++) {
        int link[2] = { -1, -1 };
        if (i + 1 < count && pipe(link) != 0) {
            fprintf(2, "sh: cannot create pipe\n");
            count = i;
            break;
        }

        pids[i] = fork();
        if (pids[i] == 0) {
            if (interactive) {
                /* One process group per command line, named after its first
                 * process. (Set here and by the shell below: whichever of
                 * the two runs first, it is done before it matters.) */
                setpgid(0, i == 0 ? 0 : pids[0]);
                signal(SIGTSTP, SIG_DFL);   /* The shell ignores Ctrl-Z; its programs must not */
            }
            if (input != -1) { dup2(input, 0); close(input); }
            if (link[1] != -1) { dup2(link[1], 1); close(link[1]); close(link[0]); }
            exec_stage(&stages[i]);
        }

        /* The parent must close its copies of the pipe ends, or the reader
         * would never see end-of-file. */
        if (input != -1) close(input);
        if (link[1] != -1) close(link[1]);
        input = link[0];

        if (pids[i] < 0) {
            fprintf(2, "sh: fork failed: %s\n", strerror(pids[i]));
            count = i;
            break;
        }
        if (interactive) setpgid(pids[i], pids[0]);
    }
    if (input != -1) close(input);
    if (count == 0) return 1;

    int alive[MAX_STAGES];
    for (int i = 0; i < count; i++) alive[i] = 1;

    if (background) {
        int n = job_add(pids, alive, count, text, 0);
        if (n > 0) {
            printf("[%d] %ld\n", n, pids[count - 1]);
            return 0;
        }
        fprintf(2, "sh: too many jobs: running %s in the foreground\n", text);
    }
    return run_in_foreground(pids, alive, count, text, NULL);
}

static int test_builtin(int argc, char **argv);
static int loop_control(int argc, char **argv);
static int return_builtin(int argc, char **argv);

#define READ_LINE_MAX (64 * 1024)   /* The longest line read takes */

/* read [-r] NAME...: one line from standard input, split at blanks into
 * the names (the last takes the rest of the line, its outer blanks
 * removed; names left over are set empty). Without -r a backslash takes
 * the next character as it is (not a separator), and one before the end
 * of the line joins the next line on. The input is read a byte at a time,
 * so that what follows the line is left for whoever reads next. 0; 1 at
 * the end of input (the names set to what there was) or if a variable
 * cannot be set or the line is longer than READ_LINE_MAX; 2 for wrong
 * arguments. */
static int read_builtin(int argc, char **argv) {
    int raw = 0, first = 1;
    if (first < argc && strcmp(argv[first], "-r") == 0) { raw = 1; first++; }
    if (first >= argc) {
        fprintf(2, "usage: read [-r] NAME...\n");
        return 2;
    }
    for (int i = first; i < argc; i++) {
        if (!is_name(argv[i], NULL)) {
            fprintf(2, "sh: read: '%s' is not a name\n", argv[i]);
            return 2;
        }
    }

    /* The line, and for each character whether it was escaped (escaped
     * blanks do not separate) */
    size_t cap = 128, len = 0;
    char *line = malloc(cap), *escaped = malloc(cap);
    int status = 0, escape = 0;
    if (!line || !escaped) { free(line); free(escaped); return 1; }
    for (;;) {
        char c;
        long n = read(0, &c, 1);
        if (n == -EINTR) continue;
        if (n <= 0) { status = 1; break; }       /* The end of the input (or an error) */
        if (c == '\0') continue;                 /* (A variable cannot hold one) */
        int literal = 0;
        if (escape) {
            escape = 0;
            if (c == '\n') continue;             /* "\" at the end of a line: the line goes on */
            literal = 1;
        } else if (c == '\\' && !raw) {
            escape = 1;
            continue;
        } else if (c == '\n') {
            break;
        }
        if (len + 1 >= cap) {
            if (cap >= READ_LINE_MAX) { status = 1; break; }
            char *grown = malloc(cap * 2), *grown_escaped = malloc(cap * 2);
            if (!grown || !grown_escaped) { free(grown); free(grown_escaped); status = 1; break; }
            memcpy(grown, line, len);
            memcpy(grown_escaped, escaped, len);
            free(line);
            free(escaped);
            line = grown;
            escaped = grown_escaped;
            cap *= 2;
        }
        escaped[len] = (char)literal;
        line[len++] = c;
    }
    line[len] = '\0';

    /* Fields at unescaped blanks; the last name takes the rest */
    int names = argc - first;
    size_t i = 0;
    for (int k = 0; k < names; k++) {
        while (i < len && !escaped[i] && is_blank(line[i])) i++;
        size_t start = i, end;
        if (k < names - 1) {
            while (i < len && (escaped[i] || !is_blank(line[i]))) i++;
            end = i;
        } else {
            end = len;
            while (end > start && !escaped[end - 1] && is_blank(line[end - 1])) end--;
            i = len;
        }
        char kept = line[end];
        line[end] = '\0';
        if (set_variable(argv[first + k], line + start) != 0) status = 1;
        line[end] = kept;
    }
    free(escaped);
    free(line);
    return status;
}

/* read with its redirections ("read x < file"), in the shell itself: it
 * sets the shell's variables, so it cannot run in a child as a program
 * does. Each redirected descriptor is saved first and put back after.
 * read's status, or 1 (a message printed) if a redirection fails. */
static int run_read(stage_t *stage) {
    long saved[MAX_REDIRECTS];
    int done = 0, status = 0;
    for (; done < stage->redirect_count; done++) {
        redirect_t *r = &stage->redirects[done];
        saved[done] = dup(r->fd);                /* (Negative if it was not open) */
        long fd;
        if (r->kind == REDIRECT_COPY) fd = r->from;
        else if (r->kind == REDIRECT_HEREDOC) fd = heredoc_open(r->path);
        else if (r->kind == REDIRECT_IN) fd = open(r->path, O_RDONLY);
        else fd = open(r->path, O_WRONLY | O_CREAT | (r->kind == REDIRECT_APPEND ? O_APPEND : O_TRUNC));
        if (fd < 0 || dup2((int)fd, r->fd) < 0) {
            if (r->kind == REDIRECT_COPY) fprintf(2, "sh: %d: bad file descriptor\n", r->from);
            else if (r->kind == REDIRECT_HEREDOC) { }      /* (heredoc_open said why) */
            else fprintf(2, "sh: %s: %s\n", r->path, strerror(fd < 0 ? fd : -EBADF));
            if (fd >= 0 && r->kind != REDIRECT_COPY) close((int)fd);
            done++;
            status = 1;
            break;
        }
        if (r->kind != REDIRECT_COPY && fd != r->fd) close((int)fd);
    }
    if (status == 0) status = read_builtin(stage->argc, stage->argv);
    for (int i = done - 1; i >= 0; i--) {        /* Back as they were, last first */
        int fd = stage->redirects[i].fd;
        if (saved[i] >= 0) {
            dup2((int)saved[i], fd);
            close((int)saved[i]);
        } else {
            close(fd);
        }
    }
    return status;
}

/* Run one pipeline (a part of a command line between ';', '&&' and '||'):
 * a built-in, or programs in the foreground or (with a final '&') the
 * background. Returns its status; sets *should_exit for "exit". */
static int run_pipeline_text(const char *line, int *should_exit) {
    static stage_t stages[MAX_STAGES];
    static char text[256];

    /* A line ending in '&' is not waited for */
    size_t len = strlen(line);
    if (len >= sizeof(text)) len = sizeof(text) - 1;
    memcpy(text, line, len);
    while (len > 0 && (is_blank(text[len - 1]) || text[len - 1] == '\n')) len--;
    int background = len > 0 && text[len - 1] == '&';
    if (background) len--;
    text[len] = '\0';
    if (background && !interactive) {
        fprintf(2, "sh: '&' needs job control, which is only on at the terminal\n");
        return 2;
    }
    line = text;

    unsigned substitutions_before = substitutions_run;
    arith_failed = 0;
    int count = parse(line, stages, 1);
    if (count < 0) {
        if (!arith_failed) fprintf(2, "sh: syntax error\n");   /* (A $((...)) said what was wrong) */
        return 2;
    }
    if (count == 0) return last_status;   /* Nothing to run: $? as it is (or as a $(...) left it) */

    /* NAME=value alone sets the shell's variables. Its status is that of
     * the last $(...) in it, as in other shells (X=$(false) fails), or 0. */
    if (stages[0].argc == 0) {
        for (int i = 0; i < stages[0].assignment_count; i++) {
            char *equals = strchr(stages[0].assignments[i], '=');
            *equals = '\0';
            if (set_variable(stages[0].assignments[i], equals + 1) != 0) return 1;
        }
        return substitutions_run != substitutions_before ? last_status : 0;
    }
    /* (Before a built-in, NAME=value has no effect: none of them reads the
     * environment. Before a program it is set in the program's alone.) */

    /* Built-ins run inside the shell itself */
    if (count == 1 && strcmp(stages[0].argv[0], "exit") == 0) {
        *should_exit = 1;
        return stages[0].argc > 1 ? (int)atoi(stages[0].argv[1]) : 0;
    }
    if (count == 1 && (strcmp(stages[0].argv[0], "break") == 0 || strcmp(stages[0].argv[0], "continue") == 0)) {
        return loop_control(stages[0].argc, stages[0].argv);
    }
    if (count == 1 && strcmp(stages[0].argv[0], "return") == 0) {
        return return_builtin(stages[0].argc, stages[0].argv);
    }
    if (count == 1 && (strcmp(stages[0].argv[0], "test") == 0 || strcmp(stages[0].argv[0], "[") == 0)) {
        return test_builtin(stages[0].argc, stages[0].argv);
    }
    if (count == 1 && !background && strcmp(stages[0].argv[0], "read") == 0) {
        return run_read(&stages[0]);
    }
    /* cd has to be a built-in: a child process could only change its own
     * directory, not the shell's */
    if (count == 1 && strcmp(stages[0].argv[0], "cd") == 0) {
        const char *target = stages[0].argc > 1 ? stages[0].argv[1] : "/";
        long err = chdir(target);
        if (err < 0) {
            fprintf(2, "sh: cd: %s: %s\n", target, strerror(err));
            return 1;
        }
        return 0;
    }
    /* export NAME=value, or export NAME (a variable of the shell's): into
     * the shell's environment, which every program started afterwards
     * inherits */
    if (count == 1 && strcmp(stages[0].argv[0], "export") == 0) {
        for (int i = 1; i < stages[0].argc; i++) {
            char *assignment = stages[0].argv[i];
            char *equals = strchr(assignment, '=');
            if (!is_name(assignment, equals)) {
                fprintf(2, "sh: export: expected NAME=value or NAME, got '%s'\n", assignment);
                return 1;
            }
            if (!equals) {
                if (export_variable(assignment) != 0) return 1;
                continue;
            }
            *equals = '\0';
            char **own = variable_entry(assignment);
            if (own) {                       /* (A name is in one place only) */
                free(*own);
                *own = NULL;
            }
            if (setenv(assignment, equals + 1) != 0) return 1;
        }
        return 0;
    }
    /* unset NAME...: forget variables, the shell's and exported ones */
    if (count == 1 && strcmp(stages[0].argv[0], "unset") == 0) {
        for (int i = 1; i < stages[0].argc; i++) {
            if (!is_name(stages[0].argv[i], NULL)) {
                fprintf(2, "sh: unset: '%s' is not a name\n", stages[0].argv[i]);
                return 1;
            }
            unset_variable(stages[0].argv[i]);
        }
        return 0;
    }
    if (count == 1 && strcmp(stages[0].argv[0], "jobs") == 0) {
        for (int n = 0; n < MAX_JOBS; n++) {
            if (jobs[n].used) printf("[%d] %s   %s\n", n + 1, jobs[n].stopped ? "Stopped" : "Running", jobs[n].text);
        }
        return 0;
    }
    if (count == 1 && (strcmp(stages[0].argv[0], "fg") == 0 || strcmp(stages[0].argv[0], "bg") == 0)) {
        int number = 0;
        int to_foreground = stages[0].argv[0][0] == 'f';
        job_t *job = pick_job(stages[0].argc > 1 ? stages[0].argv[1] : NULL, &number);
        if (!job) {
            fprintf(2, "sh: %s: no such job\n", stages[0].argv[0]);
            return 1;
        }
        /* Either way it runs again: the whole group is told to continue */
        job->stopped = 0;
        if (to_foreground) {
            printf("%s\n", job->text);
            tcsetpgrp(job->pids[0]);            /* Before it wakes, so that Ctrl-C already means this job */
            kill(-job->pids[0], SIGCONT);
            return run_in_foreground(job->pids, job->alive, job->count, job->text, job);
        }
        kill(-job->pids[0], SIGCONT);
        printf("[%d] %s &\n", number, job->text);
        return 0;
    }
    if (count == 1 && strcmp(stages[0].argv[0], "pwd") == 0) {
        char cwd[128];
        if (getcwd(cwd, sizeof(cwd)) < 0) return 1;
        printf("%s\n", cwd);
        return 0;
    }
    if (count == 1 && strcmp(stages[0].argv[0], "help") == 0) {
        printf("Programs live in /bin (try: ls /bin). Syntax:\n"
               "  prog args | prog2      pipe output into another program\n"
               "  a ; b, a && b, a || b  run b after a; only if a succeeded; only if it failed\n"
               "  if c; then a; else b; fi     while c; do a; done     for x in 1 2 3; do echo $x; done\n"
               "  until c; do a; done          (runs a as long as c fails)\n"
               "  case $x in a*|b) cmd;; *) other;; esac      f() { echo $1 $#; return 2; }; f x\n"
               "  [ -f FILE ], [ $x = y ], [ 3 -lt 4 ], [ ! ... ]   conditions (test); break, continue\n"
               "  prog < in > out        redirect input and output (>> appends)\n"
               "  cat <<EOF ... EOF      the lines up to EOF as input ($ expanded unless 'EOF'; <<- drops tabs)\n"
               "  for ...; done > f      a whole if/while/for/case/{ } too, or in a pipe (in a child), or with &\n"
               "  ls *.c /tmp/f?         wildcards: * ? [abc] [a-z] [!x] (not in a word with quotes)\n"
               "  \"$HOME/x\" '$HOME'     double quotes expand $NAME, single quotes take it as it is\n"
               "  echo $(date)           the output of a command, in place (up to 4 KiB)\n"
               "  i=$((i + 1))           arithmetic: + - * / % < <= > >= == != && || ! ( )\n"
               "  read [-r] a b          set a and b from a line of input (b takes the rest)\n"
               "  cd [dir], pwd          change or print the current directory\n"
               "  NAME=value             set a variable of the shell; $NAME or ${NAME} uses it, $? is the last status\n"
               "  export NAME[=value]    put it in the environment, for every program started after\n"
               "  NAME=value prog        set it for that one program only; unset NAME forgets it\n"
               "  prog args &            run in the background; Ctrl-Z stops the running command\n"
               "  jobs, fg [N], bg [N]   list jobs; bring one to the terminal; let a stopped one run on\n"
               "  exit [status]          leave the shell\n");
        return 0;
    }

    /* A function, alone and with nothing to set up: in the shell itself
     * (anywhere else, exec_stage runs it in the child) */
    if (count == 1 && !background && stages[0].redirect_count == 0 && stages[0].assignment_count == 0 &&
        is_function(stages[0].argv[0])) {
        return call_function(stages[0].argc, stages[0].argv, should_exit);
    }

    return run_pipeline(stages, count, background, text);
}

/* --- Compound commands: if, while, for --------------------------------------
 *
 * A command text (a line, a -c argument, or several lines of a script or of
 * the prompt) is cut into segments at ';', newlines, '&&' and '||' outside
 * quotes and $( ), and the segments are parsed into a tree: a simple
 * command (run by run_pipeline_text), or an if, while or for holding lists
 * of further commands. A segment starting with a keyword (if, then, elif,
 * else, fi, while, until, do, done, for) is structure: "then echo a" is the
 * keyword and the first command of the body. A text that stops inside an
 * if/while/for (or after '&&' or '||') is incomplete: at the prompt and in
 * a script the next line is added to it. */

enum { JOIN_ALWAYS, JOIN_IF_OK, JOIN_IF_FAILED };      /* How a command follows the one before */
enum { PARSE_OK, PARSE_SYNTAX, PARSE_INCOMPLETE };
enum { NODE_SIMPLE, NODE_IF, NODE_WHILE, NODE_UNTIL, NODE_FOR, NODE_CASE, NODE_GROUP, NODE_FUNCTION, NODE_PIPELINE };
#define RUN_INCOMPLETE (-1)                             /* run_text: more lines are needed */

typedef struct segment {
    char *text;               /* What is left of it to parse (inside `storage`) */
    char *storage;            /* malloc'd copy of the segment */
    int join_next;            /* JOIN_*: what its end separator says about the next command */
} segment_t;

#define MAX_NODE_DOCS 8       /* Here-documents in one command */

typedef struct node node_t;
typedef struct {
    node_t **items;
    int *joins;               /* joins[i]: how items[i] follows items[i - 1] (JOIN_*) */
    int count, cap;
} cmd_list_t;

typedef struct {
    char **patterns;          /* As written (quotes and all), expanded when the case runs */
    int pattern_count;
    cmd_list_t body;          /* May be empty: "x) ;;" */
} case_item_t;

struct node {
    int kind;                 /* NODE_* */
    char *text;               /* NODE_SIMPLE: the command; NODE_FOR: the words to loop over; NODE_CASE: the word */
    char name[32];            /* NODE_FOR: the variable; NODE_FUNCTION: the function */
    cmd_list_t cond, body;    /* if: condition, then-part; while, until: condition, body; for, { }, function: body */
    cmd_list_t other;         /* if: the else-part (an elif is an else holding one if) */
    case_item_t *items;       /* NODE_CASE: the PATTERN) LIST;; items, in order */
    int item_count;
    char *redirs;             /* In a NODE_PIPELINE's body: the redirections after a compound command, as written */
    char *docs[MAX_NODE_DOCS];  /* The here-documents its text or redirs name ("<\x02A" is docs[0]) */
    int doc_count;
    int background;           /* NODE_PIPELINE: ended with '&' */
};

static segment_t *segs;       /* The segments being parsed, and where the parser is */
static int seg_count, seg_pos, parse_state;
static int loop_depth;        /* Loops being run (for break and continue) */
static int pending_break, pending_continue;   /* Loops still to leave, or to continue */
static int pending_return;    /* "return" ran: the rest of the function is skipped */
static int call_depth;        /* Functions being run */
static int interrupted;       /* A command was ended by Ctrl-C: loops stop */
static int tail_waiting;      /* A closing keyword had redirections or '|' after it, left in its segment */
static int pipe_waiting;      /* The command just parsed is followed by '|': the segment holds what comes after */

/* The here-documents of the text being cut, in the order their "<<WORD"
 * appear: the word, how it is read, and (once the lines after the command
 * have been taken) the body, until a node adopts it */
#define MAX_HEREDOCS 16

typedef struct {
    char delim[64];           /* WORD, its quotes taken off */
    int strip;                /* "<<-": leading tabs go */
    int quoted;               /* Some part of WORD was quoted: the body is not expanded */
    char *body;               /* malloc'd (see heredoc_text for its first character), or NULL */
} heredoc_t;

static heredoc_t heredocs[MAX_HEREDOCS];
static int heredoc_count;     /* "<<WORD"s met */
static int heredoc_read;      /* Of them, those whose lines have been taken */

/* Read the WORD of a "<<WORD" or "<<-WORD" (`q` just after the "<<") into
 * a new entry of `heredocs`. Where the word ends, or NULL if there is no
 * word, a quote in it is not closed, it is too long, or the table is full. */
static const char *take_heredoc_word(const char *q) {
    if (heredoc_count >= MAX_HEREDOCS) return NULL;
    heredoc_t *h = &heredocs[heredoc_count];
    memset(h, 0, sizeof(*h));
    if (*q == '-') { h->strip = 1; q++; }
    while (is_blank(*q)) q++;
    size_t n = 0;
    char quote = 0;
    for (; *q; q++) {
        if (quote) {
            if (*q == quote) { quote = 0; continue; }
        } else if (*q == '"' || *q == '\'') {
            quote = *q;
            h->quoted = 1;
            continue;
        } else if (*q == '\\' && q[1] && q[1] != '\n') {
            h->quoted = 1;                                  /* \EOF: quoted too */
            q++;
        } else if (is_blank(*q) || *q == '\n' || *q == ';' || *q == '&' || *q == '|' || *q == '<' || *q == '>' ||
                   *q == '(' || *q == ')') {
            break;
        }
        if (n + 1 >= sizeof(h->delim)) return NULL;
        h->delim[n++] = *q;
    }
    if (quote || n == 0) return NULL;
    h->delim[n] = '\0';
    heredoc_count++;
    return q;
}

/* At the start of a line (*pp): take the lines of the here-documents whose
 * "<<WORD" came before it and have none yet, each up to the line that is
 * its WORD, and move *pp past them. 0; 1 if the text ended before a WORD
 * line (more lines are needed); -1 if memory ran out. */
static int read_heredoc_bodies(const char **pp) {
    const char *p = *pp;
    while (heredoc_read < heredoc_count) {
        heredoc_t *h = &heredocs[heredoc_read];
        text_t body = { 0 };
        char how = h->quoted ? '\'' : '"';
        if (text_add(&body, &how, 1) < 0) return -1;
        for (;;) {
            if (!*p) { free(body.s); *pp = p; return 1; }
            const char *line = p, *end = strchr(p, '\n');
            if (!end) end = p + strlen(p);
            p = *end ? end + 1 : end;
            if (h->strip) while (line < end && *line == '\t') line++;
            size_t n = (size_t)(end - line);
            if (n == strlen(h->delim) && memcmp(line, h->delim, n) == 0) break;
            if (text_add(&body, line, n) < 0 || text_add(&body, "\n", 1) < 0) { free(body.s); return -1; }
        }
        h->body = body.s;
        heredoc_read++;
    }
    *pp = p;
    return 0;
}

/* Add a segment: `storage` (malloc'd, now the segment's), whose text
 * starts at `text`, followed by `join`. 0, or -1 (storage freed) if memory
 * ran out. */
static int add_segment(char *storage, char *text, int join, int *cap) {
    if (seg_count == *cap) {
        int grown_cap = *cap ? *cap * 2 : 16;
        segment_t *grown = malloc(sizeof(segment_t) * (size_t)grown_cap);
        if (!grown) { free(storage); return -1; }
        if (segs) memcpy(grown, segs, sizeof(segment_t) * (size_t)seg_count);
        free(segs);
        segs = grown;
        *cap = grown_cap;
    }
    segs[seg_count].storage = storage;
    segs[seg_count].text = text;
    segs[seg_count].join_next = join;
    seg_count++;
    return 0;
}

/* Cut `text` into segments (segs, seg_count). Returns PARSE_OK, or
 * PARSE_SYNTAX for an empty command before '&&' or '||' (or no memory), or
 * PARSE_INCOMPLETE for a text that ends with '&&' or '||'. A '#' starting a
 * word outside quotes comments out the rest of its line. The ";;" that
 * ends a case item is a segment of its own. A "<<WORD" is replaced by a
 * mark naming an entry of `heredocs`, and the lines after its line, up to
 * WORD, are taken out of the text as its body; a text that ends before
 * WORD is PARSE_INCOMPLETE. */
static int cut_segments(const char *text) {
    int cap = 0, join = JOIN_ALWAYS;
    seg_count = seg_pos = 0;
    segs = NULL;
    heredoc_count = heredoc_read = 0;
    const char *p = text;
    while (*p) {
        char quoted = 0;
        const char *start = p;
        size_t n = 0;
        char *buf = malloc(strlen(p) + 1);
        if (!buf) return PARSE_SYNTAX;
        for (; *p; p++) {
            if (!quoted && *p == '#' && (p == start || is_blank(p[-1]) || p[-1] == '\n')) {
                while (*p && *p != '\n') p++;              /* A comment, to the end of the line */
                if (!*p) break;
            }
            if (quoted != '\'' && p[0] == '$' && p[1] == '(') {
                /* $(...) and $((...)), taken whole: what is inside has its
                 * own quotes and separators (the ')' of "$(echo a)" closes
                 * it although it is between double quotes) */
                const char *end = closing_paren(p + 2);
                const char *stop = end ? end + 1 : p + strlen(p);
                while (p < stop) buf[n++] = *p++;
                p--;                                        /* (The loop moves on) */
                continue;
            }
            if (!quoted && p[0] == '<' && p[1] == '<') {
                /* A here-document: "<\x02" and its letter in its place (no
                 * longer than "<<W"), its lines taken at the end of the line */
                const char *after = take_heredoc_word(p + 2);
                if (!after) { free(buf); return PARSE_SYNTAX; }
                buf[n++] = '<';
                buf[n++] = HEREDOC_MARK;
                buf[n++] = (char)('A' + heredoc_count - 1);
                p = after - 1;                              /* (The loop moves on) */
                continue;
            }
            if (!quoted && (*p == '"' || *p == '\'')) quoted = *p;
            else if (quoted && *p == quoted) quoted = 0;
            if (!quoted && (*p == ';' || *p == '\n' || (p[0] == '&' && p[1] == '&') || (p[0] == '|' && p[1] == '|'))) break;
            buf[n++] = *p;
        }
        buf[n] = '\0';
        int next = JOIN_ALWAYS, case_end = 0, line_end = 0;
        if (p[0] == ';' && p[1] == ';') { case_end = 1; p += 2; }
        else if (*p == '\n') { line_end = 1; p++; }
        else if (*p == ';') p++;
        else if (*p) { next = (*p == '&') ? JOIN_IF_OK : JOIN_IF_FAILED; p += 2; }

        char *t = buf;
        while (is_blank(*t)) t++;
        size_t len = strlen(t);
        while (len > 0 && is_blank(t[len - 1])) t[--len] = '\0';
        if (len == 0) {                                     /* Nothing here */
            free(buf);
            if (next != JOIN_ALWAYS) return PARSE_SYNTAX;   /* "&& b", "a && && b" */
        } else {
            if (add_segment(buf, t, next, &cap) < 0) return PARSE_SYNTAX;
            join = next;
        }
        if (case_end) {
            char *mark = malloc(3);
            if (!mark) return PARSE_SYNTAX;
            memcpy(mark, ";;", 3);
            if (add_segment(mark, mark, JOIN_ALWAYS, &cap) < 0) return PARSE_SYNTAX;
            join = JOIN_ALWAYS;
        }
        /* (A newline after "&&" keeps the "&&": an empty segment changes nothing) */
        if (line_end && read_heredoc_bodies(&p) < 0) return PARSE_SYNTAX;
    }
    if (heredoc_read < heredoc_count) return PARSE_INCOMPLETE;     /* A here-document's WORD line is to come */
    return join == JOIN_ALWAYS ? PARSE_OK : PARSE_INCOMPLETE;
}

/* Free the segments, and the here-documents no node adopted */
static void free_segments(void) {
    for (int i = 0; i < heredoc_count; i++) {
        free(heredocs[i].body);
        heredocs[i].body = NULL;
    }
    heredoc_count = heredoc_read = 0;
    for (int i = 0; i < seg_count; i++) free(segs[i].storage);
    free(segs);
    segs = NULL;
    seg_count = seg_pos = 0;
}

/* The first word of `text` into word[32] (ended by a blank, an operator or
 * '&', so "done>f" is "done" and ">f"; "" if longer) */
static void first_word(const char *text, char word[32]) {
    size_t n = 0;
    while (text[n] && !is_blank(text[n]) && !is_operator(text[n]) && text[n] != '&') n++;
    if (n >= 32) n = 0;
    memcpy(word, text, n);
    word[n] = '\0';
}

/* Whether `word` is one of the keywords that end or divide a compound command */
static int is_closing_keyword(const char *word) {
    static const char *const closing[] = { "then", "elif", "else", "fi", "do", "done", "esac", ";;", "}" };
    for (unsigned i = 0; i < sizeof(closing) / sizeof(closing[0]); i++) {
        if (strcmp(word, closing[i]) == 0) return 1;
    }
    return 0;
}

/* Whether `p` starts a redirection: '<', '>', or a digit just before one */
static int is_redirect_start(const char *p) {
    return *p == '<' || *p == '>' || (*p >= '0' && *p <= '9' && (p[1] == '<' || p[1] == '>'));
}

#define KEYWORD_ALONE 1       /* take_keyword: nothing may follow it ("fi x") */
#define KEYWORD_TAIL  2       /* ... except redirections and '|', left for parse_pipeline */

/* If the current segment starts with keyword `kw`, take the keyword off it
 * (and the segment, if nothing is left) and return 1; else 0. With
 * KEYWORD_ALONE, anything after the keyword is a syntax error ("fi x");
 * with KEYWORD_TAIL (the keywords that close a compound command) so is
 * anything but redirections, a '|' or a final '&', which stay in the
 * segment with tail_waiting set. */
static int take_keyword(const char *kw, int alone) {
    if (seg_pos >= seg_count) return 0;
    char word[32];
    first_word(segs[seg_pos].text, word);
    if (strcmp(word, kw) != 0) return 0;
    char *rest = segs[seg_pos].text + strlen(kw);
    while (is_blank(*rest)) rest++;
    if (*rest == '\0') {
        seg_pos++;
    } else if (alone == KEYWORD_TAIL && (*rest == '|' || *rest == '&' || is_redirect_start(rest))) {
        segs[seg_pos].text = rest;
        tail_waiting = 1;
    } else if (alone) {
        parse_state = PARSE_SYNTAX;
    } else {
        segs[seg_pos].text = rest;
    }
    return 1;
}

/* Expect keyword `kw` next: PARSE_INCOMPLETE if the text ended first,
 * PARSE_SYNTAX if something else is there. Returns whether it was taken. */
static int expect_keyword(const char *kw, int alone) {
    if (parse_state != PARSE_OK) return 0;
    if (seg_pos >= seg_count) { parse_state = PARSE_INCOMPLETE; return 0; }
    if (!take_keyword(kw, alone)) { parse_state = PARSE_SYNTAX; return 0; }
    return parse_state == PARSE_OK;
}

/* Add `item`, joined by `join`, to a list. 0, or -1 if memory ran out. */
static int list_add(cmd_list_t *list, node_t *item, int join) {
    if (list->count == list->cap) {
        int cap = list->cap ? list->cap * 2 : 4;
        node_t **items = malloc(sizeof(node_t *) * (size_t)cap);
        int *joins = malloc(sizeof(int) * (size_t)cap);
        if (!items || !joins) { free(items); free(joins); return -1; }
        if (list->count) {
            memcpy(items, list->items, sizeof(node_t *) * (size_t)list->count);
            memcpy(joins, list->joins, sizeof(int) * (size_t)list->count);
        }
        free(list->items);
        free(list->joins);
        list->items = items;
        list->joins = joins;
        list->cap = cap;
    }
    list->items[list->count] = item;
    list->joins[list->count++] = join;
    return 0;
}

static void free_list(cmd_list_t *list);

/* Free a node and everything in it */
static void free_node(node_t *node) {
    if (!node) return;
    free(node->text);
    free(node->redirs);
    for (int i = 0; i < node->doc_count; i++) free(node->docs[i]);
    free_list(&node->cond);
    free_list(&node->body);
    free_list(&node->other);
    for (int i = 0; i < node->item_count; i++) {
        for (int j = 0; j < node->items[i].pattern_count; j++) free(node->items[i].patterns[j]);
        free(node->items[i].patterns);
        free_list(&node->items[i].body);
    }
    free(node->items);
    free(node);
}

/* Free the commands of a list (not the list itself) */
static void free_list(cmd_list_t *list) {
    for (int i = 0; i < list->count; i++) free_node(list->items[i]);
    free(list->items);
    free(list->joins);
    memset(list, 0, sizeof(*list));
}

static void parse_list(cmd_list_t *list, int nested);

/* A new node of `kind`, or NULL (parse_state set) if memory ran out */
static node_t *new_node(int kind) {
    node_t *node = malloc(sizeof(node_t));
    if (!node) { parse_state = PARSE_SYNTAX; return NULL; }
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    return node;
}

/* The rest of an if (after "if" or "elif"): condition, then-part, and an
 * elif, else or fi. The node, or NULL with parse_state set. */
static node_t *parse_if(void) {
    node_t *node = new_node(NODE_IF);
    if (!node) return NULL;
    parse_list(&node->cond, 1);
    if (expect_keyword("then", 0)) parse_list(&node->body, 1);
    if (parse_state == PARSE_OK) {
        if (take_keyword("elif", 0)) {
            node_t *inner = parse_if();                   /* It takes the one "fi" */
            if (inner && list_add(&node->other, inner, JOIN_ALWAYS) < 0) { free_node(inner); parse_state = PARSE_SYNTAX; }
        } else {
            if (take_keyword("else", 0)) parse_list(&node->other, 1);
            expect_keyword("fi", KEYWORD_TAIL);
        }
    }
    if (parse_state == PARSE_OK && (node->cond.count == 0 || node->body.count == 0)) parse_state = PARSE_SYNTAX;
    if (parse_state != PARSE_OK) { free_node(node); return NULL; }
    return node;
}

/* The rest of a while or until loop (after the keyword; `kind` says
 * which): condition, "do", body, "done". The node, or NULL with
 * parse_state set. */
static node_t *parse_while(int kind) {
    node_t *node = new_node(kind);
    if (!node) return NULL;
    parse_list(&node->cond, 1);
    if (expect_keyword("do", 0)) parse_list(&node->body, 1);
    expect_keyword("done", KEYWORD_TAIL);
    if (parse_state == PARSE_OK && (node->cond.count == 0 || node->body.count == 0)) parse_state = PARSE_SYNTAX;
    if (parse_state != PARSE_OK) { free_node(node); return NULL; }
    return node;
}

/* A for, starting at the current segment, "for NAME in WORDS" (one
 * segment), then "do", body, "done". The node, or NULL with parse_state set. */
static node_t *parse_for(void) {
    node_t *node = new_node(NODE_FOR);
    if (!node) return NULL;
    const char *p = segs[seg_pos].text + 3;
    while (is_blank(*p)) p++;
    size_t n = 0;
    while (is_name_char(p[n])) n++;
    const char *after = p + n;
    while (is_blank(*after)) after++;
    int bad_name = n == 0 || n >= sizeof(node->name) || (p[0] >= '0' && p[0] <= '9');   /* Not "1" */
    if (bad_name || strncmp(after, "in", 2) != 0 || (after[2] && !is_blank(after[2]))) {
        parse_state = PARSE_SYNTAX;
        free_node(node);
        return NULL;
    }
    memcpy(node->name, p, n);
    node->name[n] = '\0';
    after += 2;
    size_t len = strlen(after);
    node->text = malloc(len + 1);
    if (node->text) memcpy(node->text, after, len + 1);
    else parse_state = PARSE_SYNTAX;
    seg_pos++;
    if (expect_keyword("do", 0)) parse_list(&node->body, 1);
    expect_keyword("done", KEYWORD_TAIL);
    if (parse_state == PARSE_OK && node->body.count == 0) parse_state = PARSE_SYNTAX;
    if (parse_state != PARSE_OK) { free_node(node); return NULL; }
    return node;
}

/* Give `node` the here-documents that `text` (its own text or
 * redirections) names: each mark's body moves from `heredocs` to
 * node->docs, and the mark's letter is changed to say where. 0, or -1 if
 * a mark names nothing or the node has too many. (A body not read yet,
 * the text being incomplete, is adopted as NULL: it will not run.) */
static int adopt_docs(node_t *node, char *text) {
    for (char *c = text; c && *c; c++) {
        if (c[0] != '<' || c[1] != HEREDOC_MARK || !c[2]) continue;
        int k = c[2] - 'A';
        if (k < 0 || k >= heredoc_count || node->doc_count >= MAX_NODE_DOCS) return -1;
        node->docs[node->doc_count] = heredocs[k].body;
        heredocs[k].body = NULL;
        c[2] = (char)('A' + node->doc_count++);
        c += 2;
    }
    return 0;
}

/* A malloc'd copy of the `n` characters at `text`, or NULL */
static char *copy_text(const char *text, size_t n) {
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, text, n);
    copy[n] = '\0';
    return copy;
}

/* Where the word starting at `p` ends: at a blank, an operator or ')'
 * outside quotes and $( ). NULL if a quote or a $( is not closed. */
static const char *word_end(const char *p) {
    char quote = 0;
    for (; *p; p++) {
        if (quote) { if (*p == quote) quote = 0; continue; }
        if (*p == '"' || *p == '\'') quote = *p;
        else if (p[0] == '$' && p[1] == '(') { if (!(p = closing_paren(p + 2))) return NULL; }
        else if (is_blank(*p) || is_operator(*p) || *p == ')') break;
    }
    return quote ? NULL : p;
}

/* The next '|' in `p` outside quotes and $( ) (a pipe, not "||", which
 * the segments were already cut at), or NULL */
static const char *next_pipe(const char *p) {
    char quote = 0;
    for (; *p; p++) {
        if (quote) { if (*p == quote) quote = 0; continue; }
        if (*p == '"' || *p == '\'') quote = *p;
        else if (p[0] == '$' && p[1] == '(') { if (!(p = closing_paren(p + 2))) return NULL; }
        else if (*p == '|') return p;
    }
    return NULL;
}

/* In a simple command's text, the first '|' followed by a compound
 * command ("echo x | while ..."), or NULL: the text before it is one
 * command of a pipeline, what follows is parsed as the next */
static const char *pipe_into_compound(const char *text) {
    static const char *const starts[] = { "if", "while", "until", "for", "case", "{" };
    for (const char *bar = next_pipe(text); bar; bar = next_pipe(bar + 1)) {
        char word[32];
        const char *after = bar + 1;
        while (is_blank(*after)) after++;
        first_word(after, word);
        for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
            if (strcmp(word, starts[i]) == 0) return bar;
        }
    }
    return NULL;
}

/* Make the current segment start at `rest` (blanks skipped), or move to
 * the next one if nothing is left of it */
static void continue_segment(const char *rest) {
    while (is_blank(*rest)) rest++;
    if (*rest) segs[seg_pos].text = (char *)rest;
    else seg_pos++;
}

/* One "PATTERN|PATTERN) LIST" of a case (an optional '(' first), and the
 * ";;" after it unless "esac" follows; added to `node`. 0, or -1 with
 * parse_state set. */
static int parse_case_item(node_t *node) {
    case_item_t *items = malloc(sizeof(case_item_t) * (size_t)(node->item_count + 1));
    if (!items) { parse_state = PARSE_SYNTAX; return -1; }
    if (node->item_count) memcpy(items, node->items, sizeof(case_item_t) * (size_t)node->item_count);
    free(node->items);
    node->items = items;
    case_item_t *item = &items[node->item_count++];
    memset(item, 0, sizeof(*item));

    const char *p = segs[seg_pos].text;
    if (*p == '(') p++;
    for (;;) {                                   /* Patterns, each one word, divided by '|', up to ')' */
        while (is_blank(*p)) p++;
        const char *end = word_end(p);
        char **grown = end && end != p ? malloc(sizeof(char *) * (size_t)(item->pattern_count + 1)) : NULL;
        if (!grown) { parse_state = PARSE_SYNTAX; return -1; }
        if (item->pattern_count) memcpy(grown, item->patterns, sizeof(char *) * (size_t)item->pattern_count);
        free(item->patterns);
        item->patterns = grown;
        if (!(item->patterns[item->pattern_count] = copy_text(p, (size_t)(end - p)))) { parse_state = PARSE_SYNTAX; return -1; }
        item->pattern_count++;
        p = end;
        while (is_blank(*p)) p++;
        if (*p == '|') { p++; continue; }
        if (*p == ')') break;
        parse_state = PARSE_SYNTAX;              /* "a b)", no ')' */
        return -1;
    }
    continue_segment(p + 1);
    parse_list(&item->body, 1);
    if (parse_state != PARSE_OK) return -1;
    if (take_keyword(";;", 1)) return parse_state == PARSE_OK ? 0 : -1;
    char word[32];
    first_word(segs[seg_pos].text, word);
    if (strcmp(word, "esac") != 0) { parse_state = PARSE_SYNTAX; return -1; }   /* "fi", "done", "}" */
    return 0;
}

/* A case, starting at the current segment: "case WORD in", then items up
 * to "esac". The node, or NULL with parse_state set. */
static node_t *parse_case(void) {
    node_t *node = new_node(NODE_CASE);
    if (!node) return NULL;
    const char *p = segs[seg_pos].text + 4;
    while (is_blank(*p)) p++;
    const char *end = word_end(p);
    if (!end || end == p || !(node->text = copy_text(p, (size_t)(end - p)))) {
        parse_state = PARSE_SYNTAX;
        free_node(node);
        return NULL;
    }
    p = end;
    while (is_blank(*p)) p++;
    if (*p == '\0') {                            /* "in" on the next line */
        seg_pos++;
        expect_keyword("in", 0);
    } else if (strncmp(p, "in", 2) == 0 && (p[2] == '\0' || is_blank(p[2]))) {
        continue_segment(p + 2);
    } else {
        parse_state = PARSE_SYNTAX;
    }
    while (parse_state == PARSE_OK) {
        if (seg_pos >= seg_count) { parse_state = PARSE_INCOMPLETE; break; }
        if (take_keyword("esac", KEYWORD_TAIL)) break;
        if (parse_case_item(node) < 0) break;
    }
    if (parse_state != PARSE_OK) { free_node(node); return NULL; }
    return node;
}

/* The rest of a { } group or a function's body: "{" (already taken for a
 * group), the commands, "}". Fills node->body; 0, or -1 with parse_state
 * set. */
static int parse_braces(node_t *node) {
    parse_list(&node->body, 1);
    expect_keyword("}", KEYWORD_TAIL);
    if (parse_state == PARSE_OK && node->body.count == 0) parse_state = PARSE_SYNTAX;
    return parse_state == PARSE_OK ? 0 : -1;
}

/* If the current segment starts "NAME()" (blanks allowed around the
 * parentheses), where its "{" is: just after, or the next segment. NULL
 * if it does not. */
static const char *function_header(const char *text) {
    size_t n = 0;
    while (is_name_char(text[n])) n++;
    if (n == 0 || n >= 32 || !is_name(text, text + n)) return NULL;
    const char *p = text + n;
    while (is_blank(*p)) p++;
    if (*p++ != '(') return NULL;
    while (is_blank(*p)) p++;
    if (*p++ != ')') return NULL;
    return p;
}

/* A function definition, "NAME() { LIST; }", the current segment starting
 * with the header (`after` just past its ')'). The node, or NULL with
 * parse_state set. */
static node_t *parse_function(const char *after) {
    node_t *node = new_node(NODE_FUNCTION);
    if (!node) return NULL;
    const char *text = segs[seg_pos].text;
    size_t n = 0;
    while (is_name_char(text[n])) n++;
    memcpy(node->name, text, n);
    node->name[n] = '\0';
    continue_segment(after);
    if (!expect_keyword("{", 0) || parse_braces(node) < 0) { free_node(node); return NULL; }
    if (tail_waiting) {                          /* "f() { ...; } > x": not taken */
        parse_state = PARSE_SYNTAX;
        free_node(node);
        return NULL;
    }
    return node;
}

/* One command: a simple one, an if/while/until/for/case or { } (whose
 * keyword starts the current segment), or a function definition. The
 * node, or NULL with parse_state set. */
static node_t *parse_command(void) {
    char word[32];
    first_word(segs[seg_pos].text, word);
    if (take_keyword("if", 0)) return parse_if();
    if (take_keyword("while", 0)) return parse_while(NODE_WHILE);
    if (take_keyword("until", 0)) return parse_while(NODE_UNTIL);
    if (strcmp(word, "for") == 0) return parse_for();
    if (strcmp(word, "case") == 0) return parse_case();
    if (take_keyword("{", 0)) {
        node_t *group = new_node(NODE_GROUP);
        if (group && parse_braces(group) < 0) { free_node(group); group = NULL; }
        return group;
    }
    const char *after = function_header(segs[seg_pos].text);
    if (after) return parse_function(after);

    node_t *node = new_node(NODE_SIMPLE);
    if (!node) return NULL;
    const char *text = segs[seg_pos].text;
    const char *bar = pipe_into_compound(text);   /* "a | while ...": a alone, the rest after */
    if (!(node->text = copy_text(text, bar ? (size_t)(bar - text) : strlen(text))) || adopt_docs(node, node->text) < 0) {
        free_node(node);
        parse_state = PARSE_SYNTAX;
        return NULL;
    }
    if (bar) {
        continue_segment(bar + 1);
        pipe_waiting = 1;
    } else {
        seg_pos++;
    }
    return node;
}

/* The redirections and '|' left after a compound command's closing keyword
 * (tail_waiting): the redirections become `node`'s, as written (they are
 * expanded when it runs), and a '|' sets pipe_waiting, the segment then
 * holding what follows it (or the next segment, if the line ends there).
 * 0, or -1 with parse_state set. */
static int take_tail(node_t *node) {
    tail_waiting = 0;
    const char *text = segs[seg_pos].text;
    const char *bar = next_pipe(text);
    size_t n = bar ? (size_t)(bar - text) : strlen(text);
    while (n > 0 && is_blank(text[n - 1])) n--;
    if (n > 0 && (!(node->redirs = copy_text(text, n)) || adopt_docs(node, node->redirs) < 0)) {
        parse_state = PARSE_SYNTAX;
        return -1;
    }
    if (bar) {
        continue_segment(bar + 1);
        pipe_waiting = 1;
    } else {
        seg_pos++;
    }
    return 0;
}

/* If the last command of `pipeline` ends with '&' (after a compound
 * command's redirections, or as a simple command's last word), take it
 * off and mark the pipeline to run in the background */
static void take_ampersand(node_t *pipeline) {
    node_t *last = pipeline->body.items[pipeline->body.count - 1];
    char *text = last->kind == NODE_SIMPLE ? last->text : last->redirs;
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0 && (is_blank(text[len - 1]) || text[len - 1] == '\n')) len--;
    if (len == 0 || text[len - 1] != '&') return;
    len--;
    while (len > 0 && is_blank(text[len - 1])) len--;
    text[len] = '\0';
    pipeline->background = 1;
    if (len == 0 && last->kind != NODE_SIMPLE) {
        free(last->redirs);
        last->redirs = NULL;
    }
}

/* A pipeline: commands joined by '|', any of them compound, a compound one
 * with redirections after its closing keyword. A single command with no
 * redirections of its own is returned as it is (it runs in the shell);
 * anything else becomes a NODE_PIPELINE whose body holds the commands,
 * each run in a child of the shell. The node, or NULL with parse_state set. */
static node_t *parse_pipeline(void) {
    tail_waiting = pipe_waiting = 0;
    node_t *element = parse_command();
    if (!element) return NULL;
    if (!tail_waiting && !pipe_waiting) return element;     /* The usual case */

    node_t *pipeline = new_node(NODE_PIPELINE);
    if (!pipeline) { free_node(element); return NULL; }
    for (;;) {
        if (tail_waiting && take_tail(element) < 0) break;
        if (pipeline->body.count >= MAX_STAGES || list_add(&pipeline->body, element, JOIN_ALWAYS) < 0) {
            parse_state = PARSE_SYNTAX;
            break;
        }
        element = NULL;
        if (!pipe_waiting) {
            take_ampersand(pipeline);
            return pipeline;
        }
        pipe_waiting = 0;
        if (seg_pos >= seg_count) { parse_state = PARSE_INCOMPLETE; break; }   /* "done |" at the end */
        char word[32];
        first_word(segs[seg_pos].text, word);
        if (is_closing_keyword(word) || segs[seg_pos].text[0] == '|') { parse_state = PARSE_SYNTAX; break; }
        if (!(element = parse_command())) break;
        if (element->kind == NODE_FUNCTION) { parse_state = PARSE_SYNTAX; break; }
    }
    free_node(element);
    free_node(pipeline);
    return NULL;
}

/* Commands up to the end of the text or (if `nested`) up to a keyword that
 * closes or divides the compound command being parsed. A closing keyword
 * at the top level, or right after '&&' or '||', is a syntax error. */
static void parse_list(cmd_list_t *list, int nested) {
    int join = JOIN_ALWAYS;
    while (parse_state == PARSE_OK) {
        if (seg_pos >= seg_count) {
            if (nested) parse_state = PARSE_INCOMPLETE;   /* The text ended inside an if/while/for */
            return;
        }
        char word[32];
        first_word(segs[seg_pos].text, word);
        if (is_closing_keyword(word)) {
            if (!nested || join != JOIN_ALWAYS) parse_state = PARSE_SYNTAX;   /* "fi" alone; "a && then" */
            return;
        }
        node_t *node = parse_pipeline();
        if (!node) return;
        if (list_add(list, node, join) < 0) { free_node(node); parse_state = PARSE_SYNTAX; return; }
        join = segs[seg_pos - 1].join_next;                /* What ended it says how the next follows */
    }
}

static int exec_list(cmd_list_t *list, int *should_exit);

/* After a round of a loop: deal with a pending break or continue. Returns
 * 1 if the loop must end (a break for it, or anything for a loop further
 * out), 0 if it goes on. */
static int leave_loop(void) {
    if (pending_return) return 1;                           /* Out of every loop of the function */
    if (pending_break) {
        pending_break--;                                    /* This loop is left; more for outer ones */
        return 1;
    }
    if (pending_continue) {
        if (--pending_continue == 0) return 0;              /* This loop carries on */
        return 1;
    }
    return 0;
}

/* for: the words expanded once, as for a command, and copied (the body
 * reuses the parser's storage); then the body once for each, with the
 * variable set to it (a variable of the shell's, unless it was exported).
 * Its status. */
static int exec_for(node_t *node, int *should_exit) {
    static stage_t stages[MAX_STAGES];
    int count = parse(node->text, stages, 0);
    if (count < 0 || count > 1) {
        fprintf(2, "sh: syntax error\n");
        return 2;
    }
    int words = count ? stages[0].argc : 0, status = 0;
    char **copy = malloc(sizeof(char *) * (size_t)(words + 1));
    if (!copy) return 1;
    for (int i = 0; i < words; i++) {
        size_t n = strlen(stages[0].argv[i]);
        copy[i] = malloc(n + 1);
        if (copy[i]) memcpy(copy[i], stages[0].argv[i], n + 1);
    }
    for (int i = 0; i < words; i++) {
        if (copy[i] && set_variable(node->name, copy[i]) != 0) { status = 1; break; }
        status = exec_list(&node->body, should_exit);
        if (*should_exit || interrupted || leave_loop()) break;
    }
    for (int i = 0; i < words; i++) free(copy[i]);
    free(copy);
    return status;
}

static node_t *copy_node(const node_t *from);

/* Copy the list `from` into `to` (empty before). 0, or -1 (nothing left
 * in `to`) if memory ran out. */
static int copy_list(const cmd_list_t *from, cmd_list_t *to) {
    memset(to, 0, sizeof(*to));
    for (int i = 0; i < from->count; i++) {
        node_t *copy = copy_node(from->items[i]);
        if (!copy || list_add(to, copy, from->joins[i]) < 0) {
            free_node(copy);
            free_list(to);
            return -1;
        }
    }
    return 0;
}

/* A copy of a node and everything in it, or NULL if memory ran out */
static node_t *copy_node(const node_t *from) {
    node_t *node = malloc(sizeof(node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->kind = from->kind;
    node->background = from->background;
    memcpy(node->name, from->name, sizeof(node->name));
    for (int i = 0; i < from->doc_count; i++) {
        if (from->docs[i] && !(node->docs[i] = copy_text(from->docs[i], strlen(from->docs[i])))) { free_node(node); return NULL; }
        node->doc_count++;
    }
    if ((from->text && !(node->text = copy_text(from->text, strlen(from->text)))) ||
        (from->redirs && !(node->redirs = copy_text(from->redirs, strlen(from->redirs)))) ||
        copy_list(&from->cond, &node->cond) < 0 || copy_list(&from->body, &node->body) < 0 ||
        copy_list(&from->other, &node->other) < 0) {
        free_node(node);
        return NULL;
    }
    if (from->item_count) {
        node->items = malloc(sizeof(case_item_t) * (size_t)from->item_count);
        if (!node->items) { free_node(node); return NULL; }
        memset(node->items, 0, sizeof(case_item_t) * (size_t)from->item_count);
        node->item_count = from->item_count;
        for (int i = 0; i < from->item_count; i++) {
            const case_item_t *item = &from->items[i];
            case_item_t *copy = &node->items[i];
            copy->patterns = malloc(sizeof(char *) * (size_t)item->pattern_count);
            if (!copy->patterns || copy_list(&item->body, &copy->body) < 0) { free_node(node); return NULL; }
            for (int j = 0; j < item->pattern_count; j++) {
                copy->patterns[j] = copy_text(item->patterns[j], strlen(item->patterns[j]));
                if (!copy->patterns[j]) { free_node(node); return NULL; }
                copy->pattern_count++;
            }
        }
    }
    return node;
}

/* --- Functions ---------------------------------------------------------------
 *
 * "NAME() { LIST; }" keeps a copy of the commands under the name; a
 * command NAME then runs them with its words as $1, $2 ... A function
 * lives in the shell that defined it (programs do not see it), is run by
 * the shell itself when it is a command of its own (so it can set the
 * shell's variables), and by a child of the shell in a pipeline, with a
 * redirection, in the background or with NAME=value before it. */

#define MAX_FUNCTIONS  32
#define MAX_CALL_DEPTH 64     /* Calls inside calls (a function calling itself, say) */

typedef struct {
    char name[32];            /* "" for a free entry */
    cmd_list_t body;
} function_t;

static function_t functions[MAX_FUNCTIONS];

/* The function called `name`, or NULL */
static function_t *find_function(const char *name) {
    for (int i = 0; i < MAX_FUNCTIONS; i++) {
        if (functions[i].name[0] && strcmp(functions[i].name, name) == 0) return &functions[i];
    }
    return NULL;
}

/* Whether a function called `name` is defined */
static int is_function(const char *name) {
    return find_function(name) != NULL;
}

/* Define (or redefine) the function of a NODE_FUNCTION: its body is copied
 * (the tree is freed once the command text has run). Its status: 0, or 1
 * (a message printed) if there is no room. */
static int define_function(node_t *node) {
    function_t *f = find_function(node->name);
    for (int i = 0; !f && i < MAX_FUNCTIONS; i++) if (!functions[i].name[0]) f = &functions[i];
    cmd_list_t body;
    if (!f || copy_list(&node->body, &body) < 0) {
        fprintf(2, "sh: %s: no room for another function\n", node->name);
        return 1;
    }
    free_list(&f->body);
    f->body = body;
    memcpy(f->name, node->name, sizeof(f->name));
    return 0;
}

/* Run the function argv[0] (it must exist) with argv[1..] as its
 * arguments, in this process. Its status: that of its last command, or
 * what "return" gave. Calls nested more than MAX_CALL_DEPTH deep fail
 * with status 2 and abandon the whole command text being run. */
static int call_function(int argc, char **argv, int *should_exit) {
    function_t *f = find_function(argv[0]);
    if (call_depth >= MAX_CALL_DEPTH) {
        fprintf(2, "sh: %s: functions called more than %d deep\n", argv[0], MAX_CALL_DEPTH);
        interrupted = 1;
        return 2;
    }
    /* The words live in word_storage, which the body's commands reuse,
     * and the body may redefine the function: both are copied */
    char **args = malloc(sizeof(char *) * (size_t)argc);
    cmd_list_t body;
    if (!args || copy_list(&f->body, &body) < 0) {
        free(args);
        fprintf(2, "sh: %s: out of memory\n", argv[0]);
        return 1;
    }
    int count = 0;
    for (int i = 1; i < argc; i++) {
        if ((args[count] = copy_text(argv[i], strlen(argv[i])))) count++;
    }

    char **saved_args = arguments;
    int saved_count = argument_count, saved_loops = loop_depth;
    arguments = args;
    argument_count = count;
    loop_depth = 0;           /* break and continue do not reach the caller's loops */
    call_depth++;
    int status = exec_list(&body, should_exit);
    call_depth--;
    pending_return = 0;
    loop_depth = saved_loops;
    arguments = saved_args;
    argument_count = saved_count;

    for (int i = 0; i < count; i++) free(args[i]);
    free(args);
    free_list(&body);
    return status;
}

/* return [N]: end the function being run, with status N (else $?). Outside
 * a function, an error (status 1). */
static int return_builtin(int argc, char **argv) {
    if (call_depth == 0) {
        fprintf(2, "sh: return: not in a function\n");
        return 1;
    }
    if (argc > 2 || (argc == 2 && (argv[1][0] < '0' || argv[1][0] > '9'))) {
        fprintf(2, "sh: return: expected a number\n");
        return 2;
    }
    pending_return = 1;
    return argc == 2 ? atoi(argv[1]) : last_status;
}

/* case: the word expanded (one word: not split, no wildcards), then each
 * item's patterns in turn; the first that matches runs its list. Quoted
 * parts of a pattern match themselves. Its status: that of the list, or
 * 0 if nothing matched (or the list is empty). */
static int exec_case(node_t *node, int *should_exit) {
    size_t used = 0;
    const char *p = node->text;
    reading_assignment = 1;
    const char *expanded = next_word(&p, &used, NULL);
    reading_assignment = 0;
    char *word = expanded ? copy_text(expanded, strlen(expanded)) : NULL;
    if (!word) {
        fprintf(2, "sh: syntax error\n");
        return 2;
    }
    for (int i = 0; i < node->item_count; i++) {
        for (int j = 0; j < node->items[i].pattern_count; j++) {
            used = 0;
            p = node->items[i].patterns[j];
            reading_assignment = reading_pattern = 1;
            const char *pattern = next_word(&p, &used, NULL);
            reading_assignment = reading_pattern = 0;
            if (!pattern) {
                free(word);
                fprintf(2, "sh: syntax error\n");
                return 2;
            }
            if (glob_match_text(pattern, word)) {
                free(word);
                return node->items[i].body.count ? exec_list(&node->items[i].body, should_exit) : 0;
            }
        }
    }
    free(word);
    return 0;
}

static int exec_node(node_t *node, int *should_exit);

/* In a child running one command of a pipeline: apply the redirections
 * written after a compound command (`text`, expanded now), or exit with
 * status 2 if they are not all redirections ("done > f x") and 1 if one
 * cannot be done. */
static void redirect_compound(const char *text) {
    static stage_t stage;
    memset(&stage, 0, sizeof(stage));
    size_t used = 0;
    const char *p = text;
    for (;;) {
        while (is_blank(*p)) p++;
        if (*p == '\0') break;
        if (!is_redirect_start(p) || parse_redirect(&p, &used, &stage) < 0) {
            fprintf(2, "sh: syntax error\n");
            exit(2);
        }
    }
    apply_redirects(&stage);
}

/* A NODE_PIPELINE's commands as "jobs" shows them, into out[size]: a
 * simple command as written, a compound one by its shape ("while ...;
 * done"), joined by " | " */
static void describe_pipeline(const node_t *node, char *out, size_t size) {
    size_t used = 0;
    for (int i = 0; i < node->body.count; i++) {
        const node_t *e = node->body.items[i];
        const char *part = e->kind == NODE_SIMPLE ? e->text :
                           e->kind == NODE_IF ? "if ...; fi" :
                           e->kind == NODE_WHILE ? "while ...; done" :
                           e->kind == NODE_UNTIL ? "until ...; done" :
                           e->kind == NODE_FOR ? "for ...; done" :
                           e->kind == NODE_CASE ? "case ... esac" : "{ ...; }";
        if (i > 0 && used + 3 < size) { memcpy(out + used, " | ", 3); used += 3; }
        for (; *part && used + 1 < size; part++) out[used++] = *part;
    }
    out[used] = '\0';
}

/* Run a NODE_PIPELINE: each command in a child of the shell (a copy of it:
 * what a compound command there sets, variables or the directory, stays
 * in the child), joined by pipes, a compound command's own redirections
 * applied after the pipe, as for a program. Waited for like any pipeline,
 * so Ctrl-C and Ctrl-Z reach all of it. Its status: the last command's. */
static int exec_pipeline(node_t *node) {
    int count = node->body.count;
    if (node->background && !interactive) {
        fprintf(2, "sh: '&' needs job control, which is only on at the terminal\n");
        return 2;
    }
    long pids[MAX_STAGES];
    int input = -1;
    for (int i = 0; i < count; i++) {
        int link[2] = { -1, -1 };
        if (i + 1 < count && pipe(link) != 0) {
            fprintf(2, "sh: cannot create pipe\n");
            count = i;
            break;
        }
        pids[i] = fork();
        if (pids[i] == 0) {
            if (interactive) {
                setpgid(0, i == 0 ? 0 : pids[0]);     /* (As run_pipeline does) */
                signal(SIGTSTP, SIG_DFL);
            }
            interactive = 0;                          /* The child runs commands, not jobs */
            ignore_interrupt(0);                      /* Ctrl-C ends it with what it runs */
            if (script_fd >= 0) { close(script_fd); script_fd = -1; }
            if (input != -1) { dup2(input, 0); close(input); }
            if (link[1] != -1) { dup2(link[1], 1); close(link[1]); close(link[0]); }
            node_t *element = node->body.items[i];
            active_docs = element->docs;              /* (For a here-document after its closing keyword) */
            active_doc_count = element->doc_count;
            if (element->redirs) redirect_compound(element->redirs);
            int should_exit = 0;
            exit(exec_node(element, &should_exit));
        }
        /* (The parent closes its copies of the pipe ends, as run_pipeline) */
        if (input != -1) close(input);
        if (link[1] != -1) close(link[1]);
        input = link[0];
        if (pids[i] < 0) {
            fprintf(2, "sh: fork failed: %s\n", strerror(pids[i]));
            count = i;
            break;
        }
        if (interactive) setpgid(pids[i], pids[0]);
    }
    if (input != -1) close(input);
    if (count == 0) return 1;
    int alive[MAX_STAGES];
    for (int i = 0; i < count; i++) alive[i] = 1;
    char text[64];
    describe_pipeline(node, text, sizeof(text));
    if (node->background) {
        int n = job_add(pids, alive, count, text, 0);
        if (n > 0) {
            printf("[%d] %ld\n", n, pids[count - 1]);
            return 0;
        }
        fprintf(2, "sh: too many jobs: running %s in the foreground\n", text);
    }
    return run_in_foreground(pids, alive, count, text, NULL);
}

/* Run one command of the tree; its status. An if whose branches are not
 * taken, a loop that never runs its body, a case that matches nothing and
 * a function definition give 0. */
static int exec_node(node_t *node, int *should_exit) {
    if (node->kind == NODE_SIMPLE) {
        char *const *outer_docs = active_docs;          /* (A function it calls sets its own) */
        int outer_count = active_doc_count;
        active_docs = node->docs;
        active_doc_count = node->doc_count;
        int status = run_pipeline_text(node->text, should_exit);
        active_docs = outer_docs;
        active_doc_count = outer_count;
        if (status == 128 + SIGINT) interrupted = 1;        /* Ctrl-C: no further loop rounds */
        return status;
    }
    if (node->kind == NODE_CASE) return exec_case(node, should_exit);
    if (node->kind == NODE_GROUP) return exec_list(&node->body, should_exit);
    if (node->kind == NODE_FUNCTION) return define_function(node);
    if (node->kind == NODE_PIPELINE) {
        int status = exec_pipeline(node);
        if (status == 128 + SIGINT) interrupted = 1;        /* As for a simple command */
        return status;
    }
    if (node->kind == NODE_IF) {
        int cond = exec_list(&node->cond, should_exit);
        if (*should_exit || interrupted || pending_break || pending_continue || pending_return) return cond;
        if (cond == 0) return exec_list(&node->body, should_exit);
        return node->other.count ? exec_list(&node->other, should_exit) : 0;
    }

    int status = 0;
    loop_depth++;
    if (node->kind == NODE_FOR) {
        status = exec_for(node, should_exit);
    } else {
        for (;;) {                                         /* while: as long as the condition succeeds; until: fails */
            int cond = exec_list(&node->cond, should_exit);
            int stop = node->kind == NODE_WHILE ? cond != 0 : cond == 0;
            if (*should_exit || interrupted || leave_loop() || stop) break;
            status = exec_list(&node->body, should_exit);
            if (*should_exit || interrupted || leave_loop()) break;
        }
    }
    loop_depth--;
    return status;
}

/* Run a list: each command in turn, the next one only if its join allows
 * (a command not run leaves the status as it was). Stops at exit, a
 * pending break, continue or return, or Ctrl-C. Returns the last status; $? is up
 * to date after each command. */
static int exec_list(cmd_list_t *list, int *should_exit) {
    int status = last_status;
    for (int i = 0; i < list->count; i++) {
        int join = list->joins[i];
        if (join == JOIN_ALWAYS || (join == JOIN_IF_OK && status == 0) || (join == JOIN_IF_FAILED && status != 0)) {
            status = last_status = exec_node(list->items[i], should_exit);
        }
        if (*should_exit || interrupted || pending_break || pending_continue || pending_return) break;
    }
    return status;
}

/* Parse and run a command text. Returns its status (2 for a syntax error,
 * with a message), or, if `may_continue` and the text stops inside an
 * if/while/for or after '&&' or '||', RUN_INCOMPLETE without running
 * anything. */
static int run_text(const char *text, int *should_exit, int may_continue) {
    cmd_list_t top;
    memset(&top, 0, sizeof(top));
    parse_state = cut_segments(text);
    int ended_early = parse_state == PARSE_INCOMPLETE;      /* (After "&&": still parse, to catch errors) */
    if (ended_early) parse_state = PARSE_OK;
    if (parse_state == PARSE_OK) parse_list(&top, 0);
    if (parse_state == PARSE_OK && ended_early) parse_state = PARSE_INCOMPLETE;
    free_segments();
    if (parse_state == PARSE_INCOMPLETE && may_continue) {
        free_list(&top);
        return RUN_INCOMPLETE;
    }
    if (parse_state != PARSE_OK) {
        free_list(&top);
        fprintf(2, "sh: syntax error\n");
        return last_status = 2;
    }
    interrupted = 0;
    int status = exec_list(&top, should_exit);
    free_list(&top);
    pending_break = pending_continue = pending_return = 0;
    return status;
}

/* Run a command text that must be complete (sh -c, $(...)): see run_text */
static int run_line(const char *line, int *should_exit) {
    return run_text(line, should_exit, 0);
}

/* break [N] and continue [N]: leave (or start the next round of) the N-th
 * enclosing loop, 1 if not given. Outside a loop, or with an N that is not
 * a positive number, an error (status 1); otherwise 0. */
static int loop_control(int argc, char **argv) {
    long n = argc > 1 ? atoi(argv[1]) : 1;
    if (argc > 2 || n < 1 || (argc > 1 && (argv[1][0] < '0' || argv[1][0] > '9'))) {
        fprintf(2, "sh: %s: expected a positive number\n", argv[0]);
        return 1;
    }
    if (loop_depth == 0) {
        fprintf(2, "sh: %s: only meaningful in a loop\n", argv[0]);
        return 1;
    }
    if (n > loop_depth) n = loop_depth;                     /* As POSIX: the outermost loop */
    if (argv[0][0] == 'b') pending_break = (int)n;
    else pending_continue = (int)n;
    return 0;
}

/* Whether `op` is one of test's two-operand operators */
static int is_binary_test(const char *op) {
    static const char *const ops[] = { "=", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge" };
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) if (strcmp(op, ops[i]) == 0) return 1;
    return 0;
}

/* Whether `s` is a decimal integer, with an optional '-' */
static int is_integer(const char *s) {
    if (*s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* test EXPR, [ EXPR ]: by the number of operands, as POSIX has it: none is
 * false; one is true if not empty; "-z s", "-n s", "-e path", "-f path",
 * "-d path"; "a = b", "a != b", "x -eq y" (and -ne -lt -le -gt -ge, for
 * integers); "! EXPR" turns any of them round. Returns 0 (true), 1 (false)
 * or 2 (an expression it cannot read, with a message). */
static int test_builtin(int argc, char **argv) {
    const char *name = argv[0];
    if (strcmp(name, "[") == 0) {
        if (strcmp(argv[argc - 1], "]") != 0) {
            fprintf(2, "sh: [: missing ]\n");
            return 2;
        }
        argc--;
    }
    char **a = argv + 1;
    int n = argc - 1, negate = 0;
    if (n >= 2 && strcmp(a[0], "!") == 0 && !(n == 3 && is_binary_test(a[1]))) {
        negate = 1;
        a++;
        n--;
    }

    int result = 2;
    struct stat st;
    if (n == 0) {
        result = 1;
    } else if (n == 1) {
        result = a[0][0] ? 0 : 1;
    } else if (n == 2) {
        if (strcmp(a[0], "-z") == 0) result = a[1][0] ? 1 : 0;
        else if (strcmp(a[0], "-n") == 0) result = a[1][0] ? 0 : 1;
        else if (strcmp(a[0], "-e") == 0) result = stat(a[1], &st) == 0 ? 0 : 1;
        else if (strcmp(a[0], "-f") == 0) result = stat(a[1], &st) == 0 && st.type == S_FILE ? 0 : 1;
        else if (strcmp(a[0], "-d") == 0) result = stat(a[1], &st) == 0 && st.type == S_DIR ? 0 : 1;
    } else if (n == 3 && strcmp(a[1], "=") == 0) {
        result = strcmp(a[0], a[2]) == 0 ? 0 : 1;
    } else if (n == 3 && strcmp(a[1], "!=") == 0) {
        result = strcmp(a[0], a[2]) != 0 ? 0 : 1;
    } else if (n == 3 && is_binary_test(a[1])) {
        if (!is_integer(a[0]) || !is_integer(a[2])) {
            fprintf(2, "sh: %s: integer expected\n", name);
            return 2;
        }
        long x = atoi(a[0]), y = atoi(a[2]);
        const char *op = a[1];
        int holds = strcmp(op, "-eq") == 0 ? x == y : strcmp(op, "-ne") == 0 ? x != y :
                    strcmp(op, "-lt") == 0 ? x < y : strcmp(op, "-le") == 0 ? x <= y :
                    strcmp(op, "-gt") == 0 ? x > y : x >= y;
        result = holds ? 0 : 1;
    }
    if (result == 2) {
        fprintf(2, "sh: %s: cannot read the expression\n", name);
        return 2;
    }
    return negate ? !result : result;
}

/* Offer the names in directory `dir` that start with `start` to the line
 * editor, with '/' after directories when `mark_dirs`. Hidden names only
 * when `start` begins with '.'. */
static void offer_directory(lineedit_t *e, const char *dir, const char *start, int mark_dirs) {
    long fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    size_t n = strlen(start);
    struct dirent entry;
    while (readdir((int)fd, &entry) == 1) {
        if (strncmp(entry.name, start, n) != 0) continue;
        if (entry.name[0] == '.' && start[0] != '.') continue;
        if (mark_dirs && entry.type == DT_DIR) {
            char name[LINEEDIT_NAME];
            size_t len = strlen(entry.name);
            if (len + 2 > sizeof(name)) continue;
            memcpy(name, entry.name, len);
            name[len] = '/';
            name[len + 1] = '\0';
            lineedit_add_match(e, name);
        } else {
            lineedit_add_match(e, entry.name);
        }
    }
    close((int)fd);
}

/* Tab completion for the line editor (lineedit_complete_fn): a program
 * name from the built-ins and the directories of $PATH, or anything else
 * from the directory the word names (the current one if it has no '/') */
static void complete_names(lineedit_t *e, const char *word, int command) {
    const char *slash = NULL;
    for (const char *c = word; *c; c++) if (*c == '/') slash = c;

    if (command && !slash) {
        static const char *const builtins[] = { "bg", "cd", "exit", "export", "fg", "help", "jobs", "pwd", "unset" };
        size_t n = strlen(word);
        for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
            if (strncmp(builtins[i], word, n) == 0) lineedit_add_match(e, builtins[i]);
        }
        const char *path = getenv("PATH");
        while (path && *path) {                       /* Each directory of "dir:dir:..." */
            char dir[128];
            size_t len = 0;
            while (path[len] && path[len] != ':') len++;
            if (len > 0 && len < sizeof(dir)) {
                memcpy(dir, path, len);
                dir[len] = '\0';
                offer_directory(e, dir, word, 0);
            }
            path += len;
            if (*path == ':') path++;
        }
        return;
    }

    char dir[LINEEDIT_MAX];
    if (slash) {                                      /* "/" itself, or the part before the last '/' */
        size_t len = slash == word ? 1 : (size_t)(slash - word);
        memcpy(dir, word, len);
        dir[len] = '\0';
    } else {
        dir[0] = '.';
        dir[1] = '\0';
    }
    offer_directory(e, dir, slash ? slash + 1 : word, 1);
}

int main(int argc, char **argv) {
    int should_exit = 0;

    /* Ctrl-C should stop the programs the shell runs, not the shell */
    ignore_interrupt(1);

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        return run_line(argv[2], &should_exit);
    }

    /* "sh FILE": read commands from the file instead of the terminal. The
     * file gets its own descriptor rather than becoming standard input, so
     * that a command in the script which reads its input (cat, wc) reads the
     * terminal and not the rest of the script. */
    if (argc >= 2) {
        long fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "sh: %s: %s\n", argv[1], strerror(fd));
            return 127;
        }
        script_fd = (int)fd;
        shell_name = argv[1];                   /* $0, and $1 .. for the words after it */
        arguments = argv + 2;
        argument_count = argc - 2;
    }

    /* Reading the terminal: take charge of it. The shell gets a process
     * group of its own, which has the terminal whenever no command does,
     * and Ctrl-Z, like Ctrl-C, is not meant for the shell itself. (Reading
     * a pipe is not reading the terminal: no prompts, no job control.) */
    if (script_fd < 0 && isatty(0) == 1) {
        interactive = 1;
        setpgid(0, 0);
        shell_group = getpgid(0);
        tcsetpgrp(shell_group);
        signal(SIGTSTP, SIG_IGN);
    }

    /* At the terminal, lines are edited here, with a history (lineedit.h);
     * from a script or a pipe they are read as they come */
    static lineedit_t editor;
    editor.complete = complete_names;
    int editing = interactive;
    /* Lines are gathered in `pending` until they make a complete command:
     * an if, while or for may take several, and the prompt is then "> " */
    static char line[256];
    static char pending[4096];
    size_t pending_len = 0;
    while (!should_exit) {
        if (interactive && pending_len == 0) check_jobs();
        long got;
        if (editing) {
            got = lineedit_read(&editor, pending_len ? "> " : "$ ", line, sizeof(line));
        } else {
            got = fdreadline(script_fd < 0 ? 0 : script_fd, line, sizeof(line));
        }
        if (got < 0) {                                       /* End of input */
            if (pending_len) {
                fprintf(2, "sh: syntax error: the input ended inside a command\n");
                last_status = 2;
            }
            break;
        }

        if (pending_len + (size_t)got + 2 > sizeof(pending)) {
            fprintf(2, "sh: command too long\n");
            pending_len = 0;
            last_status = 2;
            continue;
        }
        memcpy(pending + pending_len, line, (size_t)got);
        pending_len += (size_t)got;
        pending[pending_len++] = '\n';
        pending[pending_len] = '\0';

        int status = run_text(pending, &should_exit, 1);
        if (status == RUN_INCOMPLETE) continue;               /* More lines to come */
        last_status = status;
        pending_len = 0;
    }
    return last_status;
}
