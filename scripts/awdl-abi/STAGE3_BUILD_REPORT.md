# Stage 3 build report — AWDL lifecycle skeleton

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

## Scope

Stage 3 adds the non-virtual lifecycle around the Stage 2 concrete class:

- initialization and ownership bookkeeping;
- start preparation and validation of a `RegistrationInfo`;
- stop, teardown and failure rollback;
- double-init, double-start, double-stop and double-free handling;
- reference-counted successful starts for the future live path.

It does **not** allocate or register an interface. `startLifecycle()` always
fails closed with `kIOReturnUnsupported` after preparing and validating the
buffer, and `ITLWM_AWDL_REGISTRATION_ENABLED` is fixed at `0` by a
`static_assert`.

## What was added

| File | Purpose |
|---|---|
| `AirportItlwm/AirportItlwmAWDLInterface.hpp` | Non-virtual lifecycle API, `ITLWM_AWDL_REGISTRATION_ENABLED` gate |
| `AirportItlwm/AirportItlwmAWDLInterface.cpp` | Side table, lock, state machine, `RegistrationInfo` builder, full rollback |
| `include/Airport/experimental/IO80211AWDLRegistrationInfo_25G332.h` | KC-decoded `RegistrationInfo` prefix and Ethernet layouts with static asserts |
| `scripts/awdl-abi/verify_awdl_class.py` | Adds `[stage 3 lifecycle]` symbol and forbidden-reference checks |

The class still has no data members, no new virtual methods, and is not
referenced by `setVIRTUAL_IF_CREATE`. The vtable stays at 571 entries and the
instance size stays `0x130`.

## Kernel-collection evidence

All addresses below are from the local `BootKernelExtensions.kc`.

### `IOSkywalkEthernetInterface::initRegistrationInfo`

`0xffffff8002b158e2`

- Requires `info != NULL`, `arg2 == 1`, `size == 0x130`.
- Zeroes `0x128` bytes from `info + 8`, then writes:
  - `+0x00` = `1` (version)
  - `+0x04` = `0x130` (size)
  - `+0x08` = `getInterfaceFamily()` or fallback `2`
  - `+0x0c` = `getInterfaceSubFamily()`
  - `+0x14` = `0x100`
  - `+0x24` = `0x22`
  - `+0x28` = `getInitialMedia()` or fallback `0x20`
  - `+0x30` = `getBSDNamePrefix()` or a default string
  - `+0x38` = `getBSDUnitNumber()`
  - `+0x44` = `getMinPacketSize()` or fallback `0x2e`
  - `+0x48` = `getMaxTransferUnit()` or fallback `0x5dc`
  - `+0x4c` = `getMaxTransferUnit()` or fallback `0x5dc`

### `IOSkywalkEthernetInterface::copyRegistrationInfo`

`0xffffff8002b159f0`

- Requires `version == 1` and `size >= 0x130`.
- Allocates and copies `0x130` bytes.
- Reads the MAC from `info + 0x108`, length `6`, and sets `IOMACAddress`.

### `IOSkywalkNetworkInterface::registerNetworkInterface`

`0xffffff8002b0f204`

- Requires `version == 1`, `size >= 0x108`, `info[0x14] <= 0x400`, a
  non-null logical link, and at least one non-null packet pool.
- Copies `0x108` bytes and consumes `info[0x24]` and `info[0x28]`.
- Does not read the MAC; the Ethernet wrapper has already stored it.

### Registration and deregistration wrappers

| Symbol | Address | Behaviour |
|---|---|---|
| `IOSkywalkEthernetInterface::registerEthernetInterface` | `0xffffff8002b15a84` | `copyRegistrationInfo()`, then `registerNetworkInterface()`; frees on failure |
| `IOSkywalkEthernetInterface::deregisterEthernetInterface` | `0xffffff8002b15af4` | Releases the Ethernet `0x130` copy after a successful network deregister |
| `IOSkywalkNetworkInterface::deregisterNetworkInterface` | `0xffffff8002b0ee9c` | Releases link, pools, ordered set and the `0x108` copy |

### `RegistrationInfo` layout

`include/Airport/experimental/IO80211AWDLRegistrationInfo_25G332.h` models:

- `NetworkRegistrationInfoPrefix`, exactly `0x108` bytes, the part copied by
  `registerNetworkInterface()`;
- `EthernetRegistrationInfo`, exactly `0x130` bytes, with the MAC at `+0x108`.

Static asserts pin `sizeof`, `offsetof` and the MAC offset, so a layout edit
fails the build rather than silently changing the buffer.

## Lifecycle design

### Why a side table

The 25G332 instance size is exactly `0x130`. Adding an ivar would change the
class ABI and invalidate the Stage 2 verification. Stage 3 therefore stores
state in a fixed, module-local table:

