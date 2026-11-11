#!/usr/bin/env python3
"""
Stage 2: verify the real AirportItlwmAWDLInterface class against the 25G332
AWDL ABI evidence.

This is not a design probe. It compiles the actual
AirportItlwm/AirportItlwmAWDLInterface.cpp with the experimental gate enabled,
then checks:

  * the emitted vtable has exactly 571 entries, like
    __ZTV19IO80211AWDLProtocol in BootKernelExtensions.kc;
  * there is no remaining ___cxa_pure_virtual entry;
  * slot 488 is AirportItlwmAWDLInterface::setMacAddress(ether_addr&);
  * slots 505..567 are the 63 verified AWDL methods, in the exact order and
    with the exact parameter types recorded in evidence_25G332.tsv;
  * the class has no unexpected overrides;
  * DWARF says the instance size is 0x130;
  * every expected mangled symbol is defined in the object file;
  * optionally, the same symbols are present in a built experimental kext and
    absent from a default kext.

Usage:
    python3 scripts/awdl-abi/verify_awdl_class.py
    python3 scripts/awdl-abi/verify_awdl_class.py \
        --kext /path/to/experimental/AirportItlwm.kext \
        --default-kext /path/to/default/AirportItlwm.kext
"""

import argparse
import csv
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MK = os.path.join(ROOT, "MacKernelSDK")
SRC = os.path.join(ROOT, "AirportItlwm", "AirportItlwmAWDLInterface.cpp")
HEADER = os.path.join(ROOT, "AirportItlwm", "AirportItlwmAWDLInterface.hpp")
EVIDENCE = os.path.join(ROOT, "scripts", "awdl-abi", "evidence_25G332.tsv")

CLS = "AirportItlwmAWDLInterface"
MANGLE_PREFIX = "__ZN25AirportItlwmAWDLInterface"
PROXI_PREFIX = "__ZN30AppleBCMWLANProximityInterface"
FIRST, LAST = 505, 567
SIZE = 0x130
VTABLE_ENTRIES = 571
SET_MAC_MANGLED = MANGLE_PREFIX + "13setMacAddressER10ether_addr"

failures = []


def check(cond, msg):
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, **kw)


def find_sdk():
    sdk = os.environ.get("SDKROOT")
    if sdk and os.path.isdir(sdk):
        return sdk
    base = "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs"
    for pattern in ("MacOSX26*.sdk", "MacOSX*.sdk"):
        cands = sorted(glob.glob(os.path.join(base, pattern)))
        if cands:
            return cands[-1]
    raise SystemExit("no macOS SDK found; set SDKROOT")


SDK = find_sdk()
FLAGS = [
    "-target", "x86_64-apple-macos10.15",
    "-mkernel",
    "-std=gnu++14",
    "-fno-rtti",
    "-fno-exceptions",
    "-fapple-kext",
    "-D", "KERNEL",
    "-D", "KERNEL_PRIVATE",
    "-D", "DRIVER_PRIVATE",
    "-D", "APPLE",
    "-D", "NeXT",
    "-D", "__IO80211_TARGET=__MAC_26_0",
    "-D", "__PRIVATE_SPI__",
    "-D", "IO80211FAMILY_V2",
    "-D", "ITLWM_AWDL_EXPERIMENTAL=1",
    "-I", os.path.join(ROOT, "include"),
    "-I", os.path.join(MK, "Headers"),
    "-I", os.path.join(SDK, "System/Library/Frameworks/Kernel.framework/PrivateHeaders"),
]


def load_evidence():
    rows = list(csv.DictReader(open(EVIDENCE), delimiter="\t"))
    if len(rows) != 63:
        raise SystemExit("evidence table has %d rows, expected 63" % len(rows))
    out = []
    for row in rows:
        slot = int(row["slot"])
        mangled = row["mangled"].replace(PROXI_PREFIX, MANGLE_PREFIX, 1)
        out.append({
            "slot": slot,
            "method": row["method"],
            "param": re.sub(r"\s+", "", row["param"]).replace("struct", ""),
            "mangled": mangled,
        })
    if [r["slot"] for r in out] != list(range(FIRST, LAST + 1)):
        raise SystemExit("evidence slots are not exactly 505..567 in order")
    return out


