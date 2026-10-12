#!/usr/bin/env python3
"""List the SDL2 entry points the console build actually calls.

Preprocesses the translation units eduke32 builds with USE_OPENGL=0 and keeps
only identifiers that come from project sources, not from the SDL headers, so
the result is the shim's actual contract rather than SDL's whole API.
"""
import re
import subprocess
import sys

SRC = "/home/Sajnaps/eduke32-sf2000/src"
UNITS = [
    "source/build/src/sdlayer.cpp",
    "source/audiolib/src/driver_sdl.cpp",
    "source/audiolib/src/fx_man.cpp",
    "source/audiolib/src/drivers.cpp",
    "source/imgui/src/imgui_impl_sdl2.cpp",
]
INC = [
    "-I/usr/include/x86_64-linux-gnu",
    "-I/usr/include/SDL2",
    f"-I{SRC}/source/build/include",
    f"-I{SRC}/source/audiolib/include",
    f"-I{SRC}/source/imgui/include",
    f"-I{SRC}/source/glad/include",
    f"-I{SRC}",
]
DEF = [
    "-DSDL_TARGET=2",
    "-DRENDERTYPESDL",
    "-DSDL_USEFOLDER",
    "-DNDEBUG",
    "-DRELEASE=1",
]

marker = re.compile(r'^# \d+ "([^"]*)"')

def used_symbols(path):
    out = subprocess.run(
        ["g++", "-E", "-m32", *INC, *DEF, f"{SRC}/{path}"],
        capture_output=True, text=True,
    )
    if out.returncode != 0:
        print(f"!! preprocess failed for {path}:\n{out.stderr[:800]}", file=sys.stderr)
        return set()
    found, inside_sdl = set(), False
    for line in out.stdout.splitlines():
        m = marker.match(line)
        if m:
            f = m.group(1)
            inside_sdl = ("/SDL2/" in f) or ("/usr/include/SDL" in f)
            continue
        if inside_sdl or line.startswith("#"):
            continue
        found.update(re.findall(r"\b(SDL_[A-Za-z_0-9]+)\b", line))
    return found

all_syms = set()
for u in UNITS:
    s = used_symbols(u)
    print(f"{u}: {len(s)} symbols", file=sys.stderr)
    all_syms |= s


print(f"\n=== {len(all_syms)} SDL_ identifiers referenced from project code ===")
for _s in sorted(all_syms):
    print(_s)
