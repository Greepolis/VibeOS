/* What a Windows program meets when it calls a function nobody has written
 * yet (docs/abi/windows.md W2).
 *
 * Imports Beep from kernel32, which ours does not export - there is no sound
 * device to give it - so ntdll's loader binds it to a stub. Calling it must
 * print "unimplemented: KERNEL32.dll.Beep" and end the process - the line
 * Wine's test programs print for every function they reach first. The gate
 * reads it, and that NT_STUBCALL_RETURNED never comes.
 *
 * It imported GetTickCount until kernel32 was written and exported it; the
 * gate then failed in CI on a program that ran to the end. The function has
 * to be one kernel32 is never going to have. */

#include <stdint.h>

__declspec(dllimport) int __stdcall Beep(uint32_t frequency, uint32_t duration);
__declspec(dllimport) int printf(const char *format, ...);

int nt_stubcall_start(void) {
    printf("NT_STUBCALL: calling a function kernel32 does not have\n");
    (void)Beep(440u, 1u);
    printf("NT_STUBCALL_RETURNED\n");
    return 0;
}
