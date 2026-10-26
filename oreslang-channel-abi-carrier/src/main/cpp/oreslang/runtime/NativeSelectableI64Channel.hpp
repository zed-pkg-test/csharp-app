#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Host/runtime bounded i64 channel substrate for native static-select lowering.
 *
 * Read readiness includes terminal EOF. Write readiness includes terminal
 * closed-channel failure, so a select arm cannot hang forever when a channel
 * closes while registered. Buffered values drain before EOF.
 *
 * Lock order contract:
 *   select arbiter -> at most one channel mutex
 *
 * Readiness callbacks are never invoked while the channel mutex is held.
 */
template <std::size_t Capacity, std::size_t MaxWaiters = 64>
class NativeSelectableI64Channel final {
    static_assert(Capacity > 0, "channel capacity must be positive");
    static_assert(MaxWaiters > 0, "channel waiter capacity must be positive");

private:
    enum class WaitKind { Read, Write };

    struct Waiter final {
        std::uint64_t id = 0;
        bool active = false;
        std::function<void()> notify;
    };

    struct State final {
        mutable std::mutex mutex;
        std::array<std::int64_t, Capacity> values{};
        std::size_t head = 0;
        std::size_t tail = 0;
        std::size_t count = 0;
        std::array<Waiter, MaxWaiters> readers{};
        std::array<Waiter, MaxWaiters> writers{};
        std::uint64_t next_waiter_id = 1;
        std::exception_ptr notification_failure;
        bool closed = false;
    };

public:
    enum class ReadStatus {
        WouldBlock,
        Value,
        Eof,
    };

    struct ReadResult final {
        ReadStatus status = ReadStatus::WouldBlock;
        std::int64_t value = 0;
    };

    enum class WriteStatus {
        WouldBlock,
        Written,
        Closed,
    };

    class Subscription final {
    public:
        Subscription() = default;

        bool close() const {
            auto state = state_.lock();
            if (!state) return false;
            std::lock_guard<std::mutex> guard(state->mutex);
            auto& waiters = kind_ == WaitKind::Read
                    ? state->readers : state->writers;
            if (index_ >= MaxWaiters) return false;
            auto& waiter = waiters[index_];
            if (!waiter.active || waiter.id != id_) return false;
            waiter.active = false;
            waiter.notify = {};
            return true;
        }

        bool active() const {
            auto state = state_.lock();
            if (!state) return false;
            std::lock_guard<std::mutex> guard(state->mutex);
            if (index_ >= MaxWaiters) return false;
            const auto& waiters = kind_ == WaitKind::Read
                    ? state->readers : state->writers;
            const auto& waiter = waiters[index_];
            return waiter.active && waiter.id == id_;
        }

    private:
        friend class NativeSelectableI64Channel;
        Subscription(const std::shared_ptr<State>& state,
                     WaitKind kind,
                     std::size_t index,
                     std::uint64_t id)
            : state_(state), kind_(kind), index_(index), id_(id) {}

        std::weak_ptr<State> state_;
        WaitKind kind_ = WaitKind::Read;
        std::size_t index_ = MaxWaiters;
        std::uint64_t id_ = 0;
    };

    class NotificationBatch final {
    public:
        NotificationBatch() = default;
        NotificationBatch(const NotificationBatch&) = delete;
        NotificationBatch& operator=(const NotificationBatch&) = delete;
        NotificationBatch(NotificationBatch&&) noexcept = default;
        NotificationBatch& operator=(NotificationBatch&&) noexcept = default;

        bool empty() const noexcept { return count_ == 0; }
        std::size_t size() const noexcept { return count_; }

        void dispatch() noexcept {
            std::exception_ptr first_failure;
            for (std::size_t i = 0; i < count_; ++i) {
                auto callback = std::move(callbacks_[i]);
                callbacks_[i] = {};
                try {
                    if (callback) callback();
                } catch (...) {
                    if (!first_failure)
                        first_failure = std::current_exception();
                }
            }
            count_ = 0;

            auto state = state_;
            state_.reset();
            if (!state || !first_failure) return;
            try {
                std::lock_guard<std::mutex> guard(state->mutex);
                if (!state->notification_failure)
                    state->notification_failure = std::move(first_failure);
            } catch (...) {
            }
        }

