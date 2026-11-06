/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Randomised torture for the DRM device (kernel/drm/drm_drv.c), checked
 * against a model of handles, names, references and fake map offsets.
 *
 *     vibeos_drm_torture <seed> <rounds>
 *
 * The permanent test checks the cases somebody thought of. This one runs
 * thousands of interleavings of create, close, flink and open across four
 * files, with every return value, every handle, every name and every page
 * compared against a model that shares no code with the driver. A name must
 * stay alive exactly while one handle still refers to its object; a closed
 * object's offset must stop resolving; a file that is closed must give back
 * every reference it held. The last checks after all files are gone are what
 * catch a leaked reference or a name table entry that outlived its object. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/abi_linux.h"
#include "vibeos/drm.h"
#include "vibeos/drm_uapi.h"
#include "vibeos/file.h"
#include "vibeos/ksvc.h"

#define T_FILES 4
#define T_GEMS 16
#define T_OBJS 64
#define T_PAGES 16
#define T_HISTORY 8192

typedef struct {
    uint32_t w;
    uint32_t h;
    uint32_t bpp;
} t_shape_t;

typedef struct {
    int live;
    uint32_t size;
    uint32_t npages;
    uint32_t refs;
    uint32_t name;
    uint64_t offset;
    void *page[T_PAGES];
} t_obj_t;

typedef struct {
    vibeos_file_t *f;
    int handle[T_GEMS];   /* object index + 1, 0 when the slot is free */
} t_file_t;

static const t_shape_t g_shapes[] = {
    {4, 4, 32}, {16, 16, 16}, {32, 8, 32}, {128, 16, 32}, {300, 20, 32},
    {31, 7, 24}, {8, 9, 8}, {1, 1, 8}, {1000, 10, 32},
};

static t_obj_t g_obj[T_OBJS];
static t_file_t g_file[T_FILES];
static uint64_t g_old_offset[T_HISTORY];
static uint32_t g_old_name[T_HISTORY];
static size_t g_nold_offset;
static size_t g_nold_name;
static uint64_t g_rng;
static uint64_t g_seed;
static long g_round;
static const char *g_op;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

static void fail(const char *what, long got, long want) {
    printf("FAIL:drm_torture seed=%llu round=%ld op=%s %s got=%ld want=%ld\n",
           (unsigned long long)g_seed, g_round, g_op, what, got, want);
    exit(1);
}

static void expect(long got, long want, const char *what) {
    if (got != want) {
        fail(what, got, want);
    }
}

static void expect_ptr(const void *got, const void *want, const char *what) {
    if (got != want) {
        printf("FAIL:drm_torture seed=%llu round=%ld op=%s %s got=%p want=%p\n",
               (unsigned long long)g_seed, g_round, g_op, what, got, want);
        exit(1);
    }
}

static long call(vibeos_file_t *f, uint64_t req, const void *arg) {
    return f->ops->ioctl(f, req, (uint64_t)(uintptr_t)arg);
}

static int first_slot(const t_file_t *mf) {
    int i;

    for (i = 0; i < T_GEMS; i++) {
        if (!mf->handle[i]) {
            return i;
        }
    }
    return -1;
}

static int first_obj(void) {
    int i;

    for (i = 0; i < T_OBJS; i++) {
        if (!g_obj[i].live) {
            return i;
        }
    }
    return -1;
}

static int open_files(void) {
    int i;
    int n = 0;

    for (i = 0; i < T_FILES; i++) {
        if (g_file[i].f) {
            n++;
        }
    }
    return n;
}

static int pick_file(void) {
    int live[T_FILES];
    int n = 0;
    int i;

    for (i = 0; i < T_FILES; i++) {
        if (g_file[i].f) {
            live[n++] = i;
        }
    }
    if (n == 0) {
        return -1;
    }
    return live[rnd() % (uint32_t)n];
}

static int pick_other_file(int notthis) {
    int live[T_FILES];
    int n = 0;
    int i;

    for (i = 0; i < T_FILES; i++) {
        if (g_file[i].f && i != notthis) {
            live[n++] = i;
        }
    }
    if (n == 0) {
        return -1;
    }
    return live[rnd() % (uint32_t)n];
}

