#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace oreslang::runtime {

/**
 * Actor-generation fence for queued select/await continuations.
 *
 * Completion producers enqueue Ticket values. The actor lane calls
 * try_consume() immediately before executing a continuation. advance()
 * invalidates all outstanding tickets from the previous actor generation.
 */
template <std::size_t MaxTickets = 64>
class NativeSelectContinuationFence final {
    static_assert(MaxTickets > 0, "continuation capacity must be positive");

private:
    struct Slot {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        bool active = false;
    };
    struct State {
        mutable std::mutex mutex;
        std::array<Slot, MaxTickets> slots{};
        std::uint64_t generation = 1;
        std::uint64_t next_id = 1;
    };

public:
    class Ticket final {
    public:
        Ticket() = default;

        std::uint64_t generation() const noexcept { return generation_; }

        bool active() const {
            auto s = state_.lock();
            if (!s) return false;
            std::lock_guard<std::mutex> guard(s->mutex);
            if (index_ >= MaxTickets) return false;
            const auto& slot = s->slots[index_];
            return slot.active && slot.id == id_ &&
                   slot.generation == generation_;
        }

        bool close() const {
            auto s = state_.lock();
            if (!s) return false;
            std::lock_guard<std::mutex> guard(s->mutex);
            if (index_ >= MaxTickets) return false;
            auto& slot = s->slots[index_];
            if (!slot.active || slot.id != id_ ||
                slot.generation != generation_)
                return false;
            clear(slot);
            return true;
        }

    private:
        friend class NativeSelectContinuationFence;
        Ticket(const std::shared_ptr<State>& state,
               std::size_t index,
               std::uint64_t id,
               std::uint64_t generation)
            : state_(state), index_(index), id_(id),
              generation_(generation) {}

        static void clear(Slot& slot) noexcept {
            slot.active = false;
            slot.generation = 0;
        }

        std::weak_ptr<State> state_;
        std::size_t index_ = MaxTickets;
        std::uint64_t id_ = 0;
        std::uint64_t generation_ = 0;
    };

    explicit NativeSelectContinuationFence(std::uint64_t generation = 1)
        : state_(std::make_shared<State>()) {
        if (generation == 0)
            throw std::invalid_argument(
                "continuation generation must be positive");
        state_->generation = generation;
    }

    NativeSelectContinuationFence(
        const NativeSelectContinuationFence&) = delete;
    NativeSelectContinuationFence& operator=(
        const NativeSelectContinuationFence&) = delete;

    std::uint64_t generation() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->generation;
    }

    std::size_t pending_tickets() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        std::size_t count = 0;
        for (const auto& slot : state_->slots)
            if (slot.active) ++count;
        return count;
    }

    Ticket register_continuation(std::uint64_t expected_generation) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (expected_generation != state_->generation)
            throw std::logic_error("stale actor generation");

        std::size_t index = MaxTickets;
        for (std::size_t i = 0; i < MaxTickets; ++i) {
            if (!state_->slots[i].active) {
                index = i;
                break;
            }
        }
        if (index == MaxTickets)
            throw std::overflow_error(
                "continuation ticket capacity exhausted");
        if (state_->next_id ==
            std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error(
                "continuation ticket identity exhausted");

        const auto id = state_->next_id++;
        auto& slot = state_->slots[index];
        slot.id = id;
        slot.generation = state_->generation;
        slot.active = true;
        return Ticket(state_, index, id, state_->generation);
    }

    bool try_consume(const Ticket& ticket) {
        auto owner = ticket.state_.lock();
        if (!owner || owner.get() != state_.get())
            return false;

        std::lock_guard<std::mutex> guard(state_->mutex);
        if (ticket.index_ >= MaxTickets ||
            ticket.generation_ != state_->generation)
            return false;

        auto& slot = state_->slots[ticket.index_];
        if (!slot.active || slot.id != ticket.id_ ||
            slot.generation != ticket.generation_)
            return false;

        Ticket::clear(slot);
        return true;
    }

    bool advance(std::uint64_t expected_generation) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (expected_generation != state_->generation)
            return false;
        if (state_->generation ==
            std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor generation exhausted");

        ++state_->generation;
        for (auto& slot : state_->slots)
            Ticket::clear(slot);
        return true;
    }

private:
    std::shared_ptr<State> state_;
};

}  // namespace oreslang::runtime
