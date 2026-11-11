//
//  AirportItlwmAWDLDatapath.hpp
//  AirportItlwm-Tahoe
//
//  Stage 4: experimental Skywalk datapath resource manager.
//
//  This is a separate, non-IOKit object. It owns only the resources needed by
//  a future AWDL datapath:
//
//      TX packet pool
//      RX packet pool
//      TX submission queue
//      TX completion queue
//      RX completion queue
//
//  It deliberately does NOT:
//      - register, attach or start an interface;
//      - allocate packets, enqueue packets, or run a TX/RX callback;
//      - reference the AWDL class, the controller, or any Apple base method.
//
//  The AWDL interface class must keep its verified 0x130 size and 571-entry
//  vtable, so this manager lives beside it rather than inside it. The
//  controller (or a future AWDL owner object) owns the manager.
//
//  The manager is backend-agnostic so the state machine, ownership ledger and
//  reverse rollback can be host-tested without loading a kext. The real
//  Skywalk backend is in AirportItlwmAWDLDatapath.cpp and is fail-closed by
//  ITLWM_AWDL_DATAPATH_LIVE (default 0).
//
//  This header is pure C++14 with no IOKit dependency.
//

#ifndef AirportItlwmAWDLDatapath_hpp
#define AirportItlwmAWDLDatapath_hpp

#include <stddef.h>
#include <stdint.h>

#ifndef ITLWM_AWDL_DATAPATH_LIVE
#define ITLWM_AWDL_DATAPATH_LIVE 0
#endif

