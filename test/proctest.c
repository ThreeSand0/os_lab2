#define _POSIX_C_SOURCE 200809L
#include "proclaunch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    /* Режим дочернего процесса: --child <exit_code> <sleep_ms> [optional_arg] */
    if (argc >= 4 && strcmp(argv[1], "--child") == 0) {
        if (argc >= 5) {
            const char *expected = "hello \"big\" world\\";
            if (strcmp(argv[4], expected) != 0) {
                fprintf(stderr, "Child mismatch: got [%s], expected [%s]\n", argv[4], expected);
                return 99;
            }
        }
        sleep_ms(atoi(argv[3]));
        return atoi(argv[2]);
    }

    const char *self = argv[0];
    int failed = 0;

    /* Тест 1: Фоновый запуск на 1 с, код 42, pl_poll в цикле по 100 мс */
    {
        const char *args[] = {self, "--child", "42", "1000", NULL};
        pl_process proc;
        int code = -1;
        int iterations = 0;

        if (pl_start(args, &proc) != 0) {
            printf("[FAIL] Тест 1: pl_start вернула ошибку\n");
            failed++;
        } else {
            int st;
            while ((st = pl_poll(&proc, &code)) == 0) {
                iterations++;
                sleep_ms(100);
            }
            if (st == 1 && iterations > 3 && code == 42) {
                printf("[ OK ] Тест 1: фоновый запуск (итераций=%d, код=%d)\n", iterations, code);
            } else {
                printf("[FAIL] Тест 1: st=%d, итераций=%d, код=%d\n", st, iterations, code);
                failed++;
            }
        }
    }

    /* Тест 2: Ребёнок на 0.3 с, код 7, pl_wait */
    {
        const char *args[] = {self, "--child", "7", "300", NULL};
        pl_process proc;
        int code = -1;

        if (pl_start(args, &proc) == 0 && pl_wait(&proc, &code) == 0 && code == 7) {
            printf("[ OK ] Тест 2: pl_wait вернул код %d\n", code);
        } else {
            printf("[FAIL] Тест 2: ожидался код 7, получено %d\n", code);
            failed++;
        }
    }

    /* Тест 3: Аргумент со сложными символами: hello "big" world\ */
    {
        const char *special = "hello \"big\" world\\";
        const char *args[] = {self, "--child", "0", "50", special, NULL};
        pl_process proc;
        int code = -1;

        if (pl_start(args, &proc) == 0 && pl_wait(&proc, &code) == 0 && code == 0) {
            printf("[ OK ] Тест 3: передача аргумента с пробелами, кавычками и слешем\n");
        } else {
            printf("[FAIL] Тест 3: аргумент искажён (код возврата ребёнка %d)\n", code);
            failed++;
        }
    }

    /* Тест 4: Запуск несуществующей программы */
    {
        const char *args[] = {"no_such_program_12345", NULL};
        pl_process proc;

        if (pl_start(args, &proc) == -1) {
            printf("[ OK ] Тест 4: запуск несуществующей программы вернул -1\n");
        } else {
            printf("[FAIL] Тест 4: pl_start не вернула -1\n");
            int dummy;
            pl_wait(&proc, &dummy);
            failed++;
        }
    }

    /* Тест 5: Код возврата 300 (Linux: 44, Windows: 300) */
    {
        const char *args[] = {self, "--child", "300", "50", NULL};
        pl_process proc;
        int code = -1;
#ifdef _WIN32
        int expected_code = 300;
#else
        int expected_code = 44;
#endif
        if (pl_start(args, &proc) == 0 && pl_wait(&proc, &code) == 0 && code == expected_code) {
            printf("[ OK ] Тест 5: код 300 обработан корректно (получено %d)\n", code);
        } else {
            printf("[FAIL] Тест 5: ожидалось %d, получено %d\n", expected_code, code);
            failed++;
        }
    }

    return failed == 0 ? 0 : 1;
}
