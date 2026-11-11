# Stage 4.4 — 25G332 queue control-flow analysis

**Source:** developer-provided local `/tmp/awdl-skywalk-kc-evidence.txt`, 2026-11-11, `BootKernelExtensions.kc`. This note records interpretation of the **13 targeted functions** without publishing bulk disassembly. These addresses are build-specific and are **not ABI guarantees**.

**Status:** evidence-backed call graph for selected paths; packet ownership, callback quiescence and live AWDL remain **unproven**. Do not enable the experimental driver gates.

## 1. TX submission: accepted count controls ring progress

`IOSkywalkTxSubmissionQueue::legacyDequeue` at `0xffffff8002b31860`:

- Checks queue state bits at `this+0x28` and `this+0xcc`.
- Reads the callback from `this+0x20`, target from `this+0x18`, `refCon` from `this+0x58`, and ring at `this+0xe8`.
- Invokes the callback with the `(OSObject*, IOSkywalkTxSubmissionQueue*, IOSkywalkPacket* const*, uint32_t count, void* refCon)` register order. The return value is `eax`.
- When it is nonzero, advances the ring read index at `ring+0x80` by the returned count and updates statistics. When it is zero, marks a blocked/stalled flag at `this+0x13c` and stops dequeuing.
- It compares returned count against offered count for a partial-batch statistic, but in the shown code does **not** clamp an overlarge return before advancing. Our `dispatchTxAction` already returns zero for `consumed > count`.

**Consequence:** a callback must not return positive consumption until packet ownership and a completion/drop route are real. This function does **not** itself return consumed packets to the pool.

## 2. TX completion: two different enqueue overloads

### Pointer-array overload

`IOSkywalkTxCompletionQueue::enqueuePackets(IOSkywalkPacket* const*, uint32_t, uint32_t)` at `0xffffff8002b32bb6`:

- Rejects null packet array, zero count, and `count >= *(this+0xac)` before attempting the work loop. **This is a strict-less-than comparison**, not `<=`. The meaning of field `+0xac` must be verified via `initWithPool` before treating it as the configured queue capacity.
- Requires a workloop/event-source pointer at `this+0x30`, enters `IOEventSource::closeGate`, checks enabled/state flags at `+0x28`, `+0xcc` and `+0x110`.
- Copies pointer array into its buffer `this+0xe0`, calls `IOSkywalkTxCompletionQueue::completePackets` and exits via `openGate`. The success branch returns 0.

### Linked-list overload

`IOSkywalkTxCompletionQueue::enqueuePackets(IOSkywalkPacket*, uint32_t, uint32_t)` at `0xffffff8002b32c6e`:

- Traverses `IOSkywalkPacket::getNextPacket`, clears each `next`, notifies its source queue, invokes `IOSkywalkPacket::completeWithQueue` at vptr offset `+0x198`, and conditionally calls `_kern_packet_tx_completion`.
- Calls `IOSkywalkPacketBufferPool::deallocatePacket` via `[*(this+0x50)]+0x140` for each packet; this is **not** the same call sequence as the pointer-array overload. The destination corresponds to the packet-pool vtable, supporting `this+0x50` as a pool pointer. Inspect the target body to confirm whether every failure and partial-batch path returns a packet.
- Calls source-queue notification and stats callbacks. The shown function does not call the named `completePackets` method.

## 3. TX completion bookkeeping and possible return-to-pool boundary

`IOSkywalkTxCompletionQueue::completePackets(IOSkywalkPacket**, uint32_t)` at `0xffffff8002b17b9e`:

- For each packet, calls `IOSkywalkPacket::getDataLength` at vptr offset `+0x140`, `getSourceQueue`, `notifySrcQueue`, and `IOSkywalkPacket::completeWithQueue` at offset `+0x198` with queue and flag arguments.
- Conditionally calls `_kern_packet_tx_completion` when `this+0xe8` is non-null.
- **After the per-packet loop**, invokes `IOSkywalkPacketBufferPool::deallocatePackets` at `[*(this+0x50)]+0x148` with the packet-pointer array and count; then notifies the source and updates stats.

The 2026-11-11 vtable inspection **identifies the batch operation as `deallocatePackets()`**. This is stronger evidence for a real TX return-to-pool path. It is **not yet proof** that the method succeeds for every packet, that failures are handled safely, or that a host-only `Ownership::acknowledgeReturn` can be called at any specific point. Inspect `deallocatePacket(s)` and the caller's error paths before integrating accounting.

## 4. RX completion: different consumers and ownership boundaries

### Pointer-array overload

`IOSkywalkRxCompletionQueue::enqueuePackets(IOSkywalkPacket* const*, uint32_t, uint32_t)` at `0xffffff8002b32766`:

- Applies similar checks for null/zero, `count >= *(this+0xac)`, workloop presence and state bits.
- Under an event-source gate, dispatches to `enqueuePacketsForNetworking` (`0xffffff8002b1433c`) if `this+0x78` is non-null, otherwise `enqueuePacketsForKPipe` (`0xffffff8002b1447e`).
- After either call the displayed branch explicitly returns zero, without propagating the callee's status. **The callee body must be examined** for rejection/drop/pool-return behavior.

### Linked-list overload

`IOSkywalkRxCompletionQueue::enqueuePackets(IOSkywalkPacket*, uint32_t, uint32_t)` at `0xffffff8002b32810`:

- Traverses an `IOSkywalkPacket` chain and constructs a native `kern_packet_t` linked list with `_kern_packet_set_next`.
- Invokes `IOSkywalkPacket::getDataLength()` at `+0x140`, `completeWithQueue()` at `+0x198` (mode/flag 2), and `disposePacket()` at `+0x1e0`; optionally taps BPF. `disposePacket` does **not** by itself establish native packet return-to-pool semantics.
- Calls `_kern_netif_queue_rx_enqueue` at `0xffffff80009e7730` with the native list and count. The shown function **does not test** its return register before continuing to statistics and returning zero.