/* A per-file GEM name lives in the namespace of the file that minted it, so
 * the only file that can resolve a named object is the one that already holds
 * a handle to it (review M-082). These helpers track that owner. */
static int owner_of(int oid) {
    int j;

    for (j = 0; j < T_FILES; j++) {
        int k;

        if (!g_file[j].f) {
            continue;
        }
        for (k = 0; k < T_GEMS; k++) {
            if (g_file[j].handle[k] == oid + 1) {
                return j;
            }
        }
    }
    return -1;
}

static int file_named_count(int fi) {
    uint32_t n = 0;
    int k;

    for (k = 0; k < T_GEMS; k++) {
        int oid = g_file[fi].handle[k] - 1;

        if (oid >= 0 && g_obj[oid].name) {
            n++;
        }
    }
    return n;
}

static int file_name_used(int fi, uint32_t name) {
    int k;

    for (k = 0; k < T_GEMS; k++) {
        int oid = g_file[fi].handle[k] - 1;

        if (oid >= 0 && g_obj[oid].name == name) {
            return 1;
        }
    }
    return 0;
}

static void remember_offset(uint64_t offset) {
    if (g_nold_offset < T_HISTORY) {
        g_old_offset[g_nold_offset++] = offset;
    }
}

static void remember_name(uint32_t name) {
    if (g_nold_name < T_HISTORY) {
        g_old_name[g_nold_name++] = name;
    }
}

static void shape_of(uint32_t which, uint32_t *w, uint32_t *h, uint32_t *bpp, uint32_t *pitch,
                     uint32_t *size, uint32_t *npages) {
    const t_shape_t *s = &g_shapes[which % (sizeof(g_shapes) / sizeof(g_shapes[0]))];
    uint64_t p = ((((uint64_t)s->w * s->bpp) + 7u) / 8u + 63u) & ~63ull;
    uint64_t n = p * s->h;

    *w = s->w;
    *h = s->h;
    *bpp = s->bpp;
    *pitch = (uint32_t)p;
    *size = (uint32_t)n;
    *npages = (uint32_t)((n + 4095ull) / 4096ull);
}

static void check_pages(int oid, t_file_t *mf, int slot) {
    t_obj_t *o = &g_obj[oid];
    struct vibeos_drm_mode_map_dumb map;
    uint32_t p;

    memset(&map, 0, sizeof(map));
    map.handle = (uint32_t)slot + 1u;
    expect(call(mf->f, VIBEOS_DRM_IOCTL_MODE_MAP_DUMB, &map), 0, "map a live handle");
    expect(map.pad, 0, "map clears pad");
    if (map.offset != o->offset) {
        fail("map offset", (long)map.offset, (long)o->offset);
    }
    for (p = 0; p < o->npages; p++) {
        void *page = 0;

        expect(mf->f->ops->share_page(mf->f, o->offset + (uint64_t)p * 4096ull, &page), 0,
               "share a live page");
        expect_ptr(page, o->page[p], "shared page identity");
        ks_page_unhold(page);
    }
    {
        void *page = 0;

        expect(mf->f->ops->share_page(mf->f, o->offset + (uint64_t)o->npages * 4096ull, &page), 1,
               "share stops past the object");
    }
}

