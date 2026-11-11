//
// AirportItlwmAWDLPacketFlow.hpp — Stage 4.3 host-only packet ownership model.
//
// This pure C++14 header does NOT reference IOKit, IOSkywalk or a controller.
// The experimental driver does NOT include it. No kernel packets are touched.
//
// Transfer to a real Skywalk completion queue and actual return-to-pool
// acknowledgement are EXTERNAL operations. finalize() and acknowledgeReturn()
// only model those events after the caller has independently verified them.
// In particular, finalize() is not a real packet completion!
//
#ifndef ITLWM_AWDL_PACKET_FLOW_HPP
#define ITLWM_AWDL_PACKET_FLOW_HPP

#include <stdint.h>

namespace itlwm_awdl_packet_flow {

enum class Path : uint8_t { Tx = 0, Rx = 1 };
enum class Outcome : uint8_t { Completed = 0, Dropped = 1 };

template <uint32_t Capacity>
class Ownership {
    static_assert(Capacity > 0, "packet ownership capacity must be nonzero");

public:
    struct Ticket {
        uint32_t index;
        uint64_t generation;
    };

    Ownership() : occupied_(0), outstanding_(0), finalized_(0)
    {
        for (uint32_t i = 0; i < Capacity; ++i) {
            slots_[i].packet = nullptr;
            slots_[i].generation = 0;
            slots_[i].path = Path::Tx;
            slots_[i].state = SlotState::Free;
        }
    }

    Ownership(const Ownership &) = delete;
    Ownership &operator=(const Ownership &) = delete;

    static Ticket invalidTicket() { return Ticket{Capacity, 0}; }

    /*
     * Claim an entire batch or nothing. Reject duplicate packet addresses,
     * null pointers, duplicates still awaiting return, insufficient space,
     * invalid path, and insufficient output capacity. No dynamic allocation.
     *
     * Owner must serialize ALL access. Tokens are generation-stamped so a
     * delayed completion for an earlier use of an address cannot complete
     * the current use of that address.
     */
    bool claimBatch(void *const *packets, uint32_t count, Path path,
                    Ticket *out, uint32_t outCapacity)
    {
        if (packets == nullptr || out == nullptr || count == 0 ||
            count > Capacity || outCapacity < count ||
            (path != Path::Tx && path != Path::Rx))
            return false;

        for (uint32_t i = 0; i < count; ++i) {
            if (packets[i] == nullptr)
                return false;
            for (uint32_t j = 0; j < i; ++j) {
                if (packets[j] == packets[i])
                    return false;
            }
            for (uint32_t j = 0; j < Capacity; ++j) {
                if (slots_[j].state != SlotState::Free &&
                    slots_[j].packet == packets[i])
                    return false;
            }
        }

        uint32_t available = 0;
        for (uint32_t i = 0; i < Capacity; ++i) {
            if (slots_[i].state == SlotState::Free &&
                slots_[i].generation != UINT64_MAX)
                ++available;
        }
        if (available < count)
            return false;

        uint32_t written = 0;
        for (uint32_t i = 0; i < Capacity && written < count; ++i) {
            Slot &slot = slots_[i];
            if (slot.state != SlotState::Free ||
                slot.generation == UINT64_MAX)
                continue;
            slot.generation++;
            slot.packet = packets[written];
            slot.path = path;
            slot.state = SlotState::Outstanding;
            out[written] = Ticket{i, slot.generation};
            ++written;
        }
        occupied_ += count;
        outstanding_ += count;
        return true;
    }

    /*
     * Only a caller which has independently performed the appropriate
     * Skywalk completion or drop may call finalize(). This function
     * updates bookkeeping ONLY; it never frees or enqueues a packet.
     */
    bool finalize(Ticket ticket, Outcome outcome)
    {
        Slot *slot = find(ticket);
        if (slot == nullptr || slot->state != SlotState::Outstanding ||
            (outcome != Outcome::Completed && outcome != Outcome::Dropped))
            return false;
        slot->state = outcome == Outcome::Completed ?
            SlotState::Completed : SlotState::Dropped;
        --outstanding_;
        ++finalized_;
        return true;
    }

    /*
     * Only after the external pool/queue return has been acknowledged may
     * this ticket be recycled. A stale ticket must not free a newer use.
     */
    bool acknowledgeReturn(Ticket ticket)
    {
        Slot *slot = find(ticket);
        if (slot == nullptr ||
            (slot->state != SlotState::Completed &&
             slot->state != SlotState::Dropped))
            return false;
        slot->packet = nullptr;
        slot->state = SlotState::Free;
        --finalized_;
        --occupied_;
        return true;
    }

    bool isOutstanding(Ticket ticket) const
    {
        const Slot *slot = find(ticket);
        return slot != nullptr && slot->state == SlotState::Outstanding;
    }

    bool ticketMatchesPath(Ticket ticket, Path path) const
    {
        const Slot *slot = find(ticket);
        return slot != nullptr && slot->path == path;
    }

    uint32_t occupied() const { return occupied_; }
    uint32_t outstanding() const { return outstanding_; }
    uint32_t finalized() const { return finalized_; }
    bool empty() const { return occupied_ == 0; }

    bool invariantHolds() const
    {
        uint32_t occupied = 0, outstanding = 0, finalized = 0;
        for (uint32_t i = 0; i < Capacity; ++i) {
            const Slot &slot = slots_[i];
            if (slot.state == SlotState::Free) {
                if (slot.packet != nullptr)
                    return false;
                continue;
            }
            if (slot.packet == nullptr || slot.generation == 0)
                return false;
            for (uint32_t j = i + 1; j < Capacity; ++j) {
                if (slots_[j].state != SlotState::Free &&
                    slots_[j].packet == slot.packet)
                    return false;
            }
            ++occupied;
            if (slot.state == SlotState::Outstanding)
                ++outstanding;
            else if (slot.state == SlotState::Completed ||
                     slot.state == SlotState::Dropped)
                ++finalized;
            else
                return false;
        }
        return occupied == occupied_ &&
               outstanding == outstanding_ &&
               finalized == finalized_ &&
               occupied == outstanding + finalized;
    }

private:
    enum class SlotState : uint8_t {
        Free = 0, Outstanding = 1, Completed = 2, Dropped = 3
    };

    struct Slot {
        void *packet;
        uint64_t generation;
        Path path;
        SlotState state;
    };

    Slot *find(Ticket ticket)
    {
        if (ticket.index >= Capacity || ticket.generation == 0)
            return nullptr;
        Slot *slot = &slots_[ticket.index];
        if (slot->state == SlotState::Free ||
            slot->generation != ticket.generation)
            return nullptr;
        return slot;
    }

    const Slot *find(Ticket ticket) const
    {
        if (ticket.index >= Capacity || ticket.generation == 0)
            return nullptr;
        const Slot *slot = &slots_[ticket.index];
        if (slot->state == SlotState::Free ||
            slot->generation != ticket.generation)
            return nullptr;
        return slot;
    }

    Slot slots_[Capacity];
    uint32_t occupied_;
    uint32_t outstanding_;
    uint32_t finalized_;
};

} /* namespace itlwm_awdl_packet_flow */

#endif /* ITLWM_AWDL_PACKET_FLOW_HPP */
