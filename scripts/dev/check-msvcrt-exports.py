#!/usr/bin/env python3
"""msvcrt.dll exports exactly what msvcrt.def lists, and imports only ntdll.dll.

    scripts/dev/check-msvcrt-exports.py <msvcrt.dll> <msvcrt.def> [objdump]

The .def decides the export list (docs/abi/windows.md W2), but a name in it that
no function defines is a link error, and a function that is exported without
being listed would not be - what slips through is the DLL gaining an export
nobody asked for, or importing from a library the loader does not have. This
reads the built image with mingw's objdump, an artefact neither the .def nor
the C wrote, and compares.
"""

import re
import subprocess
import sys


def def_names(path):
    names = []
    started = False
    for line in open(path):
        line = line.split(';')[0].strip()
        if not line:
            continue
        if line.upper() == 'EXPORTS':
            started = True
            continue
        if started:
            names.append(line.split()[0])
    return names


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dll, deffile = sys.argv[1], sys.argv[2]
    objdump = sys.argv[3] if len(sys.argv) > 3 else 'x86_64-w64-mingw32-objdump'
    # The objdump and the DLL are CMake's arguments to a ctest, passed as a list: no
    # shell, and nothing a user supplies.
    out = subprocess.run([objdump, '-p', dll], capture_output=True, text=True, check=True).stdout  # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args

    wanted = def_names(deffile)
    problems = []
    if len(set(wanted)) != len(wanted):
        problems.append('duplicate names in the .def')

    exported = []
    in_table = False
    for line in out.splitlines():
        if 'Ordinal/Name Pointer' in line:
            in_table = True
            continue
        if in_table:
            m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s*$', line)
            if m:
                exported.append(m.group(1))
            elif line.strip() == '':
                break
    imports = sorted(set(m.group(1).lower() for m in re.finditer(r'DLL Name: (\S+)', out)))

    missing = sorted(set(wanted) - set(exported))
    extra = sorted(set(exported) - set(wanted))
    if missing:
        problems.append('listed but not exported: ' + ' '.join(missing))
    if extra:
        problems.append('exported but not listed: ' + ' '.join(extra))
    if imports != ['ntdll.dll']:
        problems.append('imports %s, expected only ntdll.dll' % imports)
    if not exported:
        problems.append('read no exports at all: objdump output changed?')

    if problems:
        for p in problems:
            print('FAIL', p)
        return 1
    print('msvcrt-exports=ok exports=%d imports=%s' % (len(exported), ','.join(imports)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
