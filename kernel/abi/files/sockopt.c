/* The options every socket keeps the same way (docs/abi/ L5 step 1).
 *
 * Most of what a program sets on a socket changes nothing this kernel does - a
 * buffer size the stack does not have, a keepalive it never sends - and what it
 * must do is answer as Linux does when the program reads the option back, which
 * programs do: a server checks SO_REUSEADDR took, a library reads SO_TYPE to
 * learn what a descriptor it was handed is, a non-blocking connect is finished
 * by reading SO_ERROR. So they live in the description, set and read here, and
 * a socket type (socket.c, unixsock.c) answers only the few that are its own:
 * whether it listens, what is waiting, who its peer is.
 *
 * Values are in kernel memory; the personality copied them. */

#include "files_internal.h"

/* Linux's limits: a buffer size is clamped to the system maximum and then
 * doubled, for the kernel's own bookkeeping, and never below a floor. */
#define SK_BUF_MAX       212992u
#define SK_MIN_RCVBUF    2304u
#define SK_MIN_SNDBUF    4608u

void vibeos_sockopt_init(vibeos_file_t *f, int type, uint32_t family) {
    f->sk_type = type;
    f->sk_family = family;
    f->sk_flags = 0;
    f->sk_err = 0;
    f->sk_rcvbuf = SK_BUF_MAX;
    f->sk_sndbuf = SK_BUF_MAX;
    f->sk_linger = 0;
    f->sk_rcvtimeo = 0;
    f->sk_sndtimeo = 0;
}

static uint32_t flag_of(int opt) {
    switch (opt) {
        case VIBEOS_SO_REUSEADDR: return VIBEOS_SKF_REUSEADDR;
        case VIBEOS_SO_KEEPALIVE: return VIBEOS_SKF_KEEPALIVE;
        case VIBEOS_SO_BROADCAST: return VIBEOS_SKF_BROADCAST;
        case VIBEOS_SO_OOBINLINE: return VIBEOS_SKF_OOBINLINE;
        case VIBEOS_SO_DONTROUTE: return VIBEOS_SKF_DONTROUTE;
        case VIBEOS_TCP_NODELAY:  return VIBEOS_SKF_NODELAY;
        case VIBEOS_SO_PASSCRED:  return VIBEOS_SKF_PASSCRED;
        case VIBEOS_SO_REUSEPORT: return VIBEOS_SKF_REUSEPORT;
        case VIBEOS_SO_DEBUG:     return VIBEOS_SKF_DEBUG;
        default:                  return 0;
    }
}

long vibeos_sockopt_set(vibeos_file_t *f, int opt, const void *val, uint32_t len) {
    uint32_t bit = flag_of(opt);
    int32_t v;

    if (opt == VIBEOS_SO_RCVTIMEO || opt == VIBEOS_SO_SNDTIMEO) {
        uint64_t ticks;

        if (len < sizeof(ticks)) {
            return -VIBEOS_EINVAL;
        }
        ticks = *(const uint64_t *)val;
        if (opt == VIBEOS_SO_RCVTIMEO) {
            f->sk_rcvtimeo = ticks;
        } else {
            f->sk_sndtimeo = ticks;
        }
        return 0;
    }
    if (opt == VIBEOS_SO_LINGER) {
        const int32_t *l = (const int32_t *)val;

        if (len < 2u * sizeof(int32_t)) {
            return -VIBEOS_EINVAL;
        }
        f->sk_flags = l[0] ? (f->sk_flags | VIBEOS_SKF_LINGER) : (f->sk_flags & ~VIBEOS_SKF_LINGER);
        f->sk_linger = l[1];
        return 0;
    }
    if (len < sizeof(v)) {
        return -VIBEOS_EINVAL;
    }
    v = *(const int32_t *)val;
    if (bit != 0u) {
        f->sk_flags = v ? (f->sk_flags | bit) : (f->sk_flags & ~bit);
        return 0;
    }
    switch (opt) {
        case VIBEOS_SO_RCVBUF:
        case VIBEOS_SO_SNDBUF: {
            uint32_t u = v < 0 ? 0u : (uint32_t)v;
            uint32_t floor = opt == VIBEOS_SO_RCVBUF ? SK_MIN_RCVBUF : SK_MIN_SNDBUF;

            u = u > SK_BUF_MAX ? SK_BUF_MAX : u;
            u = u * 2u < floor ? floor : u * 2u;
            if (opt == VIBEOS_SO_RCVBUF) {
                f->sk_rcvbuf = u;
            } else {
                f->sk_sndbuf = u;
            }
            return 0;
        }
        case VIBEOS_SO_TYPE:
        case VIBEOS_SO_ERROR:
        case VIBEOS_SO_ACCEPTCONN:
        case VIBEOS_SO_DOMAIN:
        case VIBEOS_SO_PEERCRED:
        case VIBEOS_SO_NREAD:
            return -VIBEOS_ENOPROTOOPT;   /* read only, as Linux has them */
        default:
            return -VIBEOS_ENOPROTOOPT;
    }
}

long vibeos_sockopt_get(vibeos_file_t *f, int opt, void *val, uint32_t *len) {
    uint32_t bit = flag_of(opt);
    int32_t v;

    if (opt == VIBEOS_SO_RCVTIMEO || opt == VIBEOS_SO_SNDTIMEO) {
        if (*len < sizeof(uint64_t)) {
            return -VIBEOS_EINVAL;
        }
        *(uint64_t *)val = opt == VIBEOS_SO_RCVTIMEO ? f->sk_rcvtimeo : f->sk_sndtimeo;
        *len = sizeof(uint64_t);
        return 0;
    }
    if (opt == VIBEOS_SO_LINGER) {
        int32_t *l = (int32_t *)val;

        if (*len < 2u * sizeof(int32_t)) {
            return -VIBEOS_EINVAL;
        }
        l[0] = (f->sk_flags & VIBEOS_SKF_LINGER) ? 1 : 0;
        l[1] = f->sk_linger;
        *len = 2u * sizeof(int32_t);
        return 0;
    }
    if (*len < sizeof(v)) {
        return -VIBEOS_EINVAL;
    }
    if (bit != 0u) {
        v = (f->sk_flags & bit) ? 1 : 0;
    } else {
        switch (opt) {
            case VIBEOS_SO_TYPE:       v = f->sk_type; break;
            case VIBEOS_SO_DOMAIN:     v = (int32_t)f->sk_family; break;
            case VIBEOS_SO_RCVBUF:     v = (int32_t)f->sk_rcvbuf; break;
            case VIBEOS_SO_SNDBUF:     v = (int32_t)f->sk_sndbuf; break;
            case VIBEOS_SO_ERROR:      v = f->sk_err; f->sk_err = 0; break;   /* reading clears it */
            case VIBEOS_SO_ACCEPTCONN: v = 0; break;                       /* a type that listens says */
            default:                   return -VIBEOS_ENOPROTOOPT;
        }
    }
    *(int32_t *)val = v;
    *len = sizeof(v);
    return 0;
}
