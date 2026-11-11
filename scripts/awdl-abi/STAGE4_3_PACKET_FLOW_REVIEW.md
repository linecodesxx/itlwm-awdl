# Stage 4.3 — host-only TX/RX packet ownership model

Date: 2026-11-11  
Base: passing Stage 4.2 source branch  
Target: macOS Tahoe 26.7.2 (25G332), Intel AX211

## Intent

We need to distinguish these events, which are **not interchangeable**:

1. Packet offered to a submission callback: the network stack/queue still owns it.
2. Driver accepts/claims a packet: ownership must be tracked.
3. Driver completes or drops a packet through the actual Skywalk queue/pool contract.
4. Skywalk acknowledges the packet's return and allows its storage to be recycled.

A callback's positive return value is the **count accepted from the submission
queue**, not the number actually transmitted on the radio. Without a verified
TX completion/drop path, returning nonzero would risk loss of ownership.

## Implemented (pure C++14, no IOKit)

- `AirportItlwm/AirportItlwmAWDLPacketFlow.hpp`: `Ownership<Capacity>`.
- `claimBatch(...)`: rejects null/duplicate pointers, invalid path, too-small
  output, over-capacity requests and already-owned packet addresses. The batch
  is **all-or-nothing**, with no allocation.
- Tickets contain a slot index and 64-bit generation. Late callbacks carrying
  an **old ticket** are rejected even if a packet pointer is reused.
- `finalize(ticket, Completed|Dropped)`: bookkeeping only. The caller must
  first have executed the real Skywalk completion/drop; this method does not.
- `acknowledgeReturn(ticket)`: bookkeeping only, valid only after finalize
  AND after independent external confirmation of the pool return.
- `invariantHolds()`: detects incorrect internal counts, null ownership and
  duplicated active addresses.
- `scripts/awdl-abi/test_packet_flow.cpp`: tests invalid inputs, atomic
  batch rejection, full-capacity rollback, exactly-once finalization,
  cross-TX/RX duplication, delayed stale-generation completions, out-of-order
  completions, and slot reuse after explicit acknowledgement.
- `verify_datapath.py`: compiles the new host test with `-Werror`, runs it
  and confirms that **no production AWDL source includes this prototype**.

This model is **owner-serialized**, NOT lock-free or generally thread-safe.
Its generation checks only protect paths that actually carry the original
ticket; callback paths using packet pointers alone are not automatically safe.
Generation exhaustion permanently retires that slot instead of wrapping.

## Deliberately NOT implemented

- No integration with the experimental kext or the existing Stage 4
  `PacketLedgerT`; the new model is an isolated host prototype.
- No real packet allocation, transmit, completion, drop, pool return, or
  fabricated return count.
- No entry into Skywalk callbacks: the Stage 4.2 `CallbackGate` is still
  **never armed** in the driver, and `ITLWM_AWDL_DATAPATH_LIVE == 0`.
- No AWDL attach, registration, `awdl0`, firmware access, EFI modification,
  installation or restart.

## Important constraints pending local KC disassembly

- `IOSkywalkTxCompletionQueue`: which exact caller owns each packet before
  and after enqueue/complete, and which invocation allows final release?
- `IOSkywalkRxCompletionQueue`: what are callback arguments/return
  semantics and who owns packets when networking accepts them?
- `IOSkywalkTxSubmissionQueue` and the real bus driver's teardown: which
  operation prevents **all future** callback entries and ensures in-flight
  calls have exited before backend/refCon destruction?
- Concrete PacketPool configuration, queue identifiers and packet types for
  the AX211 controller.
- Scheduling/serialization of the future packet owner.

Apple's public NetworkingDriverKit documentation describes a similar
submission/completion lifecycle, but those are `IOUserNetwork*` classes.
It is **not** proof of the private `IOSkywalk*` behavior in the 25G332 KC.

## Verification status

**Host-side verification PASS (developer-reported, 2026-11-11).**
`python3 scripts/awdl-abi/verify_datapath.py` built both C++14 host test
binaries with `-Werror`; all existing datapath and new packet-flow tests
passed. Source gates, isolation checks and the experimental backend object's
four 25G332 Skywalk factory imports also passed; final output:
`ALL CHECKS PASSED`.

This Stage 4.3 check does **not** by itself verify a fresh default/experimental
full Xcode build, KC ABI slot checks or linkcheck on this branch. The
predecessor Stage 4.2 did pass the previously supplied experimental build,
ABI checks and linkcheck; see `STAGE4_2_SAFETY_REVIEW.md`.
The production AWDL datapath is still not connected or enabled.

```sh
cd ~/Desktop/itlwm
python3 scripts/awdl-abi/verify_datapath.py
python3 scripts/awdl-abi/verify_awdl_class.py
python3 scripts/awdl-abi/verify_abi.py
python3 scripts/awdl-abi/verify_slots_kc.py
```

The proposed Stage 4.3 exit criterion is passing host tests and
an independently reviewed ownership model, **not** enabling runtime AWDL.
