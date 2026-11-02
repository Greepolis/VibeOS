/* The Windows registry's core (docs/abi/windows.md W8 step 2).
 *
 * A tree of keys, each with values of Windows' types, compared without case,
 * held in memory and saved as hive images. Portable and host-tested: the
 * memory it grows into and the lock that serialises it are supplied by
 * whoever starts it (vibeos_reg_init), the NT personality on the machine and
 * plain malloc in the tests. It knows nothing of handles, access rights or
 * files - the NT services above it (kernel/abi/nt/) do.
 *
 * Names are UTF-16 code units, as Windows keeps them, with a length rather
 * than a terminator: a name may hold any unit but the separator a path uses.
 * Every call answers an NTSTATUS, because that is what its only caller
 * returns. */
#ifndef VIBEOS_NTREG_H
#define VIBEOS_NTREG_H

#include <stdint.h>

/* Windows' limits (MSDN, "Registry element size limits"): a key name of 255
 * characters, a path of 32,767, a value name of 16,383, and a key 512 levels
 * deep. Value data has no hard limit on Windows; here it is a megabyte, and
 * the whole registry has a quota (vibeos_reg_env_t.quota). */
#define VIBEOS_REG_KEY_NAME_MAX   255u
#define VIBEOS_REG_VALUE_NAME_MAX 16383u
#define VIBEOS_REG_DEPTH_MAX      512u
#define VIBEOS_REG_DATA_MAX       (1024u * 1024u)
#define VIBEOS_REG_KEYS_MAX       8192u    /* slots are static: 8,192 x 80 bytes */

/* The value types (winnt.h); any other number is kept as given, as Windows
 * keeps it. */
#define VIBEOS_REG_NONE      0u
#define VIBEOS_REG_SZ        1u
#define VIBEOS_REG_EXPAND_SZ 2u
#define VIBEOS_REG_BINARY    3u
#define VIBEOS_REG_DWORD     4u
#define VIBEOS_REG_MULTI_SZ  7u
#define VIBEOS_REG_QWORD     11u

/* What the registry grows into and how it is serialised. */
typedef struct {
    void *(*page_alloc)(void);        /* 4 KiB, or null when there is none */
    void (*page_free)(void *page);
    void (*lock)(void);
    void (*unlock)(void);
    uint64_t (*now)(void);            /* the time a key was last written, FILETIME */
    uint64_t quota;                   /* bytes the registry may hold; 0: no limit */
} vibeos_reg_env_t;

/* A key, as a caller holds it: an index and the tenancy it had when taken. A
 * key that is deleted stays referable for as long as somebody holds it -
 * every call on it answers STATUS_KEY_DELETED - and its slot goes back when
 * the last reference does. */
typedef struct {
    uint32_t idx;
    uint32_t gen;
} vibeos_reg_key_t;

typedef struct {
    uint32_t subkeys, values;
    uint32_t max_subkey_name, max_value_name, max_value_data;   /* in bytes */
    uint64_t last_write;
    uint32_t name_len;                /* this key's own name, in bytes */
} vibeos_reg_info_t;

#define VIBEOS_REG_ROOT    0u   /* \Registry                               */
/* What a create did, as REG_CREATED_NEW_KEY and REG_OPENED_EXISTING_KEY. */
#define VIBEOS_REG_CREATED 1u
#define VIBEOS_REG_OPENED  2u
/* A key that is not saved: it and everything under it go at a reboot. */
#define VIBEOS_REG_VOLATILE 0x1u

void vibeos_reg_init(const vibeos_reg_env_t *env);
/* Everything given back to the environment; init may follow again. Every
 * key held by a caller is invalid afterwards. */
void vibeos_reg_fini(void);
/* \Registry itself, referenced; every path is opened from a key. */
vibeos_reg_key_t vibeos_reg_root(void);

/* Open the key at `path` (components separated by '\\', `n` units) under
 * `from`, which may be the key itself for an empty path. The reference
 * returned is the caller's. */
