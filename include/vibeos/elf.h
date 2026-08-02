#ifndef VIBEOS_ELF_H
#define VIBEOS_ELF_H

/* ELF64 program-image parser.
 *
 * Portable and side-effect free: it reads an image that is already in memory
 * and produces a description of what should be mapped where. It allocates
 * nothing and touches no hardware, so the whole of it - validation, page
 * permission merging, .bss - is exercised by host tests, while the same code
 * loads programs on metal.
 *
 * The important property is that the caller works a page at a time, not a
 * segment at a time. Two segments routinely share a page: a real binary's
 * .text usually ends part way through the page where .data begins. Mapping
 * segment by segment means allocating that page twice and losing whichever
 * was mapped first, and giving it whichever permissions happened to come
 * last. Asking per page instead makes both impossible.
 *
 * All addresses are the virtual addresses from the file; nothing here assumes
 * a particular address space layout beyond the bounds the caller supplies.
 */

#include <stdint.h>

#define VIBEOS_ELF_MAX_SEGMENTS 8u
#define VIBEOS_ELF_PAGE_SIZE 4096u

/* Permission bits, matching the ELF PF_* values. */
#define VIBEOS_ELF_X 1u
#define VIBEOS_ELF_W 2u
#define VIBEOS_ELF_R 4u

/* Parse failures. Distinct values because "this is not for us" and "this is
 * malformed" deserve different reporting. */
enum {
    VIBEOS_ELF_OK = 0,
    VIBEOS_ELF_ETRUNCATED = 1,   /* image shorter than the headers claim   */
    VIBEOS_ELF_ENOTELF = 2,      /* bad magic                              */
    VIBEOS_ELF_EMACHINE = 3,     /* not 64-bit x86-64                      */
    VIBEOS_ELF_ETYPE = 4,        /* not a static executable                */
    VIBEOS_ELF_EDYNAMIC = 5,     /* needs an interpreter; unsupported      */
    VIBEOS_ELF_EMALFORMED = 6,   /* overlapping, overflowing or absurd     */
    VIBEOS_ELF_ERANGE = 7,       /* outside the address range allowed      */
    VIBEOS_ELF_ETOOMANY = 8      /* more PT_LOAD segments than we hold     */
};

typedef struct {
    uint64_t vaddr;
    uint64_t file_off;
    uint64_t filesz;
    uint64_t memsz;
    uint32_t flags;      /* VIBEOS_ELF_R/W/X */
} vibeos_elf_segment_t;

typedef struct {
    vibeos_elf_segment_t seg[VIBEOS_ELF_MAX_SEGMENTS];
    uint32_t count;
    uint64_t entry;
    uint64_t min_vaddr;  /* page-aligned down, across all PT_LOAD */
    uint64_t end_vaddr;  /* page-aligned up, exclusive            */
    uint64_t phdr_vaddr; /* address of the program headers, 0 if not mapped */
    uint16_t phnum;
    uint16_t phentsize;
} vibeos_elf_image_t;

/* Validate `image` and describe it. `min_allowed`/`end_allowed` bound where the
 * program may ask to be placed; a file that asks for anything outside is
 * rejected rather than trusted. Returns VIBEOS_ELF_OK or one of the errors. */
int vibeos_elf_parse(const void *image, uint64_t len,
                     uint64_t min_allowed, uint64_t end_allowed,
                     vibeos_elf_image_t *out);

/* Permissions for one page: the union over every segment that touches it, so a
 * page shared between .text and .data ends up readable, writable and
 * executable rather than whichever segment was mapped last. Returns 0 when no
 * segment covers the page. */
uint32_t vibeos_elf_page_flags(const vibeos_elf_image_t *img, uint64_t page_va);

/* ---- initial process stack ------------------------------------------------
 *
 * A freestanding program with a hand-written _start can be handed a bare
 * stack. A real one cannot: a C runtime reads argc, argv, the environment and
 * the auxiliary vector from the stack before it runs a single line of the
 * program, and dies without them. AT_PHDR, AT_PHNUM and AT_PHENT in particular
 * are how it finds the program headers it was loaded from.
 *
 * The layout at entry, lowest address first, is fixed by the System V ABI:
 *
 *     argc
 *     argv[0] .. argv[argc-1], NULL
 *     envp[0] .. envp[n-1],    NULL
 *     auxv pairs, terminated by AT_NULL
 *     (strings the pointers above refer to)
 *
 * with the stack pointer 16-byte aligned. This builder fills a caller-provided
 * buffer that represents the top of the stack and reports where the stack
 * pointer must start, so the same arithmetic is exercised by host tests
 * instead of only on metal.
 */

#define VIBEOS_AT_NULL 0u
#define VIBEOS_AT_PHDR 3u
#define VIBEOS_AT_PHENT 4u
#define VIBEOS_AT_PHNUM 5u
#define VIBEOS_AT_PAGESZ 6u
#define VIBEOS_AT_ENTRY 9u
#define VIBEOS_AT_UID 11u
#define VIBEOS_AT_EUID 12u
#define VIBEOS_AT_GID 13u
#define VIBEOS_AT_EGID 14u
#define VIBEOS_AT_RANDOM 25u

typedef struct {
    const char *const *argv;   /* NULL-terminated, may be NULL for none */
    const char *const *envp;   /* NULL-terminated, may be NULL for none */
    uint64_t entry;            /* AT_ENTRY  */
    uint64_t phdr_vaddr;       /* AT_PHDR, 0 to omit */
    uint16_t phnum;
    uint16_t phentsize;
    const uint8_t *random16;   /* AT_RANDOM bytes, may be NULL */
} vibeos_elf_stack_desc_t;

/* Build the startup stack into `buf`, which represents the `buf_len` bytes
 * ending at virtual address `stack_top`. Returns the virtual stack pointer to
 * start the program on, or 0 if it does not fit. */
uint64_t vibeos_elf_build_stack(void *buf, uint64_t buf_len, uint64_t stack_top,
                                const vibeos_elf_stack_desc_t *desc);

/* Fill one page of the image. `dst` must be VIBEOS_ELF_PAGE_SIZE bytes and is
 * fully written: bytes backed by the file are copied, everything else - .bss
 * and padding either side - is zeroed. */
void vibeos_elf_fill_page(const vibeos_elf_image_t *img, const void *image,
                          uint64_t page_va, void *dst);

#endif
