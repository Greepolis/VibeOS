/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_TERMINAL_H
#define VIBEOS_TERMINAL_H

/* The console as a terminal (docs/abi/ L1 step 7).
 *
 * Until this the console was a character device that answered ENOTTY to every
 * terminal question but the foreground process group, and that answer was the
 * design: a C library asks "is this a terminal?" to decide how to buffer, and
 * "no" was true. It read a line at a time, echoed always, and gave back
 * whatever had been typed so far when the keyboard went quiet.
 *
 * It is a terminal now, with the modes a program changes one: whether typing is
 * echoed, and whether input arrives a finished line at a time - erasable until
 * Enter - or a byte at a time as it is typed, which is what an editor and a
 * shell's own line editing ask for.
 *
 * A terminal has one set of modes, shared by every description that names it
 * and outliving the program that set them - as a real
 * terminal's do, which is why a program that dies in raw mode leaves the next
 * one in it.
 *
 * The modes are kept in Linux's numbering, as the open flags are (vibeos/
 * file.h): the bits a Linux program passes are the bits stored, and another
 * personality translates its own console modes into these. Every constant here
 * is compared with Linux's by tests/kernel/linux_layout_tests.c. */

#include <stdint.h>

#include "vibeos/procstate.h"   /* vibeos_lock_t */

#define VIBEOS_TTY_NCC 19u

/* input modes */
#define VIBEOS_TTY_ICRNL  0x0100u   /* a carriage return typed is a newline read */
#define VIBEOS_TTY_IXON   0x0400u
/* output modes */
#define VIBEOS_TTY_OPOST  0x0001u   /* output is processed at all                 */
#define VIBEOS_TTY_ONLCR  0x0004u   /* a newline written goes out as CR LF        */
/* control modes: 38400 baud, eight bits, receiver on - what Linux reports of a
 * console nobody configured */
#define VIBEOS_TTY_CFLAG_DEFAULT 0x00bfu
/* local modes */
#define VIBEOS_TTY_ISIG    0x0001u
#define VIBEOS_TTY_ICANON  0x0002u  /* a line at a time, erasable until Enter     */
#define VIBEOS_TTY_ECHO    0x0008u  /* what is typed is shown                     */
#define VIBEOS_TTY_ECHOE   0x0010u  /* an erase rubs the character out            */
#define VIBEOS_TTY_ECHOK   0x0020u
#define VIBEOS_TTY_ECHONL  0x0040u  /* a newline is shown even without ECHO       */
#define VIBEOS_TTY_NOFLSH  0x0080u  /* a signal character keeps what was typed   */
#define VIBEOS_TTY_TOSTOP  0x0100u  /* a background write is SIGTTOU              */
#define VIBEOS_TTY_ECHOCTL 0x0200u
#define VIBEOS_TTY_ECHOKE  0x0800u
#define VIBEOS_TTY_IEXTEN  0x8000u
/* the control characters, by their place in cc[] */
#define VIBEOS_TTY_VINTR  0u
#define VIBEOS_TTY_VQUIT  1u
#define VIBEOS_TTY_VERASE 2u
#define VIBEOS_TTY_VKILL  3u
#define VIBEOS_TTY_VEOF   4u
#define VIBEOS_TTY_VTIME  5u
#define VIBEOS_TTY_VMIN   6u
#define VIBEOS_TTY_VSUSP 10u

typedef struct {
    uint32_t iflag;
    uint32_t oflag;
    uint32_t cflag;
    uint32_t lflag;
    uint8_t line;
    uint8_t cc[VIBEOS_TTY_NCC];
} vibeos_tty_modes_t;

typedef struct {
    uint16_t rows;
    uint16_t cols;
    uint16_t xpixel;
    uint16_t ypixel;
} vibeos_tty_size_t;

/* What is honoured: ICRNL; OPOST with ONLCR; ICANON, ECHO, ECHOE, ECHONL; ISIG
 * with the interrupt, quit and suspend characters, NOFLSH, TOSTOP; the erase,
 * kill and end-of-file characters; MIN. Everything else is stored and given
 * back as it was set, which is what lets a program save the modes and restore
 * them - and nothing more: TIME is not a timer. */
