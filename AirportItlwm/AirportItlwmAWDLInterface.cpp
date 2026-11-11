//
//  AirportItlwmAWDLInterface.cpp
//  AirportItlwm-Tahoe
//
//  Stage 2/3/4: concrete IO80211AWDLProtocol subclass plus a fail-closed
//  lifecycle. Definitions only.
//
//  Every virtual method is a fail-closed stub. The Stage 3 lifecycle only
//  tracks state and validates a local RegistrationInfo. There is no firmware
//  access, no hardware access, no packet path, no peer cache, no channel
//  change and no interface registration here. The class is never allocated by
//  this driver.
//
//  Stage 4 adds AirportItlwmAWDLDatapath.{hpp,cpp} beside this class. That
//  manager owns pools and queues but is not referenced from here, so this
//  translation unit still has no packet-pool or registration dependency.
//
//  Compiled only when ITLWM_AWDL_EXPERIMENTAL=1. The default build compiles
//  this file to an empty translation unit.
//

#include "AirportItlwmAWDLInterface.hpp"

#if ITLWM_AWDL_EXPERIMENTAL

#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include <string.h>

#include <Airport/experimental/IO80211AWDLRegistrationInfo_25G332.h>

/*
 * Stage 3 never reaches IOSkywalk registration. Keep the gate closed at
 * compile time so a future edit cannot accidentally turn the preparation
 * path into a live registration path by defining the macro alone.
 */
static_assert(ITLWM_AWDL_REGISTRATION_ENABLED == 0,
              "Stage 4 registration is not implemented; see STAGE4_BUILD_REPORT.md");

namespace {

/*
 * The class has no ivars (the 25G332 instance size is exactly 0x130), so the
 * lifecycle state lives in a small, fixed, module-local side table keyed by
 * the interface address. Nothing is allocated per interface except the lock,
 * which is created once and kept for the module lifetime.
 *
 * Locking: one IOLock protects the whole table. All lifecycle entry points
 * take it. startLifecycle() releases it while it builds and validates the
 * local RegistrationInfo, then reacquires it to commit or roll back.
 *
 * Lifetime: initLifecycle() takes one retain on the interface and
 * teardownLifecycle() drops exactly that retain. The retain is what makes the
 * address-keyed record safe: the object cannot be freed and its address
 * reused while a record still exists. stopLifecycle() never releases the
 * lifecycle retain. The owner must call teardownLifecycle() before dropping
 * its own reference; if it forgets, the interface leaks instead of leaving a
 * stale side-table entry. This class deliberately does not override free(),
 * because that would add a vtable slot and invalidate the Stage 2 ABI.
 *
 * The owner pointer stored in the record is weak and is never dereferenced by
 * this file. The controller owns the interface and must outlive it.
 */
enum LifecycleState : uint32_t {
    kLifecycleEmpty = 0,
    kLifecycleInitialized,
    kLifecycleStarting,
    kLifecyclePrepared,
    kLifecycleRunning,
    kLifecycleStopping,
    kLifecycleTornDown,
};

constexpr uint32_t kLifecycleRecordCount = 8;

struct LifecycleRecord {
    const AirportItlwmAWDLInterface *self;
    IO80211Controller *owner;
    uint32_t state;
    uint32_t startCount;
    bool holdsLifecycleReference;
    bool teardownPending;
    uint8_t mac[6];
};

LifecycleRecord gLifecycleRecords[kLifecycleRecordCount];
IOLock *gLifecycleLock = nullptr;

IOLock *lifecycleLock()
{
    IOLock *lock = gLifecycleLock;
    if (lock != nullptr)
        return lock;

    IOLock *newLock = IOLockAlloc();
    if (newLock == nullptr)
        return nullptr;

    if (!OSCompareAndSwapPtr(nullptr, newLock, &gLifecycleLock)) {
        /* Another caller won the race; drop our duplicate. */
        IOLockFree(newLock);
    }
    return gLifecycleLock;
}

LifecycleRecord *findRecordLocked(const AirportItlwmAWDLInterface *self)
{
    for (uint32_t i = 0; i < kLifecycleRecordCount; i++) {
        if (gLifecycleRecords[i].self == self)
            return &gLifecycleRecords[i];
    }
    return nullptr;
}

LifecycleRecord *claimRecordLocked(const AirportItlwmAWDLInterface *self)
{
    for (uint32_t i = 0; i < kLifecycleRecordCount; i++) {
        if (gLifecycleRecords[i].self == nullptr) {
            memset(&gLifecycleRecords[i], 0, sizeof(gLifecycleRecords[i]));
            gLifecycleRecords[i].self = self;
            return &gLifecycleRecords[i];
        }
    }
    return nullptr;
}

/*
 * Clear a record and report whether the caller must drop the lifecycle
 * retain. The caller holds the table lock, clears self before releasing the
 * lock, and performs the actual release after unlocking. That ordering is
 * important: a concurrent initLifecycle() on the same address can only claim
 * the slot after self is NULL, and it retains the object before the old
 * reference is dropped, so the final release cannot race with a new owner.
 */
bool clearRecordLocked(LifecycleRecord *record)
{
    bool releaseReference = record->holdsLifecycleReference;

    record->holdsLifecycleReference = false;
    record->teardownPending = false;
    record->state = kLifecycleTornDown;
    record->startCount = 0;
    record->owner = nullptr;
    memset(record->mac, 0, sizeof(record->mac));
    record->self = nullptr; /* frees the side-table slot exactly once */
    return releaseReference;
}

bool isZeroMAC(const uint8_t mac[6])
{
    for (uint32_t i = 0; i < 6; i++) {
        if (mac[i] != 0)
            return false;
    }
    return true;
}

bool isUnicastMAC(const uint8_t mac[6])
{
    return (mac[0] & 0x01) == 0;
}

} /* namespace */

