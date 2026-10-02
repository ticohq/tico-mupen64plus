#!/usr/bin/env python3
"""Resolve a standalone profile.txt (sdmc:/switch/mupen64plus/profile.txt) to functions.

usage: tools/resolve_profile.py profile.txt [elf] [top]

The ELF defaults to build_tico_standalone/mupen64plus_tico.elf and must be the
build that produced the profile. addr2line runs in the switch-dev image unless
aarch64-none-elf-addr2line is on PATH.
"""
import collections
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.environ.get("SWITCH_DEV_IMAGE", "ghcr.io/autorunhq/switch-dev:2026.10.01")


def addr2line(elf, addresses):
    args = ["-f", "-C", "-e", elf] + ["0x%x" % a for a in addresses]
    tool = shutil.which("aarch64-none-elf-addr2line")
    if tool:
        cmd = [tool] + args
    else:
        cmd = ["docker", "run", "--rm", "-v", "%s:%s" % (ROOT, ROOT), IMAGE,
               "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line"] + args
    lines = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.splitlines()
    # addr2line prints function and file:line per address
    return [lines[i] for i in range(0, len(lines), 2)]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    profile = sys.argv[1]
    elf = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else
                          os.path.join(ROOT, "build_tico_standalone", "mupen64plus_tico.elf"))
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40

    header = {}
    buckets = []
    with open(profile) as f:
        for line in f:
            parts = line.split()
            if not parts or line.startswith("#"):
                continue
            if parts[0] == "total":
                header = dict(zip(parts[0::2], map(int, parts[1::2])))
                continue
            buckets.append((int(parts[0], 16), int(parts[1])))

    total = header.get("total", 0) or 1
    by_function = collections.Counter()
    for (offset, count), name in zip(buckets, addr2line(elf, [b[0] for b in buckets])):
        by_function[name] += count

    by_function["[R4300 dynarec code]"] = header.get("r4300_jit", 0)
    by_function["[other JIT code (paraLLEl-RSP)]"] = header.get("other_jit", 0)

    print("%d samples (%d failed)" % (header.get("total", 0), header.get("failed", 0)))
    for name, count in by_function.most_common(top):
        print("%6.2f%%  %6d  %s" % (100.0 * count / total, count, name))


if __name__ == "__main__":
    main()
