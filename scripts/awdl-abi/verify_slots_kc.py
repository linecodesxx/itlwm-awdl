#!/usr/bin/env python3
"""
Stage 1.5: verify the 63 IO80211AWDLProtocol slots against the running
BootKernelExtensions.kc, using watchthelight-labs/WTLKcAbi.

What this proves (and what it does not):

  * Every AWDL slot 505..567 is pure (___cxa_pure_virtual) in
    __ZTV19IO80211AWDLProtocol.
  * AppleBCMWLANProximityInterface : IO80211AWDLProtocol overrides all 63
    slots with 63 distinct functions, and each mangled name carries the
    parameter type.
  * Each override's slot is independently reachable from a global
    apple80211get*/apple80211set* bridge that dispatches to the same vtable
    offset after safeMetaCast to IO80211AWDLProtocol.
  * Every override returns a 32-bit value in eax (no pointer / float return),
    consistent with IOReturn.
  * The declarations in IO80211AWDLProtocolABI_25G332.h match the KC mangled
    names and parameter types exactly.

It does NOT prove the semantics of any method, the layout of any payload
struct, or that a driver can advertise AWDL. Stage 1 stays fail-closed.

Usage:
    python3 scripts/awdl-abi/verify_slots_kc.py
    python3 scripts/awdl-abi/verify_slots_kc.py --kc /path/BootKernelExtensions.kc \
        --wtlkcabi ~/Desktop/WTLKcAbi --out scripts/awdl-abi/evidence_25G332.tsv
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(ROOT, "include", "Airport", "experimental",
                      "IO80211AWDLProtocolABI_25G332.h")
DEFAULT_KC = "/System/Library/KernelCollections/BootKernelExtensions.kc"
DEFAULT_WTL = os.path.expanduser("~/Desktop/WTLKcAbi")
AWDL_CLASS = "IO80211AWDLProtocol"
PROXI_CLASS = "AppleBCMWLANProximityInterface"
FIRST, LAST = 505, 567
OBJDUMP_FALLBACK = ("/Applications/Xcode.app/Contents/Developer/Toolchains/"
                    "XcodeDefault.xctoolchain/usr/bin/llvm-objdump")


def fail(msg):
    print("FAIL: " + msg)
    return 1


def load_kc(wtlkcabi, kcpath):
    sys.path.insert(0, wtlkcabi)
    from kcfile import KC  # noqa: E402
    return KC(kcpath)


def slot_map(kc, cls):
    vt = kc.vtable(cls)
    if vt is None:
        raise SystemExit("vtable not found: " + cls)
    return {i - 2: (raw, tgt, name) for i, raw, tgt, name in vt}, len(vt)


def demangle(names):
    if not names:
        return {}
    out = subprocess.run(["c++filt"] + names, capture_output=True,
                         text=True, check=True).stdout.splitlines()
    return dict(zip(names, out))


def parse_header(path):
    text = open(path).read()
    body = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    decls = re.findall(
        r"virtual\s+([\w:]+)\s+(\w+)\s*\(([^;]*?)\)\s*=\s*0\s*;", body, re.S)
    return [(ret, name, re.sub(r"\s+", "", params).replace("struct", ""))
            for ret, name, params in decls]


def bridge_slots(kc, dis, proxi_methods):
    """slot -> (bridge symbol, demangled bridge) for the AWDL branch.

    proxi_methods maps slot -> demangled Proxi method name. A bridge is only
    accepted when its method name matches that slot's override, so an offset
    that is shared with another protocol class cannot produce a false match.
    """
    names = [n for n in kc.byname
             if re.match(r"^__Z\d+apple80211(?:get|set)", n)
             and "P23IO80211SkywalkInterface" in n]
    dm = demangle(names)
    out = {}
    for n in names:
        lines = dis.function(kc.find(n)) or []
        base = re.sub(r"^apple80211", "", dm[n].split("(")[0])
        cands = set()
        for line in lines:
            for m in re.finditer(
                    r"(?:mov\s+(?:r1[0-5]d|e?[abcd]x|e?[sd]i|r\d+d), "
                    r"|\[r\w+ \+ )(0x[0-9a-f]+)", line):
                v = int(m.group(1), 16)
                if v % 8 == 0 and FIRST <= v // 8 <= LAST:
                    cands.add(v // 8)
        good = [s for s in cands if proxi_methods[s].startswith(base)]
        if len(good) == 1:
            out[good[0]] = (n, dm[n])
    return out


RET_WRITE = re.compile(
    r"^(?:mov|xor|lea|or|and|add|sub|imul|movzx|movsx|set[a-z]+|pop)\s+"
    r"(eax|rax|al|xmm0)\b")


def return_expr(dis, va):
    """Expressions written to the return register before each ret."""
    lines = dis.function(va) or []
    ins = []
    for line in lines:
        m = re.match(r"^([0-9a-f]+):\s+(\S+)\s*(.*)$", line)
        if m:
            ins.append((m.group(2), m.group(3)))
    exprs = []
    for i, (mnem, ops) in enumerate(ins):
        if mnem != "ret":
            continue
        expr = "?"
        for j in range(i - 1, max(-1, i - 40), -1):
            mj, oj = ins[j]
            if RET_WRITE.match("%s %s" % (mj, oj.split(",")[0].strip())):
                expr = ("%s %s" % (mj, oj)) if "," in oj else mnem
                break
            if mj == "call":
                expr = "call->eax"
                break
        exprs.append(expr)
    return sorted(set(exprs))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--kc", default=os.environ.get("KCABI_KC", DEFAULT_KC))
    ap.add_argument("--wtlkcabi", default=os.environ.get("WTLKCABI", DEFAULT_WTL))
    ap.add_argument("--out", default=os.path.join(
        ROOT, "scripts", "awdl-abi", "evidence_25G332.tsv"))
    ap.add_argument("--no-disasm", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.wtlkcabi):
        return fail("WTLKcAbi not found at %s (use --wtlkcabi)" % args.wtlkcabi)
    kc = load_kc(args.wtlkcabi, args.kc)
    awdl, awdl_entries = slot_map(kc, AWDL_CLASS)
    proxi, proxi_entries = slot_map(kc, PROXI_CLASS)
    decls = parse_header(HEADER)

    problems = []
    print("KC: %s" % args.kc)
    print("  %s: %d entries" % (AWDL_CLASS, awdl_entries))
    print("  %s: %d entries" % (PROXI_CLASS, proxi_entries))
    if awdl_entries != 571:
        problems.append("AWDL vtable entries %d != 571" % awdl_entries)
    if proxi_entries != 574:
        problems.append("Proxi vtable entries %d != 574" % proxi_entries)
    if len(decls) != 63:
        problems.append("header declares %d slots, expected 63" % len(decls))

    for s in range(FIRST, LAST + 1):
        if "pure_virtual" not in awdl[s][2]:
            problems.append("AWDL slot %d is not pure: %s" % (s, awdl[s][2]))

    # Subclass boundary: Proxi keeps the shared prefix and adds exactly three
    # slots after the AWDL block.
    if "setMacAddress" not in proxi[488][2]:
        problems.append("Proxi slot 488 is not setMacAddress: %s" % proxi[488][2])
    for s, want in ((568, "getTX_RATE"), (569, "init"), (570, "freeResources")):
        if want not in proxi[s][2]:
            problems.append("Proxi slot %d is not %s: %s" % (s, want, proxi[s][2]))

    proxi_names = [proxi[s][2] for s in range(FIRST, LAST + 1)]
    if len(set(proxi_names)) != 63:
        problems.append("Proxi overrides are not 63 distinct symbols")
    targets = [proxi[s][1] for s in range(FIRST, LAST + 1)]
    if len(set(targets)) != 63:
        problems.append("Proxi overrides share target addresses (ICF aliases)")

    dm = demangle(proxi_names)
    proxi_methods = {}
    for s, sym in zip(range(FIRST, LAST + 1), proxi_names):
        m = re.match(r"%s::(\w+)\(" % PROXI_CLASS, dm[sym])
        if m:
            proxi_methods[s] = m.group(1)
    bridges = {}
    dis = None
    if not args.no_disasm:
        if not os.environ.get("LLVM_OBJDUMP"):
            if os.path.exists(OBJDUMP_FALLBACK):
                os.environ["LLVM_OBJDUMP"] = OBJDUMP_FALLBACK
        from disasm import Disassembler  # noqa: E402
        dis = Disassembler(kc, tempfile.mkdtemp(prefix="itlwm-awdl-kc-"))
        bridges = bridge_slots(kc, dis, proxi_methods)

    rows = ["slot\toffset\tawdl\tproxi\tmangled\tmethod\tparam\treturn\t"
            "bridge\tbridge_off\tconfidence"]
    verified = 0
    for s, (ret, name, param) in zip(range(FIRST, LAST + 1), decls):
        sym = proxi[s][2]
        exp = dm[sym]
        m = re.match(r"%s::(\w+)\((.*)\)$" % PROXI_CLASS, exp)
        if not m:
            problems.append("slot %d: cannot parse %s" % (s, exp))
            continue
        kc_name, kc_param = m.group(1), re.sub(r"\s+", "", m.group(2))
        if name != kc_name:
            problems.append("slot %d: header %s != KC %s" % (s, name, kc_name))
        if param != kc_param:
            problems.append("slot %d: header param %s != KC %s"
                            % (s, param, kc_param))
        if ret != "IOReturn":
            problems.append("slot %d: header return %s != IOReturn" % (s, ret))

        rv = "not-disassembled"
        if dis is not None:
            exprs = return_expr(dis, proxi[s][1])
            rv = " | ".join(exprs)
            if any(re.match(r"(mov|lea|pop) +rax\b", e) for e in exprs) \
                    or any("xmm0" in e for e in exprs):
                problems.append("slot %d: non-int return: %s" % (s, rv))

        b = bridges.get(s)
        if dis is not None and b is None:
            problems.append("slot %d: no apple80211 bridge cross-reference" % s)
        if b is not None:
            bm = re.match(r"[\w:~]+\(([^)]*)\)$", b[1])
            barg = [re.sub(r"\s+", "", x).replace("struct", "")
                    for x in bm.group(1).split(",")] if bm else []
            if len(barg) != 2 or barg[1] != param:
                problems.append("slot %d: bridge param %s != header %s"
                                % (s, barg[1] if len(barg) == 2 else barg, param))
        conf = "verified" if (b is not None and dis is not None) else "high"
        if conf == "verified":
            verified += 1
        rows.append("\t".join([
            str(s), "%#x" % (s * 8), "pure", "%#x" % proxi[s][1], sym,
            kc_name, kc_param, rv,
            b[1] if b else "-", "%#x" % (s * 8) if b else "-", conf]))

    with open(args.out, "w") as f:
        f.write("\n".join(rows) + "\n")

    print("  slots verified with bridge + disassembly: %d/63" % verified)
    print("  evidence table: %s" % args.out)
    if problems:
        for p in problems:
            print("  FAIL: " + p)
        print("\n%d CHECK(S) FAILED" % len(problems))
        return 1
    print("\nALL SLOT CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
