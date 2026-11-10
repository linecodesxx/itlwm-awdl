/*
 * EXPERIMENTAL ABI reconstruction for macOS 26.7.2 (25G332, Darwin 25.6.0).
 *
 * Not part of the driver build. Legacy headers are untouched.
 *
 * VERIFIED against BootKernelExtensions.kc:
 *   - __ZTV19IO80211AWDLProtocol (0xffffff800245c440, span 0x11D8)
 *     -> 568 function slots, 0..567.
 *   - slots 0..504 are inherited from the VirtualInterface chain; slots 505..567
 *     are 63 pure slots, all pointing at ___cxa_pure_virtual (0xffffff8000bb77f0).
 *   - class size 0x130 (MetaClassC1), same as IO80211VirtualInterface: the AWDL
 *     protocol adds no instance variables.
 *
 * VERIFIED (Stage 1.5, 25G332): the 63 pure slots are all overridden by
 * AppleBCMWLANProximityInterface, a direct subclass of IO80211AWDLProtocol.
 * Its __ZTV (0xffffff80017f9b68, 574 entries) maps slots 505..567 to 63
 * distinct functions whose mangled names give both the method name and the
 * parameter type. Every one of those slots is also reached by a global
 * apple80211get* or apple80211set* bridge at the same vtable offset after a
 * safeMetaCast to IO80211AWDLProtocol, and every override returns a 32-bit
 * value in eax. The declarations below match the KC mangled names and
 * parameter types exactly; see scripts/awdl-abi/verify_slots_kc.py and
 * scripts/awdl-abi/evidence_25G332.tsv.
 *
 * Return type: Itanium mangling does not encode it. All 63 overrides return
 * an IOReturn-sized value in eax; the bridges tail-jump and forward that
 * value as their own IOReturn. The IOReturn declarations are ABI-safe, but
 * the exact error constants are method-specific and are not modelled here.
 *
 * Not verified here: payload struct layouts, per-method semantics, and the
 * three Proxi-only slots 568..570 (getTX_RATE, init, freeResources), which
 * are not part of IO80211AWDLProtocol.
 */
#ifndef ITLWM_IO80211_AWDL_PROTOCOL_ABI_25G332_H
#define ITLWM_IO80211_AWDL_PROTOCOL_ABI_25G332_H

#include "IO80211VirtualInterfaceABI_25G332.h"

/* Apple80211 AWDL payloads, used only by pointer. */
struct apple80211_ssid_data;
struct apple80211_bssid_data;
struct apple80211_encrypt;
struct apple80211_tx_rate;
struct apple80211_llw_params;
struct apple80211_awdl_sync_params;
struct apple80211_awdl_sync_enabled;
struct apple80211_awdl_extension_state_machine_parameter;
struct apple80211_awdl_election_id;
struct apple80211_awdl_election_metric;
struct apple80211_awdl_sync_channel_sequence;
struct apple80211_peer_cache_maximum_size;
struct apple80211_awdl_master_channel;
struct apple80211_awdl_sync_state;
struct apple80211_awdl_election_rssi_thresholds;
struct apple80211_awdl_presence_mode;
struct apple80211_awdl_election_params;
struct apple80211_awdl_device_capabilities;
struct apple80211_awdl_secondary_master_channel;
struct apple80211_awdl_af_tx_mode;
struct apple80211_awdl_min_rate;
struct apple80211_awdl_txcal_period;
struct apple80211_awdl_dev_stats;
struct apple80211_awdl_dfsp_config;
struct apple80211_awdl_cca;
struct apple80211_awdl_cap;
struct apple80211_awdl_bssid;
struct apple80211_peer_cache_control;
struct apple80211_awdl_oob_request;
struct apple80211_awdl_sync_frame_template;
struct apple80211_awdl_peer_traffic_registration;
struct apple80211_awdl_dfsp_ucsa_config;
struct apple80211_companion_skywalk_if_create;
struct apple80211_awdl_af_sec_payload_request;
struct apple80211_p2p_coex_protection;
struct apple80211_awdl_psf_dwell_params;
struct apple80211_awdl_config_request;
struct apple80211_rtg_peer_stats_cfg;
struct awdl_peer_advert_add;

