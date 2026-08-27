/* A position-independent Linux binary, to prove VibeOS can place one.
 *
 * An ET_DYN executable describes itself from address zero and expects the
 * loader to choose where it lives, which is a different contract from the
 * static ET_EXEC files this kernel started with: the entry point, the program
 * headers the C runtime reads through AT_PHDR, and every absolute address the
 * program forms about itself all shift by the same bias. Getting one of those
 * wrong does not fail at load time, it fails somewhere later and confusingly,
 * so the check is that the program runs at all and can see its own arguments.
 *
 * Written against Linux, not against VibeOS: built with musl-gcc -static-pie
 * and never linked with anything from this project.
 */
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("PIE_OK: position-independent Linux binary running on VibeOS\n");
    printf("PIE_ARGS: argc=%d argv0=%s\n", argc, argc > 0 ? argv[0] : "(none)");
    fflush(stdout);
    return 0;
}
