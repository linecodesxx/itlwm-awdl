/*
 * EXPERIMENTAL ABI reconstruction for macOS 26.7.2 (25G332, Darwin 25.6.0).
 *
 * This header is NOT part of the AirportItlwm driver build. It exists only to
 * reproduce the kernel-side IO80211/IOSkywalk class hierarchy so that a future
 * AWDL interface can be compiled against the real vtable layout. The legacy
 * headers under include/Airport and include/Airport/macOS26 are untouched.
 *
 * VERIFIED against /System/Library/KernelCollections/BootKernelExtensions.kc:
 *   - __ZTV23IO80211SkywalkInterface   span 0xE80 -> 461 function slots (0..460)
 *   - IO80211SkywalkInterface          class size 0x128 (MetaClassC1)
 *   - IOSkywalkEthernetInterface       class size 0x120 (MetaClassC1)
 *   - IOSkywalkNetworkInterface        class size 0xE0  (reference layout)
 *   - IOSkywalkInterface               class size 0xB0  (reference layout)
 *   - clang -fdump-vtable-layouts with the Tahoe SDK reproduces the reference
 *     anchors: free()=18, enable()=266, disable()=267,
 *     initBSDInterfaceParameters()=284, syncDPSStats()=460.
 *
 * ASSUMPTIONS (cannot be proven from the KC because class-specific symbols are
 * stripped): every per-slot method name and signature below is taken from the
 * reference IO80211_25_6_0.h (DrogaBox/RTL8814Native) and is treated as an
 * ABI hypothesis, not a verified fact.
 */
#ifndef ITLWM_IO80211_SKYWALK_ABI_25G332_H
#define ITLWM_IO80211_SKYWALK_ABI_25G332_H

#include <IOKit/IOService.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/network/IOEthernetController.h>
#include <stdint.h>
#include <stdarg.h>

