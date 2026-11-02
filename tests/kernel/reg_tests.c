/* The registry's core (kernel/abi/nt/registry/reg.c, docs/abi/windows.md W8
 * step 2), on pages from malloc and a lock that counts: keys and paths,
 * names without case, values of every size class, deletion while a key is
 * held, the quota, and hives - saved, loaded back, and refused when a byte of
 * them is wrong. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/ntreg.h"
#include "vibeos/nt_layout.h"

int test_reg(void);

static int g_fail;
static int g_locked;
static int g_pages;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:reg %s\n", what);
        g_fail = 1;
    }
}

static void *t_page_alloc(void) {
    void *p = malloc(4096);

    if (p) {
        memset(p, 0xE7, 4096);   /* a hostile background: nothing may rely on zeros */
        g_pages++;
    }
    return p;
}
static void t_page_free(void *p) {
    g_pages--;
    free(p);
}
static void t_lock(void) {
    if (g_locked) {
        printf("FAIL:reg the registry took its lock twice\n");
        g_fail = 1;
    }
    g_locked = 1;
}
static void t_unlock(void) { g_locked = 0; }
static uint64_t g_clock = 1000;
static uint64_t t_now(void) { return ++g_clock; }

static void start(uint64_t quota) {
    vibeos_reg_env_t env = { t_page_alloc, t_page_free, t_lock, t_unlock, t_now, quota };

    vibeos_reg_fini();   /* the last test's registry, given back whole */
    vibeos_reg_init(&env);
}

/* An ASCII string as UTF-16 units, into a static buffer. */
static const uint16_t *w(const char *s, uint32_t *n) {
    static uint16_t buf[8][600];
    static int at;
    uint16_t *b = buf[at++ & 7];
    uint32_t i;

    for (i = 0; s[i]; i++) {
        b[i] = (uint16_t)(unsigned char)s[i];
    }
    *n = i;
    return b;
}

static uint32_t open_(vibeos_reg_key_t from, const char *path, vibeos_reg_key_t *k) {
    uint32_t n;
    const uint16_t *p = w(path, &n);

    return vibeos_reg_open(from, p, n, k);
}

static uint32_t create_(vibeos_reg_key_t from, const char *path, uint32_t opts, vibeos_reg_key_t *k,
                        uint32_t *disp) {
    uint32_t n;
    const uint16_t *p = w(path, &n);

    return vibeos_reg_create(from, p, n, opts, k, disp);
}

static uint32_t set_(vibeos_reg_key_t k, const char *name, uint32_t type, const void *d, uint32_t len) {
    uint32_t n;
    const uint16_t *p = w(name, &n);

    return vibeos_reg_set(k, p, n, type, d, len);
}

static uint32_t get_(vibeos_reg_key_t k, const char *name, uint32_t *type, void *d, uint32_t cap,
                     uint32_t *len) {
    uint32_t n;
    const uint16_t *p = w(name, &n);

    return vibeos_reg_get(k, p, n, type, d, cap, len);
}

static int name_is(const uint16_t *b, uint32_t len, const char *s) {
    uint32_t i;

    if (len != strlen(s) * 2u) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        if (b[i] != (uint16_t)(unsigned char)s[i]) {
            return 0;
        }
    }
    return 1;
}