namespace itlwm_awdl_datapath {

/*
 * Explicit lifecycle. The manager never allocates while Idle/TornDown/Failed,
 * never tears down while Allocating/TearingDown, and never reports Ready
 * unless all five resource slots are non-null.
 */
enum class State : uint32_t {
    Idle = 0,
    Allocating = 1,
    Ready = 2,
    TearingDown = 3,
    TornDown = 4,
    Failed = 5,
};

enum class Resource : uint32_t {
    TxPool = 0,
    RxPool = 1,
    TxSubmissionQueue = 2,
    TxCompletionQueue = 3,
    RxCompletionQueue = 4,
    Count = 5,
};

constexpr uint32_t kResourceCount = static_cast<uint32_t>(Resource::Count);

const char *stateName(State state);
const char *resourceName(Resource resource);

/*
 * Layout-compatible with IOSkywalkPacketBufferPool::PoolOptions on 25G332.
 * The kernel's initWithName() reads packetCount, bufferCount, bufferSize,
 * maxBuffersPerPacket, memorySegmentSize and poolFlags. The final 64-bit word
 * is reserved. The static asserts below pin the 0x20-byte layout.
 */
struct PoolOptions {
    uint32_t packetCount;
    uint32_t bufferCount;
    uint32_t bufferSize;
    uint32_t maxBuffersPerPacket;
    uint32_t memorySegmentSize;
    uint32_t poolFlags;
    uint64_t reserved;
};

static_assert(sizeof(PoolOptions) == 0x20,
              "PoolOptions must match the 25G332 initWithName layout");
static_assert(offsetof(PoolOptions, packetCount) == 0x00, "packetCount offset");
static_assert(offsetof(PoolOptions, bufferCount) == 0x04, "bufferCount offset");
static_assert(offsetof(PoolOptions, bufferSize) == 0x08, "bufferSize offset");
static_assert(offsetof(PoolOptions, maxBuffersPerPacket) == 0x0c, "maxBuffersPerPacket offset");
static_assert(offsetof(PoolOptions, memorySegmentSize) == 0x10, "memorySegmentSize offset");
static_assert(offsetof(PoolOptions, poolFlags) == 0x14, "poolFlags offset");
static_assert(offsetof(PoolOptions, reserved) == 0x18, "reserved offset");

/*
 * Minimum capacities enforced by the 25G332 queue constructors:
 *   IOSkywalkTxSubmissionQueue::initWithPool  -> capacity >= 8
 *   IOSkywalkTxCompletionQueue::initWithPool  -> capacity >= 2
 *   IOSkywalkRxCompletionQueue::initWithPool  -> capacity >= 2
 */
constexpr uint32_t kMinTxSubmissionCapacity = 8;
constexpr uint32_t kMinTxCompletionCapacity = 2;
constexpr uint32_t kMinRxCompletionCapacity = 2;

static_assert(kMinTxSubmissionCapacity == 8, "TxSubmission minimum from KC");
static_assert(kMinTxCompletionCapacity == 2, "TxCompletion minimum from KC");
static_assert(kMinRxCompletionCapacity == 2, "RxCompletion minimum from KC");

struct Config {
    const char *txPoolName;
    const char *rxPoolName;
    PoolOptions txPool;
    PoolOptions rxPool;
    uint32_t txSubmissionCapacity;
    uint32_t txCompletionCapacity;
    uint32_t rxCompletionCapacity;
    uint32_t txSubmissionQueueId;
    uint32_t txCompletionQueueId;
    uint32_t rxCompletionQueueId;
};

enum Status : int {
    kOk = 0,
    kInvalidArgument = 1,
    kNotReady = 2,
    kBusy = 3,
    kBackendFailure = 4,
    kUnsupported = 5,
};

/*
 * Fail-closed TX action. The manager passes it to the backend so a future
 * queue has a non-null callback, but it always reports zero accepted
 * packets. It never claims that a packet was transmitted.
 */
struct TxAction {
    uint32_t (*dequeue)(void *context, void *const *packets, uint32_t count);
    void *context;
};

/*
 * Pure C++ callback dispatch used by the real Skywalk trampoline and host
 * tests. Return the number CONSUMED, not the number actually transmitted.
 * This stage's manager action always returns zero. No packet is freed here.
 */
inline uint32_t dispatchTxAction(const TxAction &action,
                                 void *const *packets, uint32_t count)
{
    if (action.dequeue == nullptr || packets == nullptr || count == 0)
        return 0;
    uint32_t consumed = action.dequeue(action.context, packets, count);
    return consumed <= count ? consumed : 0;
}

class Backend {
public:
    virtual ~Backend() {}
    virtual void *createPool(const char *name, const PoolOptions &options) = 0;
    virtual void destroyPool(void *pool) = 0;
    virtual void *createTxSubmissionQueue(void *pool, uint32_t capacity,
                                          uint32_t queueId,
                                          const TxAction &action) = 0;
    virtual void *createTxCompletionQueue(void *pool, uint32_t capacity,
                                          uint32_t queueId) = 0;
    virtual void *createRxCompletionQueue(void *pool, uint32_t capacity,
                                          uint32_t queueId) = 0;
    virtual void destroyQueue(void *queue, Resource kind) = 0;
};

class Manager {
public:
    explicit Manager(Backend &backend);
    ~Manager();

    Manager(const Manager &) = delete;
    Manager &operator=(const Manager &) = delete;

    /*
     * Allocate in the fixed order TX pool, RX pool, TX submission, TX
     * completion, RX completion. On any failure, release what was allocated
     * in reverse order and enter Failed. Ready is the only success state.
     */
    int initialize(const Config &config);

    /*
     * Release in reverse order: RX completion, TX completion, TX submission,
     * RX pool, TX pool. Idempotent from Idle/TornDown; returns Busy while a
     * transition is in flight.
     */
    int teardown();

    State state() const { return state_; }
    bool isReady() const;
    void *resource(Resource resource) const;
    const Config *config() const { return configured_ ? &config_ : nullptr; }

    /*
     * Stage 4 packet entry points. They never enqueue, complete, drop or
     * claim a packet. `accepted`/`received` are always set to 0.
     */
    int transmit(void *const *packets, uint32_t count, uint32_t *accepted);
    int receive(void **packets, uint32_t capacity, uint32_t *received);

private:
    static uint32_t failClosedTxAction(void *context, void *const *packets,
                                       uint32_t count);
    bool validConfig(const Config &config) const;
    void resetSlots();
    void rollback();
    void releaseAll();