def compile_object(outdir):
    obj = os.path.join(outdir, "AirportItlwmAWDLInterface.o")
    r = run(["xcrun", "clang++"] + FLAGS +
            ["-g", "-Xclang", "-fdump-vtable-layouts", "-c", SRC, "-o", obj])
    if r.returncode != 0:
        print(r.stdout)
        raise SystemExit("compile failed for %s" % SRC)
    return obj, r.stdout


def vtable_entries(dump, cls):
    m = re.search(r"Vtable for '%s' \((\d+) entries\)" % re.escape(cls), dump)
    return int(m.group(1)) if m else None


def full_listing(dump, cls):
    """{full-listing index: declaration}; index = function slot + 2."""
    pat = re.compile(
        r"Vtable for '%s' \(\d+ entries\)\.\n(.*?)(?=\nVTable indices|\Z)"
        % re.escape(cls), re.S)
    m = pat.search(dump)
    if not m:
        return {}
    out = {}
    for line in m.group(1).splitlines():
        mm = re.match(r"\s*(\d+) \| (.+)$", line)
        if mm:
            out[int(mm.group(1))] = mm.group(2)
    return out


def slot_indices(dump, cls):
    """{function slot: declaration}, ignoring interleaved compiler diagnostics."""
    pat = re.compile(
        r"VTable indices for '%s' \(\d+ entries\)\.\n(.*?)(?:\n\n|\Z)"
        % re.escape(cls), re.S)
    m = pat.search(dump)
    if not m:
        return {}
    out = {}
    for line in m.group(1).splitlines():
        mm = re.match(r"\s*(\d+) \| (.+)$", line)
        if not mm:
            continue
        value = mm.group(2).strip()
        # Warning context lines look like "31 | return ..." or "#define ...".
        if value.startswith(("return ", "#define ", "#include ", "~~~", "^")):
            continue
        if "kIOReturnUnsupported" in value or "iokit_common_err" in value:
            continue
        out[int(mm.group(1))] = value
    return out


def text_symbols(obj):
    r = run(["nm", "-m", obj])
    if r.returncode != 0:
        raise SystemExit("nm failed for %s:\n%s" % (obj, r.stdout))
    return set(re.findall(r"\(__TEXT,__text\) external (\S+)", r.stdout))


def defined_symbols(obj):
    r = run(["nm", "-m", obj])
    if r.returncode != 0:
        raise SystemExit("nm failed for %s:\n%s" % (obj, r.stdout))
    return set(re.findall(r"^[0-9a-f]+ \([^)]+\) (?:external|non-external) (\S+)$",
                          r.stdout, re.M))


def undefined_symbols(obj):
    r = run(["nm", "-u", obj])
    if r.returncode != 0:
        raise SystemExit("nm -u failed for %s:\n%s" % (obj, r.stdout))
    return set(r.stdout.split())


def kext_symbols(kext):
    exe = os.path.join(kext, "Contents", "MacOS", "AirportItlwm")
    r = run(["nm", "-m", exe])
    if r.returncode != 0:
        raise SystemExit("nm failed for %s:\n%s" % (exe, r.stdout))
    return r.stdout


