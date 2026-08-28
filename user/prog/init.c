/* Minimal native VibeOS init.
 *
 * This is deliberately separate from hello.c: hello remains the compatibility
 * and bring-up workload, while this program owns the first native userland
 * lifecycle boundary. It starts the shell as a child and waits for its exit.
 */

#define SYS_write 1
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60
#define SYS_wait4 61

#include <stdint.h>

static int64_t sys3(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
    return ret;
}

static const char init_ready[] = "NATIVE_INIT_READY\n";
static const char shell_path[] = "EFI/BOOT/SH.ELF";
static const char shell_name[] = "sh";
static char *const init_argv[] = {(char *)shell_name, 0};
static char *const init_envp[] = {(char *)"PATH=/EFI/BOOT", (char *)"TERM=dumb", 0};

int vibeos_main(int argc, char **argv, char **envp) {
    int64_t child;
    int64_t status = 0;
    (void)argc;
    (void)argv;
    (void)envp;
    sys3(SYS_write, 1, (uint64_t)(uintptr_t)init_ready, sizeof(init_ready) - 1);
    child = sys3(SYS_fork, 0, 0, 0);
    if (child == 0) {
        sys3(SYS_execve, (uint64_t)(uintptr_t)shell_path,
             (uint64_t)(uintptr_t)init_argv, (uint64_t)(uintptr_t)init_envp);
        sys3(SYS_exit, 127, 0, 0);
    }
    if (child > 0) {
        if (sys3(SYS_wait4, (uint64_t)child, (uint64_t)(uintptr_t)&status, 0) == child) {
            return (status >> 8) & 0xff;
        }
    }
    return 127;
}
