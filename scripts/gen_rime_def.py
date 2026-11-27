#!/usr/bin/env python3
"""rime_exports.txt (dumpbin /exports) -> rime.def (EXPORTS with ordinals)."""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
lines = open(src, encoding="utf-8", errors="replace").read().splitlines()
names = []
for l in lines:
    # dumpbin 行：ordinal(十进制) hint(十六进制) RVA(十六进制) name[ = demangled]
    m = re.match(r"\s*(\d+)\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+(\S+)", l)
    if m:
        names.append((int(m.group(1)), m.group(4)))
names.sort()
with open(dst, "w", encoding="ascii", newline="\n") as f:
    f.write("LIBRARY rime\nEXPORTS\n")
    for o, n in names:
        f.write("    %s @%d\n" % (n, o))
print("exports:", len(names))
sys.exit(0 if names else 1)