/*
 * Required IOKit metaclass, allocator, default constructor and destructor.
 * The base's protected constructor takes the subclass metaclass, which is the
 * standard kext ABI used by the real AppleBCMWLANProximityInterface subclass.
 */
OSDefineMetaClassAndStructors(AirportItlwmAWDLInterface, IO80211AWDLProtocol)

/* ---- Stage 3 lifecycle (non-virtual; no vtable or size change) ---- */

bool AirportItlwmAWDLInterface::
initLifecycle(IO80211Controller *owner, const ether_addr &mac)
{
    if (owner == nullptr)
        return false;

    IOLock *lock = lifecycleLock();
    if (lock == nullptr)
        return false;

    IOLockLock(lock);

    /*
     * A second init without teardown is a programming error, not a reference
     * count. Reject it instead of overwriting the owner or MAC.
     */
    if (findRecordLocked(this) != nullptr) {
        IOLockUnlock(lock);
        return false;
    }

    LifecycleRecord *record = claimRecordLocked(this);
    if (record == nullptr) {
        IOLockUnlock(lock);
        return false;
    }

    record->owner = owner;
    record->state = kLifecycleInitialized;
    record->startCount = 0;
    record->holdsLifecycleReference = true;
    record->teardownPending = false;
    memcpy(record->mac, &mac, sizeof(record->mac));

    /*
     * Take the lifecycle retain while the record is published under the
     * lock. teardownLifecycle() will drop exactly this reference. The retain
     * is what prevents the object from being freed and its address reused
     * while the side-table entry still points at it.
     */
    this->retain();

    IOLockUnlock(lock);
    return true;
}

