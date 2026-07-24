/*
 * sh: a small command interpreter.
 *
 *   sh                 read commands from standard input until "exit"
 *   sh -c "command"    run one command line and exit with its status
 *
 * A command line is one or more programs joined by '|'. Each may redirect
 * its input with "< file" and its output with "> file" or ">> file".
 * A program name without a '/' is looked up in /bin.
 *
 * Ctrl-C ends the running programs and returns to the prompt; Ctrl-D at an
 * empty prompt leaves the shell.
 *
 * Everything here is done with fork, execve, waitpid, pipe, dup2, open and
 * close: the shell needs no special help from the kernel.
 */
#include "ulib.h"

#define MAX_ARGS   16
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
static char word_storage[512];

static int is_operator(char c) { return c == '|' || c == '<' || c == '>'; }
static int is_blank(char c)    { return c == ' ' || c == '\t'; }

/* Copy the next word from *cursor into word_storage. Handles "double quotes".
 * Returns the copy, or NULL if there is no word here or no room left. */
static char *next_word(const char **cursor, size_t *used) {
    const char *p = *cursor;
    while (is_blank(*p)) p++;

    char *word = word_storage + *used;
    size_t len = 0;
    int quoted = (*p == '"');
    if (quoted) p++;

    while (*p != '\0' && (quoted ? *p != '"' : (!is_blank(*p) && !is_operator(*p)))) {
        if (*used + len + 1 >= sizeof(word_storage)) return NULL;
        word[len++] = *p++;
    }
    if (quoted) {
        if (*p != '"') return NULL; /* Missing closing quote */
        p++;
    } else if (len == 0) {
        return NULL;
    }

    word[len] = '\0';
    *used += len + 1;
    *cursor = p;
    return word;
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
            if (!(stage->in_path = next_word(&p, &used))) return -1;
        } else if (*p == '>') {
            p++;
            stage->out_append = (*p == '>');
            if (stage->out_append) p++;
            if (!(stage->out_path = next_word(&p, &used))) return -1;
        } else {
            if (stage->argc >= MAX_ARGS) return -1;
            if (!(stage->argv[stage->argc] = next_word(&p, &used))) return -1;
            stage->argc++;
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

static void exec_stage(stage_t *stage) {
    ignore_interrupt(0); /* Unlike the shell, the program should die on Ctrl-C */

    if (stage->in_path) redirect_or_die(stage->in_path, O_RDONLY, 0);
    if (stage->out_path) {
        redirect_or_die(stage->out_path,
                        O_WRONLY | O_CREAT | (stage->out_append ? O_APPEND : O_TRUNC), 1);
    }

    char path[160];
    const char *name = stage->argv[0];
    if (strchr(name, '/')) {
        strcpy(path, strlen(name) < sizeof(path) ? name : "");
    } else {
        strcpy(path, "/bin/");
        if (strlen(name) < sizeof(path) - 5) strcpy(path + 5, name);
    }

    long err = execve(path, stage->argv);
    fprintf(2, "sh: %s: %s\n", name, err == -ENOENT ? "command not found" : strerror(err));
    exit(127);
}

/* Run a parsed pipeline and return the exit status of its last program. */
static int run_pipeline(stage_t *stages, int count) {
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
    }
    if (input != -1) close(input);

    int last_status = 1;
    for (int i = 0; i < count; i++) {
        int status = 0;
        waitpid(pids[i], &status);
        if (i == count - 1) last_status = status;
    }
    return last_status;
}

static int run_line(const char *line, int *should_exit) {
    static stage_t stages[MAX_STAGES];

    int count = parse(line, stages);
    if (count < 0) {
        fprintf(2, "sh: syntax error\n");
        return 2;
    }
    if (count == 0) return 0;

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
    if (count == 1 && strcmp(stages[0].argv[0], "pwd") == 0) {
        char cwd[128];
        if (getcwd(cwd, sizeof(cwd)) < 0) return 1;
        printf("%s\n", cwd);
        return 0;
    }
    if (count == 1 && strcmp(stages[0].argv[0], "help") == 0) {
        printf("Programs live in /bin (try: ls /bin). Syntax:\n"
               "  prog args | prog2      pipe output into another program\n"
               "  prog < in > out        redirect input and output (>> appends)\n"
               "  cd [dir], pwd          change or print the current directory\n"
               "  exit [status]          leave the shell\n");
        return 0;
    }

    return run_pipeline(stages, count);
}

int main(int argc, char **argv) {
    int should_exit = 0;

    /* Ctrl-C should stop the programs the shell runs, not the shell */
    ignore_interrupt(1);

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        return run_line(argv[2], &should_exit);
    }

    static char line[256];
    int status = 0;
    while (!should_exit) {
        printf("$ ");
        if (readline(line, sizeof(line)) < 0) break; /* End of input */
        status = run_line(line, &should_exit);
    }
    return status;
}
