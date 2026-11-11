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


## Checkout, flush and buffer disposal reviewed (2026-11-11)

The developer pasted the locally disassembled bodies of
`IOSkywalkPacketBufferPool::checkOutPacketQueue`,
`IOSkywalkPacketTable::flushAllPackets`, and
`IOSkywalkPacketBuffer::disposePacketBuffer`. See
[STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md), section 12.

- `checkOutPacketQueue` takes a recursive pool lock and updates
  queue-related collections; pool checkout is **not** a proven workloop
  callback-quiescence guarantee.
- `flushAllPackets` cancels **per-packet** completion callbacks,
  passes pending packet-wrapper segments to pool
  `deallocatePackets`, and advances the table read index.
  The pool deallocation call returns native packets via
  `_kern_pbufpool_free_batch`.
- `disposePacketBuffer` zeroes wrapper fields; it does not directly
  return a packet to a native pool.
- None of these functions proves that the TX submission callback's
  `refCon` cannot be used after the owning backend is released.

The read-only collector now recognizes the named
`cancelCompletionCallback`, `setCompletionCallback`, and
`checkInPacketQueue` methods. Run the next local checks (with `cat`
to inspect the saved output):

```sh
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes IOSkywalkPacketQueue,IOSkywalkTxSubmissionQueue,IOSkywalkPacket,IOSkywalkPacketBuffer \\
  --offsets 0x18,0x148,0x1a0,0x2a0

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --methods cancelCompletionCallback,setCompletionCallback,checkInPacketQueue \\
  --max-functions 15 --max-lines 300 \\
  --out /tmp/awdl-skywalk-callback-lifetime.txt

cat /tmp/awdl-skywalk-callback-lifetime.txt
```

If the collector lists fewer than three methods, that is not itself
a failure: only symbols actually present in this KC will appear.
Do not commit raw proprietary disassembly, and do not enable AWDL.



## Vtable, packet cancellation and check-in results (2026-11-11)

The latest local inspection mapped the base queue's `vptr+0x18`
to `OSObject::getRetainCount()`, TX submission `+0x2a0` to
`purgePackets()`, packet `+0x1a0` to `complete()`, and packet
buffer `+0x148` to `completeWithQueue()`. See
[STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md), section 13.

The next local disassembly (`awdl-skywalk-callback-lifetime.txt`,
pasted by the developer) showed:

- `IOSkywalkPacket::cancelCompletionCallback()` only stores
  null in `packet+0x38`.
- `IOSkywalkPacketBuffer::cancelCompletionCallback()` only
  stores null in `buffer+0x48`.
- Neither function is a synchronization barrier for a callback that
  was previously admitted or captured the old pointer.
- `IOSkywalkPacketBufferPool::checkInPacketQueue()` creates
  an `OSSet` if needed, registers the queue under the pool's
  recursive lock and, if the queue has no interface and insertion
  succeeded, schedules `_thread_call_enter` **after unlocking**.
  This proves the presence of a deferred scheduling path, not a
  no-future-TX-callback guarantee.
- Base queue `finalize()` both mutates queue field `+0xbc` and
  separately uses `getRetainCount()`; these are distinct values.

### Next bounded inspection (read-only; show with cat)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes OSSet --offsets 0x1c0,0x1d8,0x1e0

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --methods purgePackets,complete,completeWithQueue \\
  --max-functions 12 --max-lines 350 \\
  --out /tmp/awdl-skywalk-purge-complete.txt

cat /tmp/awdl-skywalk-purge-complete.txt
```

The new script filter understands exact method names
`purgePackets` and `complete`. The output may include
more than one matching overload/class; only the three target
method bodies are needed for initial review. Do not upload
proprietary disassembly to a public GitHub repository.

**Current invariant:** Native packet pool return is evidenced,
but safe refCon lifetime and complete callback quiescence
are **not proven**. Keep `ITLWM_AWDL_DATAPATH_LIVE=0` and
AWDL registration disabled. No runtime code changes made.



## TX purge, OSSet mapping and completion routing (2026-11-11)

The developer provided local vtable names and the six matching function
bodies in `awdl-skywalk-purge-complete.txt`. See
[STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md), section 14.

- `OSSet` virtual offsets `0x1c0`, `0x1d8`, `0x1e0`
  are `setObject`, `removeObject`, `containsObject`.
  They identify the pool's queue registration collection operations.
- `IOSkywalkTxSubmissionQueue::purgePackets` under an existing
  workloop gate flushes pending ring entries via
  `PacketTable::flushAllPackets`, then separately cancels
  per-packet callbacks and deallocates entries linked from
  `queue+0x108`; optionally invokes `_ifnet_purge` and
  reopens the gate.
- `IOSkywalkPacket::complete` is flag-sensitive and can
  tail-call `completeWithQueue`; the packet-buffer
  `completeWithQueue` path can invoke an object callback
  stored at `buffer+0x48`. Simply clearing callback pointers
  is not a callback-draining synchronization operation.
- Native pool return and queued TX cleanup are both evidenced.
  The remaining safety issue is proving **no future Skywalk callback
  entry** after TX Submission Queue teardown, plus draining all
  already-admitted references to `TxCallbackContext`.

### Next read-only check: submission callback entry and registration

The collector now has a `--classes` filter to avoid unrelated
RX/TX completion queue methods. Run this next (including the
requested `cat`):

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkTxSubmissionQueue \\
  --methods initWithPool,checkForWork,packetSubmission \\
  --max-functions 12 --max-lines 450 \\
  --out /tmp/awdl-skywalk-tx-entry.txt

cat /tmp/awdl-skywalk-tx-entry.txt
```

