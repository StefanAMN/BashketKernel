/* top: the processes using the CPU, refreshed every second.
 *
 *   top        q (or Ctrl-C) quits
 *
 * Each refresh reads every /proc/<pid>/status and keeps its "Ticks" (timer
 * ticks that found the task running); the difference from the previous
 * refresh, over the ticks that passed (uptime_ticks), is the share of one
 * CPU the task had in that interval. Processes are listed busiest first.
 * SIZE is "Size" from the same file: the program, heap and mapped regions
 * (what was asked for); RES is "Resident", the pages of it present in
 * memory now. Above the list: each CPU's load from the
 * "busy ticks" lines of /proc/cpuinfo, and the totals of /proc/meminfo.
 *
 * The terminal is in raw mode while it runs, so that a key arrives at once
 * and poll() can wait for a key or the next refresh, whichever comes first.
 */
#include "ulib.h"

#define MAX_TASKS 256
#define MAX_CPUS 32
#define REFRESH_MS 1000

typedef struct {
    long pid, uid, ticks, cpu, size_kb, resident_kb;
    long delta;                /* Ticks in the last interval */
    char state[12], name[24];
} task_info_t;

static task_info_t tasks[MAX_TASKS], previous[MAX_TASKS];
static int task_count = 0, previous_count = 0;
static long cpu_busy[MAX_CPUS], cpu_busy_before[MAX_CPUS];
static int cpu_count = 0;
static unsigned columns = 80, rows = 24;

static char frame[16384];          /* One screen, written at once */
static size_t frame_len = 0;

/* --- Building the frame --------------------------------------------------- */

/* Append `s` to the frame (what does not fit is dropped) */
static void put(const char *s) {
    while (*s && frame_len < sizeof(frame)) frame[frame_len++] = *s++;
}

/* Append `value` in decimal, right-aligned in `width` columns */
static void put_number(long value, int width) {
    char digits[24];
    int n = 0, negative = value < 0;
    unsigned long v = negative ? (unsigned long)-value : (unsigned long)value;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v > 0 && n < 22);
    if (negative) digits[n++] = '-';
    for (int pad = width - n; pad > 0; pad--) put(" ");
    char out[24];
    for (int i = 0; i < n; i++) out[i] = digits[n - 1 - i];
    out[n] = '\0';
    put(out);
}

/* Append `s` left-aligned in `width` columns, cut if longer */
static void put_padded(const char *s, int width) {
    int n = 0;
    for (; s[n] && n < width && frame_len < sizeof(frame); n++) frame[frame_len++] = s[n];
    for (; n < width; n++) put(" ");
}

/* --- Reading /proc ----------------------------------------------------------- */

/* Read the whole file at `path` into out[size] (NUL-terminated); its length, or -1 */
static long slurp(const char *path, char *out, size_t size) {
    long fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long total = 0, n;
    while (total < (long)size - 1 && (n = read((int)fd, out + total, size - 1 - (size_t)total)) > 0) total += n;
    close((int)fd);
    out[total] = '\0';
    return total;
}

/* The text after "key:" and any blanks, in `text`, up to the end of its
 * line, into out[size]; "?" if the key is missing */
