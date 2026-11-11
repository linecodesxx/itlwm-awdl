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



## 16. Base queue initialization and IOWorkLoop event source delegation (2026-11-11)

**Source:** developer-uploaded `awdl-skywalk-workloop-lifetime.txt`:
the 25G332 vtable mapping and the bodies of `IOSkywalkPacketQueue::initWithPool`,
`IOWorkLoop::{addEventSource,removeEventSource,signalWorkAvailable}`,
and `IOEventSource::signalWorkAvailable`. Only interpreted behavior is
recorded; do not publish the KC disassembly.

### 16.1 Virtual slot names checked

| Class | Object-vptr offset | Method | 25G332 address |
|---|---|---|---|
| `IOSkywalkPacketQueue` | `0x230` | `initWithPool(...)` | `0xffffff8002b0dbc8` |
| `IOSkywalkTxSubmissionQueue` | `0x230` | inherits base `initWithPool(...)` | `0xffffff8002b0dbc8` |
| `IOSkywalkPacketQueue` | `0x1d8` | `packetSubmission(bool)` | `0xffffff8002b0ded4` |
| `IOSkywalkTxSubmissionQueue` | `0x1d8` | `packetSubmission(bool)` | `0xffffff8002b31d68` |
| `IOSkywalkPacketQueue` | `0x1e0` | `sendNotification(uint)` | `0xffffff8002b0dee0` |
| `IOSkywalkTxSubmissionQueue` | `0x1e0` | `sendNotification(uint)` | `0xffffff8002b31afe` |

These resolve the previously unnamed virtual calls in
`TxSubmissionQueue::checkForWork()`. In particular,
queue vptr `+0x1d8` can invoke `packetSubmission(bool)` and hence
the driver's packet callback via `legacyDequeue()`.

### 16.2 Base queue initializer saves the borrowed refCon

`IOSkywalkPacketQueue::initWithPool`, address
`0xffffff8002b0dbc8`, validates a queue-type selector
between 1 and 3 (inclusive) and an integer option less than
`0x10000`. It then calls a superclass initializer via an
indirect virtual and, on success:

- Stores the **exact refCon pointer** passed on the stack
  (the final argument) at **`queue+0x58`**.
- Stores `queue+0xac =` the `r8d` capacity passed to this
  initializer. This confirms the field used for packet-table
  sizing, completion pointer-buffer sizing, and the strict
  `count < queue+0xac` enqueue check.
- Writes queue mode/parameters at `+0xb4`, `+0xa8`
  and `+0xb0`.
- Stores the pool pointer at `queue+0x50` and invokes
  its virtual `+0x20` (consistent with retain). The
  base queue `free()` later releases this pool pointer.
- Initializes event/queue state, including `+0x28`
  disabled and `+0xcd` set.

Crucially, the **refCon is only stored as an untyped pointer**
in this initializer. There is no visible retain of the backend
context; the eventual owner must guarantee that it outlives
every callback, including calls that enter after a gate
closes. The callback action itself is supplied to the
superclass IOEventSource initialization; the exact setter
needs inspection before assigning its storage field
from this body alone.

### 16.3 WorkLoop add/remove are delegated operations

`IOWorkLoop::addEventSource(IOEventSource*)` at
`0xffffff8000acaf80` is more than a direct list insertion:
it performs special-case checks before dispatching via
the object at `workLoop+0x20` and a virtual `+0x1c0`
with an operation argument of 0.

`IOWorkLoop::removeEventSource(IOEventSource*)` at
`0xffffff8000acb070` tail-calls **that same virtual
`+0x1c0`** on `*(workLoop+0x20)`, but passes an
operation argument of **1**.

The methods' wrappers do not themselves show the
actual attach/detach algorithm or a callback-drain wait.
The class of the `workLoop+0x20` object and its virtual
`+0x1c0` target must be resolved against local KC
vtable evidence. Until then, `removeEventSource` cannot
be asserted to be a safe `refCon` lifetime barrier.

There is also **no evidence in the supplied queue
initializer** that it itself calls `addEventSource`:
that is a higher-level owner operation which must be traced.

### 16.4 WorkLoop is explicitly woken for pending work

`IOEventSource::signalWorkAvailable()` at
`0xffffff8000acc880` loads the workloop pointer at
`eventSource+0x30` and makes an indirect call via
its virtual `+0x170`.

`IOWorkLoop::signalWorkAvailable()` at
`0xffffff8000acb290` takes a spinlock when its lock
field is present, marks pending work at `workLoop+0x38`,
and invokes `_thread_wakeup_thread` on its workloop
thread pointer `+0x30`; it releases the spinlock
afterwards. The shown method does **not** wait for a
callback, drain events, or release an event-source refCon.

These two bodies directly confirm a **deferred workloop
wake-up path**, which must be considered alongside direct
submission callbacks during teardown.

### 16.5 Exact remaining proof obligations

1. Resolve the class of `workLoop+0x20` and its
   virtual `+0x1c0` operation with local KC vtables
   (including possible `IOCommandGate`).
2. Trace where the queue owner calls `addEventSource`
   and `removeEventSource` and whether the latter
   synchronously prevents **future** `checkForWork` and
   direct `packetSubmission` callbacks.
3. Verify queue/owner reference ordering, shared
   workloop gate serialization and callback-drain
   semantics before freeing `SkywalkBackend::txCallbackContext_`.
4. Do not assume `removeEventSource` alone handles
   deferred pool tasks from `checkInPacketQueue`.
5. Keep all AWDL runtime gates off, no kext installation,
   packet delivery or interface registration.

**Evidence status:** Native packet return and stored callback
parameters are verified. A no-future-entry guarantee is
**not** verified.



## 17. IOCommandGate dispatch and detach sequencing (2026-11-11)

**Evidence:** developer-uploaded local `awdl-workloop-dispatch.txt`
for Tahoe 25G332, containing vtable mappings and method disassembly
for `IOCommandGate::{runAction,runCommand,setWorkLoop}`,
`IOWorkLoop::{runAction,removeEventSource}`, and
`IOEventSource::setWorkLoop`. The interpretation below does not
redistribute proprietary KC instructions.

### 17.1 Workloop's internal +0x20 target: command gate dispatch

- `IOCommandGate` vptr `+0x1c0` is
  `IOCommandGate::runCommand(void*,void*,void*,void*)`,
  at `0xffffff8000ace050`.
- `IOWorkLoop::removeEventSource` delegates to
  `*(workloop+0x20)` vptr `+0x1c0`, with the first
  command argument set to **1** and the event source as
  another argument. Together with the vtable result,
  this is consistent with `workloop+0x20` being its
  internal command gate (the object's concrete dynamic
  class was not separately proven).
- `IOCommandGate::runCommand` is a **12-instruction
  forwarding wrapper**: rearranges registers, passes
  the saved **action function pointer** from `gate+0x20`
  to `gate->vptr+0x1c8`. The owner's object pointer
  is instead stored at `gate+0x18`.
  **Update (2026-11-11):** the local vtable maps
  `gate->vptr+0x1c8` to `IOCommandGate::runAction`.
  The saved action pointer itself has not yet been mapped
  to its 25G332 KC implementation.
- The `IOWorkLoop` class itself has a reserved symbol
  at vptr `+0x1c0`; that is *not* the callback target
  of the `IOCommandGate` object at `workloop+0x20`.

### 17.2 Gate synchronization mechanisms do exist

`IOCommandGate::runAction` at
`0xffffff8000ace080`:

- Rejects a null action and handles a missing workloop.
- Uses workloop virtual calls `+0x180`, `+0x130`
  for gate entry / current-gate-state checks; when not
  immediately in gate, the method can sleep using
  workloop vptr `+0x190` and later wake through
  vptr `+0x198`.
- Calls the action pointer while in the gated path,
  then leaves using workloop vptr `+0x178`.
- Maintains a gate-related state word at
  `IOCommandGate+0x48`. A teardown branch can call
  the event source's vptr `+0x158`, clear its
  workloop pointer `+0x30` and then leave the gate.

`IOWorkLoop::runAction` at
`0xffffff8000acb5f0` similarly enters and exits
its gate around the supplied function pointer.

**Update:** the developer's local vtable identifies
`IOCommandGate::runAction` as virtual slot `+0x1c8`
on this KC. Therefore the control gate's
`runCommand -> runAction` portion of the call chain is
**confirmed**. The specific saved workloop-maintenance action
and the Skywalk owner's detach order are not yet independently
verified.

### 17.3 Event source and command gate detach

- `IOEventSource::setWorkLoop(nullptr)` at
  `0xffffff8000acc390` invokes the event source's
  virtual `+0x158` before clearing `source+0x30`.
  The earlier Skywalk queue vtable mapping identifies
  its own `+0x158` as `disable()`. Confirm the
  relevant IOEventSource/IOCommandGate slot separately
  before generalizing to every subclass.
- `IOCommandGate::setWorkLoop(nullptr)` at
  `0xffffff8000acddf0` enters the former workloop's
  gate, marks an internal detach flag at `gate+0x48`,
  and may wait with `IOEventSource::sleepGate` while
  a state bit remains set, then disables/clears its
  workloop pointer on a suitable path. It eventually
  exits the workloop gate.
- The shown `IOCommandGate::setWorkLoop(nullptr)`
  body does **not** establish that the owner's Skywalk
  submission callback will never be called directly
  from another thread.

### 17.4 Safety consequence

The new evidence makes a **synchronous detach protocol
plausible** but does not establish its complete preconditions
or that the proprietary Skywalk queue owner calls it before
releasing the borrowed `refCon`.

The exact remaining questions:

1. Resolve `IOCommandGate` virtual `+0x1c8`,
   and all gate/workloop slot names `+0x130`,
   `+0x158`, `+0x178`, `+0x180`,
   `+0x190`, `+0x198`.
2. Inspect the actual `+0x1c8` operation handler
   and determine whether removal occurs under gate,
   how it handles current/pending work, and its return
   semantics.
3. Find the higher-level caller of
   `IOWorkLoop::removeEventSource(queue)` in
   the relevant owner-side queue teardown.
4. Independently rule out **direct** callers of
   `packetSubmission` / `legacyDequeue` after
   detach; then drain admitted callbacks before
   freeing `SkywalkBackend::txCallbackContext_`.

`ITLWM_AWDL_DATAPATH_LIVE=0` and
`ITLWM_AWDL_REGISTRATION_ENABLED=0` remain hard gates.
No native driver, kext, EFI or hardware changes are
justified by the current analysis.



## 18. Command-gate target resolved; workloop action is next (2026-11-11)

**Grounding:** user ran the vtable inspection locally on
macOS Tahoe `25G332`, revealing the following:

| Class / vptr offset | Confirmed target |
|---|---|
| `IOCommandGate +0x1c8` | `IOCommandGate::runAction` at `0xffffff8000ace080` |
| `IOCommandGate +0x158` | `IOCommandGate::disable` at `0xffffff8000acdfe0` |
| `IOWorkLoop +0x178` | `IOWorkLoop::openGate` |
| `IOWorkLoop +0x180` | `IOWorkLoop::closeGate` |
| `IOWorkLoop +0x130` | `IOWorkLoop::onThread` |
| `IOWorkLoop +0x190` | `IOWorkLoop::sleepGate` |
| `IOWorkLoop +0x198` | `IOWorkLoop::wakeupGate` |
| `IOEventSource +0x158` | `IOEventSource::disable` |

### Local call sequence, now resolved

```text
IOWorkLoop::removeEventSource(queue)
  -> workloop->controlG->runCommand(op=1, queue)
  -> IOCommandGate::runCommand
  -> IOCommandGate::runAction(gate+0x20 action, op=1, queue)
  -> gate-owner (gate+0x18) + saved action
```

