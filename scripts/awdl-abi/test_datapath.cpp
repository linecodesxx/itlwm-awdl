//
//  test_datapath.cpp
//  Host-side Stage 4 tests for the AWDL datapath resource manager.
//
//  This test uses a fake backend. It never includes IOKit and never loads a
//  kext. It proves the state machine, ownership slots, reverse rollback and
//  exactly-once packet ledger without touching hardware.
//
//  Build/run: python3 scripts/awdl-abi/verify_datapath.py
//

#include "AirportItlwmAWDLDatapath.hpp"

#include <stdio.h>
#include <string.h>

using namespace itlwm_awdl_datapath;

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

class FakeBackend : public Backend {
public:
    FakeBackend()
        : eventCount(0), allocationStep(0), failStep(-1),
          poolCreates(0), queueCreates(0), poolDestroys(0),
          queueDestroys(0), callbacksNonNull(true)
    {
        events[0] = '\0';
    }

    void resetEvents()
    {
        eventCount = 0;
        events[0] = '\0';
        allocationStep = 0;
    }

    void *createPool(const char *name, const PoolOptions &) override
    {
        record(name != nullptr && name[0] == 't' ? '1' : '2');
        poolCreates++;
        if (failNow())
            return nullptr;
        return name != nullptr && name[0] == 't' ? (void *)0x100 : (void *)0x101;
    }

    void destroyPool(void *pool) override
    {
        record(pool == (void *)0x101 ? 'b' : 'a');
        poolDestroys++;
    }

    void *createTxSubmissionQueue(void *pool, uint32_t, uint32_t,
                                  const TxAction &action) override
    {
        record('3');
        queueCreates++;
        if (action.invoke == nullptr)
            callbacksNonNull = false;
        (void)pool;
        return failNow() ? nullptr : (void *)0x200;
    }

    void *createTxCompletionQueue(void *pool, uint32_t, uint32_t) override
    {
        record('4');
        queueCreates++;
        (void)pool;
        return failNow() ? nullptr : (void *)0x300;
    }

    void *createRxCompletionQueue(void *pool, uint32_t, uint32_t) override
    {
        record('5');
        queueCreates++;
        (void)pool;
        return failNow() ? nullptr : (void *)0x400;
    }

    void destroyQueue(void *queue, Resource kind) override
    {
        switch (kind) {
        case Resource::TxSubmissionQueue: record('c'); break;
        case Resource::TxCompletionQueue: record('d'); break;
        case Resource::RxCompletionQueue: record('e'); break;
        default: record('?'); break;
        }
        (void)queue;
        queueDestroys++;
    }

    char events[64];
    int eventCount;
    int allocationStep;
    int failStep;
    int poolCreates;
    int queueCreates;
    int poolDestroys;
    int queueDestroys;
    bool callbacksNonNull;

private:
    void record(char c)
    {
        if (eventCount < (int)sizeof(events) - 1) {
            events[eventCount++] = c;
            events[eventCount] = '\0';
        }
    }

    bool failNow()
    {
        return allocationStep++ == failStep;
    }
};

struct TxActionProbe {
    void *expectedPacket;
    uint32_t calls;
    uint32_t returnCount;
};

static uint32_t probeTxAction(void *opaque, void *const *packets, uint32_t count)
{
    TxActionProbe *probe = static_cast<TxActionProbe *>(opaque);
    probe->calls++;
    CHECK(count == 1);
    CHECK(packets[0] == probe->expectedPacket);
    return probe->returnCount;
}

static void test_callback_dispatch()
{
    int packet = 123;
    void *packets[] = {&packet};
    TxActionProbe probe = {&packet, 0, 1};
    TxAction action = {probeTxAction, &probe};

    CHECK(dispatchTxAction(action, packets, 1) == 1);
    CHECK(probe.calls == 1);

    probe.returnCount = 2; /* invalid: accepted > supplied */
    CHECK(dispatchTxAction(action, packets, 1) == 0);
    CHECK(probe.calls == 2);

    CHECK(dispatchTxAction(action, nullptr, 1) == 0);
    CHECK(dispatchTxAction(action, packets, 0) == 0);
    CHECK(probe.calls == 2);

    action.invoke = nullptr;
    CHECK(dispatchTxAction(action, packets, 1) == 0);
    CHECK(probe.calls == 2);
}

static Config makeConfig()
{
    Config c;
    c.txPoolName = "txpool";
    c.rxPoolName = "rxpool";
    c.txPool = PoolOptions{64, 128, 2048, 1, 0x1000, 0, 0};
    c.rxPool = PoolOptions{64, 128, 2048, 1, 0x1000, 0, 0};
    c.txSubmissionCapacity = 8;
    c.txCompletionCapacity = 2;
    c.rxCompletionCapacity = 2;
    c.txSubmissionQueueId = 1;
    c.txCompletionQueueId = 2;
    c.rxCompletionQueueId = 3;
    return c;
}

