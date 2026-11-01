/* The Windows registry's core (docs/abi/windows.md W8 step 2). See
 * vibeos/ntreg.h for the contract.
 *
 * Keys are slots of a static table, so a caller can hold one by index and
 * tenancy across calls. Everything of variable size - names, values, data - is
 * a blob in pages the environment hands out: small ones in cells of a size
 * class, carved from pages that are kept for reuse once carved; large ones as a
 * chain of whole pages, given back when freed. Every byte taken counts against
 * the quota, so a program that sets values in a loop is refused with
 * STATUS_QUOTA_EXCEEDED rather than taking the machine's memory.
 *
 * One lock serialises the whole tree. Nothing under it waits or touches a
 * program's memory: the services above copy in and out around the calls.
 *
 * Walks of the tree are iterative, over the parent, child and sibling links,
 * never recursive: a key may be 512 deep, and the kernel's stacks are small. */

#include "vibeos/ntreg.h"
#include "vibeos/nt_layout.h"

#define PAGE 4096u
#define REG_FLAG_USED     0x1u
#define REG_FLAG_VOLATILE 0x2u
#define REG_FLAG_DELETED  0x4u
#define REG_FLAG_PERM     0x8u    /* \Registry, Machine, User: never deleted */
#define NONE 0xFFFFFFFFu

/* ---- blobs --------------------------------------------------------------------------- */

/* Cells of 32 bytes to 2 KiB; past that, a chain of pages each holding the
 * next page's address in its first eight bytes. */
#define CLASSES 7u
#define SMALL_MAX 2048u
#define CHAIN_PAYLOAD (PAGE - 8u)

static const uint32_t g_class_size[CLASSES] = { 32u, 64u, 128u, 256u, 512u, 1024u, 2048u };
static void *g_class_free[CLASSES];
static vibeos_reg_env_t g_env;
static uint64_t g_bytes;     /* taken from the environment, cells and chains */
static uint64_t g_changes;   /* every change a save would write */

static uint32_t class_of(uint32_t len) {
    uint32_t c = 0;

    while (c + 1u < CLASSES && g_class_size[c] < len) {
        c++;
    }
    return c;
}

static int quota_ok(uint64_t more) {
    return g_env.quota == 0u || g_bytes + more <= g_env.quota;
}

/* A blob of `len` bytes, or null. Zero bytes is a valid blob with no storage. */
static void *blob_alloc(uint32_t len) {
    if (len == 0u) {
        return 0;
    }
    if (len <= SMALL_MAX) {
        uint32_t c = class_of(len);
        void *cell = g_class_free[c];

        if (!cell) {
            uint8_t *page;
            uint32_t at;

            if (!quota_ok(PAGE) || !(page = (uint8_t *)g_env.page_alloc())) {
                return 0;
            }
            g_bytes += PAGE;
            for (at = 0; at + g_class_size[c] <= PAGE; at += g_class_size[c]) {
                *(void **)(void *)(page + at) = g_class_free[c];
                g_class_free[c] = page + at;
            }
            cell = g_class_free[c];
        }
        g_class_free[c] = *(void **)cell;
        return cell;
    } else {
        uint32_t pages = (len + CHAIN_PAYLOAD - 1u) / CHAIN_PAYLOAD, i;
        uint8_t *head = 0, *prev = 0;

        if (!quota_ok((uint64_t)pages * PAGE)) {
            return 0;
        }
        for (i = 0; i < pages; i++) {
            uint8_t *p = (uint8_t *)g_env.page_alloc();

            if (!p) {
                while (head) {
                    uint8_t *next = *(uint8_t **)(void *)head;

                    g_env.page_free(head);
                    g_bytes -= PAGE;
                    head = next;
                }
                return 0;
            }
            g_bytes += PAGE;
            *(uint8_t **)(void *)p = 0;
            if (prev) {
                *(uint8_t **)(void *)prev = p;
            } else {
                head = p;
            }
            prev = p;
        }
        return head;
    }
}

static void blob_free(void *b, uint32_t len) {
    if (!b || len == 0u) {
        return;
    }
    if (len <= SMALL_MAX) {
        uint32_t c = class_of(len);

        *(void **)b = g_class_free[c];
        g_class_free[c] = b;
    } else {
        uint8_t *p = (uint8_t *)b;

        while (p) {
            uint8_t *next = *(uint8_t **)(void *)p;

            g_env.page_free(p);
            g_bytes -= PAGE;
            p = next;
        }
    }
}

/* Copy between a blob of `len` bytes and flat memory, from offset `off`: into
 * the blob from `in`, or out of it to `out` - one of the two is null. Two
 * pointers rather than one and a direction, so that a const source is never
 * cast to a writable one. */
static void blob_xfer(void *b, uint32_t len, uint32_t off, const uint8_t *in, uint8_t *out,
                      uint32_t n) {
    uint32_t i;

    if (len <= SMALL_MAX) {
        uint8_t *s = (uint8_t *)b + off;

        for (i = 0; i < n; i++) {
            if (in) {
                s[i] = in[i];
            } else {
                out[i] = s[i];
            }
        }
        return;
    }
    {
        uint8_t *p = (uint8_t *)b;

        while (off >= CHAIN_PAYLOAD) {
            p = *(uint8_t **)(void *)p;
            off -= CHAIN_PAYLOAD;
        }
        for (i = 0; i < n; i++) {
            if (off == CHAIN_PAYLOAD) {
                p = *(uint8_t **)(void *)p;
                off = 0;
            }
            if (in) {
                p[8u + off] = in[i];
            } else {
                out[i] = p[8u + off];
            }
            off++;
        }
    }
}