bool AirportItlwmAWDLInterface::
buildRegistrationInfo(void *buffer, size_t size) const
{
    if (buffer == nullptr || size < itlwm_awdl_25g332::kEthernetRegistrationSize)
        return false;

    IOLock *lock = lifecycleLock();
    if (lock == nullptr)
        return false;

    uint8_t mac[6];
    IOLockLock(lock);
    LifecycleRecord *record = findRecordLocked(this);
    if (record == nullptr || record->state == kLifecycleTornDown) {
        IOLockUnlock(lock);
        return false;
    }
    memcpy(mac, record->mac, sizeof(mac));
    IOLockUnlock(lock);

    /*
     * The KC's copyRegistrationInfo() accepts any six bytes; Stage 3 adds the
     * sanity checks the hardware path will need anyway. An all-zero or
     * multicast address cannot be registered as an AWDL link address.
     */
    if (isZeroMAC(mac) || !isUnicastMAC(mac))
        return false;

    using itlwm_awdl_25g332::EthernetRegistrationInfo;
    using itlwm_awdl_25g332::kEthernetRegistrationSize;
    using itlwm_awdl_25g332::kRegistrationVersion;

    EthernetRegistrationInfo *info =
        static_cast<EthernetRegistrationInfo *>(buffer);
    memset(info, 0, sizeof(*info));

    /*
     * These are the values initRegistrationInfo() writes on 25G332, with the
     * AWDL corrections verified in AppleBCMWLANProximityInterface:
     *
     *   - start() patches both MTU fields to 0x594 (1428), not 0x5dc;
     *   - getBSDNamePrefix() returns "awdl";
     *   - headroom (+0x3c) stays 0 because the base init zeroes it and
     *     start() does not patch it;
     *   - media (+0x28) and minimum packet size (+0x44) keep the base
     *     fallbacks because start() does not patch them.
     *
     * The prefix points at a static string literal, so it stays valid for as
     * long as the buffer exists. A live path must still confirm that the
     * controller wants the same family/subfamily/media values.
     */
    info->prefix.version = kRegistrationVersion;
    info->prefix.size = kEthernetRegistrationSize;
    info->prefix.interfaceFamily = 2;
    info->prefix.interfaceSubFamily = 0;
    info->prefix.maxSize = 0x100;
    info->prefix.reserved2 = 0x22;
    info->prefix.initialMedia = itlwm_awdl_25g332::kAwdlInitialMedia;
    info->prefix.bsdNamePrefix =
        reinterpret_cast<uintptr_t>(itlwm_awdl_25g332::kAwdlBsdNamePrefix);
    info->prefix.bsdUnitNumber = 0;
    info->prefix.txDataOffset = itlwm_awdl_25g332::kAwdlTxDataOffset;
    info->prefix.minPacketSize = itlwm_awdl_25g332::kAwdlMinPacketSize;
    info->prefix.maxTransferUnit = itlwm_awdl_25g332::kAwdlMaxTransferUnit;
    info->prefix.maxTransferUnit2 = itlwm_awdl_25g332::kAwdlMaxTransferUnit;
    memcpy(info->mac, mac, sizeof(mac));

    return true;
}

IOReturn AirportItlwmAWDLInterface::
startLifecycle()
{
    IOLock *lock = lifecycleLock();
    if (lock == nullptr)
        return kIOReturnNotReady;

    IOLockLock(lock);

    LifecycleRecord *record = findRecordLocked(this);
    if (record == nullptr || record->state == kLifecycleTornDown) {
        IOLockUnlock(lock);
        return kIOReturnNotOpen;
    }

    /*
     * teardownLifecycle() can arrive while a start transition is between the
     * two lock windows. It sets teardownPending instead of clearing the
     * record out from under us. A new start must not begin in that state.
     */
    if (record->teardownPending) {
        IOLockUnlock(lock);
        return kIOReturnNotOpen;
    }

    /*
     * A successful start is reference-counted. Stage 3 cannot reach this
     * branch, but the accounting is here for the reviewed path.
     */
    if (record->state == kLifecycleRunning) {
        if (record->startCount != 0xffffffffu)
            record->startCount++;
        IOLockUnlock(lock);
        return kIOReturnSuccess;
    }

    if (record->state != kLifecycleInitialized) {
        IOLockUnlock(lock);
        return kIOReturnBusy;
    }

    record->state = kLifecycleStarting;
    IOLockUnlock(lock);

    IOReturn result = kIOReturnUnsupported;

    if (!itlwmAwdlExperimentalEnabled()) {
        /*
         * The compile-time class exists, but the runtime operator gate is
         * off. Do not prepare or touch anything.
         */
        result = kIOReturnNotPermitted;
    } else {
        alignas(8) uint8_t infoBuffer[itlwm_awdl_25g332::kEthernetRegistrationSize];

        if (!buildRegistrationInfo(infoBuffer, sizeof(infoBuffer))) {
            result = kIOReturnBadArgument;
        } else {
            using itlwm_awdl_25g332::EthernetRegistrationInfo;
            using itlwm_awdl_25g332::kEthernetRegistrationSize;
            using itlwm_awdl_25g332::kRegistrationMaxSizeField;
            using itlwm_awdl_25g332::kRegistrationVersion;

            EthernetRegistrationInfo *info =
                reinterpret_cast<EthernetRegistrationInfo *>(infoBuffer);

            /*
             * Re-check the invariants registerNetworkInterface() enforces
             * (version 1, size >= 0x108, maxSize <= 0x400) plus the exact
             * 0x130 size initRegistrationInfo() requires and the AWDL values
             * verified in AppleBCMWLANProximityInterface.
             */
            if (info->prefix.version != kRegistrationVersion ||
                info->prefix.size != kEthernetRegistrationSize ||
                info->prefix.maxSize > kRegistrationMaxSizeField ||
                info->prefix.maxTransferUnit != itlwm_awdl_25g332::kAwdlMaxTransferUnit ||
                info->prefix.maxTransferUnit2 != itlwm_awdl_25g332::kAwdlMaxTransferUnit ||
                info->prefix.txDataOffset != itlwm_awdl_25g332::kAwdlTxDataOffset ||
                info->prefix.bsdNamePrefix == 0) {
                result = kIOReturnBadArgument;
            } else {
                IOLockLock(lock);
                record = findRecordLocked(this);
                if (record != nullptr && record->state == kLifecycleStarting)
                    record->state = kLifecyclePrepared;
                IOLockUnlock(lock);

                /*
                 * Stage 3/4 stops here. The Stage 4 manager
                 * (AirportItlwmAWDLDatapath.{hpp,cpp}) implements the
                 * reviewed resource sequence in isolation, but this file
                 * does not call it. The future wiring is:
                 *
                 *   1. IOSkywalkPacketBufferPool::withName(...) for the TX
                 *      and RX pools, with a real PoolOptions (packetCount,
                 *      bufferCount, bufferSize, maxBuffersPerPacket,
                 *      memorySegmentSize, poolFlags);
                 *   2. IOSkywalkTxSubmissionQueue / TxCompletionQueue /
                 *      RxCompletionQueue with the KC minimum capacities;
                 *   3. IOSkywalkEthernetInterface::initRegistrationInfo()
                 *      or the equivalent fill, then deferBSDAttach();
                 *   4. registerEthernetInterface(info, queues, count,
                 *      txPool, rxPool, 0);
                 *   5. enable()/setInterfaceEnable(true) only after the
                 *      link state machine and sync parameters are live.
                 *
                 * None of that is called here. Returning success would be a
                 * fake registration.
                 */
                result = kIOReturnUnsupported;
            }
        }
    }

    /*
     * Full rollback: Starting/Prepared return to Initialized. No pool, queue
     * or registration state survives a failed start. The lifecycle retain is
     * not part of the start rollback; it belongs to initLifecycle() and is
     * dropped only by teardownLifecycle().
     *
     * If teardownLifecycle() arrived while this start was between lock
     * windows, it set teardownPending. Finish that teardown here and drop the
     * lifecycle retain exactly once.
     */
    bool releaseReference = false;
    IOLockLock(lock);
    record = findRecordLocked(this);
    if (record != nullptr && record->teardownPending) {
        releaseReference = clearRecordLocked(record);
    } else if (record != nullptr &&
               (record->state == kLifecycleStarting ||
                record->state == kLifecyclePrepared)) {
        record->state = kLifecycleInitialized;
        record->startCount = 0;
    }
    IOLockUnlock(lock);

    if (releaseReference)
        this->release();

    return result;
}

