#!/usr/bin/env python3
"""
Verify the experimental 25G332 IO80211 ABI headers against the Tahoe SDK.

This harness is intentionally standalone: it does NOT build, link, install or
load the driver. It compiles three throw-away probe classes, asks clang to dump
the vtable layout, and checks the function-slot indices that were read from
BootKernelExtensions.kc.

Expected anchors (25G332 / Darwin 25.6.0):
    IOSkywalkInterface                 size 0xB0
    IOSkywalkNetworkInterface          size 0xE0
    IOSkywalkEthernetInterface         size 0x120
    IO80211SkywalkInterface            size 0x128, vtable 0xE80 (464 entries)
    IO80211VirtualInterface            size 0x130, vtable 0xFE0 (508 entries)
    IO80211AWDLProtocol                size 0x130, vtable 0x11D8 (571 entries)

Slot anchors:
    enable=266  disable=267  initBSDInterfaceParameters=284
    setMacAddress=488 (pure)  first AWDL slot=505  last AWDL slot=567

Run:  python3 scripts/awdl-abi/verify_abi.py
"""

import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MK = os.path.join(ROOT, "MacKernelSDK")
SRC_DIR = os.path.join(ROOT, "scripts", "awdl-abi")


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
    "-std=c++17",
    "-fno-rtti",
    "-fno-exceptions",
    "-D", "KERNEL",
    "-D", "KERNEL_PRIVATE",
    "-D", "DRIVER_PRIVATE",
    "-D", "APPLE",
    "-D", "NeXT",
    "-D", "__IO80211_TARGET=__MAC_26_0",
    "-D", "__PRIVATE_SPI__",
    "-D", "IO80211FAMILY_V2",
    "-I", os.path.join(ROOT, "include"),
    "-I", os.path.join(MK, "Headers"),
    "-I", os.path.join(SDK, "System/Library/Frameworks/Kernel.framework/PrivateHeaders"),
]

failures = []


def check(cond, msg):
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def compile_probe(name, outdir):
    src = os.path.join(SRC_DIR, "probe_%s.cpp" % name)
    obj = os.path.join(outdir, "probe_%s.o" % name)
    r = run(["clang++"] + FLAGS + ["-Xclang", "-fdump-vtable-layouts", "-c", src, "-o", obj])
    if r.returncode != 0:
        print(r.stdout)
        raise SystemExit("compile failed for probe_%s.cpp" % name)
    return obj, r.stdout


def vtable_entries(dump, cls):
    m = re.search(r"Vtable for '%s' \((\d+) entries\)" % re.escape(cls), dump)
    return int(m.group(1)) if m else None


def slot_indices(dump, cls):
    """Return {function-slot: declaration} for a class's 'VTable indices' block."""
    pat = re.compile(r"VTable indices for '%s' \(\d+ entries\)\.\n(.*?)(?:\n\n|\Z)" % re.escape(cls), re.S)
    m = pat.search(dump)
    if not m:
        return {}
    out = {}
    for line in m.group(1).splitlines():
        mm = re.match(r"\s*(\d+) \| (.+)$", line)
        if mm:
            out[int(mm.group(1))] = mm.group(2)
    return out


def full_listing(dump, cls):
    """Return {full-listing-index: declaration}; index = function slot + 2."""
    pat = re.compile(r"Vtable for '%s' \(\d+ entries\)\.\n(.*?)(?=\nVTable indices|\Z)" % re.escape(cls), re.S)
    m = pat.search(dump)
    if not m:
        return {}
    out = {}
    for line in m.group(1).splitlines():
        mm = re.match(r"\s*(\d+) \| (.+)$", line)
        if mm:
            out[int(mm.group(1))] = mm.group(2)
    return out