This gives a likely handoff point to networking, **not** an evidenced packet-return acknowledgement or a guarantee about failed enqueue cases.

## 5. Disable is not proof of callback quiescence

Each of the three `IOSkywalk{TxSubmission,TxCompletion,RxCompletion}Queue::disable()` wrappers:

1. If workloop pointer at `this+0x30` exists, calls `IOEventSource::closeGate`.
2. Performs an indirect call at `[&__ZTV20IOSkywalkPacketQueue + 0x168]`, **measured from the raw vtable symbol**, not from the C++ object vptr. The Itanium two-entry header shifts the callable index by 16 bytes: this corresponds to **vptr offset `+0x158` (slot 43)**. The current vptr-based inspection only included `+0x168` (slot 45), which resolves to `IOEventSource::getWorkLoop()` and must **not** be mistaken for the target here. Resolve `+0x158` next.
3. Reopens the event-source gate with `IOEventSource::openGate` when the workloop pointer exists.

`IOSkywalkPacketQueue::disable` (`0xffffff8002b0de34`) additionally checks `this+0x68` and may call `resetABFS`, then jumps to a further virtual slot `+0x168`.

**The event-source gate is unlocked/reopened before these methods return.**
That does **not** necessarily mean the queue is re-enabled, but it certainly
does **not** establish that no future callback can enter or that callbacks
already executing on other threads have drained. Queue `free()`, the base
disable target, event source detach/removal and relevant caller serialization
must be investigated.

## 6. No changes authorized to live kext

- The existing experimental callback gate remains disarmed.
- The Stage 4.3 ticketed `Ownership` model remains host-only; none of these findings constitutes a real acknowledgement that a Skywalk packet was freed.
- No queue registration, AWDL object allocation, driver load, firmware transmit, EFI or reboot steps are safe to infer from this evidence.

## 7. Targeted next evidence needed

1. **Resolve base disable target:** the raw vtable symbol `__ZTV20IOSkywalkPacketQueue + 0x168` is **object-vptr + 0x158**, not +0x168. Inspect slot 43 explicitly, then examine the target body. The packet and pool vptr slots `0x140`, `0x148`, `0x198`, `0x1e0` have been named from the local KC; now inspect `deallocatePacket(s)`, `completeWithQueue`, and `disposePacket` to verify ownership and failure semantics.
2. **RX sink internals:** `IOSkywalkRxCompletionQueue::enqueuePacketsForNetworking` and `enqueuePacketsForKPipe` and their failure/return paths.
3. **Queue destruction:** all relevant `free()`, `IOSkywalkPacketQueue::disable`, `resetABFS`, and workloop/event-source detach behavior.
4. **Context lifetime:** find the owner-side teardown sequence and demonstrate no callbacks can enter after the last reference to the callback refCon is released.
5. **Configuration:** verify whether `this+0xac` corresponds to the capacity argument, since array overloads require strictly fewer packets than this field.

With these incomplete, **TX/RX live datapath remains blocked**.


## 8. Named virtual methods from local vtable listing (2026-11-11)

| Owner class | vptr offset | KC symbol | Address |
|---|---:|---|---|
| `IOSkywalkPacket` | `0x140` | `getDataLength() const` | `0xffffff8002b0b4d0` |
| `IOSkywalkPacket` | `0x198` | `completeWithQueue(IOSkywalkPacketQueue*, uint, uint)` | `0xffffff8002b0b6fa` |
| `IOSkywalkPacket` | `0x1e0` | `disposePacket()` | `0xffffff8002b0b854` |
| `IOSkywalkPacketBufferPool` | `0x140` | `deallocatePacket(IOSkywalkPacket*)` | `0xffffff8002b310de` |
| `IOSkywalkPacketBufferPool` | `0x148` | `deallocatePackets(IOSkywalkPacket**, uint)` | `0xffffff8002b31128` |

Additional local collection reported 14 symbol matches, including 32 lines of TX submission queue `free()`, 31 lines of TX completion queue `free()`, 32 lines of RX completion queue `free()`, 20 lines of base queue `free()`, 62 lines of pool `free()`, 91 lines of `enqueuePacketsForNetworking()`, 65 lines of `enqueuePacketsForKPipe()`, and 27 lines of `resetABFS()`. The actual **second disassembly file is not yet available for review**, so we cannot assert anything about their control flow or drain behavior.

**Critical ABI offset distinction:** In the `disable()` wrappers the indirect call operand is based on the raw `__ZTV` symbol (`lea ... __ZTV20IOSkywalkPacketQueue`), which includes two 8-byte Itanium metadata entries. The tool's `--offsets` switch instead takes offsets from the C++ **vptr**. Thus raw `__ZTV+0x168` maps to callable vptr `+0x158` (slot 43), **not** the printed `getWorkLoop()` at vptr `+0x168` (slot 45). Query this corrected slot before interpreting the disable target.


## 9. Review of RX and free() from second local 25G332 disassembly

**Source:** developer-provided `awdl-skywalk-remaining-evidence.txt`,
433 lines (13 substantive function bodies and allocation-view metadata),
2026-11-11. The report below paraphrases the supplied control flow; it does
not redistribute bulk proprietary disassembly.

### 9.1 Specialized free() paths