IOReturn AirportItlwmAWDLInterface::
stopLifecycle()
{
    IOLock *lock = lifecycleLock();
    if (lock == nullptr)
        return kIOReturnNotReady;

    IOLockLock(lock);

    LifecycleRecord *record = findRecordLocked(this);
    if (record == nullptr || record->state == kLifecycleTornDown) {
        /* Already stopped / torn down: idempotent. */
        IOLockUnlock(lock);
        return kIOReturnSuccess;
    }

    /*
     * A deferred teardown belongs to whichever transition is finishing. If
     * this stop sees it first (for example after a failed start rolled back
     * to Initialized), finish it here and drop the lifecycle retain once.
     */
    if (record->teardownPending) {
        bool releaseReference = clearRecordLocked(record);
        IOLockUnlock(lock);
        if (releaseReference)
            this->release();
        return kIOReturnSuccess;
    }

    if (record->state == kLifecycleInitialized) {
        IOLockUnlock(lock);
        return kIOReturnSuccess;
    }

    if (record->state != kLifecycleRunning) {
        /* Starting, Prepared, Stopping: another transition is in flight. */
        IOLockUnlock(lock);
        return kIOReturnBusy;
    }

    if (record->startCount > 1) {
        record->startCount--;
        IOLockUnlock(lock);
        return kIOReturnSuccess;
    }

    record->startCount = 0;
    record->state = kLifecycleStopping;

    IOLockUnlock(lock);

    /*
     * Stage 3 holds no resources. A reviewed stop must deregister the
     * Ethernet interface and release the pools/queues here, in the reverse
     * order of the start sequence. The lifecycle retain is NOT dropped here;
     * only teardownLifecycle() owns it.
     */

    bool releaseReference = false;
    IOLockLock(lock);
    record = findRecordLocked(this);
    if (record != nullptr && record->teardownPending) {
        releaseReference = clearRecordLocked(record);
    } else if (record != nullptr && record->state == kLifecycleStopping) {
        record->state = kLifecycleInitialized;
    }
    IOLockUnlock(lock);

    if (releaseReference)
        this->release();

    return kIOReturnSuccess;
}

