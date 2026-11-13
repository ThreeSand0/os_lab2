#include "proclaunch.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char *build_command_line(const char *const argv[]) {
    size_t total_len = 1;
    for (size_t i = 0; argv[i] != NULL; ++i) {
        const char *arg = argv[i];
        int need_quotes = (arg[0] == '\0' || strpbrk(arg, " \t\n\v\"") != NULL);
        if (need_quotes) total_len += 2;
        for (const char *p = arg; *p != '\0'; ++p) {
            if (*p == '\\' || *p == '"') total_len += 2;
            else total_len += 1;
        }
        total_len += 1;
    }

    char *cmd = (char *)malloc(total_len);
    if (!cmd) return NULL;

    char *out = cmd;
    for (size_t i = 0; argv[i] != NULL; ++i) {
        if (i > 0) *out++ = ' ';
        const char *arg = argv[i];
        int need_quotes = (arg[0] == '\0' || strpbrk(arg, " \t\n\v\"") != NULL);

        if (!need_quotes) {
            size_t len = strlen(arg);
            memcpy(out, arg, len);
            out += len;
        } else {
            *out++ = '"';
            const char *p = arg;
            while (*p != '\0') {
                size_t num_bs = 0;
                while (*p == '\\') {
                    ++num_bs;
                    ++p;
                }
                if (*p == '\0') {
                    for (size_t j = 0; j < num_bs * 2; ++j) *out++ = '\\';
                    break;
                } else if (*p == '"') {
                    for (size_t j = 0; j < num_bs * 2 + 1; ++j) *out++ = '\\';
                    *out++ = '"';
                    ++p;
                } else {
                    for (size_t j = 0; j < num_bs; ++j) *out++ = '\\';
                    *out++ = *p++;
                }
            }
            *out++ = '"';
        }
    }
    *out = '\0';
    return cmd;
}

int pl_start(const char *const argv[], pl_process *proc) {
    if (!argv || !argv[0] || !proc) return -1;

    char *cmd = build_command_line(argv);
    if (!cmd) return -1;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmd);

    if (!ok) {
        proc->handle = 0;
        proc->pid = -1;
        return -1;
    }

    CloseHandle(pi.hThread);
    proc->handle = (intptr_t)pi.hProcess;
    proc->pid = (long)pi.dwProcessId;
    return 0;
}

int pl_wait(pl_process *proc, int *exit_code) {
    if (!proc || proc->handle == 0) return -1;
    HANDLE h = (HANDLE)proc->handle;

    if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0) {
        return -1;
    }

    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) {
        CloseHandle(h);
        proc->handle = 0;
        return -1;
    }

    CloseHandle(h);
    proc->handle = 0;
    if (exit_code) *exit_code = (int)code;
    return 0;
}

int pl_poll(pl_process *proc, int *exit_code) {
    if (!proc || proc->handle == 0) return -1;
    HANDLE h = (HANDLE)proc->handle;

    DWORD res = WaitForSingleObject(h, 0);
    if (res == WAIT_TIMEOUT) {
        return 0;
    }
    if (res != WAIT_OBJECT_0) {
        return -1;
    }

    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) {
        CloseHandle(h);
        proc->handle = 0;
        return -1;
    }

    CloseHandle(h);
    proc->handle = 0;
    if (exit_code) *exit_code = (int)code;
    return 1;
}

#else /* POSIX (Linux / macOS) */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int pl_start(const char *const argv[], pl_process *proc) {
    if (!argv || !argv[0] || !proc) return -1;

    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        execvp(argv[0], (char *const *)argv);
        int err = errno;
        ssize_t written = write(pipefd[1], &err, sizeof(err));
        (void)written;
        close(pipefd[1]);
        _exit(127);
    }

    close(pipefd[1]);
    int child_errno = 0;
    ssize_t n;
    while ((n = read(pipefd[0], &child_errno, sizeof(child_errno))) < 0 && errno == EINTR) {}
    close(pipefd[0]);

    if (n > 0) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        proc->handle = 0;
        proc->pid = -1;
        errno = child_errno;
        return -1;
    }

    proc->handle = 0;
    proc->pid = (long)pid;
    return 0;
}

static int parse_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int pl_wait(pl_process *proc, int *exit_code) {
    if (!proc || proc->pid <= 0) return -1;

    int status = 0;
    pid_t res;
    do {
        res = waitpid((pid_t)proc->pid, &status, 0);
    } while (res < 0 && errno == EINTR);

    if (res < 0) return -1;

    proc->pid = -1;
    if (exit_code) *exit_code = parse_status(status);
    return 0;
}

int pl_poll(pl_process *proc, int *exit_code) {
    if (!proc || proc->pid <= 0) return -1;

    int status = 0;
    pid_t res;
    do {
        res = waitpid((pid_t)proc->pid, &status, WNOHANG);
    } while (res < 0 && errno == EINTR);

    if (res < 0) return -1;
    if (res == 0) return 0;

    proc->pid = -1;
    if (exit_code) *exit_code = parse_status(status);
    return 1;
}

#endif