The collected code can help determine callback/refCon storage,
how queued TX work is scheduled and what queue states are tested
before invoking the registered callback. It is **not** a substitute
for tracing the owner-side workloop removal and queue lifetime.
Never post raw KC disassembly to the public repository.

**No changes to production kext, runtime flags or EFI.**



## TX callback entry and workloop rescheduling (2026-11-11)

The developer provided the local `awdl-skywalk-tx-entry.txt` output
containing `IOSkywalkTxSubmissionQueue::initWithPool`,
`checkForWork` and both `packetSubmission` overloads.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md),
section 15.

- `initWithPool` enforces capacity `>= 8`, non-null pool
  and mandatory packet-submission callback, and flags `<= 0x1f`.
  The optional budget callback is stored at `queue+0xf0`;
  the mandatory packet callback and refCon are passed to
  the base initializer (raw table `__ZTV20IOSkywalkPacketQueue
  +0x240`, object-vptr `+0x230`).
- `packetSubmission(bool)` invokes
  `IOEventSource::tryCloseGate`; if that attempt fails,
  it increments a work-generation counter and calls
  `IOEventSource::signalWorkAvailable`, which permits
  deferred processing.
- The packetSubmission routes reach `legacyDequeue`
  and thus the registered `TxSubmissionAction`, or
  alternate list/refill paths. `checkForWork` checks
  counters/state and may schedule another submission.
- This means no proof exists that `disable`,
  `finalize`, or closing a driver callback admission
  gate prevents all **future** calls into `refCon`.
  The workloop detach and owner-side caller sequence
  are the next research targets.

The collector accepts `IOWorkLoop` and `IOEventSource`
in `--classes` for focused lifecycle inspection.
Use the following read-only commands, including `cat`:

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes IOSkywalkPacketQueue,IOSkywalkTxSubmissionQueue \\
  --offsets 0x230,0x1d8,0x1e0

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkPacketQueue,IOWorkLoop,IOEventSource \\
  --methods initWithPool,addEventSource,removeEventSource,signalWorkAvailable \\
  --max-functions 16 --max-lines 400 \\
  --out /tmp/awdl-skywalk-workloop-lifetime.txt

cat /tmp/awdl-skywalk-workloop-lifetime.txt
```

The matched signatures and bodies must be interpreted along with
their **callers**; merely finding `removeEventSource` does
not establish that this owner actually uses it or guarantees
that queue callbacks are quiescent. No live AWDL, queue
registration, firmware TX/RX or EFI changes should be made.



## WorkLoop detach and base queue refCon investigation (2026-11-11)

The developer provided local KC vtable and disassembly results in
`awdl-skywalk-workloop-lifetime.txt` for
`IOSkywalkPacketQueue::initWithPool`,
`IOWorkLoop::{addEventSource,removeEventSource,signalWorkAvailable}`,
and `IOEventSource::signalWorkAvailable`.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md), section 16.

- The base queue initializer stores the provided **refCon** in
  `queue+0x58` with no demonstrated lifetime retention. The mandatory
  callback is supplied to the superclass init. `queue+0xac` is
  confirmed as the passed table capacity, used later for TX ring
  and completion array sizing.
- `IOWorkLoop::addEventSource` dispatches through the object
  at `workloop+0x20` and its virtual slot `+0x1c0` using
  operation argument 0.
- `IOWorkLoop::removeEventSource` dispatches through that
  same slot with operation argument 1. The body does **not**
  show the actual removal algorithm or callback-drain barrier.
- `IOEventSource::signalWorkAvailable` forwards to its
  workloop, which marks pending work and wakes its thread
  through `_thread_wakeup_thread`.
- **No proof** has yet been found that a removed event source
  cannot run `legacyDequeue` again, including direct
  submission callers and previously queued work. The callback's
  embedded `SkywalkBackend::txCallbackContext_` must remain alive
  until a separately evidenced no-future-entry barrier exists.

### Next: map the internal workloop operation and caller teardown

The read-only collector now accepts `IOCommandGate` and
selected gate/workloop methods. Inspect the workloop's internal
dispatch virtual, then extract focused methods:

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes IOCommandGate,IOWorkLoop,IOEventSource \\
  --offsets 0x170,0x1c0

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOCommandGate,IOWorkLoop,IOEventSource \\
  --methods runAction,runCommand,setWorkLoop,removeEventSource \\
  --max-functions 12 --max-lines 400 \\
  --out /tmp/awdl-workloop-dispatch.txt

cat /tmp/awdl-workloop-dispatch.txt
```

It is possible that the workloop's private `+0x20` object is
**not** an ordinary `IOCommandGate`; in that case, names for the
`+0x1c0` operation may require identifying a private subclass.
Do not equate a vtable match on the wrong class with its runtime
target, and do not publish raw KC disassembly.

No kernel C++ sources, kext runtime flags or EFI are modified.



## IOCommandGate handler and detachment review (2026-11-11)

The user supplied the local `awdl-workloop-dispatch.txt` containing
`IOCommandGate::{runAction,runCommand,setWorkLoop}`,
`IOWorkLoop::{runAction,removeEventSource}` and
`IOEventSource::setWorkLoop`. See
[STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md), section 17.

**Verified:**

- The `IOCommandGate` vptr slot `+0x1c0` is
  `runCommand(...)`, used by `IOWorkLoop::removeEventSource`
  via its internal `workloop+0x20` object. This is
  strong evidence that removal is delegated to the
  internal command gate.
- `runCommand` is a thin forwarding wrapper to
  `gate->vptr+0x1c8`, **not** the removal implementation itself.
  The `+0x1c8` target remains unidentified.