static void check_all(void) {
    uint64_t offsets[T_OBJS];
    int n = 0;
    int i;
    int j;

    for (i = 0; i < T_OBJS; i++) {
        uint32_t refs = 0;

        if (!g_obj[i].live) {
            continue;
        }
        if (g_obj[i].refs == 0u) {
            fail("a live object has no reference", 0, 1);
        }
        if (g_obj[i].npages == 0u || g_obj[i].npages > T_PAGES) {
            fail("object page count", (long)g_obj[i].npages, T_PAGES);
        }
        for (j = 0; j < T_FILES; j++) {
            int k;

            if (!g_file[j].f) {
                continue;
            }
            for (k = 0; k < T_GEMS; k++) {
                if (g_file[j].handle[k] == i + 1) {
                    refs++;
                }
            }
        }
        expect(refs, g_obj[i].refs, "reference count matches the handles");
        offsets[n++] = g_obj[i].offset;
    }
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (offsets[i] == offsets[j]) {
                fail("two objects share an offset", (long)offsets[i], (long)offsets[j]);
            }
        }
    }
    for (i = 0; i < T_FILES; i++) {
        int k;

        if (!g_file[i].f) {
            continue;
        }
        for (j = 0; j < T_GEMS; j++) {
            int oa = g_file[i].handle[j] - 1;
            int m;

            if (oa < 0 || !g_obj[oa].name) {
                continue;
            }
            for (m = j + 1; m < T_GEMS; m++) {
                int ob = g_file[i].handle[m] - 1;

                if (ob >= 0 && g_obj[oa].name == g_obj[ob].name) {
                    fail("two objects in one file share a name", (long)g_obj[oa].name,
                         (long)g_obj[ob].name);
                }
            }
        }
    }
    for (i = 0; i < T_FILES; i++) {
        int k;

        if (!g_file[i].f) {
            continue;
        }
        for (k = 0; k < T_GEMS; k++) {
            if (g_file[i].handle[k]) {
                check_pages(g_file[i].handle[k] - 1, &g_file[i], k);
            }
        }
    }
}

static void op_create(void) {
    int fi = pick_file();
    t_file_t *mf = &g_file[fi];
    struct vibeos_drm_mode_create_dumb req;
    uint32_t w, h, bpp, pitch, size, npages;
    int slot = first_slot(mf);
    int oid = first_obj();
    long r;

    shape_of(rnd(), &w, &h, &bpp, &pitch, &size, &npages);
    memset(&req, 0, sizeof(req));
    req.width = w;
    req.height = h;
    req.bpp = bpp;
    r = call(mf->f, VIBEOS_DRM_IOCTL_MODE_CREATE_DUMB, &req);
    if (slot < 0) {
        expect(r, -VIBEOS_ENFILE, "create in a full file");
        expect(req.handle, 0, "a refused create returns no handle");
        return;
    }
    expect(oid >= 0, 1, "a free object slot");
    expect(r, 0, "create a dumb buffer");
    expect(req.handle, slot + 1, "create uses the first free handle");
    expect(req.pitch, pitch, "create pitch");
    expect(req.size, size, "create size");
    g_obj[oid].live = 1;
    g_obj[oid].size = size;
    g_obj[oid].npages = npages;
    g_obj[oid].refs = 1u;
    g_obj[oid].name = 0;
    g_obj[oid].offset = 0;
    memset(g_obj[oid].page, 0, sizeof(g_obj[oid].page));
    mf->handle[slot] = oid + 1;
    {
        struct vibeos_drm_mode_map_dumb map;
        uint32_t p;

        memset(&map, 0, sizeof(map));
        map.handle = (uint32_t)slot + 1u;
        expect(call(mf->f, VIBEOS_DRM_IOCTL_MODE_MAP_DUMB, &map), 0, "map a new dumb buffer");
        g_obj[oid].offset = map.offset;
        remember_offset(map.offset);
        for (p = 0; p < npages; p++) {
            expect(mf->f->ops->share_page(mf->f, map.offset + (uint64_t)p * 4096ull,
                                          &g_obj[oid].page[p]), 0,
                   "share a new page");
            expect(g_obj[oid].page[p] != 0, 1, "a shared page is not null");
            ks_page_unhold(g_obj[oid].page[p]);
        }
    }
}

static void drop_handle(int fi, int slot) {
    t_file_t *mf = &g_file[fi];
    int oid = mf->handle[slot] - 1;

    mf->handle[slot] = 0;
    g_obj[oid].refs--;
    if (g_obj[oid].refs == 0u) {
        g_obj[oid].live = 0;
        g_obj[oid].name = 0;
    }
}

