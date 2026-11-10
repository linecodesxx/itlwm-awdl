/*
 * EXPERIMENTAL ABI reconstruction for macOS 26.7.2 (25G332, Darwin 25.6.0).
 *
 * Not part of the driver build. Legacy headers are untouched.
 *
 * VERIFIED against BootKernelExtensions.kc by decoding __ZTV23IO80211VirtualInterface
 * (0xffffff8002478b78, span 0xFE0 -> 505 function slots, 0..504):
 *   461..487  27 new virtuals, each resolved to its IO80211VirtualInterface symbol
 *   488       ___cxa_pure_virtual  => setMacAddress is pure
 *   489..504  _RESERVEDIO80211VirtualInterface0..15
 *   class size 0x130 (MetaClassC1)
 *   IO80211VirtualInterface::MetaClassC1 constructs a 0x130-byte class.
 *
 * Signatures for 461..487 are VERIFIED from the KC mangled names (see slot
 * comments). Return types are NOT encoded in Itanium mangled names, so every
 * return type below remains a reference-derived assumption.
 *
 * The AWDL branch is IO80211AWDLProtocol : IO80211VirtualInterface :
 * IO80211SkywalkInterface. Do not include this header together with the legacy
 * include/Airport/IO80211VirtualInterface.h or with IO80211InfraInterface: the
 * Infra branch is a different subclass of IO80211SkywalkInterface and its slot
 * numbers (setMacAddress at 468) do not apply here.
 */
#ifndef ITLWM_IO80211_VIRTUAL_INTERFACE_ABI_25G332_H
#define ITLWM_IO80211_VIRTUAL_INTERFACE_ABI_25G332_H

#include "IO80211SkywalkABI_25G332.h"

struct peerSlotDataStats;
struct packetCounters_t;

/*
 * IO80211LinkState is already declared by the base header as
 *   enum IO80211LinkState : uint32_t {}
 * which matches the KC mangled name 16IO80211LinkState (by-value, 4 bytes).
 * The real enumerators (apple80211_var.h) are:
 *   kIO80211NetworkLinkUndefined=0, kIO80211NetworkLinkDown=1,
 *   kIO80211NetworkLinkUp=2.
 */

