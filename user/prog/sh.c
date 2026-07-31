/* VibeOS shell: a real user-space program that reads commands from the console
 * and runs them, using only syscalls (read/write/fork/execve/wait4/exit).
 *
 * Builtins: help, echo <text>, exit [code]
 * Anything else is treated as a program path on the filesystem and run as a
 * child process (fork + execve + wait), e.g. "EFI/BOOT/TASK.ELF".
 */

#define SYS_read   0
#define SYS_write  1
#define SYS_fork   57
#define SYS_execve 59
#define SYS_exit   60
#define SYS_wait4  61
#define SYS_open   2
#define SYS_close  3
#define SYS_getdents64 217
#define SYS_unlink 87
#define SYS_mkdir  83

static long sys3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
    return ret;
}

static unsigned long slen(const char *s) {
    unsigned long n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void put(const char *s) {
    sys3(SYS_write, 1, (long)(unsigned long)s, (long)slen(s));
}

static int seq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Split off the first word of `line`; returns a pointer to the remainder. */
static char *split_word(char *line) {
    char *p = line;
    while (*p && *p != ' ') {
        p++;
    }
    if (*p == ' ') {
        *p++ = 0;
        while (*p == ' ') {
            p++;
        }
    }
    return p;
}

static void run_program(const char *path) {
    long child = sys3(SYS_fork, 0, 0, 0);
    if (child == 0) {
        sys3(SYS_execve, (long)(unsigned long)path, 0, 0);
        put("sh: cannot exec\n");
        sys3(SYS_exit, 127, 0, 0);
    } else if (child > 0) {
        sys3(SYS_wait4, child, 0, 0);
    } else {
        put("sh: fork failed\n");
    }
}

void _start(void) {
    char line[128];

    put("VibeOS shell. Commands: help, echo <text>, exit, <program path>\n");

    for (;;) {
        long n;
        char *args;
        int i;

        put("$ ");
        n = sys3(SYS_read, 0, (long)(unsigned long)line, (long)sizeof(line) - 1);
        if (n <= 0) {
            put("\nsh: end of input\n");
            sys3(SYS_exit, 0, 0, 0);
        }
        /* Trim the trailing newline; the console echoes keystrokes itself. */
        for (i = 0; i < (int)n; i++) {
            if (line[i] == '\n' || line[i] == '\r') {
                break;
            }
        }
        line[i] = 0;

        if (line[0] == 0) {
            continue;
        }
        args = split_word(line);

        if (seq(line, "help")) {
            put("help              show this text\n"
                "echo <text>       print text\n"
                "ls [dir]          list a directory\n"
                "cat <file>        print a file\n"
                "write <f> <text>  create a file with text\n"
                "mkdir <dir>       create a directory\n"
                "rm <file>         delete a file\n"
                "exit              leave the shell\n"
                "<path>            run a program, e.g. EFI/BOOT/TASK.ELF\n");
        } else if (seq(line, "echo")) {
            put(args);
            put("\n");
        } else if (seq(line, "ls")) {
            long fd = sys3(SYS_open, (long)(unsigned long)(args[0] ? args : "/"), 0, 0);
            if (fd < 0) {
                put("ls: cannot open\n");
            } else {
                char dbuf[512];
                long n = sys3(SYS_getdents64, fd, (long)(unsigned long)dbuf, sizeof(dbuf));
                long o = 0;
                while (o + 19 < n) {
                    unsigned short reclen = (unsigned char)dbuf[o + 16] |
                                            ((unsigned char)dbuf[o + 17] << 8);
                    if (reclen == 0) {
                        break;
                    }
                    put(&dbuf[o + 19]);
                    put(dbuf[o + 18] == 4 ? "/\n" : "\n");
                    o += reclen;
                }
                sys3(SYS_close, fd, 0, 0);
            }
        } else if (seq(line, "cat")) {
            long fd = sys3(SYS_open, (long)(unsigned long)args, 0, 0);
            if (fd < 0) {
                put("cat: no such file\n");
            } else {
                char fbuf[256];
                long n;
                while ((n = sys3(SYS_read, fd, (long)(unsigned long)fbuf, sizeof(fbuf))) > 0) {
                    sys3(SYS_write, 1, (long)(unsigned long)fbuf, n);
                }
                sys3(SYS_close, fd, 0, 0);
            }
        } else if (seq(line, "write")) {
            char *text = split_word(args);
            long fd = sys3(SYS_open, (long)(unsigned long)args, 0100 /*O_CREAT*/, 0);
            if (fd < 0) {
                put("write: cannot create\n");
            } else {
                sys3(SYS_write, fd, (long)(unsigned long)text, (long)slen(text));
                sys3(SYS_write, fd, (long)(unsigned long)"\n", 1);
                if (sys3(SYS_close, fd, 0, 0) == 0) {
                    put("written\n");
                } else {
                    put("write: commit failed\n");
                }
            }
        } else if (seq(line, "rm")) {
            put(sys3(SYS_unlink, (long)(unsigned long)args, 0, 0) == 0 ? "removed\n"
                                                                      : "rm: failed\n");
        } else if (seq(line, "mkdir")) {
            put(sys3(SYS_mkdir, (long)(unsigned long)args, 0, 0) == 0 ? "created\n"
                                                                     : "mkdir: failed\n");
        } else if (seq(line, "exit")) {
            put("sh: bye\n");
            sys3(SYS_exit, 0, 0, 0);
        } else {
            run_program(line);
        }
    }
}