Unlike an earlier report, `gate+0x20` is the **action function
pointer** and `gate+0x18` is the **owner** pointer.
`IOCommandGate::runAction` synchronizes the call under
the workloop's recursive gate. This provides a plausible
serialization boundary for events *executed under the same
workloop gate*, but is **not** yet proof against direct
packet submission calls outside that gate or against callbacks
that dereference a freed backend context.

### Comparison against public Apple XNU source

Apple's public `IOWorkLoop.cpp` implements
`IOWorkLoop::init` by creating a command gate whose
action is `IOWorkLoop::_maintRequest`.
The public `_maintRequest` `mRemoveEvent` handler
unlinks the source from the workloop's active/passive
chain, calls `setWorkLoop(nullptr)`, clears its
`next` link, and releases the workloop-held reference,
under command-gate serialization.

Reference (public XNU **not a confirmed exact 25G332
binary match**):
https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOWorkLoop.cpp

This is a valuable architectural cross-check, **not** a
substitute for checking `IOWorkLoop::_maintRequest`
against the user's local KC and finding the actual
Skywalk owner calls to `removeEventSource`.

### Remaining requirements for a future live backend

1. Disassemble local `IOWorkLoop::_maintRequest` and
   optionally `IOWorkLoop::init`; check exact owner,
   callback storage and source removal status handling.
2. Identify direct TX submission producers in addition
   to workloop `checkForWork`, and define owner-side
   disable, detach and no-future-entry semantics.
3. Prove all admitted callbacks exit **after** that
   barrier, including failed detach/removal branches.
4. Do not let the currently embedded
   `SkywalkBackend::txCallbackContext_` be destroyed
   before these invariants are proven.
5. Keep `ITLWM_AWDL_DATAPATH_LIVE=0` and
   AWDL registration disabled. No live packet processing.



## 19. Local workloop maintenance verified in 25G332 (2026-11-11)

**Source:** developer-uploaded local Tahoe 25G332 KC disassembly for
`IOWorkLoop::_maintRequest()` (176 instructions, address
`0xffffff8000acaac0`), `IOWorkLoop::init()` (111 instructions,
`0xffffff8000aca680`) and `IOWorkLoop::runEventSources()`
(181 instructions, `0xffffff8000acb650`). This section records
analysis only, not any extracted Apple KC instructions.

### 19.1 Remove operation=1 confirmed in the local binary

`IOWorkLoop::_maintRequest(owner, operation, source, ...)`
takes `operation=1` from
`IOWorkLoop::removeEventSource(source)` via the internal
`IOCommandGate::runCommand -> runAction` path.

In the local KC, the remove branch:

1. Calls event-source virtual `+0x168` to inspect the source's
   workloop association. If absent, it can return 0 without
   modifying either event chain.
2. Calls `IOStatistics::detachWorkLoopEventSource`
   on the associated bookkeeping.
3. Finds the source in one of the workloop's linked chains,
   including a primary chain rooted at `workloop+0x18`
   and a special secondary chain via `workloop+0x40`.
   Uses the source's virtual `+0x138` to traverse
   and `+0x130` to relink around it.
4. On the successful found/unlink path, calls source vptr
   `+0x128` with `nullptr` (consistent with
   `IOEventSource::setWorkLoop(nullptr)`), then source
   vptr `+0x130` with `nullptr`
   (consistent with `IOEventSource::setNext(nullptr)`).
5. Calls source virtual `+0x28` (OSObject `release()`),
   sets workloop bookkeeping byte `+0x39` bit 0
   (rescan/restart flag) and returns success 0.

A linked-source search failure can return `0xe00002c2`.
Because an already-unattached source can also return 0,
a bare zero status **does not by itself prove a source
was previously attached**. It should be paired with a
known successful attach and appropriate owner state.

The add operation (`operation=0`) retains the source,
associates it with the workloop, links it into an event
chain, updates statistics and sets the same rescan flag.
This is consistent with the public XNU `_maintRequest`
description but is now independently backed by local
25G332 control flow.

### 19.2 Workloop's own command gate is created in init

`IOWorkLoop::init()` obtains an `IOCommandGate` from
its metaclass, initializes it with the workloop object
as owner plus a function pointer loaded from the
workloop vtable at `+0x118`, and stores that gate
at `workloop+0x20`. It then associates the gate
with its workloop and adds it as an event source before
starting/validating the worker thread.

**Confirmed by the subsequent local vtable result:**
`IOWorkLoop` vptr `+0x118` resolves to
`IOWorkLoop::_maintRequest(void*,void*,void*,void*)`
at `0xffffff8000acaac0`. Together with `init()` passing
this entry into the internal `IOCommandGate::init`,
this establishes the maintenance action's identity
in the **actual 25G332 KC**, rather than only by
comparison with open-source XNU.

### 19.3 runEventSources traverses under the same gate

`IOWorkLoop::runEventSources()`:

- calls workloop vptr `+0x180`, already resolved
  as `IOWorkLoop::closeGate()` (gate enter);
- scans the active source list rooted at
  `workloop+0x18`, calling each source virtual
  `+0x120` (expected `checkForWork()`) and
  `+0x138` (expected `getNext()`);
- checks flag `workloop+0x39 & 1` and restarts
  the scan after event-list modifications;
- ultimately invokes workloop vptr `+0x178`,
  mapped as `IOWorkLoop::openGate()` (gate exit).

This is stronger evidence for **same-workloop-serialized
event-source removal** than the earlier presence of a
generic gate. A successful removal from the workloop
chain, executed under this gate, prevents a subsequent
`runEventSources` traversal from selecting that source
unless it is re-registered.

### 19.4 Precisely what is and is not proven

**Proven in the local KC (subject to matching vtable offsets):**

- a real synchronous remove handler that unlinks the
  source, nulls its workloop and successor links and
  releases the workloop's reference;
- WorkLoop list traversal and maintenance actions are
  serialized by its gate;
- source-list changes request a scan restart.

**Still NOT proven for experimental AWDL:**

- that a future `SkywalkBackend` calls
  `IOWorkLoop::removeEventSource(txQueue)` at all,
  and handles failures/absence of registration correctly;
- that **direct** non-workloop `packetSubmission()`
  or other calls cannot invoke `legacyDequeue()`
  after successful detach;
- that all callbacks that have already received
  `refCon = &SkywalkBackend::txCallbackContext_`
  have finished before the backend is deleted;
- that closing the embedded `CallbackGate`
  suffices: the trampoline **dereferences the context
  before attempting its admission gate**.

This is a concrete advance toward an owner-side
`stop producers -> detach under workloop gate ->
verify no future entries -> drain admitted calls ->
release queues -> free context` protocol, but that
protocol has **not** yet been implemented or proven.

### 19.5 Next bounded inspection

Map the vptr offsets used directly above:
`IOWorkLoop+0x118`,
`IOEventSource+0x120/+0x128/+0x130/+0x138/+0x168`,
`IOSkywalkTxSubmissionQueue+0x120/+0x128/+0x138/+0x168`,
and `IOCommandGate+0x1b8`.
Then investigate owner-side queue registration/teardown
and possible external packet-submission callers.

Keep `ITLWM_AWDL_DATAPATH_LIVE=0`,
`ITLWM_AWDL_REGISTRATION_ENABLED=0`.
No kext, EFI or runtime-path changes follow from this
analysis alone.



## 20. Maintenance action and queue traversal slots now confirmed (2026-11-11)

**Source:** developer-reported output of the local `inspect_skywalk_vtables.py`
run, targeting `BootKernelExtensions.kc` on Tahoe 25G332.
This is direct vtable symbol evidence and completes the
outstanding slot mapping requested in section 19.5.

| Class | Object-vptr offset | Resolved method | Local KC address |
|---|---|---|---|
| `IOWorkLoop` | `+0x118` | `_maintRequest(void*,void*,void*,void*)` | `0xffffff8000acaac0` |
| `IOWorkLoop` | `+0x120` | `threadMain()` | `0xffffff8000acad50` |
| `IOWorkLoop` | `+0x128` | `getThread() const` | `0xffffff8000acaf30` |
| `IOWorkLoop` | `+0x130` | `onThread() const` | `0xffffff8000acaf40` |
| `IOWorkLoop` | `+0x138` | `inGate() const` | `0xffffff8000acaf60` |
| `IOEventSource` | `+0x118` | `init(OSObject*,Action)` | `0xffffff8000acc310` |
| `IOEventSource` | `+0x120` | `checkForWork()` | `0xffffff8000acc380` |
| `IOEventSource` | `+0x128` | `setWorkLoop(IOWorkLoop*)` | `0xffffff8000acc390` |
| `IOEventSource` | `+0x130` | `setNext(IOEventSource*)` | `0xffffff8000acc3c0` |
| `IOEventSource` | `+0x138` | `getNext() const` | `0xffffff8000acc3d0` |
| `IOEventSource` | `+0x168` | `getWorkLoop() const` | `0xffffff8000acc490` |
| `IOSkywalkTxSubmissionQueue` | `+0x120` | `checkForWork()` | `0xffffff8002b31746` |
| `IOSkywalkTxSubmissionQueue` | `+0x128` | inherits `setWorkLoop(IOWorkLoop*)` | `0xffffff8000acc390` |
| `IOSkywalkTxSubmissionQueue` | `+0x130` | inherits `setNext(IOEventSource*)` | `0xffffff8000acc3c0` |
| `IOSkywalkTxSubmissionQueue` | `+0x138` | inherits `getNext() const` | `0xffffff8000acc3d0` |
| `IOSkywalkTxSubmissionQueue` | `+0x168` | inherits `getWorkLoop() const` | `0xffffff8000acc490` |
| `IOSkywalkTxSubmissionQueue` | `+0x1b8` | `initialize(void*)` | `0xffffff8002b11e8e` |
| `IOCommandGate` | `+0x1b8` | `init(OSObject*,Action)` | `0xffffff8000ace020` |

Also locally resolved: `IOWorkLoop+0x168` is
`disableAllInterrupts() const`; `IOWorkLoop+0x1b8`
is a reserved function; `IOEventSource+0x1b8` is a null
slot in this scan. Those are **not** interchangeable
with the specialized TX queue's `+0x1b8`.

### Verified full gate path for workloop-driven queue activity

```text
IOWorkLoop::init()
  -> IOCommandGate::init(owner=workloop,
                         action=IOWorkLoop::_maintRequest)
  -> gate attached at workloop+0x20

IOWorkLoop::removeEventSource(txQueue)
  -> commandGate::runCommand(operation=1, txQueue)
  -> commandGate::runAction(saved _maintRequest, ...)
  -> IOWorkLoop::_maintRequest
  -> find/unlink txQueue, setWorkLoop(nullptr), setNext(nullptr), release

IOWorkLoop::runEventSources()
  -> closeGate()
  -> txQueue::checkForWork() while it is in event-source list
  -> openGate()
```

The local `_maintRequest` and `runEventSources` bodies,
combined with the vtable mapping, support a **synchronous
barrier for workloop-list-driven entries on that same
workloop**, assuming successful removal and no
subsequent re-attachment.

### Unresolved critical direct path

The queue's `packetSubmission(bool)` can be invoked through
other code paths, not just the `runEventSources` scan.
`IOWorkLoop::removeEventSource` cannot by itself prove
that such external callers have stopped, or that their
callbacks no longer hold the borrowed `refCon`.

The Skywalk callback trampoline dereferences
`TxCallbackContext` **before** attempting admission through
its `CallbackGate`. Therefore even a closed gate with
`activeCount()==0` cannot make premature context deletion
safe. The owner-side direct TX stop-and-drain protocol
is the remaining lifetime blocker before any live test.

### Next bounded read-only target

The newly identified `IOSkywalkTxSubmissionQueue::initialize(void*)`
at `0xffffff8002b11e8e` is a useful next method to
inspect for workloop/producer attachment. Also inspect
the queue's `enable()` and `disable()` paths to see
whether they create/remove workloop event-source registration
or only change queue state.

