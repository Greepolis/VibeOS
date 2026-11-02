/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What a Windows program meets when it calls a function nobody has written
 * yet (docs/abi/windows.md W2).
 *
 * Imports SetFirmwareEnvironmentVariableA from kernel32, which ours does not
 * export - writing UEFI variables from a program is not something this
 * personality will ever offer - so ntdll's loader binds it to a stub.
 * Calling it must print "unimplemented:
 * KERNEL32.dll.SetFirmwareEnvironmentVariableA" and end the process - the
 * line Wine's test programs print for every function they reach first. The
 * gate reads it, and that NT_STUBCALL_RETURNED never comes.
 *
 * It imported GetTickCount until kernel32 was written and exported it, and
 * then Beep until W7 step 4 wrote that for kernel32:process; both times the
 * gate failed on a program that ran to the end. The function has to be one
 * kernel32 is never going to have. */

#include <stdint.h>

__declspec(dllimport) int __stdcall SetFirmwareEnvironmentVariableA(const char *name, const char *guid,
                                                                    void *value, uint32_t size);
__declspec(dllimport) int printf(const char *format, ...);

int nt_stubcall_start(void) {
    printf("NT_STUBCALL: calling a function kernel32 does not have\n");
    (void)SetFirmwareEnvironmentVariableA("x", "{00000000-0000-0000-0000-000000000000}", 0, 0u);
    printf("NT_STUBCALL_RETURNED\n");
    return 0;
}
