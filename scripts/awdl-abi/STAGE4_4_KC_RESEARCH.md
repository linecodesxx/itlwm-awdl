# Stage 4.4 — read-only 25G332 Skywalk queue contract investigation

**Branch:** `research/awdl-stage4-4-skywalk-kc`  
**Status:** investigation tooling only; no experimental kext code changed

## What is already verified

On 2026-11-11 the developer ran the Stage 4.3 `verify_datapath.py`:
`ALL DATAPATH TESTS PASSED`, `ALL PACKET FLOW TESTS PASSED`,
and overall `ALL CHECKS PASSED`, including all source gates and
the four experimental Skywalk factory imports. These are host and static
checks, not live network tests.

## What Stage 4.4 must prove before programming real callbacks

The currently accepted Skywalk factory signatures are linked against
the local 25G332 Kernel Collection, but the live behavior is not yet proven.

1. TX submission callback: verify the exact ring consumer path, and whether a
   positive callback count transfers ownership to the driver. Assume zero
   until exactly-once TX completion/drop is demonstrated.
2. TX completion: trace `IOSkywalkTxCompletionQueue::enqueuePackets` and its
   callees to the final pool return/packet completion, including error
   branches, disabled queues and batch behavior. Record when external return
   may be acknowledged by the host-only ownership model.
3. RX completion: trace `IOSkywalkRxCompletionQueue` handoff, callback
   signature/return value and pool ownership, including rejection/drop.
4. Teardown: inspect queue disable/close, gate/drain and release paths plus
   caller synchronization. Showing that a counter reaches zero is not enough
   to establish that a callback cannot be invoked later.
5. Inspect relevant `IOSkywalkPacketBufferPool` free/return behavior.
   The Stage 4 `PacketLedgerT` and Stage 4.3 `Ownership` classes must remain
   host-only until these operations are verified.

## Local evidence collector

Use the read-only script, which depends on the existing
`~/Desktop/WTLKcAbi/kcfile.py` and `disasm.py`. It enumerates method
symbols in the KC and optionally disassembles a small, targeted set.

```bash
cd ~/Desktop/itlwm
python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py --list-only
```

Then collect relevant methods (run only if the list shows matching symbols):

```bash
python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \
  --methods legacyDequeue,enqueuePackets,completePackets,disable,enable \
  --max-functions 18 --max-lines 400 \
  --out /tmp/awdl-skywalk-kc-evidence.txt
```

The saved file is **local evidence only**: do not commit, publish or upload
it to the public GitHub repository. Share only relevant excerpts for review.
The output is not proof of ownership until all control paths and callers
have been checked by a developer familiar with the 25G332 kernel ABI.

## Hard safety gates

`ITLWM_AWDL_REGISTRATION_ENABLED == 0`,
`ITLWM_AWDL_DATAPATH_LIVE == 0`, and default experimental mode off.
No AWDL object allocation, network registration, firmware TX/RX, EFI changes,
kext installation or reboot in this branch.


## Initial local discovery (developer-reported, 2026-11-11)

The first read-only collection completed successfully on the local
`/System/Library/KernelCollections/BootKernelExtensions.kc`:

- `--list-only` identified **46** queue/pool symbols matching the collector
  method filter (some entries are overloads and allocation views).
- A targeted run with `legacyDequeue,enqueuePackets,completePackets,disable,enable`
  disassembled **13/13 matched methods** without collector errors.
- Evidence was saved **locally** at
  `/tmp/awdl-skywalk-kc-evidence.txt` (not committed to GitHub).

