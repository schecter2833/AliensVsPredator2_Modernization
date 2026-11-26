#!/usr/bin/env python3
"""Print imports, exports, linker version, PDB paths and RTTI class counts
for LithTech binaries. Requires: pip install pefile

usage: pe_inventory.py <binary> [<binary> ...]
"""
import collections
import os
import re
import sys

import pefile


def inventory(path):
    pe = pefile.PE(path)
    imports = collections.OrderedDict()
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        imports[entry.dll.decode()] = [
            (i.name or b"#%d" % i.ordinal).decode() for i in entry.imports
        ]
    exports = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        exports = [
            (s.name or b"#%d" % s.ordinal).decode()
            for s in pe.DIRECTORY_ENTRY_EXPORT.symbols
        ]
    pdbs = [
        d.entry.PdbFileName.decode(errors="ignore").strip("\0")
        for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", [])
        if hasattr(d.entry, "PdbFileName")
    ]
    data = open(path, "rb").read()
    rtti = sorted(set(m.decode() for m in re.findall(rb"\.\?AV[A-Za-z0-9_@]+@@", data)))
    return pe, imports, exports, pdbs, rtti


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    for path in argv[1:]:
        pe, imports, exports, pdbs, rtti = inventory(path)
        oh = pe.OPTIONAL_HEADER
        print(f"== {os.path.basename(path)}")
        print(f"   image base 0x{oh.ImageBase:08x}, linker {oh.MajorLinkerVersion}.{oh.MinorLinkerVersion}")
        print(f"   pdb: {pdbs or 'none'}")
        print(f"   exports ({len(exports)}): {', '.join(exports)}")
        for dll, names in imports.items():
            print(f"   import {dll} ({len(names)})")
        print(f"   RTTI classes: {len(rtti)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
