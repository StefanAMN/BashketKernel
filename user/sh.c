/*
 * sh: a small command interpreter.
 *
 *   sh                 read commands from standard input until "exit"
 *   sh -c "command"    run one command line and exit with its status
 *   sh FILE            run the commands in FILE (lines starting with # are comments)
 *
 * A command line is one or more pipelines separated by ';' (one after the
 * other), '&&' (the next only if this one succeeded) or '||' (only if it
 * failed). A pipeline is one or more programs joined by '|'. Each may redirect
 * its input with "< file" and its output with "> file" or ">> file".
 * A program name without a '/' is looked up in the directories of $PATH.
 * A word may join plain, "double quoted" ($ expanded) and 'single quoted'
 * (taken as it is) parts. $(command) is replaced by what the command
 * prints: one word inside double quotes, split at blanks outside them. A word with *, ? or [...] and no quoted part is
 * replaced by the names it matches, sorted (user/lib/glob.h); one that
 * matches nothing stays as typed.
 * $NAME expands to an environment variable and $? to the last exit status;
 * "export NAME=value" sets one for the shell and everything it starts.
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

#define MAX_ARGS   64
#define MAX_STAGES 8

typedef struct {
    char *argv[MAX_ARGS + 1];
    int argc;
    char *in_path;      /* "< file", or NULL */
    char *out_path;     /* "> file" or ">> file", or NULL */
    int out_append;
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
        value = getenv(name);
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
            for (long i = 0; i < n; i++) {
                char o = output[i];
                if (!quote && (is_blank(o) || o == '\n' || o == FIELD_BREAK)) o = FIELD_BREAK;
                if (*used + len + 1 >= sizeof(word_storage)) return NULL;
                word[len++] = o;
            }
            if (!quote) split = 1;
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

/* Split `line` into pipeline stages. Returns the number of stages, 0 for an
 * empty line, -1 on a syntax error. */
static int parse(const char *line, stage_t *stages) {
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
        } else if (*p == '<') {
            p++;
            if (!(stage->in_path = redirect_target(&p, &used))) return -1;
        } else if (*p == '>') {
            p++;
            stage->out_append = (*p == '>');
            if (stage->out_append) p++;
            if (!(stage->out_path = redirect_target(&p, &used))) return -1;
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

    if (stage->argc == 0) return count == 0 ? 0 : -1; /* Empty line, or "a |" */
    return count + 1; /* argv arrays are NULL-terminated by the memset */
}

/* In the child: point fd `target` at `path`, or exit. */
static void redirect_or_die(const char *path, int flags, int target) {
    long fd = open(path, flags);
    if (fd < 0) {
        fprintf(2, "sh: %s: %s\n", path, strerror(fd));
        exit(1);
    }
    dup2((int)fd, target);
    close((int)fd);
}

/* In a child: apply a stage's redirections and run its program. Never
 * returns (exits 127 if the program could not be run). */
