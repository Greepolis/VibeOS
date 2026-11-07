/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Deliberately vulnerable parser used only to prove the fuzzer's ASan runtime
 * is active. Never link this file into a production target. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t *record;
    uint8_t result;

    if (size == 0u) {
        return 0;
    }
    record = (uint8_t *)malloc(1u);
    if (!record) {
        return 0;
    }
    /* Negative control: an untrusted index is used without a bound. */
    record[data[0]] = 0xA5u;
    result = record[0];
    free(record);
    return result;
}