void vibeos_tty_get(vibeos_tty_modes_t *out);
/* `flush` discards what was typed and not yet read - from the terminal's own
 * line; what is still in the keyboard's queue behind it is kept. */
void vibeos_tty_set(const vibeos_tty_modes_t *in, int flush);
void vibeos_tty_get_size(vibeos_tty_size_t *out);
void vibeos_tty_set_size(const vibeos_tty_size_t *in);
/* Bytes a read would get now without waiting. */
uint32_t vibeos_tty_pending(void);
/* Boot and tests: the modes a terminal starts with, nothing typed. */
void vibeos_tty_reset(void);

/* A terminal as an instance (docs/shell/s1.md step 3). The functions above
 * and below named vibeos_tty_ are the console's; vibeos_term_ take the
 * instance. The backend says where typed bytes come from and where an echo
 * goes. get_fg and set_fg are optional: without them the instance keeps its
 * own foreground group. A zeroed instance with `ops` set has its default modes
 * from the first use. */
#define VIBEOS_TERM_LINE 256u

typedef struct {
    int (*getc)(void *ctx);            /* the next byte typed, or -1 */
    void (*echo)(void *ctx, char c);
    void (*rub)(void *ctx);            /* take the last character back off the screen */
    uint32_t (*get_fg)(void *ctx);
    void (*set_fg)(void *ctx, uint32_t pgid);
} vibeos_term_ops_t;

typedef struct {
    const vibeos_term_ops_t *ops;
    void *ctx;
    vibeos_tty_modes_t m;
    vibeos_tty_size_t size;
    uint8_t line[VIBEOS_TERM_LINE];
    uint32_t len;
    uint32_t ready;
    int eof;                      /* end-of-file typed on an empty line */
    uint32_t session;             /* whose controlling terminal it is; 0: nobody's */
    uint32_t fg;                  /* the foreground group, without get_fg */
    uint32_t sig_due;             /* signals typed, raised once the lock is let go */
    vibeos_lock_t lock;
    int set_up;
} vibeos_term_t;

void vibeos_term_reset(vibeos_term_t *t);
void vibeos_term_get(vibeos_term_t *t, vibeos_tty_modes_t *out);
void vibeos_term_set(vibeos_term_t *t, const vibeos_tty_modes_t *in, int flush);
void vibeos_term_get_size(vibeos_term_t *t, vibeos_tty_size_t *out);
void vibeos_term_set_size(vibeos_term_t *t, const vibeos_tty_size_t *in);
uint32_t vibeos_term_pending(vibeos_term_t *t);
int vibeos_term_control(vibeos_term_t *t, uint8_t c);
void vibeos_term_set_session(vibeos_term_t *t, uint32_t sid);
long vibeos_term_job_check(vibeos_term_t *t, uint32_t sig);
long vibeos_term_read(vibeos_term_t *t, uint64_t buf, uint64_t len, int nonblock);
uint32_t vibeos_term_ready(vibeos_term_t *t);
long vibeos_term_ioctl(vibeos_term_t *t, uint64_t req, uint64_t arg);

/* A control character as it is typed (docs/shell/s1.md step 2), from the
 * keyboard's interrupt: a signal has to reach a program that is not reading.
 * 1 when it was a signal to the foreground group; 0 when it is an ordinary
 * byte for the input queue - with ISIG clear, Ctrl+C is a program's to read. */
int vibeos_tty_control(uint8_t c);
/* The session whose controlling terminal this is; 0 for none. */
void vibeos_tty_set_session(uint32_t sid);
/* May the caller act on the terminal, or is it a background job? For SIGTTOU
 * (changing the terminal; writing under TOSTOP) and SIGTTIN (reading): 0 to go
 * on, -EIO when the job can neither be stopped nor let through, or the restart
 * code with the signal sent to its group. */
long vibeos_tty_job_check(uint32_t sig);

#endif
