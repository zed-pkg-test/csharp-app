#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

template <std::size_t MaxWaiters = 64>
class NativeSelectableI64Future final {
    static_assert(MaxWaiters > 0, "future waiter capacity must be positive");

public:
    enum class Status { Pending, Value, Failed, Cancelled };
    struct Result {
        Status status = Status::Pending;
        std::int64_t value = 0;
        std::exception_ptr failure;
    };
    class Subscription;

private:
    struct Waiter {
        std::uint64_t id = 0;
        bool active = false;
        std::function<void(const Subscription&)> notify;
    };
    struct State {
        mutable std::mutex mutex;
        std::array<Waiter, MaxWaiters> waiters{};
        std::uint64_t next_id = 1;
        Status status = Status::Pending;
        std::int64_t value = 0;
        std::exception_ptr failure;
        std::exception_ptr notification_failure;
    };

public:
    class Subscription final {
    public:
        Subscription() = default;
        bool close() const {
            auto s = state_.lock();
            if (!s) return false;
            std::lock_guard<std::mutex> guard(s->mutex);
            if (index_ >= MaxWaiters) return false;
            auto& w = s->waiters[index_];
            if (!w.active || w.id != id_) return false;
            w.active = false;
            w.notify = {};
            return true;
        }
        bool active() const {
            auto s = state_.lock();
            if (!s) return false;
            std::lock_guard<std::mutex> guard(s->mutex);
            if (index_ >= MaxWaiters) return false;
            const auto& w = s->waiters[index_];
            return w.active && w.id == id_;
        }
    private:
        friend class NativeSelectableI64Future;
        Subscription(const std::shared_ptr<State>& s,
                     std::size_t index,
                     std::uint64_t id)
            : state_(s), index_(index), id_(id) {}
        std::weak_ptr<State> state_;
        std::size_t index_ = MaxWaiters;
        std::uint64_t id_ = 0;
    };

    NativeSelectableI64Future() : state_(std::make_shared<State>()) {}

    template <class Callback>
    Subscription subscribe_ready(Callback&& callback) {
        std::function<void(const Subscription&)> fn(
            std::forward<Callback>(callback));
        if (!fn) throw std::invalid_argument("future callback is empty");

        Subscription sub;
        bool immediate = false;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            std::size_t index = MaxWaiters;
            for (std::size_t i = 0; i < MaxWaiters; ++i)
                if (!state_->waiters[i].active) { index = i; break; }
            if (index == MaxWaiters)
                throw std::overflow_error("future waiter capacity exhausted");
            if (state_->next_id == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("future waiter identity exhausted");
            const auto id = state_->next_id++;
            sub = Subscription(state_, index, id);
            auto& w = state_->waiters[index];
            w.id = id;
            w.active = true;
            w.notify = fn;
            immediate = state_->status != Status::Pending;
        }
        if (immediate) {
            try { fn(sub); }
            catch (...) { (void)sub.close(); throw; }
        }
        return sub;
    }

    bool try_commit(const Subscription& sub, Result& result) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        result = {};
        if (!matches_locked(sub) || state_->status == Status::Pending)
            return false;
        auto& w = state_->waiters[sub.index_];
        w.active = false;
        w.notify = {};
        result.status = state_->status;
        result.value = state_->value;
        result.failure = state_->failure;
        return true;
    }

    bool resolve(std::int64_t value) {
        return complete(Status::Value, value, {});
    }
    bool fail(std::exception_ptr failure) {
        if (!failure) throw std::invalid_argument("future failure is empty");
        return complete(Status::Failed, 0, std::move(failure));
    }
    bool cancel() { return complete(Status::Cancelled, 0, {}); }

    Status status() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->status;
    }
    std::size_t pending_waiters() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        std::size_t n = 0;
        for (const auto& w : state_->waiters) if (w.active) ++n;
        return n;
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
    bool matches_locked(const Subscription& sub) const {
        auto owner = sub.state_.lock();
        if (!owner || owner.get() != state_.get() || sub.index_ >= MaxWaiters)
            return false;
        const auto& w = state_->waiters[sub.index_];
        return w.active && w.id == sub.id_;
    }

    bool complete(Status status,
                  std::int64_t value,
                  std::exception_ptr failure) {
        std::array<std::function<void(const Subscription&)>, MaxWaiters> fns{};
        std::array<Subscription, MaxWaiters> subs{};
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (state_->status != Status::Pending) return false;
            for (std::size_t i = 0; i < MaxWaiters; ++i) {
                const auto& w = state_->waiters[i];
                if (!w.active || !w.notify) continue;
                fns[count] = w.notify;
                subs[count] = Subscription(state_, i, w.id);
                ++count;
            }
            state_->status = status;
            state_->value = value;
            state_->failure = std::move(failure);
        }

        std::exception_ptr first;
        for (std::size_t i = 0; i < count; ++i) {
            try { fns[i](subs[i]); }
            catch (...) { if (!first) first = std::current_exception(); }
        }
        if (first) {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (!state_->notification_failure)
                state_->notification_failure = std::move(first);
        }
        return true;
    }

    std::shared_ptr<State> state_;
};

}  // namespace oreslang::runtime
