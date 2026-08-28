/* A dynamically linked Linux binary: the last kind this kernel could not run.
 *
 * The difference from the static and position-independent tests is not the
 * program, it is who starts it. A dynamic executable names an interpreter, and
 * the kernel maps both images and enters the *interpreter*, which relocates
 * itself, resolves the program's symbols and only then jumps to AT_ENTRY. Every
 * part of that has to be right for main() to be reached at all: the second
 * image mapped somewhere it fits, AT_BASE telling the interpreter where it
 * landed, AT_ENTRY still pointing at the program rather than at the loader.
 *
 * Built with musl-gcc and nothing from this project.
 */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    /* strlen through the C library, so a symbol really had to be resolved
     * rather than inlined by the compiler from a constant. */
    const char *name = argc > 0 ? argv[0] : "?";

    printf("DYN_OK: dynamically linked Linux binary running on VibeOS\n");
    printf("DYN_ARGS: argc=%d argv0len=%d\n", argc, (int)strlen(name));
    fflush(stdout);
    return 0;
}