static void t_keys(void) {
    vibeos_reg_key_t root, m, sw, a, b, again;
    uint32_t disp = 0, len = 0;
    uint16_t buf[64];
    vibeos_reg_info_t info;

    start(0);
    root = vibeos_reg_root();
    expect(open_(root, "Machine", &m) == VIBEOS_STATUS_SUCCESS &&
               open_(root, "uSeR", &a) == VIBEOS_STATUS_SUCCESS,
           "\\Registry has Machine and User, found without case");
    vibeos_reg_unref(a);
    expect(create_(root, "Third", 0, &a, &disp) == VIBEOS_STATUS_ACCESS_DENIED,
           "and nothing else may be made beside them");
    expect(create_(m, "Software", 0, &sw, &disp) == VIBEOS_STATUS_SUCCESS && disp == VIBEOS_REG_CREATED,
           "a key made");
    expect(create_(m, "SOFTWARE", 0, &again, &disp) == VIBEOS_STATUS_SUCCESS && disp == VIBEOS_REG_OPENED &&
               again.idx == sw.idx,
           "made again in another case, it is opened");
    vibeos_reg_unref(again);
    expect(create_(m, "Software\\Vendor\\App", 0, &a, &disp) == VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND,
           "only the last component may be new");
    expect(create_(sw, "Vendor", 0, &a, &disp) == VIBEOS_STATUS_SUCCESS &&
               create_(m, "software\\vendor\\App", 0, &b, &disp) == VIBEOS_STATUS_SUCCESS &&
               disp == VIBEOS_REG_CREATED,
           "a path walks existing keys and makes its last");
    expect(open_(root, "Machine\\Software\\Vendor\\APP", &again) == VIBEOS_STATUS_SUCCESS &&
               again.idx == b.idx,
           "and opens from the root by the whole path");
    vibeos_reg_unref(again);
    expect(open_(m, "Software\\\\Vendor", &again) == VIBEOS_STATUS_OBJECT_NAME_INVALID &&
               open_(m, "Software\\", &again) == VIBEOS_STATUS_OBJECT_NAME_INVALID,
           "an empty component, or a trailing separator, is an invalid name");
    {
        char longname[300];

        memset(longname, 'x', 256);
        longname[256] = 0;
        expect(create_(m, longname, 0, &again, &disp) == VIBEOS_STATUS_OBJECT_NAME_INVALID,
               "a component of 256 characters is too long");
        longname[255] = 0;
        expect(create_(m, longname, 0, &again, &disp) == VIBEOS_STATUS_SUCCESS,
               "and 255 is not");
        vibeos_reg_unref(again);
    }
    expect(vibeos_reg_path(a, buf, sizeof(buf), &len) == 0 &&
               name_is(buf, len, "\\REGISTRY\\MACHINE\\Software\\Vendor") &&
               vibeos_reg_path(a, buf, 8, &len) == VIBEOS_STATUS_BUFFER_OVERFLOW && len == 66u,
           "a key's whole path from the top, and its size when it does not fit");
    expect(vibeos_reg_delete(a) == VIBEOS_STATUS_CANNOT_DELETE,
           "a key with a subkey is not deleted");
    expect(vibeos_reg_delete(m) == VIBEOS_STATUS_ACCESS_DENIED, "nor is Machine");
    expect(vibeos_reg_delete(b) == VIBEOS_STATUS_SUCCESS, "a leaf is");
    expect(set_(b, "x", VIBEOS_REG_DWORD, "abcd", 4) == VIBEOS_STATUS_KEY_DELETED &&
               vibeos_reg_info(b, &info) == VIBEOS_STATUS_KEY_DELETED,
           "a deleted key held by its handle answers STATUS_KEY_DELETED");
    expect(open_(a, "App", &again) == VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND, "and is not found again");
    vibeos_reg_unref(b);
    expect(set_(b, "x", VIBEOS_REG_DWORD, "abcd", 4) == VIBEOS_STATUS_INVALID_HANDLE,
           "once its last reference goes, the old handle names nothing");
    expect(create_(a, "Zeta", 0, &b, &disp) == 0 && create_(a, "alpha", 0, &again, &disp) == 0 &&
               vibeos_reg_enum_key(a, 0, buf, sizeof(buf), &len, 0) == 0 && name_is(buf, len, "alpha") &&
               vibeos_reg_enum_key(a, 1, buf, sizeof(buf), &len, 0) == 0 && name_is(buf, len, "Zeta") &&
               vibeos_reg_enum_key(a, 2, buf, sizeof(buf), &len, 0) == VIBEOS_STATUS_NO_MORE_ENTRIES,
           "subkeys enumerate by name, without case");
    expect(vibeos_reg_enum_key(a, 1, buf, 4, &len, 0) == VIBEOS_STATUS_BUFFER_OVERFLOW && len == 8u,
           "a short buffer overflows with the size it needs");
    {
        uint32_t n;
        const uint16_t *nm = w("ALPHA", &n);

        expect(vibeos_reg_rename(b, nm, n) == VIBEOS_STATUS_OBJECT_NAME_COLLISION,
               "a rename onto a sibling's name collides");
        nm = w("Beta", &n);
        expect(vibeos_reg_rename(b, nm, n) == VIBEOS_STATUS_SUCCESS &&
                   vibeos_reg_enum_key(a, 1, buf, sizeof(buf), &len, 0) == 0 && name_is(buf, len, "Beta"),
               "a rename takes its new place in the order");
    }
    expect(vibeos_reg_info(a, &info) == 0 && info.subkeys == 2u && info.max_subkey_name == 10u,
           "a key's information counts its subkeys and their longest name");
    {
        vibeos_reg_key_t v, c;

        expect(create_(m, "Vol", VIBEOS_REG_VOLATILE, &v, &disp) == 0 &&
                   create_(v, "Child", 0, &c, &disp) == VIBEOS_STATUS_CHILD_MUST_BE_VOLATILE,
               "a volatile key takes only volatile children");
        vibeos_reg_unref(v);
    }
}

