/* Randomised torture for the registry's core (kernel/abi/nt/registry/reg.c,
 * docs/abi/windows.md W8 step 2), checked against a model that shares
 * nothing with it.
 *
 *     vibeos_reg_torture <seed> <rounds>
 *
 * The registry keeps a tree of slots linked by index, names and data in size
 * classes and page chains, children in name order, values in the order they
 * were set, and a hive format that has to round-trip all of it. The model is
 * flat: keys are paths in an array, compared upper-cased as plain strings;
 * values are rows naming their key, with data generated from a seed so it is
 * never stored twice. Each round makes one random change to both - make a
 * key, delete a leaf, rename, set a value of a random size across every size
 * class, unset one - and every few rounds the whole of Machine is saved and
 * loaded back over itself, which a correct registry survives unchanged.
 *
 * Checked every round: the change's own answer; and for every key in the
 * model, that the registry opens it from the root by its path in a case
 * chosen at random, enumerates the same children in the same order, and
 * holds the same values in the same order with the same bytes. The quota is
 * small enough that some rounds are refused, and a refusal must change
 * nothing - which the next comparison checks. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/ntreg.h"
#include "vibeos/nt_layout.h"

#define KEYS 64
#define VALUES 256
#define NAMELEN 12

typedef struct {
    int used;
    int parent;              /* -1: Machine itself */
    char name[NAMELEN + 1];  /* as made, mixed case */
} m_key_t;

typedef struct {
    int used;
    int key;
    char name[NAMELEN + 1];
    uint32_t type, len, seed;
    unsigned order;          /* when first set, for enumeration order */
} m_val_t;

static m_key_t g_k[KEYS];
static m_val_t g_v[VALUES];
static unsigned g_order;
static uint64_t g_rng;
static unsigned g_round;
static int g_failed;
static int g_locked;

static uint32_t rnd(uint32_t n) {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g_rng >> 33) % n;
}

static void fail(const char *what) {
    printf("FAIL:reg_torture round=%u %s\n", g_round, what);
    g_failed = 1;
}

static void *pa(void) { return malloc(4096); }
static void pf(void *p) { free(p); }
static void lk(void) { if (g_locked) { fail("lock taken twice"); } g_locked = 1; }
static void ul(void) { g_locked = 0; }
static uint64_t now(void) { return 1; }

static int upeq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;

        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int upcmp(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;

        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || !x) return (unsigned char)x - (unsigned char)y;
    }
}

/* The path of model key `i` from Machine, each character's case flipped at
 * random: the registry must find it however it is written. */
static int path_of(int i, uint16_t *out) {
    char tmp[KEYS * (NAMELEN + 1)];
    int chain[KEYS], n = 0, len = 0, j;

    for (; i >= 0; i = g_k[i].parent) {
        chain[n++] = i;
    }
    while (n > 0) {
        const char *s = g_k[chain[--n]].name;

        if (len) tmp[len++] = '\\';
        for (j = 0; s[j]; j++) {
            char c = s[j];

            if (rnd(2)) {
                if (c >= 'a' && c <= 'z') c -= 32;
                else if (c >= 'A' && c <= 'Z') c += 32;
            }
            tmp[len++] = c;
        }
    }
    for (j = 0; j < len; j++) out[j] = (uint16_t)(unsigned char)tmp[j];
    return len;
}

static void w16(const char *s, uint16_t *o, uint32_t *n) {
    uint32_t i;

    for (i = 0; s[i]; i++) o[i] = (uint16_t)(unsigned char)s[i];
    *n = i;
}

static void fill(uint8_t *b, uint32_t len, uint32_t seed) {
    uint32_t i;

    for (i = 0; i < len; i++) b[i] = (uint8_t)((seed * 2654435761u + i * 40503u) >> 13);
}

static vibeos_reg_key_t g_machine;