- `IOCommandGate::runAction` and
  `IOWorkLoop::runAction` contain workloop gate
  enter/leave logic. `IOCommandGate::setWorkLoop(nullptr)`
  can sleep/wait while detaching its workloop.
- `IOEventSource::setWorkLoop(nullptr)` calls an event-source
  virtual at `+0x158` and clears the stored workloop pointer.
  The gate synchronization does **not automatically establish**
  safe lifetime for external direct TX callbacks or their
  borrowed `refCon`.

**Next, read-only vtable lookup (show through cat):**

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes IOCommandGate,IOWorkLoop,IOEventSource \\
  --offsets 0x130,0x158,0x178,0x180,0x190,0x198,0x1c8 \\
  > /tmp/awdl-workloop-vptr.txt

cat /tmp/awdl-workloop-vptr.txt
```

After this lookup identifies `IOCommandGate+0x1c8`, use the
read-only collector to inspect the **exact identified method**.
Do not infer that `removeEventSource` is already a proven
no-future-callback barrier. No kext/EFI/runtime flag changes.



## Workloop command action confirmed (2026-11-11)

The developer's Tahoe 25G332 vtable mapping established
`IOCommandGate::runAction` at object-vptr `+0x1c8`.
This completes the previously unnamed command-gate dispatch:

```text
IOWorkLoop::removeEventSource(queue)
  -> IOCommandGate::runCommand(operation=1,queue)
  -> IOCommandGate::runAction(action=gate+0x20,...)
  -> saved action called under workloop gate
```

A prior report erroneously described `gate+0x20` as
the owner; **correction:** it is the action pointer,
while `gate+0x18` is the owner. See
[STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md)
section 18.

The public Apple XNU `IOWorkLoop.cpp` initializes
the internal command-gate action with
`IOWorkLoop::_maintRequest`, whose removal case
unlinks an event source, clears its workloop/next
pointers and releases a retained reference.

https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOWorkLoop.cpp

This is **public-source corroboration, not verification
against the exact 25G332 KC**. The developer should now
inspect the KC's actual `IOWorkLoop::_maintRequest` to
validate that behavior.

### Next read-only evidence (with cat)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOWorkLoop \\
  --methods _maintRequest,init,runEventSources \\
  --max-functions 6 --max-lines 420 \\
  --out /tmp/awdl-workloop-maintenance.txt

cat /tmp/awdl-workloop-maintenance.txt
```

The exact `_maintRequest` function may be absent from
exported KC symbols, in which case report the method
list and proceed with a targeted vtable/function-reference
investigation instead of assuming its behavior. No
live AWDL, EFI changes or kext installation is permitted
by this evidence.



## Exact 25G332 IOWorkLoop maintenance removal verified (2026-11-11)

The developer uploaded a full **local KC** extraction of
`IOWorkLoop::_maintRequest`, `IOWorkLoop::init` and
`IOWorkLoop::runEventSources`.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md),
section 19 for specific control-flow findings.

This closes the gap between our **public-XNU architectural guess**
and the exact locally inspected Tahoe 25G332 implementation:

- `removeEventSource` calls the internal command gate.
  The `_maintRequest` remove operation (1) traverses
  active/passive source chains, unlinks the matching
  source, calls `setWorkLoop(nullptr)` and
  `setNext(nullptr)`, releases a workloop-held
  source reference, and sets a rescan flag.
- `runEventSources` traverses the same source
  lists while holding the workloop gate, invoking
  source `checkForWork` and `getNext`.
  A changed-list flag causes traversal to restart.
  **This provides evidence that removal and
  WorkLoop-driven `checkForWork` calls are serialized.**
- `IOWorkLoop::init` creates the internal command
  gate stored at `workloop+0x20`; the action callback
  comes from the workloop's own vptr `+0x118`.
  Its exact identity needs vtable confirmation.
- A remove request may return 0 on an already
  unattached source. Other cases can return
  `0xe00002c2` when the source is not located.
  The owner must track attachment state.

**Remaining danger:** queue callbacks may originate
from direct TX packetSubmission callers outside
`runEventSources`, and there is still no evidence
that an experimental backend will perform this
workloop removal before destroying its embedded
`TxCallbackContext`. The trampoline uses `refCon`
before checking its `CallbackGate`. Do not enable
any live AWDL path.

### Next read-only mapping (including cat)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes IOWorkLoop,IOEventSource,IOSkywalkTxSubmissionQueue,IOCommandGate \\
  --offsets 0x118,0x120,0x128,0x130,0x138,0x168,0x1b8 \\
  > /tmp/awdl-workloop-maint-vptr.txt

