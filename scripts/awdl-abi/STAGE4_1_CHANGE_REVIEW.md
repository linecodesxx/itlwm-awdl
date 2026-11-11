# Stage 4.1 — review branch (NOT build-verified)

Branch: `fix/awdl-stage4-1-lifecycle-reginfo`  
Base: `feature/awdl-skywalk-ax211` / `0a8f495`

## Changes prepared

1. **Lifecycle race:** `stopLifecycle()` no longer clears a deferred teardown record or drops its retain. Only the in-flight `startLifecycle()` / `stopLifecycle()` transition completion may clear it. A concurrent stop now returns `kIOReturnBusy`.
2. **RegistrationInfo:** 25G332 `AppleBCMWLANCore::createProxInterface()` evidence applied to the isolated, non-registered buffer: subfamily `3`; media capacity `2`; active/current media `0x80`; prefix `awdl`; headroom `64` at `+0x3c`; default AWDL option `4` at `+0x40`; MTU `1428` and min packet `0x2e`. Option `6` depends on a power-state condition and is **not** dynamically implemented.
3. **TX callback bridge:** `SkywalkBackend` retains the manager's `TxAction` in backend-owned callback context and passes it as `refCon` to the known 25G332 single-callback queue factory. The exact callback type returns `uint32_t`; dispatch is bounded by the offered batch count. Existing manager action remains fail-closed (returns `0`).
4. **Host tests:** `test_datapath.cpp` now checks callback argument forwarding, null callbacks, zero batches and rejection of a return count larger than the batch.

## Still blocked

- Runtime callback-to-backend lifetime ordering and callback drain before teardown are **not** proved. The backend context is a borrowed pointer from the queue.
- TX completion/drop and RX ownership are not implemented; the callback cannot accept packets for actual transmission.
- Real power-state branch for RegistrationInfo `+0x40` remains unimplemented.
- PoolOptions/queue IDs and full Skywalk registration lifecycle remain unreviewed.
- No runtime AWDL object creation, attach, registration, advertisement or actual packet transfer has been enabled.

## Gates that must remain closed

- `ITLWM_AWDL_EXPERIMENTAL` defaults to `0`.
- `ITLWM_AWDL_REGISTRATION_ENABLED == 0` is enforced.
- `ITLWM_AWDL_DATAPATH_LIVE == 0` is enforced.
- `setVIRTUAL_IF_CREATE` continues to return unsupported.

## Validation status

See user-reported results below. No kext was installed or executed in the kernel. Do not install it.

Run on the x86_64 macOS Tahoe build machine:

```sh
python3 scripts/awdl-abi/verify_datapath.py
python3 scripts/awdl-abi/verify_awdl_class.py
python3 scripts/awdl-abi/verify_abi.py
python3 scripts/awdl-abi/verify_slots_kc.py
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Debug ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO build
xcodebuild -project itlwm.xcodeproj -scheme AirportItlwm-Tahoe \
  -configuration Debug ARCHS=x86_64 CODE_SIGNING_ALLOWED=NO \
  OTHER_CPLUSPLUSFLAGS='$(inherited) -DITLWM_AWDL_EXPERIMENTAL=1' build
```

After an experimental build, run `WTLKcAbi/linkcheck.py` against the actual experimental `AirportItlwm.kext` and 25G332 KC. Require both builds and all ABI checks to pass before merging into the feature branch.

## Local macOS verification (user-reported, 2026-11-11)

The developer ran the following on their Tahoe development machine after pulling this branch:

- Experimental `AirportItlwm-Tahoe` x86_64 build: **BUILD SUCCEEDED**.
- `python3 scripts/awdl-abi/verify_datapath.py`: **ALL CHECKS PASSED**, including host C++ tests built with `-Werror`.
- `python3 scripts/awdl-abi/verify_awdl_class.py`: **ALL CHECKS PASSED** (571 vtable entries, 0x130 instance, all 64 methods defined).
- `python3 scripts/awdl-abi/verify_abi.py`: **ALL CHECKS PASSED**.
- `python3 scripts/awdl-abi/verify_slots_kc.py`: **ALL SLOT CHECKS PASSED**, 63/63.
- `WTLKcAbi/linkcheck.py` on the actual `Build/Products/Debug/Tahoe/AirportItlwm.kext`: **link check ok** against `BootKernelExtensions.kc`, 1085 unresolved imports all exported by declared bundles (corecapture 6, IO80211Family 334, IONetworkingFamily 219, IOPCIFamily 6, IOSkywalkFamily 85, kernel 435).

This confirms source-level, build-time, test-time and external symbol checks; it does not validate a loaded kext or safe execution in the kernel. Runtime queue callback drain, completion/drop ownership, and concurrent teardown behavior remain open and must be reviewed before enabling registration or datapath.