static void blob_put(void *b, uint32_t len, uint32_t off, const void *src, uint32_t n) {
    blob_xfer(b, len, off, (const uint8_t *)src, 0, n);
}

static void blob_get(void *b, uint32_t len, uint32_t off, void *dst, uint32_t n) {
    blob_xfer(b, len, off, 0, (uint8_t *)dst, n);
}

static uint16_t blob_unit(void *b, uint32_t len, uint32_t i) {
    uint16_t u;

    blob_get(b, len, i * 2u, &u, 2u);
    return u;
}

/* ---- names --------------------------------------------------------------------------- */

uint16_t vibeos_reg_upcase(uint16_t c) {
    if (c >= 'a' && c <= 'z') {
        return (uint16_t)(c - 32u);
    }
    if (c < 0x80u) {
        return c;
    }
    if (c >= 0xE0u && c <= 0xFEu && c != 0xF7u) {
        return (uint16_t)(c - 32u);
    }
    if (c == 0xFFu) {
        return 0x178u;
    }
    /* Latin Extended-A: pairs, upper first, except where the pairing shifts by
     * one around the two ranges that start on an odd code point. */
    if ((c >= 0x100u && c <= 0x137u) || (c >= 0x14Au && c <= 0x177u)) {
        return (uint16_t)(c & ~1u);
    }
    if ((c >= 0x139u && c <= 0x148u) || (c >= 0x179u && c <= 0x17Eu)) {
        return (c & 1u) ? c : (uint16_t)(c - 1u);
    }
    if (c >= 0x3B1u && c <= 0x3C9u && c != 0x3C2u) {
        return (uint16_t)(c - 32u);   /* Greek */
    }
    if (c == 0x3C2u) {
        return 0x3A3u;                /* final sigma */
    }
    if (c >= 0x430u && c <= 0x44Fu) {
        return (uint16_t)(c - 32u);   /* Cyrillic */
    }
    if (c >= 0x450u && c <= 0x45Fu) {
        return (uint16_t)(c - 80u);
    }
    return c;
}

/* -1, 0 or 1: a stored name against a given one, without case, as the
 * registry orders keys. Lengths in units. */
