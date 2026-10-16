/* What a Windows program meets when it calls a function nobody has written
 * yet (docs/abi/windows.md W2).
 *
 * Imports GetTickCount from kernel32, which is a placeholder that exports
 * nothing, so ntdll's loader binds it to a stub. Calling it must print
 * "unimplemented: KERNEL32.dll.GetTickCount" and end the process - the line
 * Wine's test programs will print for every function they reach first. The
 * gate reads it, and that NT_STUBCALL_RETURNED never comes. */

#include <stdint.h>

__declspec(dllimport) uint32_t __stdcall GetTickCount(void);
__declspec(dllimport) int printf(const char *format, ...);

int nt_stubcall_start(void) {
    printf("NT_STUBCALL: calling a function kernel32 does not have\n");
    (void)GetTickCount();
    printf("NT_STUBCALL_RETURNED\n");
    return 0;
}
