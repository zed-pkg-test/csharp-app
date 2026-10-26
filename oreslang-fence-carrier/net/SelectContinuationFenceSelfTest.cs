using Ores.Net.Runtime;

internal static class SelectContinuationFenceSelfTest
{
    private static void Require(bool value, string reason)
    {
        if (!value) throw new Exception("Continuation fence regression: " + reason);
    }

    private static void TicketConsumesOnce()
    {
        var fence = new SelectContinuationFence();
        var ticket = fence.Register(1);
        Require(ticket.Generation == 1, "ticket generation");
        Require(fence.TryConsume(ticket), "first consume");
        Require(!fence.TryConsume(ticket), "second consume rejected");
        Require(fence.PendingTickets == 0, "consumed ticket removed");
    }

    private static void AdvanceInvalidatesQueuedOldTicket()
    {
        var fence = new SelectContinuationFence();
        var queued = fence.Register(1);
        Require(fence.Advance(1), "advance current generation");
        Require(fence.Generation == 2, "generation incremented");
        Require(!fence.TryConsume(queued), "old queued ticket rejected");
    }

    private static void SlotReuseKeepsIdentityDistinct()
    {
        var fence = new SelectContinuationFence(maxTickets: 1);
        var old = fence.Register(1);
        Require(fence.Advance(1), "advance before slot reuse");
        var current = fence.Register(2);
        Require(!old.Active && current.Active, "only new ticket active");
        Require(!fence.TryConsume(old), "old ticket cannot consume new slot");
        Require(fence.TryConsume(current), "new ticket consumes");
    }

    private static void ClosedTicketCannotDeliver()
    {
        var fence = new SelectContinuationFence();
        var ticket = fence.Register(1);
        Require(ticket.Close(), "close ticket");
        Require(!ticket.Close(), "close is one-shot");
        Require(!fence.TryConsume(ticket), "closed ticket rejected");
    }

    private static void StaleAdvanceLeavesCurrentTicketIntact()
    {
        var fence = new SelectContinuationFence(generation: 7);
        var ticket = fence.Register(7);
        Require(!fence.Advance(6), "stale advance rejected");
        Require(fence.Generation == 7, "generation unchanged");
        Require(fence.TryConsume(ticket), "current ticket remains valid");
    }

    private static void StaleRegistrationFailsClosed()
    {
        var fence = new SelectContinuationFence(generation: 3);
        var observed = false;
        try
        {
            _ = fence.Register(2);
        }
        catch (InvalidOperationException)
        {
            observed = true;
        }
        Require(observed, "stale registration rejected");
        Require(fence.PendingTickets == 0, "no ticket leaked");
    }

    private static void ForeignTicketCannotConsume()
    {
        var first = new SelectContinuationFence();
        var second = new SelectContinuationFence();
        var ticket = first.Register(1);
        Require(!second.TryConsume(ticket), "foreign ticket rejected");
        Require(ticket.Close(), "owner can still close ticket");
    }

    private static void DeliveryRechecksGenerationAfterQueueing()
    {
        var fence = new SelectContinuationFence();
        var queued = fence.Register(1);
        Require(fence.Advance(1), "restart advances generation");
        var delivered = 0;
        if (fence.TryConsume(queued)) delivered++;
        Require(delivered == 0, "old queued continuation not delivered");

        var current = fence.Register(2);
        if (fence.TryConsume(current)) delivered++;
        Require(delivered == 1, "current continuation delivered once");
    }

    public static void Run()
    {
        TicketConsumesOnce();
        AdvanceInvalidatesQueuedOldTicket();
        SlotReuseKeepsIdentityDistinct();
        ClosedTicketCannotDeliver();
        StaleAdvanceLeavesCurrentTicketIntact();
        StaleRegistrationFailsClosed();
        ForeignTicketCannotConsume();
        DeliveryRechecksGenerationAfterQueueing();
    }
}
