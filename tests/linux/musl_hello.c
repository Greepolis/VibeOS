/* An ordinary Linux program.
 *
 * Nothing in this file knows VibeOS exists. It is compiled by musl-gcc into a
 * static ELF64 executable, linked at the address Linux links executables at,
 * against a real C library - and then run, unmodified, by the kernel. That is
 * the whole point: every other program in this tree was written for VibeOS and
 * therefore proves only that VibeOS agrees with itself.
 *
 * It deliberately exercises the parts of a C runtime that touch the kernel:
 *
 *   - reaching main at all requires the startup stack, the auxiliary vector
 *     and thread-local storage to be right
 *   - malloc reaches the kernel through brk and mmap
 *   - printf reaches it through ioctl (deciding how to buffer) and writev
 *   - returning from main runs the exit path through exit_group
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    char *buf = malloc(64);
    if (!buf) {
        return 1;
    }
    strcpy(buf, "MUSL_OK: unmodified static Linux binary running on VibeOS");
    printf("%s\n", buf);
    printf("MUSL_ARGS: argc=%d argv0=%s\n", argc, argv[0] ? argv[0] : "(null)");
    free(buf);

    /* stdout is not a terminal here, so it is block buffered and nothing has
     * actually reached the kernel yet. Flushing is what turns the lines above
     * into a writev. */
    fflush(stdout);
    return 0;
}