static int name_cmp(void *b, uint32_t blen, const uint16_t *n, uint32_t nlen) {
    uint32_t units = blen / 2u, i;

    for (i = 0; i < units && i < nlen; i++) {
        uint16_t x = vibeos_reg_upcase(blob_unit(b, blen, i)), y = vibeos_reg_upcase(n[i]);

        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return units == nlen ? 0 : (units < nlen ? -1 : 1);
}

/* ---- keys and values ----------------------------------------------------------------- */

typedef struct reg_value {
    struct reg_value *next;
    void *name;
    void *data;
    uint32_t name_len;   /* bytes */
    uint32_t data_len;
    uint32_t type;
} reg_value_t;

typedef struct {
    uint32_t gen;
    uint32_t refs;
    uint32_t flags;
    uint32_t parent, child, sibling;
    uint32_t subkeys, values;
    void *name;
    uint32_t name_len;   /* bytes */
    reg_value_t *vhead, *vtail;
    uint64_t last_write;
} reg_key_t;

static reg_key_t g_keys[VIBEOS_REG_KEYS_MAX];
static uint32_t g_free_key;   /* the head of the free slots, through `child` */

static void lock(void) { g_env.lock(); }
static void unlock(void) { g_env.unlock(); }

static void touch(uint32_t k) {
    g_keys[k].last_write = g_env.now ? g_env.now() : 0u;
    if (!(g_keys[k].flags & REG_FLAG_VOLATILE)) {
        g_changes++;
    }
}

/* The slot a caller's key names, or NONE: wrong tenancy, or not a key. */
static uint32_t slot_of(vibeos_reg_key_t k) {
    if (k.idx >= VIBEOS_REG_KEYS_MAX || !(g_keys[k.idx].flags & REG_FLAG_USED) ||
        g_keys[k.idx].gen != k.gen) {
        return NONE;
    }
    return k.idx;
}

/* A slot the caller may act on: STATUS_INVALID_HANDLE, STATUS_KEY_DELETED,
 * or success with *s set. */
static uint32_t live(vibeos_reg_key_t k, uint32_t *s) {
    *s = slot_of(k);
    if (*s == NONE) {
        return VIBEOS_STATUS_INVALID_HANDLE;
    }
    return (g_keys[*s].flags & REG_FLAG_DELETED) ? VIBEOS_STATUS_KEY_DELETED : VIBEOS_STATUS_SUCCESS;
}

static uint32_t key_new(const uint16_t *name, uint32_t units, uint32_t flags) {
    uint32_t s = g_free_key;
    void *nb;

    if (s == NONE) {
        return NONE;
    }
    nb = blob_alloc(units * 2u);
    if (units != 0u && !nb) {
        return NONE;
    }
    g_free_key = g_keys[s].child;
    g_keys[s].refs = 0;
    g_keys[s].flags = REG_FLAG_USED | flags;
    g_keys[s].parent = g_keys[s].child = g_keys[s].sibling = NONE;
    g_keys[s].subkeys = g_keys[s].values = 0;
    g_keys[s].name = nb;
    g_keys[s].name_len = units * 2u;
    g_keys[s].vhead = g_keys[s].vtail = 0;
    if (units != 0u) {
        blob_put(nb, units * 2u, 0, name, units * 2u);
    }
    g_keys[s].last_write = g_env.now ? g_env.now() : 0u;
    return s;
}

static void value_free(reg_value_t *v) {
    blob_free(v->name, v->name_len);
    blob_free(v->data, v->data_len);
    blob_free(v, (uint32_t)sizeof(*v));
}

/* The slot goes back for good: its tenancy moves on, so a key held by an old
 * reference is refused, not handed the next tenant. */
static void key_release(uint32_t s) {
    g_keys[s].gen++;
    g_keys[s].flags = 0;
    g_keys[s].child = g_free_key;
    g_free_key = s;
}

/* Unlink `s` from its parent, drop its name and values, and mark it deleted:
 * referenced keys stay until their last reference, the rest go now. */
static void key_kill(uint32_t s) {
    uint32_t p = g_keys[s].parent;
    reg_value_t *v = g_keys[s].vhead;

    if (p != NONE) {
        uint32_t *link = &g_keys[p].child;

        while (*link != s) {
            link = &g_keys[*link].sibling;
        }
        *link = g_keys[s].sibling;
        g_keys[p].subkeys--;
        if (!(g_keys[s].flags & REG_FLAG_VOLATILE)) {
            g_changes++;
        }
    }
    while (v) {
        reg_value_t *n = v->next;

        value_free(v);
        v = n;
    }
    g_keys[s].vhead = g_keys[s].vtail = 0;
    g_keys[s].values = 0;
    g_keys[s].parent = g_keys[s].sibling = NONE;
    g_keys[s].flags |= REG_FLAG_DELETED;
    if (g_keys[s].refs == 0u) {
        blob_free(g_keys[s].name, g_keys[s].name_len);
        g_keys[s].name = 0;
        key_release(s);
    }
}

/* Insert `c` among `p`'s children, kept in name order. */
static void link_child(uint32_t p, uint32_t c) {
    uint32_t *link = &g_keys[p].child;

    while (*link != NONE) {
        uint32_t o = *link;
        uint16_t buf[VIBEOS_REG_KEY_NAME_MAX];
        uint32_t units = g_keys[c].name_len / 2u;

        blob_get(g_keys[c].name, g_keys[c].name_len, 0, buf, g_keys[c].name_len);
        if (name_cmp(g_keys[o].name, g_keys[o].name_len, buf, units) > 0) {
            break;
        }
        link = &g_keys[o].sibling;
    }
    g_keys[c].sibling = *link;
    *link = c;
    g_keys[c].parent = p;
    g_keys[p].subkeys++;
}

static uint32_t find_child(uint32_t p, const uint16_t *name, uint32_t units) {
    uint32_t c;

    for (c = g_keys[p].child; c != NONE; c = g_keys[c].sibling) {
        if (name_cmp(g_keys[c].name, g_keys[c].name_len, name, units) == 0) {
            return c;
        }
    }
    return NONE;
}

static reg_value_t *find_value(uint32_t k, const uint16_t *name, uint32_t units) {
    reg_value_t *v;

    for (v = g_keys[k].vhead; v; v = v->next) {
        if (name_cmp(v->name, v->name_len, name, units) == 0) {
            return v;
        }
    }
    return 0;
}

static uint32_t depth_of(uint32_t s) {
    uint32_t d = 0;

    while (g_keys[s].parent != NONE) {
        s = g_keys[s].parent;
        d++;
    }
    return d;
}

static int valid_component(uint32_t n) {
    return n >= 1u && n <= VIBEOS_REG_KEY_NAME_MAX;
}

/* A key and everything under it gone, deepest first and without recursion. */
static void kill_subtree(uint32_t top) {
    for (;;) {
        uint32_t x = top;

        while (g_keys[x].child != NONE) {
            x = g_keys[x].child;
        }
        key_kill(x);
        if (x == top) {
            return;
        }
    }
}

/* ---- the root -------------------------------------------------------------------------- */

static uint32_t g_root = NONE;

void vibeos_reg_init(const vibeos_reg_env_t *env) {
    static const uint16_t machine[] = { 'M', 'A', 'C', 'H', 'I', 'N', 'E' };
    static const uint16_t user[] = { 'U', 'S', 'E', 'R' };
    static const uint16_t registry[] = { 'R', 'E', 'G', 'I', 'S', 'T', 'R', 'Y' };
    uint32_t i, m, u;

    g_env = *env;
    g_bytes = 0;
    g_changes = 0;
    for (i = 0; i < CLASSES; i++) {
        g_class_free[i] = 0;
    }
    for (i = 0; i < VIBEOS_REG_KEYS_MAX; i++) {
        g_keys[i].flags = 0;
        g_keys[i].gen = 1u;
        g_keys[i].child = i + 1u < VIBEOS_REG_KEYS_MAX ? i + 1u : NONE;
    }
    g_free_key = 0;
    g_root = key_new(registry, 8u, REG_FLAG_PERM);
    m = key_new(machine, 7u, REG_FLAG_PERM);
    u = key_new(user, 4u, REG_FLAG_PERM);
    link_child(g_root, m);
    link_child(g_root, u);
    g_changes = 0;
}

vibeos_reg_key_t vibeos_reg_root(void) {
    vibeos_reg_key_t k;

    lock();
    k.idx = g_root;
    k.gen = g_keys[g_root].gen;
    g_keys[g_root].refs++;
    unlock();
    return k;
}

void vibeos_reg_ref(vibeos_reg_key_t k) {
    uint32_t s;

    lock();
    s = slot_of(k);
    if (s != NONE) {
        g_keys[s].refs++;
    }
    unlock();
}

void vibeos_reg_unref(vibeos_reg_key_t k) {
    uint32_t s;

    lock();
    s = slot_of(k);
    if (s != NONE && g_keys[s].refs != 0u && --g_keys[s].refs == 0u &&
        (g_keys[s].flags & REG_FLAG_DELETED)) {
        blob_free(g_keys[s].name, g_keys[s].name_len);
        g_keys[s].name = 0;
        key_release(s);
    }
    unlock();
}

/* ---- opening and creating -------------------------------------------------------------- */

/* Walk `path` from `s`. With `create`, the last component may be made, with
 * `options`. *out is the slot reached, referenced. */
static uint32_t walk(uint32_t s, const uint16_t *path, uint32_t n, int create, uint32_t options,
                     uint32_t *out, uint32_t *disposition) {
    uint32_t at = 0;

    if (n == 0u) {
        if (create && disposition) {
            *disposition = VIBEOS_REG_OPENED;
        }
        g_keys[s].refs++;
        *out = s;
        return VIBEOS_STATUS_SUCCESS;
    }
    while (at < n) {
        uint32_t end = at, c;

        while (end < n && path[end] != '\\') {
            end++;
        }
        if (!valid_component(end - at) || (end < n && end + 1u == n)) {
            return VIBEOS_STATUS_OBJECT_NAME_INVALID;
        }
        c = find_child(s, path + at, end - at);
        if (c == NONE) {
            uint32_t flags;

            if (!create) {
                return VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND;
            }
            if (end < n) {
                return VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND;   /* only the last may be new */
            }
            if ((g_keys[s].flags & REG_FLAG_VOLATILE) && !(options & VIBEOS_REG_VOLATILE)) {
                return VIBEOS_STATUS_CHILD_MUST_BE_VOLATILE;
            }
            if (s == g_root) {
                return VIBEOS_STATUS_ACCESS_DENIED;   /* \Registry has its two keys and no more */
            }
            if (depth_of(s) + 1u > VIBEOS_REG_DEPTH_MAX) {
                return VIBEOS_STATUS_INVALID_PARAMETER;
            }
            flags = (options & VIBEOS_REG_VOLATILE) ? REG_FLAG_VOLATILE : 0u;
            c = key_new(path + at, end - at, flags);
            if (c == NONE) {
                return quota_ok(PAGE) ? VIBEOS_STATUS_INSUFFICIENT_RESOURCES
                                      : VIBEOS_STATUS_QUOTA_EXCEEDED;
            }
            link_child(s, c);
            touch(s);
            if (!(flags & REG_FLAG_VOLATILE)) {
                g_changes++;
            }
            if (disposition) {
                *disposition = VIBEOS_REG_CREATED;
            }
        } else if (create && end == n && disposition) {
            *disposition = VIBEOS_REG_OPENED;
        }
        s = c;
        at = end + 1u;
    }
    g_keys[s].refs++;
    *out = s;
    return VIBEOS_STATUS_SUCCESS;
}

static uint32_t open_or_create(vibeos_reg_key_t from, const uint16_t *path, uint32_t n, int create,
                               uint32_t options, vibeos_reg_key_t *out, uint32_t *disposition) {
    uint32_t s, st, got = NONE;

    lock();
    st = live(from, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = walk(s, path, n, create, options, &got, disposition);
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        out->idx = got;
        out->gen = g_keys[got].gen;
    }
    unlock();
    return st;
}

uint32_t vibeos_reg_open(vibeos_reg_key_t from, const uint16_t *path, uint32_t n,
                         vibeos_reg_key_t *out) {
    return open_or_create(from, path, n, 0, 0u, out, 0);
}

uint32_t vibeos_reg_create(vibeos_reg_key_t from, const uint16_t *path, uint32_t n,
                           uint32_t options, vibeos_reg_key_t *out, uint32_t *disposition) {
    return open_or_create(from, path, n, 1, options, out, disposition);
}

uint32_t vibeos_reg_delete(vibeos_reg_key_t k) {
    uint32_t s, st;

    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        if (g_keys[s].flags & REG_FLAG_PERM) {
            st = VIBEOS_STATUS_ACCESS_DENIED;
        } else if (g_keys[s].subkeys != 0u) {
            st = VIBEOS_STATUS_CANNOT_DELETE;
        } else {
            touch(g_keys[s].parent);
            key_kill(s);
        }
    }
    unlock();
    return st;
}