cat /tmp/awdl-workloop-maint-vptr.txt
```

Next investigation after mapping: find the **specific
owner-side attach/remove call sites** and non-workloop
packetSubmission callers, rather than inferring an
all-callback drain merely from the generic workloop
mechanism. All flags remain disabled; no kext or EFI
changes.



## 25G332 workloop action identity confirmed (2026-11-11)

The local `IOWorkLoop` vptr `+0x118` was mapped to
`IOWorkLoop::_maintRequest` at
`0xffffff8000acaac0`. This is **direct evidence in the
actual 25G332 KC** that `IOWorkLoop::init()`
passes `_maintRequest` as its internal command-gate
action, completing the chain:
`removeEventSource -> IOCommandGate::runCommand ->
runAction -> _maintRequest`.

Other newly mapped slots:

- `IOEventSource +0x120` = `checkForWork`;
  the specialized `IOSkywalkTxSubmissionQueue +0x120`
  = `IOSkywalkTxSubmissionQueue::checkForWork`.
- `IOEventSource +0x128/+0x130/+0x138/+0x168`
  = `setWorkLoop/setNext/getNext/getWorkLoop`,
  all inherited by the Skywalk TX submission queue.
- `IOCommandGate +0x1b8` = `init(owner, action)`;
  `IOSkywalkTxSubmissionQueue +0x1b8` =
  `initialize(void*)` at `0xffffff8002b11e8e`.

See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md),
section 20 for the full local table, verified call chain and
the remaining direct-callback lifetime risk.

**Meaning:** a successful workloop removal is serialized
with `runEventSources/checkForWork` under that same gate.
**Not proved:** all non-workloop TX producer paths stop,
or that a future AWDL backend performs this detach
before freeing the borrowed `refCon`.
No runtime operation is authorized.

### Next read-only KC evidence (includes cat)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkTxSubmissionQueue,IOSkywalkPacketQueue \\
  --methods initialize,enable,disable \\
  --max-functions 12 --max-lines 380 \\
  --out /tmp/awdl-skywalk-tx-attach-lifecycle.txt

cat /tmp/awdl-skywalk-tx-attach-lifecycle.txt
```

The method name filters work without a new collector code
change. This inspection seeks a queue workloop attach
path or producer registration. Finding no attach call
there would not prove no owner code can attach the queue;
the caller side still requires independent inspection.

**Guardrails:** never publish raw KC disassembly, never
enable the experimental AWDL registration/datapath, and
do not install a research kext.



## 25G332 TX init/enable/disable and owner discovery (2026-11-11)

The local `awdl-skywalk-tx-attach-lifecycle.txt` output
reveals both `IOSkywalkPacketQueue` base lifecycle methods
and TX submission overrides. See
[STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md),
section 21.

**Confirmed:**

- Base queue `initialize()` calls pool
  `checkInPacketQueue()` and increments
  `queue+0xbc`. It returns success when the
  updated count is 1 and `0xe00002d5` when
  greater than 1. Combined with the matching
  base `finalize()`, this identifies a
  queue initialization/finalization balance,
  **not an in-flight TX callback counter**.
- `TxSubmissionQueue::initialize()` calls the
  base method under an existing event-source
  workloop gate (if present), stores caller
  data at `+0xe0` and clears work counters on
  success. It handles the `0xe00002d5`
  case via virtual `finalize()`.
- `TxSubmissionQueue::enable()` uses base
  enable and may directly trigger
  `packetSubmission(false)` or
  `sendNotification(0)`.
- `TxSubmissionQueue::disable()` calls base
  disable under a temporary event-source gate
  and reopens the gate. Base disable resets
  ABFS if present, then calls the inherited
  event-source disable method.
- **None of the inspected implementations
  calls `IOWorkLoop::removeEventSource()`.**
  This is **not proof** that no higher-level
  owner performs such removal.
- **Current speculative Stage 4 backend**
  uses `q->disable(); q->release();`,
  which cannot be considered a proven
  no-future-`refCon` barrier if live allocation
  were enabled. The actual Stage 4 live gates
  remain disabled.

### Next read-only owner-side inventory and base enable

The collector now supports `--list-only --all-methods`
and recognizes `IOSkywalkNetworkInterface` and
`IOSkywalkQueueSet`. This inventories method names
without dumping full disassembly. Run:

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkNetworkInterface,IOSkywalkQueueSet \\
  --all-methods --list-only \\
  > /tmp/awdl-skywalk-owner-methods.txt

cat /tmp/awdl-skywalk-owner-methods.txt

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkPacketQueue,IOSkywalkTxSubmissionQueue \\
  --methods enable,free,withPool \\
  --max-functions 12 --max-lines 350 \\
  --out /tmp/awdl-skywalk-base-enable-free.txt

cat /tmp/awdl-skywalk-base-enable-free.txt
```

The first command is a symbol/method inventory only;
the second is bounded method-specific KC evidence.
Next research should focus on **actual owner-side**
event-source `addEventSource/removeEventSource`
call sites and external TX packet producers.

Do not redistribute raw proprietary KC output or
enable the AWDL runtime. Production kext and EFI
remain untouched.



## Queue allocation, free and owner-side entrypoint inventory (2026-11-11)

The developer supplied local Tahoe 25G332 output for
`IOSkywalkTxSubmissionQueue::withPool` overloads,
`IOSkywalkTxSubmissionQueue::free`,
`IOSkywalkPacketQueue::free`, and 171
symbol-inventory matches for
`IOSkywalkNetworkInterface` / `IOSkywalkQueueSet`.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md),
section 22.

- The full TX `withPool` allocator constructs a
  `0x168`-byte queue, invokes virtual `initWithPool`
  at object-vptr `+0x2a8`, and `release()`s the
  object if that initialization fails.
  Smaller overloads forward arguments/flags into
  this allocator; the `IOSkywalkPacket**` variant
  forces option bit `0x8`.
- TX `free()` destroys/frees its packet-table and
  stats/ABFS allocations before tail-calling base
  `IOSkywalkPacketQueue::free()`, which releases
  the pool reference. These bodies show **no
  explicit** packet purge, queue finalize, workloop
  detach or callback drain. The inherited
  `IOEventSource::free` and owner sequence require
  separate validation before asserting total absence.
- Newly found owner-side candidates:
  `IOSkywalkQueueSet::{initWithQueues,initialize,
  setEnable,finalize,free}` and
  `IOSkywalkNetworkInterface::{registerNetworkInterface,
  deregisterNetworkInterface,stop,free}`.
  None is yet proven to call `removeEventSource`.
- The 171-match inventory is not an exact
  member count. The original class filter matched
  types mentioned inside unrelated function arguments
  (such as `AppleEthernetRL::getTSOOptions`).
  The read-only collector has now been fixed to
  require that the **demangled qualified name starts
  with the selected class**, keeping methods
  correctly assigned.

### Next two bounded read-only collections (both include cat)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkQueueSet \\
  --methods initWithQueues,initialize,setEnable,finalize \\
  --max-functions 6 --max-lines 550 \\
  --out /tmp/awdl-skywalk-queueset-lifecycle.txt

cat /tmp/awdl-skywalk-queueset-lifecycle.txt

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkNetworkInterface \\
  --methods registerNetworkInterface,deregisterNetworkInterface \\
  --max-functions 8 --max-lines 500 \\
  --out /tmp/awdl-skywalk-interface-registration.txt

cat /tmp/awdl-skywalk-interface-registration.txt
```

