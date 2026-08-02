/* NET.ELF - a user-space TCP client, to prove the stack end to end.
 *
 * Opens a TCP connection to the host QEMU presents at 10.0.2.2, sends a line,
 * reads the echo back and prints it. Everything it uses is a syscall: socket,
 * connect, write, read, close - the same interface a Linux program would use.
 */

#define SYS_read    0
#define SYS_write   1
#define SYS_close   3
#define SYS_exit    60
#define SYS_socket  41
#define SYS_connect 42

#define AF_INET 2
#define SOCK_STREAM 1

/* QEMU's user-mode network puts the host at 10.0.2.2. */
#define PEER_IP 0x0A000202u
#define PEER_PORT 7777u

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

int vibeos_main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    /* struct sockaddr_in: family little endian, port and address big endian. */
    unsigned char addr[16];
    char buf[128];
    long fd, n;
    int i;

    for (i = 0; i < 16; i++) {
        addr[i] = 0;
    }
    addr[0] = AF_INET;
    addr[1] = 0;
    addr[2] = (unsigned char)(PEER_PORT >> 8);
    addr[3] = (unsigned char)(PEER_PORT & 0xFF);
    addr[4] = (unsigned char)(PEER_IP >> 24);
    addr[5] = (unsigned char)((PEER_IP >> 16) & 0xFF);
    addr[6] = (unsigned char)((PEER_IP >> 8) & 0xFF);
    addr[7] = (unsigned char)(PEER_IP & 0xFF);

    fd = sys3(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        put("net: socket failed\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    if (sys3(SYS_connect, fd, (long)(unsigned long)addr, 16) < 0) {
        put("net: connect failed\n");
        sys3(SYS_exit, 2, 0, 0);
    }
    put("net: connected to 10.0.2.2:7777\n");

    {
        const char *msg = "VIBEOS-NET-TEST\n";
        if (sys3(SYS_write, fd, (long)(unsigned long)msg, (long)slen(msg)) < 0) {
            put("net: send failed\n");
            sys3(SYS_exit, 3, 0, 0);
        }
    }

    n = sys3(SYS_read, fd, (long)(unsigned long)buf, (long)sizeof(buf) - 1);
    if (n <= 0) {
        put("net: no reply\n");
        sys3(SYS_exit, 4, 0, 0);
    }
    buf[n] = 0;
    put("net: echo: ");
    put(buf);
    put("net: TCP_OK\n");

    sys3(SYS_close, fd, 0, 0);
    sys3(SYS_exit, 0, 0, 0);
    return 0;   /* not reached; exit does not return */
}