def dwarf_size(obj):
    r = run(["xcrun", "llvm-dwarfdump", "--name=" + CLS, obj])
    if r.returncode != 0:
        return None
    m = re.search(r'DW_AT_name\s+\("%s"\)\s*\n\s*DW_AT_byte_size\s+\(0x([0-9a-fA-F]+)\)'
                  % re.escape(CLS), r.stdout)
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kext", help="experimental AirportItlwm.kext to inspect")
    ap.add_argument("--default-kext", help="default AirportItlwm.kext to inspect")
    args = ap.parse_args()

    print("SDK: %s" % SDK)
    print("Source: %s" % SRC)
    evidence = load_evidence()

    outdir = tempfile.mkdtemp(prefix="itlwm-awdl-class-")
    obj, dump = compile_object(outdir)

    # ---- vtable ------------------------------------------------------------
    print("\n[vtable]")
    entries = vtable_entries(dump, CLS)
    check(entries == VTABLE_ENTRIES,
          "vtable entries == %d (KC __ZTV19IO80211AWDLProtocol)" % VTABLE_ENTRIES)

    full = full_listing(dump, CLS)
    check(len(full) == VTABLE_ENTRIES,
          "full listing has %d entries" % VTABLE_ENTRIES)
    pure = [i for i, v in full.items() if "pure_virtual" in v]
    check(not pure, "no ___cxa_pure_virtual entries remain")
    check(not [i for i in range(2, 570) if not full.get(i)],
          "every function slot 0..567 is populated")

    idx = slot_indices(dump, CLS)
    check(idx.get(488, "").startswith(
              "uintptr_t %s::setMacAddress(ether_addr &)" % CLS),
          "slot 488 = setMacAddress(ether_addr&)")

    bad = []
    for row in evidence:
        decl = idx.get(row["slot"], "")
        want = "::%s(" % row["method"]
        params = re.sub(r"\s+", "", decl)
        if want not in decl or row["param"] not in params:
            bad.append((row["slot"], row["method"], decl))
    check(not bad, "all 63 AWDL slots 505..567 match evidence (name + parameter)")

    awdl_slots = [s for s in idx if FIRST <= s <= LAST]
    check(awdl_slots == list(range(FIRST, LAST + 1)),
          "63 AWDL slots occupy exactly 505..567 in order")

    expected_indices = {0, 1, 7, 488} | set(range(FIRST, LAST + 1))
    unexpected = sorted(set(idx) - expected_indices)
    check(not unexpected,
          "no unexpected overrides (got %s)" % (unexpected or "none"))

    # ---- size --------------------------------------------------------------
    print("\n[instance size]")
    size = dwarf_size(obj)
    check(size == SIZE, "DWARF DW_AT_byte_size == 0x%x (got %s)"
          % (SIZE, "0x%x" % size if size is not None else "missing"))
    check(os.path.exists(HEADER), "header exists")

    # ---- mangled symbols ---------------------------------------------------
    print("\n[mangled symbols]")
    syms = text_symbols(obj)
    all_syms = defined_symbols(obj)
    expected = [row["mangled"] for row in evidence] + [SET_MAC_MANGLED]
    missing = [s for s in expected if s not in syms]
    check(not missing, "all 64 expected method symbols are defined")
    check(MANGLE_PREFIX + "10gMetaClassE" in all_syms,
          "metaclass gMetaClass is defined")
    check("__ZTV25" + CLS in all_syms,
          "vtable symbol __ZTV25%s is defined" % CLS)

    # ---- Stage 3 lifecycle -------------------------------------------------
    print("\n[stage 3 lifecycle]")
    lifecycle_methods = (
        "initLifecycle",
        "startLifecycle",
        "stopLifecycle",
        "teardownLifecycle",
        "buildRegistrationInfo",
        "isLifecycleInitialized",
        "isLifecycleRunning",
    )
    missing_lifecycle = [m for m in lifecycle_methods
                         if not any(m in s for s in all_syms)]
    check(not missing_lifecycle,
          "all lifecycle methods are defined (missing %s)"
          % (missing_lifecycle or "none"))

    # The inherited vtable still references
    # IOSkywalkNetworkInterface::registerNetworkInterfaceWithLogicalLink.
    # These non-virtual registration/pool entry points must not be referenced
    # by the Stage 3 object at all.
    undef = undefined_symbols(obj)
    forbidden_prefixes = (
        "__ZN26IOSkywalkEthernetInterface25registerEthernetInterface",
        "__ZN26IOSkywalkEthernetInterface20initRegistrationInfo",
        "__ZN26IOSkywalkEthernetInterface27deregisterEthernetInterface",
        "__ZN25IOSkywalkPacketBufferPool",
        "__ZN17IOSkywalkQueueSet",
    )
    forbidden = sorted(s for s in undef
                       if any(s.startswith(p) for p in forbidden_prefixes))
    check(not forbidden,
          "no direct registration / packet-pool references (got %s)"
          % (forbidden or "none"))

    # ---- optional built kexts ---------------------------------------------
    if args.kext:
        print("\n[experimental kext]")
        text = kext_symbols(args.kext)
        missing = [s for s in expected if (" " + s) not in text]
        check(not missing, "all 64 method symbols are in the experimental kext")
        check(("__ZTV25" + CLS) in text, "vtable symbol is in the experimental kext")
        check((MANGLE_PREFIX + "10gMetaClassE") in text,
              "metaclass gMetaClass is in the experimental kext")

    if args.default_kext:
        print("\n[default kext]")
        text = kext_symbols(args.default_kext)
        check(CLS not in text,
              "default kext contains no %s symbols (empty TU)" % CLS)

    print("\n" + ("ALL CHECKS PASSED" if not failures
                  else "%d CHECK(S) FAILED" % len(failures)))
    print("Artifacts: %s" % outdir)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