| Method | 25G332 local address | Disassembly lines |
|---|---|---:|
| `IOSkywalkTxSubmissionQueue::legacyDequeue()` | `0xffffff8002b31860` | 83 |
| `IOSkywalkTxCompletionQueue::enqueuePackets(... const*, ...)` | `0xffffff8002b32bb6` | 55 |
| `IOSkywalkTxCompletionQueue::enqueuePackets(..., ...)` | `0xffffff8002b32c6e` | 114 |
| `IOSkywalkRxCompletionQueue::enqueuePackets(... const*, ...)` | `0xffffff8002b32766` | 54 |
| `IOSkywalkRxCompletionQueue::enqueuePackets(..., ...)` | `0xffffff8002b32810` | 117 |
| `IOSkywalkTxCompletionQueue::completePackets(...)` | `0xffffff8002b17b9e` | 84 |
| `IOSkywalkTxSubmissionQueue::disable()` | `0xffffff8002b11cd6` | 23 |
| `IOSkywalkTxCompletionQueue::disable()` | `0xffffff8002b17b58` | 24 |
| `IOSkywalkRxCompletionQueue::disable()` | `0xffffff8002b14040` | 23 |
| `IOSkywalkPacketQueue::disable()` | `0xffffff8002b0de34` | 15 |
| `IOSkywalkTxSubmissionQueue::enable()` | `0xffffff8002b11c76` | 29 |
| `IOSkywalkTxCompletionQueue::enable()` | `0xffffff8002b17b2c` | 14 |
| `IOSkywalkRxCompletionQueue::enable()` | `0xffffff8002b13fe0` | 30 |

The symbol list also includes `free()` for all three specialized queues
and `IOSkywalkPacketQueue` plus the pool, as well as `checkForWork()`
and `packetSubmission(...)`. These are important follow-up paths.

**Interpretation limit:** This establishes symbol presence and disassembly
availability only, NOT any queue-drain guarantee, callback lifetime,
completion/drop ownership or correct runtime behavior. The full local
disassembly is still required for control-flow review. All production
runtime gates remain off.

## First control-flow interpretation (from the local 13-function dump)

See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md) for the
method-by-method analysis. Key findings:

- TX `legacyDequeue` consumes the callback return as a **ring advancement
  count**; returning zero stalls dequeue, and a returned count greater than
  offered is not clamped within the shown path.
- TX completion array overload calls `completePackets`, which invokes a
  batch virtual at `[*(queue+0x50)]+0x148`. The linked-list overload instead
  uses `+0x140`. Targets and return semantics still require vtable mapping.
- RX array path selects `enqueuePacketsForNetworking` or
  `enqueuePacketsForKPipe`; the linked-list path calls
  `_kern_netif_queue_rx_enqueue` and does not inspect its return.
- TX/RX pointer-array `enqueuePackets` check `count < *(queue+0xac)`;
  the meaning of the field is not yet independently verified.
- Specialized `disable()` functions **reopen the event-source gate** after
  base disabling. This cannot by itself prove no-future-callback entry,
  draining or safe release of `refCon`.

### Next read-only collection

On the current research branch:

```bash
python3 scripts/awdl-abi/inspect_skywalk_vtables.py
python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \
  --methods free,enqueuePacketsForNetworking,enqueuePacketsForKPipe,resetABFS \
  --max-functions 15 --max-lines 450 \
  --out /tmp/awdl-skywalk-remaining-evidence.txt
```

These scripts are read-only. The vtable mapping will help identify the
batch/single packet return operations. Share results privately for review;
do not commit proprietary disassembly to GitHub. Kernel behavior remains
unproven and all AWDL runtime gates stay off.

## Second local method discovery and virtual mapping (2026-11-11)

The developer ran the local vtable inspector and a second read-only disassembly
collection. The vtable map verified:

- `IOSkywalkPacket::getDataLength()` at vptr `+0x140`,
  `completeWithQueue()` at `+0x198`, and `disposePacket()` at `+0x1e0`.
- `IOSkywalkPacketBufferPool::deallocatePacket()` at vptr `+0x140`
  and `deallocatePackets()` at `+0x148`.
- Consequently, the already-reviewed TX completion array code calls
  `deallocatePackets()` after individual `completeWithQueue()` operations;
  the linked-list TX overload calls `deallocatePacket()` per packet.
  The actual return-to-pool failure semantics must still be verified.

**Important vtable correction:** `disable()` wrappers call through a table
pointer loaded from the raw `__ZTV20IOSkywalkPacketQueue` symbol, not an
object vptr. The raw table has a 16-byte metadata header. Thus the
`__ZTV+0x168` indirect call is equivalent to object vptr `+0x158` (slot 43),
**not** the `getWorkLoop()` at vptr `+0x168` (slot 45). The inspector
now includes `+0x158` in its default selection. That target has since been confirmed locally as
`IOSkywalkPacketQueue::disable()` (vptr slot 43, address
`0xffffff8002b0de34`).