static void exec_stage(stage_t *stage) {
    ignore_interrupt(0); /* Unlike the shell, the program should die on Ctrl-C */
    if (script_fd >= 0) close(script_fd); /* The script is the shell's business, not the program's */

    if (stage->in_path) redirect_or_die(stage->in_path, O_RDONLY, 0);
    if (stage->out_path) {
        redirect_or_die(stage->out_path,
                        O_WRONLY | O_CREAT | (stage->out_append ? O_APPEND : O_TRUNC), 1);
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

    int count = parse(line, stages);
    if (count < 0) {
        fprintf(2, "sh: syntax error\n");
        return 2;
    }
    if (count == 0) return last_status;   /* Nothing to run: $? as it is (or as a $(...) left it) */

    /* Built-ins run inside the shell itself */
    if (count == 1 && strcmp(stages[0].argv[0], "exit") == 0) {
        *should_exit = 1;
        return stages[0].argc > 1 ? (int)atoi(stages[0].argv[1]) : 0;
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
    /* export NAME=value: change the shell's own environment, which every
     * program started afterwards inherits */
    if (count == 1 && strcmp(stages[0].argv[0], "export") == 0) {
        for (int i = 1; i < stages[0].argc; i++) {
            char *assignment = stages[0].argv[i];
            char *equals = strchr(assignment, '=');
            if (!equals || equals == assignment) {
                fprintf(2, "sh: export: expected NAME=value, got '%s'\n", assignment);
                return 1;
            }
            *equals = '\0';
            if (setenv(assignment, equals + 1) != 0) return 1;
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
               "  prog < in > out        redirect input and output (>> appends)\n"
               "  ls *.c /tmp/f?         wildcards: * ? [abc] [a-z] [!x] (not in a word with quotes)\n"
               "  \"$HOME/x\" '$HOME'     double quotes expand $NAME, single quotes take it as it is\n"
               "  echo $(date)           the output of a command, in place (up to 4 KiB)\n"
               "  cd [dir], pwd          change or print the current directory\n"
               "  export NAME=value      set an environment variable; $NAME uses it, $? is the last status\n"
               "  prog args &            run in the background; Ctrl-Z stops the running command\n"
               "  jobs, fg [N], bg [N]   list jobs; bring one to the terminal; let a stopped one run on\n"
               "  exit [status]          leave the shell\n");
        return 0;
    }

    return run_pipeline(stages, count, background, text);
}

/* Run a command line: pipelines separated by ';' (run the next whatever
 * happened), '&&' (run it only if the last one succeeded) and '||' (only if
 * it failed). A part that is not run leaves the status as it was, so
 * "a && b || c" runs c when a or b fails. $? is up to date for each part.
 * Separators inside quotes (either kind) are text. Returns the status of the last
 * part run; stops at "exit". */
static int run_line(const char *line, int *should_exit) {
    enum { ALWAYS, IF_OK, IF_FAILED } condition = ALWAYS;
    int status = last_status;
    const char *p = line;
    while (*p) {
        /* The part up to the next separator that is not inside quotes */
        char part[256];
        size_t n = 0;
        int quoted = 0, next = ALWAYS;
        int depth = 0;                                                   /* Inside $( ... ) */
        for (; *p; p++) {
            if (!quoted && (*p == '"' || *p == '\'')) quoted = *p;      /* Into a quote ... */
            else if (quoted && *p == quoted) quoted = 0;                /* ... and out of it */
            else if (quoted != '\'' && p[0] == '$' && p[1] == '(') depth++;
            else if (!quoted && depth > 0 && *p == ')') depth--;
            if (!quoted && depth == 0 && (*p == ';' || (p[0] == '&' && p[1] == '&') || (p[0] == '|' && p[1] == '|'))) break;
            if (n + 1 < sizeof(part)) part[n++] = *p;
        }
        part[n] = '\0';
        if (*p == ';') { next = ALWAYS; p++; }
        else if (*p) { next = (*p == '&') ? IF_OK : IF_FAILED; p += 2; }

        int empty = 1;
        for (size_t i = 0; i < n; i++) if (!is_blank(part[i]) && part[i] != '\n') empty = 0;
        if (empty && next != ALWAYS) {                     /* "&& b" or "a && && b" */
            fprintf(2, "sh: syntax error\n");
            return last_status = 2;
        }
        if (condition == ALWAYS || (condition == IF_OK && status == 0) || (condition == IF_FAILED && status != 0)) {
            if (!empty) status = last_status = run_pipeline_text(part, should_exit);
            if (*should_exit) break;
        }
        condition = next;
    }
    return status;
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
        static const char *const builtins[] = { "bg", "cd", "exit", "export", "fg", "help", "jobs", "pwd" };
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
    static char line[256];
    while (!should_exit) {
        if (interactive) check_jobs();
        long got;
        if (editing) {
            got = lineedit_read(&editor, "$ ", line, sizeof(line));
        } else {
            got = fdreadline(script_fd < 0 ? 0 : script_fd, line, sizeof(line));
        }
        if (got < 0) break; /* End of input */

        /* Skip leading blanks; a line starting with # is a comment */
        const char *command = line;
        while (is_blank(*command)) command++;
        if (*command == '#') continue;

        last_status = run_line(command, &should_exit);
    }
    return last_status;
}