Do not assume `initialize` calls `addEventSource`
until its body demonstrates this. Only the owner's
attach/detach callsites and the direct TX producer
lifecycle can ultimately close this gap.

**No modification to the kext source, runtime flags,
EFI, or native interface registration.**



## 21. TX initialize, enable and disable lifecycle in 25G332 (2026-11-11)

**Evidence:** developer-provided local file
`awdl-skywalk-tx-attach-lifecycle.txt` from
`/System/Library/KernelCollections/BootKernelExtensions.kc`
with method bodies for
`IOSkywalkTxSubmissionQueue::{initialize,enable,disable}`
and `IOSkywalkPacketQueue::{initialize,disable}`. The report
describes their behavior without redistributing proprietary
disassembly.

### 21.1 Base initialize confirms what queue+0xbc counts

`IOSkywalkPacketQueue::initialize(void*)` at
`0xffffff8002b0dcce`:

- Builds queue metadata/id into `queue+0x98` using
  `IOSkywalkQueueSet::getQueueSetId()`, plus stored
  queue fields; copies a name derived from
  `IOSkywalkNetworkInterface` when available.
- Invokes `pool->checkInPacketQueue(queue)`, the
  pool-side `OSSet::setObject` registration described
  earlier.
- **Increments `queue+0xbc`**. When the updated
  count equals 1, sets `queue+0xcc=1` and returns
  success (0). If greater than 1, returns
  `0xe00002d5`.

The matching base `finalize()` decrements the same
field and may call `checkOutPacketQueue`; hence
`+0xbc` is a **queue initialize/finalize balance
counter**, not evidence of in-flight callbacks.
A nonzero `+0xbc` value alone cannot tell us
whether any callback is currently executing.

This also explains why the specialized TX
`initialize()` handles `0xe00002d5` by invoking
its `finalize()`: that error can occur on repeated
initialization, and a balancing cleanup is attempted.
Exact callback safety of that recovery path remains
unproven.

### 21.2 Specialized TX initialize controls bookkeeping, not attach

`IOSkywalkTxSubmissionQueue::initialize(void*)` at
`0xffffff8002b11e8e`:

1. Calls `IOEventSource::closeGate()` only when
   `queue+0x30` (associated workloop pointer) is
   non-null.
2. Calls base `IOSkywalkPacketQueue::initialize()`
   through raw base `__ZTV+0x1c8`, corresponding to
   base object-vptr `+0x1b8`.
3. If base init returns 0, writes the argument
   into `queue+0xe0` and zeroes the fields
   `+0x13c`, `+0x14c`, `+0x148`.
4. If base init returns `0xe00002d5`,
   calls the specialized queue's virtual
   `finalize()` via object-vptr `+0x1c0`.
5. Reopens the event gate when associated with
   a workloop and returns the base init's
   status.

**No direct `IOWorkLoop::addEventSource()` or
`removeEventSource()` call appears** in this
specialized body or the reviewed base body.
Their absence here does not prove that an owner
cannot register/deregister the queue elsewhere.
Also the `queue+0xe0` field stores an opaque
caller-supplied pointer; ownership and lifetime
need inspection of its users.

### 21.3 enable and disable are not detach operations

`IOSkywalkTxSubmissionQueue::enable()` at
`0xffffff8002b11c76`:

- Calls `IOEventSource::closeGate()`,
  tests current enable bit at `queue+0x28`,
  and calls **base queue enable** via raw
  `__ZTV20IOSkywalkPacketQueue+0x160`
  (object-vptr `+0x150`) if it was disabled.
- Depending on the queue's interface and flags,
  invokes virtual `packetSubmission(false)`
  (`+0x1d8`) or `sendNotification(0)`
  (`+0x1e0`) and then reopens the gate.
- Thus `enable()` can start TX packet processing.
  It is not merely a state setter, and it does not
  itself demonstrate an event-source attachment call.

`IOSkywalkTxSubmissionQueue::disable()` at
`0xffffff8002b11cd6`:

- Closes the event gate if `queue+0x30`
  is non-null.
- Calls **base queue disable** through raw
  `__ZTV20IOSkywalkPacketQueue+0x168`
  (object-vptr `+0x158`).
- Reopens the gate if it was associated.

`IOSkywalkPacketQueue::disable()` at
`0xffffff8002b0de34` first calls
`resetABFS()` if `queue+0x68` is non-null,
then tail-calls the base `IOEventSource::disable`
at its raw vtable slot. It does **not** invoke
`removeEventSource`, does **not** null the
event source's workloop pointer, and does not
wait for externally initiated TX submissions.

This establishes that the current hypothetical
backend's `q->disable(); q->release();`
sequence **must not be treated as sufficient
evidence of a no-future-`refCon` entry barrier**.
Since all Stage 4 runtime allocation gates remain
off, no live unsafe behavior is inferred.

### 21.4 Necessary owner-side research

1. Resolve the base `IOSkywalkPacketQueue::enable`
   object-vptr slot `+0x150` and inspect its
   implementation to confirm any additional
   lifecycle side effects.
2. Identify **actual caller(s)** of
   `IOWorkLoop::addEventSource(txQueue)`,
   `IOWorkLoop::removeEventSource(txQueue)`,
   `txQueue->initialize`, and
   `txQueue->packetSubmission` in the
   surrounding Skywalk/network-interface owner.
3. Trace the queue's `free()` and
   `setWorkLoop(nullptr)` paths, ensuring
   `disable() -> detach -> drain -> release`
   or another equally proven ordering.
4. Separately prove direct TX producer
   callbacks cannot be initiated after the
   successful workloop detach, and all
   in-flight borrowed `refCon` users have exited.

**Verdict:** the WorkLoop-list detach mechanism
is now well evidenced; the *owner-side decision
to detach before destruction* and the
*external TX submission path* remain the
blocking lifecycle gaps. Keep
`ITLWM_AWDL_DATAPATH_LIVE=0` and
`ITLWM_AWDL_REGISTRATION_ENABLED=0`.



## 22. TX withPool/free and owner method inventory (2026-11-11)