| Method | Evidenced operations | NOT established |
|---|---|---|
| `TxSubmissionQueue::free`, `0xffffff8002b11be2` | Destroys `IOSkywalkPacketTable` at `+0xe8`, frees metadata allocations at `+0x100` and `+0x68`, then tail-calls base `IOSkywalkPacketQueue::free` through raw vtable `+0xa0` | No explicit gate/callback drain, unregister or disable in this specialized method |
| `TxCompletionQueue::free`, `0xffffff8002b17a94` | Calls `this->vptr+0x1c0` **first**, frees pointer-array storage at `+0xe0` using `(+0xac) * 8` bytes, metadata at `+0xf8`, recursive lock at `+0x108`, then calls base free | `+0x1c0` target is **unresolved**; do not assume it drains callbacks. Storage size makes `+0xac` a per-queue pointer-buffer allocation count, but whether it exactly matches the `withPool` capacity parameter still needs `initWithPool` inspection |
| `RxCompletionQueue::free`, `0xffffff8002b13f4a` | Destroys `IOSkywalkPacketTable` at `+0xe8`; releases object at `+0xf8` using its vptr `+0x28`; frees allocation at `+0x100`; calls base free | No explicit callback-drain or source-queue removal in this specialized method |
| `IOSkywalkPacketQueue::free`, `0xffffff8002b0dc8e` | Clears `+0x70`, `+0x60`, releases pool pointer `+0x50` through vptr `+0x28`, clears it, then tail-calls superclass free | The shown body does not prove the event source was removed from the workloop, and it does not itself prove callbacks are quiescent |
| `IOSkywalkPacketBufferPool::free`, `0xffffff8002b0a45c` | Calls `_thread_call_cancel_wait` and `_thread_call_free` on pool's `+0xb0` thread call, then calls `releaseAllPackets` and `releaseAllMemorySegments`, releases auxiliary objects and frees locks | `cancel_wait` applies to this pool's thread call, **not** automatically to queue callbacks. Pool freeing is a potentially unsafe fallback if outstanding queues or packets remain |

**Critical lifetime implication:** The presence of a destructor/free chain
does **not** establish that `disable(); release();` waits for any callback
holding the backend's borrowed `refCon`. In Stage 4.2 the callback gate
never arms, so the live queue cannot be created at all. Keep this disabled.

The repeated `__ZTV20IOSkywalkPacketQueue + 0xa0` tail call in specialized
free() functions is measured from the raw table symbol, therefore maps to
object-vptr `+0x90` (slot 18). This must not be mixed up with
object-vptr `+0xa0`.

### 9.2 RX array: Networking

`IOSkywalkRxCompletionQueue::enqueuePacketsForNetworking` at
`0xffffff8002b1433c` iterates the requested array:

1. Obtains native packet handle at `IOSkywalkPacket+0x28`.
2. Links the native handles through `_kern_packet_set_next`, tracking head
   and tail.
3. Calls `IOSkywalkPacket::getDataLength`, `completeWithQueue(queue,2,0)`,
   then `disposePacket` for each wrapper; optionally calls BPF tap.
4. Accumulates byte counts and calls `_kern_netif_queue_rx_enqueue` once
   with the native list, count and flag `1`.
5. Returns **accumulated bytes**; the displayed function does **not**
   branch on the `_kern_netif_queue_rx_enqueue` return value.

**Interpretation limit:** `disposePacket()` is performed before the native
list is handed to networking; this is not proof of a use-after-free because
the Skywalk wrapper and the native `kern_packet_t` may have distinct
ownership. We must inspect `disposePacket()`, `completeWithQueue()` and
the networking enqueue ownership contract before treating the return as
fully acknowledged. The function's return is **not** a number of packets.

### 9.3 RX array: KPipe

`IOSkywalkRxCompletionQueue::enqueuePacketsForKPipe` at
`0xffffff8002b1447e`:

1. Iterates wrappers and calls `getSourceQueue`,
   `getDataLength`, `completeWithQueue(queue,2,0)`.
2. Advances/normalizes the index in the packet table at `+0xe8` by the
   **packet count** and updates statistics.
3. Calls a virtual method at queue vptr `+0x1e0` with zero argument.
   Prior KC vtable mapping resolves this to
   `IOSkywalkRxCompletionQueue::sendNotification(uint)`.
4. Returns accumulated **byte count**, not accepted packet count.

Unlike the Networking path, the shown KPipe function does not call
`disposePacket()` or `_kern_netif_queue_rx_enqueue`. Inspect source
queue/table ownership before exposing KPipe.

### 9.4 resetABFS is statistics/scheduling reset, not a drain

`IOSkywalkPacketQueue::resetABFS()` at `0xffffff8002b0de62`
clears fields in `this+0x68` and restores
`_gIOSkywalkABFSMinLimit` values while preserving selected fields.
The supplied 27 instructions show no callback wait, queue detach or packet
release. Therefore, **resetABFS must not be interpreted as callback
quiescence or packet return**.

### 9.5 Still blocked

- **Identified, not behaviorally verified:** `TxCompletionQueue::vptr+0x1c0` is `IOSkywalkTxCompletionQueue::finalize()` and raw `__ZTV20IOSkywalkPacketQueue+0x168` is `IOSkywalkPacketQueue::disable()` at object-vptr `+0x158`. Their names and addresses are confirmed; the `finalize()` body is not yet disassembled here.
- Inspect `IOSkywalkPacket::{completeWithQueue,disposePacket}` and pool
  `deallocatePacket(s)`, `releaseAllPackets`.
- Review `initWithPool` and the source-queue/table handoff to distinguish
  capacity, packet-buffer ownership and actual callback quiescence.
- Real `AWDL` and live datapath remain **disabled**.


## 10. Next local symbol mapping and packet-lifetime discovery (2026-11-11)

The developer ran `inspect_skywalk_vtables.py --classes IOSkywalkPacketQueue,IOSkywalkTxCompletionQueue --offsets 0x90,0x158,0x1c0` against the local `25G332` KC and reported:

| Class | vptr offset | Confirmed method | Address |
|---|---|---|---|
| `IOSkywalkPacketQueue` | `0x90` | `free()` | `0xffffff8002b0dc8e` |
| `IOSkywalkPacketQueue` | `0x158` | `disable()` | `0xffffff8002b0de34` |
| `IOSkywalkPacketQueue` | `0x1c0` | `finalize()` | `0xffffff8002b0ddcc` |
| `IOSkywalkTxCompletionQueue` | `0x90` | `free()` | `0xffffff8002b17a94` |
| `IOSkywalkTxCompletionQueue` | `0x158` | `disable()` | `0xffffff8002b17b58` |
| `IOSkywalkTxCompletionQueue` | `0x1c0` | `finalize()` | `0xffffff8002b17d6e` |

