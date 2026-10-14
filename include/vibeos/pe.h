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

typedef struct {
    uint64_t image_base;        /* where the file asks to be, and is put       */
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

/* A file is a PE image if it starts "MZ"; the ELF magic is checked first by
 * whoever has both, so this is only the question of which parser to ask. */
int vibeos_pe_is_pe(const void *hdr, uint64_t hdr_len);

#endif
