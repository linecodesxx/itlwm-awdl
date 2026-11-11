//
//  AirportItlwmAWDLInterface.hpp
//  AirportItlwm-Tahoe
//
//  Stage 2/3: concrete IO80211AWDLProtocol subclass plus a fail-closed
//  lifecycle.
//
//  This class is NOT instantiated, attached, registered or started. It exists
//  so the 25G332 vtable can be reproduced and linked, and so the 63 AWDL
//  method signatures can be checked against the Kernel Collection.
//
//  Stage 3 adds non-virtual lifecycle methods that track owner/MAC/state in a
//  module-local side table. They never call the Apple base init, allocate a
//  packet pool or queue, or register an interface; startLifecycle() always
//  fails closed.
//
//  Rules that keep the ABI intact:
//    - derive only from the experimental IO80211AWDLProtocol;
//    - add no data members (instance size must stay 0x130);
//    - add no new virtual methods (vtable must stay 571 entries);
//    - override only the 63 AWDL slots and the pure setMacAddress slot;
//    - never include the legacy include/Airport/IO80211VirtualInterface.h
//      together with these experimental headers.
//
//  Compiled only when ITLWM_AWDL_EXPERIMENTAL=1. The default build sees an
//  empty translation unit and an empty header.
//
//  Return types: the 63 AWDL methods were verified in Stage 1.5 to return a
//  32-bit value in eax (IOReturn-sized). The inherited VIF/AWDL methods keep
//  the experimental header's reference-derived return types; Itanium mangling
//  does not encode return types, so those remain an ABI assumption.
//

#ifndef AirportItlwmAWDLInterface_hpp
#define AirportItlwmAWDLInterface_hpp

#include "AirportItlwmAWDLExperimental.hpp"

/*
 * Stage 3 registration gate. It is deliberately closed: the lifecycle below
 * prepares and validates a RegistrationInfo buffer, then fails closed before
 * IOSkywalkEthernetInterface::registerEthernetInterface() is reached. Flipping
 * this to 1 is not sufficient to register an interface; see the conditions in
 * scripts/awdl-abi/STAGE3_BUILD_REPORT.md.
 */
#ifndef ITLWM_AWDL_REGISTRATION_ENABLED
#define ITLWM_AWDL_REGISTRATION_ENABLED 0
#endif

#if ITLWM_AWDL_EXPERIMENTAL

#include <stddef.h>

#include <Airport/experimental/IO80211AWDLProtocolABI_25G332.h>

class AirportItlwmAWDLInterface : public IO80211AWDLProtocol {
    OSDeclareDefaultStructors(AirportItlwmAWDLInterface)

public:
    /*
     * Stage 3 lifecycle. These are deliberately NON-VIRTUAL:
     *   - the class keeps the verified 571-entry vtable and 0x130 size;
     *   - no IOService / IO80211VirtualInterface virtual is overridden, so
     *     the Stage 2 slot map (488, 505..567) is unchanged;
     *   - the methods only track ownership and validate a local
     *     RegistrationInfo. They never call the Apple base init, allocate a
     *     packet pool, create a queue, or register an interface.
     *
     * initLifecycle() records the owner and MAC. It is idempotent only in the
     * sense that a second call on an already-initialized object fails; the
     * owner must call teardownLifecycle() first.
     *
     * startLifecycle() is fail-closed in Stage 3: it validates the state,
     * builds the 0x130-byte RegistrationInfo locally, checks the KC-derived
     * invariants, then returns kIOReturnUnsupported and rolls back to
     * Initialized. No partial state is left behind.
     *
     * stopLifecycle() is idempotent. Nested successful starts would be
     * reference-counted; teardownLifecycle() is safe to call twice and frees
     * the side-table record exactly once.
     *
     * The owner pointer is weak: the controller owns the interface and must
     * outlive it. The interface never retains the controller, avoiding a
     * retain cycle.
     */
    bool initLifecycle(IO80211Controller *owner, const ether_addr &mac);
    IOReturn startLifecycle();
    IOReturn stopLifecycle();
    void teardownLifecycle();
    bool isLifecycleInitialized() const;
    bool isLifecycleRunning() const;

    /*
     * Fill a caller-provided buffer with the 0x130-byte Ethernet
     * RegistrationInfo decoded from the 25G332 KC. Returns false when the
     * buffer is too small, when the object has no lifecycle record, or when
     * the MAC is not a valid unicast address. The buffer is never passed to
     * the registration API in Stage 3.
     */
    bool buildRegistrationInfo(void *buffer, size_t size) const;
    /*
     * Slot 488, pure in the KC (___cxa_pure_virtual). The experimental VIF
     * header declares uintptr_t, so the override must use the same type.
     * The implementation is a fail-closed stub; no MAC address is stored.
     */
    uintptr_t setMacAddress(ether_addr&) override;

