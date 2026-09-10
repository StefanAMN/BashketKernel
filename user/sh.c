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
 * with break [N] and continue [N] inside loops, and the built-in test
 * EXPR (or [ EXPR ]) for conditions: -e -f -d PATH, -z -n STRING, a = b,
 * a != b, x -eq -ne -lt -le -gt -ge y, and ! to turn one round. Ctrl-C
 * ends a loop as well as the command it was running.
 *
 * A word may join plain, "double quoted" ($ expanded) and 'single quoted'
 * (taken as it is) parts. $(command) is replaced by what the command
 * prints: one word inside double quotes, split at blanks outside them. A
 * word with *, ? or [...] and no quoted part is replaced by the names it
 * matches, sorted (user/lib/glob.h); one that matches nothing stays as
 * typed.
 * $NAME expands to a variable and $? to the last exit status. "NAME=value"
 * on its own sets a variable of the shell, which programs do not see;
 * "export NAME=value" (or "export NAME" for one already set) puts it in
 * the environment, which the shell and everything it starts share; "unset
 * NAME" forgets it. "NAME=value program" sets it in that program's
 * environment only. A value is expanded like any word, but is neither
 * split nor matched against file names: X=$(ls) keeps the newlines, X=*
 * a star. A for loop's variable is the shell's own.
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

enum { REDIRECT_IN, REDIRECT_OUT, REDIRECT_APPEND, REDIRECT_COPY };