static void test_initial_state()
{
    FakeBackend backend;
    Manager manager(backend);

    CHECK(manager.state() == State::Idle);
    CHECK(!manager.isReady());
    CHECK(manager.config() == nullptr);
    for (uint32_t i = 0; i < kResourceCount; i++) {
        CHECK(manager.resource(static_cast<Resource>(i)) == nullptr);
    }
    CHECK(manager.resource(Resource::Count) == nullptr);
}

static void test_success_and_teardown()
{
    FakeBackend backend;
    Manager manager(backend);
    Config config = makeConfig();

    CHECK(manager.initialize(config) == kOk);
    CHECK(manager.state() == State::Ready);
    CHECK(manager.isReady());
    CHECK(manager.config() != nullptr);
    CHECK(strcmp(backend.events, "12345") == 0);
    CHECK(backend.poolCreates == 2);
    CHECK(backend.queueCreates == 3);
    CHECK(backend.poolDestroys == 0);
    CHECK(backend.queueDestroys == 0);
    CHECK(backend.callbacksNonNull);

    CHECK(manager.resource(Resource::TxPool) == (void *)0x100);
    CHECK(manager.resource(Resource::RxPool) == (void *)0x101);
    CHECK(manager.resource(Resource::TxSubmissionQueue) == (void *)0x200);
    CHECK(manager.resource(Resource::TxCompletionQueue) == (void *)0x300);
    CHECK(manager.resource(Resource::RxCompletionQueue) == (void *)0x400);

    backend.resetEvents();
    CHECK(manager.teardown() == kOk);
    CHECK(manager.state() == State::TornDown);
    CHECK(strcmp(backend.events, "edcba") == 0);
    CHECK(backend.poolDestroys == 2);
    CHECK(backend.queueDestroys == 3);
    CHECK(!manager.isReady());
    CHECK(manager.config() == nullptr);
    for (uint32_t i = 0; i < kResourceCount; i++) {
        CHECK(manager.resource(static_cast<Resource>(i)) == nullptr);
    }

    /* Second teardown is idempotent and must not release anything again. */
    backend.resetEvents();
    CHECK(manager.teardown() == kOk);
    CHECK(backend.eventCount == 0);
    CHECK(backend.poolDestroys == 2);
    CHECK(backend.queueDestroys == 3);
}

static void test_initialize_twice()
{
    FakeBackend backend;
    Manager manager(backend);
    Config config = makeConfig();

    CHECK(manager.initialize(config) == kOk);
    backend.resetEvents();
    CHECK(manager.initialize(config) == kBusy);
    CHECK(backend.eventCount == 0);
    CHECK(manager.state() == State::Ready);
    CHECK(manager.resource(Resource::TxPool) == (void *)0x100);
    CHECK(manager.teardown() == kOk);
}

static void test_failure_rollback()
{
    static const char *expectedDestroy[] = { "", "a", "ba", "cba", "dcba" };
    static const int expectedPoolDestroys[] = { 0, 1, 2, 2, 2 };
    static const int expectedQueueDestroys[] = { 0, 0, 0, 1, 2 };

    for (int step = 0; step < 5; step++) {
        FakeBackend backend;
        backend.failStep = step;
        Manager manager(backend);
        Config config = makeConfig();

        int result = manager.initialize(config);
        CHECK(result == kBackendFailure);
        CHECK(manager.state() == State::Failed);
        CHECK(!manager.isReady());
        CHECK(manager.config() != nullptr);

        for (uint32_t i = 0; i < kResourceCount; i++) {
            CHECK(manager.resource(static_cast<Resource>(i)) == nullptr);
        }

        /*
         * The first step+1 events are the attempted creates (including the
         * failing one). Everything after that must be the reverse-order
         * destroy sequence for the resources that were actually acquired.
         */
        CHECK(strcmp(backend.events + step + 1, expectedDestroy[step]) == 0);
        CHECK(backend.poolDestroys == expectedPoolDestroys[step]);
        CHECK(backend.queueDestroys == expectedQueueDestroys[step]);

        /* Failed has no resources, so teardown is a no-op transition. */
        backend.resetEvents();
        CHECK(manager.teardown() == kOk);
        CHECK(manager.state() == State::TornDown);
        CHECK(backend.eventCount == 0);
    }
}

