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

- Identify `TxCompletionQueue::vptr+0x1c0`, and base disable raw
  `__ZTV20IOSkywalkPacketQueue+0x168` = object-vptr `+0x158`.
- Inspect `IOSkywalkPacket::{completeWithQueue,disposePacket}` and pool
  `deallocatePacket(s)`, `releaseAllPackets`.
- Review `initWithPool` and the source-queue/table handoff to distinguish
  capacity, packet-buffer ownership and actual callback quiescence.
- Real `AWDL` and live datapath remain **disabled**.