static void op_close(int destroy) {
    int fi = pick_file();
    t_file_t *mf = &g_file[fi];
    int slot = (int)(rnd() % (T_GEMS + 2u));
    int oid = slot < T_GEMS ? mf->handle[slot] - 1 : -1;
    long r;

    if (destroy) {
        struct vibeos_drm_mode_destroy_dumb req;

        memset(&req, 0, sizeof(req));
        req.handle = (uint32_t)slot + 1u;
        r = call(mf->f, VIBEOS_DRM_IOCTL_MODE_DESTROY_DUMB, &req);
    } else {
        struct vibeos_drm_gem_close req;

        memset(&req, 0, sizeof(req));
        req.handle = (uint32_t)slot + 1u;
        r = call(mf->f, VIBEOS_DRM_IOCTL_GEM_CLOSE, &req);
    }
    if (oid < 0) {
        expect(r, -VIBEOS_EINVAL, "close an empty or out-of-range handle");
        return;
    }
    expect(r, 0, "close a live handle");
    drop_handle(fi, slot);
    if (!g_obj[oid].live) {
        void *page = 0;

        expect(mf->f->ops->share_page(mf->f, g_obj[oid].offset, &page), -VIBEOS_EINVAL,
               "a dead object's offset stops resolving");
    }
}

static void op_flink(void) {
    int fi = pick_file();
    t_file_t *mf = &g_file[fi];
    int slot = (int)(rnd() % (T_GEMS + 2u));
    int oid = slot < T_GEMS ? mf->handle[slot] - 1 : -1;
    struct vibeos_drm_gem_flink req;
    long r;

    memset(&req, 0, sizeof(req));
    req.handle = (uint32_t)slot + 1u;
    r = call(mf->f, VIBEOS_DRM_IOCTL_GEM_FLINK, &req);
    if (oid < 0) {
        expect(r, -VIBEOS_ENOENT, "flink an empty or out-of-range handle");
        return;
    }
    if (g_obj[oid].name) {
        expect(r, 0, "flink an already named object");
        expect(req.name, g_obj[oid].name, "flink is idempotent");
        return;
    }
    if (file_named_count(fi) >= T_GEMS) {
        expect(r, -VIBEOS_ENOSPC, "flink with every per-file name taken");
        return;
    }
    expect(r, 0, "flink a live object");
    expect(req.name != 0u, 1, "flink returns a name");
    expect(file_name_used(fi, req.name), 0, "flink returns an unused per-file name");
    g_obj[oid].name = req.name;
    remember_name(req.name);
}

static void op_open(void) {
    int named[T_OBJS];
    int n = 0;
    int i;
    int oid = -1;
    int ofi;
    t_file_t *mf;
    int slot;
    struct vibeos_drm_gem_open req;
    long r;

    for (i = 0; i < T_OBJS; i++) {
        if (g_obj[i].live && g_obj[i].name) {
            named[n++] = i;
        }
    }
    memset(&req, 0, sizeof(req));

    if (n == 0 || (rnd() & 1u)) {
        /* No name, or we exercise rejection. A truly bogus name (and a real
         * name opened from a different file) must both be refused: a GEM name
         * is private to its owning file (review M-082), so it only resolves
         * against the file that minted it. */
        if (n > 0 && (rnd() & 1u)) {
            oid = named[rnd() % (uint32_t)n];
            {
                int owner = owner_of(oid);

                if (owner >= 0) {
                    int wrong = pick_other_file(owner);

                    if (wrong >= 0) {
                        req.name = g_obj[oid].name;
                        r = call(g_file[wrong].f, VIBEOS_DRM_IOCTL_GEM_OPEN, &req);
                        expect(r, -VIBEOS_ENOENT, "open a per-file name from the wrong file");
                        return;
                    }
                }
            }
        }
        req.name = (rnd() & 1u) ? 0u : 0xdead0000u + (rnd() % 64u);
        {
            int fi = pick_file();

            r = call(g_file[fi].f, VIBEOS_DRM_IOCTL_GEM_OPEN, &req);
        }
        expect(r, -VIBEOS_ENOENT, "open an unknown name");
        return;
    }

    oid = named[rnd() % (uint32_t)n];
    ofi = owner_of(oid);
    if (ofi < 0) {
        int fi = pick_file();

        req.name = g_obj[oid].name;
        r = call(g_file[fi].f, VIBEOS_DRM_IOCTL_GEM_OPEN, &req);
        expect(r, -VIBEOS_ENOENT, "open an orphaned name");
        return;
    }
    mf = &g_file[ofi];
    slot = first_slot(mf);
    req.name = g_obj[oid].name;
    r = call(mf->f, VIBEOS_DRM_IOCTL_GEM_OPEN, &req);
    if (slot < 0) {
        expect(r, -VIBEOS_ENFILE, "open into a full file");
        return;
    }
    expect(r, 0, "open a per-file name in its owning file");
    expect(req.handle, slot + 1, "open uses the first free handle");
    expect(req.size, g_obj[oid].size, "open reports the object size");
    g_obj[oid].refs++;
    mf->handle[slot] = oid + 1;
}

