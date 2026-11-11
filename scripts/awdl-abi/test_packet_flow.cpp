//
// Stage 4.3 — host-only packet-ownership contract tests.
// Compile with C++14, no IOKit and no actual Skywalk traffic.
//
#include "AirportItlwmAWDLPacketFlow.hpp"

#include <stdio.h>
using namespace itlwm_awdl_packet_flow;

static unsigned failures = 0;
#define CHECK(condition) do {                                             \
    if (!(condition)) {                                                    \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);         \
        ++failures;                                                        \
    }                                                                      \
} while (0)

static void test_empty_and_invalid()
{
    Ownership<2> ledger;
    using Ticket = Ownership<2>::Ticket;
    int a = 1;
    void *packet[] = {&a};
    Ticket ticket[2] = {};

    CHECK(ledger.empty());
    CHECK(ledger.invariantHolds());
    CHECK(!ledger.finalize(Ownership<2>::invalidTicket(), Outcome::Completed));
    CHECK(!ledger.acknowledgeReturn(Ownership<2>::invalidTicket()));
    CHECK(!ledger.claimBatch(nullptr, 1, Path::Tx, ticket, 2));
    CHECK(!ledger.claimBatch(packet, 0, Path::Tx, ticket, 2));
    CHECK(!ledger.claimBatch(packet, 1, Path::Tx, nullptr, 1));
    CHECK(!ledger.claimBatch(packet, 1, Path::Tx, ticket, 0));
    CHECK(!ledger.claimBatch(packet, 1, static_cast<Path>(3), ticket, 2));
    CHECK(!ledger.claimBatch(packet, 3, Path::Tx, ticket, 2));

    void *nullPacket[] = {nullptr};
    CHECK(!ledger.claimBatch(nullPacket, 1, Path::Tx, ticket, 2));
    CHECK(ledger.empty());
    CHECK(ledger.invariantHolds());
}

static void test_claim_rollback()
{
    Ownership<2> ledger;
    using Ticket = Ownership<2>::Ticket;
    int a = 1, b = 2, c = 3;
    void *duplicate[] = {&a, &a};
    void *valid[] = {&a, &b};
    void *overCapacity[] = {&c};
    Ticket tickets[2] = {};
    Ticket later[2] = {};
    CHECK(!ledger.claimBatch(duplicate, 2, Path::Tx, tickets, 2));
    CHECK(ledger.empty());
    CHECK(ledger.claimBatch(valid, 2, Path::Tx, tickets, 2));
    CHECK(!ledger.claimBatch(overCapacity, 1, Path::Rx, later, 2));
    CHECK(!ledger.claimBatch(valid, 2, Path::Rx, later, 2));
    CHECK(ledger.occupied() == 2);
    CHECK(ledger.outstanding() == 2);
    CHECK(ledger.finalized() == 0);
    CHECK(ledger.ticketMatchesPath(tickets[0], Path::Tx));
    CHECK(!ledger.ticketMatchesPath(tickets[0], Path::Rx));
    CHECK(ledger.invariantHolds());

    CHECK(!ledger.acknowledgeReturn(tickets[0])); /* must finalize first */
    CHECK(ledger.finalize(tickets[0], Outcome::Completed));
    CHECK(!ledger.finalize(tickets[0], Outcome::Dropped));
    CHECK(!ledger.finalize(tickets[0], Outcome::Completed));
    CHECK(ledger.outstanding() == 1);
    CHECK(ledger.finalized() == 1);
    CHECK(!ledger.claimBatch(overCapacity, 1, Path::Rx, later, 2));

    CHECK(ledger.acknowledgeReturn(tickets[0]));
    CHECK(!ledger.acknowledgeReturn(tickets[0]));
    CHECK(!ledger.finalize(tickets[0], Outcome::Dropped));
    CHECK(ledger.claimBatch(overCapacity, 1, Path::Rx, later, 2));
    CHECK(ledger.outstanding() == 2);
    CHECK(ledger.invariantHolds());

    CHECK(ledger.finalize(tickets[1], Outcome::Dropped));
    CHECK(ledger.finalize(later[0], Outcome::Completed));
    CHECK(ledger.finalized() == 2);
    CHECK(!ledger.empty());
    CHECK(ledger.acknowledgeReturn(tickets[1]));
    CHECK(ledger.acknowledgeReturn(later[0]));
    CHECK(ledger.empty());
    CHECK(ledger.invariantHolds());
}