The second output may be longer because the register
method has multiple overloads and compiler-generated
allocation-view symbols. They are local research
evidence, **not** files to commit to a public GitHub
repository. The next decision will be based on
whether either owner path calls the workloop's
add/remove entrypoints and which callbacks can still
be initiated after that operation.

Runtime AWDL registration and datapath remain
disabled, with no kext or EFI modifications.



## QueueSet -> LogicalLink ownership and interface cleanup (2026-11-11)

The developer uploaded local 25G332 disassembly
`awdl-skywalk-queueset-lifecycle.txt` and
`awdl-skywalk-interface-registration.txt`.
The first captured only `IOSkywalkQueueSet::initWithQueues`
plus five non-code `kalloc_type_view` symbols because
`--max-functions 6` was exhausted early. **We have
not yet inspected QueueSet `initialize`,
`setEnable` or `finalize`.** The second report
contains both NetworkInterface registration
overloads and deregistration.

See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md)
section 23 for grounded control flow.

- `QueueSet::initWithQueues` groups queues by type,
  stores their pointers in categorized and overall
  arrays, calls `queue->setQueueSet(this)`, and
  **retains each queue**. This is positive
  QueueSet-side ownership evidence.
- The `registerNetworkInterface` overload accepting
  a queue array constructs a `QueueSet`, wraps it
  in a `LogicalLink` and passes the result to
  another registration overload.
- The `LogicalLink*` registration overload
  configures a default logical link and retains
  packet-buffer pool references.
- `deregisterNetworkInterface` releases links,
  pool and metadata references and removes
  relevant steering rules, but the displayed
  body has **no direct call** to
  `removeEventSource`. That does not rule
  out detach in the child object's teardown.
- The collector previously included function-local
  statics like `kalloc_type_view`, masking
  real target methods under `--max-functions`.
  It now excludes `__ZZN...` symbols, retains
  `__ZN...` / `__ZNK...` methods and supports
  `IOSkywalkLogicalLink`.

### Next exact bounded evidence (with cat for both)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkQueueSet \\
  --methods initialize,setEnable,finalize,free \\
  --max-functions 8 --max-lines 450 \\
  --out /tmp/awdl-skywalk-queueset-teardown.txt

cat /tmp/awdl-skywalk-queueset-teardown.txt

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkLogicalLink \\
  --methods initialize,setInterface,finalize,free \\
  --max-functions 8 --max-lines 450 \\
  --out /tmp/awdl-skywalk-logical-link-teardown.txt

cat /tmp/awdl-skywalk-logical-link-teardown.txt
```

If a requested method does not exist in this
KC, the collector will only show the available
matches. The next question is whether any
`QueueSet/LogicalLink` teardown method actually
detaches a TX event source, drains callbacks,
and releases it in a proven order. Until that
and direct TX entry paths are established, the
embedded `TxCallbackContext` remains unsafe
to destroy on a merely disabled queue.

All experiments remain read-only; AWDL runtime
registration and packet datapath stay disabled.



## QueueSet and LogicalLink cleanup method bodies (2026-11-11)

The developer supplied local 25G332 KC output for
`IOSkywalkQueueSet::{initialize,setEnable,finalize,free}`
and `IOSkywalkLogicalLink::{setInterface,free}`.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md)
section 24 for detailed control-flow and ownership evidence.

**Confirmed:**

- `QueueSet::setEnable(true/false)` walks all
  queues and calls object-vptr `+0x150/+0x158`
  respectively, i.e. queue enable/disable.
  It does **not** remove their WorkLoop event
  sources in this body.
- `QueueSet::initialize` calls a self virtual
  `+0x128` (not yet mapped), then initializes
  categorized queues at `QueueSet+0x28` and
  `+0x30`, and calls `QueueSet::finalize`
  if a later stage fails.
- `QueueSet::finalize` calls queue virtual
  `+0x1c0` (finalize) for exactly those two
  categorized queue arrays; it ignores per-queue
  status and clears `+0x48/+0x50`.
- `QueueSet::free` removes each queue's
  QueueSet backpointer, releases every retained
  queue reference and frees the arrays.
  It does not explicitly invoke finalize,
  disable, or WorkLoop detach in the shown body.
- `LogicalLink::setInterface(interface)`
  checks queue pointers into or out of their
  packet-buffer pools and sets owner registry
  IDs. **Pool registration is not WorkLoop
  event-source attachment.**
- `LogicalLink::free` calls **QueueSet vptr
  `+0x118` with null** prior to
  `setLogicalLink(nullptr)` and `release()`.
  **Subsequent vtable mapping confirms this is
  `IOSkywalkQueueSet::setEnable(false)`.** It
  disables queues but is not WorkLoop detach.

This fills in the parent/child reference
releases but still does **not** show a complete
no-future-callback barrier for the borrowed
`TxCallbackContext`. A Gate close or
`setEnable(false)` alone cannot establish
its safe destruction.

### Next two read-only checks, with cat for each output

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes IOSkywalkQueueSet,IOSkywalkLogicalLink,IOSkywalkTxSubmissionQueue,IOSkywalkPacketQueue \\
  --offsets 0x118,0x128,0x150,0x158,0x1b8,0x1c0,0x1e8,0x218 \\
  > /tmp/awdl-skywalk-parent-vptr.txt

cat /tmp/awdl-skywalk-parent-vptr.txt

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOEventSource \\
  --methods free,setWorkLoop \\
  --max-functions 4 --max-lines 330 \\
  --out /tmp/awdl-event-source-free.txt

cat /tmp/awdl-event-source-free.txt
```

