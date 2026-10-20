#ifndef VIBEOS_PE_H
#define VIBEOS_PE_H

/* PE32+ program images: the executables and DLLs of the NT personality
 * (docs/abi/windows.md W1).
 *
 * The parser describes a file; it maps nothing. Its answer is the image's
 * placement and, in `layout`, the same segment list the ELF parser produces, so
 * the architecture places a PE image with the code that places an ELF one - a
 * page at a time, each page filled from the file through whatever reader the
 * caller has. A second mapper would be a second place for the cache-sharing,
 * permission and region rules to drift apart in.
 *
 * What it accepts, and why no more yet:
 *
 *  - x86-64 only (machine 0x8664, optional-header magic 0x20B): the plan leaves
 *    32-bit programs out.
 *  - Sections aligned to a page or a multiple of one. The format allows less
 *    (file alignment equal to section alignment, below a page) and nothing
 *    this personality runs is built that way; a page that two sections share
 *    with different protections is a question the mapper should not have to
 *    answer for a file nobody here produces.
 *  - The image where its header says, inside the window the caller allows.
 *    Base relocation is the loader's next step, not this one: a file that
 *    does not fit is refused with ERANGE rather than placed and left
 *    unrelocated.
 *
 * Every field is read byte by byte and every sum checked before use: the file
 * is untrusted input, as an ELF is. */

#include <stdint.h>

#include "vibeos/elf.h"

/* Headers plus every section must fit; a section past this is refused. */
#define VIBEOS_PE_MAX_SECTIONS (VIBEOS_ELF_MAX_SEGMENTS - 1u)
#define VIBEOS_PE_MAX_DIRS 16u

/* The data directories the loaders read (index into dir_rva / dir_size). */
#define VIBEOS_PE_DIR_EXPORT 0u
#define VIBEOS_PE_DIR_IMPORT 1u
#define VIBEOS_PE_DIR_EXCEPTION 3u
#define VIBEOS_PE_DIR_BASERELOC 5u
#define VIBEOS_PE_DIR_TLS 9u

/* IMAGE_FILE_DLL, in `characteristics`. */
#define VIBEOS_PE_FILE_DLL 0x2000u

enum {
    VIBEOS_PE_OK = 0,
    VIBEOS_PE_ETRUNCATED = 1,   /* shorter than its headers say               */
    VIBEOS_PE_ENOTPE = 2,       /* no MZ, or no PE signature where it points  */
    VIBEOS_PE_EMACHINE = 3,     /* not x86-64, or not PE32+                   */
    VIBEOS_PE_EMALFORMED = 4,   /* overlapping, overflowing or absurd         */
    VIBEOS_PE_ERANGE = 5,       /* not inside the window the caller allows    */
    VIBEOS_PE_ETOOMANY = 6,     /* more sections than an image holds          */
    VIBEOS_PE_EALIGN = 7        /* sections aligned below a page              */
};

typedef struct vibeos_pe_image {
    uint64_t image_base;        /* where the image is put                      */
    uint64_t preferred_base;    /* where the file asked to be                  */
    int64_t delta;              /* image_base - preferred_base: 0, or relocated */
    uint64_t image_size;        /* SizeOfImage                                 */
    uint64_t entry;             /* absolute; 0 when the file names none        */
    uint32_t headers_size;      /* SizeOfHeaders                               */
    uint16_t characteristics;   /* COFF header: VIBEOS_PE_FILE_DLL and others  */
    uint16_t dll_characteristics;
    uint16_t subsystem;         /* 3 is a console program                      */
    uint16_t sections;          /* how many the file declared                  */
    uint64_t stack_reserve;
    uint64_t stack_commit;
    uint32_t dirs;              /* how many directories the file has (<= 16)   */
    uint32_t dir_rva[VIBEOS_PE_MAX_DIRS];
    uint32_t dir_size[VIBEOS_PE_MAX_DIRS];
    /* The headers and every section that occupies memory, as segments: what
     * the mapper places. min_vaddr is image_base, end_vaddr its end rounded to
     * a page; nothing in it is relocatable (is_dyn = 0) or has an
     * interpreter. */
    vibeos_elf_image_t layout;
} vibeos_pe_image_t;

