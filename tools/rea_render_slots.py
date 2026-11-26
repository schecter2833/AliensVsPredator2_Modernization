#!/usr/bin/env python3
"""Compare the renderer function-pointer slots filled by RenderDLLSetup
in two builds of d3d.ren.

Input: the saved output of
    npx rea-agents@latest decompile <d3d.ren> --procedure RenderDLLSetup --snapshot <snap>
for each build. Prints offset -> implementing address for both builds and
reports any layout differences. Only addresses and offsets are printed, never
pseudocode.

usage: rea_render_slots.py <build_a.txt> <build_b.txt>
"""
import json
import re
import sys

SLOT_RE = re.compile(r"\(param_1 \+ (0x[0-9a-fA-F]+|\d+)\) = &?(?:FUN|LAB)_([0-9a-fA-F]+)")


def slots(path):
    text = open(path, encoding="utf-8").read()
    match = re.search(r'normalized_result: (".*?")\nconfidence', text, re.S)
    if not match:
        sys.exit(f"{path}: no pseudocode result found (is this REA decompile output?)")
    code = json.loads(match.group(1))
    return {int(off, 0): int(addr, 16) for off, addr in SLOT_RE.findall(code)}


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 1
    a, b = slots(argv[1]), slots(argv[2])
    print(f"slots: A={len(a)} B={len(b)}")
    for off in sorted(set(a) | set(b)):
        fa = f"0x{a[off]:08X}" if off in a else "-"
        fb = f"0x{b[off]:08X}" if off in b else "-"
        print(f"0x{off:03X}  slot {(off - min(a)) // 4:2d}  {fa}  {fb}")
    only_a, only_b = set(a) - set(b), set(b) - set(a)
    if only_a or only_b:
        print("LAYOUT DIFFERS:", sorted(map(hex, only_a)), sorted(map(hex, only_b)))
        return 2
    print("Layouts match.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