static int open_model(int i, vibeos_reg_key_t *k) {
    uint16_t p[KEYS * (NAMELEN + 1)];
    int n;

    if (i < 0) {
        vibeos_reg_ref(g_machine);
        *k = g_machine;
        return 1;
    }
    n = path_of(i, p);
    return vibeos_reg_open(g_machine, p, (uint32_t)n, k) == VIBEOS_STATUS_SUCCESS;
}

static void random_name(char *s) {
    static const char a[] = "abcXYZqrsTUV019";
    uint32_t n = 1 + rnd(NAMELEN), i;

    for (i = 0; i < n; i++) s[i] = a[rnd(sizeof(a) - 1)];
    s[n] = 0;
}

static int child_named(int parent, const char *name) {
    int i;

    for (i = 0; i < KEYS; i++) {
        if (g_k[i].used && g_k[i].parent == parent && upeq(g_k[i].name, name)) return i;
    }
    return -2;
}

static int has_children(int k) {
    int i;

    for (i = 0; i < KEYS; i++) {
        if (g_k[i].used && g_k[i].parent == k) return 1;
    }
    return 0;
}

static int random_key(void) {
    int i, tries;

    for (tries = 0; tries < 8; tries++) {
        i = (int)rnd(KEYS + 4) - 4;   /* Machine itself now and then */
        if (i < 0 || g_k[i].used) return i < 0 ? -1 : i;
    }
    return -1;
}

static int g_img_len;
static uint8_t g_img[8 << 20];

static int sink(void *ctx, const void *b, uint32_t n) {
    (void)ctx;
    if ((size_t)g_img_len + n > sizeof(g_img)) return -1;
    memcpy(g_img + g_img_len, b, n);
    g_img_len += (int)n;
    return 0;
}

/* Every model key against the registry: its children and its values. */
static void compare(void) {
    int i;
    static uint8_t want[40000], got[40000];

    for (i = -1; i < KEYS; i++) {
        vibeos_reg_key_t k;
        int kids[KEYS], nk = 0, j, a, b, nv = 0;
        m_val_t *vals[VALUES];
        uint16_t nb[64];
        uint32_t len, type, dl;

        if (i >= 0 && !g_k[i].used) continue;
        if (!open_model(i, &k)) {
            fail("a key of the model does not open by its path");
            continue;
        }
        for (j = 0; j < KEYS; j++) {
            if (g_k[j].used && g_k[j].parent == i) kids[nk++] = j;
        }
        for (a = 0; a < nk; a++) {      /* the model's order: by name, without case */
            for (b = a + 1; b < nk; b++) {
                if (upcmp(g_k[kids[b]].name, g_k[kids[a]].name) < 0) {
                    int t = kids[a]; kids[a] = kids[b]; kids[b] = t;
                }
            }
        }
        for (j = 0; j <= nk; j++) {
            uint32_t st = vibeos_reg_enum_key(k, (uint32_t)j, nb, sizeof(nb), &len, 0);
            char s[NAMELEN + 1];
            uint32_t c;

            if (j == nk) {
                if (st != VIBEOS_STATUS_NO_MORE_ENTRIES) fail("more subkeys than the model");
                break;
            }
            if (st != 0) { fail("fewer subkeys than the model"); break; }
            for (c = 0; c < len / 2 && c < NAMELEN; c++) s[c] = (char)nb[c];
            s[c] = 0;
            if (strcmp(s, g_k[kids[j]].name) != 0) { fail("a subkey's name or order differs"); break; }
        }
        for (j = 0; j < VALUES; j++) {
            if (g_v[j].used && g_v[j].key == i) vals[nv++] = &g_v[j];
        }
        for (a = 0; a < nv; a++) {
            for (b = a + 1; b < nv; b++) {
                if (vals[b]->order < vals[a]->order) { m_val_t *t = vals[a]; vals[a] = vals[b]; vals[b] = t; }
            }
        }
        for (j = 0; j <= nv; j++) {
            uint32_t st = vibeos_reg_enum_value(k, (uint32_t)j, nb, sizeof(nb), &len, &type, got,
                                                sizeof(got), &dl);
            char s[NAMELEN + 1];
            uint32_t c;

            if (j == nv) {
                if (st != VIBEOS_STATUS_NO_MORE_ENTRIES) fail("more values than the model");
                break;
            }
            if (st != 0) { fail("fewer values than the model"); break; }
            for (c = 0; c < len / 2 && c < NAMELEN; c++) s[c] = (char)nb[c];
            s[c] = 0;
            fill(want, vals[j]->len, vals[j]->seed);
            if (!upeq(s, vals[j]->name) || type != vals[j]->type || dl != vals[j]->len ||
                memcmp(got, want, dl) != 0) {
                fail("a value's name, order, type or bytes differ");
                break;
            }
        }
        vibeos_reg_unref(k);
    }
}