static void t_values(void) {
    vibeos_reg_key_t root, m, k;
    uint32_t disp, type = 0, len = 0, i;
    static uint8_t big[300000], back[300000];
    uint8_t small[8];
    uint16_t name[64];

    start(0);
    root = vibeos_reg_root();
    (void)open_(root, "Machine", &m);
    (void)create_(m, "Values", 0, &k, &disp);
    expect(set_(k, "", VIBEOS_REG_SZ, "d\0e\0f\0\0\0", 8) == 0 &&
               get_(k, "", &type, small, sizeof(small), &len) == 0 && type == VIBEOS_REG_SZ && len == 8u &&
               memcmp(small, "d\0e\0f\0\0\0", 8) == 0,
           "the default value, by the empty name");
    expect(set_(k, "Count", VIBEOS_REG_DWORD, "\x2a\0\0\0", 4) == 0 &&
               get_(k, "COUNT", &type, small, sizeof(small), &len) == 0 && type == VIBEOS_REG_DWORD &&
               small[0] == 42u,
           "a DWORD set and read back without case");
    expect(get_(k, "Count", &type, small, 2, &len) == VIBEOS_STATUS_BUFFER_OVERFLOW && len == 4u,
           "a short buffer overflows with the size it needs");
    expect(get_(k, "Missing", &type, small, sizeof(small), &len) == VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND,
           "a value nobody set is not found");
    for (i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 7u + 3u);
    }
    /* Every size class and the page chain, both sides of each boundary. */
    {
        static const uint32_t sizes[] = { 0, 1, 31, 32, 33, 2047, 2048, 2049, 4088, 4089, 8177, 300000 };

        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            memset(back, 0, sizeof(back));
            if (set_(k, "Blob", VIBEOS_REG_BINARY, big, sizes[i]) != 0 ||
                get_(k, "Blob", &type, back, sizeof(back), &len) != 0 || len != sizes[i] ||
                memcmp(back, big, sizes[i]) != 0) {
                printf("  size %u\n", sizes[i]);
                expect(0, "a value of every size class comes back as it was set");
            }
        }
    }
    expect(set_(k, "Huge", VIBEOS_REG_BINARY, big, VIBEOS_REG_DATA_MAX + 1u) ==
               VIBEOS_STATUS_INSUFFICIENT_RESOURCES,
           "a value past a megabyte is refused");
    expect(vibeos_reg_enum_value(k, 0, name, sizeof(name), &len, &type, 0, 0, &i) == 0 && len == 0u &&
               vibeos_reg_enum_value(k, 1, name, sizeof(name), &len, &type, 0, 0, &i) == 0 &&
               name_is(name, len, "Count") &&
               vibeos_reg_enum_value(k, 2, name, sizeof(name), &len, &type, 0, 0, &i) == 0 &&
               name_is(name, len, "Blob") &&
               vibeos_reg_enum_value(k, 3, name, sizeof(name), &len, &type, 0, 0, &i) ==
                   VIBEOS_STATUS_NO_MORE_ENTRIES,
           "values enumerate in the order they were set");
    {
        uint32_t n;
        const uint16_t *nm = w("count", &n);

        expect(vibeos_reg_unset(k, nm, n) == 0 &&
                   get_(k, "Count", &type, small, sizeof(small), &len) == VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND &&
                   vibeos_reg_enum_value(k, 1, name, sizeof(name), &len, &type, 0, 0, &i) == 0 &&
                   name_is(name, len, "Blob"),
               "a value unset goes, and the order closes over it");
        expect(set_(k, "Tail", VIBEOS_REG_DWORD, "\1\0\0\0", 4) == 0 &&
                   vibeos_reg_enum_value(k, 2, name, sizeof(name), &len, &type, 0, 0, &i) == 0 &&
                   name_is(name, len, "Tail"),
               "and the next value set goes on the end");
    }
    {
        /* A value name 16,383 units long, and one more. */
        static uint16_t lname[VIBEOS_REG_VALUE_NAME_MAX + 1u];
        static uint16_t lback[VIBEOS_REG_VALUE_NAME_MAX + 1u];

        for (i = 0; i <= VIBEOS_REG_VALUE_NAME_MAX; i++) {
            lname[i] = (uint16_t)('a' + i % 26u);
        }
        expect(vibeos_reg_set(k, lname, VIBEOS_REG_VALUE_NAME_MAX + 1u, VIBEOS_REG_DWORD, "abcd", 4) ==
                   VIBEOS_STATUS_INVALID_PARAMETER,
               "a value name of 16,384 units is refused");
        expect(vibeos_reg_set(k, lname, VIBEOS_REG_VALUE_NAME_MAX, VIBEOS_REG_DWORD, "abcd", 4) == 0 &&
                   vibeos_reg_enum_value(k, 3, lback, sizeof(lback), &len, &type, 0, 0, &i) == 0 &&
                   len == VIBEOS_REG_VALUE_NAME_MAX * 2u &&
                   memcmp(lback, lname, len) == 0,
               "and one of 16,383 is kept whole across the page chain");
    }
    expect(vibeos_reg_delete(k) == 0, "a key with values is deleted");
    vibeos_reg_unref(k);
    vibeos_reg_unref(m);
    vibeos_reg_unref(root);
}