static void op_map(void) {
    int fi = pick_file();
    t_file_t *mf = &g_file[fi];
    int slot = (int)(rnd() % (T_GEMS + 2u));
    int oid = slot < T_GEMS ? mf->handle[slot] - 1 : -1;
    struct vibeos_drm_mode_map_dumb req;
    long r;

    memset(&req, 0, sizeof(req));
    req.handle = (uint32_t)slot + 1u;
    r = call(mf->f, VIBEOS_DRM_IOCTL_MODE_MAP_DUMB, &req);
    if (oid < 0) {
        expect(r, -VIBEOS_ENOENT, "map an empty or out-of-range handle");
        return;
    }
    expect(r, 0, "map a live handle");
    expect(req.pad, 0, "map clears pad");
    expect(req.offset, g_obj[oid].offset, "map returns the object offset");
}

static void op_get_cap(void) {
    static const uint64_t cap[] = {
        VIBEOS_DRM_CAP_DUMB_BUFFER,          VIBEOS_DRM_CAP_VBLANK_HIGH_CRTC,
        VIBEOS_DRM_CAP_DUMB_PREFERRED_DEPTH, VIBEOS_DRM_CAP_DUMB_PREFER_SHADOW,
        VIBEOS_DRM_CAP_PRIME,                VIBEOS_DRM_CAP_TIMESTAMP_MONOTONIC,
        VIBEOS_DRM_CAP_ASYNC_PAGE_FLIP,      VIBEOS_DRM_CAP_CURSOR_WIDTH,
        VIBEOS_DRM_CAP_CURSOR_HEIGHT,        VIBEOS_DRM_CAP_ADDFB2_MODIFIERS,
    };
    static const uint64_t value[] = {1u, 0u, 32u, 0u, 0u, 1u, 0u, 0u, 0u, 0u};
    struct vibeos_drm_get_cap req;
    int fi = pick_file();
    uint32_t i;

    if ((rnd() & 3u) == 0u) {
        memset(&req, 0, sizeof(req));
        req.capability = 0xdeadbeefu;
        req.value = 0xa5a5a5a5u;
        expect(call(g_file[fi].f, VIBEOS_DRM_IOCTL_GET_CAP, &req), -VIBEOS_EINVAL,
               "get an unknown capability");
        return;
    }
    i = rnd() % (uint32_t)(sizeof(cap) / sizeof(cap[0]));
    memset(&req, 0, sizeof(req));
    req.capability = cap[i];
    expect(call(g_file[fi].f, VIBEOS_DRM_IOCTL_GET_CAP, &req), 0, "get a known capability");
    expect(req.value, value[i], "capability value");
}

static void op_version(void) {
    struct vibeos_drm_version v;
    char name[16];
    char date[16];
    char desc[32];
    int fi = pick_file();

    memset(&v, 0, sizeof(v));
    memset(name, 0, sizeof(name));
    memset(date, 0, sizeof(date));
    memset(desc, 0, sizeof(desc));
    v.name = name;
    v.name_len = sizeof(name);
    v.date = date;
    v.date_len = sizeof(date);
    v.desc = desc;
    v.desc_len = sizeof(desc);
    expect(call(g_file[fi].f, VIBEOS_DRM_IOCTL_VERSION, &v), 0, "get the version");
    expect(v.version_major, 1, "version major");
    expect(strcmp(name, "vibeos"), 0, "version name");
    expect(strcmp(date, "2026"), 0, "version date");
    expect(strcmp(desc, "VibeOS DRM/KMS"), 0, "version description");
}

