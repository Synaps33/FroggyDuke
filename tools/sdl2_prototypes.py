#!/usr/bin/env python3
"""Print the exact SDL2 prototypes the shim has to match.

The shim implements SDL2 against the vendored headers, so every signature it
defines must agree with them. Reading them off by hand is how the first build
ended with 40-odd "conflicting types" errors, so extract them mechanically and
print `symbol -> prototype` for each one named on the command line.
"""
import glob
import re
import sys

HDRS = glob.glob("/home/Sajnaps/eduke32-sf2000/thirdparty/include/SDL2/*.h")

START = re.compile(r"^[A-Za-z_].*\b(S[A-Za-z_0-9]+)\(")


def prototypes():
    """Yield (symbol, full_prototype_text) for every declaration in the headers."""
    for path in HDRS:
        lines = open(path, errors="replace").read().splitlines()
        i = 0
        while i < len(lines):
            line = lines[i]
            if not START.match(line) or line.lstrip().startswith(("*", "#")):
                i += 1
                continue
            # Walk forward until the declaration is balanced.
            text = line
            while text.count("(") > text.count(")") and i + 1 < len(lines):
                i += 1
                text += " " + lines[i].strip()
            m = re.search(r"\b(S[A-Za-z_0-9]+)\s*\(", text)
            if m and text.rstrip().endswith(";"):
                yield m.group(1), " ".join(text.split())
            i += 1


if __name__ == "__main__":
    wanted = set(sys.argv[1:])
    for sym, proto in prototypes():
        if sym in wanted:
            print(f"{sym}\t{proto}")