static void t_quota(void) {
    vibeos_reg_key_t root, m, k;
    uint32_t disp, i, st = 0;
    static uint8_t data[4000];
    char name[16];
    uint64_t before;

    start(64u * 4096u);
    root = vibeos_reg_root();
    (void)open_(root, "Machine", &m);
    (void)create_(m, "Quota", 0, &k, &disp);
    for (i = 0; i < 200u; i++) {
        snprintf(name, sizeof(name), "v%u", i);
        st = set_(k, name, VIBEOS_REG_BINARY, data, sizeof(data));
        if (st != 0) {
            break;
        }
    }
    expect(st == VIBEOS_STATUS_QUOTA_EXCEEDED && i < 200u, "values past the quota are refused");
    expect(vibeos_reg_bytes() <= 64u * 4096u, "and the registry never holds more than its quota");
    {
        /* A value set over an existing one and refused leaves the old one. */
        static uint8_t huge[300000];
        uint8_t back[4000];
        uint32_t type = 0, len = 0;

        data[0] = 0x5A;
        expect(set_(k, "v0", VIBEOS_REG_SZ, huge, sizeof(huge)) == VIBEOS_STATUS_QUOTA_EXCEEDED &&
                   get_(k, "v0", &type, back, sizeof(back), &len) == 0 && len == sizeof(data) &&
                   type == VIBEOS_REG_BINARY,
               "a value refused over an existing one leaves that one as it was");
    }
    before = vibeos_reg_bytes();
    expect(vibeos_reg_delete(k) == 0, "a key holding them deleted");
    vibeos_reg_unref(k);
    expect(vibeos_reg_bytes() < before, "gives back what its large values held");
    (void)create_(m, "Again", 0, &k, &disp);
    expect(set_(k, "v", VIBEOS_REG_BINARY, data, sizeof(data)) == 0, "and room is there again");
}

static int g_img_fail;
static uint8_t g_img[1 << 20];
static uint32_t g_img_len;

static int sink(void *ctx, const void *buf, uint32_t len) {
    (void)ctx;
    if (g_img_fail || g_img_len + len > sizeof(g_img)) {
        return -1;
    }
    memcpy(g_img + g_img_len, buf, len);
    g_img_len += len;
    return 0;
}

