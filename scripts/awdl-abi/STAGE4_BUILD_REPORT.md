# Stage 4 build report — AWDL datapath resource manager

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
| Repo | AirportItlwm, base `7204c69` + Stage 1.5 `07443d4` + Stage 3 `5f36c7f` |
| Target | `AirportItlwm-Tahoe`, Release, x86_64 |

## Scope

Stage 4 adds a **separate, experimental datapath resource manager** for the
future AWDL interface. It does not add ivars or virtuals to
`AirportItlwmAWDLInterface`, so the verified instance size (`0x130`) and
vtable (571 entries) are unchanged.

The manager owns five Skywalk resources:

1. TX packet pool;
2. RX packet pool;
3. TX submission queue;
4. TX completion queue;
5. RX completion queue.

It implements allocation, explicit state transitions, reverse-order teardown
and reverse-order failure rollback. It does **not** register, attach, start,
enable, transmit, receive, complete or drop a packet. `startLifecycle()` still
returns `kIOReturnUnsupported`; `Running` is unreachable; `setVIRTUAL_IF_CREATE`
still fails closed; AWDL is still absent from `CARD_CAPABILITIES`.

Stage 4 also re-audited the Stage 3 lifecycle and fixed three concurrency
issues:

- `initLifecycle()` now takes exactly one retain on the interface and
  `teardownLifecycle()` drops exactly that retain. This prevents the object
  from being freed and its address reused while the side-table record still
  points at it.
- `stopLifecycle()` no longer drops the lifecycle retain.
- `teardownLifecycle()` defers clearing the record (`teardownPending`) while a
  start/stop transition is between its two lock windows; the transition
  finishes the teardown and releases exactly once.

`clearRecordLocked()` centralizes the “clear `self` and report whether to
release” step, so the side-table slot is freed exactly once.

## 25G332 evidence

All addresses below are from the local BootKernelExtensions.kc.

### Packet pools and queues

| Symbol | Address |
|---|---|
|---|---|
| `IOSkywalkPacketBufferPool::withName` | `0xffffff8002b09be8` |
| `IOSkywalkPacketBufferPool::initWithName` | `0xffffff8002b09c88` |
| `IOSkywalkTxSubmissionQueue::withPool` | `0xffffff8002b11a86` |
| `IOSkywalkTxSubmissionQueue::initWithPool` | `0xffffff8002b11aac` |
| `IOSkywalkTxCompletionQueue::withPool` | `0xffffff8002b17860` |
| `IOSkywalkTxCompletionQueue::initWithPool` | `0xffffff8002b179cc` |
| `IOSkywalkRxCompletionQueue::withPool` | `0xffffff8002b13e04` |
| `IOSkywalkRxCompletionQueue::initWithPool` | `0xffffff8002b13eae` |
| `IOSkywalkPacketQueue::initWithPool` | `0xffffff8002b0dbc8` |

### `PoolOptions` layout

`initWithName` reads the options structure at:

| Offset | Field | Evidence |
|---|---|---|
| `+0x00` | `packetCount` | `mov eax, [r14]` |
| `+0x04` | `bufferCount` | `mov eax, [r14 + 4]` |
| `+0x08` | `bufferSize` | `mov eax, [r14 + 8]` |
| `+0x0c` | `maxBuffersPerPacket` | `mov eax, [r14 + 0xc]` |
| `+0x10` | `memorySegmentSize` | `mov eax, [r14 + 0x10]` |
| `+0x14` | `poolFlags` | `mov eax, [r14 + 0x14]` |

The manager pins this with `sizeof(PoolOptions) == 0x20` and per-field
`static_assert`s.

### Queue minimum capacities

| Queue | KC check | Minimum |
|---|---|---|
| TX submission | `cmp ecx, 0x8; jb fail` | 8 |
| TX completion | `cmp edx, 0x2; setb fail` | 2 |
| RX completion | `cmp edx, 0x2; setb fail` | 2 |

`Config` validation rejects smaller values before calling the backend.

### RegistrationInfo corrections

Stage 4 re-read `IOSkywalkEthernetInterface::initRegistrationInfo`
(`0xffffff8002b158e2`), `IOSkywalkNetworkInterface::getTxDataOffset`
(`0xffffff8002b10aba`), `AppleBCMWLANProximityInterface::start`
(`0xffffff800172d3a6`) and
`AppleBCMWLANProximityInterface::getBSDNamePrefix`
(`0xffffff8001719aac`).