static void op_file_open(void) {
    int i;

    for (i = 0; i < T_FILES; i++) {
        if (!g_file[i].f) {
            long err = 0;

            g_file[i].f = vibeos_drm_open(VIBEOS_O_RDWR, &err);
            expect(g_file[i].f != 0, 1, "open a DRM file");
            expect(err, 0, "open error");
            memset(g_file[i].handle, 0, sizeof(g_file[i].handle));
            return;
        }
    }
}

static void op_file_close(void) {
    int fi;
    int i;

    if (open_files() <= 1) {
        return;
    }
    fi = pick_file();
    for (i = 0; i < T_GEMS; i++) {
        if (g_file[fi].handle[i]) {
            drop_handle(fi, i);
        }
    }
    vibeos_file_put(g_file[fi].f);
    g_file[fi].f = 0;
}

static void final_checks(void) {
    vibeos_file_t *fresh;
    long err = 0;
    size_t i;

    for (i = 0; i < T_FILES; i++) {
        if (g_file[i].f) {
            int k;

            for (k = 0; k < T_GEMS; k++) {
                if (g_file[i].handle[k]) {
                    drop_handle((int)i, k);
                }
            }
            vibeos_file_put(g_file[i].f);
            g_file[i].f = 0;
        }
    }
    for (i = 0; i < T_OBJS; i++) {
        if (g_obj[i].live || g_obj[i].refs != 0u) {
            fail("an object outlived every file", (long)g_obj[i].refs, 0);
        }
    }
    fresh = vibeos_drm_open(VIBEOS_O_RDWR, &err);
    expect(fresh != 0, 1, "open a fresh file for the final checks");
    for (i = 0; i < g_nold_offset; i++) {
        void *page = 0;

        expect(fresh->ops->share_page(fresh, g_old_offset[i], &page), -VIBEOS_EINVAL,
               "an old object offset stops resolving");
    }
    for (i = 0; i < g_nold_name; i++) {
        struct vibeos_drm_gem_open req;

        memset(&req, 0, sizeof(req));
        req.name = g_old_name[i];
        expect(call(fresh, VIBEOS_DRM_IOCTL_GEM_OPEN, &req), -VIBEOS_ENOENT,
               "an old name stops resolving");
    }
    vibeos_file_put(fresh);
}

void drm_torture_support_check(void);

int main(int argc, char **argv) {
    long rounds = argc > 2 ? strtol(argv[2], 0, 0) : 2000;
    int i;

    g_seed = argc > 1 ? strtoull(argv[1], 0, 0) : 1u;
    g_rng = g_seed * 0x9e3779b97f4a7c15ull + 1u;
    printf("drm_torture seed=%llu rounds=%ld\n", (unsigned long long)g_seed, rounds);
    for (i = 0; i < 3; i++) {
        long err = 0;

        g_file[i].f = vibeos_drm_open(VIBEOS_O_RDWR, &err);
        if (!g_file[i].f || err != 0) {
            printf("FAIL:drm_torture could not open a DRM file\n");
            return 1;
        }
    }
    for (g_round = 0; g_round < rounds; g_round++) {
        switch (rnd() % 12u) {
            case 0: g_op = "create"; op_create(); break;
            case 1: g_op = "close"; op_close(0); break;
            case 2: g_op = "destroy"; op_close(1); break;
            case 3: g_op = "flink"; op_flink(); break;
            case 4: g_op = "open"; op_open(); break;
            case 5: g_op = "map"; op_map(); break;
            case 6: g_op = "get_cap"; op_get_cap(); break;
            case 7: g_op = "version"; op_version(); break;
            case 8: g_op = "file_open"; op_file_open(); break;
            case 9: g_op = "file_close"; op_file_close(); break;
            case 10: {
                static const uint64_t unknown = 0xdeadbeefu;
                int fi = pick_file();

                g_op = "unknown_ioctl";
                expect(call(g_file[fi].f, unknown, 0), -VIBEOS_ENOTTY, "unknown ioctl");
                break;
            }
            default:
                g_op = "map"; op_map(); break;
        }
        check_all();
    }
    g_op = "final";
    final_checks();
    drm_torture_support_check();
    printf("drm_torture ok seed=%llu rounds=%ld\n", (unsigned long long)g_seed, rounds);
    return 0;
}
