#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove that libFuzzer's AddressSanitizer catches a parser-style heap OOB."""
import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    clang = sys.argv[1] if len(sys.argv) > 1 else "clang"
    source = pathlib.Path(__file__).with_name("sanitizer_negative_control.c")
    with tempfile.TemporaryDirectory(prefix="vibeos-fuzz-negative-") as tmp:
        root = pathlib.Path(tmp)
        binary = root / "negative-control"
        corpus = root / "corpus"
        corpus.mkdir()
        (corpus / "oob-index").write_bytes(b"\x41")
        subprocess.run(
            [clang, "-g", "-O1", "-fsanitize=fuzzer,address",
             str(source), "-o", str(binary)],
            check=True,
        )
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "abort_on_error=1:halt_on_error=1:detect_leaks=0"
        result = subprocess.run(
            [str(binary), str(corpus), "-runs=10", "-max_len=1"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env,
        )
        if result.returncode == 0 or "AddressSanitizer" not in result.stdout:
            sys.stderr.write(result.stdout)
            raise SystemExit("negative control was not caught by ASan")
        print("PASS: ASan caught the deliberately out-of-bounds parser access")


if __name__ == "__main__":
    main()
