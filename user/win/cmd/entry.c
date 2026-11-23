/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Where Wine's cmd.exe starts on VibeOS (docs/shell/s6.md step 2).
 *
 * mingw's start-up code would ask msvcrt for a dozen things only it uses. cmd
 * needs none of them: it reads GetCommandLineW itself, because an argv has
 * already lost the quotes its parameters depend on, so wmain's arguments are
 * never looked at. */

#include <stdint.h>

int wmain(int argc, uint16_t **argv);
__attribute__((dllimport, noreturn)) void __attribute__((ms_abi)) ExitProcess(uint32_t code);

void vibeos_cmd_start(void) {
    /* Windows enters on a stack aligned for a call that has already pushed its
     * return address; the compiler keeps that alignment from here. */
    ExitProcess((uint32_t)wmain(0, 0));
}