    /* ---- 63 verified AWDL slots, 505..567 ---- */
    IOReturn getSSID(apple80211_ssid_data*) override;                                     /* 505 */
    IOReturn getCHANNEL(apple80211_channel_data*) override;                               /* 506 */
    IOReturn getBSSID(apple80211_bssid_data*) override;                                   /* 507 */
    IOReturn getSTATE(apple80211_state_data*) override;                                   /* 508 */
    IOReturn getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) override;                  /* 509 */
    IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) override;                /* 510 */
    IOReturn getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(
        apple80211_awdl_extension_state_machine_parameter*) override;                     /* 511 */
    IOReturn getAWDL_ELECTION_ID(apple80211_awdl_election_id*) override;                  /* 512 */
    IOReturn getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) override;          /* 513 */
    IOReturn getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(
        apple80211_awdl_sync_channel_sequence*) override;                                 /* 514 */
    IOReturn getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size*) override;    /* 515 */
    IOReturn getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) override;            /* 516 */
    IOReturn getAWDL_SYNC_STATE(apple80211_awdl_sync_state*) override;                    /* 517 */
    IOReturn getAWDL_ELECTION_RSSI_THRESHOLDS(
        apple80211_awdl_election_rssi_thresholds*) override;                              /* 518 */
    IOReturn getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) override;              /* 519 */
    IOReturn getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) override;          /* 520 */
    IOReturn getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities*) override;  /* 521 */
    IOReturn getAWDL_SECONDARY_MASTER_CHANNEL(
        apple80211_awdl_secondary_master_channel*) override;                              /* 522 */
    IOReturn getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) override;                    /* 523 */
    IOReturn getAWDL_MIN_RATE(apple80211_awdl_min_rate*) override;                        /* 524 */
    IOReturn getAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) override;                       /* 525 */
    IOReturn getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) override;                /* 526 */
    IOReturn getAWDL_DEV_STATS(apple80211_awdl_dev_stats*) override;                      /* 527 */
    IOReturn getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) override;                  /* 528 */
    IOReturn getAWDL_CCA(apple80211_awdl_cca*) override;                                  /* 529 */
    IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap*) override;    /* 530 */
    IOReturn getLLW_PARAMS(apple80211_llw_params*) override;                              /* 531 */
    IOReturn setSSID(apple80211_ssid_data*) override;                                     /* 532 */
    IOReturn setCHANNEL(apple80211_channel_data*) override;                               /* 533 */
    IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) override;                  /* 534 */
    IOReturn setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) override;                /* 535 */
    IOReturn setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(
        apple80211_awdl_extension_state_machine_parameter*) override;                     /* 536 */
    IOReturn setAWDL_ELECTION_ID(apple80211_awdl_election_id*) override;                  /* 537 */
    IOReturn setAWDL_BSSID(apple80211_awdl_bssid*) override;                              /* 538 */
    IOReturn setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) override;          /* 539 */
    IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(
        apple80211_awdl_sync_channel_sequence*) override;                                 /* 540 */
    IOReturn setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) override;            /* 541 */
    IOReturn setAWDL_SYNC_STATE(apple80211_awdl_sync_state*) override;                    /* 542 */
    IOReturn setAWDL_ELECTION_RSSI_THRESHOLDS(
        apple80211_awdl_election_rssi_thresholds*) override;                              /* 543 */
    IOReturn setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) override;              /* 544 */
    IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control*) override;              /* 545 */
    IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*) override;                  /* 546 */
    IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template*) override;  /* 547 */
    IOReturn setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) override;          /* 548 */
    IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(
        apple80211_awdl_peer_traffic_registration*) override;                             /* 549 */
    IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(
        apple80211_awdl_secondary_master_channel*) override;                              /* 550 */
    IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) override;                    /* 551 */
    IOReturn setAWDL_MIN_RATE(apple80211_awdl_min_rate*) override;                        /* 552 */
    IOReturn setAWDL_ENCRYPTION_KEYS(apple80211_key*) override;                           /* 553 */
    IOReturn setAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) override;                       /* 554 */
    IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request*) override;             /* 555 */
    IOReturn setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) override;                /* 556 */
    IOReturn setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add*) override;                    /* 557 */
    IOReturn setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) override;                  /* 558 */
    IOReturn setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config*) override;        /* 559 */
    IOReturn setCOMPANION_SKYWALK_IF_CREATE(
        apple80211_companion_skywalk_if_create*) override;                                /* 560 */
    IOReturn setAWDL_OOB_SEC_PAYLOAD_REQUEST(
        apple80211_awdl_af_sec_payload_request*) override;                                /* 561 */
    IOReturn setLLW_PARAMS(apple80211_llw_params*) override;                              /* 562 */
    IOReturn setTX_RATE(apple80211_tx_rate*) override;                                    /* 563 */
    IOReturn setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection*) override;            /* 564 */
    IOReturn setPSF_DWELL(apple80211_awdl_psf_dwell_params*) override;                    /* 565 */
    IOReturn setAWDL_CONFIG(apple80211_awdl_config_request*) override;                    /* 566 */
    IOReturn setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg*) override;      /* 567 */
};

/*
 * These fail the build if a future edit adds an ivar or a new virtual.
 * The 25G332 MetaClassC1 size is 0x130 and the AWDL vtable has 571 entries.
 */
static_assert(sizeof(AirportItlwmAWDLInterface) == 0x130,
              "AirportItlwmAWDLInterface must stay 0x130 bytes");
static_assert(!__is_abstract(AirportItlwmAWDLInterface),
              "AirportItlwmAWDLInterface must implement every pure slot");

#endif /* ITLWM_AWDL_EXPERIMENTAL */

#endif /* AirportItlwmAWDLInterface_hpp */