Thus the first virtual call made by the specialized `TxCompletionQueue::free()` at `this->vptr+0x1c0` resolves to **`TxCompletionQueue::finalize()`**. It is a potentially important teardown operation, but its name **does not establish** callback draining, no-future-entry, queue detachment, or packet ownership correctness. Its actual body and callers still need inspection.

The subsequent read-only collector run found 7 symbol entries with 6 nonempty method bodies:

- `IOSkywalkPacketBufferPool::deallocatePacket()` — 27 lines
- `IOSkywalkPacketBufferPool::deallocatePackets()` — 51 lines
- `IOSkywalkPacket::completeWithQueue()` — 53 lines
- `IOSkywalkPacket::disposePacket()` — 29 lines
- `IOSkywalkPacketBufferPool::releaseAllPackets()` — 27 lines
- `IOSkywalkPacketBufferPool::releaseAllMemorySegments()` — 19 lines

The seventh entry is a kalloc type-view metadata symbol with zero instructions.
**Update:** the actual `awdl-skywalk-packet-lifetime.txt` was subsequently uploaded and its control flow reviewed in section 11 below. The console-only status here is historical.

**Next:** collect `finalize()` bodies for the base and specialized queues using the read-only collector, and review the developer's local packet-lifetime evidence file. Keep AWDL runtime flags off.


## 11. Packet lifetime and queue finalize: full developer-uploaded disassembly

**Source:** two developer-uploaded local text files, `awdl-skywalk-packet-lifetime.txt`
and `awdl-skywalk-finalize.txt`, from the local 25G332 BootKernelExtensions.kc.
This is a paraphrased behavioral analysis; no bulk KC disassembly is
committed here. All addresses and offsets are specific to this KC.

### 11.1 Deallocation is a real native pool-return call

- `IOSkywalkPacketBufferPool::deallocatePacket(packet)` at
  `0xffffff8002b310de`: null packets are skipped; for non-null wrappers,
  it saves the native packet handle at wrapper `+0x28`; if state bit
  `+0x58 & 2` is set, invokes `completePreparedPacket`; then invokes
  virtual wrapper `disposePacket()` at vptr `+0x1e0` and tail-calls
  `_kern_pbufpool_free(pool->+0x18, savedNativeHandle)`.
- `IOSkywalkPacketBufferPool::deallocatePackets(array,count)` at
  `0xffffff8002b31128`: null pointer or zero count returns. Otherwise,
  **per element** it conditionally calls `completePreparedPacket`,
  replaces the pointer in the supplied array with the wrapper's native
  packet handle (wrapper `+0x28`), calls `disposePacket()`, and finally
  tail-calls `_kern_pbufpool_free_batch(pool->+0x18,array,count)`.
  It does **not** validate each element for null or return a per-packet
  success status in the shown path.
- This identifies the native pool return operations, strengthening the
  evidence that TX completion's pointer-array and linked-list overloads
  reclaim native handles. But it is **not** evidence that these calls may
  be invoked with unprepared/duplicate packets, nor an independent
  contract for when another owner may recycle the same address.
- **Important implementation detail:** batch deallocation overwrites the
  supplied pointer array in place with `kern_packet_t` values. The
  reviewed TX array overload uses its own queue-internal scratch array;
  code tracking packet-wrapper pointers must not assume that array
  still contains `IOSkywalkPacket*` after the call.

### 11.2 Completion and disposal are distinct from free-to-pool

- `IOSkywalkPacket::completeWithQueue(queue, mode, flags)` at
  `0xffffff8002b0b6fa`: iterates packet buffers and calls their
  virtual `+0x148`, then for `mode == 2` calls
  `_kern_packet_finalize(nativeHandle)` and, if `wrapper+0x38` exists,
  invokes a virtual callback `+0x220` on that object. It clears
  `wrapper+0x58 & 2` and finally tail-calls the **packet** virtual
  method at `+0x1a0`. The targets of packet `+0x1a0`, buffer
  `+0x148` and auxiliary-object `+0x220` are still unknown.
- `IOSkywalkPacket::disposePacket()` at `0xffffff8002b0b854`:
  iterates packet buffers and calls `disposePacketBuffer()` on each,
  then zeros the native packet-handle pointer `wrapper+0x28` and
  several wrapper metadata fields. **It does not itself call
  `_kern_pbufpool_free` or `_kern_pbufpool_free_batch`.**
- This matters for RX Networking: its handoff to
  `_kern_netif_queue_rx_enqueue` is after `disposePacket()` on
  wrappers. The native handles are saved in a separate linked list;
  whether their ownership is then always accepted/freed must be
  established from the netif enqueue callee and the caller contract.

### 11.3 Pool destructor is cleanup, not evidence of a safe drain

- `IOSkywalkPacketBufferPool::releaseAllPackets` at
  `0xffffff8002b0a57c` releases an owned object at `+0x60`,
  clears pool bookkeeping, and frees metadata storage at `+0x58`.
- `releaseAllMemorySegments` at `0xffffff8002b0a5d4` iterates
  an object collection at `+0x68` using a block callback, then
  releases the collection.
- Neither shown body waits for TX/RX callbacks. Calling pool-level
  release while another owner still holds native packets would
  remain a lifecycle hazard.

### 11.4 Queue finalize call graph and state counter

The base `IOSkywalkPacketQueue::finalize()` at
`0xffffff8002b0ddcc` does **not** merely close the event gate:

1. If `queue+0xbc` is zero, return success immediately.
2. Otherwise, decrement `queue+0xbc` and invoke virtual `queue-vptr
   +0x18` (method identity not yet resolved).
