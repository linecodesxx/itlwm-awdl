//
//  AirportItlwmAWDLDatapath.cpp
//  AirportItlwm-Tahoe
//
//  Stage 4: experimental Skywalk datapath resource manager.
//
//  The generic manager and the host-testable ledger are pure C++ and live in
//  AirportItlwmAWDLDatapath.hpp. This file adds the real 25G332 Skywalk
//  backend. The backend is fail-closed by ITLWM_AWDL_DATAPATH_LIVE (default
//  0): its create methods return NULL, so even an accidental call cannot
//  allocate a pool or queue. The live helpers are still emitted so the strict
//  link check can prove the exact KC symbols and calling convention.
//
//  Nothing in this file registers, attaches, starts, enables or transmits.
//  It is not referenced by AirportItlwmAWDLInterface, setVIRTUAL_IF_CREATE or
//  any hardware path.
//
//  Compiled only when ITLWM_AWDL_EXPERIMENTAL=1. The default build compiles
//  this file to an empty translation unit.
//

#include "AirportItlwmAWDLDatapath.hpp"

#if ITLWM_AWDL_EXPERIMENTAL

#include <IOKit/IOService.h>
#include <IOKit/skywalk/IOSkywalkPacketBufferPool.h>
#include <IOKit/skywalk/IOSkywalkTxSubmissionQueue.h>
#include <IOKit/skywalk/IOSkywalkTxCompletionQueue.h>
#include <IOKit/skywalk/IOSkywalkRxCompletionQueue.h>

/*
 * Stage 4 must not allocate live Skywalk resources. Removing this assert is
 * a deliberate Stage 5 change, not a configuration option.
 */
static_assert(ITLWM_AWDL_DATAPATH_LIVE == 0,
              "Stage 4 datapath must stay fail-closed");

/* ------------------------------------------------------------------------- */
/* Real 25G332 Skywalk backend                                               */
/* ------------------------------------------------------------------------- */

namespace {

/*
 * Exact 25G332 symbols. The SDK's queue callback typedefs use IOReturn
 * (int), while the Kernel Collection mangles the callback return as
 * unsigned int (`j`). Declaring the entry points with their exact symbols
 * and ABI-compatible signatures avoids referencing a non-existent PFi...
 * symbol.
 */
typedef uint32_t (*TxSubmissionAction)(OSObject *, IOSkywalkTxSubmissionQueue *,
                                       IOSkywalkPacket * const *, uint32_t, void *);
typedef uint32_t (*TxCompletionAction)(OSObject *, IOSkywalkTxCompletionQueue *,
                                       IOSkywalkPacket **, uint32_t, void *);
typedef uint32_t (*RxCompletionAction)(OSObject *, IOSkywalkRxCompletionQueue *,
                                       IOSkywalkPacket **, uint32_t, void *);

extern "C" IOSkywalkPacketBufferPool *awdlPoolWithName(
    const char *, OSObject *, uint32_t, const void *)
    asm("__ZN25IOSkywalkPacketBufferPool8withNameEPKcP8OSObjectjPKNS_11PoolOptionsE");

extern "C" IOSkywalkTxSubmissionQueue *awdlTxSubmissionWithPool(
    IOSkywalkPacketBufferPool *, uint32_t, uint32_t, OSObject *,
    TxSubmissionAction, void *, uint32_t)
    asm("__ZN26IOSkywalkTxSubmissionQueue8withPoolEP25IOSkywalkPacketBufferPooljjP8OSObjectPFjS3_PS_PKP15IOSkywalkPacketjPvES9_j");

extern "C" IOSkywalkTxCompletionQueue *awdlTxCompletionWithPool(
    IOSkywalkPacketBufferPool *, uint32_t, uint32_t, OSObject *,
    TxCompletionAction, void *, uint32_t)
    asm("__ZN26IOSkywalkTxCompletionQueue8withPoolEP25IOSkywalkPacketBufferPooljjP8OSObjectPFjS3_PS_PP15IOSkywalkPacketjPvES8_j");

extern "C" IOSkywalkRxCompletionQueue *awdlRxCompletionWithPool(
    IOSkywalkPacketBufferPool *, uint32_t, uint32_t, OSObject *,
    RxCompletionAction, void *, uint32_t)
    asm("__ZN26IOSkywalkRxCompletionQueue8withPoolEP25IOSkywalkPacketBufferPooljjP8OSObjectPFjS3_PS_PP15IOSkywalkPacketjPvES8_j");

uint32_t txSubmissionAction(OSObject *, IOSkywalkTxSubmissionQueue *,
                            IOSkywalkPacket * const *, uint32_t, void *)
{
    /* Never claim that any packet was transmitted. */
    return 0;
}

uint32_t txCompletionAction(OSObject *, IOSkywalkTxCompletionQueue *,
                            IOSkywalkPacket **, uint32_t, void *)
{
    return 0;
}

uint32_t rxCompletionAction(OSObject *, IOSkywalkRxCompletionQueue *,
                            IOSkywalkPacket **, uint32_t, void *)
{
    return 0;
}

/*
 * Live allocation helpers. They are emitted even while
 * ITLWM_AWDL_DATAPATH_LIVE is 0 so the strict link check can resolve the
 * exact KC symbols. The backend methods below do not call them unless the
 * gate is enabled.
 */
__attribute__((noinline, used))
IOSkywalkPacketBufferPool *createPoolLive(OSObject *owner, const char *name,
                                          const void *options)
{
    return awdlPoolWithName(name, owner, 0, options);
}

__attribute__((noinline, used))
IOSkywalkTxSubmissionQueue *createTxSubmissionLive(
    IOSkywalkPacketBufferPool *pool, uint32_t capacity, uint32_t queueId,
    OSObject *owner)
{
    return awdlTxSubmissionWithPool(pool, capacity, queueId, owner,
                                    txSubmissionAction, nullptr, 0);
}

__attribute__((noinline, used))
IOSkywalkTxCompletionQueue *createTxCompletionLive(
    IOSkywalkPacketBufferPool *pool, uint32_t capacity, uint32_t queueId,
    OSObject *owner)
{
    return awdlTxCompletionWithPool(pool, capacity, queueId, owner,
                                    txCompletionAction, nullptr, 0);
}

__attribute__((noinline, used))
IOSkywalkRxCompletionQueue *createRxCompletionLive(
    IOSkywalkPacketBufferPool *pool, uint32_t capacity, uint32_t queueId,
    OSObject *owner)
{
    return awdlRxCompletionWithPool(pool, capacity, queueId, owner,
                                    rxCompletionAction, nullptr, 0);
}

class SkywalkBackend : public itlwm_awdl_datapath::Backend {
public:
    explicit SkywalkBackend(OSObject *owner) : owner_(owner) {}