The additional disassembler run matched 14 methods and produced nonempty
bodies for specialized and base `free()` methods, both RX array enqueue
paths, and `resetABFS()`. Its content was saved at
`/tmp/awdl-skywalk-remaining-evidence.txt` on the developer's Mac.
**Only its symbol/count output has been provided**; the underlying second
disassembly has not yet been reviewed, so no lifetime or error-path claim
is justified from it.

Next: review that second file and run:

```sh
python3 scripts/awdl-abi/inspect_skywalk_vtables.py \
  --classes IOSkywalkPacketQueue,IOSkywalkTxSubmissionQueue,IOSkywalkTxCompletionQueue,IOSkywalkRxCompletionQueue \
  --offsets 0x158
```

See `STAGE4_4_KC_ANALYSIS.md` for details. No runtime gates changed.


## Second disassembly control-flow review (developer-uploaded file, 2026-11-11)

The actual `awdl-skywalk-remaining-evidence.txt` was reviewed.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md) section 9.

**Established from the code:**

- `TxSubmissionQueue::free` tears down its packet table and metadata; its
  displayed specialized body does **not** explicitly drain callbacks.
- `TxCompletionQueue::free` calls `this->vptr+0x1c0` **before** freeing
  storage and lock. This remains a priority unknown. Field `+0xac` is
  used as a pointer-array storage length; its meaning at initialization
  still needs checking.
- `RxCompletionQueue::free` destroys packet table and support objects;
  base queue free releases its pool reference.
- Pool free cancels **its own** deferred thread call with
  `_thread_call_cancel_wait`, then releases all packets and memory segments.
  This does not establish no-future-entry into the TX submission callback.
- RX Networking builds native `kern_packet_t` chains and sends them to
  `_kern_netif_queue_rx_enqueue`; the wrapper is disposed beforehand.
  This may be legitimate distinct ownership, but must be verified.
- RX KPipe operates on packet-table state and calls
  `RxCompletionQueue::sendNotification`, without the Networking enqueue.
- `resetABFS` resets adaptive queue fields, **not** callback lifetime.

These observations are relevant to eventual ownership accounting, but
they do not justify enabling runtime queues.

### Targeted next commands (read-only)

```sh
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes IOSkywalkPacketQueue,IOSkywalkTxCompletionQueue \\
  --offsets 0x90,0x158,0x1c0

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --methods deallocatePacket,deallocatePackets,completeWithQueue,disposePacket,releaseAllPackets,releaseAllMemorySegments \\
  --max-functions 15 --max-lines 350 \\
  --out /tmp/awdl-skywalk-packet-lifetime.txt
```

The new collector method filter now covers `IOSkywalkPacket` as well as
the queue and pool classes. The local file should be shared privately,
not committed as full disassembly.


## Finalize virtual and pool-lifetime discovery (developer-reported, 2026-11-11)

The corrected vptr inspection confirmed `IOSkywalkPacketQueue::finalize()` at object-vptr `+0x1c0` (`0xffffff8002b0ddcc`) and specialized `IOSkywalkTxCompletionQueue::finalize()` at the same slot (`0xffffff8002b17d6e`). Therefore, the virtual call preceding storage free in `TxCompletionQueue::free()` targets its specialized `finalize()`. **It is not yet proven to wait for callbacks.**

The read-only packet-lifetime collector matched **7** entries: `deallocatePacket` (27 lines), `deallocatePackets` (51), `completeWithQueue` (53), `disposePacket` (29), `releaseAllPackets` (27), `releaseAllMemorySegments` (19), and a zero-line allocator type view. File `/tmp/awdl-skywalk-packet-lifetime.txt` has not been provided yet, so only symbol presence and successful extraction are confirmed.

The collector now also accepts exact method name `finalize`. Inspect the queue finalize implementations independently:

```sh
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc
python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --methods finalize \\
  --max-functions 10 --max-lines 350 \\
  --out /tmp/awdl-skywalk-finalize.txt
```

Do not add extracted proprietary disassembly to public GitHub; share relevant details privately. No production kext sources or runtime gates changed.

## Finalize symbol extraction completed (developer-reported, 2026-11-11)

Read-only collector command with `--methods finalize --max-functions 10
--max-lines 350` completed successfully against local
`BootKernelExtensions.kc` (25G332). Exactly five methods were matched:

