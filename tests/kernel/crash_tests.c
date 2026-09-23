/* Host tests for crash records (C6, kernel/diag/crash.c). */

#include <stdio.h>
#include <string.h>

#include "vibeos/crash.h"

int test_crash(void);

static int g_locks, g_unlocks, g_held, g_bad;

static void t_lock(void) {
    g_locks++;
    if (g_held != 0) {
        g_bad = 1;
    }
    g_held++;
}

static void t_unlock(void) {
    g_unlocks++;
    if (g_held <= 0) {
        g_bad = 1;
        return;
    }
    g_held--;
}

static char g_out[4096];
static uint32_t g_out_len;
static int g_out_calls;

static int t_out(void *ctx, const char *line, uint32_t len) {
    (void)ctx;
    if (g_out_len + len < sizeof(g_out)) {
        memcpy(g_out + g_out_len, line, len);
        g_out_len += len;
        g_out[g_out_len] = 0;
    }
    g_out_calls++;
    return 0;
}

static void clear_out(void) {
    g_out_len = 0;
    g_out[0] = 0;
    g_out_calls = 0;
}

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:crash %s\n", what);
    }
    return cond;
}

static void make(vibeos_crash_t *r, uint32_t pid, const char *exe, uint32_t words) {
    static const char *const names[12] = {
        "rip", "rsp", "rbp", "rflags", "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9"
    };
    uint32_t i;

    memset(r, 0, sizeof(*r));
    r->pid = pid;
    r->sig = 11;
    r->vector = 14;
    r->error_code = 4;
    r->fault_addr = 0x10;
    for (i = 0; i < 12u; i++) {
        r->regs[i].name = names[i];
        r->regs[i].value = 0x1000u + i;
    }
    r->nregs = 12;
    for (i = 0; i < words; i++) {
        r->stack[i] = 0xA0u + i;
    }
    r->stack_words = words;
    strncpy(r->exe, exe, sizeof(r->exe) - 1u);
}

int test_crash(void) {
    vibeos_crash_t r, got;
    uint32_t i;

    g_locks = g_unlocks = g_held = g_bad = 0;
    vibeos_crash_set_lock(t_lock, t_unlock);
    vibeos_crash_reset();

    /* ---- nothing crashed: said plainly ------------------------------------------ */
    clear_out();
    vibeos_crash_dump(t_out, 0);
    if (!expect(strcmp(g_out, "[CRASH] no process has faulted since boot\n") == 0,
                "an empty ring says so rather than printing an empty record")) { return -1; }
    if (!expect(vibeos_crash_get(0, &got) == -1, "and has no latest record")) { return -1; }

    /* ---- one record, dumped in full ------------------------------------------------ */
    make(&r, 9, "/EFI/BOOT/SVC_CRSH.ELF", 2);
    if (!expect(vibeos_crash_record(&r) == 1u, "the first record is number one")) { return -1; }
    clear_out();
    vibeos_crash_dump(t_out, 0);
    if (!expect(strcmp(g_out,
        "[CRASH] total=0x0000000000000001 pid=0x0000000000000009 sig=0x000000000000000b exe=/EFI/BOOT/SVC_CRSH.ELF\n"
        "[CRASH] vector=0x000000000000000e err=0x0000000000000004 fault_addr=0x0000000000000010\n"
        "[CRASH] rip=0x0000000000001000 rsp=0x0000000000001001 rbp=0x0000000000001002 rflags=0x0000000000001003\n"
        "[CRASH] rax=0x0000000000001004 rbx=0x0000000000001005 rcx=0x0000000000001006 rdx=0x0000000000001007\n"
        "[CRASH] rsi=0x0000000000001008 rdi=0x0000000000001009 r8=0x000000000000100a r9=0x000000000000100b\n"
        "[CRASH] stack+0x0000000000000000 = 0x00000000000000a0\n"
        "[CRASH] stack+0x0000000000000008 = 0x00000000000000a1\n"
        "[CRASH] stack truncated: the next word is not readable\n"
        "[CRASH] end lines=0x0000000000000008\n") == 0,
        "the dump names the program, the fault, every register and the readable stack")) {
        printf("  got:\n%s", g_out);
        return -1;
    }
    if (!expect(g_out_calls == 9, "one line per call, so the device gets whole lines")) { return -1; }

    /* ---- a full stack is not called truncated, and no exe is said aloud ------------ */
    make(&r, 10, "", VIBEOS_CRASH_STACK_WORDS);
    (void)vibeos_crash_record(&r);
    clear_out();
    vibeos_crash_dump(t_out, 0);
    if (!expect(strstr(g_out, "exe=(unknown)\n") && !strstr(g_out, "truncated"),
                "an unknown program and a whole stack are reported as what they are")) { return -1; }

    /* ---- the ring keeps four, newest first --------------------------------------- */
    vibeos_crash_reset();
    for (i = 1; i <= 6u; i++) {
        make(&r, i, "x", 0);
        if (!expect(vibeos_crash_record(&r) == (uint64_t)i, "the count goes up by one")) { return -1; }
    }
    if (!expect(vibeos_crash_count() == 6u, "six recorded")) { return -1; }
    if (!expect(vibeos_crash_get(0, &got) == 0 && got.pid == 6u, "back 0 is the latest")) { return -1; }
    if (!expect(vibeos_crash_get(3, &got) == 0 && got.pid == 3u, "back 3 is the oldest kept")) { return -1; }
    if (!expect(vibeos_crash_get(4, &got) == -1, "a fifth one back has been overwritten")) { return -1; }

    /* ---- a record that lies about its sizes cannot run the dump off its end ------- */
    make(&r, 7, "", 0);
    r.nregs = 999;
    r.stack_words = 999;
    memset(r.exe, 'E', sizeof(r.exe));   /* no terminator */
    (void)vibeos_crash_record(&r);
    if (!expect(vibeos_crash_get(0, &got) == 0 && got.nregs == VIBEOS_CRASH_REGS &&
                got.stack_words == VIBEOS_CRASH_STACK_WORDS &&
                got.exe[VIBEOS_CRASH_EXE - 1u] == 0,
                "counts are clamped and the name terminated when the record is kept")) { return -1; }

    /* ---- the ring serialises itself ---------------------------------------------- */
    {
        int before = g_locks;
        make(&r, 8, "y", 0);
        (void)vibeos_crash_record(&r);
        if (!expect(g_locks == before + 1,
                    "a record is claimed, filled and published in one critical section: "
                    "done as three, two cores faulting at once took one slot")) { return -1; }
    }
    if (!expect(!g_bad && g_held == 0 && g_locks == g_unlocks,
                "every lock was released once, and never taken twice")) { return -1; }

    vibeos_crash_set_lock(0, 0);
    vibeos_crash_reset();
    return 0;
}
