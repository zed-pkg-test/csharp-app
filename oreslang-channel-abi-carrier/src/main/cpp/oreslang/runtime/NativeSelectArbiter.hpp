#pragma once

#include <array>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Host/runtime arbitration primitive for a future native static-select lowering.
 *
 * This type does not parse Oreslang source and does not implement Channel<T>.
 * It owns only the select linearization point:
 *
 * - exactly one registered case may win;
 * - every armed losing registration is revoked exactly once;
 * - cancellation revokes every armed registration exactly once;
 * - a loser that becomes terminal before its external revoker is armed is
 *   revoked immediately when arm() supplies that revoker;
 * - revocation callbacks execute outside the arbiter mutex and may re-enter
 *   cancellation/select code without deadlocking this object.
 *
 * Source-level select must remain fail-closed until channel/future registration,
 * ownership and continuation lowering are wired to this primitive.
 */
template <std::size_t Capacity>
class NativeSelectArbiter final {
    static_assert(Capacity > 0, "select capacity must be positive");

private:
    enum class Terminal {
        Pending,
        Won,
        Cancelled,
    };

    enum class SlotState {
        Empty,
        Pending,
        Winner,
        Lost,
        Closed,
    };

    struct Slot final {
        SlotState state = SlotState::Empty;
        std::function<void()> revoke;
    };

    struct State final {
        mutable std::mutex mutex;
        std::array<Slot, Capacity> slots{};
        Terminal terminal = Terminal::Pending;
        std::size_t winner = Capacity;
        std::exception_ptr cleanup_failure;
    };

    static void record_cleanup_failure(
            const std::shared_ptr<State>& state,
            std::exception_ptr failure) {
        if (!failure) return;
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->cleanup_failure) state->cleanup_failure = std::move(failure);
    }

    static void invoke_all(
            const std::shared_ptr<State>& state,
            std::array<std::function<void()>, Capacity>& callbacks,
            std::size_t callback_count) {
        std::exception_ptr first_failure;
        for (std::size_t i = 0; i < callback_count; ++i) {
            try {
                callbacks[i]();
            } catch (...) {
                if (!first_failure) first_failure = std::current_exception();
            }
        }
        record_cleanup_failure(state, std::move(first_failure));
    }

    static bool cancel_state(const std::shared_ptr<State>& state) {
        std::array<std::function<void()>, Capacity> callbacks{};
        std::size_t callback_count = 0;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            if (state->terminal != Terminal::Pending) return false;
            state->terminal = Terminal::Cancelled;
            for (auto& slot : state->slots) {
                if (slot.state != SlotState::Pending) continue;
                if (slot.revoke) {
                    callbacks[callback_count++] = std::move(slot.revoke);
                    slot.state = SlotState::Closed;
                } else {
                    // The external registration may still be in the process of
                    // being created. arm() will observe Lost and revoke it
                    // immediately after the producer returns its cancellation
                    // callback.
                    slot.state = SlotState::Lost;
                }
            }
        }
        invoke_all(state, callbacks, callback_count);
        return true;
    }

    template <class Commit>
    static bool try_win_state(const std::shared_ptr<State>& state,
                              std::size_t index,
                              Commit&& commit) {
        if (index >= Capacity) return false;

        std::array<std::function<void()>, Capacity> callbacks{};
        std::size_t callback_count = 0;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            if (state->terminal != Terminal::Pending ||
                state->slots[index].state != SlotState::Pending)
                return false;

            // The commit hook is the operation's atomic readiness/consume or
            // readiness/publish step. It runs while the arbiter is still
            // pending, before this case becomes the winner. It MUST be bounded
            // and must not re-enter this select arbiter. Channel adapters use
            // the lock order arbiter -> one channel; readiness notifications
            // must never call into the arbiter while holding the channel lock.
            if (!static_cast<bool>(std::forward<Commit>(commit)()))
                return false;

            state->terminal = Terminal::Won;
            state->winner = index;
            state->slots[index].state = SlotState::Winner;

            for (std::size_t i = 0; i < Capacity; ++i) {
                if (i == index) continue;
                auto& slot = state->slots[i];
                if (slot.state != SlotState::Pending) continue;
                if (slot.revoke) {
                    callbacks[callback_count++] = std::move(slot.revoke);
                    slot.state = SlotState::Closed;
                } else {
                    slot.state = SlotState::Lost;
                }
            }
        }
        invoke_all(state, callbacks, callback_count);
        return true;
    }