static void test_reuse_and_stale_completion()
{
    Ownership<1> ledger;
    using Ticket = Ownership<1>::Ticket;
    int value = 42;
    void *sameAddress[] = {&value};
    Ticket oldTicket[1] = {};
    Ticket newTicket[1] = {};

    CHECK(ledger.claimBatch(sameAddress, 1, Path::Tx, oldTicket, 1));
    CHECK(ledger.finalize(oldTicket[0], Outcome::Completed));
    CHECK(ledger.acknowledgeReturn(oldTicket[0]));
    CHECK(ledger.claimBatch(sameAddress, 1, Path::Rx, newTicket, 1));

    CHECK(oldTicket[0].index == newTicket[0].index);
    CHECK(oldTicket[0].generation != newTicket[0].generation);
    CHECK(!ledger.finalize(oldTicket[0], Outcome::Completed));
    CHECK(!ledger.finalize(oldTicket[0], Outcome::Dropped));
    CHECK(!ledger.acknowledgeReturn(oldTicket[0]));
    CHECK(ledger.isOutstanding(newTicket[0]));
    CHECK(ledger.ticketMatchesPath(newTicket[0], Path::Rx));
    CHECK(!ledger.ticketMatchesPath(oldTicket[0], Path::Tx));
    CHECK(!ledger.finalize(newTicket[0], static_cast<Outcome>(3)));
    CHECK(ledger.finalize(newTicket[0], Outcome::Dropped));
    CHECK(ledger.acknowledgeReturn(newTicket[0]));
    CHECK(ledger.empty());
    CHECK(ledger.invariantHolds());
}

static void test_mixed_tx_rx_and_out_of_order_completion()
{
    Ownership<4> ledger;
    using Ticket = Ownership<4>::Ticket;
    int a=1, b=2, c=3, d=4;
    void *txPackets[] = {&a, &b};
    void *rxPackets[] = {&c, &d};
    Ticket tx[2] = {}, rx[2] = {}, duplicate[1] = {};

    CHECK(ledger.claimBatch(txPackets, 2, Path::Tx, tx, 2));
    CHECK(ledger.claimBatch(rxPackets, 2, Path::Rx, rx, 2));
    CHECK(!ledger.claimBatch(txPackets, 1, Path::Rx, duplicate, 1));
    CHECK(!ledger.empty());
    CHECK(ledger.occupied() == 4);
    CHECK(ledger.outstanding() == 4);

    CHECK(ledger.finalize(rx[1], Outcome::Dropped));
    CHECK(ledger.finalize(tx[1], Outcome::Completed));
    CHECK(ledger.finalize(tx[0], Outcome::Dropped));
    CHECK(ledger.finalize(rx[0], Outcome::Completed));
    CHECK(ledger.outstanding() == 0);
    CHECK(ledger.finalized() == 4);
    CHECK(!ledger.empty()); /* finalization is not a pool return */
    CHECK(ledger.invariantHolds());

    CHECK(ledger.acknowledgeReturn(tx[0]));
    CHECK(ledger.acknowledgeReturn(rx[1]));
    CHECK(ledger.acknowledgeReturn(tx[1]));
    CHECK(ledger.acknowledgeReturn(rx[0]));
    CHECK(ledger.empty());
    CHECK(ledger.invariantHolds());
}

int main()
{
    test_empty_and_invalid();
    test_claim_rollback();
    test_reuse_and_stale_completion();
    test_mixed_tx_rx_and_out_of_order_completion();

    if (failures == 0) {
        printf("ALL PACKET FLOW TESTS PASSED\n");
        return 0;
    }
    printf("%u PACKET FLOW TEST(S) FAILED\n", failures);
    return 1;
}