The vtable check resolves the unknown
`QueueSet+0x118` and `+0x128` methods,
and packet-pool getter slot
`queue+0x1e8`. The second check inspects
the inherited `IOEventSource::free` path,
which can affect whether event-source removal
is guaranteed before the subclass's memory
is reclaimed. Inspect both without live
registration, kext loading, or EFI changes.
Do not publish raw KC assembly in the repo.



## 25G332 vtable resolves QueueSet pre-release disable; IOEventSource free (2026-11-11)

The developer ran the targeted vtable inspection and provided
`IOEventSource::{free,setWorkLoop}` bodies from the local
`BootKernelExtensions.kc` (Tahoe 25G332). The summarized
evidence is recorded in
[STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md),
section 25. Raw KC disassembly is intentionally excluded.

**Verified:**

- `IOSkywalkQueueSet +0x118` =
  `IOSkywalkQueueSet::setEnable(bool)`.
  Therefore `IOSkywalkLogicalLink::free` calls
  `QueueSet::setEnable(false)` for each retained
  QueueSet **before** `setLogicalLink(nullptr)`
  and `release()`. This is real queue disable
  at object teardown, but not an event-source
  removal call.
- `IOSkywalkQueueSet +0x128` =
  `setQueueSetId(uint64_t)`. The virtual
  call during `QueueSet::initialize` sets an
  ID, not a new callback source.
- `IOSkywalkTxSubmissionQueue +0x218` =
  `performCommand(unsigned,void*,unsigned long)`
  at `0xffffff8002b128b6`, targeted by
  `QueueSet::initialize`; its behavior still
  needs inspection.
- `IOSkywalkPacketQueue +0x1e8` =
  `getPacketBufferPool() const`, confirming
  pool bookkeeping in
  `LogicalLink::setInterface`.
- `IOEventSource::free()` at
  `0xffffff8000acc2a0` unregisters stats,
  optionally releases a Block callback, frees
  statistics memory and delegates superclass
  cleanup. **No direct WorkLoop detach or
  callback-drain call exists in this body.**
- `IOEventSource::setWorkLoop(nullptr)`
  invokes the source's `disable()` virtual
  and then nulls `eventSource+0x30`. The
  higher-level `_maintRequest` is the known
  synchronized event-source removal entry.

**Safety interpretation:** owner-level
`disable`, `removeEventSource`,
`finalize` and `release/free` are
different actions. In particular, a disabled
event source can still be linked into a
workloop, and a borrowed callback `refCon`
remains unsafe to free before direct and
workloop-driven callbacks have fully quiesced.

### Next bounded read-only inspection (with cat)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkLogicalLink,IOSkywalkTxSubmissionQueue,IOSkywalkPacketQueue \\
  --methods setEnable,performCommand \\
  --max-functions 5 --max-lines 450 \\
  --out /tmp/awdl-skywalk-tx-command-lifecycle.txt

cat /tmp/awdl-skywalk-tx-command-lifecycle.txt

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkNetworkInterface \\
  --methods stop,free \\
  --max-functions 3 --max-lines 350 \\
  --out /tmp/awdl-skywalk-network-stop.txt

cat /tmp/awdl-skywalk-network-stop.txt
```

The next focus is whether `performCommand` or
higher-level interface shutdown establishes a
no-new-TX-callback barrier, and whether the
owner actually detaches queues from WorkLoop.
Absence of a direct `removeEventSource` call
in any one method is **not proof that no
other owner handles it**. Do not post the
proprietary disassembly to public GitHub.

No runtime kext, EFI or experimental AWDL
registration/datapath changes are part of
this research stage.



## TX performCommand and interface stop/free inspection (2026-11-11)

The developer supplied local 25G332 KC disassembly
from `awdl-skywalk-tx-command-lifecycle.txt`
and `awdl-skywalk-network-stop.txt` for
`IOSkywalkLogicalLink::setEnable`,
`IOSkywalkTxSubmissionQueue::performCommand`,
base `IOSkywalkPacketQueue::performCommand`,
and `IOSkywalkNetworkInterface::{stop,free}`.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md),
section 26.

**Verified:**

- `LogicalLink::setEnable(bool)` iterates
  its QueueSets, invokes each `setEnable(bool)`,
  and exits early on a nonzero status;
  empty collections return `0xe00002bc`.
  `QueueSet::setEnable` itself returns zero
  without monitoring every queue's transition.
- `TxSubmissionQueue::performCommand` dispatches
  configuration and query commands via a jump table.
  Visible branches access capacity/pool info,
  connection/flag fields, and `addReporters`;
  unhandled commands fall through to base
  `performCommand` (`0xe00002c7`).
  No direct workloop detach or TX callback drain
  is seen. The exact semantics of the
  `QueueSet::initialize` command `0x50000`
  are **not** established solely from this output.
- `NetworkInterface::stop` conditionally
  calls `deregisterNetworkInterface`, then
  delegates to `IOSkywalkInterface::stop`.
- `NetworkInterface::free` calls deregistration
  then cancels and frees a private thread call,
  frees locks, releases a private object and
  delegates to superclass `free`.
  **Cancellation is not automatically a proof
  of running callbacks being quiescent.**
  This private thread call has not been linked
  to the AWDL TX callback path.

**Safety status:** still no documented owner-side
queue `removeEventSource` call or independent
barrier preventing all future direct
`packetSubmission` entries. Freeing the
backend's borrowed callback context would remain
unsafe without those proofs. Do not enable
experimental registration/datapath.

### Next read-only superclass and network-enable inventory

The collector now includes the `IOSkywalkInterface`
superclass for targeted named-method collection.
Run the following, keeping the outputs local:

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkInterface \\
  --all-methods --list-only \\
  > /tmp/awdl-skywalk-interface-base-methods.txt

cat /tmp/awdl-skywalk-interface-base-methods.txt

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkNetworkInterface \\
  --methods enable,disable \\
  --max-functions 4 --max-lines 400 \\
  --out /tmp/awdl-skywalk-network-enable-disable.txt

cat /tmp/awdl-skywalk-network-enable-disable.txt
```

