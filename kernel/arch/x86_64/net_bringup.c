/* The network bring-up: the interface the portable stack drives, the pump that
 * feeds it received frames and runs its timers, and DHCP at boot with a stated
 * fallback.
 *
 * The protocol stack is kernel/net/inet.c and knows nothing of hardware; the
 * device is whichever network driver registered (C7). This is the glue between
 * the two, as io_bringup.c is for storage.
 *
 * Lifted out of arch_hw.c whole (2026-09-29). Nothing changed in the move. */

#include <stdint.h>
#include "vibeos/arch_x86_64.h"
#include "vibeos/trap.h"
#include "vibeos/boot.h"
#include "vibeos/mm.h"
#include "vibeos/inet.h"
#include "vibeos/elf.h"
#include "vibeos/services.h"
#include "vibeos/exec_stats.h"
#include "arch_hw_internal.h"
#include "vibeos/account.h"
#include "vibeos/forkguard.h"
#include "vibeos/sched_policy.h"
#include "vibeos/pageinfo.h"
#include "vibeos/rmap.h"
#include "vibeos/reclaim.h"
#include "vibeos/mbz.h"
#include "vibeos/abi.h"
#include "vibeos/abi_linux.h"
#include "vibeos/ceildiv.h"
#include "vibeos/blkdev.h"
#include "vibeos/io_stats.h"
#include "vibeos/blockdev.h"
#include "vibeos/partition.h"
#include "vibeos/parttab.h"
#include "vibeos/ext2.h"
#include "vibeos/iso9660.h"
#include "vibeos/exfat.h"
#include "vibeos/ntfs.h"
#include "vibeos/logsink.h"
#include "vibeos/storage.h"
#include "vibeos/swapmap.h"
#include "vibeos/anon.h"
#include "vibeos/swaparea.h"
#include "vibeos/log.h"
#include "vibeos/klog.h"
#include "vibeos/crash.h"
#include "vibeos/device.h"
#include "vibeos/mm_model.h"
#include "vibeos/frame.h"
#include "vibeos/vmspace.h"
#include "vibeos/vma.h"
#include "vibeos/backing.h"
#include "vibeos/task_stats.h"
#include "vibeos/task.h"
#include "vibeos/runq.h"
#include "vibeos/lifetime.h"
#include "vibeos/vfs.h"

/* ---- networking ----------------------------------------------------------
 *
 * The protocol stack (kernel/net/inet.c) is portable and hardware-free: this
 * layer gives it a transmit path, feeds it received frames, and drives its
 * timers from the tick. Everything is serialized on one lock - the stack is
 * entered both from syscalls and from the timer interrupt. */

static uint64_t hw_net_now_ms(void) {
    return g_timer_ticks * (1000ull / VIBEOS_HW_TIMER_HZ);
}

/* The interface the stack drives: the first network device the registry found
 * present. arch_hw.c used to name virtio-net's functions directly, so a second
 * network driver meant editing this file (C7). Set once, before g_net_up. */
static const vibeos_net_ops_t *g_netdev;

static int hw_net_tx(void *ctx, const void *frame, uint32_t len) {
    (void)ctx;
    return g_netdev->send(frame, len);
}

/* Drain the receive queue into the stack and advance its timers. Called from
 * the timer IRQ, so the whole system keeps making network progress even while
 * a task is blocked in a socket call. */
/* Staging buffer for one received frame. Static rather than a local: this runs
 * on the interrupt stack, and 1.5 KiB of frame there is enough to overflow it.
 * Covered by the network lock, like everything else that touches the stack. */
static uint8_t g_net_rxframe[VIBEOS_INET_MTU];

void hw_net_pump(void) {
    int n;
    int budget = 16;

    if (!g_net_up) {
        return;
    }
    hw_spin_lock_named(&g_net_lock, __func__);
    while (budget-- > 0) {
        n = g_netdev->recv(g_net_rxframe, (uint32_t)sizeof(g_net_rxframe));
        if (n <= 0) {
            break;
        }
        (void)vibeos_inet_input(&g_net, g_net_rxframe, (uint32_t)n);
    }
    vibeos_inet_poll(&g_net, hw_net_now_ms());
    hw_spin_unlock(&g_net_lock);
}

static void hw_net_print_ip(uint32_t ip) {
    int i;
    for (i = 3; i >= 0; i--) {
        uint32_t b = (ip >> (i * 8)) & 0xFFu;
        char buf[4];
        int k = 0;
        if (b >= 100u) { buf[k++] = (char)('0' + b / 100u); }
        if (b >= 10u)  { buf[k++] = (char)('0' + (b / 10u) % 10u); }
        buf[k++] = (char)('0' + b % 10u);
        buf[k] = 0;
        vibeos_x86_64_serial_puts(buf);
        if (i > 0) {
            vibeos_x86_64_serial_puts(".");
        }
    }
}

/* Bring the interface up and take a DHCP lease. Runs before the scheduler is
 * armed, so it pumps the device itself while it waits. */
