/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Where Wine's cmd.exe starts on VibeOS (docs/shell/s6.md step 2).
 *
 * mingw's start-up code would ask msvcrt for a dozen things only it uses. cmd
 * needs none of them: it reads GetCommandLineW itself, because an argv has
 * already lost the quotes its parameters depend on, so wmain's arguments are
 * never looked at.
 *
 * A process started from a Linux shell inherits a Linux PATH, whose directories
 * mean nothing to cmd, and no PATHEXT: `cmd /c ...` from a batch file was "not
 * recognized". Windows' own defaults go in unless the PATH is already a Windows
 * one. */

#include <stdint.h>

int wmain(int argc, uint16_t **argv);
__attribute__((dllimport, noreturn)) void ExitProcess(uint32_t code);
__attribute__((dllimport)) uint32_t GetEnvironmentVariableW(const uint16_t *name, uint16_t *buf, uint32_t size);
__attribute__((dllimport)) int32_t SetEnvironmentVariableW(const uint16_t *name, const uint16_t *value);

static const uint16_t k_path[] = {'P', 'A', 'T', 'H', 0};
static const uint16_t k_pathext[] = {'P', 'A', 'T', 'H', 'E', 'X', 'T', 0};
static const uint16_t k_root[] = {'S', 'y', 's', 't', 'e', 'm', 'R', 'o', 'o', 't', 0};
static const uint16_t v_path[] = u"C:\\Windows\\System32;C:\\Windows";
static const uint16_t v_pathext[] = u".COM;.EXE;.BAT;.CMD";
static const uint16_t v_root[] = u"C:\\Windows";

static void default_environment(void) {
    uint16_t b[512];
    uint32_t n = GetEnvironmentVariableW(k_path, b, 512u);

    /* A Windows PATH has a drive in its first entry. */
    if (n == 0u || n >= 512u || n < 2u || b[1] != ':') {
        (void)SetEnvironmentVariableW(k_path, v_path);
    }
    if (GetEnvironmentVariableW(k_pathext, b, 512u) == 0u) {
        (void)SetEnvironmentVariableW(k_pathext, v_pathext);
    }
    if (GetEnvironmentVariableW(k_root, b, 512u) == 0u) {
        (void)SetEnvironmentVariableW(k_root, v_root);
    }
}

void vibeos_cmd_start(void) {
    default_environment();
    ExitProcess((uint32_t)wmain(0, 0));
}