```c
enum LifecycleState {
    kLifecycleEmpty, kLifecycleInitialized, kLifecycleStarting,
    kLifecyclePrepared, kLifecycleRunning, kLifecycleStopping,
    kLifecycleTornDown,
};

struct LifecycleRecord {
    const AirportItlwmAWDLInterface *self;
    IO80211Controller *owner;   // weak
    uint32_t state;
    uint32_t startCount;
    bool holdsReference;
    uint8_t mac[6];
};
```

- Eight records (`kLifecycleRecordCount = 8`), keyed by object address.
- One lazily allocated `IOLock` protects the whole table.
- `IOLockAlloc` is raced with `OSCompareAndSwapPtr`; the loser frees its
  duplicate lock. The table itself is static, so no per-interface allocation
  occurs.
- The owner pointer is **weak**. The controller owns the interface and must
  outlive it; the interface never retains the controller, so no retain cycle
  is created.

### State transitions

| Call | Precondition | Effect | Failure behaviour |
|---|---|---|---|
| `initLifecycle(owner, mac)` | no record for `this`; `owner != NULL` | claims a record, stores owner/MAC, state `Initialized` | returns `false`; no record left behind |
| `startLifecycle()` | state `Initialized` | `Initialized -> Starting -> Prepared -> Initialized` | returns `kIOReturnUnsupported`, rolls back to `Initialized` |
| `startLifecycle()` | state `Running` | increments `startCount` | returns `kIOReturnSuccess` |
| `startLifecycle()` | any other state | rejected | `kIOReturnNotOpen` / `kIOReturnBusy` |
| `stopLifecycle()` | state `Running`, `startCount > 1` | decrements `startCount` | returns success |
| `stopLifecycle()` | state `Running`, `startCount == 1` | `Running -> Stopping -> Initialized` | returns success |
| `stopLifecycle()` | `Initialized` / `TornDown` | no-op | returns success (idempotent) |
| `stopLifecycle()` | `Starting` / `Prepared` / `Stopping` | rejected | `kIOReturnBusy` |
| `teardownLifecycle()` | record exists | clears owner/MAC, releases the slot, state `TornDown` | safe to call twice |
| `isLifecycleInitialized()` | record in any live state | returns `true` | `false` otherwise |
| `isLifecycleRunning()` | state `Running` | returns `true` | `false` otherwise |

### `startLifecycle()` sequence

1. Take the table lock, require state `Initialized`, set `Starting`, release.
2. If the runtime boot-arg gate is off, return `kIOReturnNotPermitted`.
3. Build a local `alignas(8)` `0x130`-byte buffer via
   `buildRegistrationInfo()`.
4. Re-check the invariants the KC enforces: version `1`, size `0x130`,
   `maxSize <= 0x400`.
5. Set `Prepared`, then stop: return `kIOReturnUnsupported`.
6. Roll back `Starting`/`Prepared` to `Initialized`, clearing `startCount` and
   `holdsReference`.

No pool, queue, registration, retain or persistent MAC state survives a
failed start.

### `buildRegistrationInfo()`

- Requires a buffer of at least `0x130` bytes and a live lifecycle record.
- Rejects an all-zero or multicast MAC (`mac[0] & 1`).
- Zeroes the buffer and writes the KC-derived fallbacks:
  version `1`, size `0x130`, family `2`, subfamily `0`, `maxSize 0x100`,
  reserved `0x22`, initial media `0x20`, min packet size `0x2e`, MTU `0x5dc`.
- Copies the six MAC bytes to `+0x108`.
- Leaves `bsdNamePrefix` NULL. Neither `copyRegistrationInfo()` nor
  `registerNetworkInterface()` reads it before the Ethernet registration
  step, but a live path must replace it with a real prefix.

## Build

### Default (gate off)

```sh
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Release \
  SYMROOT=/private/var/folders/fx/_yf5j3p100s7k6y2gznmxmrm0000gn/T/opencode/awdl-build/default \
  build
```

Result: `** BUILD SUCCEEDED **`. The `.cpp` compiles to an empty translation
unit; `nm` finds **0** `AirportItlwmAWDLInterface` symbols in the kext.

### Experimental (gate on)

```sh
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Release \
  SYMROOT=/private/var/folders/fx/_yf5j3p100s7k6y2gznmxmrm0000gn/T/opencode/awdl-build/experimental \
  OTHER_CPLUSPLUSFLAGS='$(inherited) -DITLWM_AWDL_EXPERIMENTAL=1' \
  build
```

Result: `** BUILD SUCCEEDED **`. The kext contains the vtable, metaclass, the
64 Stage 2 methods and the 7 lifecycle methods:

```
__ZN25AirportItlwmAWDLInterface13initLifecycleEP17IO80211ControllerRK10ether_addr
__ZN25AirportItlwmAWDLInterface14startLifecycleEv
__ZN25AirportItlwmAWDLInterface13stopLifecycleEv
__ZN25AirportItlwmAWDLInterface17teardownLifecycleEv
__ZNK25AirportItlwmAWDLInterface21buildRegistrationInfoEPvm
__ZNK25AirportItlwmAWDLInterface22isLifecycleInitializedEv
__ZNK25AirportItlwmAWDLInterface18isLifecycleRunningEv
```

The side table and lock are module-local (`__ZN12_GLOBAL__N_1...`,
non-external), so they do not leak into the kext symbol namespace.

## Verification

### `verify_awdl_class.py`

```sh
SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk \
python3 scripts/awdl-abi/verify_awdl_class.py \
  --kext /private/var/folders/fx/_yf5j3p100s7k6y2gznmxmrm0000gn/T/opencode/awdl-build/experimental/Release/Tahoe/AirportItlwm.kext \
  --default-kext /private/var/folders/fx/_yf5j3p100s7k6y2gznmxmrm0000gn/T/opencode/awdl-build/default/Release/Tahoe/AirportItlwm.kext
```

`ALL CHECKS PASSED`.

| Check | Result |
|---|---|
| vtable entries | 571 |
| full listing entries | 571 |
| `___cxa_pure_virtual` left | 0 |
| every function slot 0..567 populated | yes |
| slot 488 | `uintptr_t AirportItlwmAWDLInterface::setMacAddress(ether_addr&)` |
| slots 505..567 | 63/63 exact names and parameter types |
| unexpected overrides | none |
| DWARF `DW_AT_byte_size` | `0x130` |
| expected mangled method symbols | 64/64 |
| lifecycle methods defined | 7/7 |
| direct registration / pool / queue references | none |
| `gMetaClass` and vtable | defined |
| experimental kext | all 64 methods, 7 lifecycle methods, metaclass, vtable |
| default kext | no class symbols |

The forbidden-reference check rejects any undefined symbol starting with
`IOSkywalkEthernetInterface::registerEthernetInterface`,
`initRegistrationInfo`, `deregisterEthernetInterface`,
`IOSkywalkPacketBufferPool` or `IOSkywalkQueueSet`. The object imports none
of them.

### `verify_abi.py`

```sh
python3 scripts/awdl-abi/verify_abi.py
```

`ALL CHECKS PASSED`: 508-entry VirtualInterface vtable, 571-entry AWDL
vtable, 63 slots at 505..567, and a `Mach-O 64-bit kext bundle` link.

### `verify_slots_kc.py`

```sh
python3 scripts/awdl-abi/verify_slots_kc.py
```

`ALL SLOT CHECKS PASSED`: `IO80211AWDLProtocol` 571 entries,
`AppleBCMWLANProximityInterface` 574 entries, **63/63** slots verified.

### Strict link against the local KC

```sh
python3 ~/Desktop/WTLKcAbi/linkcheck.py \
  /private/var/folders/fx/_yf5j3p100s7k6y2gznmxmrm0000gn/T/opencode/awdl-build/experimental/Release/Tahoe/AirportItlwm.kext \
  --baseline /private/var/folders/fx/_yf5j3p100s7k6y2gznmxmrm0000gn/T/opencode/awdl-build/default/Release/Tahoe/AirportItlwm.kext
```

Result:

```
AirportItlwm.kext: 1082 undefined symbols, collection BootKernelExtensions.kc
  com.apple.driver.corecapture                    6  ok
  com.apple.iokit.IO80211Family                 334  ok
  com.apple.iokit.IONetworkingFamily            219  ok
  com.apple.iokit.IOPCIFamily                     6  ok
  com.apple.iokit.IOSkywalkFamily                81  ok
  com.apple.kernel                              436  ok
link check ok
```

The Stage 2 experimental kext had 1077 undefined symbols. Stage 3 adds five,
all exported by `com.apple.kernel`:

| Symbol | KC address |
|---|---|
| `_IOLockAlloc` | `0xffffff8000a88f10` |
| `_IOLockFree` | `0xffffff8000a88f80` |
| `_IOLockLock` | `0xffffff80004cceb0` |
| `_IOLockUnlock` | `0xffffff80004cd1e0` |
| `_OSCompareAndSwapPtr` | `0xffffff8000a1b8c0` |

### Header isolation

With `ITLWM_AWDL_EXPERIMENTAL=0`, the preprocessed translation unit contains
**0** occurrences of `IO80211AWDLProtocol` and **0** of
`IO80211AWDLRegistrationInfo`.