uint32_t vibeos_reg_rename(vibeos_reg_key_t k, const uint16_t *name, uint32_t n) {
    uint32_t s, st, p, *link;
    void *nb;

    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS && (g_keys[s].flags & REG_FLAG_PERM)) {
        st = VIBEOS_STATUS_ACCESS_DENIED;
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        uint32_t i;

        for (i = 0; i < n; i++) {
            if (name[i] == '\\') {
                break;
            }
        }
        if (!valid_component(n) || i < n) {
            st = VIBEOS_STATUS_OBJECT_NAME_INVALID;
        }
    }
    p = st == VIBEOS_STATUS_SUCCESS ? g_keys[s].parent : NONE;
    if (st == VIBEOS_STATUS_SUCCESS) {
        uint32_t o = find_child(p, name, n);

        if (o != NONE && o != s) {
            st = VIBEOS_STATUS_OBJECT_NAME_COLLISION;
        }
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        nb = blob_alloc(n * 2u);
        if (!nb) {
            st = VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
        } else {
            blob_put(nb, n * 2u, 0, name, n * 2u);
            link = &g_keys[p].child;
            while (*link != s) {
                link = &g_keys[*link].sibling;
            }
            *link = g_keys[s].sibling;
            g_keys[p].subkeys--;
            blob_free(g_keys[s].name, g_keys[s].name_len);
            g_keys[s].name = nb;
            g_keys[s].name_len = n * 2u;
            link_child(p, s);
            touch(s);
            touch(p);
        }
    }
    unlock();
    return st;
}

