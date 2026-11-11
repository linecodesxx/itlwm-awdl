# Stage 2 build report — native AWDL interface class

Date: 2026-11-11

## Environment

| Item | Value |
|---|---|
| OS | macOS 26.7.2, build `25G332` |
| Xcode | 26.5 (17F42) |
| SDK | `MacOSX26.5.sdk` |
| Kernel collection | `/System/Library/KernelCollections/BootKernelExtensions.kc` |
| KC SHA-256 | `96c12f421a8e99580b5015dfd39877df38ecde9b9e056b8ccd28f15847bca35c` |
| Wi-Fi | Intel AX211, PCI `8086:51F1` |
| Repo | AirportItlwm, base `7204c69` + Stage 1.5 `07443d4` |
| Target | `AirportItlwm-Tahoe`, Release, x86_64 |

## What was added

| File | Purpose |
|---|---|
| `AirportItlwm/AirportItlwmAWDLInterface.hpp` | Concrete `IO80211AWDLProtocol` subclass; 64 overrides; size/abstract static asserts |
| `AirportItlwm/AirportItlwmAWDLInterface.cpp` | IOKit metaclass + 64 fail-closed stubs; empty TU when the gate is off |
| `scripts/awdl-abi/verify_awdl_class.py` | Compiles the real class and checks vtable, size and symbols |
| `itlwm.xcodeproj/project.pbxproj` | Adds the `.cpp` to the Tahoe Sources phase only |

The class adds no data members, no new virtual methods, and is not referenced
by `setVIRTUAL_IF_CREATE`. `ITLWM_AWDL_EXPERIMENTAL` remains `0` by default.

## Build

### Default (gate off)

```sh
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Release \
  SYMROOT=/tmp/awdl-default \
  build
```

Result: `** BUILD SUCCEEDED **`. The `.cpp` compiles to an empty translation
unit. `nm` finds **0** `AirportItlwmAWDLInterface` symbols in the kext.

### Experimental (gate on)

```sh
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Release \
  SYMROOT=/tmp/awdl-experimental \
  OTHER_CPLUSPLUSFLAGS='$(inherited) -DITLWM_AWDL_EXPERIMENTAL=1' \
  build
```

Result: `** BUILD SUCCEEDED **`. The kext contains the vtable, metaclass and
all 64 method symbols.

## Verification

### `verify_abi.py`

```sh
python3 scripts/awdl-abi/verify_abi.py
```

`ALL CHECKS PASSED`. Confirms the 25G332 base-chain anchors (`enable` 266,
`disable` 267, `initBSDInterfaceParameters` 284, `setMacAddress` 488), the
571-entry AWDL vtable, the 63 slots at 505..567, and a kext link.

### `verify_slots_kc.py`

```sh
python3 scripts/awdl-abi/verify_slots_kc.py
```

`ALL SLOT CHECKS PASSED`: `IO80211AWDLProtocol` 571 entries,
`AppleBCMWLANProximityInterface` 574 entries, **63/63** slots verified with
bridge + disassembly, evidence table regenerated.

### `verify_awdl_class.py`

```sh
SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk \
python3 scripts/awdl-abi/verify_awdl_class.py \
  --kext /tmp/awdl-experimental/Release/Tahoe/AirportItlwm.kext \
  --default-kext /tmp/awdl-default/Release/Tahoe/AirportItlwm.kext
```

`ALL CHECKS PASSED`.

| Check | Result |
|---|---|
| vtable entries | 571 |
| full listing entries | 571 |
| `___cxa_pure_virtual` left | 0 |
| every function slot 0..567 populated | yes |
| slot 488 | `uintptr_t AirportItlwmAWDLInterface::setMacAddress(ether_addr&)` |
| slots 505..567 | 63/63 exact names and parameter types from evidence TSV |
| AWDL slots contiguous and ordered | 505..567 |
| unexpected overrides | none |
| DWARF `DW_AT_byte_size` | `0x130` |
| expected mangled method symbols | 64/64 |
| `gMetaClass` | defined |
| `__ZTV25AirportItlwmAWDLInterface` | defined |
| experimental kext | all 64 methods + `gMetaClass` + vtable present |
| default kext | no class symbols |

The vtable's `VTable indices` block contains exactly the class destructors,
`getMetaClass`, `setMacAddress`, and the 63 AWDL overrides. No pure slot is
left, so the class is concrete.

### Strict link against the local KC

```sh
python3 ~/Desktop/WTLKcAbi/linkcheck.py \
  /tmp/awdl-experimental/Release/Tahoe/AirportItlwm.kext \
  --baseline /tmp/awdl-default/Release/Tahoe/AirportItlwm.kext
```

Result:

```
AirportItlwm.kext: 1077 undefined symbols, collection BootKernelExtensions.kc
  com.apple.driver.corecapture                    6  ok
  com.apple.iokit.IO80211Family                 334  ok
  com.apple.iokit.IONetworkingFamily            219  ok
  com.apple.iokit.IOPCIFamily                     6  ok
  com.apple.iokit.IOSkywalkFamily                81  ok
  com.apple.kernel                              431  ok
link check ok
```

The default baseline has 955 undefined symbols. The extra 122 undefined
symbols are the base-chain/metaclass/vtable/constructor references introduced
by the class; the 64 stubs themselves are defined in the kext. All 1077 are
exported by bundles already declared in `Info.plist`.

### Header collisions and project integrity

- `plutil -lint itlwm.xcodeproj/project.pbxproj` reports `OK`.
- The `.cpp` appears in the `AirportItlwm-Tahoe` Sources phase only.
- With `ITLWM_AWDL_EXPERIMENTAL=0`, the preprocessed TU contains **0**
  occurrences of `IO80211AWDLProtocol`.
- With `ITLWM_AWDL_EXPERIMENTAL=1`, it includes only the experimental
  `IO80211AWDLProtocolABI_25G332.h` chain. The legacy
  `include/Airport/IO80211VirtualInterface.h` is not included.

## Return types and ABI risks

- The 63 AWDL slots were verified in Stage 1.5 to write a 32-bit value in
  `eax` before `ret`. The header declares `IOReturn`, which is ABI-safe for
  that width, but Itanium mangling does not encode return types, so the exact
  source-level type is still an assumption.
- The inherited VIF/Skywalk methods keep the experimental header's
  reference-derived return types (`uintptr_t`, and `bool` for `init`). A wrong
  inherited return type would be an ABI mismatch the compiler cannot catch.
- `setMacAddress` is declared `uintptr_t` by the experimental VIF header and
  returns `static_cast<uintptr_t>(kIOReturnUnsupported)`. The real Proxi
  override writes a 32-bit code to `r14d` and returns `mov eax, r14d`, so the
  Stage 2 stub's zero-extended 32-bit unsupported code is valid for both
  `eax`-reading and `rax`-reading callers.
- The class is a compile/link artifact. Loading the experimental kext
  registers its IOKit metaclass (normal `OSDefineMetaClassAndStructors`
  behaviour), but no instance is allocated and no IO80211 interface is
  registered.
- The vtable and size checks are tied to 25G332. A future kernel collection
  that changes the AWDL vtable would invalidate the experimental headers and
  this class together.
- The payload structs (`apple80211_*`) are only used as opaque pointers. The
  stubs never dereference them.

## Non-goals preserved

- No allocation, attach, registration, start or advertisement of an AWDL
  interface.
- No connection from `setVIRTUAL_IF_CREATE` to the new class.
- No firmware or hardware access in any stub.
- No EFI change, no kext installation, no SIP change, no boot-arg change, no
  reboot.