| Field | Offset | Stage 3 value | Stage 4 value | Evidence |
|---|---|---|---|---|
| headroom | `+0x3c` (`uint16_t`) | missing / reserved | `0` | base init zeroes it; Proxi `start()` does not patch it; `getTxDataOffset` reads a `uint16_t` |
| MTU | `+0x48` | `0x5dc` (1500) | `0x594` (1428) | Proxi `start()` writes both MTU fields |
| MTU 2 | `+0x4c` | `0x5dc` | `0x594` | same |
| BSD prefix | `+0x30` | `NULL` | pointer to `"awdl"` | `getBSDNamePrefix()` returns the literal |
| media | `+0x28` | `0x20` | `0x20` | base fallback; Proxi does not patch |
| min packet | `+0x44` | `0x2e` | `0x2e` | base fallback; Proxi does not patch |

`bsdNamePrefix` is now `uintptr_t`, matching the pointer-sized store. The
prefix points at a static string literal, so it stays valid for the lifetime
of the buffer. `startLifecycle()` re-checks the MTU, headroom and non-null
prefix before failing closed.

## Ownership model

```
future AWDL owner / controller
  |
  +-- itlwm_awdl_datapath::Backend (virtual interface)
  |     |
  |     +-- SkywalkBackend (experimental build only)
  |           owner_ = controller (weak)
  |
  +-- itlwm_awdl_datapath::Manager (plain C++, not refcounted)
        |
        +-- [0] TxPool            (owned)
        +-- [1] RxPool            (owned)
        +-- [2] TxSubmissionQueue (owned, references TxPool)
        +-- [3] TxCompletionQueue (owned, references TxPool)
        +-- [4] RxCompletionQueue (owned, references RxPool)
```

Ownership rules:

- The manager owns exactly one reference to each non-null slot.
- A queue references its pool but does not own it; the pool is released only
  after every queue that references it has been destroyed.
- The backend must outlive the manager.
- The owner must call `teardown()` before destroying the manager. The
  destructor still releases any non-null slot, but that is a safety net, not
  the intended path.
- The manager has no internal lock. The owner must serialize `initialize`,
  `teardown` and any future packet calls.

Allocation order:

```
TxPool -> RxPool -> TxSubmissionQueue -> TxCompletionQueue -> RxCompletionQueue
```

Destruction order:

```
RxCompletionQueue -> TxCompletionQueue -> TxSubmissionQueue -> RxPool -> TxPool
```

## State machine

| State | Meaning | Allowed next |
|---|---|---|
| `Idle` | constructed, nothing configured | `initialize` |
| `Allocating` | resources being created | `Ready` or `Failed` |
| `Ready` | all five slots non-null | `teardown` |
| `TearingDown` | reverse release in progress | `TornDown` |
| `TornDown` | released, config cleared | `initialize` |
| `Failed` | a create failed and rollback completed | `teardown` or `initialize` |

Rules enforced in code:

- `initialize()` returns `kBusy` from `Ready`, `Allocating` or `TearingDown`.
- `initialize()` returns `kInvalidArgument` before touching the backend when
  the config is invalid.
- `isReady()` is true only in `Ready` **and** when every slot is non-null.
- `teardown()` from `Idle`/`TornDown` is idempotent and releases nothing.
- `teardown()` from `Allocating`/`TearingDown` returns `kBusy`.
- `teardown()` from `Failed` is a no-op transition because rollback already
  released everything.
- `transmit()`/`receive()` always zero the out-count; they return `kNotReady`
  when not `Ready` and `kUnsupported` when `Ready`. They never claim a packet
  was transmitted or received.

## Rollback matrix

The host test forces each of the five creates to fail in turn.

| Failing create | Resources acquired | Destroy sequence | Result state |
|---|---|---|---|
| TX pool | none | — | `Failed` |
| RX pool | TX pool | `a` | `Failed` |
| TX submission | TX pool, RX pool | `b a` | `Failed` |
| TX completion | + TX submission | `c b a` | `Failed` |
| RX completion | + TX completion | `d c b a` | `Failed` |

Legend: `a` = TX pool, `b` = RX pool, `c` = TX submission, `d` = TX
completion. No pool is destroyed while a queue that references it still
exists. After every failure, all five slots are null and `teardown()` releases
nothing further.

## Implementation

### `AirportItlwm/AirportItlwmAWDLDatapath.hpp`

Pure C++14, no IOKit dependency:

- `State`, `Resource`, `PoolOptions`, `Config`, `Status`, `TxAction`;
- `Backend` abstract interface;
- `Manager` with inline implementation, so the host test does not need a
  kernel build;