void hw_net_bringup(void) {
    uint32_t spins;

    /* The driver was brought up by the device registry's probe; this asks
     * for the result rather than naming the driver. */
    g_netdev = vibeos_net_device();
    if (!g_netdev) {
        vibeos_x86_64_serial_puts("[NET] no network interface; networking disabled\n");
        return;
    }
    if (vibeos_inet_init(&g_net, g_netdev->mac(), hw_net_tx, 0) != 0) {
        vibeos_x86_64_serial_puts("[NET] stack init failed\n");
        return;
    }
    /* The secret the stack derives its unguessable identifiers from (H-008).
     *
     * RDRAND when the processor has it. Otherwise the timestamp mix that
     * AT_RANDOM already uses - which that function's own comment calls what it
     * is: it differs between boots and is not an entropy source. The line
     * below says which one this boot got, because a secret whose quality is
     * not stated is one somebody will later assume is good. QEMU's default
     * TCG CPU does not advertise RDRAND, so under the gate this is the weak one. */
    {
        uint32_t eax, ebx, ecx, edx;
        uint64_t k[2] = {0, 0};
        int from_rdrand = 0;

        __asm__ __volatile__("cpuid"
                             : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(1u), "c"(0u));
        if (ecx & (1u << 30)) {
            int i, tries;
            from_rdrand = 1;
            for (i = 0; i < 2; i++) {
                uint64_t v = 0, ok = 0;
                for (tries = 0; tries < 10 && !ok; tries++) {
                    __asm__ __volatile__("xorl %%edx, %%edx; rdrand %%rax; setc %%dl"
                                         : "=a"(v), "=d"(ok));
                }
                if (!ok) {
                    from_rdrand = 0;   /* a failing RDRAND is not a source */
                }
                k[i] = v;
            }
        }
        if (!from_rdrand) {
            uint8_t seed[16];
            int i;
            hw_seed_at_random(seed);
            k[0] = 0;
            k[1] = 0;
            for (i = 0; i < 8; i++) {
                k[0] |= (uint64_t)seed[i] << (8 * i);
                k[1] |= (uint64_t)seed[8 + i] << (8 * i);
            }
        }
        vibeos_inet_set_secret(&g_net, k[0], k[1]);
        vibeos_x86_64_serial_puts(from_rdrand
            ? "[NET] stack secret from rdrand\n"
            : "[NET] stack secret from tsc mix (weak: no entropy source on this cpu)\n");
    }
    g_net_up = 1;

    vibeos_x86_64_serial_puts("[NET] requesting a DHCP lease\n");
    /* Under the lock: this transmits, and the timer's pump is already live. */
    hw_spin_lock_named(&g_net_lock, __func__);
    (void)vibeos_inet_dhcp_start(&g_net);
    hw_spin_unlock(&g_net_lock);

    /* The timer is already live but the scheduler is not, so pump inline.
     * Bounded: a network that does not answer must not hold up the boot. */
    for (spins = 0; spins < 400u; spins++) {
        /* Go through the same locked path the timer uses: the interrupt is
         * already live and would otherwise reuse the staging buffer under us. */
        hw_net_pump();
        {
            int bound_now;
            hw_spin_lock_named(&g_net_lock, __func__);
            bound_now = vibeos_inet_dhcp_bound(&g_net);
            hw_spin_unlock(&g_net_lock);
            if (bound_now) {
                break;
            }
        }
        {
            uint32_t d;
            for (d = 0; d < 200000u; d++) {
                __asm__ __volatile__("pause" ::: "memory");
            }
        }
    }

    /* Snapshot under the lock: the timer is pumping the stack concurrently, and
     * a DHCP retry blanks the address for the duration of the send. Reading the
     * live fields here could catch that window and report 0.0.0.0. */
    {
        int bound;
        uint32_t ip, gw, dns;

        hw_spin_lock_named(&g_net_lock, __func__);
        bound = vibeos_inet_dhcp_bound(&g_net);
        ip = g_net.ip;
        gw = g_net.gateway;
        dns = g_net.dns;
        hw_spin_unlock(&g_net_lock);

        vibeos_x86_64_serial_lock();
        if (bound) {
            vibeos_x86_64_serial_puts("[NET] NET_OK dhcp lease ip=");
            hw_net_print_ip(ip);
            vibeos_x86_64_serial_puts(" gw=");
            hw_net_print_ip(gw);
            vibeos_x86_64_serial_puts(" dns=");
            hw_net_print_ip(dns);
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
            return;
        }
        vibeos_x86_64_serial_unlock();
    }
    /* No DHCP server answered: fall back to QEMU's user-mode defaults so the
     * stack is still usable, and say so plainly. */
    hw_spin_lock_named(&g_net_lock, __func__);
    vibeos_inet_set_addr(&g_net, 0x0A000210u, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    hw_spin_unlock(&g_net_lock);

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[NET] no DHCP answer; using a static address ip=");
    hw_net_print_ip(0x0A000210u);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}
