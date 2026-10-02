r"""Lists function entry points of a PS1 RAM image by scanning every `jal` in it.

Usage: python re\ghidra\jal_targets.py <ram.bin> <out functions.txt>
Output: one "<hex address> <call sites> 0" line per distinct jal target inside KSEG0 RAM, the format
SeedFunctions.java / DecompileSeeds.java read. Nothing from the image itself is written.
"""
import struct
import sys
from collections import Counter

ram = open(sys.argv[1], "rb").read()
base = 0x80000000
calls = Counter()
for off in range(0x10000, len(ram) - 3, 4):
    w = struct.unpack_from("<I", ram, off)[0]
    if (w >> 26) == 3:  # jal
        target = ((base + off) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        if 0x80010000 <= target < base + len(ram):
            calls[target] += 1
with open(sys.argv[2], "w") as out:
    for t in sorted(calls):
        out.write("%08X %d 0\n" % (t, calls[t]))
print("jal_targets: %d functions" % len(calls))
