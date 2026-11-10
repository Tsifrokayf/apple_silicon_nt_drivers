#!/usr/bin/env python3
"""Make WDK/SDK headers usable on a case-sensitive file system.

Windows headers include each other with mixed case (e.g. <WdfQueryInterface.h>)
while the files in the NuGet packages may be named differently. For every
#include in the given trees that does not resolve with its exact spelling but
does resolve case-insensitively in the same directory, add a symlink with the
spelling the include uses. Idempotent: existing names are left alone.

Usage: casefix.py <include dir>...
"""
import os
import re
import sys

INCLUDE_RE = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


def main(dirs):
    # Index every file under every include dir by lower-case relative path.
    index = {}
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), d)
                index.setdefault(rel.lower(), (d, rel))

    wanted = set()
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                try:
                    with open(os.path.join(root, f), 'rb') as fh:
                        data = fh.read()
                except OSError:
                    continue
                for m in INCLUDE_RE.finditer(data):
                    wanted.add(m.group(1).decode('latin1').replace('\\', '/'))

    added = 0
    for name in sorted(wanted):
        if any(os.path.lexists(os.path.join(d, name)) for d in dirs):
            continue
        hit = index.get(name.lower())
        if hit is None:
            continue
        d, rel = hit
        # Only fix the file-name part; directories must already match.
        if os.path.dirname(rel) != os.path.dirname(name):
            continue
        os.symlink(os.path.basename(rel), os.path.join(d, name))
        added += 1
    print('casefix: %d symlinks added' % added)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