class IOSkywalkInterface;
class IOSkywalkNetworkInterface;
class IOSkywalkEthernetInterface;
class IO80211SkywalkInterface;
class IO80211InfraInterface;
class IO80211InfraProtocol;
class IO80211Controller;
class IO80211WorkQueue;
struct CCPipe;
struct FlowIdMetadata;
struct IO80211APIUserClient;
enum IO80211CountryCodeOp : uint32_t {};  /* by value: assumed a 32-bit enum */
enum IO80211FeatureCode : uint32_t {};  /* by value: assumed a 32-bit enum */
struct IO80211FlowQueue;
enum IO80211FlowQueueHash : uint32_t {};  /* by value: assumed a 32-bit enum */
struct IO80211InterfaceAVCAdvisory;
enum IO80211LinkState : uint32_t {};  /* by value: assumed a 32-bit enum */
struct IO80211NetworkPacket;
struct IO80211Peer;
struct IO80211PeerManager;
struct IO80211VirtualInterface;
struct IOSkywalkLogicalLink;
struct IOSkywalkPacketBufferPool;
struct IOSkywalkPacketQueue;
struct PacketSkywalkScratch;
struct TxCompletionEnqueueStats;
struct TxSubmissionDequeueStats;
struct __ifnet;
struct __mbuf;
struct appl80211_chip_diags_data;
struct appl80211_sleep_on_inactivity_config;
struct apple80211AssocCandidates;
struct apple80211ChannelInfo;
struct apple80211ScanRequest;
struct apple80211_ManagementInformationBasedot11_counters;
struct apple80211_apmode_data;
struct apple80211_associated_sleep_config;
struct apple80211_battery_ps_config;
struct apple80211_bcn_mute_config;
struct apple80211_beacon_info_t;
struct apple80211_beacon_msg;
struct apple80211_bg_motion_profile;
struct apple80211_bg_network;
struct apple80211_bg_params;
struct apple80211_bg_scan;
struct apple80211_bgscan_cached_network_data_list;
struct apple80211_btcoex_2g_chain_disable;
struct apple80211_btcoex_ext_profile;
struct apple80211_btcoex_max_nss_for_ap_data;
struct apple80211_btcoex_profile;
struct apple80211_btcoex_profile_active_data;
struct apple80211_bypass_tx_power_cap;
struct apple80211_capability_data;
struct apple80211_cca_report;
struct apple80211_channel;
struct apple80211_channel_data;
struct apple80211_channels_info;
struct apple80211_chip_counters_rx;
struct apple80211_chip_counters_tx;
struct apple80211_chip_error_counters_tx;
struct apple80211_chip_power_limit;
struct apple80211_chip_stats;
struct apple80211_congestion_control_indication;
struct apple80211_country_channel_data;
struct apple80211_country_code_data;
struct apple80211_crash_command;
struct apple80211_data_path_interface_stats;
struct apple80211_data_path_peer_stats;
struct apple80211_dbg_guard_time_params;
struct apple80211_debug_command;
struct apple80211_device_accessory_info;
struct apple80211_device_orientation;
struct apple80211_dhcp_renewal_data;
struct apple80211_drbg_entropy;
struct apple80211_dual_power_mode_params;
struct apple80211_dynamic_rssi_window_config;
struct apple80211_dynsar_detail;
struct apple80211_eap_filter_config;
struct apple80211_extended_bss_info;
struct apple80211_facetime_wificalling_params;
struct apple80211_fastlane;
struct apple80211_feature_flags;
struct apple80211_frame_counters;
struct apple80211_fw_clock_info;
struct apple80211_fw_clock_source;
struct apple80211_fw_hot_channels;
struct apple80211_gas_query_t;
struct apple80211_guard_interval_data;
struct apple80211_he_capability;
struct apple80211_he_counters_ctl;
struct apple80211_host_clock_info;
struct apple80211_hp2p_ctrl;
struct apple80211_ht_capability;
struct apple80211_hw_mac_address;
struct apple80211_ie_data;
struct apple80211_infra_enumerated;
struct apple80211_infra_specific_stats;
struct apple80211_interface_availability;
struct apple80211_interface_setting;
struct apple80211_ipv4_params;
struct apple80211_ipv6_params;
struct apple80211_key;
struct apple80211_latency_all_ac;
struct apple80211_le_scan_params;
struct apple80211_leaky_ap_setting;
struct apple80211_leave_network;
struct apple80211_legacy_roam_profile_config;
struct apple80211_limited_aggregation_config;
struct apple80211_lmtpc_config;
struct apple80211_low_latency_info;
struct apple80211_lqm_config_t;
struct apple80211_lqm_statistics;
struct apple80211_lqm_summary;
struct apple80211_mcs_data;
struct apple80211_mcs_vht_data;
struct apple80211_mimo_config;
struct apple80211_mimo_status;
struct apple80211_mitigate_interference;
struct apple80211_mws_accessory_power_limit;
struct apple80211_mws_antenna_selection;
struct apple80211_mws_condition_id_config;
struct apple80211_mws_rfem_config;
struct apple80211_mws_scan_freq;
struct apple80211_mws_scan_freq_mode;
struct apple80211_mws_time_sharing;
struct apple80211_mws_wifi_channel_bitmap;
struct apple80211_ndd_data;
struct apple80211_network_data;
struct apple80211_noise_per_ant_t;
struct apple80211_nss_data;
struct apple80211_offload_arp_data;
struct apple80211_offload_ndp_data;
struct apple80211_offload_tcpka_enable_t;
struct apple80211_opmode_data;
struct apple80211_os_eligibility;
struct apple80211_p2p_device_capability;
struct apple80211_platform_config;
struct apple80211_pm_mode;
struct apple80211_pmk;
struct apple80211_power_budget_t;
struct apple80211_power_data;
struct apple80211_power_debug_info;
struct apple80211_power_profile;
struct apple80211_powersave_data;
struct apple80211_powertable_version_data;
struct apple80211_private_mac_data;
struct apple80211_ranging_authenticate_request_t;
struct apple80211_ranging_capabilities_t;
struct apple80211_ranging_enable_request_t;
struct apple80211_ranging_start_request_t;
struct apple80211_rate_data;
struct apple80211_reassoc;
struct apple80211_reset_command;
struct apple80211_roam_cache_data;
struct apple80211_roam_profile_all_bands;
struct apple80211_roam_profile_config;
struct apple80211_rsdb_capability;
struct apple80211_rsn_xe_data;
struct apple80211_rssi_data;
struct apple80211_rx_data_stall_report;
struct apple80211_scan_data;
struct apple80211_sdb_enable;
struct apple80211_sensing_data_t;
struct apple80211_sensing_disable_t;
struct apple80211_sensing_enable_t;
struct apple80211_set_property_unserialized_data;
struct apple80211_set_roam_lock;
struct apple80211_sib_coex_status;
struct apple80211_slow_wifi_feature_enabled;
struct apple80211_smartcca_opmode;
struct apple80211_standalone_state;
struct apple80211_stat_report;
struct apple80211_state_data;
struct apple80211_sup_channel_data;
struct apple80211_system_sleep_config;
struct apple80211_thermal_index_t;
struct apple80211_timestamping_en;
struct apple80211_timesync_gpio;
struct apple80211_timesync_info;
struct apple80211_timesync_rx_policy;
struct apple80211_timesync_stats;
struct apple80211_timesync_tx_policy;
struct apple80211_tko_dump;
struct apple80211_tko_params;
struct apple80211_traffic_eng_params;
struct apple80211_trap_info_data;
struct apple80211_trap_mini_dump_data;
struct apple80211_tx_mode_config;
struct apple80211_txpower_data;
struct apple80211_txrx_chain_info;
struct apple80211_usb_host_notification_data;
struct apple80211_user_roam_cache;
struct apple80211_version_data;
struct apple80211_vht_capability;
struct apple80211_virt_if_create_data;
struct apple80211_voice_ind_state;
struct apple80211_wcl_abort_join;
struct apple80211_wcl_action_frame;
struct apple80211_wcl_advisory_info;
struct apple80211_wcl_arp_mode;
struct apple80211_wcl_low_latency_stats;
struct apple80211_wcl_qos_params;
struct apple80211_wcl_real_time_mode;
struct apple80211_wcl_traffic_counters;
struct apple80211_wcl_tx_rx_latency;
struct apple80211_wcl_ulofdma_state;
struct apple80211_wcl_update_link_state;
struct apple80211_wcl_wnm_config_t;
struct apple80211_wcl_wnm_offload_t;
struct apple80211_wifi_assertion_data;
enum apple80211_wme_ac : uint32_t {};  /* by value: assumed a 32-bit enum */
struct apple80211_wow_low_power_mode;
struct apple80211_wow_test_data;
struct apple82011_postMessage_dps;
struct bss_blacklist;
struct ether_addr;
struct ether_header;
struct if_link_status;
struct ifdrv;
struct ifnet_init_eparams;
struct ifnet_traffic_descriptor_common;
struct mloAddrArray;
struct nicproxy_info_s;
struct nicproxy_limits_info_s;
struct packet_info_tag;
struct scanHomeAndAwayTime;
struct sockaddr_dl;
struct triggerCC;
struct userPrintCtx;