With `ITLWM_AWDL_EXPERIMENTAL=1`, it uses only the experimental ABI chain,
contains **0** occurrences of the legacy
`include/Airport/IO80211VirtualInterface.h`, and includes the new
`RegistrationInfo` header.

### Project integrity

- `plutil -lint itlwm.xcodeproj/project.pbxproj` reports `OK`.
- Only `AirportItlwmAWDLInterface.cpp` is added to the Tahoe Sources phase.
- `setVIRTUAL_IF_CREATE` still returns `kIOReturnUnsupported` for role 6,
  writes no `bsd_name`, and does not reference the Stage 3 class.
- AWDL is still absent from `CARD_CAPABILITIES`.

## Invariants preserved

- `ITLWM_AWDL_EXPERIMENTAL` defaults to `0`.
- `ITLWM_AWDL_REGISTRATION_ENABLED` defaults to `0`; `static_assert` rejects
  `1` until a registration path exists.
- `startLifecycle()` cannot reach `Running`; no code path returns success
  from a start.
- No Apple base `init`, packet pool, queue set, link, registration or
  deregistration API is called.
- No fake TX or RX result is produced.
- No interface is allocated, attached, registered or published.
- No kext installed, no EFI/SIP change, no boot-arg change, no reboot.

## Conditions required before enabling registration

Registration must stay disabled until **all** of the following are
implemented, reviewed and verified against 25G332:

1. **Real base initialization.** The interface must be allocated through the
   correct IOKit path and its `IO80211VirtualInterface`/`IOSkywalkEthernetInterface`
   base `init` must run with the real owner and dispatch queue. Stage 3 does
   not call it.
2. **Packet pools.** TX and RX `IOSkywalkPacketBufferPool` instances with a
   reviewed `PoolOptions` (`packetCount`, `bufferCount`, `bufferSize`,
   `maxBuffersPerPacket`, `memorySegmentSize`, `poolFlags`) and a defined
   ownership/release order.
3. **Packet queues.** Queue allocation and `IOSkywalkQueueSet` wiring, plus
   the `registerEthernetInterface` queue array, count and ordering.
4. **Complete `RegistrationInfo`.** A real `bsdNamePrefix`, unit number,
   interface family/subfamily, media and MTU taken from the controller rather
   than the KC fallbacks, and a verified MAC source.
5. **Link state machine.** `bringupLink`/`bringdownLink` semantics and the
   logical-link object that `registerNetworkInterface` requires.
6. **Sync and channel configuration.** AWDL sync parameters, master/secondary
   channel, election state and the 63 AWDL methods that must return live
   values instead of `kIOReturnUnsupported`.
7. **Start ordering and rollback.** The reviewed sequence
   `base init -> pools -> queues -> initRegistrationInfo -> registerEthernetInterface
   -> enable/setInterfaceEnable`, with a rollback at every step.
8. **Stop ordering.** Deregister the Ethernet interface, drain and release
   queues, release pools, bring down the link, then release the base object —
   the reverse of start.
9. **Locking and lifetime.** A reviewed locking model that does not hold the
   side-table lock across Apple calls, plus a rule for who owns the final
   `release()`.
10. **Runtime gate.** `itlwm_awdl=1`, a compiled-in experimental build, and a
    deliberate decision to expose `awdl0`.
11. **KC drift check.** Re-run `verify_abi.py`, `verify_slots_kc.py`,
    `verify_awdl_class.py` and `linkcheck.py` on the target build, and update
    the experimental headers if any vtable, size or symbol changes.
12. **Explicit review.** Remove the `static_assert` and the unconditional
    `kIOReturnUnsupported` return only in a separate, reviewed change.

## Unresolved blockers

- **No live registration path.** `Running` is unreachable; `startLifecycle()`
  always returns `kIOReturnUnsupported`.
- **No pool/queue/link objects.** The KC requires at least one packet pool and
  a non-null logical link; Stage 3 creates neither.
- **`bsdNamePrefix` is NULL.** A real registration needs a valid
  `OSString`/C-string prefix and unit number.
- **Inherited return types remain an ABI assumption.** Itanium mangling does
  not encode return types; only the 32-bit `eax` width of the 63 AWDL
  overrides is verified.
- **Payload structs are opaque.** The `apple80211_*` layouts are not checked
  against the 25G332 KC, so the stubs must not dereference them.
- **Single-controller assumption.** The side table is keyed by object address
  and holds eight records; a multi-interface design needs a reviewed capacity
  and lifetime rule.
- **No hardware validation.** Nothing in Stage 3 has run on the AX211; all
  evidence is static (build, DWARF, symbols, KC disassembly).