/* ---- describing a key ------------------------------------------------------------------- */

uint32_t vibeos_reg_info(vibeos_reg_key_t k, vibeos_reg_info_t *out) {
    uint32_t s, st, c;
    reg_value_t *v;

    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        out->subkeys = g_keys[s].subkeys;
        out->values = g_keys[s].values;
        out->max_subkey_name = out->max_value_name = out->max_value_data = 0;
        for (c = g_keys[s].child; c != NONE; c = g_keys[c].sibling) {
            if (g_keys[c].name_len > out->max_subkey_name) {
                out->max_subkey_name = g_keys[c].name_len;
            }
        }
        for (v = g_keys[s].vhead; v; v = v->next) {
            if (v->name_len > out->max_value_name) {
                out->max_value_name = v->name_len;
            }
            if (v->data_len > out->max_value_data) {
                out->max_value_data = v->data_len;
            }
        }
        out->last_write = g_keys[s].last_write;
        out->name_len = g_keys[s].name_len;
    }
    unlock();
    return st;
}

static uint32_t put_name(void *b, uint32_t blen, uint16_t *buf, uint32_t cap, uint32_t *len) {
    *len = blen;
    if (cap < blen) {
        blob_get(b, blen, 0, buf, cap & ~1u);
        return VIBEOS_STATUS_BUFFER_OVERFLOW;
    }
    blob_get(b, blen, 0, buf, blen);
    return VIBEOS_STATUS_SUCCESS;
}

uint32_t vibeos_reg_name(vibeos_reg_key_t k, uint16_t *buf, uint32_t cap, uint32_t *len) {
    uint32_t s, st;

    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = put_name(g_keys[s].name, g_keys[s].name_len, buf, cap, len);
    }
    unlock();
    return st;
}

uint32_t vibeos_reg_enum_key(vibeos_reg_key_t k, uint32_t index, uint16_t *buf, uint32_t cap,
                             uint32_t *len, uint64_t *last_write) {
    uint32_t s, st, c, i = 0;

    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        for (c = g_keys[s].child; c != NONE && i < index; c = g_keys[c].sibling) {
            i++;
        }
        if (c == NONE) {
            st = VIBEOS_STATUS_NO_MORE_ENTRIES;
        } else {
            if (last_write) {
                *last_write = g_keys[c].last_write;
            }
            st = put_name(g_keys[c].name, g_keys[c].name_len, buf, cap, len);
        }
    }
    unlock();
    return st;
}

/* ---- values ----------------------------------------------------------------------------- */

/* Under the lock, on slot `s`. */
static uint32_t set_locked(uint32_t s, const uint16_t *name, uint32_t n, uint32_t type,
                           const void *data, uint32_t len) {
    uint32_t st = VIBEOS_STATUS_SUCCESS;
    reg_value_t *v;
    void *nd = 0;

    if (s == g_root) {
        st = VIBEOS_STATUS_ACCESS_DENIED;
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        /* The new data first, so a refusal leaves the old value as it was. */
        nd = blob_alloc(len);
        if (len != 0u && !nd) {
            st = quota_ok(len) ? VIBEOS_STATUS_INSUFFICIENT_RESOURCES : VIBEOS_STATUS_QUOTA_EXCEEDED;
        }
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        v = find_value(s, name, n);
        if (!v) {
            v = (reg_value_t *)blob_alloc((uint32_t)sizeof(*v));
            if (v) {
                v->name = blob_alloc(n * 2u);
                if (n != 0u && !v->name) {
                    blob_free(v, (uint32_t)sizeof(*v));
                    v = 0;
                }
            }
            if (!v) {
                blob_free(nd, len);
                st = VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
            } else {
                v->name_len = n * 2u;
                blob_put(v->name, n * 2u, 0, name, n * 2u);
                v->data = 0;
                v->data_len = 0;
                v->next = 0;
                if (g_keys[s].vtail) {
                    g_keys[s].vtail->next = v;
                } else {
                    g_keys[s].vhead = v;
                }
                g_keys[s].vtail = v;
                g_keys[s].values++;
            }
        }
        if (st == VIBEOS_STATUS_SUCCESS) {
            blob_free(v->data, v->data_len);
            v->data = nd;
            v->data_len = len;
            v->type = type;
            if (len != 0u) {
                blob_put(nd, len, 0, data, len);
            }
            touch(s);
        }
    }
    return st;
}

uint32_t vibeos_reg_set(vibeos_reg_key_t k, const uint16_t *name, uint32_t n, uint32_t type,
                        const void *data, uint32_t len) {
    uint32_t s, st;

    if (n > VIBEOS_REG_VALUE_NAME_MAX) {
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    if (len > VIBEOS_REG_DATA_MAX) {
        return VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
    }
    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = set_locked(s, name, n, type, data, len);
    }
    unlock();
    return st;
}