struct IO80211VirtualInterface : public IO80211SkywalkInterface {
    OSDeclareAbstractStructors(IO80211VirtualInterface)
public:
    /*
     * Overrides of inherited slots. They add no vtable entries; declaring them
     * makes the compiler reference the real IO80211VirtualInterface symbols
     * (resolved from IO80211Family at load time) instead of the Skywalk bodies.
     */
    virtual void free() override;
    virtual void stop(IOService*) override;
    /* KC slot 266 is IO80211VirtualInterface::enable; slot 267 stays
     * IO80211SkywalkInterface::disable, so disable is intentionally NOT
     * redeclared here. */
    virtual uintptr_t enable(unsigned int) override;
    virtual IOReturn configureReport(IOReportChannelList*, unsigned int, void*, void*) override;
    virtual IOReturn updateReport(IOReportChannelList*, unsigned int, void*, void*) override;
    virtual bool start(IOService*) override;
    virtual bool terminate(IOOptionBits options = 0) override;
    virtual bool attach(IOService*) override;
    virtual void detach(IOService*) override;
    virtual IOReturn newUserClient(task*, void*, unsigned int, OSDictionary*, IOUserClient**) override;
    virtual const char* stringFromReturn(int) override;
    virtual uintptr_t prepareBSDInterface(__ifnet*, unsigned int) override;
    virtual uintptr_t processBSDCommand(__ifnet*, unsigned int, void*) override;
    virtual uintptr_t setInterfaceEnable(bool) override;
    virtual uintptr_t getFeatureFlags() override;
    virtual uintptr_t bpfTap(unsigned int, unsigned int) override;
    virtual uintptr_t setMulticastAddresses(ether_addr const*, unsigned int) override;
    virtual uintptr_t createPeerManager() override;
    virtual uintptr_t postMessage(unsigned int, void*, unsigned long, bool) override;
    virtual uintptr_t logTxPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, apple80211_wme_ac, bool) override;
    virtual uintptr_t logTxCompletionPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, unsigned char*, apple80211_wme_ac, int, unsigned int, bool, bool) override;
    virtual uintptr_t inputPacket(IO80211NetworkPacket*, packet_info_tag*, ether_header*, bool*, bool) override;
    virtual uintptr_t logSkywalkTxReqPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, unsigned char*, apple80211_wme_ac, bool) override;
    virtual uintptr_t pendingPackets(unsigned char) override;
    virtual uintptr_t packetSpace(unsigned char) override;
    virtual uintptr_t linkState() override;
    virtual uintptr_t setScanningState(unsigned int, bool, apple80211_scan_data*, int) override;
    virtual uintptr_t updateLinkParameters(apple80211_interface_availability*) override;
    virtual uintptr_t updateInterfaceCoexRiskPct(unsigned long long) override;
    virtual uintptr_t setInterfaceNF(apple80211_channel, long long) override;
    virtual uintptr_t setDebugFlags(unsigned long long, unsigned int) override;
    virtual uintptr_t debugFlags() override;
    virtual uintptr_t setInterfaceChipCounters(apple80211_stat_report*, apple80211_chip_counters_tx*, apple80211_chip_error_counters_tx*, apple80211_chip_counters_rx*) override;
    virtual uintptr_t setInterfaceMIBdot11(apple80211_stat_report*, apple80211_ManagementInformationBasedot11_counters*) override;
    virtual uintptr_t setFrameStats(apple80211_stat_report*, apple80211_frame_counters*) override;
    virtual uintptr_t setInfraSpecificFrameStats(apple80211_stat_report*, apple80211_infra_specific_stats*) override;
    virtual uintptr_t getWmeTxCounters(unsigned long long*) override;
    virtual uintptr_t setWoWEnabled(bool) override;
    virtual uintptr_t wowEnabled() override;
    virtual uintptr_t printDataPath(userPrintCtx*) override;
    virtual uintptr_t findOrCreateFlowQueue(IO80211FlowQueueHash) override;
    virtual uintptr_t findOrCreateFlowQueueWithCache(IO80211FlowQueueHash, bool*) override;
    virtual uintptr_t findExistingFlowQueue(IO80211FlowQueueHash) override;
    virtual uintptr_t removePacketQueue(IO80211FlowQueueHash const*) override;
    virtual uintptr_t flushPacketQueues() override;
    virtual uintptr_t cachePeer(ether_addr*, unsigned int*) override;
    virtual uintptr_t shouldLog(unsigned long long) override;
    virtual uintptr_t vlogDebug(unsigned long long, char const*, va_list) override;
    virtual uintptr_t vlogDebugBPF(unsigned long long, char const*, va_list) override;
    virtual uintptr_t getP2PSkywalkPeerMgr() override;
    virtual uintptr_t findPeer(ether_addr&) override;
    virtual uintptr_t getEventPipeSize() override;
    virtual uintptr_t createEventPipe(IO80211APIUserClient*) override;
    using IO80211SkywalkInterface::init;

    /* ---- New virtuals, slots 461..487 (signatures from KC mangled names) ---- */
    virtual bool init(IO80211Controller*, ether_addr*, uint32_t, char const*);          /* 461 */
    virtual uintptr_t setLinkState(IO80211LinkState, unsigned int);                     /* 462 */
    virtual uintptr_t forwardPacket(IO80211NetworkPacket*);                             /* 463 */
    virtual uintptr_t handleIoctl(__ifnet*, unsigned long, void*);                      /* 464 */
    virtual uintptr_t addPeerToCache(unsigned char*);                                   /* 465 */
    virtual uintptr_t deletePeerFromCache(unsigned char*);                              /* 466 */
    virtual uintptr_t handleDebugCmd(apple80211_debug_command*);                        /* 467 */
    virtual uintptr_t getPeerSlotDataStats(ether_addr*, bool, peerSlotDataStats*);      /* 468 */
    virtual uintptr_t getPeerDataStats(ether_addr*, bool, packetCounters_t*);           /* 469 */
    virtual uintptr_t clearPeerDataStats(ether_addr*, bool);                            /* 470 */
    virtual uintptr_t setAwdlCurrentChannelSequenceIndex(unsigned int);                 /* 471 */
    virtual uintptr_t isOutputFlowControlled();                                         /* 472 */
    virtual uintptr_t setOutputFlowControlled();                                        /* 473 */
    virtual uintptr_t clearOutputFlowControlled();                                      /* 474 */
    virtual uintptr_t flushFlowQueues(ether_addr*);                                     /* 475 */
    virtual uintptr_t configureAQMOutput();                                             /* 476 */
    virtual uintptr_t attachToBpf();                                                    /* 477 */
    virtual uintptr_t configureIfnet();                                                 /* 478 */
    virtual uintptr_t reset();                                                          /* 479 */
    virtual uintptr_t dupAndTransmitMcastPacket(IO80211NetworkPacket*, unsigned char*, unsigned char*); /* 480 */
    virtual uintptr_t freeMulticastPacket(IO80211NetworkPacket*);                       /* 481 */
    virtual uintptr_t triggerAWDLMulticastTx(unsigned int, unsigned char);              /* 482 */
    virtual uintptr_t p2pMulticastTx(IO80211NetworkPacket**, unsigned int);             /* 483 */
    virtual uintptr_t p2pMulticastTx(unsigned int, bool);                               /* 484 */
    virtual uintptr_t signalMulticastPackets(unsigned int, unsigned int);               /* 485 */
    virtual uintptr_t sendPendingPacketsToStack(IO80211NetworkPacket**, unsigned int);  /* 486 */
    virtual uintptr_t freePendingPackets(IO80211NetworkPacket**, unsigned int);         /* 487 */

    /* ---- Slot 488: pure in the KC (___cxa_pure_virtual) ---- */
    virtual uintptr_t setMacAddress(ether_addr&) = 0;                                   /* 488 */

    /* ---- Slots 489..504: real _RESERVEDIO80211VirtualInterface symbols ---- */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface0();                              /* 489 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface1();                              /* 490 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface2();                              /* 491 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface3();                              /* 492 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface4();                              /* 493 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface5();                              /* 494 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface6();                              /* 495 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface7();                              /* 496 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface8();                              /* 497 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface9();                              /* 498 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface10();                             /* 499 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface11();                             /* 500 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface12();                             /* 501 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface13();                             /* 502 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface14();                             /* 503 */
    virtual uintptr_t _RESERVEDIO80211VirtualInterface15();                             /* 504 */

private:
    uint8_t _abiPadIO80211VirtualInterface[0x8];  /* 0x128 -> KC size 0x130 */
};
static_assert(sizeof(IO80211VirtualInterface) == 0x130,
              "IO80211VirtualInterface must match 25G332 MetaClassC1 size");

#endif /* ITLWM_IO80211_VIRTUAL_INTERFACE_ABI_25G332_H */