- `PacketLedgerT<Capacity>`: fixed-capacity exactly-once accounting model for
  Stage 5. It rejects duplicate consume, double complete, double drop,
  complete-after-drop and drop-after-complete.
- `static_assert`s for pool layout, resource count/order, state values and
  queue minimums.

### `AirportItlwm/AirportItlwmAWDLDatapath.cpp`

Experimental build only:

- `static_assert(ITLWM_AWDL_DATAPATH_LIVE == 0, ...)`;
- real backend with exact 25G332 `asm` symbol names for the four factories;
- callbacks typed as `uint32_t` (`unsigned int` in the KC mangling), not
  `IOReturn` (`int`);
- fail-closed callbacks return `0` and never claim transmission;
- live helpers are emitted with `__attribute__((noinline, used))` so the
  strict link check resolves the exact symbols, but the backend methods return
  `NULL` while the gate is `0`; the helpers are never called in Stage 4;
- the backend currently installs its own zero-return callbacks and ignores the
  manager's `TxAction`; Stage 5 must bridge that deliberately;
- `destroyQueue()` calls `disable()` then `release()`, matching
  `AppleBCMWLANProximityInterface::freeResources()`; `destroyPool()` calls
  `release()`.

### `scripts/awdl-abi/test_datapath.cpp`

Fake backend; no IOKit, no kext. Covers:

- initial state and empty slots;
- successful allocation order `12345` and teardown order `edcba`;
- double initialize and double teardown;
- all five rollback steps;
- invalid names, zero pool fields, capacities below the KC minimum and
  duplicate queue IDs;
- fail-closed `transmit()`/`receive()` before and after `Ready`, including
  NULL out-pointers;
- exactly-once ledger behaviour.

## Build results

Default (gate off, datapath is an empty TU):

```sh
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Release \
  -derivedDataPath .../awdl-build/default build
```

Result: `** BUILD SUCCEEDED **`. The default kext contains **0**
`itlwm_awdl_datapath` symbols and does not import the four factory symbols.

Experimental (gate on):

```sh
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Release \
  -derivedDataPath .../awdl-build/experimental \
  OTHER_CPLUSPLUSFLAGS='$(inherited) -DITLWM_AWDL_EXPERIMENTAL=1' build
```

Result: `** BUILD SUCCEEDED **`. The experimental kext defines:

```
__ZN19itlwm_awdl_datapath20createSkywalkBackendEPv
__ZN19itlwm_awdl_datapath21destroySkywalkBackendEPNS_7BackendE
```

and imports exactly the four pool/queue factories.

## Verification results

### Host tests

```sh
xcrun clang++ -std=c++14 -Wall -Wextra -Werror \
  -I AirportItlwm scripts/awdl-abi/test_datapath.cpp -o /tmp/test_datapath
/tmp/test_datapath
```

Result: `ALL DATAPATH TESTS PASSED`.

### `verify_datapath.py`

```sh
python3 scripts/awdl-abi/verify_datapath.py \
  --kext .../awdl-build/experimental/Build/Products/Release/Tahoe/AirportItlwm.kext \
  --default-kext .../awdl-build/default/Build/Products/Release/Tahoe/AirportItlwm.kext
```

Result: `ALL CHECKS PASSED`. It verifies:

| Check | Result |
|---|---|
| host test builds with `-Werror` and passes | pass |
| `ITLWM_AWDL_DATAPATH_LIVE` defaults to `0` | pass |
| `ITLWM_AWDL_DATAPATH_LIVE == 0` static assert | pass |
| `ITLWM_AWDL_REGISTRATION_ENABLED == 0` static assert | pass |
| AWDL interface does not include/reference the manager | pass |
| datapath object defines both factory functions | pass |
| datapath object imports exactly the four Skywalk factories | pass |
| datapath object imports no registration/attach entry point | pass |
| AWDL interface object has no datapath reference | pass |
| experimental kext contains the manager and the four imports | pass |
| default kext contains no manager symbols and no factory imports | pass |

### ABI / vtable / slot checks

| Script | Result |
|---|---|
| `verify_awdl_class.py` | `ALL CHECKS PASSED` — 571 entries, size `0x130`, 64/64 symbols, 63/63 slots |
| `verify_abi.py` | `ALL CHECKS PASSED` — probes and kext link |
| `verify_slots_kc.py` | `ALL SLOT CHECKS PASSED` — 63/63, Proxi 574 entries |

### Strict link check