def main():
    print("SDK: %s" % SDK)
    print("MacKernelSDK: %s" % MK)
    outdir = tempfile.mkdtemp(prefix="itlwm-awdl-abi-")

    # ---- Skywalk chain -----------------------------------------------------
    _, dump = compile_probe("sky", outdir)
    entries = vtable_entries(dump, "ProbeSky")
    idx = slot_indices(dump, "ProbeSky")
    full = full_listing(dump, "ProbeSky")
    print("\n[IO80211SkywalkInterface]")
    check(entries == 464, "vtable entries == 464 (KC span 0xE80)")
    check(idx.get(266, "").startswith("uintptr_t ProbeSky::enable"), "enable at slot 266")
    check(idx.get(267, "").startswith("uintptr_t ProbeSky::disable"), "disable at slot 267")
    check(idx.get(284, "").startswith("uintptr_t ProbeSky::initBSDInterfaceParameters"), "initBSDInterfaceParameters at slot 284")
    check("syncDPSStats" in full.get(462, ""), "syncDPSStats at slot 460")

    # ---- VirtualInterface --------------------------------------------------
    _, dump = compile_probe("vif", outdir)
    entries = vtable_entries(dump, "ProbeVIF")
    idx = slot_indices(dump, "ProbeVIF")
    full = full_listing(dump, "ProbeVIF")
    print("\n[IO80211VirtualInterface]")
    check(entries == 508, "vtable entries == 508 (KC span 0xFE0)")
    check("IO80211VirtualInterface::enable" in full.get(268, ""), "enable resolves to IO80211VirtualInterface at slot 266")
    check(idx.get(267, "").startswith("uintptr_t ProbeVIF::disable"), "disable at slot 267")
    check(idx.get(284, "").startswith("uintptr_t ProbeVIF::initBSDInterfaceParameters"), "initBSDInterfaceParameters at slot 284")
    check(idx.get(488, "").startswith("uintptr_t ProbeVIF::setMacAddress"), "setMacAddress at slot 488")
    check("_RESERVEDIO80211VirtualInterface15" in full.get(506, ""), "last reserved slot 504")

    # ---- AWDLProtocol ------------------------------------------------------
    obj, dump = compile_probe("awdl", outdir)
    entries = vtable_entries(dump, "ProbeAWDL")
    idx = slot_indices(dump, "ProbeAWDL")
    print("\n[IO80211AWDLProtocol]")
    check(entries == 571, "vtable entries == 571 (KC span 0x11D8)")
    check(idx.get(266, "").startswith("uintptr_t ProbeAWDL::enable"), "enable at slot 266")
    check(idx.get(267, "").startswith("uintptr_t ProbeAWDL::disable"), "disable at slot 267")
    check(idx.get(284, "").startswith("uintptr_t ProbeAWDL::initBSDInterfaceParameters"), "initBSDInterfaceParameters at slot 284")
    check(idx.get(488, "").startswith("uintptr_t ProbeAWDL::setMacAddress"), "setMacAddress at slot 488")
    check(idx.get(505, "").startswith("IOReturn ProbeAWDL::getSSID"), "first AWDL slot 505 = getSSID")
    check(idx.get(567, "").startswith("IOReturn ProbeAWDL::setAWDL_RTG_PEER_STATS_CONFIG"),
          "last AWDL slot 567 = setAWDL_RTG_PEER_STATS_CONFIG")
    awdl_slots = [s for s in idx if 505 <= s <= 567]
    check(len(awdl_slots) == 63 and min(awdl_slots) == 505 and max(awdl_slots) == 567,
          "63 AWDL slots occupy exactly 505..567")
    check(not [s for s in idx if s not in (0, 1, 7, 266, 267, 284, 488) and not (505 <= s <= 567)],
          "no unexpected override indices")

    # ---- link as a kext ----------------------------------------------------
    kext = os.path.join(outdir, "probe.kext", "Contents", "MacOS")
    os.makedirs(kext, exist_ok=True)
    linked = os.path.join(kext, "probe")
    r = run(["xcrun", "ld", "-arch", "x86_64", "-kext", "-undefined", "dynamic_lookup",
             "-platform_version", "macos", "10.15", "26.0", "-o", linked, obj,
             "-L", os.path.join(MK, "Library", "x86_64"), "-lkmod"])
    print("\n[link]")
    if r.returncode != 0:
        print(r.stdout)
    check(r.returncode == 0, "links as a kext (-kext -undefined dynamic_lookup)")
    if r.returncode == 0:
        ftype = run(["file", linked]).stdout
        check("kext bundle" in ftype, "output is a Mach-O kext bundle")

    print("\n" + ("ALL CHECKS PASSED" if not failures else "%d CHECK(S) FAILED" % len(failures)))
    print("Artifacts: %s" % outdir)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