public:
    class Registration final {
    public:
        Registration() = default;
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&&) noexcept = default;
        Registration& operator=(Registration&&) noexcept = default;

        std::size_t index() const noexcept { return index_; }

        /**
         * Supplies the cancellation action for the external channel/future
         * registration. If this case already lost/cancelled, the callback is
         * invoked immediately outside the arbiter lock. If this case already
         * won, the callback is discarded: the producer consumed the winning
         * registration and it must not be revoked as a loser.
         */
        void arm(std::function<void()> revoke) {
            if (!revoke) throw std::invalid_argument("select revoker is empty");
            auto state = state_;
            if (!state) throw std::logic_error("empty select registration");

            std::function<void()> immediate;
            {
                std::lock_guard<std::mutex> guard(state->mutex);
                auto& slot = state->slots[index_];
                switch (slot.state) {
                    case SlotState::Pending:
                        if (slot.revoke)
                            throw std::logic_error("select registration already armed");
                        slot.revoke = std::move(revoke);
                        return;
                    case SlotState::Winner:
                        return;
                    case SlotState::Lost:
                        slot.state = SlotState::Closed;
                        immediate = std::move(revoke);
                        break;
                    case SlotState::Closed:
                        throw std::logic_error("select registration already closed");
                    case SlotState::Empty:
                        throw std::logic_error("select registration was not admitted");
                }
            }
            try {
                immediate();
            } catch (...) {
                NativeSelectArbiter::record_cleanup_failure(
                    state, std::current_exception());
            }
        }

        bool try_win() const {
            return try_win_if([]() noexcept { return true; });
        }

        template <class Commit>
        bool try_win_if(Commit&& commit) const {
            auto state = state_;
            return state && NativeSelectArbiter::try_win_state(
                state, index_, std::forward<Commit>(commit));
        }

        bool is_winner() const {
            auto state = state_;
            if (!state) return false;
            std::lock_guard<std::mutex> guard(state->mutex);
            return state->terminal == Terminal::Won &&
                   state->winner == index_ &&
                   state->slots[index_].state == SlotState::Winner;
        }

    private:
        friend class NativeSelectArbiter;
        Registration(std::shared_ptr<State> state, std::size_t index)
            : state_(std::move(state)), index_(index) {}

        std::shared_ptr<State> state_;
        std::size_t index_ = Capacity;
    };

    class CancellationHandle final {
    public:
        CancellationHandle() = default;

        bool cancel() const {
            auto state = state_;
            return state && NativeSelectArbiter::cancel_state(state);
        }

    private:
        friend class NativeSelectArbiter;
        explicit CancellationHandle(std::shared_ptr<State> state)
            : state_(std::move(state)) {}

        std::shared_ptr<State> state_;
    };

    NativeSelectArbiter()
        : state_(std::make_shared<State>()) {}

    NativeSelectArbiter(const NativeSelectArbiter&) = delete;
    NativeSelectArbiter& operator=(const NativeSelectArbiter&) = delete;
    NativeSelectArbiter(NativeSelectArbiter&&) = delete;
    NativeSelectArbiter& operator=(NativeSelectArbiter&&) = delete;

    ~NativeSelectArbiter() noexcept {
        try {
            (void)cancel_state(state_);
        } catch (...) {
            // Mutex/runtime failures are unrecoverable here. User revoker
            // exceptions are captured in cleanup_failure instead.
        }
    }

    Registration register_case(std::size_t index) {
        if (index >= Capacity)
            throw std::out_of_range("select case index exceeds capacity");
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->terminal != Terminal::Pending)
            throw std::logic_error("cannot register case after select is terminal");
        auto& slot = state_->slots[index];
        if (slot.state != SlotState::Empty)
            throw std::logic_error("duplicate select case index");
        slot.state = SlotState::Pending;
        return Registration(state_, index);
    }

    CancellationHandle cancellation_handle() const {
        return CancellationHandle(state_);
    }

    bool cancel() {
        return cancel_state(state_);
    }

    std::optional<std::size_t> winner() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->terminal != Terminal::Won) return std::nullopt;
        return state_->winner;
    }

    bool is_cancelled() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->terminal == Terminal::Cancelled;
    }

    bool has_cleanup_failure() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return bool(state_->cleanup_failure);
    }

    std::exception_ptr cleanup_failure() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->cleanup_failure;
    }

    /**
     * Number of losing registrations that became terminal before arm().
     * A nonzero value is a temporary registration-handshake state, not a
     * leaked loser: each must become zero when its producer supplies arm().
     */
    std::size_t unarmed_loser_count() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        std::size_t count = 0;
        for (const auto& slot : state_->slots)
            if (slot.state == SlotState::Lost) ++count;
        return count;
    }

private:
    std::shared_ptr<State> state_;
};

}  // namespace oreslang::runtime