class IOSkywalkInterface : public IOService {
    OSDeclareAbstractStructors(IOSkywalkInterface)
public:
    virtual void free() override;  // slot 18
    virtual bool init(OSDictionary*) override;  // slot 73
    virtual bool willTerminate(IOService*, unsigned int) override;  // slot 128
    virtual bool didTerminate(IOService*, unsigned int, bool*) override;  // slot 129
    virtual bool handleOpen(IOService*, unsigned int, void*) override;  // slot 189
    virtual void handleClose(IOService*, unsigned int) override;  // slot 190
    virtual bool handleIsOpen(IOService const*) const override;  // slot 191
    virtual uintptr_t enable(unsigned int) = 0;  // slot 266
    virtual uintptr_t disable(unsigned int) = 0;  // slot 267
    virtual uintptr_t clientConnectWithTask(task*, IOService*, unsigned int);  // slot 268
    virtual uintptr_t clientDisconnect(IOService*, unsigned int);  // slot 269
    virtual uintptr_t isTerminating();  // slot 270
    virtual uintptr_t _RESERVEDIOSkywalkInterface0();  // slot 271
    virtual uintptr_t _RESERVEDIOSkywalkInterface1();  // slot 272
    virtual uintptr_t _RESERVEDIOSkywalkInterface2();  // slot 273
    virtual uintptr_t _RESERVEDIOSkywalkInterface3();  // slot 274
    virtual uintptr_t _RESERVEDIOSkywalkInterface4();  // slot 275
    virtual uintptr_t _RESERVEDIOSkywalkInterface5();  // slot 276
    virtual uintptr_t _RESERVEDIOSkywalkInterface6();  // slot 277
    virtual uintptr_t _RESERVEDIOSkywalkInterface7();  // slot 278
    virtual uintptr_t _RESERVEDIOSkywalkInterface8();  // slot 279
    virtual uintptr_t _RESERVEDIOSkywalkInterface9();  // slot 280
    virtual uintptr_t _RESERVEDIOSkywalkInterface10();  // slot 281
private:
    uint8_t _rtlPadIOSkywalkInterface[0x28];  // KC size 0xb0
};
static_assert(sizeof(IOSkywalkInterface) == 0xb0, "IOSkywalkInterface size");

