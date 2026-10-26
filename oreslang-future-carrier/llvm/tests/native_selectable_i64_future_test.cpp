#include "oreslang/runtime/NativeSelectArbiter.hpp"
#include "oreslang/runtime/NativeSelectableI64Future.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>

using oreslang::runtime::NativeSelectArbiter;
using oreslang::runtime::NativeSelectableI64Future;

template <class Future, class Registration>
static auto bind_future(Future& future,
                        Registration& registration,
                        typename Future::Result& result,
                        int& callbacks,
                        int& side_effects,
                        int side_effect_delta = 0) {
    return future.subscribe_ready(
        [&future, &registration, &result, &callbacks, &side_effects,
         side_effect_delta](const typename Future::Subscription& subscription) {
            ++callbacks;
            typename Future::Result candidate;
            const bool won = registration.try_win_if([&] {
                return future.try_commit(subscription, candidate);
            });
            if (won) {
                result = candidate;
                side_effects += side_effect_delta;
            }
        });
}

static void pending_resolution_wins_with_value() {
    using Future = NativeSelectableI64Future<4>;
    Future future;
    NativeSelectArbiter<1> select;
    auto arm = select.register_case(0);
    Future::Result result;
    int callbacks = 0;
    int side_effects = 0;

    auto wait = bind_future(
        future, arm, result, callbacks, side_effects);
    arm.arm([wait] { (void)wait.close(); });

    assert(!select.winner().has_value());
    assert(future.resolve(42));
    assert(select.winner().has_value() && *select.winner() == 0);
    assert(callbacks == 1);
    assert(result.status == Future::Status::Value);
    assert(result.value == 42);
    assert(future.pending_waiters() == 0);
}

static void completion_is_immutable_and_non_consuming() {
    using Future = NativeSelectableI64Future<4>;
    Future future;
    assert(future.resolve(7));
    assert(!future.resolve(8));
    assert(!future.cancel());

    for (int i = 0; i < 2; ++i) {
        NativeSelectArbiter<1> select;
        auto arm = select.register_case(0);
        Future::Result result;
        int callbacks = 0;
        int side_effects = 0;
        auto wait = bind_future(
            future, arm, result, callbacks, side_effects);
        arm.arm([wait] { (void)wait.close(); });

        assert(select.winner().has_value() && *select.winner() == 0);
        assert(result.status == Future::Status::Value);
        assert(result.value == 7);
    }
}

static void stale_same_future_notification_cannot_win_loser() {
    using Future = NativeSelectableI64Future<4>;
    Future future;
    NativeSelectArbiter<2> select;
    auto first = select.register_case(0);
    auto second = select.register_case(1);
    Future::Result first_result;
    Future::Result second_result;
    int first_callbacks = 0;
    int second_callbacks = 0;
    int side_effects = 0;

    auto first_wait = bind_future(
        future, first, first_result, first_callbacks, side_effects, 1);
    auto second_wait = bind_future(
        future, second, second_result, second_callbacks, side_effects, 100);
    first.arm([first_wait] { (void)first_wait.close(); });
    second.arm([second_wait] { (void)second_wait.close(); });

    assert(future.resolve(9));
    assert(select.winner().has_value() && *select.winner() == 0);
    assert(first_callbacks == 1);
    assert(second_callbacks == 1);
    assert(side_effects == 1);
    assert(first_result.status == Future::Status::Value);
    assert(second_result.status == Future::Status::Pending);
    assert(future.pending_waiters() == 0);
}

static void select_cancellation_revokes_subscription() {
    using Future = NativeSelectableI64Future<2>;
    Future future;
    NativeSelectArbiter<1> select;
    auto arm = select.register_case(0);
    Future::Result result;
    int callbacks = 0;
    int side_effects = 0;
    auto wait = bind_future(
        future, arm, result, callbacks, side_effects);
    arm.arm([wait] { (void)wait.close(); });

    assert(future.pending_waiters() == 1);
    assert(select.cancel());
    assert(future.pending_waiters() == 0);
    assert(future.resolve(5));
    assert(callbacks == 0);
}

static void cancellation_and_failure_are_terminal_outcomes() {
    using Future = NativeSelectableI64Future<2>;

    {
        Future future;
        NativeSelectArbiter<1> select;
        auto arm = select.register_case(0);
        Future::Result result;
        int callbacks = 0;
        int side_effects = 0;
        auto wait = bind_future(
            future, arm, result, callbacks, side_effects);
        arm.arm([wait] { (void)wait.close(); });
        assert(future.cancel());
        assert(select.winner().has_value());
        assert(result.status == Future::Status::Cancelled);
    }

    {
        Future future;
        NativeSelectArbiter<1> select;
        auto arm = select.register_case(0);
        Future::Result result;
        int callbacks = 0;
        int side_effects = 0;
        auto wait = bind_future(
            future, arm, result, callbacks, side_effects);
        arm.arm([wait] { (void)wait.close(); });

        auto failure = std::make_exception_ptr(
            std::runtime_error("boom"));
        assert(future.fail(failure));
        assert(select.winner().has_value());
        assert(result.status == Future::Status::Failed);
        bool observed = false;
        try {
            std::rethrow_exception(result.failure);
        } catch (const std::runtime_error&) {
            observed = true;
        }
        assert(observed);
    }
}

static void notification_failure_is_recorded() {
    using Future = NativeSelectableI64Future<2>;
    Future future;
    auto wait = future.subscribe_ready(
        [](const Future::Subscription&) {
            throw std::runtime_error("notify failed");
        });

    assert(future.resolve(1));
    assert(future.has_notification_failure());
    assert(wait.close());
    assert(future.pending_waiters() == 0);
}

static void foreign_subscription_fails_closed() {
    using Future = NativeSelectableI64Future<2>;
    Future first;
    Future second;
    auto foreign = first.subscribe_ready(
        [](const Future::Subscription&) {});
    Future::Result result;
    assert(!second.try_commit(foreign, result));
    assert(foreign.close());
}

int main() {
    pending_resolution_wins_with_value();
    completion_is_immutable_and_non_consuming();
    stale_same_future_notification_cannot_win_loser();
    select_cancellation_revokes_subscription();
    cancellation_and_failure_are_terminal_outcomes();
    notification_failure_is_recorded();
    foreign_subscription_fails_closed();
    return 0;
}