class IO80211AWDLProtocol : public IO80211VirtualInterface {
    OSDeclareAbstractStructors(IO80211AWDLProtocol)
public:
    /* ---- 63 pure slots, 505..567 ---- */
    virtual IOReturn getSSID(apple80211_ssid_data*) = 0;                                     /* 505 */
    virtual IOReturn getCHANNEL(apple80211_channel_data*) = 0;                               /* 506 */
    virtual IOReturn getBSSID(apple80211_bssid_data*) = 0;                                   /* 507 */
    virtual IOReturn getSTATE(apple80211_state_data*) = 0;                                   /* 508 */
    virtual IOReturn getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) = 0;                  /* 509 */
    virtual IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) = 0;                /* 510 */
    virtual IOReturn getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(
        apple80211_awdl_extension_state_machine_parameter*) = 0;                             /* 511 */
    virtual IOReturn getAWDL_ELECTION_ID(apple80211_awdl_election_id*) = 0;                  /* 512 */
    virtual IOReturn getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) = 0;          /* 513 */
    virtual IOReturn getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(
        apple80211_awdl_sync_channel_sequence*) = 0;                                         /* 514 */
    virtual IOReturn getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size*) = 0;    /* 515 */
    virtual IOReturn getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) = 0;            /* 516 */
    virtual IOReturn getAWDL_SYNC_STATE(apple80211_awdl_sync_state*) = 0;                    /* 517 */
    virtual IOReturn getAWDL_ELECTION_RSSI_THRESHOLDS(
        apple80211_awdl_election_rssi_thresholds*) = 0;                                      /* 518 */
    virtual IOReturn getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) = 0;              /* 519 */
    virtual IOReturn getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) = 0;          /* 520 */
    virtual IOReturn getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities*) = 0;  /* 521 */
    virtual IOReturn getAWDL_SECONDARY_MASTER_CHANNEL(
        apple80211_awdl_secondary_master_channel*) = 0;                                      /* 522 */
    virtual IOReturn getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) = 0;                    /* 523 */
    virtual IOReturn getAWDL_MIN_RATE(apple80211_awdl_min_rate*) = 0;                        /* 524 */
    virtual IOReturn getAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) = 0;                       /* 525 */
    virtual IOReturn getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) = 0;                /* 526 */
    virtual IOReturn getAWDL_DEV_STATS(apple80211_awdl_dev_stats*) = 0;                      /* 527 */
    virtual IOReturn getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) = 0;                  /* 528 */
    virtual IOReturn getAWDL_CCA(apple80211_awdl_cca*) = 0;                                  /* 529 */
    virtual IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap*) = 0;    /* 530 */
    virtual IOReturn getLLW_PARAMS(apple80211_llw_params*) = 0;                              /* 531 */
    virtual IOReturn setSSID(apple80211_ssid_data*) = 0;                                     /* 532 */
    virtual IOReturn setCHANNEL(apple80211_channel_data*) = 0;                               /* 533 */
    virtual IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) = 0;                  /* 534 */
    virtual IOReturn setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) = 0;                /* 535 */
    virtual IOReturn setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(
        apple80211_awdl_extension_state_machine_parameter*) = 0;                             /* 536 */
    virtual IOReturn setAWDL_ELECTION_ID(apple80211_awdl_election_id*) = 0;                  /* 537 */
    virtual IOReturn setAWDL_BSSID(apple80211_awdl_bssid*) = 0;                              /* 538 */
    virtual IOReturn setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) = 0;          /* 539 */
    virtual IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(
        apple80211_awdl_sync_channel_sequence*) = 0;                                         /* 540 */
    virtual IOReturn setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) = 0;            /* 541 */
    virtual IOReturn setAWDL_SYNC_STATE(apple80211_awdl_sync_state*) = 0;                    /* 542 */
    virtual IOReturn setAWDL_ELECTION_RSSI_THRESHOLDS(
        apple80211_awdl_election_rssi_thresholds*) = 0;                                      /* 543 */
    virtual IOReturn setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) = 0;              /* 544 */
    virtual IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control*) = 0;              /* 545 */
    virtual IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*) = 0;                  /* 546 */
    virtual IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template*) = 0;  /* 547 */
    virtual IOReturn setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) = 0;          /* 548 */
    virtual IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(
        apple80211_awdl_peer_traffic_registration*) = 0;                                     /* 549 */
    virtual IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(
        apple80211_awdl_secondary_master_channel*) = 0;                                      /* 550 */
    virtual IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) = 0;                    /* 551 */
    virtual IOReturn setAWDL_MIN_RATE(apple80211_awdl_min_rate*) = 0;                        /* 552 */
    virtual IOReturn setAWDL_ENCRYPTION_KEYS(apple80211_key*) = 0;                           /* 553 */
    virtual IOReturn setAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) = 0;                       /* 554 */
    virtual IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request*) = 0;             /* 555 */
    virtual IOReturn setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) = 0;                /* 556 */
    virtual IOReturn setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add*) = 0;                    /* 557 */
    virtual IOReturn setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) = 0;                  /* 558 */
    virtual IOReturn setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config*) = 0;        /* 559 */
    virtual IOReturn setCOMPANION_SKYWALK_IF_CREATE(
        apple80211_companion_skywalk_if_create*) = 0;                                        /* 560 */
    virtual IOReturn setAWDL_OOB_SEC_PAYLOAD_REQUEST(
        apple80211_awdl_af_sec_payload_request*) = 0;                                        /* 561 */
    virtual IOReturn setLLW_PARAMS(apple80211_llw_params*) = 0;                              /* 562 */
    virtual IOReturn setTX_RATE(apple80211_tx_rate*) = 0;                                    /* 563 */
    virtual IOReturn setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection*) = 0;            /* 564 */
    virtual IOReturn setPSF_DWELL(apple80211_awdl_psf_dwell_params*) = 0;                    /* 565 */
    virtual IOReturn setAWDL_CONFIG(apple80211_awdl_config_request*) = 0;                    /* 566 */
    virtual IOReturn setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg*) = 0;      /* 567 */
};
static_assert(sizeof(IO80211AWDLProtocol) == 0x130,
              "IO80211AWDLProtocol must match 25G332 MetaClassC1 size");
static_assert(__is_abstract(IO80211AWDLProtocol),
              "IO80211AWDLProtocol must stay abstract: 64 pure slots");

#endif /* ITLWM_IO80211_AWDL_PROTOCOL_ABI_25G332_H */
