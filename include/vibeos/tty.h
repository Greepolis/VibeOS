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
 * There is one terminal, so one set of modes, shared by every description that
 * names the console and outliving the program that set them - as a real
 * terminal's do, which is why a program that dies in raw mode leaves the next
 * one in it.
 *
 * The modes are kept in Linux's numbering, as the open flags are (vibeos/
 * file.h): the bits a Linux program passes are the bits stored, and another
 * personality translates its own console modes into these. Every constant here
 * is compared with Linux's by tests/kernel/linux_layout_tests.c. */

#include <stdint.h>

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

/* What is honoured: ICRNL; OPOST with ONLCR; ICANON, ECHO, ECHOE, ECHONL; the
 * erase, kill and end-of-file characters; MIN. Everything else is stored and
 * given back as it was set, which is what lets a program save the modes and
 * restore them - and nothing more: the interrupt character still interrupts
 * with ISIG clear, and TIME is not a timer. */
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

#endif