3. Depending on the virtual result and remaining count, possibly invoke
   `IOSkywalkPacketBufferPool::checkOutPacketQueue(pool,queue)`.
4. If the counter remains nonzero, return `0xe00002d5`; when it
   reaches zero, clear `queue+0xcc` and return success.

The observed count manipulation is **not** proof that all callback
invocations have exited, nor that the counter is an in-flight callback
counter. `checkOutPacketQueue` and the vptr `+0x18` target must be
identified before the counter can be interpreted as a safe-drain proof.

Specialized `finalize()` wrappers all enter `IOEventSource::closeGate`
when a workloop is present, invoke **base** finalize via raw
`__ZTV20IOSkywalkPacketQueue+0x1d0` (object-vptr `+0x1c0`), then
reopen the event-source gate:

| Method | After the base finalize | Implication |
|---|---|---|
| `TxSubmissionQueue::finalize` `0xffffff8002b11f16` | On base success, calls the queue's own virtual method `+0x2a0`, then clears `+0xf8`, `+0x13c`, `+0xe0` | Need name/body of `+0x2a0`; no explicit callback drain in this wrapper |
| `TxCompletionQueue::finalize` `0xffffff8002b17d6e` | On base success, clears `+0xe8`, `+0x110`. **Converts base return `0xe00002d5` into success (0)** without those resets | A reported success can mean the base queue is **not yet fully finalized**; do not use this return code alone as a no-callbacks guarantee |
| `RxCompletionQueue::finalize` `0xffffff8002b140f6` | On base success, calls `IOSkywalkPacketTable::flushAllPackets(table,pool)`, clears `+0xf0`, `+0x118`, `+0xe0` | Flushes stored table packets in the success case; still not a demonstrated prevention of future callback entry |

**Particularly dangerous observation:** `IOSkywalkTxCompletionQueue::free()`
calls its virtual `finalize()` without checking the returned status before
freeing buffer storage, stats and the lock. Since the specialized finalize
can translate `0xe00002d5` into 0, no safe no-future-callback guarantee
follows solely from a call to `free()`.

This is a risk statement about **unproven preconditions**, not proof
that Apple's own callers are incorrect: they may arrange the necessary
serialization/detach **before** calling `free()`.

### 11.5 Remaining conditions before live use

1. Resolve the object-vptr `+0x18` used in base queue finalize, the
   TX submission vptr `+0x2a0`, packet `+0x1a0` and packet-buffer
   `+0x148`. Resolve receiver class before naming a target.
2. Trace `checkOutPacketQueue`, packet-table
   `flushAllPackets`, and the owner workloop detach/removal that
   prevents callbacks from entering after `refCon` is freed.
3. Confirm the native networking enqueue ownership/failure contract for
   RX and that pool deallocation semantics are compatible with packet
   accounting, including partial batches and teardown.
4. Keep the experimental callback gate **disarmed**, allocation and AWDL
   registration hard-disabled, and all packet-ledger models host-only.

**Conclusion:** named packet-reclamation operations have been
confirmed in the local 25G332 KC, and queue finalize semantics are more
complex than a simple drain. The key no-future-callback lifetime
invariant remains unverified. Do not install or load experimental AWDL.


## 11. Packet and queue finalize behavior verified from uploaded source

The full `awdl-skywalk-packet-lifetime.txt` and `awdl-skywalk-finalize.txt` were reviewed. Detailed analysis: [STAGE4_4_PACKET_FINALIZE_REVIEW.md](STAGE4_4_PACKET_FINALIZE_REVIEW.md). Key findings:

- `deallocatePacket(s)` invokes native `_kern_pbufpool_free(_batch)` after wrapper disposal. The batch form **mutates its incoming packet-pointer array** into native packet handles before freeing.
- `IOSkywalkPacket::completeWithQueue` contains a mode-2 `_kern_packet_finalize` path and invokes additional unknown buffer and packet vtable methods; wrapper `disposePacket` alone does not free the native packet.
- `IOSkywalkPacketQueue::finalize` manages state/counter `+0xbc` and optionally calls pool `checkOutPacketQueue`. This counter is **not proven to count in-flight callbacks**.
- Specialized `TxCompletionQueue::finalize` maps base code `0xe00002d5` to success **without its normal field-clearing branch**. In the earlier disassembly, `TxCompletionQueue::free` ignores `finalize`'s return value and frees support buffers regardless. No callback quiescence guarantee is established.
- `RxCompletionQueue::finalize` invokes `IOSkywalkPacketTable::flushAllPackets` after base success; internals remain to be examined.

No live TX/RX, AWDL interface registration, or callback lifetime change is authorized.



## 12. Packet-table flush and pool queue checkout (2026-11-11)

**Source:** developer-provided console disassembly from local Tahoe
`BootKernelExtensions.kc`, for three precisely named methods.
Only interpretation and key call names are recorded, not raw code.

### 12.1 `IOSkywalkPacketBufferPool::checkOutPacketQueue`

Address `0xffffff8002b0ab92`:

- Acquires `IORecursiveLockLock(pool+0x80)`.
- If `pool+0x90` is not null, invokes a virtual method at offset
  `+0x1d8` on that object, passing the candidate queue.
- Creates an `OSCollectionIterator` over `pool+0xa0`; examines
  contained objects (including a field at `object+0x88` compared against
  the pool), conditionally calls a virtual removal method `+0x1d8`
  on the collection, and completes iteration.
- Releases `pool+0x80` lock on exit.

This is evidence of **pool-side queue registration/collection bookkeeping**
under a lock. It does **not** demonstrate that the queue's workloop
was detached or that external callers have stopped invoking the queue
callback. The identity of the objects at `pool+0x90` and `pool+0xa0`
requires inspection before calling this a full unregister barrier.

### 12.2 `IOSkywalkPacketTable::flushAllPackets`

Address `0xffffff8002b0e7bc`:

1. Computes remaining packet count in the ring/table using fields at
   `table+0`, `table+4` and the current read index at `table+0x80`.
2. For every remaining `IOSkywalkPacket*`, invokes
   `IOSkywalkPacket::cancelCompletionCallback()` (at
   `0xffffff8002b0b8d4`).
3. Passes the same contiguous ring segment and count to pool virtual
   offset `+0x148`, previously resolved as
   `IOSkywalkPacketBufferPool::deallocatePackets()`. That method
   mutates the segment from wrapper pointers to native packet handles
   before calling native `_kern_pbufpool_free_batch`.
4. Advances the table read index, including wraparound, and repeats
   until no pending entries remain.

**This is a real cleanup path for queued RX packets**, confirmed by
the earlier `RxCompletionQueue::finalize()` call to `flushAllPackets`
on base-finalize success. The local body offers no separate per-packet
error/status handling for pool batch deallocation. Its cancellation
refers to a **per-packet completion callback**, not necessarily the
TX submission queue's function pointer/refCon.

**Important:** because the packet-pointer table segment is mutated
in place by `deallocatePackets`, the ring must not be read again as
`IOSkywalkPacket*` until its entries are populated afresh.

### 12.3 `IOSkywalkPacketBuffer::disposePacketBuffer`

Address `0xffffff8002b0be8c`: stores zero in packet-buffer fields
`+0x30`, `+0x38`, `+0x40`, `+0x48`, and `+0x78`.
No native packet pool-free, dynamic free or callback wait appears
in this 13-instruction body. It is a **wrapper metadata reset**,
not proof that native packet buffers have been freed.

### 12.4 Open questions

- The body of `IOSkywalkPacket::cancelCompletionCallback()` remains
  unreviewed. Investigate where the completion callback is stored
  and whether cancel prevents later delivery, without conflating
  it with the TX submission queue's `TxSubmissionAction`.
- Base queue finalize invokes `checkOutPacketQueue`, but pool
  collection checkout is not a proven workloop callback-quiescence
  barrier.
- The earlier request for the vptr mapping of base queue `+0x18`,
  packet `+0x1a0`, packet buffer `+0x148`, and TX submission
  queue `+0x2a0` still needs a local result.
- No evidence yet proves safe `SkywalkBackend` or
  `TxCallbackContext` destruction after `disable()/release()`.

**Safety conclusion:** keep all Stage 4.2/4.3 runtime gates disabled;
do not infer that canceling RX packet completion callbacks also
quiesces the TX submission callback.



## 13. Retain-count and packet completion callback mappings (2026-11-11)

**Evidence:** developer-provided local vtable mapping and three function
bodies from `BootKernelExtensions.kc` on Tahoe 25G332. This section
paraphrases the relevant operations only.

### 13.1 Previously unknown virtual targets, now resolved

| Object and vptr offset | Resolved method | KC address |
|---|---|---|
| `IOSkywalkPacketQueue +0x18` | `OSObject::getRetainCount() const` | `0xffffff8000a1df50` |
| `IOSkywalkTxSubmissionQueue +0x2a0` | `IOSkywalkTxSubmissionQueue::purgePackets()` | `0xffffff8002b12870` |
| `IOSkywalkPacket +0x1a0` | `IOSkywalkPacket::complete(IOSkywalkPacketQueue*,unsigned)` | `0xffffff8002b0b6ae` |
| `IOSkywalkPacketBuffer +0x148` | `IOSkywalkPacketBuffer::completeWithQueue(IOSkywalkPacketQueue*,unsigned,unsigned)` | `0xffffff8002b0bde8` |

The base queue `finalize()` virtual call `this->vptr+0x18`
therefore reads **OSObject retain count**, not a callback-drain
counter or an independent queue operation. The base method still
decrements **its own field `queue+0xbc`**, and may call pool
`checkOutPacketQueue` depending on the retain count and remaining
`+0xbc`. These two counters must not be conflated. The precise
meaning of `+0xbc` is still unverified.

The specialized TX submission `finalize()` calls
`purgePackets()` on the successful base-finalize path, but
`purgePackets` body and race behavior remain to be inspected.

The `IOSkywalkPacket::completeWithQueue` path tail-calls
`IOSkywalkPacket::complete()` and sends packet-buffer notifications
through `IOSkywalkPacketBuffer::completeWithQueue()`.
Those targets are now named but their actual bodies/side effects
still require inspection.

### 13.2 Completion callback cancellation merely clears a field

`IOSkywalkPacket::cancelCompletionCallback()` at
`0xffffff8002b0b8d4` is exactly a store of null to
`this+0x38`. `IOSkywalkPacketBuffer::cancelCompletionCallback()`
at `0xffffff8002b0beaa` is exactly a store of null to
`this+0x48`.

Neither routine visibly takes a lock, waits for callback work, or
releases a reference in its small body. A callback that already
captured the previous pointer may still be running. This is
**per-packet** completion cancellation, not prevention of new
`TxSubmissionQueue` callbacks using the queue's `refCon`.

The RX flush's call to `cancelCompletionCallback()` must not be
described as a global callback quiescence barrier.

### 13.3 `IOSkywalkPacketBufferPool::checkInPacketQueue`

Method `0xffffff8002b0aafe`:

1. Acquires recursive pool lock at `pool+0x80`.
2. Creates a pool-owned `OSSet` at `pool+0x90` if absent
   (initial capacity 8), then calls virtual offset `+0x1c0`
   on that `OSSet` with the queue. This is consistent with
   inserting/retaining a queue in the set; the exact vtable
   method name has not been independently mapped.
3. Unlocks `pool+0x80`.
4. Calls `queue->getInterface()`. If there is no interface
   and the set insertion returned nonzero, schedules the pool's
   deferred task through `_thread_call_enter(pool+0xb0)`.