The inventory lets us choose exact superclass lifecycle
methods on the next pass. The second command checks whether
enable/disable directly interact with WorkLoop registration
or hand off to other owner objects. If not, another
reverse-caller investigation will be required.
Do not commit raw KC disassembly or make any
kext/EFI/runtime changes.



## IOSkywalkInterface queue API and network virtual enable path (2026-11-11)

The developer posted the 38-symbol method inventory for
`IOSkywalkInterface` and the local Tahoe 25G332
`IOSkywalkNetworkInterface::{enable,disable}`
function bodies. See
[STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md)
section 27.

**Confirmed by local KC:**

- `NetworkInterface::enable(unsigned)` calls its
  **dynamic vptr `+0x930`** with `true`; stores
  whether that returned success as `interface+0xd8`.
- `NetworkInterface::disable(unsigned)` uses an
  atomic `cmpxchg` transition of `+0xd8`
  from 1 to 0; only on that successful
  transition does it call the same `+0x930`
  virtual with `false`. It may report the
  link-down status and clears a private
  state flag at `private+0x3c`.
- **Updated by exact KC vtable evidence:** `+0x930`
  is `IOSkywalkNetworkInterface::setInterfaceEnable(bool)`
  at `0xffffff8002b10558`. Its body must still
  be inspected before claiming a queue-set cascade.
- New superclass candidates:
  `IOSkywalkInterface::registerInterfaceWithQueues`
  at `0xffffff8002b00d0c`,
  `deregisterInterfaceWithQueues` at
  `0xffffff8002b00b9c`,
  and `free` at `0xffffff8002b00b14`.
  Their behavior has **not** yet been inspected.
- Important earlier naming correction:
  `NetworkInterface::stop` tail-calls
  the raw `IOSkywalkInterface` base table
  `__ZTV+0x5d8`, object-vptr `+0x5c8`.
  The symbol inventory contains no
  `IOSkywalkInterface::stop` override;
  subsequent local vtable inspection confirms
  this entry is `IOService::stop(IOService*)`
  at `0xffffff8000a94e80`.
- No proven workloop detach, direct-TX callback
  drain or borrowed-`refCon` no-future-entry
  barrier follows merely from `interface+0xd8`
  state transitions.

### Next precise, read-only local KC checks (each ends in cat)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/inspect_skywalk_vtables.py \\
  --classes IOSkywalkNetworkInterface,IOSkywalkInterface \\
  --offsets 0x930,0x5c8 \\
  > /tmp/awdl-network-virtuals.txt

cat /tmp/awdl-network-virtuals.txt

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkInterface \\
  --methods registerInterfaceWithQueues,deregisterInterfaceWithQueues,free \\
  --max-functions 4 --max-lines 550 \\
  --out /tmp/awdl-interface-queue-api.txt

