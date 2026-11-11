#!/usr/bin/env python3
"""
Read-only local KC vtable inspector for Stage 4.4.

Resolves logical C++ vptr offsets used by IOSkywalkPacket,
IOSkywalkPacketBufferPool and IOSkywalkPacketQueue disassembly.
It never builds, loads, installs or calls the actual kernel code.

Example:
  python3 scripts/awdl-abi/inspect_skywalk_vtables.py
  python3 scripts/awdl-abi/inspect_skywalk_vtables.py --offsets 0x140,0x148,0x158,0x198,0x1e0,0x168
"""
import argparse
import os
import subprocess
import sys

DEFAULT_KC = "/System/Library/KernelCollections/BootKernelExtensions.kc"
DEFAULT_WTL = os.path.expanduser("~/Desktop/WTLKcAbi")
CLASSES = (
    "IOSkywalkPacket",
    "IOSkywalkPacketBuffer",
    "IOSkywalkPacketBufferPool",
    "IOSkywalkPacketQueue",
    "IOSkywalkTxSubmissionQueue",
    "IOSkywalkTxCompletionQueue",
    "IOSkywalkRxCompletionQueue",
)
DEFAULT_OFFSETS = "0x140,0x148,0x158,0x198,0x1e0,0x160,0x168"


def demangle(name):
    if not name or name.startswith("<"):
        return name
    proc = subprocess.run(["c++filt", name], capture_output=True, text=True)
    return proc.stdout.strip() if proc.returncode == 0 else name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kc", default=DEFAULT_KC)
    parser.add_argument("--wtlkcabi", default=DEFAULT_WTL)
    parser.add_argument("--classes", default=",".join(CLASSES))
    parser.add_argument("--offsets", default=DEFAULT_OFFSETS)
    args = parser.parse_args()

    if not os.path.isfile(args.kc):
        parser.error("KC does not exist: " + args.kc)
    if not os.path.isdir(args.wtlkcabi):
        parser.error("WTLKcAbi does not exist: " + args.wtlkcabi)

    try:
        offsets = sorted(set(int(v.strip(), 0)
                             for v in args.offsets.split(",") if v.strip()))
    except ValueError as exc:
        parser.error("invalid offset: " + str(exc))
    if not offsets or any(value < 0 or value % 8 for value in offsets):
        parser.error("all offsets must be nonnegative 8-byte multiples")

    sys.path.insert(0, args.wtlkcabi)
    from kcfile import KC
    kc = KC(args.kc)
    print("KC:", args.kc)
    print("Offsets are measured from the C++ vptr (entry index = offset / 8).")
    print("Raw __ZTV symbol offsets include two 8-byte metadata entries;")
    print("e.g. __ZTV+0x168 corresponds to object-vptr+0x158 (slot 43).")
    found = 0
    for cls in (v.strip() for v in args.classes.split(",") if v.strip()):
        rows = kc.vtable(cls)
        print("\n[%s]" % cls)
        if not rows:
            print("  No matching vtable in this KC")
            continue
        # WTLKcAbi's KC.vtable rows include the two Itanium metadata cells;
        # verify_slots_kc.py uses 'i-2' to get the callable vptr index.
        by_index = {int(i) - 2: (target, symbol)
                    for i, _raw, target, symbol in rows
                    if int(i) >= 2}
        for offset in offsets:
            index = offset // 8
            entry = by_index.get(index)
            if entry is None:
                print("  +0x%x [slot %d]: unavailable" % (offset, index))
                continue
            address, symbol = entry
            if isinstance(address, int):
                location = "%#x" % address
            else:
                location = str(address)
            print("  +0x%x [slot %d]: %s %s" %
                  (offset, index, location, demangle(symbol)))
            found += 1
    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main())