        ~NotificationBatch() noexcept = default;

    private:
        friend class NativeSelectableI64Channel;

        void reset() noexcept {
            for (std::size_t i = 0; i < count_; ++i)
                callbacks_[i] = {};
            count_ = 0;
            state_.reset();
        }

        std::array<std::function<void()>, MaxWaiters> callbacks_{};
        std::size_t count_ = 0;
        std::shared_ptr<State> state_;
    };

    NativeSelectableI64Channel()
        : state_(std::make_shared<State>()) {}

    NativeSelectableI64Channel(const NativeSelectableI64Channel&) = delete;
    NativeSelectableI64Channel& operator=(const NativeSelectableI64Channel&) = delete;
    NativeSelectableI64Channel(NativeSelectableI64Channel&&) = delete;
    NativeSelectableI64Channel& operator=(NativeSelectableI64Channel&&) = delete;

    template <class Callback>
    Subscription subscribe_readable(Callback&& callback) {
        return subscribe(WaitKind::Read, std::forward<Callback>(callback));
    }

    template <class Callback>
    Subscription subscribe_writable(Callback&& callback) {
        return subscribe(WaitKind::Write, std::forward<Callback>(callback));
    }

    bool close() {
        NotificationBatch readers;
        NotificationBatch writers;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (state_->closed) return false;
            try {
                capture_locked(state_->readers, readers);
                capture_locked(state_->writers, writers);
            } catch (...) {
                readers.reset();
                writers.reset();
                throw;
            }
            state_->closed = true;
        }
        readers.dispatch();
        writers.dispatch();
        return true;
    }

    bool closed() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->closed;
    }

    bool try_commit_read(const Subscription& subscription,
                         ReadResult& result,
                         NotificationBatch& notifications) {
        require_empty_batch(notifications);
        result = {};
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (!matches_locked(subscription, WaitKind::Read))
            return false;

        if (state_->count == 0) {
            if (!state_->closed) return false;
            close_locked(state_->readers[subscription.index_]);
            result.status = ReadStatus::Eof;
            return true;
        }

        if (state_->closed && state_->count == 1)
            capture_locked(state_->readers, notifications);
        else
            capture_locked(state_->writers, notifications);

        result.value = state_->values[state_->head];
        state_->head = (state_->head + 1) % Capacity;
        --state_->count;
        close_locked(state_->readers[subscription.index_]);
        result.status = ReadStatus::Value;
        return true;
    }

    bool try_commit_write(const Subscription& subscription,
                          std::int64_t value,
                          WriteStatus& result,
                          NotificationBatch& notifications) {
        require_empty_batch(notifications);
        result = WriteStatus::WouldBlock;
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (!matches_locked(subscription, WaitKind::Write))
            return false;

        if (state_->closed) {
            close_locked(state_->writers[subscription.index_]);
            result = WriteStatus::Closed;
            return true;
        }
        if (state_->count == Capacity) return false;

        capture_locked(state_->readers, notifications);
        state_->values[state_->tail] = value;
        state_->tail = (state_->tail + 1) % Capacity;
        ++state_->count;
        close_locked(state_->writers[subscription.index_]);
        result = WriteStatus::Written;
        return true;
    }

    bool try_write(std::int64_t value) {
        NotificationBatch notifications;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (state_->closed || state_->count == Capacity)
                return false;
            capture_locked(state_->readers, notifications);
            state_->values[state_->tail] = value;
            state_->tail = (state_->tail + 1) % Capacity;
            ++state_->count;
        }
        notifications.dispatch();
        return true;
    }

    ReadResult try_read_result() {
        NotificationBatch notifications;
        ReadResult result;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (state_->count == 0) {
                if (state_->closed)
                    result.status = ReadStatus::Eof;
                return result;
            }

            if (state_->closed && state_->count == 1)
                capture_locked(state_->readers, notifications);
            else
                capture_locked(state_->writers, notifications);

            result.value = state_->values[state_->head];
            state_->head = (state_->head + 1) % Capacity;
            --state_->count;
            result.status = ReadStatus::Value;
        }
        notifications.dispatch();
        return result;
    }

    std::optional<std::int64_t> try_read() {
        auto result = try_read_result();
        if (result.status != ReadStatus::Value)
            return std::nullopt;
        return result.value;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->count;
    }

    std::size_t pending_readers() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return active_count(state_->readers);
    }

    std::size_t pending_writers() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return active_count(state_->writers);
    }

    bool has_notification_failure() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return bool(state_->notification_failure);
    }

    std::exception_ptr notification_failure() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->notification_failure;
    }