| Method | Reported disassembly lines |
|---|---:|
| `IOSkywalkTxSubmissionQueue::finalize()` | 32 |
| `IOSkywalkTxCompletionQueue::finalize()` | 30 |
| `IOSkywalkRxCompletionQueue::finalize()` | 33 |
| `IOSkywalkPacketQueue::finalize()` | 35 |
| `IOSkywalkPacket::finalize()` | 23 |

Local output: `/tmp/awdl-skywalk-finalize.txt`.

The full files have subsequently been reviewed; see the dedicated [Stage 4.4 packet-finalize report](STAGE4_4_PACKET_FINALIZE_REVIEW.md). Those methods establish real native TX packet pool-free calls but **do not** establish callback quiescence or safe deletion of callback context. No runtime gates or driver source were changed.


## Full packet-lifetime and finalize review (developer-uploaded, 2026-11-11)

Both previously requested evidence files are now available and reviewed:
`awdl-skywalk-packet-lifetime.txt` and `awdl-skywalk-finalize.txt`.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md), section 11,
for the evidence-based control-flow analysis.

**Confirmed behavior:**

- `deallocatePacket` tail-calls `_kern_pbufpool_free`.
  `deallocatePackets` changes the passed pointer array from wrapper
  pointers into native handles before calling `_kern_pbufpool_free_batch`.
- `completeWithQueue` invokes `_kern_packet_finalize` in mode 2;
  `disposePacket` disposes buffers and clears the wrapper's native handle
  without itself returning that handle through `_kern_pbufpool_free`.
- Base `IOSkywalkPacketQueue::finalize` decrements the field at `+0xbc`,
  invokes a still-unidentified virtual at `+0x18`, optionally calls pool
  `checkOutPacketQueue`, and returns `0xe00002d5` when the tracked count
  remains nonzero.
- `TxCompletionQueue::finalize` treats base `0xe00002d5` as a successful
  return **without** resetting its completion state. Its `free()` calls
  this finalize before freeing memory, but that alone is **not** evidence
  of callback quiescence.
- `TxSubmissionQueue::finalize` calls vptr `+0x2a0` on base success;
  `RxCompletionQueue::finalize` calls
  `IOSkywalkPacketTable::flushAllPackets` on base success.
- All specialized finalize wrappers reopen the `IOEventSource` gate.
  No proven no-future-callback lifetime boundary has been found.

**Consequence:** native TX pool return is directly evidenced, but the
safety condition for releasing the embedded `TxCallbackContext` is **still
not verified**. This is not permission to enable AWDL or reuse the host-only
ownership ledger in a live kernel packet path.

### Next read-only evidence

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes IOSkywalkPacketQueue,IOSkywalkTxSubmissionQueue,IOSkywalkPacket,IOSkywalkPacketBuffer \\
  --offsets 0x18,0x148,0x1a0,0x2a0

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --methods checkOutPacketQueue,flushAllPackets,disposePacketBuffer \\
  --max-functions 12 --max-lines 450 \\
  --out /tmp/awdl-skywalk-next-lifetime.txt
```

Send the printed vtable mapping and the new local text file for review.
The evidence should remain outside public GitHub.

**No production kext files changed; experimental gates stay off.**


## Full packet and finalize review completed

Both uploaded evidence files have been reviewed: [STAGE4_4_PACKET_FINALIZE_REVIEW.md](STAGE4_4_PACKET_FINALIZE_REVIEW.md).

The critical outstanding safety issue is proving callback quiescence before freeing the TX submission queue's `refCon`. No `finalize()` body proves that. Additionally, TX completion's `finalize()` masks base code `0xe00002d5` as success while skipping some state cleanup. The observed native TX pool-return calls are `_kern_pbufpool_free` and `_kern_pbufpool_free_batch`, after wrapper disposal. RX's native network handoff is separate.

Next evidence: `IOSkywalkPacketTable::flushAllPackets`, `IOSkywalkPacketBufferPool::completePreparedPacket`/`checkOutPacketQueue`, and vptr offsets `IOSkywalkPacket::0x1a0`, `IOSkywalkPacketBuffer::0x148`, `TxSubmissionQueue::0x2a0`.