static uint32_t put_data(reg_value_t *v, uint32_t *type, void *data, uint32_t cap, uint32_t *len) {
    if (type) {
        *type = v->type;
    }
    *len = v->data_len;
    if (!data) {
        return VIBEOS_STATUS_SUCCESS;
    }
    if (cap < v->data_len) {
        blob_get(v->data, v->data_len, 0, data, cap);
        return VIBEOS_STATUS_BUFFER_OVERFLOW;
    }
    blob_get(v->data, v->data_len, 0, data, v->data_len);
    return VIBEOS_STATUS_SUCCESS;
}

uint32_t vibeos_reg_get(vibeos_reg_key_t k, const uint16_t *name, uint32_t n, uint32_t *type,
                        void *data, uint32_t cap, uint32_t *len) {
    uint32_t s, st;
    reg_value_t *v;

    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        v = find_value(s, name, n);
        st = v ? put_data(v, type, data, cap, len) : VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    unlock();
    return st;
}

uint32_t vibeos_reg_unset(vibeos_reg_key_t k, const uint16_t *name, uint32_t n) {
    uint32_t s, st;

    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        reg_value_t **link = &g_keys[s].vhead, *prev = 0;

        while (*link && name_cmp((*link)->name, (*link)->name_len, name, n) != 0) {
            prev = *link;
            link = &(*link)->next;
        }
        if (!*link) {
            st = VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND;
        } else {
            reg_value_t *v = *link;

            *link = v->next;
            if (g_keys[s].vtail == v) {
                g_keys[s].vtail = prev;
            }
            g_keys[s].values--;
            value_free(v);
            touch(s);
        }
    }
    unlock();
    return st;
}

uint32_t vibeos_reg_enum_value(vibeos_reg_key_t k, uint32_t index, uint16_t *name, uint32_t name_cap,
                               uint32_t *name_len, uint32_t *type, void *data, uint32_t cap,
                               uint32_t *len) {
    uint32_t s, st, i = 0;
    reg_value_t *v;

    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        for (v = g_keys[s].vhead; v && i < index; v = v->next) {
            i++;
        }
        if (!v) {
            st = VIBEOS_STATUS_NO_MORE_ENTRIES;
        } else {
            uint32_t a = name ? put_name(v->name, v->name_len, name, name_cap, name_len)
                              : VIBEOS_STATUS_SUCCESS;
            uint32_t b;

            if (!name && name_len) {
                *name_len = v->name_len;
            }
            b = put_data(v, type, data, cap, len);
            st = a != VIBEOS_STATUS_SUCCESS ? a : b;
        }
    }
    unlock();
    return st;
}

uint64_t vibeos_reg_changes(void) {
    uint64_t c;

    lock();
    c = g_changes;
    unlock();
    return c;
}

uint64_t vibeos_reg_bytes(void) {
    uint64_t b;

    lock();
    b = g_bytes;
    unlock();
    return b;
}

/* ---- hives -------------------------------------------------------------------------------
 *
 * The image: a 32-byte header - "VREG", the version, the record count, the
 * payload's length and its FNV-1a 64 - then the records. A key is
 *   'K' 0 depth:u16 name_len:u16 0:u16 last_write:u64 name
 * at depth 1 for the saved key's children, its parent the nearest key before
 * it at depth - 1; its values follow it, each
 *   'V' 0 name_len:u16 type:u32 data_len:u32 name data
 * and the saved key's own values come first, at depth 0. Volatile keys and
 * everything under them are not written. */

#define HIVE_MAGIC 0x47455256u   /* "VREG" */
#define HIVE_VERSION 1u
#define HIVE_HEADER 32u

typedef struct {
    int (*write)(void *ctx, const void *buf, uint32_t len);
    void *ctx;
    int dry;            /* counting and summing, not writing */
    uint64_t length, sum;
    uint32_t records;
    int failed;
} hive_out_t;

static void sum_bytes(uint64_t *h, const uint8_t *b, uint32_t n) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        *h = (*h ^ b[i]) * 0x100000001B3ull;
    }
}

static void out_bytes(hive_out_t *o, const void *b, uint32_t n) {
    if (o->failed || n == 0u) {
        return;
    }
    if (o->dry) {
        sum_bytes(&o->sum, (const uint8_t *)b, n);
        o->length += n;
    } else if (o->write(o->ctx, b, n) != 0) {
        o->failed = 1;
    }
}

/* A blob through a small buffer, so a megabyte of data is not on the stack. */
static void out_blob(hive_out_t *o, void *b, uint32_t len) {
    uint8_t buf[256];
    uint32_t at = 0;

    while (at < len) {
        uint32_t n = len - at > sizeof(buf) ? (uint32_t)sizeof(buf) : len - at;

        blob_get(b, len, at, buf, n);
        out_bytes(o, buf, n);
        at += n;
    }
}

static void out_values(hive_out_t *o, uint32_t s) {
    reg_value_t *v;

    for (v = g_keys[s].vhead; v; v = v->next) {
        uint8_t h[12];

        h[0] = 'V';
        h[1] = 0;
        h[2] = (uint8_t)v->name_len;
        h[3] = (uint8_t)(v->name_len >> 8);
        h[4] = (uint8_t)v->type;
        h[5] = (uint8_t)(v->type >> 8);
        h[6] = (uint8_t)(v->type >> 16);
        h[7] = (uint8_t)(v->type >> 24);
        h[8] = (uint8_t)v->data_len;
        h[9] = (uint8_t)(v->data_len >> 8);
        h[10] = (uint8_t)(v->data_len >> 16);
        h[11] = (uint8_t)(v->data_len >> 24);
        out_bytes(o, h, 12u);
        out_blob(o, v->name, v->name_len);
        out_blob(o, v->data, v->data_len);
        o->records++;
    }
}