class IOSkywalkNetworkInterface : public IOSkywalkInterface {
    OSDeclareAbstractStructors(IOSkywalkNetworkInterface)
public:
    struct IOSkywalkTSOOptions;
    struct RegistrationInfo;
    virtual void free() override;  // slot 18
    virtual bool init(OSDictionary*) override;  // slot 73
    virtual bool willTerminate(IOService*, unsigned int) override;  // slot 128
    virtual void stop(IOService*) override;  // slot 185
    virtual void joinPMtree(IOService*) override;  // slot 244
    virtual IOReturn setAggressiveness(unsigned long, unsigned long) override;  // slot 248
    virtual uintptr_t enable(unsigned int) override;  // slot 266
    virtual uintptr_t disable(unsigned int) override;  // slot 267
    virtual uintptr_t registerNetworkInterfaceWithLogicalLink(IOSkywalkNetworkInterface::RegistrationInfo const*, IOSkywalkLogicalLink*, IOSkywalkPacketBufferPool*, IOSkywalkPacketBufferPool*, unsigned int);  // slot 282
    virtual uintptr_t deregisterLogicalLink();  // slot 283
    virtual uintptr_t initBSDInterfaceParameters(ifnet_init_eparams*, sockaddr_dl**) = 0;  // slot 284
    virtual uintptr_t prepareBSDInterface(__ifnet*, unsigned int);  // slot 285
    virtual uintptr_t finalizeBSDInterface(__ifnet*, unsigned int);  // slot 286
    virtual uintptr_t getBSDInterface() const;  // slot 287
    virtual uintptr_t setBSDName(char const*);  // slot 288
    virtual uintptr_t getBSDName() const;  // slot 289
    virtual uintptr_t processBSDCommand(__ifnet*, unsigned int, void*);  // slot 290
    virtual uintptr_t processInterfaceCommand(ifdrv*);  // slot 291
    virtual uintptr_t interfaceAdvisoryEnable(bool);  // slot 292
    virtual uintptr_t setRxFlowSteering(unsigned int, ifnet_traffic_descriptor_common*, unsigned int);  // slot 293
    virtual uintptr_t setInterfaceEnable(bool);  // slot 294
    virtual uintptr_t setRunningState(bool);  // slot 295
    virtual uintptr_t handleChosenMedia(unsigned int);  // slot 296
    virtual uintptr_t getSupportedMediaArray(unsigned int*, unsigned int*);  // slot 297
    virtual uintptr_t getPacketTapInfo(unsigned int*, unsigned int*);  // slot 298
    virtual uintptr_t getUnsentDataByteCount(unsigned int*, unsigned int*, unsigned int) const;  // slot 299
    virtual uintptr_t getSupportedWakeFlags(unsigned int*);  // slot 300
    virtual uintptr_t enableNetworkWake(unsigned int);  // slot 301
    virtual uintptr_t calculateRingSizeForQueue(IOSkywalkPacketQueue const*, unsigned int*) const;  // slot 302
    virtual uintptr_t getMaxTransferUnit();  // slot 303
    virtual uintptr_t setMaxTransferUnit(unsigned int);  // slot 304
    virtual uintptr_t getMinPacketSize();  // slot 305
    virtual uintptr_t getHardwareAssists();  // slot 306
    virtual uintptr_t setHardwareAssists(unsigned int, unsigned int);  // slot 307
    virtual uintptr_t getInterfaceFamily();  // slot 308
    virtual uintptr_t getInterfaceSubFamily();  // slot 309
    virtual uintptr_t getInitialMedia();  // slot 310
    virtual uintptr_t getFeatureFlags();  // slot 311
    virtual uintptr_t getTxDataOffset();  // slot 312
    virtual uintptr_t captureInterfaceState(unsigned int);  // slot 313
    virtual uintptr_t restoreInterfaceState(unsigned int);  // slot 314
    virtual IOReturn setMTU(unsigned int);  // slot 315
    virtual uintptr_t bpfTap(unsigned int, unsigned int);  // slot 316
    virtual uintptr_t getBSDNamePrefix();  // slot 317
    virtual uintptr_t getBSDUnitNumber();  // slot 318
    virtual uintptr_t classNameOverride();  // slot 319
    virtual uintptr_t deferBSDAttach(bool);  // slot 320
    virtual uintptr_t reportDetailedLinkStatus(if_link_status const*);  // slot 321
    virtual uintptr_t getTSOOptions(IOSkywalkNetworkInterface::IOSkywalkTSOOptions*);  // slot 322
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface0();  // slot 323
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface1();  // slot 324
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface2();  // slot 325
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface3();  // slot 326
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface4();  // slot 327
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface5();  // slot 328
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface6();  // slot 329
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface7();  // slot 330
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface8();  // slot 331
    virtual uintptr_t _RESERVEDIOSkywalkNetworkInterface9();  // slot 332
private:
    uint8_t _rtlPadIOSkywalkNetworkInterface[0x30];  // KC size 0xe0
};
static_assert(sizeof(IOSkywalkNetworkInterface) == 0xe0, "IOSkywalkNetworkInterface size");