void AirportItlwmAWDLInterface::
teardownLifecycle()
{
    IOLock *lock = lifecycleLock();
    if (lock == nullptr)
        return;

    IOLockLock(lock);

    LifecycleRecord *record = findRecordLocked(this);
    if (record == nullptr) {
        /* Double teardown is a no-op. */
        IOLockUnlock(lock);
        return;
    }

    /*
     * If a start/stop transition is between its two lock windows, clearing
     * the record now would free the side-table slot while that transition
     * still expects to find it. Defer the clear to the transition's
     * completion path instead. The transition still holds the lifecycle
     * retain, so `this` stays valid.
     */
    if (record->state == kLifecycleStarting ||
        record->state == kLifecyclePrepared ||
        record->state == kLifecycleStopping) {
        record->teardownPending = true;
        IOLockUnlock(lock);
        return;
    }

    bool releaseReference = clearRecordLocked(record);

    IOLockUnlock(lock);

    if (releaseReference)
        this->release();
}

bool AirportItlwmAWDLInterface::
isLifecycleInitialized() const
{
    IOLock *lock = lifecycleLock();
    if (lock == nullptr)
        return false;

    IOLockLock(lock);
    LifecycleRecord *record = findRecordLocked(this);
    bool initialized = record != nullptr &&
        (record->state == kLifecycleInitialized ||
         record->state == kLifecycleStarting ||
         record->state == kLifecyclePrepared ||
         record->state == kLifecycleRunning ||
         record->state == kLifecycleStopping);
    IOLockUnlock(lock);
    return initialized;
}

bool AirportItlwmAWDLInterface::
isLifecycleRunning() const
{
    IOLock *lock = lifecycleLock();
    if (lock == nullptr)
        return false;

    IOLockLock(lock);
    LifecycleRecord *record = findRecordLocked(this);
    bool running = record != nullptr && record->state == kLifecycleRunning;
    IOLockUnlock(lock);
    return running;
}

/* ---- Slot 488: pure in the KC ---- */

uintptr_t AirportItlwmAWDLInterface::
setMacAddress(ether_addr&)
{
    /*
     * The inherited slot is declared uintptr_t in the 25G332 VIF ABI, while
     * kIOReturnUnsupported is a signed 32-bit IOReturn constant. The cast
     * keeps the low 32 bits exact and avoids a sign-conversion warning.
     */
    return static_cast<uintptr_t>(kIOReturnUnsupported);
}

/* ---- 63 verified AWDL slots, 505..567 ---- */

IOReturn AirportItlwmAWDLInterface::
getSSID(apple80211_ssid_data*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getCHANNEL(apple80211_channel_data*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getBSSID(apple80211_bssid_data*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getSTATE(apple80211_state_data*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_ELECTION_ID(apple80211_awdl_election_id*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SYNC_STATE(apple80211_awdl_sync_state*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_MIN_RATE(apple80211_awdl_min_rate*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_ENCRYPTION_TYPE(apple80211_encrypt*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_DEV_STATS(apple80211_awdl_dev_stats*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_CCA(apple80211_awdl_cca*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
getLLW_PARAMS(apple80211_llw_params*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setSSID(apple80211_ssid_data*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setCHANNEL(apple80211_channel_data*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_ELECTION_ID(apple80211_awdl_election_id*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_BSSID(apple80211_awdl_bssid*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNC_STATE(apple80211_awdl_sync_state*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setPEER_CACHE_CONTROL(apple80211_peer_cache_control*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_MIN_RATE(apple80211_awdl_min_rate*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_ENCRYPTION_KEYS(apple80211_key*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_ENCRYPTION_TYPE(apple80211_encrypt*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setLLW_PARAMS(apple80211_llw_params*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setTX_RATE(apple80211_tx_rate*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setPSF_DWELL(apple80211_awdl_psf_dwell_params*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_CONFIG(apple80211_awdl_config_request*)
{
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmAWDLInterface::
setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg*)
{
    return kIOReturnUnsupported;
}

#endif /* ITLWM_AWDL_EXPERIMENTAL */