int main(int argc, char **argv) {
    vibeos_reg_env_t env = { pa, pf, lk, ul, now, 2u << 20 };
    unsigned rounds = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 2000u;
    uint64_t seed = argc > 1 ? strtoull(argv[1], 0, 10) : 1u;
    static uint8_t data[40000];
    vibeos_reg_key_t root;
    uint16_t mname[8];
    uint32_t mn;

    g_rng = seed * 0x9E3779B97F4A7C15ull + 1u;
    vibeos_reg_init(&env);
    root = vibeos_reg_root();
    w16("Machine", mname, &mn);
    if (vibeos_reg_open(root, mname, mn, &g_machine) != 0) {
        printf("FAIL:reg_torture no Machine\n");
        return 1;
    }
    for (g_round = 0; g_round < rounds && !g_failed; g_round++) {
        uint32_t op = rnd(100);

        if (op < 25) {                                   /* make a key */
            int p = random_key(), slot, existing;
            char name[NAMELEN + 1];
            vibeos_reg_key_t pk, k;
            uint16_t n16[NAMELEN];
            uint32_t n, disp = 0, st;

            for (slot = 0; slot < KEYS && g_k[slot].used; slot++) {}
            if (!open_model(p, &pk)) { fail("parent does not open"); break; }
            random_name(name);
            w16(name, n16, &n);
            existing = child_named(p, name);
            st = vibeos_reg_create(pk, n16, n, 0, &k, &disp);
            if (existing >= 0) {
                if (st != 0 || disp != VIBEOS_REG_OPENED) fail("an existing key was not opened");
            } else if (slot == KEYS) {
                if (st == 0) { vibeos_reg_delete(k); }   /* the model is full: undo */
            } else if (st == 0) {
                if (disp != VIBEOS_REG_CREATED) fail("a new key was not made");
                g_k[slot].used = 1;
                g_k[slot].parent = p;
                strcpy(g_k[slot].name, name);
            } else if (st != VIBEOS_STATUS_QUOTA_EXCEEDED && st != VIBEOS_STATUS_INSUFFICIENT_RESOURCES) {
                fail("a key could not be made");
            }
            if (st == 0) vibeos_reg_unref(k);
            vibeos_reg_unref(pk);
        } else if (op < 35) {                            /* delete a key */
            int i = random_key();
            vibeos_reg_key_t k;
            uint32_t st, j;

            if (i < 0) continue;
            if (!open_model(i, &k)) { fail("a key does not open"); break; }
            st = vibeos_reg_delete(k);
            if (has_children(i)) {
                if (st != VIBEOS_STATUS_CANNOT_DELETE) fail("a key with children was deleted");
            } else if (st != 0) {
                fail("a leaf was not deleted");
            } else {
                g_k[i].used = 0;
                for (j = 0; j < VALUES; j++) {
                    if (g_v[j].used && g_v[j].key == i) g_v[j].used = 0;
                }
            }
            vibeos_reg_unref(k);
        } else if (op < 42) {                            /* rename a key */
            int i = random_key(), other;
            char name[NAMELEN + 1];
            vibeos_reg_key_t k;
            uint16_t n16[NAMELEN];
            uint32_t n, st;

            if (i < 0) continue;
            if (!open_model(i, &k)) { fail("a key does not open"); break; }
            random_name(name);
            w16(name, n16, &n);
            other = child_named(g_k[i].parent, name);
            st = vibeos_reg_rename(k, n16, n);
            if (other == i) {
                /* Onto its own name, in any case: Windows' CANNOT_DELETE. */
                if (st != VIBEOS_STATUS_CANNOT_DELETE) fail("a rename onto its own name was not refused");
            } else if (other >= 0) {
                if (st != VIBEOS_STATUS_OBJECT_NAME_COLLISION) fail("a rename onto a sibling did not collide");
            } else if (st == 0) {
                strcpy(g_k[i].name, name);
            } else if (st != VIBEOS_STATUS_INSUFFICIENT_RESOURCES && st != VIBEOS_STATUS_QUOTA_EXCEEDED) {
                fail("a rename failed");
            }
            vibeos_reg_unref(k);
        } else if (op < 80) {                            /* set a value */
            int i = random_key(), j, slot = -1;
            char name[NAMELEN + 1];
            vibeos_reg_key_t k;
            uint16_t n16[NAMELEN];
            uint32_t n, st, sizes[] = { 0, 4, 31, 33, 600, 2048, 2049, 5000, 9000, 39000 };
            uint32_t len = sizes[rnd(10)], sd = rnd(1u << 30), type = rnd(12);

            if (!open_model(i, &k)) { fail("a key does not open"); break; }
            random_name(name);
            if (rnd(4) == 0) name[0] = 0;                 /* the default value */
            w16(name, n16, &n);
            for (j = 0; j < VALUES; j++) {
                if (g_v[j].used && g_v[j].key == i && upeq(g_v[j].name, name)) slot = j;
            }
            if (slot < 0) {
                for (j = 0; j < VALUES && g_v[j].used; j++) {}
                if (j == VALUES) { vibeos_reg_unref(k); continue; }
            }
            fill(data, len, sd);
            st = vibeos_reg_set(k, n16, n, type, data, len);
            if (st == 0) {
                if (slot < 0) {
                    slot = j;
                    g_v[slot].used = 1;
                    g_v[slot].key = i;
                    strcpy(g_v[slot].name, name);
                    g_v[slot].order = g_order++;
                }
                g_v[slot].type = type;
                g_v[slot].len = len;
                g_v[slot].seed = sd;
            } else if (st != VIBEOS_STATUS_QUOTA_EXCEEDED && st != VIBEOS_STATUS_INSUFFICIENT_RESOURCES) {
                fail("a value could not be set");
            }
            vibeos_reg_unref(k);
        } else if (op < 95) {                            /* unset a value */
            int j = (int)rnd(VALUES);
            vibeos_reg_key_t k;
            uint16_t n16[NAMELEN];
            uint32_t n;

            if (!g_v[j].used) continue;
            if (!open_model(g_v[j].key, &k)) { fail("a key does not open"); break; }
            w16(g_v[j].name, n16, &n);
            if (vibeos_reg_unset(k, n16, n) != 0) fail("a value could not be unset");
            g_v[j].used = 0;
            vibeos_reg_unref(k);
        } else {                                         /* save and load back */
            uint32_t st;

            g_img_len = 0;
            if (vibeos_reg_save(g_machine, sink, 0) != 0) { fail("a save failed"); break; }
            st = vibeos_reg_load(g_machine, g_img, (uint64_t)g_img_len);
            if (st != 0 && st != VIBEOS_STATUS_QUOTA_EXCEEDED && st != VIBEOS_STATUS_INSUFFICIENT_RESOURCES) {
                fail("a saved image did not load");
            }
        }
        if (g_locked) fail("the lock was left held");
        compare();
    }
    if (g_failed) {
        printf("reg_torture seed=%llu FAILED\n", (unsigned long long)seed);
        return 1;
    }
    printf("reg_torture seed=%llu rounds=%u ok bytes=%llu\n", (unsigned long long)seed, rounds,
           (unsigned long long)vibeos_reg_bytes());
    vibeos_reg_unref(g_machine);
    vibeos_reg_unref(root);
    vibeos_reg_fini();   /* every page back, which the sanitized nightly's leak check asks */
    return 0;
}
