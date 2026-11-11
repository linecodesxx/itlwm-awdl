#!/usr/bin/env python3
"""
Stage 4: verify the experimental AWDL datapath resource manager.

The Stage 4 manager owns five Skywalk resources (TX pool, RX pool, TX
submission queue, TX completion queue, RX completion queue) but must never
register, attach, start or transmit. This script proves the parts that can be
proved without loading the kext:

  * the host test builds cleanly with -Wall -Wextra -Werror and passes every
    state-machine, reverse-rollback and exactly-once ledger check;
  * the source keeps both compile-time gates closed
    (ITLWM_AWDL_DATAPATH_LIVE == 0, ITLWM_AWDL_REGISTRATION_ENABLED == 0);
  * the real backend object references exactly the four 25G332 factory
    symbols and no registration/attach entry point;
  * the AWDL interface object does not reference the datapath manager;
  * an experimental kext contains the manager but a default kext does not.

Usage:
    python3 scripts/awdl-abi/verify_datapath.py
    python3 scripts/awdl-abi/verify_datapath.py \
        --kext /path/to/experimental/AirportItlwm.kext \
        --default-kext /path/to/default/AirportItlwm.kext
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MK = os.path.join(ROOT, "MacKernelSDK")
DATAPATH_CPP = os.path.join(ROOT, "AirportItlwm", "AirportItlwmAWDLDatapath.cpp")
DATAPATH_HPP = os.path.join(ROOT, "AirportItlwm", "AirportItlwmAWDLDatapath.hpp")
AWDL_CPP = os.path.join(ROOT, "AirportItlwm", "AirportItlwmAWDLInterface.cpp")
TEST_CPP = os.path.join(ROOT, "scripts", "awdl-abi", "test_datapath.cpp")

FACTORY_SYMBOLS = (
    "__ZN25IOSkywalkPacketBufferPool8withNameEPKcP8OSObjectjPKNS_11PoolOptionsE",
    "__ZN26IOSkywalkTxSubmissionQueue8withPoolEP25IOSkywalkPacketBufferPooljjP8OSObjectPFjS3_PS_PKP15IOSkywalkPacketjPvES9_j",
    "__ZN26IOSkywalkTxCompletionQueue8withPoolEP25IOSkywalkPacketBufferPooljjP8OSObjectPFjS3_PS_PP15IOSkywalkPacketjPvES8_j",
    "__ZN26IOSkywalkRxCompletionQueue8withPoolEP25IOSkywalkPacketBufferPooljjP8OSObjectPFjS3_PS_PP15IOSkywalkPacketjPvES8_j",
)

DATAPATH_DEFINED = (
    "__ZN19itlwm_awdl_datapath20createSkywalkBackendEPv",
    "__ZN19itlwm_awdl_datapath21destroySkywalkBackendEPNS_7BackendE",
)

# Entry points the datapath object must not reference. Virtual enable/disable
# calls go through the vtable and are checked by source review instead.
FORBIDDEN_IMPORTS = (
    "registerEthernetInterface",
    "deregisterEthernetInterface",
    "registerNetworkInterface",
    "initRegistrationInfo",
    "deferBSDAttach",
    "setInterfaceEnable",
)

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
KERNEL_FLAGS = [
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
    "-I", os.path.join(ROOT, "AirportItlwm"),
    "-I", os.path.join(MK, "Headers"),
    "-I", os.path.join(SDK, "System/Library/Frameworks/Kernel.framework/PrivateHeaders"),
]


def kext_binary(path):
    if os.path.isdir(path):
        return os.path.join(path, "Contents", "MacOS", "AirportItlwm")
    return path


def nm(path, undefined=False):
    path = kext_binary(path)
    cmd = ["nm"]
    if undefined:
        cmd.append("-u")
    cmd.append(path)
    result = run(cmd)
    if result.returncode != 0:
        raise SystemExit("nm failed for %s:\n%s" % (path, result.stdout))
    out = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if not fields:
            continue
        # Defined symbols are "address type name"; undefined are just "name".
        out.add(fields[-1])
    return out


def compile_kernel_object(source, outdir, name):
    obj = os.path.join(outdir, name + ".o")
    result = run(["xcrun", "clang++"] + KERNEL_FLAGS +
                 ["-c", source, "-o", obj])
    if result.returncode != 0:
        raise SystemExit("compile failed for %s:\n%s" % (source, result.stdout))
    return obj


def test_host_manager(outdir):
    print("[host tests]")
    binary = os.path.join(outdir, "test_datapath")
    result = run(["xcrun", "clang++", "-std=c++14", "-Wall", "-Wextra",
                  "-Werror", "-I", os.path.join(ROOT, "AirportItlwm"),
                  TEST_CPP, "-o", binary])
    check(result.returncode == 0, "test_datapath.cpp builds with -Werror")
    if result.returncode != 0:
        print(result.stdout)
        return
    result = run([binary])
    check(result.returncode == 0, "all datapath host tests pass")
    check("ALL DATAPATH TESTS PASSED" in result.stdout,
          "test binary reports success")
    if result.returncode != 0:
        print(result.stdout)


def source_gates():
    print("\n[source gates]")
    hpp = open(DATAPATH_HPP).read()
    cpp = open(DATAPATH_CPP).read()
    awdl = open(AWDL_CPP).read()

    check(re.search(r"#define\s+ITLWM_AWDL_DATAPATH_LIVE\s+0", hpp) is not None,
          "ITLWM_AWDL_DATAPATH_LIVE defaults to 0")
    check(re.search(r"static_assert\(ITLWM_AWDL_DATAPATH_LIVE\s*==\s*0", cpp)
          is not None,
          "datapath backend asserts ITLWM_AWDL_DATAPATH_LIVE == 0")
    check(re.search(r"static_assert\(ITLWM_AWDL_REGISTRATION_ENABLED\s*==\s*0",
                    awdl) is not None,
          "AWDL interface asserts ITLWM_AWDL_REGISTRATION_ENABLED == 0")

    # The AWDL class must stay isolated from the Stage 4 manager. Comments may
    # name the files, so only includes and the C++ namespace count.
    check("#include \"AirportItlwmAWDLDatapath" not in awdl and
          "itlwm_awdl_datapath" not in awdl,
          "AWDL interface does not include or reference the datapath manager")

    # Fail-closed packet entry points: they may only zero the out-count.
    check(re.search(r"\*accepted\s*=\s*0", hpp) is not None,
          "transmit() zeroes the accepted count")
    check(re.search(r"\*received\s*=\s*0", hpp) is not None,
          "receive() zeroes the received count")
    check("kUnsupported" in hpp,
          "transmit()/receive() return kUnsupported when Ready")

    # No registration / attach / start token in the real backend.
    for token in ("registerEthernetInterface", "deregisterEthernetInterface",
                  "registerNetworkInterface", "initRegistrationInfo",
                  "deferBSDAttach"):
        check(token not in cpp,
              "datapath backend does not call %s" % token)


def datapath_object(outdir):
    print("\n[datapath object]")
    obj = compile_kernel_object(DATAPATH_CPP, outdir, "AirportItlwmAWDLDatapath")
    defined = nm(obj)
    undefined = nm(obj, undefined=True)

    for symbol in DATAPATH_DEFINED:
        check(symbol in defined, "defines %s" % symbol)

    missing = [s for s in FACTORY_SYMBOLS if s not in undefined]
    check(not missing,
          "imports all four 25G332 factory symbols (missing %s)"
          % (missing or "none"))

    forbidden = sorted(s for s in undefined
                       if any(t in s for t in FORBIDDEN_IMPORTS))
    check(not forbidden,
          "no registration/attach import (got %s)" % (forbidden or "none"))

    # Exactly four Skywalk imports: the pools and the three queues.
    skywalk = sorted(s for s in undefined if "IOSkywalk" in s)
    check(skywalk == sorted(FACTORY_SYMBOLS),
          "Skywalk imports are exactly the four factory symbols")


def awdl_isolation(outdir):
    print("\n[AWDL interface isolation]")
    obj = compile_kernel_object(AWDL_CPP, outdir, "AirportItlwmAWDLInterface")
    undefined = nm(obj, undefined=True)
    refs = sorted(s for s in undefined if "itlwm_awdl_datapath" in s)
    check(not refs,
          "AWDL interface object does not reference the datapath (got %s)"
          % (refs or "none"))


def kext_checks(args):
    if args.kext:
        print("\n[experimental kext]")
        defined = nm(args.kext)
        undefined = nm(args.kext, undefined=True)
        for symbol in DATAPATH_DEFINED:
            check(symbol in defined, "defines %s" % symbol)
        missing = [s for s in FACTORY_SYMBOLS if s not in undefined]
        check(not missing,
              "kext imports all four factory symbols (missing %s)"
              % (missing or "none"))

    if args.default_kext:
        print("\n[default kext]")
        defined = nm(args.default_kext)
        undefined = nm(args.default_kext, undefined=True)
        refs = sorted(s for s in defined | undefined
                      if "itlwm_awdl_datapath" in s)
        check(not refs,
              "default kext contains no datapath symbols (got %s)"
              % (refs or "none"))
        missing = [s for s in FACTORY_SYMBOLS if s in undefined]
        check(not missing,
              "default kext does not import the factory symbols (got %s)"
              % (missing or "none"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kext", help="experimental AirportItlwm.kext to inspect")
    parser.add_argument("--default-kext",
                        help="default AirportItlwm.kext to inspect")
    args = parser.parse_args()

    print("SDK: %s" % SDK)
    print("Source: %s" % DATAPATH_CPP)

    outdir = tempfile.mkdtemp(prefix="itlwm-awdl-datapath-")
    test_host_manager(outdir)
    source_gates()
    datapath_object(outdir)
    awdl_isolation(outdir)
    kext_checks(args)

    print("\n" + ("ALL CHECKS PASSED" if not failures
                  else "%d CHECK(S) FAILED" % len(failures)))
    print("Artifacts: %s" % outdir)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
