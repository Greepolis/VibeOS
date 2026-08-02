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
#define SYS_netctl 1000

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

/* Print an unsigned decimal number. */
static void put_u(unsigned long v) {
    char b[24];
    int i = 0;
    if (v == 0) {
        put("0");
        return;
    }
    while (v > 0 && i < 23) {
        b[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        char one[2];
        one[0] = b[--i];
        one[1] = 0;
        put(one);
    }
}

static void put_ip(unsigned long ip) {
    int shift;
    for (shift = 24; shift >= 0; shift -= 8) {
        put_u((ip >> shift) & 0xFF);
        if (shift > 0) {
            put(".");
        }
    }
}

static unsigned long parse_ip(const char *s) {
    unsigned long v = 0, part = 0;
    int parts = 0, digits = 0;
    for (;;) {
        char c = *s++;
        if (c >= '0' && c <= '9') {
            part = part * 10 + (unsigned long)(c - '0');
            digits++;
        } else if (c == '.' || c == 0) {
            if (!digits || part > 255) {
                return 0;
            }
            v = (v << 8) | part;
            part = 0;
            digits = 0;
            parts++;
            if (c == 0) {
                break;
            }
        } else {
            return 0;
        }
    }
    return (parts == 4) ? v : 0;
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

/* Shell entry point.
 *
 * Reads a line at a time from the console and dispatches it: builtins are
 * handled here, anything else is treated as a program path and run as a child
 * process. Every operation goes through syscalls - there is no libc - so this
 * also serves as a working example of the system call surface.
 */
int vibeos_main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    char line[128];

    put("VibeOS shell. Commands: help, echo <text>, exit, <program path>\n");

    for (;;) {
        long n;
        char *args;
        int i;

        /* Prompt, then block until the console hands us a line. The kernel
         * echoes keystrokes and handles editing, so this just reads. */
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
        /* First word selects the command, the rest is its argument text. */
        args = split_word(line);

        if (seq(line, "help")) {
            put("help              show this text\n"
                "echo <text>       print text\n"
                "ls [dir]          list a directory\n"
                "cat <file>        print a file\n"
                "write <f> <text>  create a file with text\n"
                "mkdir <dir>       create a directory\n"
                "rm <file>         delete a file\n"
                "net               show the network interface\n"
                "ping <a.b.c.d>    ICMP echo a host\n"
                "resolve <name>    look a name up in DNS\n"
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
                long dn = sys3(SYS_getdents64, fd, (long)(unsigned long)dbuf, sizeof(dbuf));
                long o = 0;
                while (o + 19 < dn) {
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
                long cn;
                while ((cn = sys3(SYS_read, fd, (long)(unsigned long)fbuf, sizeof(fbuf))) > 0) {
                    sys3(SYS_write, 1, (long)(unsigned long)fbuf, cn);
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
        } else if (seq(line, "net")) {
            unsigned int cfg[5];
            unsigned long stats[4];
            if (sys3(SYS_netctl, 0, (long)(unsigned long)cfg, 0) != 0) {
                put("net: interface down\n");
            } else {
                put("ip      ");  put_ip(cfg[0]); put("\n");
                put("netmask ");  put_ip(cfg[1]); put("\n");
                put("gateway ");  put_ip(cfg[2]); put("\n");
                put("dns     ");  put_ip(cfg[3]); put("\n");
                put("dhcp    ");  put(cfg[4] ? "bound\n" : "static\n");
                if (sys3(SYS_netctl, 3, (long)(unsigned long)stats, 0) == 0) {
                    put("frames  tx="); put_u(stats[0]);
                    put(" rx=");        put_u(stats[1]);
                    put(" dropped=");   put_u(stats[2]);
                    put(" retrans=");   put_u(stats[3]);
                    put("\n");
                }
            }
        } else if (seq(line, "ping")) {
            unsigned long ip = parse_ip(args);
            long rtt;
            if (ip == 0) {
                put("ping: usage: ping <a.b.c.d>\n");
            } else if ((rtt = sys3(SYS_netctl, 1, (long)ip, 0)) < 0) {
                put("ping: no reply\n");
            } else {
                put("reply from "); put_ip(ip);
                put(" time="); put_u((unsigned long)rtt); put("ms\n");
            }
        } else if (seq(line, "resolve")) {
            long ip = sys3(SYS_netctl, 2, (long)(unsigned long)args, 0);
            if (ip <= 0) {
                put("resolve: not found\n");
            } else {
                put(args); put(" is "); put_ip((unsigned long)ip); put("\n");
            }
        } else if (seq(line, "exit")) {
            put("sh: bye\n");
            sys3(SYS_exit, 0, 0, 0);
        } else {
            run_program(line);
        }
    }
}
