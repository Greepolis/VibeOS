/* A DLL that loads and exports nothing (docs/abi/windows.md W2): built as
 * kernel32.dll, user32.dll and advapi32.dll until each is written.
 *
 * Wine's test programs import all three, and a DLL missing from the disk ends a
 * process before its first line. Present and empty, every function a program
 * imports from it is bound to one of ntdll's named stubs instead, so the
 * program runs until it calls the first one - and says which, which is the
 * order these DLLs get written in. */

#include <stdint.h>

int vibeos_placeholder_main(uint64_t instance, uint32_t reason, void *reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;   /* TRUE: loaded */
}