This is evidence of **asynchronous pool-side queue housekeeping**
which may run after `checkInPacketQueue` returns. Pool
`free()` cancels its own `thread_call` using
`_thread_call_cancel_wait`, but this does **not** establish
that TX submission workloop callbacks are also canceled.
The owner-side queue deregistration and scheduling order must be
examined before callback context release.

The earlier `checkOutPacketQueue` removes queue-related
registrations under the pool lock. This is separate from the
TX submission callback's workloop entry path.

### 13.4 Outstanding high-risk invariant

**Before freeing `TxCallbackContext`, prove both:**
- No *future* callback can enter and dereference this context,
  accounting for workloop events and deferred scheduling.
- Every callback already admitted has returned (and any completion
  callbacks referencing its objects have been canceled or drained).

A disarmed `CallbackGate`, a zero in-flight admission count,
`purgePackets()`, or `finalize()` **alone** is not that proof.
The experimental kext remains fail-closed.

### 13.5 Next local evidence

Collect `IOSkywalkTxSubmissionQueue::purgePackets()`,
`IOSkywalkPacket::complete()`, and
`IOSkywalkPacketBuffer::completeWithQueue()` bodies and inspect
`TxSubmissionQueue::initWithPool` to establish callback/refCon
storage and scheduling. Do not change runtime flags.



## 14. TX purge and completion routing from local 25G332 KC (2026-11-11)

**Evidence:** the developer-provided local log of `OSSet` vtable names,
`IOSkywalkTxSubmissionQueue::purgePackets` and its `.cold.1` body,
`IOSkywalkPacket::complete`, `IOSkywalkPacketBuffer::complete`,
and both `IOSkywalk{Packet,PacketBuffer}::completeWithQueue` bodies.
Only summarized behavior is recorded in this public report, not raw disassembly.

### 14.1 OSSet operation names independently confirmed

`OSSet` object-vptr offsets:

| vptr offset | Method | Local KC address |
|---|---|---|
| `0x1c0` | `OSSet::setObject(OSMetaClassBase const*)` | `0xffffff8000a49760` |
| `0x1d8` | `OSSet::removeObject(OSMetaClassBase const*)` | `0xffffff8000a49860` |
| `0x1e0` | `OSSet::containsObject(OSMetaClassBase const*) const` | `0xffffff8000a498d0` |

This resolves the previously unidentified set operations used by
`PacketBufferPool::checkInPacketQueue()` and
`checkOutPacketQueue()`. The pool keeps a set of registered queues
and a related collection; this is evidence of registration
bookkeeping, **not** automatic proof that the workloop stops
scheduling the queue callback.

### 14.2 TX submission purge: two stores of outstanding packets

`IOSkywalkTxSubmissionQueue::purgePackets()` at
`0xffffff8002b12870` returns immediately if the event-source
workloop pointer at `queue+0x30` is null. Otherwise it calls its
`.cold.1` helper at `0xffffff8002b32015`.

Within the workloop-attached case, the helper:

1. Calls `IOEventSource::closeGate`.
2. Calls `IOSkywalkPacketTable::flushAllPackets` with the TX
   submission table at `queue+0xe8` and pool at `queue+0x50`.
   The previously reviewed flush method cancels per-packet
   completion callbacks and batch-deallocates pending entries.
3. Separately walks a linked packet chain from `queue+0x108`:
   obtains `getNextPacket`, unlinks each wrapper, invokes
   `cancelCompletionCallback`, and invokes the pool's
   `deallocatePacket` (object-vptr `+0x140`) for each packet.
4. Clears queue fields `+0x150`, `+0x110`, and `+0x108`.
5. If `queue+0xf8` is set, calls `_ifnet_purge`.
6. Reopens the event-source gate.

**New insight:** TX cleanup handles **both** the ring/table and
the separate linked list, and the native pool return is real.
However, `purgePackets()` is called on the successful
`TxSubmissionQueue::finalize()` path; the shown code does not
prevent a new queue callback from starting **after** the gate is
reopened, nor does it itself prove driver-side callback refCon
lifetime safety. The no-workloop early return must not be
interpreted as proof of no outstanding packets without other
initialization/finalization invariants.

### 14.3 Packet and buffer completion routing

`IOSkywalkPacket::complete(IOSkywalkPacketQueue*, unsigned)`
at `0xffffff8002b0b6ae`:

- Returns 0 immediately when wrapper flag `+0x58 & 0x2` is clear.
- If set, obtains direction from `queue->getDirection()` when
  the queue is non-null, or uses direction 0.
- Tail-calls the packet virtual `+0x198`, resolved as
  `IOSkywalkPacket::completeWithQueue`, passing direction and flags.

`IOSkywalkPacketBuffer::complete(unsigned)` at
`0xffffff8002b0bd14` is a short no-op returning 0.
This does not mean `completeWithQueue()` is a no-op.

`IOSkywalkPacketBuffer::completeWithQueue(queue,direction,flags)`
at `0xffffff8002b0bde8`:

- If direction/mode equals 2 and buffer field `+0x30` is
  non-null, calls `IOSkywalkPacketBuffer::finalize()`.
- Clears buffer flag `+0x78 & 0x2`.
- If the object pointer stored at `buffer+0x48` is non-null,
  dispatches a virtual callback at that object's vptr `+0x228`.
- Returns 0.

The outer `IOSkywalkPacket::completeWithQueue()` iterates
buffers through the same method, may call native
`_kern_packet_finalize` when direction/mode is 2, may invoke
an additional callback via packet `+0x38`, clears state
and tail-calls `IOSkywalkPacket::complete()` (guarded by its
prepared-bit check). The exact callback owners and their
lifetime synchronization remain unverified.

### 14.4 Implications and remaining blockers

- `cancelCompletionCallback()` clears object pointers but
  does not wait for callbacks that are already executing.
- `purgePackets()` provides real outstanding packet cleanup,
  but it is **not** proven to revoke the TX Submission Queue
  callback function/refCon or remove the event source from
  its owning workloop.