typedef struct {
    int kind;           /* REDIRECT_* */
    int fd;             /* The descriptor it changes */
    int from;           /* REDIRECT_COPY: the descriptor copied */
    char *path;         /* The others: the file opened */
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

/* Append the value of the variable at *p ($NAME or $?, *p just after the
 * '$') to `word` (length *len, in word_storage from `used`), and move *p
 * past its name. An unset variable is nothing. 0, or -1 if it does not fit. */
static int append_variable(const char **p, char *word, size_t *len, size_t used) {
    char name[32], status_text[12];
    const char *value;
    size_t n = 0;
    if (**p == '?') {
        (*p)++;
        int v = last_status, digits = 0;
        char tmp[12];
        do { tmp[digits++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        for (int i = 0; i < digits; i++) status_text[i] = tmp[digits - 1 - i];
        status_text[digits] = '\0';
        value = status_text;
    } else {
        while (is_name_char(**p) && n < sizeof(name) - 1) name[n++] = *(*p)++;
        name[n] = '\0';
        value = get_variable(name);
    }
    for (; value && *value; value++) {
        if (used + *len + 1 >= sizeof(word_storage)) return -1;
        word[(*len)++] = *value;
    }
    return 0;
}

static int run_line(const char *line, int *should_exit);

#define WORD_QUOTED 1         /* next_word: some part was in quotes */
#define WORD_SPLIT  2         /* next_word: holds FIELD_BREAKs, to be split into words */
#define FIELD_BREAK '\x1f'    /* Where unquoted $(...) output had blanks (0x1f in output counts as one) */
#define SUBST_MAX   4096      /* Most output a $(...) may give */

/* Set while next_word reads NAME=value: the value is one word whatever a
 * $(...) in it prints, as in other Bourne shells (X=$(ls) keeps the
 * newlines) */
static int reading_assignment = 0;
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
        reading_assignment = 0;               /* (Its own words split as usual) */
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

/* The next word of the line at *cursor, stored in word_storage from *used
 * on; moves *cursor past it. A word runs to a blank or an operator outside
 * quotes, and may be made of parts: plain text, "double quoted" ($NAME and
 * $? expanded, blanks and operators kept) and 'single quoted' (everything
 * kept as it is): `"a b"c` is one word, `a b c`. NULL if no word starts
 * here (the end of the line, an operator), a quote is not closed, or the
 * line's words do not fit in word_storage. $(command) is replaced by the
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
                if (*used + len + 1 >= sizeof(word_storage)) return NULL;
                word[len++] = o;
            }
            if (!whole) split = 1;
        } else if (c == '$' && quote != '\'' && (is_name_char(p[1]) || p[1] == '?')) {
            p++;
            if (append_variable(&p, word, &len, *used) < 0) return NULL;
        } else {
            if (*used + len + 1 >= sizeof(word_storage)) return NULL;
            word[len++] = c;
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

/* In a child: apply a stage's redirections and run its program. Never
 * returns (exits 127 if the program could not be run). */
static void exec_stage(stage_t *stage) {
    ignore_interrupt(0); /* Unlike the shell, the program should die on Ctrl-C */
    if (script_fd >= 0) close(script_fd); /* The script is the shell's business, not the program's */

    for (int i = 0; i < stage->redirect_count; i++) {
        redirect_t *r = &stage->redirects[i];
        if (r->kind == REDIRECT_IN) {
            redirect_or_die(r->path, O_RDONLY, r->fd);
        } else if (r->kind == REDIRECT_OUT) {
            redirect_or_die(r->path, O_WRONLY | O_CREAT | O_TRUNC, r->fd);
        } else if (r->kind == REDIRECT_APPEND) {
            redirect_or_die(r->path, O_WRONLY | O_CREAT | O_APPEND, r->fd);
        } else if (dup2(r->from, r->fd) < 0) {
            fprintf(2, "sh: %d: bad file descriptor\n", r->from);
            exit(1);
        }
    }

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
    int count = parse(line, stages, 1);
    if (count < 0) {
        fprintf(2, "sh: syntax error\n");
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
    if (count == 1 && (strcmp(stages[0].argv[0], "test") == 0 || strcmp(stages[0].argv[0], "[") == 0)) {
        return test_builtin(stages[0].argc, stages[0].argv);
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
               "  [ -f FILE ], [ $x = y ], [ 3 -lt 4 ], [ ! ... ]   conditions (test); break, continue\n"
               "  prog < in > out        redirect input and output (>> appends)\n"
               "  ls *.c /tmp/f?         wildcards: * ? [abc] [a-z] [!x] (not in a word with quotes)\n"
               "  \"$HOME/x\" '$HOME'     double quotes expand $NAME, single quotes take it as it is\n"
               "  echo $(date)           the output of a command, in place (up to 4 KiB)\n"
               "  cd [dir], pwd          change or print the current directory\n"
               "  NAME=value             set a variable of the shell; $NAME uses it, $? is the last status\n"
               "  export NAME[=value]    put it in the environment, for every program started after\n"
               "  NAME=value prog        set it for that one program only; unset NAME forgets it\n"
               "  prog args &            run in the background; Ctrl-Z stops the running command\n"
               "  jobs, fg [N], bg [N]   list jobs; bring one to the terminal; let a stopped one run on\n"
               "  exit [status]          leave the shell\n");
        return 0;
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
enum { NODE_SIMPLE, NODE_IF, NODE_WHILE, NODE_UNTIL, NODE_FOR };
#define RUN_INCOMPLETE (-1)                             /* run_text: more lines are needed */

typedef struct segment {
    char *text;               /* What is left of it to parse (inside `storage`) */
    char *storage;            /* malloc'd copy of the segment */
    int join_next;            /* JOIN_*: what its end separator says about the next command */
} segment_t;

typedef struct node node_t;
typedef struct {
    node_t **items;
    int *joins;               /* joins[i]: how items[i] follows items[i - 1] (JOIN_*) */
    int count, cap;
} cmd_list_t;

struct node {
    int kind;                 /* NODE_* */
    char *text;               /* NODE_SIMPLE: the command; NODE_FOR: the words to loop over */
    char name[32];            /* NODE_FOR: the variable */
    cmd_list_t cond, body;    /* if: condition, then-part; while, until: condition, body; for: body */
    cmd_list_t other;         /* if: the else-part (an elif is an else holding one if) */
};

static segment_t *segs;       /* The segments being parsed, and where the parser is */
static int seg_count, seg_pos, parse_state;
static int loop_depth;        /* Loops being run (for break and continue) */
static int pending_break, pending_continue;   /* Loops still to leave, or to continue */
static int interrupted;       /* A command was ended by Ctrl-C: loops stop */

/* Cut `text` into segments (segs, seg_count). Returns PARSE_OK, or
 * PARSE_SYNTAX for an empty command before '&&' or '||' (or no memory), or
 * PARSE_INCOMPLETE for a text that ends with '&&' or '||'. A '#' starting a
 * word outside quotes comments out the rest of its line. */
static int cut_segments(const char *text) {
    int cap = 0, join = JOIN_ALWAYS;
    seg_count = seg_pos = 0;
    segs = NULL;
    const char *p = text;
    while (*p) {
        char quoted = 0;
        int depth = 0;
        const char *start = p;
        size_t n = 0;
        char *buf = malloc(strlen(p) + 1);
        if (!buf) return PARSE_SYNTAX;
        for (; *p; p++) {
            if (!quoted && depth == 0 && *p == '#' && (p == start || is_blank(p[-1]) || p[-1] == '\n')) {
                while (*p && *p != '\n') p++;              /* A comment, to the end of the line */
                if (!*p) break;
            }
            if (!quoted && (*p == '"' || *p == '\'')) quoted = *p;
            else if (quoted && *p == quoted) quoted = 0;
            else if (quoted != '\'' && p[0] == '$' && p[1] == '(') depth++;
            else if (!quoted && depth > 0 && *p == ')') depth--;
            if (!quoted && depth == 0 && (*p == ';' || *p == '\n' || (p[0] == '&' && p[1] == '&') || (p[0] == '|' && p[1] == '|'))) break;
            buf[n++] = *p;
        }
        buf[n] = '\0';
        int next = JOIN_ALWAYS;
        if (*p == ';' || *p == '\n') p++;
        else if (*p) { next = (*p == '&') ? JOIN_IF_OK : JOIN_IF_FAILED; p += 2; }

        char *t = buf;
        while (is_blank(*t)) t++;
        size_t len = strlen(t);
        while (len > 0 && is_blank(t[len - 1])) t[--len] = '\0';
        if (len == 0) {                                     /* Nothing here */
            free(buf);
            if (next != JOIN_ALWAYS) return PARSE_SYNTAX;   /* "&& b", "a && && b" */
            continue;                                       /* (A newline after "&&" keeps the "&&") */
        }
        if (seg_count == cap) {
            cap = cap ? cap * 2 : 16;
            segment_t *grown = malloc(sizeof(segment_t) * (size_t)cap);
            if (!grown) { free(buf); return PARSE_SYNTAX; }
            if (segs) memcpy(grown, segs, sizeof(segment_t) * (size_t)seg_count);
            free(segs);
            segs = grown;
        }
        segs[seg_count].storage = buf;
        segs[seg_count].text = t;
        segs[seg_count].join_next = next;
        seg_count++;
        join = next;
    }
    return join == JOIN_ALWAYS ? PARSE_OK : PARSE_INCOMPLETE;
}

/* Free the segments */
static void free_segments(void) {
    for (int i = 0; i < seg_count; i++) free(segs[i].storage);
    free(segs);
    segs = NULL;
    seg_count = seg_pos = 0;
}

/* The first word of `text` into word[32] (blank-separated; "" if longer) */
static void first_word(const char *text, char word[32]) {
    size_t n = 0;
    while (text[n] && !is_blank(text[n])) n++;
    if (n >= 32) n = 0;
    memcpy(word, text, n);
    word[n] = '\0';
}

/* Whether `word` is one of the keywords that end or divide a compound command */
static int is_closing_keyword(const char *word) {
    static const char *const closing[] = { "then", "elif", "else", "fi", "do", "done" };
    for (unsigned i = 0; i < sizeof(closing) / sizeof(closing[0]); i++) {
        if (strcmp(word, closing[i]) == 0) return 1;
    }
    return 0;
}

/* If the current segment starts with keyword `kw`, take the keyword off it
 * (and the segment, if nothing is left) and return 1; else 0. With
 * `alone`, anything after the keyword is a syntax error ("fi x"). */
static int take_keyword(const char *kw, int alone) {
    if (seg_pos >= seg_count) return 0;
    char word[32];
    first_word(segs[seg_pos].text, word);
    if (strcmp(word, kw) != 0) return 0;
    char *rest = segs[seg_pos].text + strlen(kw);
    while (is_blank(*rest)) rest++;
    if (*rest == '\0') seg_pos++;
    else if (alone) parse_state = PARSE_SYNTAX;
    else segs[seg_pos].text = rest;
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
    free_list(&node->cond);
    free_list(&node->body);
    free_list(&node->other);
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
            expect_keyword("fi", 1);
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
    expect_keyword("done", 1);
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
    expect_keyword("done", 1);
    if (parse_state == PARSE_OK && node->body.count == 0) parse_state = PARSE_SYNTAX;
    if (parse_state != PARSE_OK) { free_node(node); return NULL; }
    return node;
}

/* One command: a simple one, or an if/while/for (whose keyword starts the
 * current segment). The node, or NULL with parse_state set. */
static node_t *parse_command(void) {
    char word[32];
    first_word(segs[seg_pos].text, word);
    if (take_keyword("if", 0)) return parse_if();
    if (take_keyword("while", 0)) return parse_while(NODE_WHILE);
    if (take_keyword("until", 0)) return parse_while(NODE_UNTIL);
    if (strcmp(word, "for") == 0) return parse_for();

    node_t *node = new_node(NODE_SIMPLE);
    if (!node) return NULL;
    size_t len = strlen(segs[seg_pos].text);
    node->text = malloc(len + 1);
    if (!node->text) { free_node(node); parse_state = PARSE_SYNTAX; return NULL; }
    memcpy(node->text, segs[seg_pos].text, len + 1);
    seg_pos++;
    return node;
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
        node_t *node = parse_command();
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

/* Run one command of the tree; its status. An if whose branches are not
 * taken, and a loop that never runs its body, give 0. */
static int exec_node(node_t *node, int *should_exit) {
    if (node->kind == NODE_SIMPLE) {
        int status = run_pipeline_text(node->text, should_exit);
        if (status == 128 + SIGINT) interrupted = 1;        /* Ctrl-C: no further loop rounds */
        return status;
    }
    if (node->kind == NODE_IF) {
        int cond = exec_list(&node->cond, should_exit);
        if (*should_exit || interrupted || pending_break || pending_continue) return cond;
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
 * pending break or continue, or Ctrl-C. Returns the last status; $? is up
 * to date after each command. */
static int exec_list(cmd_list_t *list, int *should_exit) {
    int status = last_status;
    for (int i = 0; i < list->count; i++) {
        int join = list->joins[i];
        if (join == JOIN_ALWAYS || (join == JOIN_IF_OK && status == 0) || (join == JOIN_IF_FAILED && status != 0)) {
            status = last_status = exec_node(list->items[i], should_exit);
        }
        if (*should_exit || interrupted || pending_break || pending_continue) break;
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
    pending_break = pending_continue = 0;
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