private:
    template <class Callback>
    Subscription subscribe(WaitKind kind, Callback&& callback) {
        std::function<void(const Subscription&)> consumer(
            std::forward<Callback>(callback));
        if (!consumer)
            throw std::invalid_argument("channel readiness callback is empty");

        Subscription token;
        std::function<void()> immediate;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            auto& waiters = kind == WaitKind::Read
                    ? state_->readers : state_->writers;
            std::size_t index = MaxWaiters;
            for (std::size_t i = 0; i < MaxWaiters; ++i) {
                if (!waiters[i].active) {
                    index = i;
                    break;
                }
            }
            if (index == MaxWaiters)
                throw std::overflow_error(
                    "channel select waiter capacity exhausted");
            if (state_->next_waiter_id ==
                std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error(
                    "channel select waiter identity exhausted");

            const auto id = state_->next_waiter_id;
            token = Subscription(state_, kind, index, id);
            std::function<void()> wrapped(
                [consumer = std::move(consumer), token]() mutable {
                    consumer(token);
                });

            const bool ready = kind == WaitKind::Read
                    ? state_->count != 0 || state_->closed
                    : state_->count != Capacity || state_->closed;
            if (ready) immediate = wrapped;

            auto& waiter = waiters[index];
            waiter.id = id;
            waiter.active = true;
            waiter.notify = std::move(wrapped);
            ++state_->next_waiter_id;
        }

        if (immediate) {
            try {
                immediate();
            } catch (...) {
                (void)token.close();
                throw;
            }
        }
        return token;
    }

    bool matches_locked(const Subscription& subscription,
                        WaitKind expected) const {
        auto owner = subscription.state_.lock();
        if (!owner || owner.get() != state_.get() ||
            subscription.kind_ != expected ||
            subscription.index_ >= MaxWaiters)
            return false;
        const auto& waiters = expected == WaitKind::Read
                ? state_->readers : state_->writers;
        const auto& waiter = waiters[subscription.index_];
        return waiter.active && waiter.id == subscription.id_;
    }

    static void close_locked(Waiter& waiter) noexcept {
        waiter.active = false;
        waiter.notify = {};
    }

    static std::size_t active_count(
            const std::array<Waiter, MaxWaiters>& waiters) {
        std::size_t count = 0;
        for (const auto& waiter : waiters)
            if (waiter.active) ++count;
        return count;
    }

    void capture_locked(const std::array<Waiter, MaxWaiters>& waiters,
                        NotificationBatch& notifications) {
        notifications.state_ = state_;
        try {
            for (const auto& waiter : waiters) {
                if (!waiter.active) continue;
                if (notifications.count_ == MaxWaiters)
                    throw std::logic_error(
                        "channel notification batch overflow");
                notifications.callbacks_[notifications.count_++] =
                    waiter.notify;
            }
        } catch (...) {
            notifications.reset();
            throw;
        }
    }

    static void require_empty_batch(
            const NotificationBatch& notifications) {
        if (!notifications.empty())
            throw std::logic_error(
                "channel notification batch must be empty");
    }

    std::shared_ptr<State> state_;
};

}  // namespace oreslang::runtime