static void field(const char *text, const char *key, char *out, size_t size) {
    size_t k = strlen(key);
    for (const char *p = text; *p; ) {
        if (strncmp(p, key, k) == 0 && p[k] == ':') {
            p += k + 1;
            while (*p == ' ' || *p == '\t') p++;
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

/* The number at the start of the value of "key:" in `text` (0 if missing) */
static long number_field(const char *text, const char *key) {
    char value[32];
    field(text, key, value, sizeof(value));
    return atoi(value);
}

/* Whether `name` is all digits (a /proc/<pid> directory) */
static int is_pid(const char *name) {
    if (!*name) return 0;
    for (; *name; name++) if (*name < '0' || *name > '9') return 0;
    return 1;
}

/* Fill t from /proc/<name>/status; whether the task was still there */
static int read_task(const char *name, task_info_t *t) {
    char path[40], status[1024];
    size_t n = strlen(name);
    if (n > 20) return 0;
    memcpy(path, "/proc/", 6);
    memcpy(path + 6, name, n);
    memcpy(path + 6 + n, "/status", 8);
    if (slurp(path, status, sizeof(status)) <= 0) return 0;   /* Gone since the listing */
    t->pid = atoi(name);
    t->uid = number_field(status, "Uid");
    t->ticks = number_field(status, "Ticks");
    t->cpu = number_field(status, "Cpu");
    t->size_kb = number_field(status, "Size");
    t->resident_kb = number_field(status, "Resident");
    field(status, "State", t->state, sizeof(t->state));
    field(status, "Name", t->name, sizeof(t->name));
    return 1;
}

/* Take a new sample of every task and CPU; the old one becomes `previous`
 * and each task's delta is its ticks since then (all of them if it is new) */
static void sample(void) {
    memcpy(previous, tasks, sizeof(tasks[0]) * (size_t)task_count);
    previous_count = task_count;
    task_count = 0;

    long fd = open("/proc", O_RDONLY);
    if (fd >= 0) {
        struct dirent entry;
        while (readdir((int)fd, &entry) == 1 && task_count < MAX_TASKS) {
            if (entry.type != DT_DIR || !is_pid(entry.name)) continue;
            task_info_t *t = &tasks[task_count];
            if (!read_task(entry.name, t)) continue;
            int duplicate = 0;                  /* (A listing can repeat one if the task list changes) */
            for (int i = 0; i < task_count; i++) if (tasks[i].pid == t->pid) duplicate = 1;
            if (duplicate) continue;
            t->delta = t->ticks;
            for (int i = 0; i < previous_count; i++) {
                if (previous[i].pid == t->pid && previous[i].ticks <= t->ticks) t->delta = t->ticks - previous[i].ticks;
            }
            task_count++;
        }
        close((int)fd);
    }

    /* "cpuN:   123 busy ticks", one line per CPU */
    static char text[4096];
    memcpy(cpu_busy_before, cpu_busy, sizeof(cpu_busy));
    cpu_count = 0;
    if (slurp("/proc/cpuinfo", text, sizeof(text)) > 0) {
        for (const char *p = text; *p && cpu_count < MAX_CPUS; ) {
            if (strncmp(p, "cpu", 3) == 0 && p[3] >= '0' && p[3] <= '9') {
                const char *colon = p;
                while (*colon && *colon != ':' && *colon != '\n') colon++;
                if (*colon == ':') {
                    colon++;
                    while (*colon == ' ') colon++;
                    cpu_busy[cpu_count++] = atoi(colon);
                }
            }
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
    }
}

/* Busiest first; equal ones by PID */
static void sort_tasks(void) {
    for (int i = 1; i < task_count; i++) {
        task_info_t t = tasks[i];
        int j = i;
        while (j > 0 && (tasks[j - 1].delta < t.delta || (tasks[j - 1].delta == t.delta && tasks[j - 1].pid > t.pid))) {
            tasks[j] = tasks[j - 1];
            j--;
        }
        tasks[j] = t;
    }
}

/* Percentage of `part` in `whole`, rounded, 0 if `whole` is 0 */
static long percent(long part, long whole) {
    return whole > 0 ? (part * 100 + whole / 2) / whole : 0;
}

/* --- Drawing ---------------------------------------------------------------- */

/* Draw the screen for an interval of `elapsed` ticks */
static void draw(long elapsed) {
    frame_len = 0;
    put("\033[H");

    /* The load of each CPU, as many to a line as fit */
    put("top: ");
    put_number(task_count, 0);
    put(" processes, ");
    put_number(cpu_count, 0);
    put(" CPUs, interval ");
    put_number(elapsed * 10, 0);
    put(" ms   (q quits)\033[K\r\n");
    unsigned lines = 1, used = 0;
    for (int i = 0; i < cpu_count; i++) {
        if (used + 12 > columns) {
            put("\033[K\r\n");
            lines++;
            used = 0;
        }
        put("cpu");
        put_number(i, i < 10 ? 1 : 2);
        put(i < 10 ? " " : "");
        put_number(percent(cpu_busy[i] - cpu_busy_before[i], elapsed), 4);
        put("%  ");
        used += 12;
    }
    put("\033[K\r\n");
    lines++;

    static char meminfo[2048];
    if (slurp("/proc/meminfo", meminfo, sizeof(meminfo)) > 0) {
        put("mem: ");
        put_number(number_field(meminfo, "MemTotal") / 1024, 0);
        put(" MiB, ");
        put_number(number_field(meminfo, "MemFree") / 1024, 0);
        put(" free, ");
        put_number(number_field(meminfo, "Cached") / 1024, 0);
        put(" cached   swap: ");
        put_number(number_field(meminfo, "SwapTotal") / 1024, 0);
        put(" MiB, ");
        put_number(number_field(meminfo, "SwapFree") / 1024, 0);
        put(" free");
    }
    put("\033[K\r\n\033[K\r\n");
    lines += 2;

    put("\033[30;47m");
    put_padded("  PID   UID STATE     CPU%  ON  SIZE kB   RES kB  NAME", (int)columns);
    put("\033[0m\r\n");
    lines++;

    for (int i = 0; i < task_count && lines < rows; i++, lines++) {
        task_info_t *t = &tasks[i];
        put_number(t->pid, 5);
        put_number(t->uid, 6);
        put(" ");
        put_padded(t->state, 8);
        /* (One that was not there last time, or was missed, has all its
         * ticks as its delta; it cannot have had more than the interval) */
        put_number(percent(t->delta < elapsed ? t->delta : elapsed, elapsed), 5);
        put_number(t->cpu, 4);
        put_number(t->size_kb, 9);
        put_number(t->resident_kb, 9);
        put("  ");
        put_padded(t->name, (int)columns > 55 ? (int)columns - 55 : 1);
        put("\033[K\r\n");
    }
    put("\033[J");                                /* Clear what an earlier, longer list left */
    write(1, frame, frame_len);
}

int main(void) {
    tty_size(&columns, &rows);
    if (columns > 120) columns = 120;
    if (columns < 50 || rows < 8) {
        fprintf(2, "top: the screen is too small\n");
        return 1;
    }

    tty_raw(1);
    write(1, "\033[2J", 4);

    sample();
    long last = uptime_ticks();
    long due = last + 50;                         /* The first interval: soon, but long enough to measure */
    int quit = 0;
    while (!quit) {
        long now = uptime_ticks();
        if (now >= due) {
            sample();
            sort_tasks();
            draw(now - last > 0 ? now - last : 1);
            last = now;
            due = now + REFRESH_MS / 10;
            continue;
        }
        /* A key, or the time for the next refresh, whichever comes first
         * (another key only shortens the wait: the refresh stays due) */
        struct pollfd key = { 0, POLLIN, 0 };
        if (poll(&key, 1, (due - now) * 10) > 0) {
            char buf[16];
            long n = read(0, buf, sizeof(buf));
            for (long i = 0; i < n; i++) if (buf[i] == 'q' || buf[i] == 'Q' || buf[i] == 3) quit = 1;
            if (n <= 0) quit = 1;                 /* End of input: nobody can press q */
        }
    }

    tty_raw(0);
    write(1, "\033[0m\033[2J\033[H", 11);
    return 0;
}