**Source:** developer's local Tahoe 25G332 KC results from
`awdl-skywalk-owner-methods.txt` and
`awdl-skywalk-base-enable-free.txt`. The former is a
method-symbol inventory (171 matches before correction of the
collector's overbroad class-match filter), and the latter
contains the bodies of TX `enable()`, TX/base `free()`
and three TX `withPool()` overloads. No raw KC code
is committed to this public research repository.

### 22.1 Queue destruction has no explicit local detach or drain

`IOSkywalkTxSubmissionQueue::free()` at
`0xffffff8002b11be2`:

1. Destroys and frees the `IOSkywalkPacketTable*`
   at `queue+0xe8`, then sets this field to null.
2. Frees the statistics block at `queue+0x100`
   and the optional ABFS block at `queue+0x68`,
   nulling each pointer.
3. Tail-calls the inherited
   `IOSkywalkPacketQueue::free()` via the
   **raw base vtable `__ZTV+0xa0`**.

`IOSkywalkPacketQueue::free()` at
`0xffffff8002b0dc8e`:

1. Clears wrapper metadata fields `queue+0x70`
   and `queue+0x60`.
2. Releases the pool object at `queue+0x50`,
   if present, and zeros that field.
3. Tail-calls the superclass `free()` via
   a separate superclass vtable.

There are **no explicit calls in either body** to
`IOWorkLoop::removeEventSource`, `finalize`,
`purgePackets`, or a callback-drain barrier.
This does **not** demonstrate that the inherited
`IOEventSource::free` or the owner never handle
teardown: those are separate code paths that
must be checked. In particular, freeing an
allocated TX packet table should only follow
an independently established safe packet
cleanup and callback-detach sequence.

### 22.2 Three overloads of TX withPool

- The shorter `withPool(pool, ..., owner,
  packetAction, refCon, flags)` at
  `0xffffff8002b11a86` forwards into the
  full-argument allocator with no optional
  budget callback and a sentinel argument.
- The full allocator at `0xffffff8002b119b4`
  constructs a `IOSkywalkTxSubmissionQueue`
  object of size **`0x168` bytes**, dispatches
  virtual `initWithPool` at vptr `+0x2a8`,
  returns it if init succeeds, otherwise calls
  virtual `release` at `+0x28` and returns
  null.
- The overload taking `IOSkywalkPacket**`
  at `0xffffff8002b11a62` sets option bit
  `0x8` and forwards to the full allocator.
  This is an overload/flag distinction;
  packet-array ownership semantics still
  require checking the consumer.

The shown `withPool` implementations do not
attach the newly constructed queue as a
workloop event source. A higher-level owner
must perform that attachment if the queue is
to receive WorkLoop-driven events.

### 22.3 Concrete owner-side method candidates

The local symbol inventory directly exposes:

- `IOSkywalkNetworkInterface::registerNetworkInterface`
  overloads at `0xffffff8002b0f204` and
  `0xffffff8002b0f0d6`.
- `IOSkywalkNetworkInterface::deregisterNetworkInterface`
  at `0xffffff8002b0ee9c`, `stop` at
  `0xffffff8002b0ee66`, and `free` at
  `0xffffff8002b0effe`.
- `IOSkywalkQueueSet::initWithQueues`
  at `0xffffff8002b18406`,
  `initialize` at `0xffffff8002b18a22`,
  `finalize` at `0xffffff8002b18afa`,
  `setEnable` at `0xffffff8002b189ce`,
  and `free` at `0xffffff8002b18846`.

These are *candidate owner-side control paths*,
not yet verified callers of
`IOWorkLoop::addEventSource` or
`removeEventSource`.

### 22.4 Collector inventory precision

The previous `--list-only --all-methods`
inventory reported 171 matches. The collector
had an overbroad `f + "::" in decoded` class
check; this admitted unrelated methods
whose **argument types** mentioned a selected
class, e.g. `AppleEthernetRL::getTSOOptions`
with an `IOSkywalkNetworkInterface::IOSkywalkTSOOptions*`
argument. This is not evidence the unrelated method
belongs to `IOSkywalkNetworkInterface`.

The collector has been corrected to match the
demangled function's **leading qualified owner**
when selecting classes. Nested class members
(e.g. `IOSkywalkNetworkInterface::MetaClass::...`)
remain part of that class-family inventory,
so an inventory count is not a count of
runtime queue owner entry points.

### 22.5 Remaining safety invariant

The directly observed `withPool -> initWithPool`
and `free -> base free` paths provide object
allocation and destruction mechanics but
do **not** implement the required
`stop external producers -> disable ->
detach from workloop -> drain callbacks and
packets -> release` ordering.

The next bounded read-only analysis must
inspect `IOSkywalkQueueSet` enable/finalize/free,
`IOSkywalkNetworkInterface` register/deregister,
and `IOEventSource::free`, without claiming
to know their call graph from names alone.
Keep `ITLWM_AWDL_REGISTRATION_ENABLED=0`
and `ITLWM_AWDL_DATAPATH_LIVE=0`.



## 23. QueueSet ownership and NetworkInterface registration lifecycle (2026-11-11)

**Evidence:** local Tahoe 25G332 disassembly supplied by the developer
for `IOSkywalkQueueSet::initWithQueues`,
both `IOSkywalkNetworkInterface::registerNetworkInterface`
overloads, and `deregisterNetworkInterface`. The original
collector matched 10 QueueSet symbols, but a 6-function limit
captured only the real `initWithQueues` plus five non-code
`kalloc_type_view` symbols; `initialize`, `setEnable`
and `finalize` have **NOT** yet been reviewed. These
conclusions intentionally exclude that missing evidence.

### 23.1 QueueSet stores and retains queue pointers

`IOSkywalkQueueSet::initWithQueues` at
`0xffffff8002b18406`:

- Validates a queue-pointer array, a nonzero bounded
  queue count, and a `QueueSetInfo_s` pointer.
- Iterates the supplied queues and uses virtual `+0x218`
  to inspect a queue-specific type/classification value.
  Maintains per-category counts at `QueueSet+0x60/+0x68/
  +0x70/+0x78`, with distinct allocation arrays
  rooted at `+0x20/+0x28/+0x30/+0x38`.
- Allocates an all-queues pointer array at
  `QueueSet+0x18`, indexed using total count
  stored at `QueueSet+0x58`.
- For each queue, explicitly calls
  `IOSkywalkPacketQueue::setQueueSet(this)`
  (at `0xffffff8002b0e038`),
  stores its pointer in the all-queues array
  and appropriate categorized array, and invokes
  the queue virtual `+0x20` (OSObject `retain()`).
- Optionally validates the WMM scheduling capability
  and allocates queue-set nexus info.

This is **positive evidence that QueueSet retains the
queue objects** after creation; it is more than a
borrowed-pointer container. However, the reciprocal
`IOSkywalkQueueSet::finalize/free` release order,
per-queue disable/finalize, and WorkLoop detach
are not yet established.

**Parsing caution:** the local disassembler continued
beyond the function return into an inline jump table,
so the last apparent “unknown instructions” are likely
table *data*, not meaningful execution steps.

### 23.2 Interface registration constructs a queue-owning graph

The overload
`IOSkywalkNetworkInterface::registerNetworkInterface(
RegistrationInfo*,IOSkywalkPacketQueue**,unsigned,
pool,pool,unsigned)`
at `0xffffff8002b0f0d6`:

1. Checks whether the supplied queues support the
   WMM scheduling model.
2. Creates an `IOSkywalkQueueSet` through
   `IOSkywalkQueueSet::createWithQueues`.
3. Constructs an `IOSkywalkLogicalLink` through
   `IOSkywalkLogicalLink::createWithQueueSet`.
4. Forwards this graph to the other registration
   overload at `0xffffff8002b0f204`.
5. Releases temporary owned references to the newly
   constructed graph after the call.

The `registerNetworkInterface(..., IOSkywalkLogicalLink*,
pool,pool,unsigned)` overload validates a
`RegistrationInfo` version and size, interface
options and pool arguments, copies registration
metadata, attaches a default logical link,
maintains an ordered set of links, and **retains**
selected input packet-buffer pools into the
interface fields `+0x98/+0xa0`.

This establishes concrete parent/child structure:

```text
NetworkInterface
  -> LogicalLink (default)
       -> QueueSet
            -> IOSkywalkPacketQueue* (retained by QueueSet)
  -> packet-buffer pools (retained by NetworkInterface)
```

**These two bodies show no direct
`IOWorkLoop::addEventSource(txQueue)`**. The
QueueSet/LogicalLink methods and surrounding
registration owners may still do so.

### 23.3 Deregistration tears down link and pool references

`IOSkywalkNetworkInterface::deregisterNetworkInterface`
at `0xffffff8002b0ee9c`:

- Calls `removeNonDefaultLogicalLinks()` when
  a logical-link collection is present.
- Iterates existing queue sets to remove steering
  rules under a qualifying condition.
- Calls `IOSkywalkLogicalLink::setInterface(nullptr)`
  on the default link when present.
- Releases the ordered logical-link collection
  at `interface+0xc8`, setting it to null.
- Releases and nulls references at
  `interface+0xa0`, `+0x98`, `+0x90`,
  `+0xd0`; frees registration-metadata
  backing storage through `interface+0xc0`.

**Important limit:** this method does **not**
directly call `IOWorkLoop::removeEventSource`,
`IOSkywalkTxSubmissionQueue::disable`, or
`TxSubmissionQueue::purgePackets` in its shown body.
Those effects may happen through `LogicalLink`/
`QueueSet` release/finalize paths, so asserting a
missing cleanup bug in Apple Skywalk would be
unjustified until the child teardown is mapped.

### 23.4 Read-only collector fix

The old function filter accepted demangled names
belonging to an owner class even when the mangled
symbol actually referred to **static metadata
local to a function** (e.g.
`__ZZN17IOSkywalkQueueSet14initWithQueues...kalloc_type_view`).
That made `--max-functions 6` silently exclude
later real methods. The read-only collector now:

- restricts candidates to normal Itanium C++
  `__ZN...` method symbols (which also includes
  `__ZNK...` const methods);
- excludes `__ZZN...` local static symbols;
- supports `IOSkywalkLogicalLink` as an
  additional `--classes` filter.

No live driver or kext code was modified.

### 23.5 Next research: actual parent/child teardown

First inspect QueueSet `initialize`, `setEnable`,
`finalize`, `free` and LogicalLink
`initialize`, `finalize`, `free`,
`setInterface` (where present). Map
whether they release or disable TX submission
queues, whether they call `removeEventSource`,
and their ordering. Only then trace all direct
`packetSubmission` callers and prove that
`TxCallbackContext` outlives every possible
callback entry.

**Do not enable AWDL runtime registration or
datapath while these ownership constraints
remain unproven.**



## 24. QueueSet and LogicalLink teardown implementation in 25G332 (2026-11-11)

**Source:** developer-provided local kernel collection log containing
`IOSkywalkQueueSet::{initialize,setEnable,finalize,free}` and
`IOSkywalkLogicalLink::{setInterface,free}`. Each symbol/body
was captured from `BootKernelExtensions.kc` on Tahoe 25G332.
This public report contains only derived observations, **not**
the proprietary assembly.

### 24.1 QueueSet enable/disable dispatch is explicit

`IOSkywalkQueueSet::setEnable(bool)`
at `0xffffff8002b189ce` iterates the *all-queues* array
`QueueSet+0x18` of length `QueueSet+0x58`.

It computes an object-vptr offset from the bool:
`+0x150` for `true`, `+0x158` for `false`
and dispatches every queue through this virtual slot.
From the earlier queue lifecycle/vtable evidence, these
correspond to queue `enable()` and `disable()`.

**Important:** `setEnable` returns 0 and does not check
individual queue method results. It does **not** call
`IOWorkLoop::removeEventSource()` in the shown body.
Disable is a *state transition*, not independently
a no-future-TX-callback or queue-detach guarantee.

### 24.2 QueueSet initialize and finalize operate on selected categories

`IOSkywalkQueueSet::initialize(void*, uint64_t, void*)`
at `0xffffff8002b18a22`:

1. Rejects a null first argument with `0xe00002c2`.
2. Invokes a `QueueSet` virtual at `+0x128`, passing its
   second argument; **target identity not yet resolved**.
3. On success stores the supplied first pointer in
   `QueueSet+0x48`.
4. Iterates the categorized array `+0x28` (count `+0x68`),
   calling each queue virtual `+0x1b8`
   (`initialize(nullptr)`) and then `+0x218`
   with additional arguments including `0x50000`.
5. Iterates another categorized array `+0x30`
   (count `+0x70`), invoking queue virtual
   `+0x1b8` (`initialize(nullptr)`).
6. On failure in either category, calls the QueueSet's
   `finalize()` and returns the original error.

`IOSkywalkQueueSet::finalize()` at
`0xffffff8002b18afa` iterates these **same two**
categorized arrays `+0x28` and `+0x30`,
calling each queue virtual `+0x1c0`,
corresponding to `finalize()` for the reviewed
packet queue class. It then clears `QueueSet+0x48`
and `+0x50` and returns 0. The method does not
check each queue finalization status; the earlier TX
queue finalize can have status-dependent behavior,
so this call chain alone is **not evidence that every
queue was successfully finalized**. The other
queue categories in the all-queues array are not
iterated by this method.

No direct WorkLoop event-source removal is shown
in either QueueSet method.

### 24.3 QueueSet free releases its retained queue references

`IOSkywalkQueueSet::free()` at
`0xffffff8002b18846` walks all
`QueueSet+0x18` queue pointers and, for each:

- explicitly calls `IOSkywalkPacketQueue::setQueueSet(nullptr)`;
- calls the queue virtual `+0x28` (`release()`);
- clears the array entry.

It then frees the categorized arrays at
`+0x20/+0x28/+0x30/+0x38`, all-queue array
at `+0x18`, and optional nexus info at
`+0x40`, before calling its superclass `free()`.

This complements section 23.1, where
`initWithQueues()` retained each queue.
**No explicit queue `disable()`, `finalize()`,
or `removeEventSource()` is present in
the body of QueueSet `free()`.**
Whether these happen earlier in a separate
shutdown method or through superclass/owner
code must be evaluated separately.

### 24.4 LogicalLink setInterface changes pool bookkeeping

`IOSkywalkLogicalLink::setInterface(IOSkywalkNetworkInterface*)`
at `0xffffff8002b116d6` stores the interface
pointer in `LogicalLink+0x18`, traverses each
associated QueueSet and its all-queues array,
obtains each queue's packet pool through a
virtual at `queue+0x1e8` (exact method label
to confirm), then:

- For non-null interface: calls
  `IOSkywalkPacketBufferPool::checkInPacketQueue(queue)`
  and sets `queue->ownerId` to the interface
  registry-entry ID.
- For null interface: calls
  `IOSkywalkPacketBufferPool::checkOutPacketQueue(queue)`
  and sets `queue->ownerId` to 0.

**Caution:** pool check-in/check-out is
collection bookkeeping, **not** the same
as `IOWorkLoop::addEventSource` or
`removeEventSource`. In particular,
`setInterface(nullptr)` does not establish
callback quiescence or revoke a borrowed refCon.

### 24.5 LogicalLink free has an important unidentified pre-release step

`IOSkywalkLogicalLink::free()` at
`0xffffff8002b11538` frees its ancillary
nexus/metadata allocation, then iterates its
QueueSet array at `LogicalLink+0x10`
(count `LogicalLink+0x28`). For each QueueSet:

1. Calls **QueueSet virtual `+0x118`** with
   argument `nullptr` (resolved by the next local
   vtable scan as **`IOSkywalkQueueSet::setEnable(false)`**).
   This disables each queue but is not event-source removal.
2. Calls `IOSkywalkQueueSet::setLogicalLink(nullptr)`
   directly.
3. Calls QueueSet virtual `+0x28` (`release()`),
   clears the pointer slot, then frees the array
   and tails to superclass `free()`.

The virtual call is now verified as **QueueSet::setEnable(false)**.
The outstanding question is whether the higher-level
owner later removes the disabled queues from the
workloop, rules out external TX callbacks, and drains
their borrowed `refCon` users.

There are **no directly identified calls**
to `IOWorkLoop::removeEventSource()` in
the shown LogicalLink bodies.

### 24.6 Safety verdict and next targeted mapping

The owner/reference chain is now backed by
both construction and destruction bodies:

```text
NetworkInterface -> LogicalLink
  -> QueueSet -> TX/RX PacketQueue(s)
                     -> PacketBufferPool
```

`QueueSet::setEnable(false)` disables each
queue; `QueueSet::finalize()` finalizes
selected queue categories; `QueueSet::free()`
releases held references. `LogicalLink::setInterface(nullptr)`
unregisters queues from their pools, while
`LogicalLink::free()` calls the now-confirmed
`QueueSet::setEnable(false)` before its release.

**Unproven:** that the higher-level owner invokes
these lifecycle stages in an appropriate order,
that all event sources are detached from their
WorkLoops, that no direct `packetSubmission()`
can subsequently enter, and that all already-
executing calls using `TxCallbackContext` exit
before its memory is freed.

The subsequent local scan resolved `IOSkywalkQueueSet`
`+0x118/+0x128` and queue `+0x1b8/+0x1c0/+0x1e8/+0x218`;
`IOEventSource::free()` has also been inspected.
See section 25 for the confirmed names and outcome. Keep
`ITLWM_AWDL_REGISTRATION_ENABLED=0`
and `ITLWM_AWDL_DATAPATH_LIVE=0`.



## 25. QueueSet disable mapping and no implicit IOEventSource detach (2026-11-11)

**Evidence:** developer-supplied local 25G332 KC vtable mapping
for `IOSkywalkQueueSet`, `IOSkywalkLogicalLink`,
`IOSkywalkPacketQueue` and `IOSkywalkTxSubmissionQueue`,
plus the full local bodies of
`IOEventSource::{free,setWorkLoop}`. The section
summarizes the result without reproducing proprietary code.

### 25.1 LogicalLink::free pre-release dispatch is setEnable(false)

The unidentified call from
`IOSkywalkLogicalLink::free()` at
`0xffffff8002b11538`, using QueueSet
object-vptr `+0x118` with `nullptr`, is
**now exactly identified**:

- `IOSkywalkQueueSet +0x118` is
  `IOSkywalkQueueSet::setEnable(bool)`,
  `0xffffff8002b189ce`.
- `IOSkywalkLogicalLink +0x118` is
  `IOSkywalkLogicalLink::setEnable(bool)`,
  `0xffffff8002b11652`.

Thus, `LogicalLink::free` deliberately invokes
`QueueSet::setEnable(false)` for each retained
QueueSet **before** calling
`setLogicalLink(nullptr)` and `release()`.
This dispatches queue `disable()` as already
verified in section 24.1. It does **not**
show queue WorkLoop removal.

`IOSkywalkQueueSet +0x128` also resolves to
`setQueueSetId(uint64_t)` at
`0xffffff8002b18b68`; the early self-virtual
in `QueueSet::initialize` is an ID assignment,
**not** workloop registration or callback startup.

### 25.2 Queue object virtuals resolve additional lifecycle calls

| Queue vptr offset | Base IOSkywalkPacketQueue | IOSkywalkTxSubmissionQueue |
|---|---|---|
| `+0x150` | inherited `IOEventSource::enable()` | `enable()` |
| `+0x158` | `IOSkywalkPacketQueue::disable()` | `disable()` |
| `+0x1b8` | `initialize(void*)` | `initialize(void*)` |
| `+0x1c0` | `finalize()` | `finalize()` |
| `+0x1e8` | `getPacketBufferPool() const` | inherited getter |
| `+0x218` | `performCommand(unsigned,void*,unsigned long)` | TX-specific override |

Concrete mapping:

- `IOSkywalkPacketQueue::getPacketBufferPool()`
  at `0xffffff8002b0df40` is what
  `LogicalLink::setInterface` calls before
  `checkInPacketQueue`/`checkOutPacketQueue`.
- `IOSkywalkTxSubmissionQueue::performCommand`
  at `0xffffff8002b128b6` is the target of
  the virtual `+0x218` when the QueueSet
  initializes this queue subtype, including
  its earlier `0x50000` invocation. The
  method's body remains a next target;
  do not assign it unknown side effects.

### 25.3 IOEventSource::free does NOT remove itself from WorkLoop

`IOEventSource::free()` at
`0xffffff8000acc2a0`:

1. If its statistics object exists at
   `eventSource+0x40`, calls
   `IOStatistics::unregisterEventSource`.
2. When an internal flag is set, releases
   a stored Block action at `+0x20`.
3. Frees the event-source statistics backing
   storage, nulls `+0x40`, handles metaclass
   destruction bookkeeping and tail-calls
   the next superclass free/destructor slot.

**There is no call in this method** to
`IOWorkLoop::removeEventSource`, `setWorkLoop`,
or any callback-drain function, nor does it
explicitly inspect the workloop pointer at `+0x30`.
It performs resource reclamation, **not
automatic workloop detachment**.

`IOEventSource::setWorkLoop(IOWorkLoop*)`
at `0xffffff8000acc390` calls the
object's `disable()` vptr `+0x158`
when passed null, and then stores the new
workloop pointer in `eventSource+0x30`.
This matches section 19's
`IOWorkLoop::_maintRequest` remove operation,
which calls `setWorkLoop(nullptr)`.

### 25.4 Distinct lifecycle stages and safety invariant

```text
QueueSet::setEnable(false) -> each queue::disable()
workloop::removeEventSource(queue) -> remove + disable + clear link
QueueSet::finalize() -> selected queue::finalize()
QueueSet::free() -> release held queue references
IOEventSource::free() -> destroy local event-source resources
```

These are distinct stages; **none on its own
proves global no-future-entry into TX
packet submission callbacks**. A workloop may
still retain a disabled queue until explicitly
removed, and a driver-held borrowed `refCon`
may outlive the backend context incorrectly.
A successful gated detach prevents later
workloop-list traversal entries but not
direct calls from outside the workloop.
There is also no evidence that `IOEventSource::free`
will act as a last-chance detach if the owner
forgot to remove it.

**Unresolved before experimental AWDL enablement:**

1. Identify the owner-side queue `addEventSource`/
   `removeEventSource` sites (or another independently
   equivalent no-future-entry sequence).
2. Resolve `LogicalLink::setEnable` and TX
   `performCommand` bodies to see how they affect
   direct TX scheduling and queue state.
3. Prove direct packet-submission callers stop,
   then drain all borrowed-`refCon` callbacks
   before `TxCallbackContext` destruction.
4. Keep both Stage 4 registration/datapath flags
   disabled and do not modify runtime/EFI.



## 26. LogicalLink command dispatch and NetworkInterface stop/free (2026-11-11)

**Source:** developer-supplied local Tahoe 25G332 disassembly
for `IOSkywalkLogicalLink::setEnable`,
`IOSkywalkTxSubmissionQueue::performCommand`,
`IOSkywalkPacketQueue::performCommand`,
`IOSkywalkNetworkInterface::{stop,free}`.
No proprietary KC instruction dump is copied here.

### 26.1 LogicalLink enable/disable cascade propagates QueueSet errors

`IOSkywalkLogicalLink::setEnable(bool)`
at `0xffffff8002b11652`:

- Rejects a LogicalLink with no QueueSet entries
  (`LogicalLink+0x28 == 0`), returning
  `0xe00002bc`.
- Iterates its QueueSet pointers at
  `LogicalLink+0x10`, calling each object's
  vptr `+0x118 = QueueSet::setEnable(bool)`
  with the original bool argument.
- Exits early on any nonzero QueueSet result;
  otherwise returns zero.

**Nuance:** the local `QueueSet::setEnable`
body itself calls each queue's `enable/disable`
without inspecting any return value and returns
zero. Therefore the LogicalLink early-error
branch does not prove that queue shutdown errors
are accurately propagated.

The method does not call `IOWorkLoop::removeEventSource`
or drain direct TX callback users.

### 26.2 TX performCommand operates on queue configuration, not visible detach

`IOSkywalkTxSubmissionQueue::performCommand(unsigned,void*,unsigned long)`
at `0xffffff8002b128b6` selects among a
small set of commands using an integer transformation
and jump table. The inspected branches include:

- A 16-byte output structure, when supplied with
  a non-null output pointer and correct length,
  receiving a fixed version/value, the queue
  capacity from `queue+0xac`, and the pool pointer
  from `queue+0x50`.
- Assignment to `queue+0xf8` and to
  configuration fields `queue+0x164`,
  `queue+0x144` from a caller-provided structure.
- A branch setting flag `queue+0x142 |= 4`.
- An `addReporters(IOService*,OSSet*)` call
  with validated 16-byte input.
- An unknown/unrecognized command path
  delegating to base `IOSkywalkPacketQueue::performCommand`
  through a raw base vtable `__ZTV+0x228`.
  The base method at `0xffffff8002b0df98`
  returns `0xe00002c7` in the reviewed body.

The prior `QueueSet::initialize` sends command
`0x50000` through this virtual `+0x218`.
The exact jump-table target and the semantic
label of that command are **not established**
by this symbol-level evidence; do not claim
it starts or stops TX. No direct
`packetSubmission`, `removeEventSource` or
callback-drain call is present in the shown
TX command body.

Some bytes following the function's return
belong to the jump table and disassemble
as nonsensical opcodes; treat these as **data**,
not executable paths.

### 26.3 NetworkInterface stop and free

`IOSkywalkNetworkInterface::stop(IOService*)`
at `0xffffff8002b0ee66`:

- Checks `interface+0xc0` for registration
  state, and when present invokes
  `deregisterNetworkInterface`.
- Tail-calls the superclass
  the slot at raw `IOSkywalkInterface` base-vtable
  `__ZTV+0x5d8` (object-vptr `+0x5c8`).
  **The exact target is not resolved:** the local
  `IOSkywalkInterface` symbol inventory does not
  include a class-defined `stop()` override, so
  naming this target `IOSkywalkInterface::stop`
  would be premature. Resolve the vptr before
  attributing detach or drain behavior to it.

`IOSkywalkNetworkInterface::free()`
at `0xffffff8002b0effe`:

- Invokes `deregisterNetworkInterface`
  unconditionally.
- If private state at `interface+0xc0`
  remains, and an internal thread-call pointer
  at `private+0x28` exists, calls
  `_thread_call_cancel` then
  `_thread_call_free`, and clears the
  pointer. The corresponding callback function
  and whether cancellation synchronizes
  in-flight execution have not been identified.
- Frees locks at `private+0x10/+0x18`;
  releases a private object at `+0x20`,
  frees the private allocation and sets
  `interface+0xc0=nullptr`.
- Delegates to superclass `free()`.

No direct `IOWorkLoop::removeEventSource` call
was identified in these two local bodies.
However, their inherited superclass methods,
deregistration dependencies and child-queue
release sequences are separate paths and
must not be assumed to lack cleanup.

**Important distinction:** `thread_call_cancel`
might cancel queued work without necessarily
proving that a callback already executing on
another thread has exited. The presence of
`thread_call_cancel/free` is not proof of
a no-future-`TxCallbackContext` barrier,
nor is this private thread-call yet proven
to invoke TX submission callbacks specifically.

### 26.4 Remaining proof gap and next target

1. Inspect `IOSkywalkInterface` owner/superclass
   lifecycle to see whether it tracks workloops
   or removes event sources.
2. Inspect `IOSkywalkNetworkInterface::enable`
   and `disable` to identify the actual
   network-interface state transitions, and
   corresponding direct TX producers.
3. Independently locate actual TX queue
   `addEventSource/removeEventSource`
   call sites; class and method names alone
   are insufficient for ownership claims.
4. Confirm that **no future direct TX callback**
   can arrive and **all previously entered
   callbacks have exited** before deleting
   the embedded `SkywalkBackend::txCallbackContext_`.
5. Keep experimental registration and datapath
   flags disabled; no runtime/EFI changes.

**Evidence status:** static owner cleanup is better
understood, but the full callback lifetime
invariant is still unproven.



## 27. NetworkInterface enable/disable virtual and new IOSkywalkInterface lifecycle candidates (2026-11-11)

**Evidence:** user-supplied Tahoe 25G332
`BootKernelExtensions.kc` symbol inventory
for `IOSkywalkInterface` (38 matched symbols)
and disassembly of
`IOSkywalkNetworkInterface::{enable,disable}`.
This report records only interpreted control flow.

### 27.1 NetworkInterface enable delegates a bool to object-vptr +0x930

`IOSkywalkNetworkInterface::enable(unsigned)` at
`0xffffff8002b1041a`:

- Calls a virtual method on **the concrete
  network-interface object**, vptr `+0x930`,
  with bool argument `true` (in `esi=1`).
  The first method's `unsigned` parameter is
  not forwarded to this call.
- Uses the zero/nonzero return code to set
  the byte at `interface+0xd8` to 1 only on
  success (`sete` after `test eax,eax`).
- Does not itself call
  `IOWorkLoop::addEventSource`,
  `packetSubmission`, or an explicit queue
  callback barrier.

**Updated by 25G332 vtable evidence (section 28):**
virtual `+0x930` is confirmed as
`IOSkywalkNetworkInterface::setInterfaceEnable(bool)`.
Its body still needs inspection to identify any
`LogicalLink::setEnable`/`QueueSet::setEnable` cascade.

### 27.2 NetworkInterface disable uses atomic state transition

`IOSkywalkNetworkInterface::disable(unsigned)`
at `0xffffff8002b10442`:

1. Performs a locked `cmpxchg` at
   `interface+0xd8`, expecting old value
   1 and replacing it with 0. Returns
   immediately when the transition fails.
2. On the successful 1 -> 0 path, invokes
   the **same virtual `+0x930` with false**.
   Its return value is not checked in the
   shown caller before subsequent actions.
3. Inspects private registration data at
   `interface+0xc0`. Under a flag condition
   at `private+0x3c`, calls
   `reportLinkStatus(1,flags)` with an
   argument derived from `private+0x30`.
4. Clears `private+0x3c` and returns.

There is no direct `IOWorkLoop::removeEventSource`
or callback-drain call in this body. The
atomic enabled-state guard reduces repeat
disable work but does **not** prove that
external TX producers are stopped or that
already executing callbacks finished.

### 27.3 New named superclass interface registration entrypoints

The `IOSkywalkInterface` inventory includes
two especially relevant methods:

- `IOSkywalkInterface::registerInterfaceWithQueues(
  IOSkywalkPacketQueue**,unsigned,
  IOSkywalkPacketBufferPool*,
  IOSkywalkPacketBufferPool*,unsigned)`
  at `0xffffff8002b00d0c`.
- `IOSkywalkInterface::deregisterInterfaceWithQueues()`
  at `0xffffff8002b00b9c`.

Other named lifecycle methods include
`IOSkywalkInterface::free()` at
`0xffffff8002b00b14`,
`didTerminate` and `willTerminate`.
These are more specific next candidates
for the queue attachment/teardown contract
than a generic superclass stop symbol.

**Important correction to section 26.3, now resolved:**
`IOSkywalkNetworkInterface::stop` tail-calls
raw `IOSkywalkInterface` base vtable entry
`__ZTV+0x5d8`, i.e. object-vptr `+0x5c8`.
The subsequent local vtable lookup maps this
entry to **`IOService::stop(IOService*)`** at
`0xffffff8000a94e80`, not to an
`IOSkywalkInterface::stop` override.

### 27.4 Next bounded evidence

Resolve `IOSkywalkNetworkInterface`
object-vptr `+0x930` and the
`IOSkywalkInterface` base slot `+0x5c8`.
Then disassemble the **actual** superclass
`registerInterfaceWithQueues`,
`deregisterInterfaceWithQueues`, and
`free` bodies, looking for
`IOWorkLoop::addEventSource/removeEventSource`
and reference/producer sequencing. These
methods may delegate via more virtual calls,
so their names alone do not prove they
implement the detach protocol.

Keep the 25G332 research read-only:
`ITLWM_AWDL_REGISTRATION_ENABLED=0`;
`ITLWM_AWDL_DATAPATH_LIVE=0`. No kext
installation, native queue registration,
EFI, or runtime-code changes.



## 28. IOSkywalkInterface queue-array retention and actual enable/stop dispatch (2026-11-11)

**Source:** developer-provided Tahoe 25G332 KC vtable results
and disassembly of
`IOSkywalkInterface::{registerInterfaceWithQueues,
deregisterInterfaceWithQueues,free}`.
This report includes only interpretation, not copied
proprietary assembly.

### 28.1 Previously unresolved interface vptrs are named

Local vtable inspection identifies:

- `IOSkywalkNetworkInterface` object-vptr `+0x930` =
  **`IOSkywalkNetworkInterface::setInterfaceEnable(bool)`**
  at `0xffffff8002b10558`. Therefore the
  `NetworkInterface::{enable,disable}` bodies
  call this concrete method. Its implementation
  has not yet been disassembled.
- `IOSkywalkInterface` object-vptr `+0x5c8` =
  **`IOService::stop(IOService*)`** at
  `0xffffff8000a94e80`. This resolves
  the inherited stop target used by
  `IOSkywalkNetworkInterface::stop` after
  `deregisterNetworkInterface()`.
  Do not confuse this with the concrete
  `IOSkywalkNetworkInterface+0x5c8` target,
  which points to the network-class override
  `IOSkywalkNetworkInterface::stop`.

These names confirm dispatch identity, **not**
implicit event-source removal or callback drain.

### 28.2 Separate IOSkywalkInterface register-with-queues API

`IOSkywalkInterface::registerInterfaceWithQueues` at
`0xffffff8002b00d0c`:

1. Validates non-null queue array and count `>= 2`,
   and at least one non-null pool argument; otherwise
   returns `0xe00002c2`.
2. Creates an `OSArray::withObjects(queueArray,count,...)`
   holding references to the supplied queues.
3. Atomically compares/exchanges a null into
   `interface+0x90` to install this newly created
   array, rejecting an already registered array
   with `0xe00002c5`. Failed installation
   releases the temporary OSArray.
4. On successful install, calls
   `IOSkywalkPacketQueue::setInterface(interface)`
   for each queue pointer. The implementation
   of that setter has not yet been inspected.
5. Chooses two pool arguments (substituting
   the one supplied pool when only one is
   present), stores them at
   `interface+0x98/+0xa0` and performs the
   corresponding reference retains. Note that
   both fields may refer to the same pool,
   and reference accounting must be read in
   terms of *each stored reference*.

**No direct WorkLoop `addEventSource` call was
observed in this routine.** The array and
`queue->setInterface` calls establish an
ownership/association path for this
`IOSkywalkInterface` API. Do **not** assume
this API is the same call path as the
separately inspected
`IOSkywalkNetworkInterface::registerNetworkInterface`
with LogicalLink/QueueSet; exact callers
have not been compared.

### 28.3 Deregistration removes the reference container

`IOSkywalkInterface::deregisterInterfaceWithQueues()`
at `0xffffff8002b00b9c`:

- Atomically exchanges `interface+0x90`
  with null. When it was absent, returns
  `0xe00002f0`.
- On a non-null former OSArray, releases it
  (and hence its retained queue references
  according to OSArray ownership).
- Releases and nulls pools at
  `interface+0x98/+0xa0`, and returns zero.

The shown implementation has **no explicit**
per-queue `disable`, `finalize`,
`setInterface(nullptr)`, `removeEventSource`
or callback drain; it also does not iterate
through each original queue to clear its
interface pointer. Some side effects could
belong to `IOSkywalkPacketQueue::setInterface`,
prior owner shutdown, caller ordering, or
other lifecycle APIs. From this routine
alone, one cannot conclude that Apple
has a teardown bug, or that the borrowed
`TxCallbackContext` is safe.

### 28.4 Base free invokes deregistration, not explicit detach

`IOSkywalkInterface::free()` at
`0xffffff8002b00b14` calls its own
`deregisterInterfaceWithQueues()` and
then frees an auxiliary private structure
at `interface+0xa8`:
an owned lock, an owned object reference and
the typed allocation. It finally delegates
to superclass `free()`.

The body does not call
`IOWorkLoop::removeEventSource`, explicitly
drain outstanding queue callbacks or clear
all queue `refCon` data. The inherited
superclass implementation has not been traced
here. As usual, `release` may trigger
destruction **if** it consumes the final
reference, so it is not a substitute for an
owner-specified quiescence barrier.

### 28.5 Implications and next exact inspection

```text
IOSkywalkInterface::registerInterfaceWithQueues
  -> OSArray holds IOSkywalkPacketQueue references
  -> queue->setInterface(this)
  -> two pool reference slots

IOSkywalkInterface::deregisterInterfaceWithQueues
  -> atomic unlink of OSArray
  -> OSArray release (queue ref releases)
  -> pool ref releases

IOSkywalkNetworkInterface::{enable,disable}
  -> concrete setInterfaceEnable(bool)
  -> remaining behavior NOT YET INSPECTED
```

A successful WorkLoop remove already has a
well-evidenced synchronous boundary for
WorkLoop-driven callbacks, but this
`IOSkywalkInterface` registration/deregistration
API does **not** itself show that boundary,
and does not rule out *direct* TX
`packetSubmission` users after removal.

**Next bounded targets:**

- Disassemble
  `IOSkywalkNetworkInterface::setInterfaceEnable`
  (actual vptr `+0x930`): **subsequently confirmed
  as a success-returning no-op for this exact class**;
  see section 29. Subclasses may still override
  the virtual, so actual dynamic dispatch matters.
- Disassemble
  `IOSkywalkPacketQueue::setInterface`
  to verify whether queue->interface association
  is a simple pointer store or manages lifetime.
- Optionally inspect
  `IOSkywalkInterface::{willTerminate,didTerminate,
  handleClose}` for higher-level ordering.
- Then trace the **actual owner-side**
  `addEventSource/removeEventSource`
  invocations and all non-workloop TX
  submissions, preserving the strict
  borrowed-`refCon` lifetime requirement.

Guardrails: `ITLWM_AWDL_REGISTRATION_ENABLED=0`,
`ITLWM_AWDL_DATAPATH_LIVE=0`, no kext
loading, EFI modification or live AWDL traffic.



## 29. SetInterfaceEnable is a no-op; queue association is borrowed (2026-11-11)

**Evidence:** developer-supplied local Tahoe 25G332 KC
bodies of `IOSkywalkNetworkInterface::setInterfaceEnable`,
`IOSkywalkPacketQueue::setInterface`,
`IOSkywalkInterface::{willTerminate,didTerminate,handleClose}`.
No proprietary raw disassembly is reproduced here.

### 29.1 Exact NetworkInterface implementation of setInterfaceEnable is empty

`IOSkywalkNetworkInterface::setInterfaceEnable(bool)` at
`0xffffff8002b10558` simply returns `0`
(success) without reading its argument or accessing
any queue, workloop, link or pool. Therefore,
the `NetworkInterface::enable/disable` virtual
`+0x930` operation is **not itself** an
enable/disable cascade when the object uses
this exact class vtable.

**Subclasses could override vptr `+0x930`.**
Therefore the no-op body does not establish that
all possible concrete IOSkywalkNetworkInterface
subclasses lack enable/disable side effects.
An owner call graph must distinguish dynamic types.

### 29.2 PacketQueue::setInterface stores a raw pointer and IDs

`IOSkywalkPacketQueue::setInterface(IOSkywalkInterface*)`
at `0xffffff8002b0dfa4`:

1. Stores the supplied pointer in `queue+0x70`.
2. If non-null, calls
   `IORegistryEntry::getRegistryEntryID()` on
   the interface; otherwise uses zero.
3. Stores that same registry ID to both
   `queue+0x90` and `queue+0xa0`.

No `retain`, `release`, `WorkLoop::addEventSource`,
`WorkLoop::removeEventSource`, callback cancellation
or drain is evident in this setter. Consequently
the interface pointer is **stored without a local
retain**, and the owner must separately ensure
its referent stays alive while any queue user
could dereference `queue+0x70`.

Section 28's `IOSkywalkInterface::registerInterfaceWithQueues`
called this setter to establish the association.
The corresponding deregistration routine merely
released its queue `OSArray` and pool refs
and did not iterate to `setInterface(nullptr)`.
That is an **unresolved lifetime/ordering
question**, not proof of a runtime UAF, because
other owners may hold/release the queues or
perform explicit teardown first.

### 29.3 Termination hooks do not explicitly detach TX event sources

`IOSkywalkInterface::willTerminate(IOService*,uint)`
at `0xffffff8002b00dfe` sets byte
`private+0x8=1` through `interface+0xa8`,
then returns true. This is a state flag,
not a workloop drain or queue detach.

`IOSkywalkInterface::didTerminate(IOService*,uint,bool*)`
at `0xffffff8002b00e12` calls one method
through a statically loaded inherited vtable
entry `+0x410`, and conditionally tail-calls
another entry `+0x418` depending on the
first call's Boolean result. These concrete
function targets have not yet been resolved;
the shown body itself does not remove
TX event sources or wait for callbacks.

`IOSkywalkInterface::handleClose(IOService*,uint)`
at `0xffffff8002b00c54`:

- Acquires an `IOLock` stored in
  `interface+0xa8 -> private+0x10`.
- Loads a private object from
  `interface+0xa8 -> private+0x0`, and
  calls **that object's** vptr `+0x1d8`
  with the closing client.
- Releases the same lock.

**Important disambiguation:** this `+0x1d8`
belongs to an **as-yet unidentified private
object**. It must NOT be conflated with
`IOSkywalkTxSubmissionQueue`'s vptr
`+0x1d8 = packetSubmission(bool)`.
The subsequent local `IOSkywalkInterface::init`
body identifies this object as **`OSSet`**, created
with `OSSet::withCapacity(2)`. See section 30.
The virtual method names still require an OSSet
vtable lookup; they must not be named from offsets
shared with unrelated classes.

### 29.4 Research approach: stop guessing and seek direct call sites

Repeated named-method analysis has not
revealed any explicit owner-side
`IOWorkLoop::removeEventSource(txQueue)`
call. To progress, added a **read-only, bounded**
collector
`scripts/awdl-abi/scan_skywalk_direct_calls.py`
that scans selected locally disassembled
symbolic function bodies for **direct**
`call` or `jmp` targets at known local
25G332 addresses:

- `IOWorkLoop::addEventSource`:
  `0xffffff8000acaf80`.
- `IOWorkLoop::removeEventSource`:
  `0xffffff8000acb070`.
- `IOSkywalkTxSubmissionQueue::packetSubmission(bool)`:
  `0xffffff8002b31d68`.

The script reports caller names, call-site
addresses, matched target, bounded scan
coverage and truncation/failures, with
**no verbatim disassembly in the output**.
Its address constants are tied to Tahoe
25G332; do not run them unverified on
another kernel build.

**LIMITATION:** direct-target searching misses
indirect calls, vtable dispatch, wrapper
functions outside selected symbol families,
inlining and any target call past scan
limits. No matches is **not** a proof
that callback invocations cannot occur.

### 29.5 Safety verdict

The local KC exposes a pointer-only
interface-to-queue association, state flag
on termination and lock-protected close
handling, but **none proves an owner-side
no-future-entry + in-flight-drain barrier**
for the borrowed
`SkywalkBackend::txCallbackContext_`.
Keep
`ITLWM_AWDL_REGISTRATION_ENABLED=0`
and `ITLWM_AWDL_DATAPATH_LIVE=0`;
no live queues, kext installation, EFI or
runtime driver modifications.



## 30. Private client OSSet and limited no-hit direct-call scan (2026-11-11)

**Evidence:** developer's local Tahoe 25G332 KC output
of `IOSkywalkInterface::{init,handleOpen,clientDisconnect}`,
plus a read-only direct-call scan across
`IOSkywalk/IOUserNetwork/IOEthernet` symbol families.

### 30.1 The private object used by open/close is an OSSet

`IOSkywalkInterface::init(OSDictionary*)` at
`0xffffff8002b00a66` calls an inherited
base init method and, if it succeeds:

- optionally reads the `iosk_debug` boot argument
  and ORs its value into `gIOSkywalkDebugFlags`;
- allocates a private object/struct at
  `interface+0xa8`;
- creates **`OSSet::withCapacity(2)`** and
  stores its pointer at `private+0x0`;
- allocates an **`IOLock`** and stores
  that pointer at `private+0x10`.

The earlier `IOSkywalkInterface::willTerminate`
writes a termination flag at
`private+0x8`.

This disambiguates the earlier calls:

```text
handleOpen:
  lock(private+0x10)
  OSSet(private+0x0)->vptr+0x1c0(client)
  unlock(private+0x10)

handleClose:
  lock(private+0x10)
  OSSet(private+0x0)->vptr+0x1d8(client)
  unlock(private+0x10)
```

These are **OSSet methods, not TX submission
callbacks**, despite `+0x1d8` also being
the offset of the unrelated TX queue
`packetSubmission(bool)` method.
The subsequent 25G332 `OSSet` vtable inspection
resolved these slots to `OSSet::setObject` and
`OSSet::removeObject` respectively (section 31).

`IOSkywalkInterface::clientDisconnect(IOService*,uint)`
at `0xffffff8002b00d06` is an empty return
and therefore adds no observed explicit TX
detach or callback-drain operation.

### 30.2 Exact bounded caller scan outcome and coverage

The direct-call scanner completed on
`BootKernelExtensions.kc` with:

- **180 scanned / 2353 candidate function
  symbols (7.65%)** in its prioritized,
  alphabetically sorted pool;
- **0 direct `call/jmp` hits** to the three
  specified local addresses:
  `IOWorkLoop::addEventSource`,
  `IOWorkLoop::removeEventSource`,
  `IOSkywalkTxSubmissionQueue::packetSubmission(bool)`;
- **0 disassembly failures** and
  **0 function bodies truncated**.

The zero-hit result is **only about the
180 checked functions**. There are
2173 other candidate symbols not checked
by this invocation. In addition, vtable
or indirect dispatch, callbacks, inlined
calls and different symbol families remain
invisible to this method of searching.

The direct-call scanner now supports
`--skip-functions N` for predictable,
bounded, non-overlapping scans of later
slices of the same ranked symbol pool.
Its output includes the 0-based selection
range and total. This is static KC research,
not a queue-activity test.

### 30.3 Current lifetime conclusion

`IOSkywalkInterface` has a private
OSSet/IOLock-based client tracking mechanism
and an independent termination flag.
No provided body shows that client close,
will/didTerminate, queue release or
`setInterfaceEnable` removes the TX event
source from its WorkLoop or drains
non-workloop `packetSubmission` callbacks.

The experimental backend's embedded
`TxCallbackContext` must remain alive
until all possible borrowed-`refCon` users
have ceased entering and exited. The
observed OSSet mechanics do not change
that requirement.

### 30.4 Next bounded checks

1. Completed by the follow-up vtable scan:
   `OSSet +0x1c0/+0x1d8` are `setObject/removeObject`.
   See section 31.
2. Scan the **next** ranked caller-function
   slice with `--skip-functions 180` and
   `--max-functions 180`, recording
   the actual coverage and direct hits.
3. If still no call sites, prioritize
   **virtual and function-pointer-based
   owners** rather than repeatedly assuming
   that named method bodies will call
   `removeEventSource()` directly.
4. Keep the AWDL live registration and
   datapath gates disabled. No EFI or
   kext runtime changes.

**Guardrail:** do not commit Apple proprietary
KC disassembly; only interpreted call-site
summaries and read-only research scripts.



## 31. OSSet client membership verified and 360/2353 direct scan (2026-11-11)

**Evidence:** developer-provided 25G332 KC vtable output for
`OSSet` and read-only scanner report for ranked
function indices `[180,360)`, following the previously
recorded `[0,180)` scan. No disassembly text
is redistributed.

### 31.1 Exact client-membership calls

Local `OSSet` object-vptr lookup resolved:

- `OSSet +0x1c0` = `OSSet::setObject(OSMetaClassBase const*)`
  at `0xffffff8000a49760`.
- `OSSet +0x1d8` = `OSSet::removeObject(OSMetaClassBase const*)`
  at `0xffffff8000a49860`.

Together with `IOSkywalkInterface::init` and
`handleOpen/handleClose`, this **confirms**
that `private+0x0` is an `OSSet` used
to record/remove clients under an `IOLock`
at `private+0x10`.

`handleClose`'s `+0x1d8` call is definitively
`OSSet::removeObject`, **not**
`IOSkywalkTxSubmissionQueue::packetSubmission`.
A same-offset match across unrelated classes
is insufficient evidence for a call graph.

### 31.2 Second direct target scan: no hits, only 15.30% covered

The second local `scan_skywalk_direct_calls.py`
run used:

- `--families IOSkywalk,IOUserNetwork,IOEthernet`;
- `--skip-functions 180 --max-functions 180`;
- `--max-lines 450`.

It reported:

- ranked range `[180,360)` out of 2353
  candidate named functions;
- **0 direct call/jmp hits** to the three
  hard-coded KC addresses
  `IOWorkLoop::addEventSource`,
  `IOWorkLoop::removeEventSource`, and
  `IOSkywalkTxSubmissionQueue::packetSubmission(bool)`;
- 0 disassembly failures and 0 truncated bodies.

Combined with the previous first batch:
`[0,360)` scanned **360 of 2353**,
approximately **15.3%** of selected candidates,
with 0 observed direct matches. Other symbols,
remaining functions, indirect calls,
function pointers, vtable dispatch and
inlined paths remain **outside this result**.

A zero-hit finding is **not** a no-entry or
no-future-callback safety guarantee.
The scan order was ranked by method-name
priority, so 15.3% is not a uniform or
representative sample of the KC.

### 31.3 Opt-in indirect-slot candidate scan

The research-only direct-call scanner now also
supports `--virtual-offsets` for **bounded
candidate indirect calls** in the same selected
functions. It recognizes only textual call/jmp
patterns of the form `call qword ptr [reg + offset]`.
It outputs the caller name, function/callsite
address, base register, and requested offset
**without copying instruction text**. Each
match is only a **possible indirect slot call**:
its receiver object's class and virtual target
are NOT proven by an offset match alone.

The `IOSkywalkTxSubmissionQueue` known virtual
`+0x1d8` is one relevant candidate offset.
Other offsets from known event-source and
WorkLoop virtuals can be included, but a
same-slot call on another object is a
false positive until the receiver is traced.

The next pass should map `IOWorkLoop`
`addEventSource/removeEventSource` virtual
slots, then run a bounded candidate indirect
scan on the next ranked group `[360,540)`,
optionally including `+0x1d8`,
`+0x120`, `+0x150/+0x158`
as broad discovery targets. This
does **not** replace a type-aware reverse
call-graph analysis. If candidate hits
appear, request **specific** method bodies
to trace receiver provenance rather than
assigning a class by slot number.

### 31.4 Still no lifetime proof

The WorkLoop-driven event-source detach
mechanism has been identified in the KC;
however, **no owner-side guaranteed
queue detach and external TX producer
stop/drain sequence** has yet been
demonstrated for the embedded
`SkywalkBackend::txCallbackContext_`.
Both experimental flags remain disabled:
`ITLWM_AWDL_REGISTRATION_ENABLED=0`,
`ITLWM_AWDL_DATAPATH_LIVE=0`.
No live kext, EFI or native AWDL queue
modification is authorized by this research.



## 32. WorkLoop add/remove vptr slots and third bounded indirect-caller scan (2026-11-11)

**Evidence:** developer-supplied local 25G332 `IOWorkLoop`
vtable report and `scan_skywalk_direct_calls.py` summary
for ranked functions `[360,540)` out of 2353
selected symbols. No verbatim Apple KC code is included.

### 32.1 Exact WorkLoop event-source attachment and removal slots

`IOWorkLoop` object-vptr offsets (not raw
`__ZTV` symbol offsets) were directly resolved:

| WorkLoop object-vptr slot | KC method | Local address |
|---|---|---|
| `+0x140` | `IOWorkLoop::addEventSource(IOEventSource*)` | `0xffffff8000acaf80` |
| `+0x148` | `IOWorkLoop::removeEventSource(IOEventSource*)` | `0xffffff8000acb070` |
| `+0x150` | `enableAllEventSources()` | `0xffffff8000acb0a0` |
| `+0x158` | `disableAllEventSources()` | `0xffffff8000acb120` |
| `+0x170` | `signalWorkAvailable()` | `0xffffff8000acb290` |
| `+0x178` | `openGate()` | `0xffffff8000acb340` |
| `+0x180` | `closeGate()` | `0xffffff8000acb3b0` |
| `+0x1a8` | `runEventSources()` | `0xffffff8000acb650` |

This supplies the precise vtable offsets
`+0x140` and `+0x148` for **type-aware
indirect caller searches**. The existing
direct-call scanner already knows the two
absolute target addresses. Do not treat a
match at `+0x148` as confirmed
`removeEventSource` until the caller's
receiver is shown to be an IOWorkLoop
or a compatible subclass.

### 32.2 Third bounded search returned four unrelated slot candidates

The local scanner used method name prefixes
`IOSkywalk,IOUserNetwork,IOEthernet`,
`--skip-functions 360 --max-functions 180`,
`--max-lines 450`, and candidate virtual
offsets `0x1d8,0x120,0x150,0x158,0x1c0`.

Results:

- `[360,540)` — 180 inspected symbolic
  functions, 0 direct hits to the three
  named `25G332` targets.
- 0 disassembly failures and 0 truncated
  bodies.
- **4 candidate indirect calls** matching
  requested textual slot patterns:

| Candidate method | Function address | Indirect slot | Call-site address |
|---|---|---|---|
| `IOEthernetInterface::inputEvent` | `0xffffff80026f4f8a` | `+0x158` | `0xffffff80026f5066` |
| `IOEthernetInterface::setFilters` | `0xffffff80026f32f8` | `+0x1c0` | `0xffffff80026f335d` |
| `IOSkywalkBSDClient::finiRing` | `0xffffff8002b0281c` | `+0x1c0` | `0xffffff8002b02834` |
| `IOSkywalkCloneableNetworkPacket::getPacketBufferObjectIOBusBaseAddr` | `0xffffff8002b0d424` | `+0x158` | `0xffffff8002b0d4ca` |

These **do not include `+0x140` or
`+0x148`**, which were not requested
in this batch; no claim about their
absence follows. The other offsets
are heavily reused between unrelated
objects; the report does not expose
receiver type or prove any candidate
invokes a TX queue or a WorkLoop.

`IOSkywalkBSDClient::finiRing` is an
interesting potential lifecycle location
by name, **not** evidence that its
`+0x1c0` indirect target is the
queue `finalize()`. Receiver-provenance
analysis is mandatory before labeling it.

### 32.3 Combined coverage is still partial

The three nonoverlapping scanner batches
`[0,180)`, `[180,360)`, `[360,540)`
cover **540/2353 = 22.95%** of the
scanner's ranked candidate functions.
They produced no direct hits to known
WorkLoop add/remove or TX packetSubmission
entry addresses. Four candidate indirect
offset matches came from the third batch.

**Limitations:** the current scanner's
indirect mode recognizes only the specific
`call/jmp qword ptr [register + offset]`
textual shape. It misses calls routed through
a register loaded from a vtable, wrappers
outside the chosen symbol families,
inlined code, other compilation forms,
and functions beyond the selected range.
An unmatched function does not prove
the absence of workloop registration.

### 32.4 Change in strategy: focus on true attach/detach virtuals

Next apply `--virtual-offsets 0x140,0x148`
to a **targeted family subset** containing
`IOSkywalk{NetworkInterface,Interface,QueueSet,
LogicalLink,TxSubmissionQueue,PacketQueue,
PacketBufferPool}`, instead of
continuing to treat all similarly named
queue/ethernet methods as equivalent.
The scanner's `--families` argument
accepts comma-separated leading
demangled owner/class prefixes. A
matching indirect slot remains a
*call-site candidate*, never a verified
WorkLoop call until the receiver is traced.

Optionally scan the next ranked general
slice `[540,720)` specifically for
`+0x140/+0x148`; interpret results as
bounded evidence rather than total coverage.
If matches are found, retrieve the
exact few caller bodies to trace the
object on which the vtable call occurs.

**Safety invariant unchanged:**
`TxCallbackContext` is embedded in
`SkywalkBackend` and the kernel
callback receives a borrowed `refCon`.
Nothing so far proves stopping future
direct TX callback entry plus draining
in-flight callbacks before that
context is destroyed. Keep
`ITLWM_AWDL_REGISTRATION_ENABLED=0`
and `ITLWM_AWDL_DATAPATH_LIVE=0`;
do not change runtime, EFI, or live kexts.



## 33. Exhaustive selected-owner scan and typed receiver candidates (2026-11-11)

**Source:** developer-supplied local Tahoe 25G332 interpreted
`scan_skywalk_direct_calls.py` reports for (1) the selected
owner-class families and (2) ranked general symbolic
function indices `[540,720)`. The logs contain
symbolic caller addresses and candidate vptr
offsets only, not proprietary disassembly.

### 33.1 Exhaustive owner-class scan, 420/420

The targeted scan used exactly these class-prefix families:

```text
IOSkywalkNetworkInterface, IOSkywalkInterface,
IOSkywalkQueueSet, IOSkywalkLogicalLink,
IOSkywalkTxSubmissionQueue, IOSkywalkPacketQueue,
IOSkywalkPacketBufferPool
```

It scanned **420/420 distinct resolved symbolic function
addresses** with `--max-lines 500`, zero disassembly
failures, zero truncated bodies, zero direct
`call/jmp` hits to the exact hardcoded
`IOWorkLoop::{add,remove}EventSource` and
`IOSkywalkTxSubmissionQueue::packetSubmission(bool)`
entry addresses.

There were **30 candidate indirect call/jmp sites**
using one of the requested offsets
`+0x140,+0x148,+0x1d8`. Their presence
is expected in this region because these are
generic C++ vptr slot offsets, heavily reused
across unrelated object types. **Zero direct hits
is not a proof of absent virtual callers.**

Several method bodies are immediately relevant:

| Symbol owner / method | Symbol address | Call site | Offset | Interpretation |
|---|---|---|---|---|
| `IOSkywalkTxSubmissionQueue::enable` | `0xffffff8002b11c76` | `0xffffff8002b11cb2` | `+0x1d8` | Prior section 21 disassembly and TX vtable establish this method can invoke `packetSubmission(bool)` on its TX queue. This is a *typed* path, not evidence of WorkLoop attach. |
| `IOSkywalkNetworkInterface::removeSteeringRule` | `0xffffff8002b0f43a` | `0xffffff8002b0f514` | `+0x148` | Receiver type not yet checked; **not** verified WorkLoop `removeEventSource`. |
| `IOSkywalkTxSubmissionQueue::packetSubmission(uint64_t*,uint*,uint*)` | `0xffffff8002b31b2a` | `0xffffff8002b31d12` | `+0x140` | Receiver type unresolved; distinct from `packetSubmission(bool)` overload. |
| `IOSkywalkTxSubmissionQueue::getPacketListForKPipe` | `0xffffff8002b12164` | `0xffffff8002b12292` | `+0x140` | Unresolved receiver. |
| `IOSkywalkTxSubmissionQueue::getPacketListForNetworking` | `0xffffff8002b1257c` | `0xffffff8002b1267a` | `+0x140` | Unresolved receiver. |
| `IOSkywalkTxSubmissionQueue::refillTxRingForKPipe` | `0xffffff8002b11fb6` | `0xffffff8002b120de` | `+0x140` | Unresolved receiver. |
| `IOSkywalkTxSubmissionQueue::refillTxRingForNetworking` | `0xffffff8002b123e0` | `0xffffff8002b124e7` | `+0x140` | Unresolved receiver. |
| `IOSkywalkTxSubmissionQueue::purgePackets (.cold.1)` | `0xffffff8002b32015` | `0xffffff8002b3206b` | `+0x140` | Unresolved receiver. |
| `IOSkywalkPacketBufferPool::checkOutPacketQueue` | `0xffffff8002b0ab92` | `0xffffff8002b0abc4`, `0xffffff8002b0ac2f` | `+0x1d8` | Could be pool/OSSet bookkeeping; do **not** infer TX submission. |

Additional candidate virtual calls of interest:
`IOSkywalkTxSubmissionQueue::{requestDequeue,checkForWork,
getPacketListForKPipe,getPacketListForNetworking,
refillTxRingForKPipe,refillTxRingForNetworking}`
at `+0x1d8`, but unlike `enable()` each
caller must be type-traced individually before
attributing a queue `packetSubmission(bool)` call.
The scan also captured pool reporter/packet
allocation virtuals at these reused offsets.

**Selected class coverage is exhaustive within this
script's symbol selection and per-function line cap,
not exhaustive across all loaded driver code.**
The search omits anonymous functions, other
class families and symbol bodies not indexed by KC,
and recognizes only two instruction text patterns
for indirect candidates.

### 33.2 Next general ranked slice [540,720)

The independent general scan using
`IOSkywalk,IOUserNetwork,IOEthernet` prefixes
and exact indirect offsets `+0x140,+0x148`
found:

- 180/180 selected functions in `[540,720)`
  of 2353, 0 direct hits, 0 errors and 0 truncated.
- Four candidate indirect calls:

| Function | Function address | Site | Offset |
|---|---|---|---|
| `IOSkywalkEthernetInterface::ioctl_lladdr` | `0xffffff8002b163ea` | `0xffffff8002b16464` | `+0x140` |
| `IOSkywalkLegacyEthernet::getHardwareAddress` | `0xffffff8002b01696` | `0xffffff8002b016fd` | `+0x140` |
| `IOSkywalkLegacyEthernetInterface::init` | `0xffffff8002b01cac` | `0xffffff8002b01d46` | `+0x148` |
| `IOSkywalkLegacyEthernetInterface::init` | same | `0xffffff8002b01f09` | `+0x148` |

These candidate instructions need **receiver
provenance checks**. Two calls happen in an
initialization routine, but the matching
vptr slot alone does **not** mean it uses
`IOWorkLoop::removeEventSource`. Their
actual type might be a different IOKit class,
object or collection.

Combined general ranked batches
`[0,180)`, `[180,360)`, `[360,540)`,
`[540,720)` cover **720/2353 ~30.6%**
with 0 direct hits. The exhaustive
420-class selected-owner scan **overlaps**
the general one; do not add 420 to 720
when describing unique coverage.

### 33.3 Typed provenance work is the next bottleneck

To decide whether any candidate genuinely calls
`IOWorkLoop::removeEventSource`, retrieve
the exact bodies of:

1. `IOSkywalkNetworkInterface::removeSteeringRule`
   at `0xffffff8002b0f43a`, specifically
   the receiver of virtual `+0x148`
   at `0xffffff8002b0f514`.
2. `IOSkywalkLegacyEthernetInterface::init`
   at `0xffffff8002b01cac`, specifically
   both `+0x148` sites at
   `0xffffff8002b01d46` and
   `0xffffff8002b01f09`.
3. `IOSkywalkTxSubmissionQueue::packetSubmission(
   uint64_t*,uint*,uint*)` at
   `0xffffff8002b31b2a`, especially the
   `+0x140` at `0xffffff8002b31d12`,
   plus `requestDequeue` at
   `0xffffff8002b31a7c`.

The read-only named-method collector now
allows `IOSkywalkLegacyEthernetInterface`
to support the second target. Each output
should be read locally and *not* copied
into public GitHub. The goal is to follow
the register containing the receiver to its
origin, map that object's **actual** vtable,
and only then classify the virtual operation.

### 33.4 No change to callback lifetime verdict

The scan establishes a known *internal*
TX submission path at `TxSubmissionQueue::enable`,
but **does not** show a complete chain of
stop producers -> detach WorkLoop -> drain
in-flight and direct TX callbacks ->
destroy borrowed `refCon` context.
`ITLWM_AWDL_REGISTRATION_ENABLED=0`,
`ITLWM_AWDL_DATAPATH_LIVE=0` remain
the required guardrails. Do not load
research kexts, alter EFI or enable AWDL.