```sh
python3 ~/Desktop/WTLKcAbi/linkcheck.py \
  .../awdl-build/experimental/Build/Products/Release/Tahoe/AirportItlwm.kext \
  --baseline .../awdl-build/default/Build/Products/Release/Tahoe/AirportItlwm.kext
```

Result:

```
AirportItlwm.kext: 1086 undefined symbols, collection BootKernelExtensions.kc
  com.apple.driver.corecapture                    6  ok
  com.apple.iokit.IO80211Family                 334  ok
  com.apple.iokit.IONetworkingFamily            219  ok
  com.apple.iokit.IOPCIFamily                     6  ok
  com.apple.iokit.IOSkywalkFamily                85  ok
  com.apple.kernel                              436  ok
link check ok
```

Stage 3 had 1082 undefined symbols; Stage 4 adds the four Skywalk factories
(`IOSkywalkFamily` goes from 81 to 85).

## Unresolved runtime risks

These cannot be proved without loading the kext and exercising a real
controller:

1. **Pool sizing.** `Config` is caller-supplied. The correct packet counts,
   buffer sizes, segment sizes and flags for AWDL on the AX211 are not derived
   from the controller yet.
2. **Queue ID semantics.** The manager only rejects duplicate IDs. The KC
   does not document what the queue ID selects or whether a specific range is
   required.
3. **Callback ABI.** The `uint32_t` callback return is inferred from the KC
   mangling (`j`) rather than the SDK typedef (`IOReturn`, `int`). The host
   test does not execute a real callback. The real backend currently ignores
   the manager's `TxAction` and installs its own zero-return
   `txSubmissionAction`; a live path must bridge or replace that callback
   deliberately.
4. **`withName` packet type.** The live helper passes `packetType = 0`. The
   accepted values and the owner requirements are not verified.
5. **`disable()`/`release()` ordering.** Copied from
   `AppleBCMWLANProximityInterface::freeResources()`. Stage 4 never enables a
   queue, so the enabled-path teardown is untested.
6. **Thread safety.** `Manager` has no lock. The owner must serialize calls;
   concurrent `initialize`/`teardown` would race on `state_` and the slots.
7. **Backend allocation failure.** `createSkywalkBackend()` uses `new`; the
   caller must check for `NULL`. The kernel build disables exceptions.
8. **Owner lifetime.** `SkywalkBackend::owner_` is weak. The controller must
   outlive the manager and every queue.
9. **Unload safety.** If a future live path leaves a queue registered, the
   manager cannot force the kernel to drain it. Stage 4 does not register, so
   this is deferred.
10. **Registration semantics.** The corrected `RegistrationInfo` matches the
    Proxi patches, but `initRegistrationInfo()` was not called on a live
    object, so the base fallbacks (family/subfamily/media) are still assumed.
11. **KC drift.** All addresses and minimums are tied to 25G332.

## Stage 5 readiness

Stage 4 is a complete, host-tested ownership/rollback layer, but it is **not**
ready to be wired in by flipping `ITLWM_AWDL_DATAPATH_LIVE`. Before Stage 5:

1. Choose real `PoolOptions` and queue IDs from the controller and document
   where each value comes from.
2. Decide the owner object and its lifetime; the manager and backend must not
   outlive the controller.
3. Add serialization at the owner boundary (or make the manager lock-aware).
4. Integrate `PacketLedgerT` into every future consume/complete/drop path so
   each packet is returned exactly once.
5. Implement the registration path from `STAGE3_BUILD_REPORT.md` and prove
   `startLifecycle()` can reach `Running` only after pools, queues,
   registration, link state and sync/channel setup all succeed.
6. Keep `setVIRTUAL_IF_CREATE` fail-closed until the live path is reviewed
   end to end.

Recommended Stage 5 shape: keep `ITLWM_AWDL_DATAPATH_LIVE == 0` as the
default, add a second explicit runtime gate for the live datapath, and test
the registration/teardown path on a disposable machine before enabling AWDL
advertisement.

## Deliberately not done

- No registration, attach, start or hardware TX/RX.
- No interface allocated or advertised; `awdl0` is not created.
- `ITLWM_AWDL_REGISTRATION_ENABLED` remains `0`.
- `ITLWM_AWDL_DATAPATH_LIVE` remains `0`.
- `ITLWM_AWDL_EXPERIMENTAL` remains default OFF.
- `setVIRTUAL_IF_CREATE` still returns `kIOReturnUnsupported` for role 6.
- AWDL remains absent from `CARD_CAPABILITIES`.
- No kext installed, no EFI/SIP/boot-arg changes, no reboot.
