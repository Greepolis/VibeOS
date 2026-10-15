/* A PE32+ image built by mingw-w64, for tests/kernel/pe_tests.c: the artefact
 * neither the parser nor its test wrote. Never run - it only has to be the kind
 * of file a linker writes: code, initialised and read-only data, a .bss larger
 * than a page, so that a section's memory size and its file size differ.
 *
 * Built -nostdlib with an entry of its own, so it has no imports and no C
 * runtime; CMake links it at the address the test expects (VIBEOS_PE_FIXTURE_BASE). */

int fixture_counter = 3;
volatile char fixture_bss[20000];
const char fixture_text[] = "read-only data of the PE fixture";
/* Absolute addresses, in writable and in read-only data: what base relocation
 * exists to move (docs/abi/windows.md W2). The linker records each in .reloc. */
int *fixture_ptr = &fixture_counter;
const char *const fixture_strs[2] = { fixture_text, fixture_text + 5 };

int fixture_start(void) {
    fixture_bss[5] = (char)fixture_counter;
    return fixture_bss[5] + fixture_text[0] + *fixture_ptr + fixture_strs[1][0];
}
