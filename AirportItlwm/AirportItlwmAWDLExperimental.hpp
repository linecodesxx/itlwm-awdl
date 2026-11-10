//
//  AirportItlwmAWDLExperimental.hpp
//  AirportItlwm-Tahoe
//
//  Stage-1 gate for native AWDL (role 6) on IO80211Family / IOSkywalkFamily.
//
//  OFF by default. It can be armed in two steps:
//    1. build with  ITLWM_AWDL_EXPERIMENTAL=1
//       (for example:
//        xcodebuild ... GCC_PREPROCESSOR_DEFINITIONS='$(inherited) ITLWM_AWDL_EXPERIMENTAL=1')
//    2. boot with   itlwm_awdl=1
//
//  Even when armed, Stage 1 does not allocate or register a virtual interface.
//  The 25G332 IO80211AWDLProtocol vtable is abstract and the headers in this
//  tree do not describe its base chain; see the ABI gate in
//  AirportItlwmSkywalkInterface::setVIRTUAL_IF_CREATE.
//
//  This file is header-only on purpose: it must not change the ABI of any
//  class in the driver.
//

#ifndef AirportItlwmAWDLExperimental_hpp
#define AirportItlwmAWDLExperimental_hpp

#ifndef ITLWM_AWDL_EXPERIMENTAL
#define ITLWM_AWDL_EXPERIMENTAL 0
#endif

#if ITLWM_AWDL_EXPERIMENTAL
#include <pexpert/pexpert.h>
#endif

/*
 * Runtime gate. Returns true only when the code was built with the
 * experimental path and the operator explicitly asked for it with the
 * itlwm_awdl=1 boot-arg. Unknown/missing boot-arg means "off".
 */
static inline bool itlwmAwdlExperimentalEnabled()
{
#if ITLWM_AWDL_EXPERIMENTAL
    int enabled = 0;

    if (PE_parse_boot_argn("itlwm_awdl", &enabled, sizeof(enabled)))
        return enabled != 0;
#endif
    return false;
}

#endif /* AirportItlwmAWDLExperimental_hpp */