    Backend &backend_;
    Config config_;
    bool configured_;
    State state_;
    void *resources_[kResourceCount];
};

/* ---- inline manager implementation (host-testable, no IOKit) ---- */

inline const char *stateName(State state)
{
    switch (state) {
    case State::Idle: return "Idle";
    case State::Allocating: return "Allocating";
    case State::Ready: return "Ready";
    case State::TearingDown: return "TearingDown";
    case State::TornDown: return "TornDown";
    case State::Failed: return "Failed";
    }
    return "Unknown";
}

inline const char *resourceName(Resource resource)
{
    switch (resource) {
    case Resource::TxPool: return "TxPool";
    case Resource::RxPool: return "RxPool";
    case Resource::TxSubmissionQueue: return "TxSubmissionQueue";
    case Resource::TxCompletionQueue: return "TxCompletionQueue";
    case Resource::RxCompletionQueue: return "RxCompletionQueue";
    case Resource::Count: break;
    }
    return "Unknown";
}

inline Manager::Manager(Backend &backend)
    : backend_(backend), configured_(false), state_(State::Idle)
{
    resetSlots();
}

inline Manager::~Manager()
{
    /*
     * The owner must call teardown(). A destructor-time release would hide an
     * ownership bug; still, release any slots that are somehow left so the
     * manager cannot leak a pool or queue silently. The backend must outlive
     * the manager.
     */
    if (state_ != State::Idle && state_ != State::TornDown)
        releaseAll();
}

inline void Manager::resetSlots()
{
    for (uint32_t i = 0; i < kResourceCount; i++)
        resources_[i] = nullptr;
}

inline bool Manager::validConfig(const Config &config) const
{
    if (config.txPoolName == nullptr || config.txPoolName[0] == '\0' ||
        config.rxPoolName == nullptr || config.rxPoolName[0] == '\0')
        return false;

    if (config.txPool.packetCount == 0 || config.txPool.bufferCount == 0 ||
        config.txPool.bufferSize == 0 || config.txPool.maxBuffersPerPacket == 0)
        return false;
    if (config.rxPool.packetCount == 0 || config.rxPool.bufferCount == 0 ||
        config.rxPool.bufferSize == 0 || config.rxPool.maxBuffersPerPacket == 0)
        return false;

    if (config.txSubmissionCapacity < kMinTxSubmissionCapacity ||
        config.txCompletionCapacity < kMinTxCompletionCapacity ||
        config.rxCompletionCapacity < kMinRxCompletionCapacity)
        return false;

    if (config.txSubmissionQueueId == config.txCompletionQueueId ||
        config.txSubmissionQueueId == config.rxCompletionQueueId ||
        config.txCompletionQueueId == config.rxCompletionQueueId)
        return false;

    return true;
}

inline int Manager::initialize(const Config &config)
{
    if (state_ == State::Ready || state_ == State::Allocating ||
        state_ == State::TearingDown)
        return kBusy;

    if (!validConfig(config))
        return kInvalidArgument;

    config_ = config;
    configured_ = true;
    state_ = State::Allocating;
    resetSlots();

    /*
     * Fixed allocation order. Every failure rolls back the resources that
     * were already acquired, in reverse order, before returning.
     */
    resources_[static_cast<uint32_t>(Resource::TxPool)] =
        backend_.createPool(config.txPoolName, config.txPool);
    if (resources_[static_cast<uint32_t>(Resource::TxPool)] == nullptr) {
        rollback();
        return kBackendFailure;
    }

    resources_[static_cast<uint32_t>(Resource::RxPool)] =
        backend_.createPool(config.rxPoolName, config.rxPool);
    if (resources_[static_cast<uint32_t>(Resource::RxPool)] == nullptr) {
        rollback();
        return kBackendFailure;
    }

    TxAction action = { &Manager::failClosedTxAction, nullptr };

    resources_[static_cast<uint32_t>(Resource::TxSubmissionQueue)] =
        backend_.createTxSubmissionQueue(
            resources_[static_cast<uint32_t>(Resource::TxPool)],
            config.txSubmissionCapacity,
            config.txSubmissionQueueId,
            action);
    if (resources_[static_cast<uint32_t>(Resource::TxSubmissionQueue)] == nullptr) {
        rollback();
        return kBackendFailure;
    }

    resources_[static_cast<uint32_t>(Resource::TxCompletionQueue)] =
        backend_.createTxCompletionQueue(
            resources_[static_cast<uint32_t>(Resource::TxPool)],
            config.txCompletionCapacity,
            config.txCompletionQueueId);
    if (resources_[static_cast<uint32_t>(Resource::TxCompletionQueue)] == nullptr) {
        rollback();
        return kBackendFailure;
    }

    resources_[static_cast<uint32_t>(Resource::RxCompletionQueue)] =
        backend_.createRxCompletionQueue(
            resources_[static_cast<uint32_t>(Resource::RxPool)],
            config.rxCompletionCapacity,
            config.rxCompletionQueueId);
    if (resources_[static_cast<uint32_t>(Resource::RxCompletionQueue)] == nullptr) {
        rollback();
        return kBackendFailure;
    }

    state_ = State::Ready;
    return kOk;
}

inline void Manager::releaseAll()
{
    /*
     * Reverse allocation order. Queues are destroyed before the pools they
     * reference, and the RX queue is destroyed before the TX queues. This
     * mirrors the ownership edges in the Stage 4 report.
     */
    void *queue = resources_[static_cast<uint32_t>(Resource::RxCompletionQueue)];
    if (queue != nullptr) {
        backend_.destroyQueue(queue, Resource::RxCompletionQueue);
        resources_[static_cast<uint32_t>(Resource::RxCompletionQueue)] = nullptr;
    }

    queue = resources_[static_cast<uint32_t>(Resource::TxCompletionQueue)];
    if (queue != nullptr) {
        backend_.destroyQueue(queue, Resource::TxCompletionQueue);
        resources_[static_cast<uint32_t>(Resource::TxCompletionQueue)] = nullptr;
    }

    queue = resources_[static_cast<uint32_t>(Resource::TxSubmissionQueue)];
    if (queue != nullptr) {
        backend_.destroyQueue(queue, Resource::TxSubmissionQueue);
        resources_[static_cast<uint32_t>(Resource::TxSubmissionQueue)] = nullptr;
    }

    void *pool = resources_[static_cast<uint32_t>(Resource::RxPool)];
    if (pool != nullptr) {
        backend_.destroyPool(pool);
        resources_[static_cast<uint32_t>(Resource::RxPool)] = nullptr;
    }

    pool = resources_[static_cast<uint32_t>(Resource::TxPool)];
    if (pool != nullptr) {
        backend_.destroyPool(pool);
        resources_[static_cast<uint32_t>(Resource::TxPool)] = nullptr;
    }
}

inline void Manager::rollback()
{
    releaseAll();
    state_ = State::Failed;
}

inline int Manager::teardown()
{
    if (state_ == State::Idle || state_ == State::TornDown) {
        state_ = State::TornDown;
        configured_ = false;
        return kOk;
    }

    if (state_ == State::Allocating || state_ == State::TearingDown)
        return kBusy;

    /* Ready or Failed. Failed already rolled back, so releaseAll is a no-op. */
    state_ = State::TearingDown;
    releaseAll();
    configured_ = false;
    state_ = State::TornDown;
    return kOk;
}

inline bool Manager::isReady() const
{
    if (state_ != State::Ready)
        return false;
    for (uint32_t i = 0; i < kResourceCount; i++) {
        if (resources_[i] == nullptr)
            return false;
    }
    return true;
}

inline void *Manager::resource(Resource resource) const
{
    uint32_t index = static_cast<uint32_t>(resource);
    if (index >= kResourceCount)
        return nullptr;
    return resources_[index];
}

inline uint32_t Manager::failClosedTxAction(void *, void *const *, uint32_t)
{
    /*
     * The backend may install this callback, but Stage 4 never enqueues a
     * packet, so it must not run. If it ever does, report zero accepted
     * packets. It never claims that a packet was transmitted.
     */
    return 0;
}

inline int Manager::transmit(void *const *packets, uint32_t count,
                             uint32_t *accepted)
{
    if (accepted != nullptr)
        *accepted = 0;
    (void)packets;
    (void)count;
    if (!isReady())
        return kNotReady;
    return kUnsupported;
}

inline int Manager::receive(void **packets, uint32_t capacity,
                            uint32_t *received)
{
    if (received != nullptr)
        *received = 0;
    (void)packets;
    (void)capacity;
    if (!isReady())
        return kNotReady;
    return kUnsupported;
}

/*
 * Fixed-capacity exactly-once packet ledger. This is the Stage 5 accounting
 * contract, host-tested now so a future completion/drop path has a reviewed
 * model to follow. It is not used by the Stage 4 manager because Stage 4
 * never consumes a packet.
 *
 * A packet can be consumed once and then either completed or dropped once.
 * Duplicate consume, double complete, complete-after-drop and drop-after-
 * complete are all rejected. No entry is ever freed implicitly.
 */
template <uint32_t Capacity>
class PacketLedgerT {
public:
    PacketLedgerT() : used_(0), outstanding_(0), completed_(0), dropped_(0)
    {
        for (uint32_t i = 0; i < Capacity; i++) {
            entries_[i].packet = nullptr;
            entries_[i].state = EntryState::Free;
        }
    }