uint32_t vibeos_reg_open(vibeos_reg_key_t from, const uint16_t *path, uint32_t n,
                         vibeos_reg_key_t *out);
/* Open it, or make it - and every missing key is not made: only the last
 * component may be new, as NtCreateKey has it. A volatile parent takes only
 * volatile children (STATUS_CHILD_MUST_BE_VOLATILE). */
uint32_t vibeos_reg_create(vibeos_reg_key_t from, const uint16_t *path, uint32_t n,
                           uint32_t options, vibeos_reg_key_t *out, uint32_t *disposition);
void vibeos_reg_ref(vibeos_reg_key_t k);
void vibeos_reg_unref(vibeos_reg_key_t k);
/* A key with subkeys is not deleted (STATUS_CANNOT_DELETE); \Registry and its
 * two children never are. */
uint32_t vibeos_reg_delete(vibeos_reg_key_t k);
uint32_t vibeos_reg_rename(vibeos_reg_key_t k, const uint16_t *name, uint32_t n);
uint32_t vibeos_reg_info(vibeos_reg_key_t k, vibeos_reg_info_t *out);
/* The key's own name into `buf` (`cap` bytes); *len is its size, written
 * whole or STATUS_BUFFER_OVERFLOW with as much as fits. */
uint32_t vibeos_reg_name(vibeos_reg_key_t k, uint16_t *buf, uint32_t cap, uint32_t *len);
/* The index-th subkey's name, in the order Windows enumerates: by name. */
uint32_t vibeos_reg_enum_key(vibeos_reg_key_t k, uint32_t index, uint16_t *buf, uint32_t cap,
                             uint32_t *len, uint64_t *last_write);

/* Values. An empty name is the key's default value. */
uint32_t vibeos_reg_set(vibeos_reg_key_t k, const uint16_t *name, uint32_t n, uint32_t type,
                        const void *data, uint32_t len);
/* The value's type and size, and as much of its data as `cap` holds:
 * STATUS_BUFFER_OVERFLOW when it does not all fit, with *len its full size. */
uint32_t vibeos_reg_get(vibeos_reg_key_t k, const uint16_t *name, uint32_t n, uint32_t *type,
                        void *data, uint32_t cap, uint32_t *len);
uint32_t vibeos_reg_unset(vibeos_reg_key_t k, const uint16_t *name, uint32_t n);
/* The index-th value, in the order it was set: its name and, if `data`, its
 * data, each written whole or reported as overflowing. */
uint32_t vibeos_reg_enum_value(vibeos_reg_key_t k, uint32_t index, uint16_t *name, uint32_t name_cap,
                               uint32_t *name_len, uint32_t *type, void *data, uint32_t cap,
                               uint32_t *len);

/* A hive: the key and everything under it that is not volatile, as one byte
 * image - a header with a magic, a version, the payload's length and a
 * checksum, then the keys depth first. `write` is called with consecutive
 * pieces and answers 0, or -1 to abandon the save. */
uint32_t vibeos_reg_save(vibeos_reg_key_t k, int (*write)(void *ctx, const void *buf, uint32_t len),
                         void *ctx);
/* Replace what is under `k` with the hive in `img`, all of it or none: an
 * image that does not check - magic, version, lengths, checksum, limits - is
 * STATUS_REGISTRY_CORRUPT and nothing is changed. Volatile subkeys of `k`
 * are kept. */
uint32_t vibeos_reg_load(vibeos_reg_key_t k, const void *img, uint64_t len);

/* Whether anything saved under the key has changed since `mark`, a number the
 * registry counts up on every change that a save would write. */
uint64_t vibeos_reg_changes(void);
/* Bytes held, for the quota and the tests. */
uint64_t vibeos_reg_bytes(void);

/* Windows' upcase of one UTF-16 unit, as far as the registry folds names:
 * ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic. */
uint16_t vibeos_reg_upcase(uint16_t c);

#endif