static void out_tree(hive_out_t *o, uint32_t top) {
    uint32_t s = g_keys[top].child, depth = 1;

    out_values(o, top);
    while (s != NONE && s != top) {
        if (!(g_keys[s].flags & REG_FLAG_VOLATILE)) {
            uint8_t h[16];
            uint32_t i;

            h[0] = 'K';
            h[1] = 0;
            h[2] = (uint8_t)depth;
            h[3] = (uint8_t)(depth >> 8);
            h[4] = (uint8_t)g_keys[s].name_len;
            h[5] = (uint8_t)(g_keys[s].name_len >> 8);
            h[6] = h[7] = 0;
            for (i = 0; i < 8u; i++) {
                h[8u + i] = (uint8_t)(g_keys[s].last_write >> (8u * i));
            }
            out_bytes(o, h, 16u);
            out_blob(o, g_keys[s].name, g_keys[s].name_len);
            out_values(o, s);
            o->records++;
            if (g_keys[s].child != NONE) {
                s = g_keys[s].child;
                depth++;
                continue;
            }
        }
        /* Next sibling, or up until there is one. */
        while (s != top && g_keys[s].sibling == NONE) {
            s = g_keys[s].parent;
            depth--;
        }
        if (s == top) {
            break;
        }
        s = g_keys[s].sibling;
    }
}

uint32_t vibeos_reg_save(vibeos_reg_key_t k, int (*write)(void *ctx, const void *buf, uint32_t len),
                         void *ctx) {
    hive_out_t o;
    uint32_t s, st, i;
    uint8_t h[HIVE_HEADER];

    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        /* Twice: once to learn the length and the checksum the header carries,
         * once to write. The tree cannot change between, under the lock. */
        o.write = write;
        o.ctx = ctx;
        o.dry = 1;
        o.length = 0;
        o.sum = 0xCBF29CE484222325ull;
        o.records = 0;
        o.failed = 0;
        out_tree(&o, s);
        for (i = 0; i < HIVE_HEADER; i++) {
            h[i] = 0;
        }
        for (i = 0; i < 4u; i++) {
            h[i] = (uint8_t)(HIVE_MAGIC >> (8u * i));
            h[4u + i] = (uint8_t)(HIVE_VERSION >> (8u * i));
            h[8u + i] = (uint8_t)(o.records >> (8u * i));
        }
        for (i = 0; i < 8u; i++) {
            h[16u + i] = (uint8_t)(o.length >> (8u * i));
            h[24u + i] = (uint8_t)(o.sum >> (8u * i));
        }
        o.dry = 0;
        out_bytes(&o, h, HIVE_HEADER);
        out_tree(&o, s);
        if (o.failed) {
            st = VIBEOS_STATUS_UNSUCCESSFUL;
        }
    }
    unlock();
    return st;
}

static uint64_t rd(const uint8_t *b, uint32_t n) {
    uint64_t v = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        v |= (uint64_t)b[i] << (8u * i);
    }
    return v;
}

static uint16_t g_name_bounce[VIBEOS_REG_VALUE_NAME_MAX];   /* under the lock */

/* One pass over the image: with `build` null, only checks it; with it,
 * makes the keys under `build` (a detached key) and its values. 0, or the
 * status that refuses it. */
static uint32_t hive_walk(const uint8_t *p, uint64_t len, uint32_t build, uint32_t *records) {
    uint64_t at = 0;
    uint32_t depth = 0, cur = build;
    uint32_t keys = 0;

    *records = 0;
    while (at < len) {
        ++*records;
        uint8_t tag = p[at];

        if (tag == 'V') {
            uint32_t nl, type, dl;

            if (len - at < 12u) {
                return VIBEOS_STATUS_REGISTRY_CORRUPT;
            }
            nl = (uint32_t)rd(p + at + 2u, 2u);
            type = (uint32_t)rd(p + at + 4u, 4u);
            dl = (uint32_t)rd(p + at + 8u, 4u);
            if ((nl & 1u) || nl / 2u > VIBEOS_REG_VALUE_NAME_MAX || dl > VIBEOS_REG_DATA_MAX ||
                len - at - 12u < (uint64_t)nl + dl) {
                return VIBEOS_STATUS_REGISTRY_CORRUPT;
            }
            if (build != NONE) {
                uint32_t i, st;

                /* Through a bounce under the lock: the image has no alignment
                 * to promise, and a name may be 16,383 units. */
                for (i = 0; i < nl / 2u; i++) {
                    g_name_bounce[i] = (uint16_t)rd(p + at + 12u + 2u * i, 2u);
                }
                st = set_locked(cur, g_name_bounce, nl / 2u, type, p + at + 12u + nl, dl);
                if (st != VIBEOS_STATUS_SUCCESS) {
                    return st;
                }
            }
            at += 12u + nl + dl;
        } else if (tag == 'K') {
            uint32_t d, nl, i;
            uint16_t name[VIBEOS_REG_KEY_NAME_MAX];

            if (len - at < 16u) {
                return VIBEOS_STATUS_REGISTRY_CORRUPT;
            }
            d = (uint32_t)rd(p + at + 2u, 2u);
            nl = (uint32_t)rd(p + at + 4u, 2u);
            if (d == 0u || d > depth + 1u || d > VIBEOS_REG_DEPTH_MAX || (nl & 1u) || nl == 0u ||
                nl / 2u > VIBEOS_REG_KEY_NAME_MAX || len - at - 16u < nl ||
                ++keys > VIBEOS_REG_KEYS_MAX) {
                return VIBEOS_STATUS_REGISTRY_CORRUPT;
            }
            for (i = 0; i < nl / 2u; i++) {
                name[i] = (uint16_t)rd(p + at + 16u + 2u * i, 2u);
                if (name[i] == '\\') {
                    return VIBEOS_STATUS_REGISTRY_CORRUPT;
                }
            }
            if (build != NONE) {
                uint32_t parent = cur, c;

                while (depth >= d) {
                    parent = g_keys[parent].parent;
                    depth--;
                }
                if (find_child(parent, name, nl / 2u) != NONE) {
                    return VIBEOS_STATUS_REGISTRY_CORRUPT;   /* a name twice */
                }
                c = key_new(name, nl / 2u, 0u);
                if (c == NONE) {
                    return VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
                }
                link_child(parent, c);
                g_keys[c].last_write = rd(p + at + 8u, 8u);
                cur = c;
            }
            depth = d;
            at += 16u + nl;
        } else {
            return VIBEOS_STATUS_REGISTRY_CORRUPT;
        }
    }
    return VIBEOS_STATUS_SUCCESS;
}