class IOSkywalkEthernetInterface : public IOSkywalkNetworkInterface {
    OSDeclareAbstractStructors(IOSkywalkEthernetInterface)
public:
    virtual void free() override;  // slot 18
    virtual bool init(OSDictionary*) override;  // slot 73
    virtual IOReturn newUserClient(task*, void*, unsigned int, OSDictionary*, IOUserClient**) override;  // slot 238
    virtual IOReturn setPowerState(unsigned long, IOService*) override;  // slot 253
    virtual uintptr_t enable(unsigned int) override;  // slot 266
    virtual uintptr_t initBSDInterfaceParameters(ifnet_init_eparams*, sockaddr_dl**) override;  // slot 284
    virtual uintptr_t prepareBSDInterface(__ifnet*, unsigned int) override;  // slot 285
    virtual uintptr_t processBSDCommand(__ifnet*, unsigned int, void*) override;  // slot 290
    virtual uintptr_t getPacketTapInfo(unsigned int*, unsigned int*) override;  // slot 298
    virtual uintptr_t enableNetworkWake(unsigned int) override;  // slot 301
    virtual uintptr_t getMaxTransferUnit() override;  // slot 303
    virtual uintptr_t getMinPacketSize() override;  // slot 305
    virtual uintptr_t getInterfaceFamily() override;  // slot 308
    virtual uintptr_t getInterfaceSubFamily() override;  // slot 309
    virtual uintptr_t getInitialMedia() override;  // slot 310
    virtual uintptr_t getBSDNamePrefix() override;  // slot 317
    virtual uintptr_t getHardwareAddress(ether_addr*);  // slot 333
    virtual uintptr_t setHardwareAddress(ether_addr*);  // slot 334
    virtual uintptr_t setLinkLayerAddress(ether_addr*);  // slot 335
    virtual uintptr_t configureMulticastFilter(unsigned int, ether_addr const*, unsigned int);  // slot 336
    virtual uintptr_t setMulticastAddresses(ether_addr const*, unsigned int);  // slot 337
    virtual uintptr_t setAllMulticastModeEnable(bool);  // slot 338
    virtual uintptr_t setPromiscuousModeEnable(bool, unsigned int);  // slot 339
    virtual uintptr_t reportNicProxyLimits(nicproxy_limits_info_s);  // slot 340
    virtual uintptr_t hwConfigNicProxyData(nicproxy_info_s*);  // slot 341
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface0();  // slot 342
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface1();  // slot 343
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface2();  // slot 344
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface3();  // slot 345
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface4();  // slot 346
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface5();  // slot 347
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface6();  // slot 348
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface7();  // slot 349
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface8();  // slot 350
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface9();  // slot 351
    virtual uintptr_t _RESERVEDIOSkywalkEthernetInterface10();  // slot 352
private:
    uint8_t _rtlPadIOSkywalkEthernetInterface[0x40];  // KC size 0x120
};
static_assert(sizeof(IOSkywalkEthernetInterface) == 0x120, "IOSkywalkEthernetInterface size");

