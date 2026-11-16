/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_UTF8FOLD_H
#define VIBEOS_UTF8FOLD_H

/* Names compared without regard to case, as Linux's utf8-12.1 compares them in
 * an ext4 casefolded directory (E4-030): each name is taken to its "nfdicf"
 * form - full case folding, canonical decomposition, combining marks in
 * canonical order - and the two forms are compared as bytes. Generated from
 * the Unicode 12.1 database (scripts/dev/make-utf8-fold.py). Portable, and
 * meant for any filesystem or personality that folds names: Windows' own
 * case-insensitive names are a later question, with their own table. */

#include <stdint.h>

/* The folded form of `in` as UTF-8 into `out`: its length, or -1 when the
 * name is not valid UTF-8, holds a code point Unicode 12.1 does not assign,
 * or folds to more than `cap` bytes. Linux on a volume that is not strict
 * then compares such a name as the bytes it is. */
int32_t vibeos_utf8_fold(const char *in, uint32_t len, uint8_t *out, uint32_t cap);

/* 1 when the two names are equal without regard to case, by the rule above:
 * folded and compared when both fold, compared as bytes when either does not. */
int vibeos_utf8_fold_eq(const char *a, uint32_t alen, const char *b, uint32_t blen);

/* 1 when the name folds: valid UTF-8 of code points Unicode 12.1 assigns.
 * What a strict casefold volume asks of every name it is given. */
int vibeos_utf8_folds(const char *name, uint32_t len);

#endif