/* How many bytes from the start of the file the headers need, section table
 * included - what a caller staging a header window must have read - or 0 for a
 * window too short to tell. */
uint64_t vibeos_pe_header_extent(const void *hdr, uint64_t hdr_len);

/* Describe the image whose first `hdr_len` bytes are at `hdr` and whose file is
 * `file_len` long. Section contents are not read, only checked against
 * `file_len`. The image must lie in [min_allowed, end_allowed). */
int vibeos_pe_parse(const void *hdr, uint64_t hdr_len, uint64_t file_len,
                    uint64_t min_allowed, uint64_t end_allowed,
                    vibeos_pe_image_t *out);

/* The same, placed at `base` instead of where the file asks (W2): every address
 * in the answer - layout, entry - is `base`'s, and `delta` says by how much the
 * image moved. ERANGE if it does not fit [min_allowed, end_allowed) there, or if
 * it cannot move at all: its relocations stripped, or none to apply. The
 * relocations themselves are applied as its pages are filled
 * (vibeos_pe_relocate_page), because that is the only moment the kernel holds
 * each page privately. */
int vibeos_pe_parse_at(const void *hdr, uint64_t hdr_len, uint64_t file_len,
                       uint64_t base, uint64_t min_allowed, uint64_t end_allowed,
                       vibeos_pe_image_t *out);

/* The base-relocation directory, read whole by the caller from the image (its
 * RVA and size are dir_rva/dir_size[VIBEOS_PE_DIR_BASERELOC]), checked once:
 * blocks that tile it exactly, each for a page inside the image, every entry
 * ABSOLUTE or DIR64 - the only kinds a PE32+ linker writes - and every DIR64
 * target's eight bytes inside the image. 0, or VIBEOS_PE_EMALFORMED. */
int vibeos_pe_check_relocs(const vibeos_pe_image_t *pe, const uint8_t *relocs,
                           uint32_t len);

/* Apply `pe->delta` to the page at `page_va`, just filled from the file into
 * `page`: every DIR64 entry whose eight bytes touch this page, including the
 * tail of one that starts on the page before. Each value is read from the file
 * through `read` - not from the page - so an entry straddling two pages comes
 * out right whichever is filled first. Relocations already checked by
 * vibeos_pe_check_relocs. */
/* Does a base relocation write into this page - an entry of its block, or of
 * the block before it, which may straddle into it? A loader that leaves a
 * zero-fill page to the page fault must not leave one of these: the fault would
 * make zeros where the relocation wrote the image's delta. Conservative: a
 * block for the page is a yes. */
int vibeos_pe_page_relocated(const vibeos_pe_image_t *pe, const uint8_t *relocs, uint32_t len,
                             uint64_t page_va);
void vibeos_pe_relocate_page(const vibeos_pe_image_t *pe, const uint8_t *relocs,
                             uint32_t len, vibeos_elf_read_fn read, void *ctx,
                             uint64_t page_va, uint8_t *page);

/* `n` bytes of the image at `va` as the file supplies them - zero where no
 * section's file part reaches - through `read`, or from a file held whole in
 * memory. What a loader reads a directory out of before the image is mapped. */
void vibeos_pe_read_image(const vibeos_pe_image_t *pe, vibeos_elf_read_fn read, void *ctx,
                          uint64_t va, uint8_t *out, uint32_t n);
void vibeos_pe_read_image_mem(const vibeos_pe_image_t *pe, const void *file, uint64_t len,
                              uint64_t va, uint8_t *out, uint32_t n);

/* IMAGE_FILE_RELOCS_STRIPPED: the image can only be where it asks to be. */
#define VIBEOS_PE_FILE_RELOCS_STRIPPED 0x0001u

/* A file is a PE image if it starts "MZ"; the ELF magic is checked first by
 * whoever has both, so this is only the question of which parser to ask. */
int vibeos_pe_is_pe(const void *hdr, uint64_t hdr_len);

#endif