static void t_hive(void) {
    vibeos_reg_key_t root, m, a, b, v, x;
    uint32_t disp, type, len;
    uint8_t buf[16];
    uint64_t changes;
    static uint8_t copy[1 << 20];
    uint32_t copy_len;

    start(0);
    root = vibeos_reg_root();
    (void)open_(root, "Machine", &m);
    (void)create_(m, "Software", 0, &a, &disp);
    (void)create_(a, "Deep", 0, &b, &disp);
    (void)set_(b, "Answer", VIBEOS_REG_DWORD, "\x2a\0\0\0", 4);
    (void)set_(m, "Top", VIBEOS_REG_SZ, "t\0\0\0", 4);
    {
        /* Big enough to need a chain of two pages when loaded, which the
         * small-quota load below cannot have. */
        static uint8_t big[5000];

        (void)set_(b, "Big", VIBEOS_REG_BINARY, big, sizeof(big));
    }
    (void)create_(m, "Volatile", VIBEOS_REG_VOLATILE, &v, &disp);
    (void)set_(v, "Gone", VIBEOS_REG_DWORD, "\1\0\0\0", 4);
    g_img_len = 0;
    expect(vibeos_reg_save(m, sink, 0) == 0 && g_img_len > 32u, "a hive is saved");
    memcpy(copy, g_img, g_img_len);
    copy_len = g_img_len;   /* g_img_len is the next save's, and the failing one below resets it */

    /* Changed after the save: the load must put it back as it was. */
    (void)set_(b, "Answer", VIBEOS_REG_DWORD, "\x07\0\0\0", 4);
    (void)create_(a, "Later", 0, &x, &disp);
    changes = vibeos_reg_changes();
    expect(changes > 0u, "a change is counted for the next save");

    /* A byte wrong anywhere refuses the whole image and changes nothing. */
    {
        uint32_t at;
        int all = 1;

        for (at = 0; at < g_img_len; at += at < 32u ? 1u : 7u) {   /* every header byte */
            memcpy(g_img, copy, g_img_len);
            g_img[at] ^= 0x41u;
            if (vibeos_reg_load(m, g_img, g_img_len) != VIBEOS_STATUS_REGISTRY_CORRUPT) {
                printf("  byte %u accepted\n", at);
                all = 0;
            }
        }
        expect(all, "an image with any byte changed is refused as corrupt");
        expect(vibeos_reg_load(m, copy, g_img_len - 1u) == VIBEOS_STATUS_REGISTRY_CORRUPT,
               "and so is one cut short");
        expect(get_(b, "Answer", &type, buf, sizeof(buf), &len) == 0 && buf[0] == 7u,
               "a refused load left the tree as it was");
    }
    expect(vibeos_reg_load(m, copy, g_img_len) == 0, "the saved image loads");
    expect(get_(b, "Answer", &type, buf, sizeof(buf), &len) == VIBEOS_STATUS_KEY_DELETED,
           "a key held across a load is the old one, deleted");
    vibeos_reg_unref(b);
    expect(open_(m, "software\\deep", &b) == 0 && get_(b, "Answer", &type, buf, sizeof(buf), &len) == 0 &&
               buf[0] == 42u && type == VIBEOS_REG_DWORD,
           "and the new one has the saved value");
    expect(open_(m, "Software\\Later", &x) == VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND,
           "a key made after the save is gone");
    expect(get_(m, "Top", &type, buf, sizeof(buf), &len) == 0 && buf[0] == 't',
           "the saved key's own values came back");
    expect(open_(m, "Volatile", &x) == 0 && get_(x, "Gone", &type, buf, sizeof(buf), &len) == 0,
           "a volatile key was not saved and was kept through the load");
    g_img_len = 0;
    g_img_fail = 1;
    expect(vibeos_reg_save(m, sink, 0) == VIBEOS_STATUS_UNSUCCESSFUL, "a save whose writer fails says so");
    g_img_fail = 0;
    {
        /* Out of room half way through a load: refused, and the tree as it
         * was. A fresh registry with a quota of three pages holds the roots'
         * names and one value, and not the hive. */
        start(3u * 4096u);
        root = vibeos_reg_root();
        (void)open_(root, "Machine", &m);
        (void)set_(m, "Kept", VIBEOS_REG_DWORD, "\x09\0\0\0", 4);
        expect(vibeos_reg_load(m, copy, copy_len) != 0, "a load that runs out of room is refused");
        expect(get_(m, "Kept", &type, buf, sizeof(buf), &len) == 0 && buf[0] == 9u &&
                   open_(m, "Software", &x) == VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND,
               "and the key keeps what it had, with nothing of the image");
        expect(vibeos_reg_bytes() <= 3u * 4096u, "within its quota throughout");
    }
}

int test_reg(void) {
    g_fail = 0;
    t_keys();
    t_values();
    t_quota();
    t_hive();
    expect(!g_locked, "the lock is free after every call");
    vibeos_reg_fini();
    expect(g_pages == 0, "and fini gives back every page the registry was given");
    return g_fail;
}
