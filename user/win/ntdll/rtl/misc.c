/* Memory comparison, CRC-32 and the two random number generators
 * (docs/abi/windows.md W2). */

#include "rtl_internal.h"

/* Number of leading bytes of Source that repeat Pattern, in whole ULONGs. A
 * Length that is not a multiple of four is rounded down: the last partial word
 * is never looked at. */
size_t RtlCompareMemoryUlong(const void *src, size_t length, uint32_t pattern) {
    const uint8_t *p = (const uint8_t *)src;
    size_t i, words = length / 4u;

    for (i = 0; i < words; i++) {
        uint32_t w;

        memcpy(&w, p + i * 4u, sizeof(w));
        if (w != pattern) {
            break;
        }
    }
    return i * 4u;
}

/* CRC-32 as zlib's crc32 computes it: the reflected polynomial 0xEDB88320, the
 * register preset to all ones and the result inverted, so that the CRC of a
 * buffer continues from the CRC of its first part. Bit by bit, because a table
 * is 1 KiB of .data that this file would have to initialise or ship. */
uint32_t RtlComputeCrc32(uint32_t initial, const uint8_t *buf, int32_t length) {
    uint32_t crc = ~initial;
    int32_t i;
    int k;

    for (i = 0; i < length; i++) {
        crc ^= buf[i];
        for (k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

/* The multiplicative congruential generator Microsoft documents for RtlUniform:
 * seed' = (seed * 0x7FFFFFED + 0x7FFFFFC3) mod 0x7FFFFFFF, computed in 64 bits so
 * that the product does not wrap. The seed is replaced and returned. The result
 * is below 0x7FFFFFFF. */
uint32_t RtlUniform(uint32_t *seed) {
    uint64_t v = (uint64_t)*seed * 0x7FFFFFEDull + 0x7FFFFFC3ull;

    *seed = (uint32_t)(v % 0x7FFFFFFFull);
    return *seed;
}

/* RtlRandom improves RtlUniform's low-order randomness by shuffling its output
 * through a table of 128 earlier values (Knuth's algorithm B): the previous
 * result picks the slot, the slot's value is returned and the slot is refilled
 * from RtlUniform. The table is filled from the first seed a process passes, and
 * is process-wide state, as Windows' is.
 *
 * Whether Windows' table is filled the same way is not something this was checked
 * against; the shape is the documented one. */
static int g_rand_ready;
static uint32_t g_rand_last;
static uint32_t g_rand_table[128];

void vibeos_rtl_random_reset(void) {
    g_rand_ready = 0;
}

uint32_t RtlRandom(uint32_t *seed) {
    uint32_t pos;

    if (!g_rand_ready) {
        int i;

        for (i = 0; i < 128; i++) {
            g_rand_table[i] = RtlUniform(seed);
        }
        g_rand_last = RtlUniform(seed);
        g_rand_ready = 1;
    }
    pos = (uint32_t)(((uint64_t)g_rand_last * 128u) / 0x7FFFFFFFull);
    g_rand_last = g_rand_table[pos];
    g_rand_table[pos] = RtlUniform(seed);
    return g_rand_last;
}

/* ---- NTSTATUS to Win32 error ---------------------------------------------------------
 * What kernel32 sets as the last error after a service fails. The pairs are
 * the ones Wine's conformance test (ntdll/tests/error.c) checks on Windows, for
 * the statuses this kernel answers; one not listed is ERROR_MR_MID_NOT_FOUND,
 * which is what Windows says of a status nobody mapped. */
static const uint32_t g_dos_errors[][2] = {
    { 0x00000000u, 0u },      /* SUCCESS                 */
    { 0x00000103u, 997u },    /* PENDING: ERROR_IO_PENDING */
    { 0x40000000u, 698u },    /* OBJECT_NAME_EXISTS: ERROR_OBJECT_NAME_EXISTS */
    { 0x80000005u, 234u },    /* BUFFER_OVERFLOW: ERROR_MORE_DATA */
    { 0xC0000001u, 31u },     /* UNSUCCESSFUL: ERROR_GEN_FAILURE */
    { 0xC0000002u, 1u },      /* NOT_IMPLEMENTED: ERROR_INVALID_FUNCTION */
    { 0xC0000003u, 87u },     /* INVALID_INFO_CLASS: ERROR_INVALID_PARAMETER */
    { 0xC0000004u, 24u },     /* INFO_LENGTH_MISMATCH: ERROR_BAD_LENGTH */
    { 0xC0000005u, 998u },    /* ACCESS_VIOLATION: ERROR_NOACCESS */
    { 0xC0000008u, 6u },      /* INVALID_HANDLE */
    { 0xC000000Du, 87u },     /* INVALID_PARAMETER */
    { 0xC000000Fu, 2u },      /* NO_SUCH_FILE: ERROR_FILE_NOT_FOUND */
    { 0xC0000011u, 38u },     /* END_OF_FILE: ERROR_HANDLE_EOF */
    { 0xC0000017u, 8u },      /* NO_MEMORY: ERROR_NOT_ENOUGH_MEMORY */
    { 0xC0000018u, 487u },    /* CONFLICTING_ADDRESSES: ERROR_INVALID_ADDRESS */
    { 0xC0000019u, 487u },    /* NOT_MAPPED_VIEW: ERROR_INVALID_ADDRESS */
    { 0xC000001Au, 87u },     /* UNABLE_TO_FREE_VM: ERROR_INVALID_PARAMETER */
    { 0xC000001Bu, 87u },     /* UNABLE_TO_DELETE_SECTION: ERROR_INVALID_PARAMETER */
    { 0xC000001Fu, 5u },      /* INVALID_VIEW_SIZE: ERROR_ACCESS_DENIED */
    { 0xC0000020u, 193u },    /* INVALID_FILE_FOR_SECTION: ERROR_BAD_EXE_FORMAT */
    { 0xC0000022u, 5u },      /* ACCESS_DENIED */
    { 0xC0000023u, 122u },    /* BUFFER_TOO_SMALL: ERROR_INSUFFICIENT_BUFFER */
    { 0xC0000024u, 6u },      /* OBJECT_TYPE_MISMATCH: ERROR_INVALID_HANDLE */
    { 0xC000002Du, 487u },    /* NOT_COMMITTED: ERROR_INVALID_ADDRESS */
    { 0xC0000030u, 87u },     /* INVALID_PARAMETER_MIX */
    { 0xC0000033u, 123u },    /* OBJECT_NAME_INVALID: ERROR_INVALID_NAME */
    { 0xC0000034u, 2u },      /* OBJECT_NAME_NOT_FOUND: ERROR_FILE_NOT_FOUND */
    { 0xC0000035u, 183u },    /* OBJECT_NAME_COLLISION: ERROR_ALREADY_EXISTS */
    { 0xC000003Au, 3u },      /* OBJECT_PATH_NOT_FOUND: ERROR_PATH_NOT_FOUND */
    { 0xC000003Bu, 161u },    /* OBJECT_PATH_SYNTAX_BAD: ERROR_BAD_PATHNAME */
    { 0xC0000040u, 8u },      /* SECTION_TOO_BIG: ERROR_NOT_ENOUGH_MEMORY */
    { 0xC0000043u, 32u },     /* SHARING_VIOLATION */
    { 0xC0000044u, 1816u },   /* QUOTA_EXCEEDED: ERROR_NOT_ENOUGH_QUOTA */
    { 0xC0000045u, 87u },     /* INVALID_PAGE_PROTECTION: ERROR_INVALID_PARAMETER */
    { 0xC0000046u, 288u },    /* MUTANT_NOT_OWNED: ERROR_NOT_OWNER */
    { 0xC0000047u, 298u },    /* SEMAPHORE_LIMIT_EXCEEDED: ERROR_TOO_MANY_POSTS */
    { 0xC0000049u, 87u },     /* SECTION_NOT_IMAGE: ERROR_INVALID_PARAMETER */
    { 0xC000004Eu, 87u },     /* SECTION_PROTECTION: ERROR_INVALID_PARAMETER */
    { 0xC000007Au, 127u },    /* PROCEDURE_NOT_FOUND: ERROR_PROC_NOT_FOUND */
    { 0xC000007Bu, 193u },    /* INVALID_IMAGE_FORMAT: ERROR_BAD_EXE_FORMAT */
    { 0xC000009Au, 1450u },   /* INSUFFICIENT_RESOURCES: ERROR_NO_SYSTEM_RESOURCES */
    { 0xC000009Fu, 487u },    /* FREE_VM_NOT_AT_BASE: ERROR_INVALID_ADDRESS */
    { 0xC00000A0u, 487u },    /* MEMORY_NOT_ALLOCATED: ERROR_INVALID_ADDRESS */
    { 0xC00000BAu, 5u },      /* FILE_IS_A_DIRECTORY: ERROR_ACCESS_DENIED */
    { 0xC00000BBu, 50u },     /* NOT_SUPPORTED */
    { 0xC00000F0u, 87u },     /* INVALID_PARAMETER_2 */
    { 0xC00000F1u, 87u },     /* INVALID_PARAMETER_3 */
    { 0xC00000F2u, 87u },     /* INVALID_PARAMETER_4 */
    { 0xC00000F4u, 87u },     /* INVALID_PARAMETER_6 */
    { 0xC0000101u, 145u },    /* DIRECTORY_NOT_EMPTY: ERROR_DIR_NOT_EMPTY */
    { 0xC0000103u, 267u },    /* NOT_A_DIRECTORY: ERROR_DIRECTORY */
    { 0xC000011Eu, 1006u },   /* MAPPED_FILE_SIZE_ZERO: ERROR_FILE_INVALID */
    { 0xC0000135u, 126u },    /* DLL_NOT_FOUND: ERROR_MOD_NOT_FOUND */
    { 0xC0000139u, 127u },    /* ENTRYPOINT_NOT_FOUND: ERROR_PROC_NOT_FOUND */
    { 0xC000014Bu, 109u },    /* PIPE_BROKEN: ERROR_BROKEN_PIPE */
    { 0xC0000220u, 1132u },   /* MAPPED_ALIGNMENT: ERROR_MAPPED_ALIGNMENT */
    { 0xC0000235u, 6u },      /* HANDLE_NOT_CLOSABLE: ERROR_INVALID_HANDLE */
};

uint32_t RtlNtStatusToDosError(vibeos_ntstatus_t status) {
    uint32_t s = (uint32_t)status, i;

    /* Zero and a customer status (bit 29) are returned as they are; 0xD... is
     * 0xC...; facility 1 and 7 carry the Win32 error in the low word. */
    if (s == 0u || (s & 0x20000000u)) {
        return s;
    }
    if ((s & 0xF0000000u) == 0xD0000000u) {
        s &= ~0x10000000u;
    }
    if ((s >> 16) == 0xC001u || (s >> 16) == 0x8007u || (s >> 16) == 0xC007u) {
        return s & 0xFFFFu;
    }
    for (i = 0; i < sizeof(g_dos_errors) / sizeof(g_dos_errors[0]); i++) {
        if (g_dos_errors[i][0] == s) {
            return g_dos_errors[i][1];
        }
    }
    return 317u;   /* ERROR_MR_MID_NOT_FOUND */
}
