/**
 * @file process.h
 * @brief UNIX Process Subsystem (fork, execve, waitpid, signals)
 */

#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SIGINT  2
#define SIGKILL 9
#define SIGSEGV 11

typedef int pid_t;

/**
 * @brief Duplicate current process (fork).
 * @return 0 in child process, child PID in parent process, -1 on failure.
 */
pid_t process_fork(void);

/**
 * @brief Execute new ELF executable binary with environment array (execve).
 * @param path File path to binary.
 * @param argv Array of argument strings (or NULL).
 * @param envp Array of environment variable strings (or NULL).
 * @return 0 on success, -1 on failure.
 */
int process_execve_env(const char *path, char *const argv[], char *const envp[]);

/**
 * @brief Execute new ELF executable binary (execve convenience wrapper).
 * @param path File path to binary.
 * @return 0 on success, -1 on failure.
 */
int process_execve(const char *path);

/**
 * @brief Wait for child process termination (waitpid).
 * @param pid Process ID to wait for (-1 for any child).
 * @param status Pointer to store child exit status.
 * @return Child PID on success, -1 on failure.
 */
pid_t process_waitpid(pid_t pid, int *status);

/**
 * @brief Send UNIX signal to process.
 * @param pid Target process ID.
 * @param sig Signal number (e.g. SIGKILL).
 * @return 0 on success, -1 on failure.
 */
int process_kill(pid_t pid, int sig);

#endif // PROCESS_H
