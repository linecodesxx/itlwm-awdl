//
//  AirportItlwmAWDLInterface.cpp
//  AirportItlwm-Tahoe
//
//  Stage 2/3: concrete IO80211AWDLProtocol subclass plus a fail-closed
//  lifecycle. Definitions only.
//
//  Every virtual method is a fail-closed stub. The Stage 3 lifecycle only
//  tracks state and validates a local RegistrationInfo. There is no firmware
//  access, no hardware access, no packet path, no peer cache, no channel
//  change and no interface registration here. The class is never allocated by
//  this driver.
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
              "Stage 3 registration is not implemented; see STAGE3_BUILD_REPORT.md");

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
    bool holdsReference;
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
    record->holdsReference = false;
    memcpy(record->mac, &mac, sizeof(record->mac));

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
     * These are the values initRegistrationInfo() writes on 25G332, using
     * the base class fallbacks (2, 0x20, 0x2e, 0x5dc) rather than calling
     * the base virtuals. bsdNamePrefix is left NULL because neither
     * registerNetworkInterface() nor copyRegistrationInfo() reads it before
     * the Ethernet registration step; a future registration path must
     * replace it with a real prefix pointer.
     */
    info->prefix.version = kRegistrationVersion;
    info->prefix.size = kEthernetRegistrationSize;
    info->prefix.interfaceFamily = 2;
    info->prefix.interfaceSubFamily = 0;
    info->prefix.maxSize = 0x100;
    info->prefix.reserved2 = 0x22;
    info->prefix.initialMedia = 0x20;
    info->prefix.bsdNamePrefix = 0;
    info->prefix.bsdUnitNumber = 0;
    info->prefix.minPacketSize = 0x2e;
    info->prefix.maxTransferUnit = 0x5dc;
    info->prefix.maxTransferUnit2 = 0x5dc;
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
             * 0x130 size initRegistrationInfo() requires.
             */
            if (info->prefix.version != kRegistrationVersion ||
                info->prefix.size != kEthernetRegistrationSize ||
                info->prefix.maxSize > kRegistrationMaxSizeField) {
                result = kIOReturnBadArgument;
            } else {
                IOLockLock(lock);
                record = findRecordLocked(this);
                if (record != nullptr && record->state == kLifecycleStarting)
                    record->state = kLifecyclePrepared;
                IOLockUnlock(lock);

                /*
                 * Stage 3 stops here. The reviewed future sequence is:
                 *
                 *   1. IOSkywalkPacketBufferPool::withName(...) for the TX
                 *      and RX pools, with a real PoolOptions (packetCount,
                 *      bufferCount, bufferSize, maxBuffersPerPacket,
                 *      memorySegmentSize, poolFlags);
                 *   2. IOSkywalkPacketQueue allocation / IOSkywalkQueueSet;
                 *   3. IOSkywalkEthernetInterface::initRegistrationInfo()
                 *      or the equivalent fill, then deferBSDAttach();
                 *   4. registerEthernetInterface(info, queues, count,
                 *      txPool, rxPool, 0);
                 *   5. enable()/setInterfaceEnable(true) only after the
                 *      link state machine and sync parameters are live.
                 *
                 * None of that is implemented or called here. Returning
                 * success would be a fake registration.
                 */
                result = kIOReturnUnsupported;
            }
        }
    }

    /*
     * Full rollback: Starting/Prepared return to Initialized. No pool, queue,
     * registration, retain, or MAC state survives a failed start.
     */
    IOLockLock(lock);
    record = findRecordLocked(this);
    if (record != nullptr &&
        (record->state == kLifecycleStarting ||
         record->state == kLifecyclePrepared)) {
        record->state = kLifecycleInitialized;
        record->startCount = 0;
        record->holdsReference = false;
    }
    IOLockUnlock(lock);

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
    bool releaseReference = record->holdsReference;
    record->holdsReference = false;

    IOLockUnlock(lock);

    /*
     * Stage 3 holds no resources. A reviewed stop must deregister the
     * Ethernet interface and release the pools/queues here, in the reverse
     * order of the start sequence.
     */

    IOLockLock(lock);
    record = findRecordLocked(this);
    if (record != nullptr && record->state == kLifecycleStopping)
        record->state = kLifecycleInitialized;
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

    bool releaseReference = record->holdsReference;
    record->holdsReference = false;
    record->state = kLifecycleTornDown;
    record->startCount = 0;
    record->owner = nullptr;
    memset(record->mac, 0, sizeof(record->mac));
    record->self = nullptr; /* frees the side-table slot exactly once */

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