    bool consume(void *packet)
    {
        if (packet == nullptr || find(packet) != nullptr || used_ == Capacity)
            return false;
        for (uint32_t i = 0; i < Capacity; i++) {
            if (entries_[i].state == EntryState::Free) {
                entries_[i].packet = packet;
                entries_[i].state = EntryState::Outstanding;
                used_++;
                outstanding_++;
                return true;
            }
        }
        return false;
    }

    bool complete(void *packet)
    {
        Entry *entry = find(packet);
        if (entry == nullptr || entry->state != EntryState::Outstanding)
            return false;
        entry->state = EntryState::Completed;
        outstanding_--;
        completed_++;
        return true;
    }

    bool drop(void *packet)
    {
        Entry *entry = find(packet);
        if (entry == nullptr || entry->state != EntryState::Outstanding)
            return false;
        entry->state = EntryState::Dropped;
        outstanding_--;
        dropped_++;
        return true;
    }

    bool isOutstanding(void *packet) const
    {
        const Entry *entry = find(packet);
        return entry != nullptr && entry->state == EntryState::Outstanding;
    }

    uint32_t used() const { return used_; }
    uint32_t outstanding() const { return outstanding_; }
    uint32_t completed() const { return completed_; }
    uint32_t dropped() const { return dropped_; }

