# Stage 4.2 — callback admission and teardown audit (source-review branch)

**Target:** macOS Tahoe 26.7.2, build `25G332`, Intel AX211.  
**Based on:** passing Stage 4.1 branch `fix/awdl-stage4-1-lifecycle-reginfo`.  
**Scope:** callback accounting and host-side safety tests, **NOT** runtime AWDL.

## Changes

- Added `CallbackGate`, a no-STL, C++14 atomic admission counter, and RAII `CallbackLease` to `AirportItlwmAWDLDatapath.hpp`. The gate starts **disarmed**; there are **no driver calls to `arm()`**. Only RAII leases may enter/leave (counter mutation is private).
- `txSubmissionAction` requires successful gate admission before forwarding the manager's `TxAction`. In the experimental driver **all admissions fail**; it always returns 0 and never consumes packets.
- The TX submission queue destroy path closes admission before `disable()` and `release()`. The backend destructor also closes the gate defensively.
- The existing `TxCallbackContext` stays inside `SkywalkBackend` and is passed by pointer as `refCon`, preserving the exact verified `25G332` callback ABI.
- Host tests exercise disarmed, armed, closing, denied-entry, admitted-in-flight, drained, never-rearm states; a concurrent close with one outstanding lease; and reentrant `Manager::initialize`/`teardown` calls made by a fake backend during allocation and release.
- The host verifier links with `-pthread` and statically checks that the real backend does not arm its callback gate.

## Critical distinction: close is NOT queue quiescence

The gate tracks **already-admitted** callbacks. Once closed, it refuses additional admissions. However, a queue that still holds `refCon` may invoke a callback **after** the backend and gate have been freed, resulting in use-after-free *before* admission can even be checked. Thus:

1. Before any live use, identify the exact documented/verified mechanism by which `IOSkywalkTxSubmissionQueue` stops entering its callback.
2. Separately prove the TX submission callback has returned from all invocations and that **no further entry** is possible after the registration/disable/deregister sequence.
3. Only then may `SkywalkBackend` and its embedded callback context be destroyed.
4. `CallbackGate::drained() == true` alone **does not** establish conditions (1)-(3). Do not introduce a kernel busy-wait or infer a drain guarantee from `disable()` without evidence.

The manager is also still owner-serialized, **not** generally thread-safe. The reentrancy tests prove `kBusy` during nested state changes, not concurrent data-race freedom.

## TX/RX packet contract: still not proven

The 25G332 evidence from Stage 4.1 identified:

- `IOSkywalkTxSubmissionQueue` callback type returns an **unsigned 32-bit count of consumed packets**. It does **not** mean "successfully transmitted"; returning a positive count without taking ownership and arranging completion/drop leaks packets.
- `IOSkywalkTxCompletionQueue` has a separate completion path. The KC evidence currently covers part of `completePackets`, but not every completion callback invocation or release edge.
- RX completion callbacks and packet ownership transfer still require analysis in the local 25G332 KC.
- The Stage 4 `PacketLedgerT` remains a host-only accounting model; no kernel TX/RX path consumes, completes, drops or releases a packet.

Consequently, the production submission callback is **never admitted**, the completion/RX callbacks remain non-operative and all runtime gates remain closed.

## Invariants and verification requirements

- `AirportItlwmAWDLInterface` remains `0x130` bytes with the same 571-entry vtable.
- Default `ITLWM_AWDL_EXPERIMENTAL=0`; static asserts enforce `ITLWM_AWDL_REGISTRATION_ENABLED == 0` and `ITLWM_AWDL_DATAPATH_LIVE == 0`.
- No new `registerEthernetInterface`, attach/start, firmware TX/RX, `awdl0`, EFI or boot-argument changes.
- **Status:** source changes pushed to this branch. The independent callback-gate C++14 fragment compiled in a host smoke check; the **full updated repository test suite and Tahoe builds have not yet been run**.

### Run on the Mac

```sh
cd ~/Desktop/itlwm
python3 scripts/awdl-abi/verify_datapath.py
python3 scripts/awdl-abi/verify_awdl_class.py
python3 scripts/awdl-abi/verify_abi.py
python3 scripts/awdl-abi/verify_slots_kc.py

xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Debug -derivedDataPath /tmp/awdl42-default \
  ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build

xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Debug -derivedDataPath /tmp/awdl42-experimental \
  ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO \
  OTHER_CPLUSPLUSFLAGS='$(inherited) -DITLWM_AWDL_EXPERIMENTAL=1' build

python3 scripts/awdl-abi/verify_datapath.py \
  --kext /tmp/awdl42-experimental/Build/Products/Debug/Tahoe/AirportItlwm.kext \
  --default-kext /tmp/awdl42-default/Build/Products/Debug/Tahoe/AirportItlwm.kext

python3 ~/Desktop/WTLKcAbi/linkcheck.py \
  /tmp/awdl42-experimental/Build/Products/Debug/Tahoe/AirportItlwm.kext
```

No kext install or reboot is needed. Do **not** enable registration or live datapath even if all static tests pass.

## Next blockers

1. Verify full `TxCompletionQueue` and `RxCompletionQueue` callback and packet return semantics against the local 25G332 KC.
2. Prove queue callback quiescence/no-further-entry before backend lifetime ends; encode that requirement in the future registration teardown path.
3. Establish owner locking and drain guarantees, plus correct PoolOptions and queue IDs.
4. Integrate actual packet ownership accounting with complete/drop only after a real packet path exists.