- Pool registration set operations and deferred pool tasks
  must be included in any eventual teardown ordering.
- `TxCallbackContext` currently lives **inside** `SkywalkBackend`
  and the backend is not retain-counted by Skywalk. Freeing the
  backend while a callback can still dereference refCon would
  create a use-after-free even if the callback gate was closed.

**Recommendation for Stage 4.5:** trace queue initialization and
workloop callback registration/removal from the **caller side**,
then write an explicit two-phase `detach -> no future entry ->
drain -> destroy` protocol. Do not wire live Skywalk queues,
claim packets, or flip `ITLWM_AWDL_DATAPATH_LIVE` until those
guarantees have independent evidence.



## 15. TX submission callback registration and rescheduling (local 25G332 KC)

**Evidence:** developer-provided `awdl-skywalk-tx-entry.txt`,
locally extracted from `BootKernelExtensions.kc` (2026-11-11).
The uploaded report contains disassembly for:
`TxSubmissionQueue::initWithPool`, `checkForWork`, and two
`packetSubmission` overloads. Here are the key control-flow
findings, without publishing proprietary instruction dumps.

### 15.1 initWithPool: the queue stores two separate callback paths

`IOSkywalkTxSubmissionQueue::initWithPool`
at `0xffffff8002b11aac`:

- Rejects capacity argument `< 8`, null pool,
  null **packet-array submission callback** and
  flags `> 0x1f`.
- Dispatches through the **raw** base
  `__ZTV20IOSkywalkPacketQueue + 0x240` entry, which corresponds to
  object-vptr offset `+0x230`. The base method's exact identity and
  field assignments should be checked, not inferred solely from the
  call name in this specialized method.
- On success stores the first queue-specific unsigned argument
  in `queue+0xb8` and the **optional budget/space callback**
  (the first callback argument in this signature, at
  `[rbp+0x10]`) at `queue+0xf0`.
- Creates a packet table with `queue+0xac` as its capacity,
  stores it at `queue+0xe8` and allocates stats at `+0x100`.
  The field `+0xac` is therefore **definitely used as the
  allocation/table size here**; its exact mapping to the
  external `withPool` capacity argument still requires the
  base init method.
- The mandatory packet-array callback (second function-pointer
  parameter), owner, and `refCon` are passed to the base init
  through the stack. Prior `legacyDequeue` evidence shows an
  indirect packet callback at queue `+0x20`, the callback target
  at `+0x18` and the `refCon` at `+0x58`.
  Whether these are all retained/cleared during teardown must
  be verified against base init and owner detach.

**Important:** The optional `queue+0xf0` callback used for a
budget/space calculation has **different signature and semantics**
from the mandatory packet-array submission callback reached by
`legacyDequeue()`. Do not conflate them or treat the former's
return as a count of packets consumed.

### 15.2 checkForWork: pending generation and state-gated calls

`IOSkywalkTxSubmissionQueue::checkForWork()`
at `0xffffff8002b31746` initially compares event-generation
fields `queue+0x14c` and `+0x148`, and returns when equal.
On work, it updates the observed value and follows a mixture
of feature flags, statistics and queue states. Paths include:

- virtual queue `+0x1d8` on one configuration;
- `packetSubmissionForKPipe(false)` in another branch;
- queue virtual `+0x1e0` when state bits at `+0x28` and
  `+0xcc` permit it.

This shows a second scheduler/entry mechanism for TX work
besides direct calls to the packetSubmission methods, and is
consistent with deferred event-source work. Exact vptr targets
need separate lookup.

### 15.3 packetSubmission(bool): work can be deferred

`IOSkywalkTxSubmissionQueue::packetSubmission(bool)`
at `0xffffff8002b31d68`:

- Checks queue enable flag `+0x28` and on a true argument
  tries `IOEventSource::tryCloseGate()`.
- On failure to acquire the gate, increments a work-generation
  field `+0x148` and invokes
  `IOEventSource::signalWorkAvailable()` instead of entering
  packet processing synchronously.
- The local packet path checks state and capacity, invokes the
  optional budget callback at `queue+0xf0` when non-null,
  can call `refillTxRingForNetworking`, then calls
  `legacyDequeue()`; the alternate path uses
  `getPacketListForNetworking` then `listDequeue()`.
- It reopens the gate on the acquired-gate path.

**Lifetime consequence:** the ability to queue work via
`signalWorkAvailable()` means merely closing a driver-owned
`CallbackGate` and observing zero admitted callbacks is not
sufficient to prove the workloop event source can never run
again and dereference the queue's callback/refCon.

### 15.4 packetSubmission(out pointers): another gated path

`IOSkywalkTxSubmissionQueue::packetSubmission(uint64_t*,
uint32_t*,uint32_t*)` at `0xffffff8002b31b2a`
checks queue enable state, calls `IOEventSource::closeGate()`,
optionally invokes the same budget callback `queue+0xf0`,
then processes native packet handles via
`IOSkywalkPacketBufferPool::getPacketWithHandle`,
`IOSkywalkPacketTable::producePacket`, and
`legacyDequeue()`. It reopens the gate on its active path.
No demonstrated no-future-entry barrier is present in this
single function.

### 15.5 Next proof obligation: owner-side detach

The above establishes **a callback invocation path plus a
deferred work-event path**, not the final ownership contract.
Before a Stage 5 live callback, independently establish:

1. Which owner/workloop calls `addEventSource` and later
   `removeEventSource`, with correct locking and lifetime.
2. Whether `removeEventSource` guarantees no later
   `checkForWork` / `packetSubmission` / `legacyDequeue`
   invocations with borrowed `refCon`.
3. Drain all callbacks that were already admitted, including
   direct calls from the owner and deferred events, **after**
   future entry is ruled out.
4. Preserve the existing Stage 4 fail-closed
   `ITLWM_AWDL_DATAPATH_LIVE=0` and registration disabled.