    bool invariantHolds() const
    {
        uint32_t seen = 0;
        for (uint32_t i = 0; i < Capacity; i++) {
            if (entries_[i].state != EntryState::Free)
                seen++;
        }
        return seen == used_ &&
               outstanding_ + completed_ + dropped_ == used_ &&
               used_ <= Capacity;
    }

private:
    enum class EntryState : uint8_t {
        Free = 0,
        Outstanding = 1,
        Completed = 2,
        Dropped = 3,
    };

    struct Entry {
        void *packet;
        EntryState state;
    };

    Entry *find(void *packet) const
    {
        if (packet == nullptr)
            return nullptr;
        for (uint32_t i = 0; i < Capacity; i++) {
            if (entries_[i].state != EntryState::Free &&
                entries_[i].packet == packet) {
                return const_cast<Entry *>(&entries_[i]);
            }
        }
        return nullptr;
    }

    Entry entries_[Capacity];
    uint32_t used_;
    uint32_t outstanding_;
    uint32_t completed_;
    uint32_t dropped_;
};

using PacketLedger = PacketLedgerT<256>;

static_assert(kResourceCount == 5, "the manager owns exactly five resources");
static_assert(static_cast<uint32_t>(State::Ready) == 2, "state values are stable");
static_assert(static_cast<uint32_t>(Resource::RxCompletionQueue) == 4,
              "resource order is stable");

/*
 * Real Skywalk backend factory. Defined only in the experimental build. The
 * returned backend is fail-closed while ITLWM_AWDL_DATAPATH_LIVE is 0.
 */
Backend *createSkywalkBackend(void *owner);
void destroySkywalkBackend(Backend *backend);

} /* namespace itlwm_awdl_datapath */

#endif /* AirportItlwmAWDLDatapath_hpp */
