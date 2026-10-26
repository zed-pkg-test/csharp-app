#include "oreslang/runtime/NativeSelectContinuationFence.hpp"
#include <cassert>
#include <stdexcept>

using oreslang::runtime::NativeSelectContinuationFence;

int main() {
    {
        NativeSelectContinuationFence<4> fence;
        auto ticket = fence.register_continuation(1);
        assert(fence.try_consume(ticket));
        assert(!fence.try_consume(ticket));
        assert(fence.pending_tickets() == 0);
    }
    {
        NativeSelectContinuationFence<4> fence;
        auto old = fence.register_continuation(1);
        assert(fence.advance(1));
        assert(fence.generation() == 2);
        assert(!fence.try_consume(old));
        auto current = fence.register_continuation(2);
        assert(fence.try_consume(current));
    }
    {
        NativeSelectContinuationFence<1> fence;
        auto old = fence.register_continuation(1);
        assert(fence.advance(1));
        auto current = fence.register_continuation(2);
        assert(!old.active());
        assert(current.active());
        assert(!fence.try_consume(old));
        assert(fence.try_consume(current));
    }
    {
        NativeSelectContinuationFence<2> fence;
        auto ticket = fence.register_continuation(1);
        assert(ticket.close());
        assert(!ticket.close());
        assert(!fence.try_consume(ticket));
    }
    {
        NativeSelectContinuationFence<2> fence(7);
        auto ticket = fence.register_continuation(7);
        assert(!fence.advance(6));
        assert(fence.generation() == 7);
        assert(fence.try_consume(ticket));
    }
    {
        NativeSelectContinuationFence<2> fence(3);
        bool rejected = false;
        try {
            (void)fence.register_continuation(2);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        assert(rejected);
        assert(fence.pending_tickets() == 0);
    }
    {
        NativeSelectContinuationFence<2> first;
        NativeSelectContinuationFence<2> second;
        auto ticket = first.register_continuation(1);
        assert(!second.try_consume(ticket));
        assert(ticket.close());
    }
    return 0;
}
