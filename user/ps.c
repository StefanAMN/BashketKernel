/* ps: list the tasks in the system, one line each, from /proc/<pid>/status.
 * The columns are those /proc/tasks has always had (PID, parent, user,
 * state, kind, priority, ticks, CPU, name), in order of PID. */
#include "ulib.h"

#define MAX_TASKS 256

/* The text after "key:\t" in a status file, up to the end of its line,
 * into out[size]; "?" if the key is missing */
static void field(const char *status, const char *key, char *out, size_t size) {
    size_t k = strlen(key);
    const char *p = status;
    while (*p) {
        if (strncmp(p, key, k) == 0 && p[k] == ':' && p[k + 1] == '\t') {
            p += k + 2;
            size_t n = 0;
            while (p[n] && p[n] != '\n' && n + 1 < size) n++;
            memcpy(out, p, n);
            out[n] = '\0';
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    out[0] = '?';
    out[1] = '\0';
}

/* Whether `name` is all digits (a /proc/<pid> directory) */
static int is_pid(const char *name) {
    if (!*name) return 0;
    for (; *name; name++) if (*name < '0' || *name > '9') return 0;
    return 1;
}

/* Print the line for one task, if it is still there. `pid` is its /proc
 * directory's name (the PID in decimal). */
static void print_task(const char *pid) {
    char path[40], status[1024];
    size_t n_pid = strlen(pid);
    if (n_pid > 20) return;
    memcpy(path, "/proc/", 6);
    memcpy(path + 6, pid, n_pid);
    memcpy(path + 6 + n_pid, "/status", 8);
    long fd = open(path, O_RDONLY);
    if (fd < 0) return;                            /* Gone since the listing */
    long n = read((int)fd, status, sizeof(status) - 1);
    close((int)fd);
    if (n <= 0) return;
    status[n] = '\0';

    char ppid[16], uid[16], state[16], kind[16], prio[16], ticks[24], cpu[16], name[40];
    field(status, "PPid", ppid, sizeof(ppid));
    field(status, "Uid", uid, sizeof(uid));
    field(status, "State", state, sizeof(state));
    field(status, "Kind", kind, sizeof(kind));
    field(status, "Priority", prio, sizeof(prio));
    field(status, "Ticks", ticks, sizeof(ticks));
    field(status, "Cpu", cpu, sizeof(cpu));
    field(status, "Name", name, sizeof(name));
    /* The state padded to seven characters, as /proc/tasks has it */
    size_t len = strlen(state);
    while (len < 7 && len + 1 < sizeof(state)) state[len++] = ' ';
    state[len] = '\0';
    printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", pid, ppid, uid, state, kind, prio, ticks, cpu, name);
}

/* `value` in decimal into out[24] */
static void decimal(long value, char out[24]) {
    char tmp[24];
    int n = 0;
    do { tmp[n++] = (char)('0' + value % 10); value /= 10; } while (value > 0 && n < 23);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

int main(void) {
    long fd = open("/proc", O_RDONLY);
    if (fd < 0) {
        fprintf(2, "ps: /proc: %s\n", strerror(fd));
        return 1;
    }
    static long pids[MAX_TASKS];
    int count = 0;
    struct dirent entry;
    while (readdir((int)fd, &entry) == 1 && count < MAX_TASKS) {
        if (entry.type != DT_DIR || !is_pid(entry.name)) continue;
        long pid = atoi(entry.name);
        /* Sorted as they come, and once each (the listing can repeat one
         * if the task list changes between two reads of it) */
        int at = count, seen = 0;
        for (int i = 0; i < count; i++) if (pids[i] == pid) seen = 1;
        if (seen) continue;
        while (at > 0 && pids[at - 1] > pid) {
            pids[at] = pids[at - 1];
            at--;
        }
        pids[at] = pid;
        count++;
    }
    close((int)fd);

    printf("PID\tPPID\tUID\tSTATE\tKIND\tPRIO\tTICKS\tCPU\tNAME\n");
    for (int i = 0; i < count; i++) {
        char pid[24];
        decimal(pids[i], pid);
        print_task(pid);
    }
    return 0;
}