static void test_invalid_config()
{
    {
        FakeBackend backend;
        Manager manager(backend);
        Config config = makeConfig();
        config.txPoolName = nullptr;
        CHECK(manager.initialize(config) == kInvalidArgument);
        CHECK(manager.state() == State::Idle);
        CHECK(backend.eventCount == 0);
    }
    {
        FakeBackend backend;
        Manager manager(backend);
        Config config = makeConfig();
        config.rxPool.packetCount = 0;
        CHECK(manager.initialize(config) == kInvalidArgument);
        CHECK(manager.state() == State::Idle);
        CHECK(backend.eventCount == 0);
    }
    {
        FakeBackend backend;
        Manager manager(backend);
        Config config = makeConfig();
        config.txSubmissionCapacity = kMinTxSubmissionCapacity - 1;
        CHECK(manager.initialize(config) == kInvalidArgument);
        CHECK(manager.state() == State::Idle);
        CHECK(backend.eventCount == 0);
    }
    {
        FakeBackend backend;
        Manager manager(backend);
        Config config = makeConfig();
        config.txCompletionCapacity = kMinTxCompletionCapacity - 1;
        CHECK(manager.initialize(config) == kInvalidArgument);
        CHECK(manager.state() == State::Idle);
        CHECK(backend.eventCount == 0);
    }
    {
        FakeBackend backend;
        Manager manager(backend);
        Config config = makeConfig();
        config.rxCompletionCapacity = kMinRxCompletionCapacity - 1;
        CHECK(manager.initialize(config) == kInvalidArgument);
        CHECK(manager.state() == State::Idle);
        CHECK(backend.eventCount == 0);
    }
    {
        FakeBackend backend;
        Manager manager(backend);
        Config config = makeConfig();
        config.txCompletionQueueId = config.txSubmissionQueueId;
        CHECK(manager.initialize(config) == kInvalidArgument);
        CHECK(manager.state() == State::Idle);
        CHECK(backend.eventCount == 0);
    }
}

static void test_fail_closed_packet_paths()
{
    FakeBackend backend;
    Manager manager(backend);
    Config config = makeConfig();
    uint32_t count = 123;
    void *packet = (void *)0xdead;

    CHECK(manager.transmit(&packet, 1, &count) == kNotReady);
    CHECK(count == 0);

    count = 123;
    CHECK(manager.receive(&packet, 1, &count) == kNotReady);
    CHECK(count == 0);

    CHECK(manager.initialize(config) == kOk);

    count = 123;
    CHECK(manager.transmit(&packet, 1, &count) == kUnsupported);
    CHECK(count == 0);

    count = 123;
    CHECK(manager.receive(&packet, 1, &count) == kUnsupported);
    CHECK(count == 0);

    /* NULL out-pointers must not crash. */
    CHECK(manager.transmit(&packet, 1, nullptr) == kUnsupported);
    CHECK(manager.receive(&packet, 1, nullptr) == kUnsupported);

    CHECK(manager.teardown() == kOk);
}

static void test_ledger_exactly_once()
{
    int a = 0, b = 0, c = 0, d = 0;
    PacketLedgerT<4> ledger;

    CHECK(ledger.invariantHolds());
    CHECK(ledger.used() == 0);
    CHECK(!ledger.consume(nullptr));
    CHECK(!ledger.complete(nullptr));
    CHECK(!ledger.drop(nullptr));

    CHECK(ledger.consume(&a));
    CHECK(!ledger.consume(&a));       /* duplicate consume */
    CHECK(ledger.consume(&b));
    CHECK(ledger.consume(&c));
    CHECK(ledger.consume(&d));
    CHECK(!ledger.consume(&a));       /* capacity is full */

    CHECK(ledger.used() == 4);
    CHECK(ledger.outstanding() == 4);
    CHECK(ledger.invariantHolds());

    CHECK(ledger.complete(&a));
    CHECK(!ledger.complete(&a));      /* double complete */
    CHECK(!ledger.drop(&a));          /* drop after complete */
    CHECK(ledger.isOutstanding(&b));
    CHECK(!ledger.isOutstanding(&a));

    CHECK(ledger.drop(&b));
    CHECK(!ledger.drop(&b));          /* double drop */
    CHECK(!ledger.complete(&b));      /* complete after drop */

    CHECK(ledger.used() == 4);
    CHECK(ledger.outstanding() == 2); /* c, d */
    CHECK(ledger.completed() == 1);   /* a */
    CHECK(ledger.dropped() == 1);     /* b */
    CHECK(ledger.invariantHolds());

    /* A completed packet is not silently reusable in the same ledger. */
    CHECK(!ledger.consume(&a));

    PacketLedgerT<1> one;
    CHECK(one.consume(&a));
    CHECK(!one.consume(&b));
    CHECK(one.complete(&a));
    CHECK(one.invariantHolds());
}

int main()
{
    test_callback_dispatch();
    test_initial_state();
    test_success_and_teardown();
    test_initialize_twice();
    test_failure_rollback();
    test_invalid_config();
    test_fail_closed_packet_paths();
    test_ledger_exactly_once();

    if (failures == 0) {
        printf("ALL DATAPATH TESTS PASSED\n");
        return 0;
    }

    printf("%d DATAPATH TEST(S) FAILED\n", failures);
    return 1;
}
