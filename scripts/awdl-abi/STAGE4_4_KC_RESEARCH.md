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
