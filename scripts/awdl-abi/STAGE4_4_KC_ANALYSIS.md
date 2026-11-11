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

- Traverses `IOSkywalkPacket::getNextPacket`, clears each `next`, notifies its source queue, invokes packet virtual method at `+0x198`, and conditionally calls `_kern_packet_tx_completion`.
- Processes individual packets via a virtual method at `[*(this+0x50)]+0x140`. This is **not** the same call sequence as the pointer-array overload; the exact virtual target is not yet identified.
- Calls source-queue notification and stats callbacks. The shown function does not call the named `completePackets` method.

## 3. TX completion bookkeeping and possible return-to-pool boundary

`IOSkywalkTxCompletionQueue::completePackets(IOSkywalkPacket**, uint32_t)` at `0xffffff8002b17b9e`:

- For each packet, calls a packet virtual at offset `+0x140`, `getSourceQueue`, `notifySrcQueue`, and another packet virtual at `+0x198` with queue and flag arguments.
- Conditionally calls `_kern_packet_tx_completion` when `this+0xe8` is non-null.
- **After the per-packet loop**, invokes a virtual method at `[*(this+0x50)]+0x148` with the packet-pointer array and count; then notifies the source and updates stats.

This call sequence is direct evidence of a **later batch operation** after per-packet TX completion. It is **not yet proof** that the `+0x148` method frees, returns or recycles every packet: resolve the target's class/vtable and inspect it before wiring `Ownership::acknowledgeReturn`. Likewise identify packet virtual slots `+0x140` and `+0x198`.

## 4. RX completion: different consumers and ownership boundaries

### Pointer-array overload

`IOSkywalkRxCompletionQueue::enqueuePackets(IOSkywalkPacket* const*, uint32_t, uint32_t)` at `0xffffff8002b32766`:

- Applies similar checks for null/zero, `count >= *(this+0xac)`, workloop presence and state bits.
- Under an event-source gate, dispatches to `enqueuePacketsForNetworking` (`0xffffff8002b1433c`) if `this+0x78` is non-null, otherwise `enqueuePacketsForKPipe` (`0xffffff8002b1447e`).
- After either call the displayed branch explicitly returns zero, without propagating the callee's status. **The callee body must be examined** for rejection/drop/pool-return behavior.

### Linked-list overload

`IOSkywalkRxCompletionQueue::enqueuePackets(IOSkywalkPacket*, uint32_t, uint32_t)` at `0xffffff8002b32810`:

- Traverses an `IOSkywalkPacket` chain and constructs a native `kern_packet_t` linked list with `_kern_packet_set_next`.
- Invokes packet virtual slots `+0x140`, `+0x198` (with mode/flag 2), and `+0x1e0`; optionally taps BPF.
- Calls `_kern_netif_queue_rx_enqueue` at `0xffffff80009e7730` with the native list and count. The shown function **does not test** its return register before continuing to statistics and returning zero.

This gives a likely handoff point to networking, **not** an evidenced packet-return acknowledgement or a guarantee about failed enqueue cases.

## 5. Disable is not proof of callback quiescence

Each of the three `IOSkywalk{TxSubmission,TxCompletion,RxCompletion}Queue::disable()` wrappers:

1. If workloop pointer at `this+0x30` exists, calls `IOEventSource::closeGate`.
2. Invokes base `IOSkywalkPacketQueue` virtual slot `+0x168`.
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

1. **Resolve vtable slots:** `IOSkywalkPacketBufferPool` virtual offsets `0x140`, `0x148`, and `IOSkywalkPacket` offsets `0x140`, `0x198`, `0x1e0`. A reported symbol name is necessary but not sufficient: inspect the target body and overrides.
2. **RX sink internals:** `IOSkywalkRxCompletionQueue::enqueuePacketsForNetworking` and `enqueuePacketsForKPipe` and their failure/return paths.
3. **Queue destruction:** all relevant `free()`, `IOSkywalkPacketQueue::disable`, `resetABFS`, and workloop/event-source detach behavior.
4. **Context lifetime:** find the owner-side teardown sequence and demonstrate no callbacks can enter after the last reference to the callback refCon is released.
5. **Configuration:** verify whether `this+0xac` corresponds to the capacity argument, since array overloads require strictly fewer packets than this field.

With these incomplete, **TX/RX live datapath remains blocked**.
