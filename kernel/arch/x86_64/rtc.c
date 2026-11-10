/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The wall clock: the CMOS real-time clock, read once at boot (E4-029).
 *
 * Until this the machine had no idea what time it was. CLOCK_REALTIME was the
 * time since boot, NtQuerySystemTime was 1601 plus that, and every file the
 * kernel wrote on ext4 was dated 1970. The chip is read once; from then on the
 * wall clock is that reading plus the ticks since it, which is what a kernel
 * without an NTP client can honestly offer. Nothing sets the chip.
 *
 * The chip is assumed to keep UTC, as QEMU's does by default and as every
 * system but Windows expects of it. A machine whose firmware keeps local time
 * will be wrong by its offset; the line this prints says which time was read,
 * so that is visible rather than silent. */

#include <stdint.h>

#include "arch_hw_internal.h"

/* Seconds since 1970 at the tick g_wall_base_ticks was taken, in ticks: the
 * wall clock in ticks is ks_ticks() + this. 0: no reading (the clocks then
 * count from boot, as they did before). */
uint64_t g_wall_base_ticks;

static inline void rtc_outb(uint16_t p, uint8_t v) {
    __asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(p));
}
static inline uint8_t rtc_inb(uint16_t p) {
    uint8_t v;
    __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

/* Register `r` of the CMOS, with NMI left enabled (bit 7 clear). */
static uint8_t cmos(uint8_t r) {
    rtc_outb(0x70, r);
    return rtc_inb(0x71);
}

typedef struct {
    uint8_t sec, min, hour, day, mon, year, century;
} rtc_raw_t;

/* One reading, taken when no update is in progress (status A, bit 7): an
 * update rolls the registers over one at a time, and a reading across it can
 * say 23:59:59 of the next day. */
static int rtc_read_once(rtc_raw_t *r) {
    uint32_t spins = 0;

    while ((cmos(0x0A) & 0x80u) != 0u) {
        if (++spins > 1000000u) {
            return -1;   /* a chip that is always updating is no clock */
        }
    }
    r->sec = cmos(0x00);
    r->min = cmos(0x02);
    r->hour = cmos(0x04);
    r->day = cmos(0x07);
    r->mon = cmos(0x08);
    r->year = cmos(0x09);
    r->century = cmos(0x32);   /* ACPI's FADT names the century register; 0x32 is where it always is */
    return 0;
}

static int rtc_same(const rtc_raw_t *a, const rtc_raw_t *b) {
    return a->sec == b->sec && a->min == b->min && a->hour == b->hour && a->day == b->day &&
           a->mon == b->mon && a->year == b->year && a->century == b->century;
}

static uint32_t bcd(uint8_t v) {
    return (uint32_t)(v & 0x0Fu) + (uint32_t)(v >> 4) * 10u;
}

/* Days from 1970-01-01 to y-m-d of the proleptic Gregorian calendar (Howard
 * Hinnant's days_from_civil). */
static int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d) {
    int64_t era;
    uint32_t yoe, doy, doe;

    y -= m <= 2u;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (uint32_t)(y - era * 400);
    doy = (153u * (m + (m > 2u ? (uint32_t)-3 : 9u)) + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void put_dec(char *out, uint32_t *at, uint32_t v, uint32_t width) {
    char d[10];
    uint32_t n = 0;

    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u && n < sizeof(d));
    while (n < width) {
        d[n++] = '0';
    }
    while (n > 0u) {
        out[(*at)++] = d[--n];
    }
}

void hw_rtc_bringup(void) {
    rtc_raw_t a, b;
    uint8_t status_b;
    uint32_t sec, min, hour, day, mon, year, tries = 0;
    int pm;
    int64_t secs;
    char line[64];
    uint32_t at = 0;

    /* Twice the same, so a reading was not torn by an update that began after
     * the in-progress check. */
    do {
        if (rtc_read_once(&a) != 0 || rtc_read_once(&b) != 0) {
            vibeos_x86_64_serial_puts("[RTC] no clock: the chip never left its update; wall time is uptime\n");
            return;
        }
    } while (!rtc_same(&a, &b) && ++tries < 8u);
    status_b = cmos(0x0B);
    pm = (a.hour & 0x80u) != 0u;
    a.hour &= 0x7Fu;
    if ((status_b & 0x04u) == 0u) {   /* BCD, which is what nearly every chip keeps */
        sec = bcd(a.sec);
        min = bcd(a.min);
        hour = bcd(a.hour);
        day = bcd(a.day);
        mon = bcd(a.mon);
        year = bcd(a.year) + (a.century != 0u ? bcd(a.century) * 100u : 2000u);
    } else {
        sec = a.sec;
        min = a.min;
        hour = a.hour;
        day = a.day;
        mon = a.mon;
        year = a.year + (a.century != 0u ? a.century * 100u : 2000u);
    }
    if ((status_b & 0x02u) == 0u) {   /* a 12-hour clock: 12 is midnight or noon */
        hour %= 12u;
        if (pm) {
            hour += 12u;
        }
    }
    if (mon < 1u || mon > 12u || day < 1u || day > 31u || hour > 23u || min > 59u || sec > 60u ||
        year < 2000u) {
        vibeos_x86_64_serial_puts("[RTC] the clock reads no date; wall time is uptime\n");
        return;
    }
    secs = days_from_civil((int64_t)year, mon, day) * 86400 + (int64_t)(hour * 3600u + min * 60u + sec);
    g_wall_base_ticks = (uint64_t)secs * VIBEOS_HW_TIMER_HZ - g_timer_ticks;

    put_dec(line, &at, year, 4);
    line[at++] = '-';
    put_dec(line, &at, mon, 2);
    line[at++] = '-';
    put_dec(line, &at, day, 2);
    line[at++] = 'T';
    put_dec(line, &at, hour, 2);
    line[at++] = ':';
    put_dec(line, &at, min, 2);
    line[at++] = ':';
    put_dec(line, &at, sec, 2);
    line[at++] = 'Z';
    line[at] = 0;
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[RTC] wall clock ");
    vibeos_x86_64_serial_puts(line);
    vibeos_x86_64_serial_puts(" epoch=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)secs);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}