class IO80211SkywalkInterface : public IOSkywalkEthernetInterface {
    OSDeclareAbstractStructors(IO80211SkywalkInterface)
public:
    virtual bool init() override;  // slot 17
    virtual void free() override;  // slot 18
    virtual IOReturn configureReport(IOReportChannelList*, unsigned int, void*, void*) override;  // slot 133
    virtual IOReturn updateReport(IOReportChannelList*, unsigned int, void*, void*) override;  // slot 134
    virtual bool start(IOService*) override;  // slot 184
    virtual void stop(IOService*) override;  // slot 185
    virtual IOReturn newUserClient(task*, void*, unsigned int, OSDictionary*, IOUserClient**) override;  // slot 238
    virtual const char * stringFromReturn(int) override;  // slot 240
    virtual int errnoFromReturn(int) override;  // slot 241
    virtual IOReturn setPowerState(unsigned long, IOService*) override;  // slot 253
    virtual unsigned long maxCapabilityForDomainState(unsigned long) override;  // slot 254
    virtual unsigned long initialPowerStateForDomainState(unsigned long) override;  // slot 255
    virtual uintptr_t enable(unsigned int) override;  // slot 266
    virtual uintptr_t disable(unsigned int) override;  // slot 267
    virtual uintptr_t initBSDInterfaceParameters(ifnet_init_eparams*, sockaddr_dl**) override;  // slot 284
    virtual uintptr_t prepareBSDInterface(__ifnet*, unsigned int) override;  // slot 285
    virtual uintptr_t processBSDCommand(__ifnet*, unsigned int, void*) override;  // slot 290
    virtual uintptr_t setInterfaceEnable(bool) override;  // slot 294
    virtual uintptr_t setRunningState(bool) override;  // slot 295
    virtual uintptr_t handleChosenMedia(unsigned int) override;  // slot 296
    virtual uintptr_t getSupportedMediaArray(unsigned int*, unsigned int*) override;  // slot 297
    virtual uintptr_t getFeatureFlags() override;  // slot 311
    virtual uintptr_t classNameOverride() override;  // slot 319
    virtual uintptr_t getHardwareAddress(ether_addr*) override;  // slot 333
    virtual uintptr_t setHardwareAddress(ether_addr*) override;  // slot 334
    virtual uintptr_t setPromiscuousModeEnable(bool, unsigned int) override;  // slot 339
    virtual uintptr_t createPeerManager();  // slot 353
    virtual uintptr_t createPeer(unsigned char const*, IO80211PeerManager*);  // slot 354
    virtual uintptr_t postMessage(unsigned int, void*, unsigned long, bool);  // slot 355
    virtual uintptr_t reportDataPathEvents(unsigned int, void*, unsigned long, bool);  // slot 356
    virtual uintptr_t recordOutputPackets(TxSubmissionDequeueStats*, TxSubmissionDequeueStats*);  // slot 357
    virtual uintptr_t recordOutputPacket(apple80211_wme_ac, int, int);  // slot 358
    virtual uintptr_t logTxPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, apple80211_wme_ac, bool);  // slot 359
    virtual uintptr_t logTxCompletionPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, unsigned char*, apple80211_wme_ac, int, unsigned int, bool, bool);  // slot 360
    virtual uintptr_t recordCompletionPackets(TxCompletionEnqueueStats*, TxCompletionEnqueueStats*);  // slot 361
    virtual uintptr_t inputPacket(IO80211NetworkPacket*, packet_info_tag*, ether_header*, bool*, bool);  // slot 362
    virtual uintptr_t forwardInfraRelayPackets(IO80211NetworkPacket*, ether_header*);  // slot 363
    virtual uintptr_t logSkywalkTxReqPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, unsigned char*, apple80211_wme_ac, bool);  // slot 364
    virtual uintptr_t pendingPackets(unsigned char);  // slot 365
    virtual uintptr_t packetSpace(unsigned char);  // slot 366
    virtual uintptr_t isChipInterfaceReady();  // slot 367
    virtual uintptr_t isDebounceOnGoing();  // slot 368
    virtual uintptr_t linkState();  // slot 369
    virtual uintptr_t setScanningState(unsigned int, bool, apple80211_scan_data*, int);  // slot 370
    virtual uintptr_t setDataPathState(bool);  // slot 371
    virtual uintptr_t getScanManager();  // slot 372
    virtual uintptr_t getController();  // slot 373
    virtual uintptr_t updateLinkParameters(apple80211_interface_availability*);  // slot 374
    virtual uintptr_t updateInterfaceCoexRiskPct(unsigned long long);  // slot 375
    virtual IOReturn setLQM(unsigned long long);  // slot 376
    virtual uintptr_t updateLinkStatus();  // slot 377
    virtual uintptr_t updateLinkStatusGated();  // slot 378
    virtual uintptr_t setInterfaceExtendedCCA(apple80211_channel, apple80211_cca_report*);  // slot 379
    virtual uintptr_t setInterfaceCCA(apple80211_channel, int);  // slot 380
    virtual uintptr_t setInterfaceNF(apple80211_channel, long long);  // slot 381
    virtual uintptr_t setInterfaceOFDMDesense(apple80211_channel, long long);  // slot 382
    virtual uintptr_t removePacketQueue(IO80211FlowQueueHash*);  // slot 383
    virtual uintptr_t setDebugFlags(unsigned long long, unsigned int);  // slot 384
    virtual uintptr_t debugFlags();  // slot 385
    virtual uintptr_t setInterfaceChipCounters(apple80211_stat_report*, apple80211_chip_counters_tx*, apple80211_chip_error_counters_tx*, apple80211_chip_counters_rx*);  // slot 386
    virtual uintptr_t setInterfaceMIBdot11(apple80211_stat_report*, apple80211_ManagementInformationBasedot11_counters*);  // slot 387
    virtual uintptr_t setFrameStats(apple80211_stat_report*, apple80211_frame_counters*);  // slot 388
    virtual uintptr_t setInfraSpecificFrameStats(apple80211_stat_report*, apple80211_infra_specific_stats*);  // slot 389
    virtual uintptr_t setRxDataStallStats(apple80211_stat_report*, apple80211_rx_data_stall_report*);  // slot 390
    virtual uintptr_t getWmeTxCounters(unsigned long long*);  // slot 391
    virtual uintptr_t setPeerManagerLogFlag(unsigned int, unsigned int, unsigned int);  // slot 392
    virtual uintptr_t setWoWEnabled(bool);  // slot 393
    virtual uintptr_t wowEnabled();  // slot 394
    virtual uintptr_t printDataPath(userPrintCtx*);  // slot 395
    virtual uintptr_t getDataQueueDepth();  // slot 396
    virtual uintptr_t findOrCreateFlowQueue(IO80211FlowQueueHash);  // slot 397
    virtual uintptr_t findOrCreateFlowQueueWithCache(IO80211FlowQueueHash, bool*);  // slot 398
    virtual uintptr_t findExistingFlowQueue(IO80211FlowQueueHash);  // slot 399
    virtual uintptr_t removePacketQueue(IO80211FlowQueueHash const*);  // slot 400
    virtual uintptr_t flushPacketQueues();  // slot 401
    virtual uintptr_t cachePeer(ether_addr*, unsigned int*);  // slot 402
    virtual uintptr_t shouldLog(unsigned long long);  // slot 403
    virtual uintptr_t vlogDebug(unsigned long long, char const*, va_list);  // slot 404
    virtual uintptr_t vlogDebugBPF(unsigned long long, char const*, va_list);  // slot 405
    virtual uintptr_t createLinkQualityMonitor(IO80211Peer*, bool);  // slot 406
    virtual uintptr_t releaseLinkQualityMonitor(IO80211Peer*);  // slot 407
    virtual uintptr_t getP2PSkywalkPeerMgr();  // slot 408
    virtual bool isCommandProhibited(int);  // slot 409
    virtual uintptr_t findPeer(ether_addr&);  // slot 410
    virtual uintptr_t setNotificationProperty(OSSymbol const*, OSObject const*);  // slot 411
    virtual uintptr_t getWorkerMatchingDict(OSString*);  // slot 412
    virtual uintptr_t init(IOService*, ether_addr*);  // slot 413
    virtual uintptr_t isInterfaceEnabled();  // slot 414
    virtual uintptr_t getSelfMacAddr();  // slot 415
    virtual uintptr_t getPacketPool(OSString*);  // slot 416
    virtual void * getLogger() const;  // slot 417
    virtual uintptr_t handleSIOCSIFADDR();  // slot 418
    virtual uintptr_t debugHandler(apple80211_debug_command*);  // slot 419
    virtual uintptr_t statsDump();  // slot 420
    virtual uintptr_t powerOnNotification();  // slot 421
    virtual uintptr_t powerOffNotification();  // slot 422
    virtual uintptr_t getTxQueueDepth();  // slot 423
    virtual uintptr_t getRxQueueCapacity();  // slot 424
    virtual uintptr_t updateRxCounter(unsigned long long);  // slot 425
    virtual uintptr_t getMultiCastQueue();  // slot 426
    virtual uintptr_t getAssocState();  // slot 427
    virtual uintptr_t notifyQueueState(apple80211_wme_ac, unsigned short);  // slot 428
    virtual uintptr_t getTxHeadroom();  // slot 429
    virtual uintptr_t getRxCompQueue();  // slot 430
    virtual uintptr_t getTxCompQueue();  // slot 431
    virtual uintptr_t getTxSubQueue(apple80211_wme_ac);  // slot 432
    virtual uintptr_t getTxPacketPool();  // slot 433
    virtual uintptr_t getRxPacketPool();  // slot 434
    virtual uintptr_t enableDatapath();  // slot 435
    virtual uintptr_t disableDatapath();  // slot 436
    virtual uintptr_t getNumTxQueues();  // slot 437
    virtual uintptr_t getLQMSummary(apple80211_lqm_summary*);  // slot 438
    virtual uintptr_t getEventPipeSize();  // slot 439
    virtual uintptr_t createEventPipe(IO80211APIUserClient*);  // slot 440
    virtual uintptr_t destroyEventPipe(IO80211APIUserClient*);  // slot 441
    virtual uintptr_t setUserBufferInfo(IOMemoryDescriptor*, unsigned long long);  // slot 442
    virtual uintptr_t postMessageIOUC(char const*, unsigned int, void*, unsigned long);  // slot 443
    virtual uintptr_t isIOUCPipeOpened();  // slot 444
    virtual uintptr_t getRingMD(IO80211APIUserClient*, unsigned long long);  // slot 445
    virtual uintptr_t attachPeer(ether_addr*);  // slot 446
    virtual uintptr_t detachPeer(ether_addr*);  // slot 447
    virtual uintptr_t setDebugTrafficReport(bool);  // slot 448
    virtual uintptr_t getDataPathInterfaceStats(apple80211_data_path_interface_stats*);  // slot 449
    virtual uintptr_t getDataPathPeerStats(apple80211_data_path_peer_stats*);  // slot 450
    virtual uintptr_t getLastQueuePacketTime(ether_addr*);  // slot 451
    virtual uintptr_t getLastRxUnicastLinkActivityTime(ether_addr*);  // slot 452
    virtual uintptr_t updateInterfaceDataStats(apple80211_data_path_interface_stats*);  // slot 453
    virtual uintptr_t updatePeerDataStats(apple80211_data_path_peer_stats*);  // slot 454
    virtual uintptr_t logTxLatency(unsigned char*, unsigned int, unsigned long long);  // slot 455
    virtual uintptr_t logRxLatency(unsigned int, unsigned long long);  // slot 456
    virtual uintptr_t getNClearTxRxLatency(apple80211_latency_all_ac*, apple80211_latency_all_ac*);  // slot 457
    virtual uintptr_t getLastTxTimeStamp(unsigned long long&);  // slot 458
    virtual uintptr_t getLastRxTimeStamp(unsigned long long&);  // slot 459
    virtual uintptr_t syncDPSStats(apple82011_postMessage_dps*);  // slot 460
private:
    uint8_t _rtlPadIO80211SkywalkInterface[0x8];  // KC size 0x128
};
static_assert(sizeof(IO80211SkywalkInterface) == 0x128, "IO80211SkywalkInterface size");

#endif /* ITLWM_IO80211_SKYWALK_ABI_25G332_H */