/* Everything under `s` that is not volatile, and its values: gone. */
static void drop_saved(uint32_t s) {
    uint32_t c = g_keys[s].child;
    reg_value_t *v = g_keys[s].vhead;

    while (v) {
        reg_value_t *n = v->next;

        value_free(v);
        v = n;
    }
    g_keys[s].vhead = g_keys[s].vtail = 0;
    g_keys[s].values = 0;
    while (c != NONE) {
        uint32_t next = g_keys[c].sibling;

        if (!(g_keys[c].flags & REG_FLAG_VOLATILE)) {
            kill_subtree(c);
        }
        c = next;
    }
}

uint32_t vibeos_reg_load(vibeos_reg_key_t k, const void *img, uint64_t len) {
    const uint8_t *p = (const uint8_t *)img;
    uint64_t plen, sum = 0xCBF29CE484222325ull;
    uint32_t s, st, tmp, records = 0;

    /* Every byte of the header means something or must be zero: a field
     * nothing checks is one a corrupted image can change unseen. */
    if (len < HIVE_HEADER || rd(p, 4u) != HIVE_MAGIC || rd(p + 4u, 4u) != HIVE_VERSION ||
        rd(p + 12u, 4u) != 0u) {
        return VIBEOS_STATUS_REGISTRY_CORRUPT;
    }
    plen = rd(p + 16u, 8u);
    if (plen != len - HIVE_HEADER) {
        return VIBEOS_STATUS_REGISTRY_CORRUPT;
    }
    sum_bytes(&sum, p + HIVE_HEADER, (uint32_t)plen);
    if (plen > 0xFFFFFFFFull || sum != rd(p + 24u, 8u)) {
        return VIBEOS_STATUS_REGISTRY_CORRUPT;
    }
    lock();
    st = live(k, &s);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = hive_walk(p + HIVE_HEADER, plen, NONE, &records);   /* the whole image first */
        if (st == VIBEOS_STATUS_SUCCESS && records != (uint32_t)rd(p + 8u, 4u)) {
            st = VIBEOS_STATUS_REGISTRY_CORRUPT;
        }
    }
    tmp = NONE;
    if (st == VIBEOS_STATUS_SUCCESS) {
        /* Built detached, then swapped in: a load that runs out of memory half
         * way leaves the tree as it was. */
        tmp = key_new(0, 0u, 0u);
        if (tmp == NONE) {
            st = VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
        } else {
            g_keys[tmp].refs = 1u;
            st = hive_walk(p + HIVE_HEADER, plen, tmp, &records);
        }
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        uint32_t c = g_keys[tmp].child;

        drop_saved(s);
        while (c != NONE) {
            uint32_t next = g_keys[c].sibling;

            uint16_t buf[VIBEOS_REG_KEY_NAME_MAX];

            g_keys[tmp].subkeys--;
            g_keys[c].sibling = NONE;
            g_keys[c].parent = NONE;
            blob_get(g_keys[c].name, g_keys[c].name_len, 0, buf, g_keys[c].name_len);
            if (find_child(s, buf, g_keys[c].name_len / 2u) != NONE) {
                /* A volatile key of the same name stays, and the saved one is
                 * not loaded over it. */
                kill_subtree(c);
            } else {
                link_child(s, c);
            }
            c = next;
        }
        g_keys[tmp].child = NONE;
        /* The saved key's own values moved too. */
        g_keys[s].vhead = g_keys[tmp].vhead;
        g_keys[s].vtail = g_keys[tmp].vtail;
        g_keys[s].values = g_keys[tmp].values;
        g_keys[tmp].vhead = g_keys[tmp].vtail = 0;
        g_keys[tmp].values = 0;
        g_changes++;
    }
    if (tmp != NONE) {
        if (st != VIBEOS_STATUS_SUCCESS) {
            drop_saved(tmp);
        }
        g_keys[tmp].refs = 0;
        key_kill(tmp);
    }
    unlock();
    return st;
}
