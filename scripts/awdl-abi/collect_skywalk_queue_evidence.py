#!/usr/bin/env python3
"""
Stage 4.4: read-only evidence collection from a LOCAL 25G332 Kernel Collection.

Runs on the developer's Mac with WTLKcAbi; never imports, installs or executes
kernel driver code. Outputs local text only. Do not commit the KC disassembly
to a public repository.

Usage:
  python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py --list-only
  python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py --out /tmp/awdl-skywalk-kc.txt
  python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \
      --methods legacyDequeue,enqueuePackets,completePackets --max-functions 12
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

DEFAULT_KC = "/System/Library/KernelCollections/BootKernelExtensions.kc"
DEFAULT_WTL = os.path.expanduser("~/Desktop/WTLKcAbi")
DEFAULT_OUT = "/tmp/awdl-skywalk-kc-evidence.txt"
OBJDUMP_FALLBACK = ("/Applications/Xcode.app/Contents/Developer/Toolchains/"
                    "XcodeDefault.xctoolchain/usr/bin/llvm-objdump")

FAMILIES = (
    "IOSkywalkTxSubmissionQueue",
    "IOSkywalkTxCompletionQueue",
    "IOSkywalkRxCompletionQueue",
    "IOSkywalkPacketQueue",
    "IOSkywalkPacketBufferPool",
    "IOSkywalkPacket",  # exact 'Class::' match excludes other packet classes
    "IOSkywalkPacketBuffer",
    "IOSkywalkPacketTable",
)
DEFAULT_METHODS = (
    "legacyDequeue",
    "enqueuePackets",
    "completePackets",
    "initWithPool",
    "withPool",
    "disable",
    "enable",
    "flush",
    "free",
    "release",
    "packetSubmission",
    "checkForWork",
    "notify",
    "getPacket",
    "returnPacket",
    "deallocatePacket",
    "deallocatePackets",
    "releaseAllPackets",
    "releaseAllMemorySegments",
    "completeWithQueue",
    "disposePacket",
    "finalize",
    "completePreparedPacket",
    "checkOutPacketQueue",
    "flushAllPackets",
    "disposePacketBuffer",
    "purgePackets",
    "complete",
    "cancelCompletionCallback",
    "setCompletionCallback",
    "checkInPacketQueue",
)


def demangle(names):
    result = {}
    for begin in range(0, len(names), 100):
        part = names[begin:begin + 100]
        proc = subprocess.run(["c++filt"] + part,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
        if proc.returncode != 0:
            raise RuntimeError("c++filt failed: " + proc.stderr)
        lines = proc.stdout.splitlines()
        if len(lines) != len(part):
            raise RuntimeError("c++filt returned inconsistent symbol count")
        result.update(zip(part, lines))
    return result


def method_name(demangled):
    match = re.search(r"::(~?[A-Za-z_][A-Za-z_0-9]*)\(", demangled)
    return match.group(1) if match else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kc", default=DEFAULT_KC)
    parser.add_argument("--wtlkcabi", default=DEFAULT_WTL)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--list-only", action="store_true")
    parser.add_argument("--classes", default=",".join(FAMILIES),
                        help="Comma-separated class names (restrict candidates)")
    parser.add_argument("--methods", default=",".join(DEFAULT_METHODS),
                        help="Comma-separated exact method names")
    parser.add_argument("--max-functions", type=int, default=30)
    parser.add_argument("--max-lines", type=int, default=450)
    args = parser.parse_args()

    if args.max_functions < 1 or args.max_lines < 1:
        parser.error("--max-functions and --max-lines must be positive")
    if not os.path.isfile(args.kc):
        parser.error("Kernel Collection not found: " + args.kc)
    if not os.path.isdir(args.wtlkcabi):
        parser.error("WTLKcAbi not found: " + args.wtlkcabi)

    sys.path.insert(0, args.wtlkcabi)
    from kcfile import KC
    kc = KC(args.kc)

    selected_classes = [name.strip() for name in args.classes.split(",")
                        if name.strip()]
    unknown = sorted(set(selected_classes) - set(FAMILIES))
    if not selected_classes or unknown:
        parser.error("unknown or empty class selection: %s" % ", ".join(unknown))
    candidates = [symbol for symbol in kc.byname
                  if any(family in symbol for family in selected_classes)
                  and symbol.startswith("__Z")]
    names = demangle(candidates)
    methods = [m.strip() for m in args.methods.split(",") if m.strip()]
    method_set = set(methods)
    items = []
    for mangled, decoded in names.items():
        cls = next((f for f in selected_classes if f + "::" in decoded), None)
        meth = method_name(decoded)
        if cls is None or meth not in method_set:
            continue
        try:
            address = kc.find(mangled)
        except (KeyError, ValueError):
            continue
        if not address:
            continue
        items.append((methods.index(meth), selected_classes.index(cls), decoded,
                      mangled, address))
    items.sort()
    print("KC:", args.kc)
    print("Matched queue/pool methods:", len(items))
    if not items:
        print("No methods found; inspect KC symbols and update filters.")
        return 1

    if args.list_only:
        for _, _, name, _, address in items:
            print("%#x %s" % (address, name))
        return 0

    if not os.environ.get("LLVM_OBJDUMP") and os.path.isfile(OBJDUMP_FALLBACK):
        os.environ["LLVM_OBJDUMP"] = OBJDUMP_FALLBACK

    from disasm import Disassembler
    dis = Disassembler(kc, tempfile.mkdtemp(prefix="itlwm-skywalk-kc-"))
    selection = items[:args.max_functions]
    # Write one function at a time, not an in-memory dump of the entire KC.
    with open(args.out, "w", encoding="utf-8") as output:
        output.write("TARGET: %s\nMETHODS: %s\n" %
                     (args.kc, args.methods))
        output.write("WARNING: local KC evidence, not validated ABI semantics.\n")
        for i, (_, _, name, mangled, address) in enumerate(selection, 1):
            output.write("\n%s\n%s\nADDRESS: %#x\nSYMBOL: %s\n" %
                         ("=" * 75, name, address, mangled))
            try:
                instructions = dis.function(address) or []
            except (RuntimeError, ValueError, KeyError) as exc:
                output.write("DISASSEMBLY ERROR: %s\n" % exc)
                continue
            for line in instructions[:args.max_lines]:
                output.write(line + "\n")
            if len(instructions) > args.max_lines:
                output.write("...TRUNCATED %d remaining lines...\n" %
                             (len(instructions) - args.max_lines))
            print("[%d/%d] %s: %d instruction lines" %
                  (i, len(selection), name, len(instructions)))

    print("Saved local evidence to:", args.out)
    print("Review output locally; do not commit proprietary disassembly.")
    if len(items) > len(selection):
        print("%d matching functions not dumped; increase --max-functions"
              % (len(items) - len(selection)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
