/*
 * EXPERIMENTAL: IOSkywalk RegistrationInfo layout for macOS 26.7.2 (25G332).
 *
 * This header is not part of the default build. It is included only by
 * AirportItlwm/AirportItlwmAWDLInterface.cpp when ITLWM_AWDL_EXPERIMENTAL=1.
 *
 * The legacy headers in include/Airport/macOS26/ declare RegistrationInfo as
 * an opaque uint8_t[0x130]. The fields below were decoded from the local
 * BootKernelExtensions.kc so Stage 3 can validate a buffer without calling
 * the real registration path:
 *
 *   IOSkywalkEthernetInterface::initRegistrationInfo
 *       0xffffff8002b158e2
 *       requires (info != NULL, arg2 == 1, size == 0x130), then:
 *         +0x00 = 1                    (version)
 *         +0x04 = 0x130                (size)
 *         +0x08 = getInterfaceFamily() or 2
 *         +0x0c = getInterfaceSubFamily()
 *         +0x14 = 0x100
 *         +0x24 = 0x22
 *         +0x28 = getInitialMedia() or 0x20
 *         +0x30 = getBSDNamePrefix() or a default string
 *         +0x38 = getBSDUnitNumber()
 *         +0x3c = 0                    (headroom; bzero, getTxDataOffset reads u16)
 *         +0x44 = getMinPacketSize() or 0x2e
 *         +0x48 = getMaxTransferUnit() or 0x5dc
 *         +0x4c = getMaxTransferUnit() or 0x5dc
 *
 *   IOSkywalkNetworkInterface::getTxDataOffset
 *       0xffffff8002b10aba
 *       reads a uint16_t at info + 0x3c. The base initRegistrationInfo()
 *       zeroes it. AppleBCMWLANProximityInterface::start() does not patch it,
 *       so an AWDL RegistrationInfo built from the base fallbacks has
 *       headroom 0. A live path must take the value from the controller.
 *
 *   AppleBCMWLANProximityInterface::start
 *       0xffffff800172d3a6
 *       patches +0x48 and +0x4c to 0x59400000594 (both MTU fields = 1428).
 *       It does not patch +0x3c, +0x28 (media) or +0x44 (min packet size).
 *       AppleBCMWLANProximityInterface::getBSDNamePrefix returns "awdl".
 *
 *   IOSkywalkEthernetInterface::copyRegistrationInfo
 *       0xffffff8002b159f0
 *       requires (version == 1, size >= 0x130), allocates 0x130 bytes,
 *       copies them, and sets "IOMACAddress" from info + 0x108, length 6.
 *
 *   IOSkywalkNetworkInterface::registerNetworkInterface
 *       0xffffff8002b0f204
 *       requires (version == 1, size >= 0x108, info[0x14] <= 0x400,
 *       logical link != NULL, at least one packet pool != NULL), allocates
 *       0x108 bytes, copies the prefix, and consumes info[0x24] / info[0x28].
 *       It does not read the MAC; the Ethernet wrapper has already stored it.
 *
 *   IOSkywalkEthernetInterface::registerEthernetInterface
 *       0xffffff8002b15a84
 *       calls copyRegistrationInfo() first, then registerNetworkInterface()
 *       with the Ethernet copy, then frees on failure.
 *
 *   IOSkywalkEthernetInterface::deregisterEthernetInterface
 *       0xffffff8002b15af4
 *   IOSkywalkNetworkInterface::deregisterNetworkInterface
 *       0xffffff8002b0ee9c
 *       release the logical link, packet pools, ordered set and the 0x108 /
 *       0x130 copies. The Network deregister frees mExpansionData->fRegistrationInfo
 *       (offset 0xc0); the Ethernet deregister frees mExpansionData2's copy
 *       (offset 0x118) after a successful network deregister.
 *
 * The struct is a compile-time check only. Stage 3 never passes it to
 * registerEthernetInterface() or registerNetworkInterfaceWithLogicalLink().
 */
#ifndef ITLWM_IO80211_AWDL_REGISTRATION_INFO_25G332_H
#define ITLWM_IO80211_AWDL_REGISTRATION_INFO_25G332_H

#include <stddef.h>
#include <stdint.h>