    void *createPool(const char *name,
                     const itlwm_awdl_datapath::PoolOptions &options) override
    {
#if ITLWM_AWDL_DATAPATH_LIVE
        return createPoolLive(owner_, name, &options);
#else
        (void)name;
        (void)options;
        return nullptr;
#endif
    }

    void destroyPool(void *pool) override
    {
        if (pool != nullptr)
            static_cast<IOSkywalkPacketBufferPool *>(pool)->release();
    }

    void *createTxSubmissionQueue(
        void *pool, uint32_t capacity, uint32_t queueId,
        const itlwm_awdl_datapath::TxAction &action) override
    {
#if ITLWM_AWDL_DATAPATH_LIVE
        (void)action;
        return createTxSubmissionLive(
            static_cast<IOSkywalkPacketBufferPool *>(pool),
            capacity, queueId, owner_);
#else
        (void)pool;
        (void)capacity;
        (void)queueId;
        (void)action;
        return nullptr;
#endif
    }

    void *createTxCompletionQueue(void *pool, uint32_t capacity,
                                  uint32_t queueId) override
    {
#if ITLWM_AWDL_DATAPATH_LIVE
        return createTxCompletionLive(
            static_cast<IOSkywalkPacketBufferPool *>(pool),
            capacity, queueId, owner_);
#else
        (void)pool;
        (void)capacity;
        (void)queueId;
        return nullptr;
#endif
    }

    void *createRxCompletionQueue(void *pool, uint32_t capacity,
                                  uint32_t queueId) override
    {
#if ITLWM_AWDL_DATAPATH_LIVE
        return createRxCompletionLive(
            static_cast<IOSkywalkPacketBufferPool *>(pool),
            capacity, queueId, owner_);
#else
        (void)pool;
        (void)capacity;
        (void)queueId;
        return nullptr;
#endif
    }

    void destroyQueue(void *queue,
                      itlwm_awdl_datapath::Resource kind) override
    {
        if (queue == nullptr)
            return;

        /*
         * AppleBCMWLANProximityInterface::freeResources() calls disable()
         * and then release() for the two queue slots, and release() only for
         * the pool slots. Stage 4 has never enabled a queue, but the order is
         * kept identical so a later live path inherits it.
         */
        switch (kind) {
        case itlwm_awdl_datapath::Resource::TxSubmissionQueue: {
            IOSkywalkTxSubmissionQueue *q =
                static_cast<IOSkywalkTxSubmissionQueue *>(queue);
            q->disable();
            q->release();
            break;
        }
        case itlwm_awdl_datapath::Resource::TxCompletionQueue: {
            IOSkywalkTxCompletionQueue *q =
                static_cast<IOSkywalkTxCompletionQueue *>(queue);
            q->disable();
            q->release();
            break;
        }
        case itlwm_awdl_datapath::Resource::RxCompletionQueue: {
            IOSkywalkRxCompletionQueue *q =
                static_cast<IOSkywalkRxCompletionQueue *>(queue);
            q->disable();
            q->release();
            break;
        }
        default:
            break;
        }
    }

private:
    OSObject *owner_;
};

} /* namespace */

namespace itlwm_awdl_datapath {

Backend *createSkywalkBackend(void *owner)
{
    return new SkywalkBackend(static_cast<OSObject *>(owner));
}

void destroySkywalkBackend(Backend *backend)
{
    delete backend;
}

} /* namespace itlwm_awdl_datapath */

#endif /* ITLWM_AWDL_EXPERIMENTAL */