cat /tmp/awdl-interface-queue-api.txt
```

The first command identifies the vtable targets
on the **actual KC**. The second inspects the
specific superclass queue API. A call to
`removeEventSource` here would be positive
evidence; its absence would redirect research
to the caller / concrete subclass and must
not be treated as proof of no detach anywhere.

Do not commit local proprietary KC output.
Neither runtime feature flag is enabled;
do not install a research kext or alter EFI.



## Queue-array register/deregister and concrete setInterfaceEnable identity (2026-11-11)

The developer supplied the local Tahoe 25G332 KC
vtable scan and full, bounded disassembly of
`IOSkywalkInterface::{registerInterfaceWithQueues,
deregisterInterfaceWithQueues,free}`.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md)
section 28 for the detailed flow.

- `IOSkywalkNetworkInterface` object-vptr
  `+0x930` maps to
  **`IOSkywalkNetworkInterface::setInterfaceEnable(bool)`**
  at `0xffffff8002b10558`.
- `IOSkywalkInterface` object-vptr `+0x5c8`
  maps to inherited **`IOService::stop`**
  at `0xffffff8000a94e80`;
  `IOSkywalkNetworkInterface +0x5c8` maps
  to its own overriding `stop`.
- `IOSkywalkInterface::registerInterfaceWithQueues`
  requires a non-null array with `count >= 2`
  and at least one pool. It constructs a queue
  `OSArray`, atomically publishes it at
  `interface+0x90`, associates each queue
  with this interface via `setInterface`,
  and retains pool references at
  `+0x98/+0xa0`.
- `IOSkywalkInterface::deregisterInterfaceWithQueues`
  atomically unpublishes the `OSArray`
  and releases the array/pool references,
  without an explicit direct queue disable,
  `setInterface(nullptr)`, workloop
  `removeEventSource` or TX callback drain.
  This is **a local method observation**, not
  proof that other owner paths are absent.
- `IOSkywalkInterface::free` invokes that
  deregistration, then tears down an auxiliary
  object/lock allocation and delegates
  to superclass free.
- The `IOSkywalkInterface` queue-array API
  is not yet proven to be the same route as
  `IOSkywalkNetworkInterface::registerNetworkInterface`
  with QueueSet/LogicalLink. Do not merge
  ownership models without tracing callers.

**Safety status:** no global no-future-callback
barrier has yet been established for the
borrowed `SkywalkBackend::txCallbackContext_`.
An `OSArray::release` and workloop-list
detach are distinct operations. No experimental
AWDL registration or datapath should run.

### Next bounded local method inspection (both commands end in cat)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkNetworkInterface,IOSkywalkPacketQueue \\
  --methods setInterfaceEnable,setInterface \\
  --max-functions 4 --max-lines 450 \\
  --out /tmp/awdl-skywalk-interface-enable-association.txt

cat /tmp/awdl-skywalk-interface-enable-association.txt

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkInterface \\
  --methods willTerminate,didTerminate,handleClose \\
  --max-functions 5 --max-lines 400 \\
  --out /tmp/awdl-skywalk-interface-termination.txt

cat /tmp/awdl-skywalk-interface-termination.txt
```

The follow-up local KC results found a **no-op**
`IOSkywalkNetworkInterface::setInterfaceEnable`,
a **pointer-only** `IOSkywalkPacketQueue::setInterface`
and termination hooks with no explicit queue detach.
The named-method approach is now supplemented by
a bounded direct-call cross-reference scan (below).
Note that overriding subclasses can change the
virtual enable implementation.

Only read local KC disassembly; do not
publish proprietary output, install kexts,
modify EFI, or enable the experimental AWDL
code paths.



## Queue/interface association and bounded direct-caller scan (2026-11-11)

The developer provided new local Tahoe 25G332
bodies of `IOSkywalkNetworkInterface::setInterfaceEnable`,
`IOSkywalkPacketQueue::setInterface`, and
`IOSkywalkInterface::{willTerminate,didTerminate,handleClose}`.
See [STAGE4_4_KC_ANALYSIS.md](STAGE4_4_KC_ANALYSIS.md),
section 29.

- **`NetworkInterface::setInterfaceEnable(bool)`
  returns success immediately.** No queue,
  `LogicalLink` or WorkLoop operation appears
  in this class's method body. A concrete
  derived-class override may still do more.
- **`PacketQueue::setInterface(interface)`
  stores the raw interface pointer at
  `queue+0x70` without retaining it** and
  records the interface's registry ID in
  `queue+0x90/+0xa0`. Passing null
  clears those values.
  The `IOSkywalkInterface` deregistration
  body did not call the setter with null,
  leaving exact owner ordering to be proved.
- `IOSkywalkInterface::willTerminate` sets
  a private flag at `private+0x8`.
  `didTerminate` delegates conditionally
  through two yet-unresolved inherited
  vtable entries. `handleClose` acquires
  a private `IOLock` and dispatches
  `+0x1d8` on an **unidentified private
  object**; this must not be misidentified
  as TX queue's `packetSubmission` solely
  because the offset matches.
- These routines do **not** provide
  a proven direct-TX no-future-callback
  barrier or a verified
  `removeEventSource(txQueue)` invocation.

### Improved next approach

A new research-only script,
`scripts/awdl-abi/scan_skywalk_direct_calls.py`,
performs bounded, **read-only** disassembly
of relevant named functions and reports
direct `call/jmp` sites to three known
local `25G332` targets:
`IOWorkLoop::addEventSource`,
`IOWorkLoop::removeEventSource`, and
`TxSubmissionQueue::packetSubmission(bool)`.
It outputs only symbolic caller information,
**not raw KC instructions**.

This scan is deliberately incomplete:
virtual dispatch, function pointers,
inlining, other classes and truncated
functions are not captured. **A no-hit
result is NOT evidence of callback
quiescence or teardown correctness.**
We also need to inspect
`IOSkywalkInterface::init/handleOpen`
to identify the private object's type
before interpreting `handleClose+0x1d8`.

### Next read-only local checks (each ends in cat)

```bash
cd ~/Desktop/itlwm
git pull --ff-only awdl research/awdl-stage4-4-skywalk-kc

python3 scripts/awdl-abi/scan_skywalk_direct_calls.py \\
  --families IOSkywalk,IOUserNetwork,IOEthernet \\
  --max-functions 180 --max-lines 450 \\
  --out /tmp/awdl-skywalk-direct-callers.txt

cat /tmp/awdl-skywalk-direct-callers.txt

python3 scripts/awdl-abi/collect_skywalk_queue_evidence.py \\
  --classes IOSkywalkInterface \\
  --methods init,handleOpen,clientDisconnect \\
  --max-functions 6 --max-lines 400 \\
  --out /tmp/awdl-skywalk-private-object.txt

cat /tmp/awdl-skywalk-private-object.txt
```

All actions remain local static research.
No Apple KC disassembly is added to
public GitHub. Preserve
`ITLWM_AWDL_REGISTRATION_ENABLED=0`
and `ITLWM_AWDL_DATAPATH_LIVE=0`
until the callback entry/barrier protocol
is proven.