namespace itlwm_awdl_25g332 {

/* Values verified in initRegistrationInfo / copyRegistrationInfo. */
constexpr uint32_t kRegistrationVersion = 1;
constexpr uint32_t kEthernetRegistrationSize = 0x130;
constexpr uint32_t kNetworkRegistrationMinSize = 0x108;
constexpr uint32_t kRegistrationMaxSizeField = 0x400;
constexpr uint32_t kMacAddressOffset = 0x108;
constexpr uint32_t kMacAddressLength = 6;

/*
 * AWDL values verified in AppleBCMWLANProximityInterface on 25G332.
 *
 * - start() patches both MTU fields to 0x594 (1428), not the 0x5dc (1500)
 *   Ethernet fallback.
 * - getBSDNamePrefix() returns the literal "awdl".
 * - start() does not patch headroom, media or minimum packet size, so those
 *   keep the base initRegistrationInfo() fallbacks. Headroom is 0 in the base
 *   buffer; a live path must replace it with the controller's value.
 */
constexpr uint32_t kAwdlMaxTransferUnit = 0x594; /* 1428 */
constexpr uint16_t kAwdlTxDataOffset = 0;        /* base init zeroes +0x3c */
constexpr uint32_t kAwdlInitialMedia = 0x20;
constexpr uint32_t kAwdlMinPacketSize = 0x2e;
constexpr char kAwdlBsdNamePrefix[] = "awdl";

/*
 * First 0x108 bytes, the part copied by registerNetworkInterface().
 * Every offset is asserted below against the KC disassembly.
 */
struct NetworkRegistrationInfoPrefix {
    uint32_t version;                 /* +0x00 */
    uint32_t size;                    /* +0x04 */
    uint32_t interfaceFamily;         /* +0x08 */
    uint32_t interfaceSubFamily;      /* +0x0c */
    uint8_t  reserved0[0x04];         /* +0x10 */
    uint32_t maxSize;                 /* +0x14, must be <= 0x400 */
    uint8_t  reserved1[0x0c];         /* +0x18 */
    uint32_t reserved2;               /* +0x24, init writes 0x22 */
    uint32_t initialMedia;            /* +0x28 */
    uint8_t  reserved3[0x04];         /* +0x2c */
    uintptr_t bsdNamePrefix;          /* +0x30, pointer-sized */
    uint32_t bsdUnitNumber;           /* +0x38 */
    uint16_t txDataOffset;            /* +0x3c, getTxDataOffset reads u16 */
    uint8_t  reserved4[0x06];         /* +0x3e */
    uint32_t minPacketSize;           /* +0x44 */
    uint32_t maxTransferUnit;         /* +0x48 */
    uint32_t maxTransferUnit2;        /* +0x4c */
    uint8_t  reserved5[0x108 - 0x50]; /* +0x50 .. +0x107 */
};

/*
 * Full 0x130-byte object used by IOSkywalkEthernetInterface.
 * The MAC is at +0x108; copyRegistrationInfo() reads exactly 6 bytes there.
 */
struct EthernetRegistrationInfo {
    NetworkRegistrationInfoPrefix prefix;
    uint8_t mac[kMacAddressLength];                    /* +0x108 */
    uint8_t reserved6[0x130 - 0x10e];                  /* +0x10e .. +0x12f */
};

static_assert(sizeof(NetworkRegistrationInfoPrefix) == 0x108,
              "RegistrationInfo prefix must match registerNetworkInterface's 0x108 copy");
static_assert(sizeof(EthernetRegistrationInfo) == 0x130,
              "Ethernet RegistrationInfo must match initRegistrationInfo's 0x130 size");

static_assert(offsetof(NetworkRegistrationInfoPrefix, version) == 0x00, "version offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, size) == 0x04, "size offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, interfaceFamily) == 0x08, "interfaceFamily offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, interfaceSubFamily) == 0x0c, "interfaceSubFamily offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, maxSize) == 0x14, "maxSize offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, reserved2) == 0x24, "reserved2 offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, initialMedia) == 0x28, "initialMedia offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, bsdNamePrefix) == 0x30, "bsdNamePrefix offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, bsdUnitNumber) == 0x38, "bsdUnitNumber offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, txDataOffset) == 0x3c, "txDataOffset offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, minPacketSize) == 0x44, "minPacketSize offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, maxTransferUnit) == 0x48, "maxTransferUnit offset");
static_assert(offsetof(NetworkRegistrationInfoPrefix, maxTransferUnit2) == 0x4c, "maxTransferUnit2 offset");
static_assert(offsetof(EthernetRegistrationInfo, mac) == kMacAddressOffset, "MAC offset");
static_assert(kNetworkRegistrationMinSize < kEthernetRegistrationSize,
              "network registration consumes a prefix of the Ethernet object");
static_assert(kAwdlMaxTransferUnit == 1428,
              "AppleBCMWLANProximityInterface patches AWDL MTU to 0x594");
static_assert(kAwdlTxDataOffset == 0,
              "base initRegistrationInfo zeroes the AWDL headroom field");
static_assert(sizeof(kAwdlBsdNamePrefix) == 5,
              "AppleBCMWLANProximityInterface::getBSDNamePrefix returns \"awdl\"");

} /* namespace itlwm_awdl_25g332 */

#endif /* ITLWM_IO80211_AWDL_REGISTRATION_INFO_25G332_H */
