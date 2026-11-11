# Stage 4.4 — TX/RX packet finalization and pool-return evidence

Target: local macOS Tahoe 25G332 BootKernelExtensions.kc.

Inputs: developer-provided `awdl-skywalk-packet-lifetime.txt` (244 lines) and `awdl-skywalk-finalize.txt` (181 lines), 2026-11-11. This note is an interpretation only; raw disassembly is not committed.

## 1. TX native pool return confirmed

**`IOSkywalkPacketBufferPool::deallocatePacket`** at `0xffffff8002b310de`:

- Ignores a null wrapper.
- Saves native packet handle from wrapper offset `+0x28`.
- If bit `0x2` of wrapper field `+0x58` is set, calls `completePreparedPacket`.
- Calls virtual wrapper `disposePacket` at vptr offset `+0x1e0`.
- Tail-calls `_kern_pbufpool_free(pool+0x18, native_handle)`.

**`IOSkywalkPacketBufferPool::deallocatePackets`** at `0xffffff8002b31128`:

- For each input wrapper, conditionally calls `completePreparedPacket`.
- **Replaces each input array entry with the native packet handle** from wrapper offset `+0x28`, then calls `disposePacket`.
- Tail-calls `_kern_pbufpool_free_batch(pool+0x18, mutated_array, count)`.

The observed TX completion pointer-array overload copies the input array into scratch storage at `queue+0xe0` before calling `completePackets`, so mutation occurs in scratch storage in that path. Do not assume arbitrary callers have the same protection.

Neither native free call yields a separately verified acknowledgement through the displayed void-return wrapper. Precisely when a host ledger may retire/recycle its tracking token is not yet proven.

## 2. Wrapper completion versus disposal

**`IOSkywalkPacket::completeWithQueue`** at `0xffffff8002b0b6fa`:

1. Calls packet-buffer virtual `vptr+0x148` for each buffer.
2. Only if the second integer argument is `2`, invokes `_kern_packet_finalize(native)`, and may invoke another callback through the object at wrapper `+0x38`, virtual `+0x220`.
3. Clears wrapper flag bit `0x2` at `+0x58`.
4. Tail-calls packet virtual `vptr+0x1a0`. The target remains unidentified.

**`IOSkywalkPacket::disposePacket`** at `0xffffff8002b0b854` disposes each packet-buffer wrapper, clears its array references, and clears wrapper metadata including `+0x28`. No native `_kern_pbufpool_free` is visible in this method itself.

**`IOSkywalkPacket::finalize`** at `0xffffff8002b0b792` finalizes all packet buffers and invokes `_kern_packet_finalize(native)`. This is a separate method from queue `finalize` and native pool-free. The three must not be treated as equivalent ownership events.

The earlier RX Networking path caches native `kern_packet_t` before disposing the wrapper, then passes the native packet chain to `_kern_netif_queue_rx_enqueue`. That explains why a native handle is still available after wrapper disposal; it does **not** prove networking accepted the packet on every error path.

## 3. Queue finalize — control flow, not a proven drain

**`IOSkywalkPacketQueue::finalize`**, `0xffffff8002b0ddcc`:

- If count/state field `this+0xbc` is zero, returns zero.
- Otherwise decrements that field, invokes a virtual method at vptr `+0x18`, may call pool `checkOutPacketQueue(this)` when the virtual call returns nonzero and the count is sufficiently low, and clears state byte `this+0xcc` when the remaining count reaches zero.
- When the count remains nonzero, returns `0xe00002d5`. The meaning of `+0xbc` and virtual `+0x18` is not yet established; do not assume an in-flight callback count.

**`IOSkywalkTxSubmissionQueue::finalize`**, `0xffffff8002b11f16`:
acquires/releases the event-source gate around a base finalize call (raw `IOSkywalkPacketQueue` vtable `+0x1d0` = vptr `+0x1c0`). On base success it calls its virtual `vptr+0x2a0`, clears fields `+0xf8`, `+0x13c`, `+0xe0`, and reopens the gate. That virtual target still needs identification.

**`IOSkywalkTxCompletionQueue::finalize`**, `0xffffff8002b17d6e`:
acquires/releases event gate, calls base finalize; on base success clears fields `+0xe8` and `+0x110`. **A base result of `0xe00002d5` is mapped to return value zero without executing this clearing path.** Other nonzero error codes are preserved.

**`IOSkywalkRxCompletionQueue::finalize`**, `0xffffff8002b140f6`:
under the event gate, calls base finalize. On base success it invokes `IOSkywalkPacketTable::flushAllPackets(table, pool)`, then clears `+0xf0`, `+0x118` and `+0xe0`. The flush method's code remains uninspected.

## 4. Critical callback-context lifetime gap

The previously reviewed `IOSkywalkTxCompletionQueue::free` invokes virtual `finalize`, **ignores its return value**, and proceeds to free its scratch pointer-array buffer and lock. This alone cannot prove a callback has stopped or that no future callback may use the workloop queue.

A zero return from specialized TX completion finalize is **not** a sufficient quiescence marker: the code explicitly maps `0xe00002d5` to zero while leaving some state fields uncleared.

No displayed `finalize` method explicitly waits for in-flight TX submission callbacks or proves no-future-entry into callback `refCon`. Event-source `closeGate` followed by `openGate` is not independently a lifetime proof.

The existing Stage 4.2 callback gate remains disarmed; `drained()` alone is not queue quiescence. Live datapath and AWDL registration must remain disabled.

## 5. Pool shutdown

`IOSkywalkPacketBufferPool::releaseAllPackets` releases an object at `+0x60` and frees its packet-entry storage at `+0x58`. `releaseAllMemorySegments` iterates/relinquishes its memory-segment collection at `+0x68`. These are resource cleanup operations, not proof that all external packets and callbacks have returned.

## 6. Next bounded research

1. Resolve vptr offsets `IOSkywalkPacket::+0x1a0`, `IOSkywalkPacketBuffer::+0x148`, and `IOSkywalkTxSubmissionQueue::+0x2a0` using the KC vtable inspector.
2. Inspect `IOSkywalkPacketTable::flushAllPackets`, `IOSkywalkPacketBufferPool::completePreparedPacket`, and `checkOutPacketQueue`.
3. Find the higher-level removeEventSource/workloop detach and owner serialization that prevent **future** TX callback entry, then prove all admitted invocations have returned, **before** freeing the `TxCallbackContext`.
4. Do not integrate the host-only `Ownership` model into live Skywalk TX/RX until those contracts are established.

No driver C++ code, kext, EFI, or runtime gate was changed as part of this review.
